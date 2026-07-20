#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT_BIN="${ROOT_DIR}/build/sclient"

if [[ ! -x "${CLIENT_BIN}" ]]; then
  echo "missing client binary: ${CLIENT_BIN}" >&2
  echo "build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <board-ip> [bind-ip]" >&2
  echo "example: $0 192.168.137.99" >&2
  exit 2
fi

SERVER_HOST="$1"
BIND_HOST="${2:-0.0.0.0}"
exec "${CLIENT_BIN}" \
  --host "${BIND_HOST}" \
  --port 10002 \
  --rtp-server-host "${SERVER_HOST}" \
  --rtp-server-port 10002 \
  --transport rtp \
  --renderer opengl \
  --decoder software \
  --metadata off
