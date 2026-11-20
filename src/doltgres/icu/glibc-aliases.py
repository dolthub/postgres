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

"""Writes glibc-aliases.tsv, the table from glibc's locale aliases (`german`, `no_NO`) to language tags (design item
12): glibc's `setlocale` stores an alias as it was typed, so a libc locale's stored name may be one, and it must give
the same tag on every platform. The aliases are glibc's own, from a saved copy of its `intl/locale.alias` at the
release the table's header names; each alias's tag is its locale's language and territory, the encoding left out, as
a POSIX name's is. A run prints each alias that Windows' table also holds with another tag, where glibc's wins.

usage: glibc-aliases.py <saved intl/locale.alias> (it rewrites glibc-aliases.tsv beside it)"""

import os
import re
import sys

# RELEASE names the glibc release the saved file is read from, and its commit.
RELEASE = "glibc 2.44, commit c3a3a9808ad3ab4a3336836833f83288b672ccbf"

here = os.path.dirname(os.path.abspath(__file__))
with open(sys.argv[1], encoding="ascii") as f:
    lines = f.read().split("\n")
copyright_line = next(line for line in lines if line.startswith("# Copyright"))
start = next(i for i, line in enumerate(lines) if line.startswith("# This program is free software"))
licence = lines[start:next(i for i, line in enumerate(lines) if "any later version" in line) + 1]
aliases = {}
for line in lines:
    if not line.strip() or line.startswith("#"):
        continue
    alias, locale = line.split()
    found = re.fullmatch(r"([a-z]{2,3})_([A-Z]{2})(\.[A-Za-z0-9-]+)?", locale)
    assert found, locale
    assert alias.lower() not in aliases, alias
    aliases[alias.lower()] = (alias, f"{found[1]}-{found[2]}")

windows = {}
with open(os.path.join(here, "windows-names.tsv"), encoding="utf-8") as f:
    for line in f.read().split("\n"):
        if line and not line.startswith("#"):
            name, tag = line.split("\t")[:2]
            windows[name.lower()] = tag
for key, (alias, tag) in sorted(aliases.items()):
    if key in windows and windows[key] != tag:
        print(f"{alias}: glibc's {tag}, Windows' {windows[key]}")

with open(os.path.join(here, "glibc-aliases.py"), encoding="utf-8") as f:
    header = "".join(line for line in f.readlines()[:13])
with open(os.path.join(here, "glibc-aliases.tsv"), "w", encoding="utf-8", newline="\n") as out:
    out.write(header)
    out.write(f"""
# glibc's locale aliases and the language tag each stands for (design item 12), matched whatever the case of their
# letters, as glibc matches them. Written by glibc-aliases.py, never by hand, from intl/locale.alias of
# {RELEASE}
# (https://sourceware.org/git/glibc.git), whose own header reads:
{copyright_line}
{chr(10).join(licence)}
# A row is an alias, a tab, and its tag.
""")
    for key, (alias, tag) in aliases.items():
        out.write(f"{alias}\t{tag}\n")
print(f"{len(aliases)} aliases")
