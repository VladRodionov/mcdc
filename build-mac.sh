#!/usr/bin/env bash
# macOS (Homebrew) equivalent of the Linux build steps

set -euo pipefail

# 0) Ensure Xcode Command Line Tools (compilers) are installed
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Installing Xcode Command Line Tools..."
  xcode-select --install || true
  echo "Re-run this script after the tools finish installing."
  exit 1
fi

# 1) Install build deps via Homebrew
brew update
brew install \
  pkg-config \
  autoconf \
  automake \
  libtool \
  m4 \
  autoconf-archive \
  libevent \
  zstd

# 2) Homebrew paths (Intel: /usr/local, Apple Silicon: /opt/homebrew)
BREW_PREFIX="$(brew --prefix)"

# 3) Help autotools find Homebrew libs/headers
export PKG_CONFIG_PATH="${BREW_PREFIX}/opt/libevent/lib/pkgconfig:${BREW_PREFIX}/opt/zstd/lib/pkgconfig:${PKG_CONFIG_PATH-}"
export CPPFLAGS="-I${BREW_PREFIX}/opt/libevent/include -I${BREW_PREFIX}/opt/zstd/include ${CPPFLAGS-}"
export LDFLAGS="-L${BREW_PREFIX}/opt/libevent/lib -L${BREW_PREFIX}/opt/zstd/lib ${LDFLAGS-}"

# 4) macOS uses 'glibtoolize' instead of 'libtoolize'
export LIBTOOLIZE=glibtoolize

# 5) Generate build system, configure, and build
./autogen.sh
# If configure still can't find libs, add explicit prefixes:
#   --with-libevent="${BREW_PREFIX}/opt/libevent" --with-zstd="${BREW_PREFIX}/opt/zstd"
./configure --with-zstd --with-libevent
make -j"$(sysctl -n hw.ncpu)"
