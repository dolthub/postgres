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

"""Prints the converter each of Postgres' server encodings opens, by ICU's own answer over its whole data: the name,
the converter's own name, ICU's status, and the converter's type. The data filter's list of converters is the second
column (design item 12).

usage: converters.py <build folder of make-data.sh> <the whole data's archive>"""

import ctypes
import sys

from library import ENCODINGS, bind, load

common, _ = load(sys.argv[1], sys.argv[2])
ERR = ctypes.POINTER(ctypes.c_int)
ucnv_open = bind(common, "ucnv_open", ctypes.c_void_p, ctypes.c_char_p, ERR)
ucnv_getName = bind(common, "ucnv_getName", ctypes.c_char_p, ctypes.c_void_p, ERR)
ucnv_getType = bind(common, "ucnv_getType", ctypes.c_int, ctypes.c_void_p)
for name in ENCODINGS:
    status = ctypes.c_int(0)
    converter = ucnv_open(name.encode(), ctypes.byref(status))
    opened = ucnv_getName(converter, ctypes.byref(status)).decode() if converter else "FAILED"
    print(f"{name}\t{opened}\t{status.value}\t{ucnv_getType(converter) if converter else ''}")
