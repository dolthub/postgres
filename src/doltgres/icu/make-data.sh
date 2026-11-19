#!/bin/sh
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
# Makes the vendored ICU's data archive (design item 12) from upstream's
# sources: it clones ICU at the commit pins.tsv names, with upstream's line
# endings on every platform, and runs ICU's own configure and make in a new
# work folder, with filter.json unless ICU's whole data is asked for. It
# prints the archive's path and SHA-256. The filtered archive is
# icudt79l.dat, byte for byte, on every platform; the whole one and the
# libraries built beside it are what differential.py and converters.py read.
#
# usage: make-data.sh <work folder, which must not exist yet> [full]
#
# It needs git, a C++17 compiler, GNU make (MAKE names it when it isn't
# make; MAKEFLAGS sets its jobs), and Python 3 (PYTHON names it when it isn't
# python3).

set -eu

here=$(cd "$(dirname "$0")" && pwd)

case $(uname) in
	Darwin) platform=macOS ;;
	Linux) platform=Linux ;;
	*)
		echo "make-data.sh: ICU's configure has no platform for $(uname)" >&2
		exit 1
		;;
esac

work=$1
whole=${2:-}
pin=$(awk -F '\t' '$1 == "icu/common" { print $2, $4 }' "$here/../third_party/pins.tsv")

mkdir "$work"
cd "$work"
git clone --quiet --no-checkout --filter=blob:none -c core.autocrlf=false -c core.eol=lf "${pin% *}" icu
git -C icu sparse-checkout set icu4c/source
git -C icu checkout --quiet --detach "${pin#* }"

if test "$whole" = full; then
	unset ICU_DATA_FILTER_FILE
else
	ICU_DATA_FILTER_FILE=$here/filter.json
	export ICU_DATA_FILTER_FILE
fi
PYTHON=${PYTHON:-python3}
export PYTHON

mkdir build
cd build
sh ../icu/icu4c/source/runConfigureICU "$platform" --with-data-packaging=archive \
	--disable-tests --disable-samples --disable-extras --disable-icuio --disable-layoutex
"${MAKE:-make}"
"$PYTHON" -c 'import hashlib, os, sys
path = os.path.abspath(sys.argv[1])
print(path, hashlib.sha256(open(path, "rb").read()).hexdigest())' data/out/icudt79l.dat
