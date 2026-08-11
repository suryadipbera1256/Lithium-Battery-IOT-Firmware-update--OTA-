# syntax=docker/dockerfile:1.7
# =========================================================
# AS AI — OTA & Diagnostics Dashboard
#
# Multi-stage: wheels are built with a toolchain that is then discarded, so the
# runtime image carries no compiler and no pip cache.
# Runs as a non-root user on a read-only root filesystem (see the ECS task
# definition), which is why /tmp and the Streamlit state dir are explicit.
# =========================================================

# ---------- stage 1: dependencies ----------
FROM public.ecr.aws/docker/library/python:3.12-slim AS build

ENV PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

WORKDIR /wheels
COPY requirements.txt .
# Build wheels once; the runtime stage installs from them with --no-index so a
# compromised or unreachable PyPI cannot alter the runtime image.
RUN python -m pip install --upgrade pip \
 && python -m pip wheel --wheel-dir=/wheels -r requirements.txt


# ---------- stage 2: runtime ----------
FROM public.ecr.aws/docker/library/python:3.12-slim AS runtime

# PYTHONDONTWRITEBYTECODE: root FS is read-only, .pyc writes would fail noisily.
# PYTHONUNBUFFERED: stdout reaches CloudWatch immediately instead of on flush.
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    HOME=/home/app \
    STREAMLIT_HOME=/home/app/.streamlit

# curl is required by the ECS container health check below. Nothing else is
# added: no shell utilities, no build tools.
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --create-home --uid 10001 --shell /usr/sbin/nologin app

WORKDIR /app

COPY --from=build /wheels /wheels
COPY requirements.txt .
RUN python -m pip install --no-index --find-links=/wheels -r requirements.txt \
 && rm -rf /wheels

# Application code only. .dockerignore keeps .venv, secrets.toml, __pycache__
# and tests out of the build context entirely.
COPY --chown=app:app app.py ./
COPY --chown=app:app core ./core
COPY --chown=app:app components ./components
COPY --chown=app:app static ./static
COPY --chown=app:app .streamlit/config.toml ./.streamlit/config.toml

# NOTE: no secrets.toml is ever baked in. The entrypoint materialises it at
# runtime from the value ECS injects as SECRETS_TOML (Secrets Manager).
COPY --chown=app:app docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod 0555 /usr/local/bin/docker-entrypoint.sh

USER app
EXPOSE 8501

# Matches the ALB target-group health check. /_stcore/health returns 200 without
# executing the page script, so it stays green during a slow AWS API call.
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8501/_stcore/health || exit 1

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["streamlit", "run", "app.py", \
     "--server.port=8501", \
     "--server.address=0.0.0.0", \
     "--server.headless=true", \
     "--server.fileWatcherType=none", \
     "--browser.gatherUsageStats=false"]
