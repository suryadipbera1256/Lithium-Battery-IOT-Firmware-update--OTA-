# Local Docker development against live AWS

Runs the **production image** locally, talking to real S3 / IoT Core, with no
Secrets Manager dependency.

## Quick start

```bash
cd tools/ota-dashboard
cp .env.example .env          # then fill in credentials — see below
DASH_PORT=8599 docker compose up --build
```

Open `http://localhost:8599`.

`DASH_PORT` defaults to 8501. Override it whenever `run.ps1` already owns 8501 —
see *Port collision* below, which fails in a genuinely misleading way.

---

## Secret resolution (docker-entrypoint.sh)

First match wins:

| # | Source | Used by |
|---|---|---|
| 1 | mounted `/app/.streamlit/secrets.toml` | **local Docker** |
| 2 | `$SECRETS_TOML` env var | ECS / Secrets Manager |
| 3 | neither → `exit 78` | — |

The mount is checked **first** by design: when both exist, the local file is the
one being actively edited, and silently preferring the remote copy would make
edits appear to do nothing. The entrypoint logs which source it used:

```
[entrypoint] secrets: mounted /app/.streamlit/secrets.toml (local development mode)
```

Compose mounts it read-only, so the container can never rewrite your credentials.

---

## AWS credentials — three options

`core/aws.py::session()` resolves in this order, and **earlier wins**:

1. `[aws].access_key_id` + `[aws].secret_access_key` from `secrets.toml`
2. `[aws].profile` from `secrets.toml`
3. boto3 default chain — env vars, `~/.aws/credentials`, IMDS

> **The trap:** non-empty keys in `secrets.toml` silently shadow
> `AWS_ACCESS_KEY_ID`. If env vars seem ignored, clear those two fields.

The effective **region always comes from `[aws].region`** in `secrets.toml`,
because `session()` passes `region_name` explicitly. `AWS_REGION` does not
override it.

### Option A — environment variables (simplest)

Leave `access_key_id`, `secret_access_key`, `profile` **empty** in
`secrets.toml`, then put keys in `.env` (git-ignored):

```dotenv
AWS_ACCESS_KEY_ID=AKIA...
AWS_SECRET_ACCESS_KEY=...
AWS_SESSION_TOKEN=            # only for STS/SSO temporary credentials
```

Compose already forwards all three.

### Option B — mount `~/.aws` (best: no keys on disk in the repo)

Uncomment in `docker-compose.yml`:

```yaml
- ${USERPROFILE}/.aws:/home/app/.aws:ro     # Windows
# - ${HOME}/.aws:/home/app/.aws:ro          # Linux / macOS
```

Set `[aws].profile = "default"` in `secrets.toml`. `AWS_CONFIG_FILE` and
`AWS_SHARED_CREDENTIALS_FILE` are already pointed at the mount.

Caveats: SSO requires the `~/.aws/sso/cache` token to be present and unexpired —
run `aws sso login` on the host first. The new `aws login` provider
(`login_session` in `~/.aws/config`) needs `botocore[crt]`, which is **not** in
`requirements.txt`; use Option A or a classic profile instead.

### Option C — keys in `secrets.toml` (works, least preferred)

Fill `[aws].access_key_id` / `secret_access_key`. They then take precedence over
everything else. Acceptable for a short-lived local test; the file is git-ignored.

### Verify credentials actually reached the container

```bash
docker compose exec dashboard python -c "import botocore.session as s; c=s.get_session().get_credentials(); print('resolved:', c is not None, getattr(c,'method',None))"
```

`resolved: False` means the container has no credentials — the app will render
fine and every AWS call will fail. The sidebar identity panel shows the same
thing (`unavailable (...)`).

---

## Port collision — fails misleadingly

If `run.ps1` is already serving 8501, `docker compose up -d` leaves the container
in state `Created` with **no logs**, and a health check against
`http://127.0.0.1:8501/_stcore/health` returns **200 from the other Streamlit**.
It looks like success.

```bash
docker compose ps                 # State must be "running", not "Created"
docker compose logs dashboard     # must show the [entrypoint] line
```

Fix: `DASH_PORT=8599 docker compose up`, or stop the host Streamlit.

---

## Git Bash path mangling (Windows)

`docker compose` reads paths from the YAML and is unaffected. Raw `docker run`
from Git Bash is not — MSYS rewrites the **container-side** path, so
`-v ...:/app/.streamlit/secrets.toml` silently becomes a Windows path and the
mount lands nowhere. The entrypoint then reports "no secrets available".

```bash
MSYS_NO_PATHCONV=1 docker run ...      # or use double slashes: //app/...
```

Prefer `docker compose`.

---

## Hot reload

Uncomment the source bind mounts **and** the `command:` override in
`docker-compose.yml`. The override switches to `fileWatcherType=poll`, which is
required because inotify does not fire across a Docker Desktop bind mount on
Windows.

---

## What this deliberately does not replicate

| Production | Local |
|---|---|
| Cognito OIDC at the ALB | none — the app's own password gate is the only auth |
| ECS task role | your IAM user/profile, usually broader |
| `readonlyRootFilesystem` | off |
| Secrets Manager | bind-mounted file |

Local runs therefore have **more** privilege than production, not less. A local
`CreateJob` reaches the real fleet — target exactly one thing.
