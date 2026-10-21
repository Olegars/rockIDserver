#!/bin/bash
# Копирует этот каталог на удалённую машину и запускает там deploy/setup.sh.
# Повторный запуск обновляет исходники, пересобирает бинарник и перезапускает службу.
# Каталог data/ на сервере не затирается: в нём лежат загруженные файлы.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dir="/opt/rockidserver"
svc="rockid"
port=7410
http=""
dbname="rockid"
identity=""
target=""

usage() {
  echo "usage: bash deploy/remote.sh [--dir PATH] [--user NAME] [--port N] [--http N] [--db NAME] [--identity FILE] user@host" >&2
  echo "  user@host   SSH-адрес машины, где будет работать сервер" >&2
  echo "  --dir       каталог на той машине (по умолчанию /opt/rockidserver)" >&2
  echo "  --user      учётная запись службы (по умолчанию rockid)" >&2
  echo "  --port      порт мессенджера (по умолчанию 7410)" >&2
  echo "  --http      HTTP-порт (по умолчанию port+1)" >&2
  echo "  --db        имя базы PostgreSQL (по умолчанию rockid)" >&2
  echo "  --identity  файл ключа SSH" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir) dir="${2:-}"; shift 2 ;;
    --user) svc="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    --http) http="${2:-}"; shift 2 ;;
    --db) dbname="${2:-}"; shift 2 ;;
    --identity) identity="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) usage; exit 2 ;;
    *)
      if [[ -n "$target" ]]; then
        usage
        exit 2
      fi
      target="$1"
      shift
      ;;
  esac
done

if [[ -z "$target" ]]; then
  usage
  exit 2
fi
if [[ ! "$dir" =~ ^/[A-Za-z0-9._/-]+$ ]]; then
  echo "Каталог должен быть абсолютным путём из букв, цифр, точки, подчёркивания и дефиса: $dir" >&2
  exit 2
fi
if [[ -n "$identity" && ! -r "$identity" ]]; then
  echo "Ключ SSH не читается: $identity" >&2
  exit 2
fi

if ! command -v ssh >/dev/null || ! command -v rsync >/dev/null; then
  echo "На этой машине нужны ssh и rsync." >&2
  exit 1
fi

ssh_base=(ssh)
if [[ -n "$identity" ]]; then
  ssh_base+=(-i "$identity")
fi
if [[ "$target" == *@* ]]; then
  remote_user="${target%%@*}"
else
  remote_user="${USER:-root}"
fi

remote() {
  if [[ "$remote_user" == "root" ]]; then
    "${ssh_base[@]}" "$target" "$@"
  else
    "${ssh_base[@]}" -t "$target" sudo "$@"
  fi
}

echo "Каталог $dir на $target"
remote mkdir -p "$dir"
if [[ "$remote_user" != "root" ]]; then
  remote chown "$remote_user:" "$dir"
fi

rsync_ssh="${ssh_base[*]}"
rsync -az --delete \
  --exclude '/build/' \
  --exclude '/data/' \
  --exclude '/.git/' \
  --exclude '__pycache__/' \
  --exclude '*.pyc' \
  -e "$rsync_ssh" \
  "$root/" "$target:$dir/"

setup=(bash "$dir/deploy/setup.sh" --user "$svc" --port "$port" --db "$dbname")
if [[ -n "$http" ]]; then
  setup+=(--http "$http")
fi
echo "Сборка и запуск на $target"
remote "${setup[@]}"
