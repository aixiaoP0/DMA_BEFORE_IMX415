#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT_BIN="${ROOT_DIR}/build/sclient"

if [[ ! -x "${CLIENT_BIN}" ]]; then
  echo "missing client binary: ${CLIENT_BIN}" >&2
  echo "build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

HOST="${1:-127.0.0.1}"
exec "${CLIENT_BIN}" \
  --host "${HOST}" \
  --port 10000 \
  --transport udp \
  --renderer opengl \
  --decoder software \
  --metadata on \
  --udp-jitter-buffer on \
  --udp-fec off
