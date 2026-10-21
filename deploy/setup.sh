#!/bin/bash
# Подготовка машины, на которую разворачивается rockIDserver.
# Запускается на целевом хосте от root. Повторный запуск обновляет сборку и службу.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
svc="rockid"
port=7410
http=""
dbname="rockid"

usage() {
  echo "usage: sudo bash deploy/setup.sh [--user NAME] [--port N] [--http N] [--db NAME]" >&2
  echo "  --user   учётная запись службы и роль PostgreSQL (по умолчанию rockid)" >&2
  echo "  --port   порт мессенджера (по умолчанию 7410)" >&2
  echo "  --http   порт мини-приложений и ботов (по умолчанию port+1, 0 выключает)" >&2
  echo "  --db     имя базы (по умолчанию rockid)" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --user) svc="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    --http) http="${2:-}"; shift 2 ;;
    --db) dbname="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Запустите от root: sudo bash $root/deploy/setup.sh" >&2
  exit 1
fi

if [[ ! "$svc" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]]; then
  echo "Недопустимое имя пользователя: $svc" >&2
  exit 2
fi
if [[ ! "$dbname" =~ ^[a-z_][a-z0-9_]{0,31}$ ]]; then
  echo "Недопустимое имя базы: $dbname" >&2
  exit 2
fi
if [[ ! "$port" =~ ^[0-9]+$ ]] || (( port < 1 || port > 65535 )); then
  echo "Недопустимый порт: $port" >&2
  exit 2
fi
if [[ -z "$http" ]]; then
  http=$((port + 1))
fi
if [[ ! "$http" =~ ^[0-9]+$ ]] || (( http < 0 || http > 65535 )) || (( http == port )); then
  echo "Недопустимый HTTP-порт: $http" >&2
  exit 2
fi
if [[ "$root" == *"\""* || "$root" == *$'\n'* ]]; then
  echo "Путь проекта содержит кавычку или перевод строки: $root" >&2
  exit 2
fi

if [[ ! -r /etc/os-release ]] || ! grep -qE '^ID(_LIKE)?=.*(debian|ubuntu)' /etc/os-release; then
  echo "Скрипт ставит пакеты через apt и рассчитан на Debian или Ubuntu." >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
  build-essential cmake \
  libssl-dev zlib1g-dev libpq5 \
  postgresql \
  ffmpeg imagemagick \
  python3

systemctl enable postgresql
systemctl start postgresql
ready=0
for _ in $(seq 1 30); do
  if su -s /bin/bash postgres -c "pg_isready -q"; then
    ready=1
    break
  fi
  sleep 1
done
if (( ready == 0 )); then
  echo "PostgreSQL не ответил на pg_isready" >&2
  exit 1
fi

if ! id "$svc" >/dev/null 2>&1; then
  useradd --system --home-dir "$root" --shell /usr/sbin/nologin "$svc"
fi
group="$(id -gn "$svc")"

sql="$(mktemp)"
chmod 644 "$sql"
cat >"$sql" <<'SQL'
SELECT format('CREATE ROLE %I LOGIN', :'svcuser')
WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = :'svcuser')
\gexec

SELECT format('CREATE DATABASE %I OWNER %I', :'dbname', :'svcuser')
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = :'dbname')
\gexec
SQL
su -s /bin/bash postgres -c "psql -d postgres -v ON_ERROR_STOP=1 -v svcuser='$svc' -v dbname='$dbname' -f '$sql'"
rm -f "$sql"

owner="$(su -s /bin/bash postgres -c "psql -d postgres -tAc \"SELECT pg_catalog.pg_get_userbyid(datdba) FROM pg_database WHERE datname = '$dbname'\"")"
owner="${owner//[[:space:]]/}"
if [[ "$owner" != "$svc" ]]; then
  echo "База $dbname принадлежит $owner, а служба работает от $svc." >&2
  echo "Peer-аутентификация пустит в базу только роль с тем же именем, что у процесса." >&2
  exit 1
fi

cmake -S "$root" -B "$root/build"
cmake --build "$root/build" -j "$(nproc)"
chmod a+rx "$root" "$root/build" "$root/build/rockidserver"
install -d -o "$svc" -g "$svc" -m 750 "$root/data/files"

cat >/etc/systemd/system/rockidserver.service <<EOF
[Unit]
Description=rockID messenger server
After=network-online.target postgresql.service
Wants=network-online.target

[Service]
Type=simple
User=$svc
Group=$group
WorkingDirectory=$root
ExecStart=$root/build/rockidserver --port $port --http $http --files $root/data/files --db "host=/var/run/postgresql dbname=$dbname"
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable rockidserver.service
systemctl restart rockidserver.service

active=0
for _ in $(seq 1 30); do
  if systemctl is-active --quiet rockidserver.service; then
    active=1
    break
  fi
  sleep 1
done
if (( active == 0 )); then
  journalctl -u rockidserver.service -n 40 --no-pager || true
  echo "Служба rockidserver не перешла в active" >&2
  exit 1
fi

if ! python3 - "$port" <<'PY'
import json, socket, struct, sys, time
port = int(sys.argv[1])
body = json.dumps({"id": 1, "method": "ping", "params": {}}).encode()
last = "нет ответа"
for _ in range(15):
    try:
        sock = socket.create_connection(("127.0.0.1", port), 3)
        sock.sendall(struct.pack(">I", len(body)) + body)
        hdr = b""
        while len(hdr) < 4:
            chunk = sock.recv(4 - len(hdr))
            if not chunk:
                raise ConnectionError("соединение закрыто")
            hdr += chunk
        size = struct.unpack(">I", hdr)[0] & 0x7FFFFFFF
        raw = b""
        while len(raw) < size:
            chunk = sock.recv(size - len(raw))
            if not chunk:
                raise ConnectionError("ответ оборвался")
            raw += chunk
        msg = json.loads(raw.decode())
        if msg.get("ok") and msg.get("result", {}).get("pong") is True:
            print("ping ok")
            raise SystemExit(0)
        last = str(msg)
    except SystemExit:
        raise
    except Exception as exc:
        last = str(exc)
        time.sleep(1)
raise SystemExit(f"ping не ответил pong: {last}")
PY
then
  journalctl -u rockidserver.service -n 40 --no-pager || true
  exit 1
fi

echo "rockidserver слушает порт $port, HTTP $http, пользователь $svc, каталог $root"
