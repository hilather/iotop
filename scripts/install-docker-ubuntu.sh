#!/usr/bin/env bash
# Install Docker on Ubuntu so we can run Rocky Linux 8 iotop tests.
set -euo pipefail

echo "==> Installing docker.io, compose plugin, and uidmap"
sudo apt-get update
sudo apt-get install -y docker.io docker-compose-v2 uidmap

echo "==> Enabling Docker service"
sudo systemctl enable --now docker

echo "==> Adding $USER to the docker group"
sudo usermod -aG docker "$USER"

echo
echo "Done. Either:"
echo "  1) run:  newgrp docker"
echo "  2) or log out and back in"
echo
echo "Then verify with:"
echo "  docker version"
echo "  docker run --rm hello-world"
