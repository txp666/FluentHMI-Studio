#!/bin/sh
set -eu

# Ubuntu's archive and ports archive carry the same official signed metadata at MIT.
case "$(dpkg --print-architecture)" in
    amd64) archive=ubuntu ;;
    arm64) archive=ubuntu-ports ;;
    *) echo 'Unsupported Ubuntu architecture' >&2; exit 1 ;;
esac
sources=${1:-/etc/apt/sources.list.d/ubuntu.sources}
# Hosted x64 runners restore this mirror-list URI during apt update. Keep its index identity.
if grep -q '^URIs:.*mirror+file:/etc/apt/apt-mirrors.txt' "$sources"; then
    printf 'https://mirrors.mit.edu/%s/\n' "$archive" > "${2:-/etc/apt/apt-mirrors.txt}"
else
    # Direct sources (ARM runners and clean containers) retain suites, components and Signed-By.
    sed -i "s|^URIs:.*|URIs: https://mirrors.mit.edu/$archive/|" "$sources"
fi
