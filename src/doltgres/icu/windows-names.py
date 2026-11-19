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

"""Adds the locale names of the Windows it runs on to windows-names.tsv, the table from Windows' traditional locale
names (`English_United States`, or a language's name alone) to language tags (design item 12): a libc locale's stored
name must give the same tag on every platform, so the table is in the tree and no operating system is asked when a
locale is opened.

Windows itself gives each of its locales' English language name, English country name, and tag, and for a locale of a
language alone the tag of that language's default country. Windows renames countries and languages between releases
and a stored name keeps its old spelling, so a run keeps every row the table has and adds its own; a name that would
get a second tag stops the run. The names Postgres' own `win32setlocale.c` maps are rows too, each beside the name it
stands for.

Given a saved copy of the page of Microsoft's published list of its locales, a run adds that list's names as well,
which hold the spellings of earlier Windows releases: [MS-LCID] Windows Language Code Identifier (LCID) Reference,
Appendix A: Product Behavior, whose Language, Location, and Language tag columns Microsoft's documentation of the C
runtime's country and region strings names as the list of names by Windows version.

usage: windows-names.py [<saved page of [MS-LCID]'s Appendix A>]
(on Windows; it rewrites windows-names.tsv beside it)"""

import ctypes
import html
import os
import re
import sys
import winreg
from ctypes import wintypes

# The spellings `win32setlocale.c` maps: each country or language name it knows, and the name this table has for it.
COUNTRIES = {"Hong Kong S.A.R.": "Hong Kong SAR", "Macau S.A.R.": "Macao SAR", "Macao S.A.R.": "Macao SAR",
             "U.A.E.": "United Arab Emirates"}
LANGUAGES = {"Norwegian": "Norwegian Bokmål", "Norwegian (Bokmål)": "Norwegian Bokmål"}

LOCALE_SENGLISHLANGUAGENAME, LOCALE_SENGLISHCOUNTRYNAME = 0x1001, 0x1002
LOCALE_ALTERNATE_SORTS, LOCALE_NEUTRALDATA = 0x4, 0x10
ENUM = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.LPWSTR, wintypes.DWORD, wintypes.LPARAM)
kernel32 = ctypes.WinDLL("kernel32")
kernel32.EnumSystemLocalesEx.argtypes = [ENUM, wintypes.DWORD, wintypes.LPARAM, ctypes.c_void_p]
kernel32.GetLocaleInfoEx.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.LPWSTR, ctypes.c_int]
kernel32.ResolveLocaleName.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_int]

table = os.path.join(os.path.dirname(os.path.abspath(__file__)), "windows-names.tsv")
rows = {}
cased = {}
locales = []


def add(name, tag, kept=False):
    """Adds a name's row unless the table has the name in some case of its letters. A name that already has another
    tag stops the run, or keeps the tag it has where that is asked for."""
    known = cased.setdefault(name.lower(), name)
    if rows.setdefault(known, tag).lower() != tag.lower() and not kept:
        sys.exit(f"{name} is {rows[known]} in the table and {tag} here")


def published(page):
    """Adds the names of Microsoft's published list: each row's language and location, with its tag, but for the rows
    of a language alone and of a sort name, and for a name the list itself gives two tags, which it prints."""
    with open(page, encoding="utf-8") as f:
        text = f.read()
    found = {}
    for row in re.findall(r"<tr.*?</tr>", text, flags=re.S):
        cells = [re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", cell))).strip()
                 for cell in re.findall(r"<td[^>]*>(.*?)</td>", row, flags=re.S)]
        if len(cells) == 5 and cells[1] and cells[2].startswith("0x") and "_" not in cells[3]:
            found.setdefault(f"{cells[0]}_{cells[1]}", set()).add(cells[3])
    for name, tags in found.items():
        if len(tags) == 1:
            add(name, tags.pop())
        else:
            print(f"left out: {name} is {' and '.join(sorted(tags))} in the published list")


def text(call, locale, *more):
    """Returns the string a Windows locale function writes for a locale."""
    buffer = ctypes.create_unicode_buffer(256)
    if not call(locale, *more, buffer, 256):
        sys.exit(f"Windows gave nothing for {locale}")
    return buffer.value


def each(locale, flags, _):
    """Notes one of Windows' locales and its kind."""
    locales.append((locale, flags))
    return True


def read(locale, flags):
    """Adds one of Windows' locales: its language and country, and its language alone when it names no country. Where
    a locale of a language and a script has the name of the language's own locale, the name stays the language's."""
    language = text(kernel32.GetLocaleInfoEx, locale, LOCALE_SENGLISHLANGUAGENAME)
    country = text(kernel32.GetLocaleInfoEx, locale, LOCALE_SENGLISHCOUNTRYNAME)
    tag = text(kernel32.ResolveLocaleName, locale)
    for spelled in [language] + [old for old, new in LANGUAGES.items() if new == language]:
        if flags & LOCALE_NEUTRALDATA:
            add(spelled, tag, "-" in locale)
        for land in [country] + [old for old, new in COUNTRIES.items() if new == country]:
            add(f"{spelled}_{land}", tag)


builds = []
lists = []
with open(table, encoding="utf-8", newline="") as f:
    header = []
    for line in f.read().splitlines():
        if line.startswith("# Windows builds read:"):
            builds = line.split(":", 1)[1].split()
        elif line.startswith("# Published lists read:"):
            lists = [found.strip() for found in line.split(":", 1)[1].split(";") if found.strip()]
        elif line.startswith("#") or not line:
            header.append(line)
        else:
            add(*line.split("\t"))
kernel32.EnumSystemLocalesEx(ENUM(each), 0, 0, None)
for locale, flags in sorted(locales, key=lambda found: ("-" in found[0], found[0])):
    if locale and not flags & LOCALE_ALTERNATE_SORTS:
        read(locale, flags)
version = sys.getwindowsversion()
with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion") as key:
    build = f"{version.major}.{version.minor}.{version.build}.{winreg.QueryValueEx(key, 'UBR')[0]}"
if build not in builds:
    builds.append(build)
if len(sys.argv) > 1:
    published(sys.argv[1])
    with open(sys.argv[1], encoding="utf-8") as f:
        updated = re.search(r'"updated_at" content="([0-9-]+)', f.read()).group(1)
    name = f"[MS-LCID] Appendix A: Product Behavior, as updated {updated}"
    if name not in lists:
        lists.append(name)
with open(table, "w", encoding="utf-8", newline="") as f:
    f.write("\n".join(header + ["# Windows builds read: " + " ".join(builds),
                               "# Published lists read: " + "; ".join(lists)]) + "\n")
    f.writelines(f"{name}\t{tag}\n" for name, tag in sorted(rows.items()))
