"""Firmware image inspection + S3 publication.

The .bin is parsed locally BEFORE it is ever uploaded, so a wrong-target or
truncated image is rejected at the desk instead of over a cellular link.

ESP32 image layout (esp_image_format.h / esp_app_format.h):
    0x00  esp_image_header_t          24 B, magic byte 0xE9
    0x18  esp_image_segment_header_t   8 B
    0x20  esp_app_desc_t              magic 0xABCD5432, then version/name/...

Everything is a single pass over the uploaded bytes: O(n) hash, O(1) parse.
"""
from __future__ import annotations

import base64
import hashlib
import re
import struct
from dataclasses import dataclass

from botocore.exceptions import ClientError

from core.aws import AWS_ERRORS, s3

IMAGE_MAGIC = 0xE9
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
# magic(4) secure_version(4) reserv1(8) version(32) project_name(32)
# time(16) date(16) idf_ver(32)
_APP_DESC_FMT = "<II8s32s32s16s16s32s"
_APP_DESC_LEN = struct.calcsize(_APP_DESC_FMT)


def _cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace").strip()


@dataclass(frozen=True, slots=True)
class FirmwareMeta:
    filename: str
    size: int
    sha256: str
    md5_b64: str
    image_magic_ok: bool
    app_desc_ok: bool
    version: str
    project_name: str
    idf_version: str
    built: str
    errors: tuple[str, ...]
    warnings: tuple[str, ...]

    @property
    def deployable(self) -> bool:
        return not self.errors

    @property
    def size_kb(self) -> float:
        return self.size / 1024.0


def inspect(filename: str, data: bytes, app_slot_bytes: int) -> FirmwareMeta:
    """Validate + fingerprint an image. Single pass, no temp files."""
    errors: list[str] = []
    warnings: list[str] = []

    size = len(data)
    digest = hashlib.sha256(data).hexdigest()
    md5_b64 = base64.b64encode(hashlib.md5(data).digest()).decode()

    magic_ok = size > 0 and data[0] == IMAGE_MAGIC
    if not magic_ok:
        errors.append(
            f"Not an ESP32 application image: first byte is "
            f"0x{data[0]:02X} (expected 0x{IMAGE_MAGIC:02X})."
        )

    version = project = idf = built = ""
    desc_ok = False
    if size >= APP_DESC_OFFSET + _APP_DESC_LEN:
        (
            magic, _secure, _res, v_raw, p_raw, t_raw, d_raw, idf_raw,
        ) = struct.unpack_from(_APP_DESC_FMT, data, APP_DESC_OFFSET)
        if magic == APP_DESC_MAGIC:
            desc_ok = True
            version = _cstr(v_raw)
            project = _cstr(p_raw)
            idf = _cstr(idf_raw)
            built = f"{_cstr(d_raw)} {_cstr(t_raw)}".strip()
        else:
            warnings.append(
                "esp_app_desc_t magic absent -- embedded version cannot be "
                "verified; the version field below is operator-supplied only."
            )
    else:
        errors.append("File is too small to be a valid application image.")

    if size > app_slot_bytes:
        errors.append(
            f"Image is {size:,} B but the app slot in partitions_ota.csv is "
            f"{app_slot_bytes:,} B. It will not fit -- Update.begin() fails."
        )
    elif size > app_slot_bytes * 0.92:
        warnings.append(
            f"Image occupies {size / app_slot_bytes:.0%} of the app slot. "
            "Headroom is nearly gone."
        )

    return FirmwareMeta(
        filename=filename,
        size=size,
        sha256=digest,
        md5_b64=md5_b64,
        image_magic_ok=magic_ok,
        app_desc_ok=desc_ok,
        version=version,
        project_name=project,
        idf_version=idf,
        built=built,
        errors=tuple(errors),
        warnings=tuple(warnings),
    )


def version_slug(version: str) -> str:
    """1.0.5 -> v1_0_5. Dots are legal in S3 keys but the agreed convention uses
    underscores, and it keeps the key shell- and URL-safe with no escaping."""
    return "v" + re.sub(r"[^0-9A-Za-z]+", "_", version.strip().lstrip("vV"))


def s3_key(prefix: str, version: str) -> str:
    """Firmware object key under the agreed layout:

        as-ota-firmware/firmwares/firmware_v1_0_1.bin

    Flat inside `firmwares/` -- the version lives in the filename, not in a
    per-version folder, and there is exactly one bucket for all versions.
    """
    return f"{prefix.strip('/')}/firmware_{version_slug(version)}.bin"


def job_doc_key(prefix: str, version: str) -> str:
    """Job-document archive key:

        as-ota-firmware/jobs/ota_job_v1_0_1.json
    """
    return f"{prefix.strip('/')}/ota_job_{version_slug(version)}.json"


def archive_job_doc(bucket: str, key: str, document: str, job_id: str) -> None:
    """Store the exact document handed to CreateJob, for audit.

    NOTE: this copy is an ARCHIVE, not the source of truth. CreateJob is called
    with the inline `document`, not `documentSource`, because the two are not
    equivalent for us: documentSource requires the IoT service role to read the
    object, and a later edit to the S3 copy would silently change what devices
    receive. Inline means what we validated is exactly what ships.
    """
    s3().put_object(
        Bucket=bucket,
        Key=key,
        Body=document.encode("utf-8"),
        ContentType="application/json",
        Metadata={"job-id": job_id},
    )


def upload(bucket: str, key: str, data: bytes, meta: FirmwareMeta, version: str) -> dict:
    """Single PutObject -- a <2 MB image never needs multipart.

    Content-MD5 makes S3 reject a corrupted transfer server-side, and the
    sha256/version go into object metadata so the artefact is self-describing
    when someone audits the bucket six months from now.
    """
    resp = s3().put_object(
        Bucket=bucket,
        Key=key,
        Body=data,
        ContentType="application/octet-stream",
        ContentMD5=meta.md5_b64,
        Metadata={
            "fw-version": version,
            "sha256": meta.sha256,
            "embedded-version": meta.version or "unknown",
            "project": meta.project_name or "unknown",
            "source-file": meta.filename,
        },
    )
    return {"etag": resp.get("ETag", "").strip('"'), "version_id": resp.get("VersionId", "")}


def list_artifacts(bucket: str, prefix: str, limit: int = 60) -> tuple[dict, ...]:
    """Existing .bin artefacts, newest first -- lets an operator schedule a
    re-push of an already-published image without re-uploading it."""
    try:
        pages = s3().get_paginator("list_objects_v2").paginate(
            Bucket=bucket, Prefix=prefix,
            PaginationConfig={"MaxItems": limit * 4, "PageSize": 1000},
        )
        objs = [
            o for page in pages for o in page.get("Contents", [])
            if o["Key"].endswith(".bin")
        ]
    except AWS_ERRORS:
        return ()
    objs.sort(key=lambda o: o["LastModified"], reverse=True)
    return tuple(objs[:limit])


def head(bucket: str, key: str) -> dict | None:
    """Does this key already exist? Used to warn before overwriting a
    firmware artefact that devices may already be pinned to.

    Best-effort: a 404 and an unreachable/denied S3 both return None. This is
    only an advisory check -- the actual PutObject surfaces any real failure --
    so it must never take down the page.
    """
    try:
        return s3().head_object(Bucket=bucket, Key=key)
    except ClientError as exc:
        if exc.response.get("Error", {}).get("Code") in ("404", "NoSuchKey", "NotFound"):
            return None
        return None
    except AWS_ERRORS:
        return None
