#!/bin/bash
#
# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# deps.sh <source tree>: installs what building and testing the fork needs on a GitHub runner, Ubuntu or macOS: the
# packages the queue's Linux container has (or their Homebrew equivalents), Perl's IPC::Run for the TAP suites, the
# meson the queue uses, and the en_US.UTF-8 locale the reference runs under. Only a tree with the Rust workspace also
# gets the Rust toolchain it pins and, on Linux, the libclang its bindings are generated with. Paths and settings later
# steps need go to GITHUB_PATH and GITHUB_ENV.
set -euo pipefail

if [ "$(uname)" = Linux ]; then
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends build-essential flex bison pkg-config perl libipc-run-perl \
    libicu-dev libxml2-dev liblz4-dev libzstd-dev libssl-dev libreadline-dev zlib1g-dev ninja-build locales
  sudo locale-gen en_US.UTF-8
else
  brew install bison flex icu4c lz4 zstd openssl@3 pkg-config ninja cpanminus
  cpanm --local-lib "$HOME/perl5" --notest IPC::Run
  echo "PERL5LIB=$HOME/perl5/lib/perl5" >> "$GITHUB_ENV"
  echo "$(brew --prefix bison)/bin" >> "$GITHUB_PATH"
  echo "$(brew --prefix flex)/bin" >> "$GITHUB_PATH"
  pkgconfig=""
  for formula in icu4c lz4 zstd openssl@3; do
    pkgconfig="$pkgconfig${pkgconfig:+:}$(brew --prefix "$formula")/lib/pkgconfig"
  done
  echo "PKG_CONFIG_PATH=$pkgconfig" >> "$GITHUB_ENV"
fi
echo "LANG=en_US.UTF-8" >> "$GITHUB_ENV"
echo "LC_ALL=en_US.UTF-8" >> "$GITHUB_ENV"
pipx install meson==1.11.2

if [ -f "$1/src/doltgres/Cargo.toml" ]; then
  if [ "$(uname)" = Linux ]; then
    sudo apt-get install -y --no-install-recommends clang libclang-dev llvm
  fi
  if ! command -v rustup > /dev/null; then
    curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain none
    echo "$HOME/.cargo/bin" >> "$GITHUB_PATH"
    export PATH="$HOME/.cargo/bin:$PATH"
  fi
  (cd "$1/src/doltgres" && rustup toolchain install)
fi
