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

"""Loads the ICU libraries a run of make-data.sh built, with one data archive as the only data they read (design item
12), for the scripts here that ask ICU itself: differential.py and converters.py."""

import ctypes
import os
import sys

# ICU's major version, which its library files and every one of its C names carry.
VERSION = "79"

# Postgres' server encodings under the names its ICU code opens their converters by (`pg_enc2icu_tbl` in
# `pg_locale_icu.c`).
ENCODINGS = ["EUC-JP", "EUC-CN", "EUC-KR", "EUC-TW", "UTF-8", "ISO-8859-1", "ISO-8859-2", "ISO-8859-3", "ISO-8859-4",
             "ISO-8859-9", "ISO-8859-10", "ISO-8859-13", "ISO-8859-14", "ISO-8859-15", "CP1256", "CP1258", "CP866",
             "KOI8-R", "CP1251", "CP1252", "ISO-8859-5", "ISO-8859-6", "ISO-8859-7", "ISO-8859-8", "CP1250", "CP1253",
             "CP1254", "CP1255", "CP1257", "KOI8-U"]

# Each platform's files for the empty data library the build links, the common library, and the i18n library, in the
# order they are loaded, so that each finds the one before it already there.
FILES = {
    "win32": [f"stubdata/icudt{VERSION}.dll", f"lib/icuuc{VERSION}.dll", f"lib/icuin{VERSION}.dll"],
    "darwin": [f"stubdata/libicudata.{VERSION}.dylib", f"lib/libicuuc.{VERSION}.dylib",
               f"lib/libicui18n.{VERSION}.dylib"],
    "linux": [f"stubdata/libicudata.so.{VERSION}", f"lib/libicuuc.so.{VERSION}", f"lib/libicui18n.so.{VERSION}"],
}


def bind(library, name, restype, *argtypes):
    """Returns an ICU function of this version with its types."""
    function = getattr(library, f"{name}_{VERSION}")
    function.restype, function.argtypes = restype, list(argtypes)
    return function


def load(build, archive):
    """Returns the common and i18n libraries of make-data.sh's build folder, with the archive as ICU's data, its bytes
    kept with the common library since ICU keeps only their address."""
    mode = {"winmode": 0} if sys.platform == "win32" else {"mode": ctypes.RTLD_GLOBAL}
    _, common, i18n = [ctypes.CDLL(os.path.join(os.path.abspath(build), file), **mode) for file in FILES[sys.platform]]
    with open(archive, "rb") as f:
        data = f.read()
    common.archive = ctypes.create_string_buffer(data, len(data))
    status = ctypes.c_int(0)
    bind(common, "udata_setCommonData", None, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int))(
        common.archive, ctypes.byref(status))
    if status.value > 0:
        sys.exit(f"ICU refused {archive} as its data: status {status.value}")
    return common, i18n
