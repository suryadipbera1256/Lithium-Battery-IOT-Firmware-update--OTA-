#!/bin/sh
# =========================================================
# Materialise .streamlit/secrets.toml from the environment, then exec Streamlit.
#
# WHY THIS EXISTS: core/settings.py reads st.secrets, which Streamlit only loads
# from a file. Baking that file into the image would put credentials in every
# ECR layer, and mounting it needs EFS. Instead ECS injects the whole TOML
# document as one Secrets Manager value and it is written to a tmpfs at boot.
#
# The root filesystem is read-only, so HOME is backed by a tmpfs mount declared
# in the task definition. Secrets therefore exist only in memory and vanish with
# the task -- they are never on disk and never in a layer.
# =========================================================
set -eu

SECRETS_DIR="/app/.streamlit"
SECRETS_FILE="${SECRETS_DIR}/secrets.toml"

# ---------------------------------------------------------------------------
# RESOLUTION ORDER (first match wins)
#   1. A mounted /app/.streamlit/secrets.toml   -> local Docker development
#   2. $SECRETS_TOML from Secrets Manager       -> ECS / production
#   3. nothing                                  -> fail fast, exit 78
#
# The mount is checked FIRST and deliberately: when both are present the local
# file is what the developer is actively editing, and silently preferring the
# remote copy would make edits appear to do nothing.
# ---------------------------------------------------------------------------
if [ -f "${SECRETS_FILE}" ]; then
    echo "[entrypoint] secrets: mounted ${SECRETS_FILE} (local development mode)" >&2
    if [ -n "${SECRETS_TOML:-}" ]; then
        echo "[entrypoint] NOTE: SECRETS_TOML is also set but the mounted file wins." >&2
    fi
    # settings.secrets_health() reads the /app path for diagnostics, and
    # Streamlit itself also probes $HOME/.streamlit. Mirror it there when
    # possible so both agree; a read-only mount makes this a no-op.
    if mkdir -p "${STREAMLIT_HOME}" 2>/dev/null; then
        cp "${SECRETS_FILE}" "${STREAMLIT_HOME}/secrets.toml" 2>/dev/null || true
        chmod 0400 "${STREAMLIT_HOME}/secrets.toml" 2>/dev/null || true
    fi
    exec "$@"
fi

if [ -n "${SECRETS_TOML:-}" ]; then
    echo "[entrypoint] secrets: injected via SECRETS_TOML (Secrets Manager)" >&2
    # /app is read-only under the hardened task definition; write to the tmpfs
    # HOME instead and point Streamlit at it. STREAMLIT_HOME is honoured for
    # secrets discovery in addition to the CWD.
    mkdir -p "${STREAMLIT_HOME}"
    umask 077
    printf '%s' "${SECRETS_TOML}" > "${STREAMLIT_HOME}/secrets.toml"

    # A UTF-8 BOM makes Streamlit silently report zero secrets. Strip it rather
    # than let the app come up authenticating against empty passwords.
    if head -c 3 "${STREAMLIT_HOME}/secrets.toml" | od -An -tx1 | grep -q 'ef bb bf'; then
        tail -c +4 "${STREAMLIT_HOME}/secrets.toml" > "${STREAMLIT_HOME}/.s.tmp"
        mv "${STREAMLIT_HOME}/.s.tmp" "${STREAMLIT_HOME}/secrets.toml"
        echo "[entrypoint] stripped UTF-8 BOM from injected secrets" >&2
    fi

    # Also expose it at the CWD path when that location is writable (local
    # docker run without the read-only flag), so behaviour matches production.
    if [ -w "${SECRETS_DIR}" ] 2>/dev/null; then
        cp "${STREAMLIT_HOME}/secrets.toml" "${SECRETS_FILE}" 2>/dev/null || true
        chmod 0400 "${SECRETS_FILE}" 2>/dev/null || true
    fi
elif [ ! -f "${STREAMLIT_HOME}/secrets.toml" ]; then
    # Fail fast and loudly. Starting without secrets yields a login page that
    # accepts nothing, which reads as a broken deploy rather than a config gap.
    echo "[entrypoint] FATAL: no secrets available." >&2
    echo "             Local Docker : mount ./.streamlit/secrets.toml to" >&2
    echo "                            /app/.streamlit/secrets.toml (see compose file)" >&2
    echo "             ECS          : set SECRETS_TOML via secrets[].valueFrom" >&2
    exit 78   # EX_CONFIG
fi

exec "$@"
