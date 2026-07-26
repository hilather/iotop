#!/usr/bin/env bash
# Finish Docker access for the current user (group + daemon).
set -euo pipefail

echo "==> Ensure docker group exists and $USER is a member"
sudo groupadd -f docker
sudo usermod -aG docker "$USER"
sudo systemctl enable --now docker

echo
echo "Group membership after change:"
getent group docker

echo
echo "Apply the group in this shell with:"
echo "  newgrp docker"
echo "Then verify:"
echo "  docker version"
echo "  docker run --rm hello-world"
echo
echo "Or open a new terminal / log out and back in."
