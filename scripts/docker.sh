#!/usr/bin/env bash
# Thin wrapper: use plain docker if the socket is usable, otherwise sudo docker.
set -euo pipefail

if docker info >/dev/null 2>&1; then
  exec docker "$@"
fi

if sudo -n docker info >/dev/null 2>&1; then
  exec sudo docker "$@"
fi

echo "Docker socket not accessible as $USER; using sudo docker (password may be required)." >&2
exec sudo docker "$@"
