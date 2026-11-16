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
# build.sh <source tree> <build folder>: builds the fork with meson and installs it into the build folder's own prefix,
# with the options the merge queue's builds use (assertions, injection points, ICU, libxml, lz4, zstd, OpenSSL, and
# the TAP suites when Perl's IPC::Run is installed), so CI and the queue test the same build. When targets.sh names
# targets for the tree, it builds only those and installs nothing.
set -euo pipefail

read -r -a targets <<< "$("$(dirname "$0")/targets.sh" "$1")"
tap=disabled
if perl -MIPC::Run -e 1 2> /dev/null; then
  tap=enabled
fi
meson setup "$2" "$1" --buildtype=debugoptimized --prefix="$2/install" -Dcassert=true -Dinjection_points=true \
  -Dicu=enabled -Dlibxml=enabled -Dlz4=enabled -Dzstd=enabled -Dssl=openssl -Dtap_tests=$tap
if [ ${#targets[@]} -gt 0 ]; then
  meson compile -C "$2" "${targets[@]}"
else
  ninja -C "$2"
  ninja -C "$2" install
fi
