#!/bin/bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Запустите: sudo bash $root/deploy/install.sh" >&2
  exit 1
fi

su -s /bin/bash postgres -c "/usr/bin/psql -d postgres -v ON_ERROR_STOP=1 -f '$root/deploy/init-postgres.sql'"
install -m 644 "$root/deploy/rockidserver.service" /etc/systemd/system/rockidserver.service
systemctl daemon-reload
systemctl enable --now rockidserver.service
systemctl --no-pager --full status rockidserver.service
