#!/usr/bin/env bash
# Boots ninjamzap-server-test image with the fast-interval test.cfg mounted.
# Prints the host port on stdout. Caller is expected to docker rm -f the container.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAME="${NJ_TEST_NAME:-nzs-test-$$}"
IMAGE="${NJ_TEST_IMAGE:-ninjamzap-server-test:latest}"

docker run --rm -d \
  --name "$NAME" \
  -p 0:2049 \
  -v "$SCRIPT_DIR/test.cfg:/opt/ninjam/server.cfg:ro" \
  "$IMAGE" >/dev/null

# Wait until port mapping is available (immediate in practice).
for _ in 1 2 3 4 5; do
  PORT="$(docker port "$NAME" 2049/tcp 2>/dev/null | head -1 | awk -F: '{print $NF}')"
  [[ -n "${PORT:-}" ]] && break
  sleep 0.1
done

if [[ -z "${PORT:-}" ]]; then
  echo "failed to read port from docker" >&2
  docker logs "$NAME" >&2 || true
  docker rm -f "$NAME" >/dev/null || true
  exit 1
fi

# Wait until ninjamsrv is actually listening.
for _ in $(seq 1 50); do
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
    break
  fi
  sleep 0.1
done

echo "$NAME $PORT"
