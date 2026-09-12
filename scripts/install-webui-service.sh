#!/usr/bin/env bash
set -euo pipefail

if [ "${EUID}" -ne 0 ]; then
  echo "[!] Need root privileges; re-running with sudo..."
  exec sudo -E bash "$0" "$@"
fi

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE_NAME="${SERVICE_NAME:-aimbot-webui}"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
PYTHON_BIN="${PYTHON_BIN:-$(command -v python3 || true)}"

if [ -z "${PYTHON_BIN}" ]; then
  echo "[!] python3 not found. Install python3 first." >&2
  exit 1
fi

APP_USER="$(stat -c '%U' "${APP_DIR}")"
APP_GROUP="$(stat -c '%G' "${APP_DIR}")"
if [ -z "${APP_USER}" ] || [ "${APP_USER}" = "UNKNOWN" ]; then
  APP_USER="${SUDO_USER:-root}"
fi
if [ -z "${APP_GROUP}" ] || [ "${APP_GROUP}" = "UNKNOWN" ]; then
  APP_GROUP="$(id -gn "${APP_USER}" 2>/dev/null || echo root)"
fi

echo "[+] App directory: ${APP_DIR}"
echo "[+] Writable file owner: ${APP_USER}:${APP_GROUP}"
echo "[+] Python: ${PYTHON_BIN}"

install -d -m 0775 -o "${APP_USER}" -g "${APP_GROUP}" "${APP_DIR}/logs"
touch "${APP_DIR}/logs/engine.log" "${APP_DIR}/logs/streamer.log"
chown "${APP_USER}:${APP_GROUP}" "${APP_DIR}/logs/engine.log" "${APP_DIR}/logs/streamer.log"
chmod ug+rw,o+r "${APP_DIR}/logs/engine.log" "${APP_DIR}/logs/streamer.log"

for f in "${APP_DIR}/runtime_config.json" "${APP_DIR}/pid_config.json"; do
  if [ -e "${f}" ]; then
    chown "${APP_USER}:${APP_GROUP}" "${f}"
    chmod ug+rw,o+r "${f}"
  fi
done
if [ -d "${APP_DIR}/configs" ]; then
  chown -R "${APP_USER}:${APP_GROUP}" "${APP_DIR}/configs"
  chmod -R ug+rwX,o+rX "${APP_DIR}/configs"
fi

cat > "${SERVICE_FILE}" <<EOF
[Unit]
Description=Aimbot WebUI
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
Group=root
WorkingDirectory=${APP_DIR}
Environment=PYTHONUNBUFFERED=1
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ExecStartPre=/usr/bin/install -d -m 0775 -o ${APP_USER} -g ${APP_GROUP} ${APP_DIR}/logs
ExecStartPre=/usr/bin/touch ${APP_DIR}/logs/engine.log ${APP_DIR}/logs/streamer.log
ExecStartPre=/bin/chown ${APP_USER}:${APP_GROUP} ${APP_DIR}/logs/engine.log ${APP_DIR}/logs/streamer.log
ExecStartPre=/bin/chmod ug+rw,o+r ${APP_DIR}/logs/engine.log ${APP_DIR}/logs/streamer.log
ExecStart=${PYTHON_BIN} ${APP_DIR}/webui.py
Restart=always
RestartSec=2
KillSignal=SIGTERM
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now "${SERVICE_NAME}.service"

echo "[+] Installed and started ${SERVICE_NAME}.service"
echo "[+] Check status: sudo systemctl status ${SERVICE_NAME} --no-pager"
echo "[+] Follow logs:  sudo journalctl -u ${SERVICE_NAME} -f"
