#!/bin/sh
set -eu
cd "$(dirname "$0")"
if [ "$(uname -s)" = Darwin ] && command -v brew >/dev/null 2>&1; then
    export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix curl)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
fi
cc -std=c11 -Os -Wall -Wextra -Wpedantic $(pkg-config --cflags libcurl openssl) brokie.c \
    -o brokie $(pkg-config --libs libcurl openssl) -lm -pthread
./brokie selftest
