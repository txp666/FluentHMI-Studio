#!/bin/sh
set -eu

# Ubuntu's archive and ports archive carry the same official signed metadata at MIT.
case "$(dpkg --print-architecture)" in
    amd64) archive=ubuntu ;;
    arm64) archive=ubuntu-ports ;;
    *) echo 'Unsupported Ubuntu architecture' >&2; exit 1 ;;
esac
# Preserve suites, components and Signed-By, including in the clean export-test container.
sed -i "s|^URIs:.*|URIs: https://mirrors.mit.edu/$archive/|" "${1:-/etc/apt/sources.list.d/ubuntu.sources}"
