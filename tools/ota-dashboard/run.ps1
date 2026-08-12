# =========================================================
# AS AI — OTA Dashboard launcher (Windows / PowerShell)
# Creates and uses a venv INSIDE this folder only. It never touches the
# repo-root .venv that PlatformIO's apply_env.py depends on.
# =========================================================
$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

$venv = Join-Path $PSScriptRoot ".venv"
$py   = Join-Path $venv "Scripts\python.exe"

if (-not (Test-Path $py)) {
    Write-Host "[setup] Creating isolated venv at $venv" -ForegroundColor Cyan
    python -m venv $venv
    & $py -m pip install --upgrade pip --quiet
    & $py -m pip install -r (Join-Path $PSScriptRoot "requirements.txt")
}

$secrets = Join-Path $PSScriptRoot ".streamlit\secrets.toml"
if (-not (Test-Path $secrets)) {
    Write-Host "[error] .streamlit\secrets.toml is missing." -ForegroundColor Red
    Write-Host "        Copy .streamlit\secrets.toml.example and fill it in."
    exit 1
}

# Bind to loopback. Do not expose this port; put Cognito/ALB or a VPN in front
# if more than one person needs access.
& $py -m streamlit run app.py --server.address 127.0.0.1 --server.port 8501
