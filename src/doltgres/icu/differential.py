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

"""Runs every kind of ICU call Postgres makes over a corpus against one data archive and writes one hash for each
group of results, so that two runs, one over the filtered archive and one over ICU's whole data, can be compared line
by line (design item 12: the filter leaves out only what no call Postgres makes loads, and every line must be the
same).

usage: differential.py <build folder of make-data.sh> <archive under test> <corpus file> <output file>

The corpus is one string a line, as JSON. A run that finds no corpus file writes one from its own data (the display
names, which are text in every script the data knows, and runs of code points); run that once over the whole data, and
every other run reads the same file, so a difference is the calls' and never the corpus's."""

import ctypes
import hashlib
import json
import os
import sys

from library import ENCODINGS, bind, load

build, archive, corpus_file, output = sys.argv[1:]
common, i18n = load(build, archive)
P, I, C = ctypes.c_void_p, ctypes.c_int32, ctypes.c_char_p
ERR = ctypes.POINTER(ctypes.c_int)
UChars = ctypes.POINTER(ctypes.c_uint16)

uloc_countAvailable = bind(common, "uloc_countAvailable", I)
uloc_getAvailable = bind(common, "uloc_getAvailable", C, I)
uloc_toLanguageTag = bind(common, "uloc_toLanguageTag", I, C, C, I, ctypes.c_int8, ERR)
uloc_canonicalize = bind(common, "uloc_canonicalize", I, C, C, I, ERR)
uloc_getLanguage = bind(common, "uloc_getLanguage", I, C, C, I, ERR)
uloc_getDisplayName = bind(common, "uloc_getDisplayName", I, C, C, UChars, I, ERR)
ucol_countAvailable = bind(i18n, "ucol_countAvailable", I)
ucol_getAvailable = bind(i18n, "ucol_getAvailable", C, I)
ucol_open = bind(i18n, "ucol_open", P, C, ERR)
ucol_openRules = bind(i18n, "ucol_openRules", P, UChars, I, I, I, P, ERR)
ucol_close = bind(i18n, "ucol_close", None, P)
ucol_getVersion = bind(i18n, "ucol_getVersion", None, P, ctypes.c_char * 4)
ucol_getRules = bind(i18n, "ucol_getRules", UChars, P, ctypes.POINTER(I))
ucol_getSortKey = bind(i18n, "ucol_getSortKey", I, P, UChars, I, ctypes.c_char_p, I)
ucol_strcoll = bind(i18n, "ucol_strcoll", I, P, UChars, I, UChars, I)
ucol_strcollUTF8 = bind(i18n, "ucol_strcollUTF8", I, P, C, I, C, I, ERR)
ucol_setAttribute = bind(i18n, "ucol_setAttribute", None, P, I, I, ERR)
ucol_nextSortKeyPart = bind(i18n, "ucol_nextSortKeyPart", I, P, P, ctypes.POINTER(ctypes.c_uint32 * 2),
                            ctypes.c_char_p, I, ERR)
uiter_setString = bind(common, "uiter_setString", None, P, UChars, I)
uiter_setUTF8 = bind(common, "uiter_setUTF8", None, P, C, I)
ucol_getKeywordValuesForLocale = bind(i18n, "ucol_getKeywordValuesForLocale", P, C, C, ctypes.c_int8, ERR)
uenum_next = bind(common, "uenum_next", C, P, ctypes.POINTER(I), ERR)
uenum_close = bind(common, "uenum_close", None, P)
ucnv_open = bind(common, "ucnv_open", P, C, ERR)
ucnv_close = bind(common, "ucnv_close", None, P)
ucnv_getName = bind(common, "ucnv_getName", C, P, ERR)
ucnv_toUChars = bind(common, "ucnv_toUChars", I, P, UChars, I, ctypes.c_char_p, I, ERR)
ucnv_fromUChars = bind(common, "ucnv_fromUChars", I, P, ctypes.c_char_p, I, UChars, I, ERR)
ucasemap_open = bind(common, "ucasemap_open", P, C, ctypes.c_uint32, ERR)
ucasemap_close = bind(common, "ucasemap_close", None, P)
CASE_MAPS = [bind(common, "ucasemap_utf8" + name, I, P, ctypes.c_char_p, I, ctypes.c_char_p, I, ERR)
             for name in ("ToUpper", "ToLower", "ToTitle", "FoldCase")]
u_strToTitle = bind(common, "u_strToTitle", I, UChars, I, UChars, I, P, C, ERR)
u_strFoldCase = bind(common, "u_strFoldCase", I, UChars, I, UChars, I, ctypes.c_uint32, ERR)
CHAR_MAPS = [bind(common, name, I, I) for name in ("u_toupper", "u_tolower", "u_totitle")]
u_foldCase = bind(common, "u_foldCase", I, I, ctypes.c_uint32)
CLASSES = [bind(common, name, ctypes.c_int8, I) for name in (
    "u_isalpha", "u_isalnum", "u_isdigit", "u_isxdigit", "u_isupper", "u_islower", "u_isspace", "u_isblank",
    "u_ispunct", "u_isprint", "u_isgraph", "u_isUAlphabetic", "u_isUUppercase", "u_isULowercase", "u_isUWhiteSpace",
    "u_charType")]
u_hasBinaryProperty = bind(common, "u_hasBinaryProperty", ctypes.c_int8, I, I)
u_errorName = bind(common, "u_errorName", C, I)

lines = []


def record(group, parts):
    """Adds a group's line: its name and the hash of its results."""
    digest = hashlib.sha256()
    for part in parts:
        digest.update(part if isinstance(part, bytes) else str(part).encode())
        digest.update(b"\x00")
    lines.append(f"{group}\t{digest.hexdigest()}")


def utf16(text):
    """Returns a string as ICU's UChars and their count."""
    encoded = text.encode("utf-16-le")
    count = len(encoded) // 2
    return (ctypes.c_uint16 * max(count, 1)).from_buffer_copy(encoded + b"\x00\x00" * (1 if not count else 0)), count


def text_call(function, name, *more):
    """Returns the text an ICU function writes into a char buffer for a name, with its status."""
    buffer, status = ctypes.create_string_buffer(512), ctypes.c_int(0)
    function(name, buffer, 512, *more, ctypes.byref(status))
    return buffer.value + b"|" + u_errorName(status.value)


# The locales, and what Postgres asks of each.
locales = [uloc_getAvailable(i) for i in range(uloc_countAvailable())]
record("locales", locales)
record("language tags", [text_call(uloc_toLanguageTag, name, 1) for name in locales])
record("canonical names", [text_call(uloc_canonicalize, name) for name in locales])
record("languages", [text_call(uloc_getLanguage, name) for name in locales])
names = []
for name in locales:
    for display in (b"en", name):
        buffer, status = (ctypes.c_uint16 * 512)(), ctypes.c_int(0)
        count = uloc_getDisplayName(name, display, buffer, 512, ctypes.byref(status))
        names.append(bytes(buffer)[: 2 * max(count, 0)].decode("utf-16-le", "replace"))
record("display names", names)

# The corpus: read from its file, or made from this run's data and written there.
if os.path.exists(corpus_file):
    with open(corpus_file, encoding="utf-8") as f:
        corpus = [json.loads(line) for line in f]
else:
    corpus = sorted(set(names))[::3]
    for start in range(0x20, 0x30000, 0x1D3):
        corpus.append("".join(chr(c) for c in range(start, start + 6) if not 0xD800 <= c < 0xE000))
    corpus += ["", "a", "A", "a b", "ab", "résumé", "resume", "Résumé", "ǆx", "ß", "İ", "i", "ı", "ﬁ", "ǅ"]
    # Text of the scripts whose words the break dictionaries find, with cased letters beside and inside it, so
    # that a word boundary a dictionary moves would move a title-cased letter.
    for words in ("ภาษาไทยสวัสดีครับ", "ພາສາລາວສະບາຍດີ", "ភាសាខ្មែរសួស្តី", "မြန်မာဘာသာမင်္ဂလာပါ", "中文排序日本語", "ひらがなカタカナ"):
        half = len(words) // 2
        corpus += [words, words + "abc", "abc" + words, words[:half] + "abc" + words[half:],
                   words[:half] + "ǆx" + words[half:] + "ß", "x" + words[:3] + "y" + words[3:6] + "z" + words[6:],
                   words + " " + words + "def", words[:half] + "éa" + words[half:] + " ǆ"]
    with open(corpus_file, "w", encoding="utf-8", newline=chr(10)) as f:
        f.writelines(json.dumps(text) + chr(10) for text in corpus)
units = [utf16(text) for text in corpus]
utf8 = [text.encode("utf-8", "surrogatepass") for text in corpus]
record("corpus", utf8)

# The collators: each available locale, each collation type it names, and Postgres' own special tags.
collations = [ucol_getAvailable(i) for i in range(ucol_countAvailable())]
record("collation locales", collations)
tags = [b"", b"und", b"root", b"und-u-ks-level2", b"und-u-kn", b"en-u-kf-upper", b"de-u-co-phonebk",
        b"zh-u-co-big5han", b"zh-u-co-gb2312han", b"und-u-co-emoji", b"und-u-co-eor", b"en-US-u-va-posix"]
for name in collations:
    tags.append(name)
    status = ctypes.c_int(0)
    values = ucol_getKeywordValuesForLocale(b"collation", name, 1, ctypes.byref(status))
    while values:
        value = uenum_next(values, None, ctypes.byref(status))
        if not value:
            uenum_close(values)
            break
        tags.append(name + b"@collation=" + value)
for tag in tags:
    status = ctypes.c_int(0)
    collator = ucol_open(tag, ctypes.byref(status))
    parts = [u_errorName(status.value)]
    if collator:
        version = (ctypes.c_char * 4)()
        ucol_getVersion(collator, version)
        length = I(0)
        rules = ucol_getRules(collator, ctypes.byref(length))
        parts += [bytes(version), ctypes.string_at(rules, 2 * length.value)]
        keys = []
        key = ctypes.create_string_buffer(4096)
        for chars, count in units:
            size = ucol_getSortKey(collator, chars, count, key, 4096)
            keys.append(key.raw[:size])
        parts += keys
        order = sorted(range(len(units)), key=lambda at: keys[at])
        for left, right in zip(order[::7], order[3::7]):
            parts.append(ucol_strcoll(collator, units[left][0], units[left][1], units[right][0], units[right][1]))
            parts.append(ucol_strcollUTF8(collator, utf8[left], len(utf8[left]), utf8[right], len(utf8[right]),
                                          ctypes.byref(status)))
        iterator = ctypes.create_string_buffer(256)
        for chars, count in units[::11]:
            uiter_setString(iterator, chars, count)
            state, part = (ctypes.c_uint32 * 2)(), ctypes.create_string_buffer(8)
            size = ucol_nextSortKeyPart(collator, iterator, ctypes.byref(state), part, 8, ctypes.byref(status))
            parts.append(part.raw[:size])
        for text in utf8[::11]:
            uiter_setUTF8(iterator, text, len(text))
            state, part = (ctypes.c_uint32 * 2)(), ctypes.create_string_buffer(8)
            size = ucol_nextSortKeyPart(collator, iterator, ctypes.byref(state), part, 8, ctypes.byref(status))
            parts.append(part.raw[:size])
        ucol_close(collator)
    record("collator " + tag.decode(), parts)

# Rules of each kind a collation may be made from: orders, reorderings, imports of other collations, and sets of
# characters, which a rule may give by a character's name or by any property, each read from its own part of the data.
SETS = ["\\N{CYRILLIC SMALL LETTER A}", "\\N{LATIN SMALL LETTER C}-\\N{LATIN SMALL LETTER F}",
        "[:name=LATIN SMALL LETTER C:]", "[:Script=Greek:]", "[:Script_Extensions=Deva:]", "[:Block=Thai:]",
        "[:Emoji:]", "[:Basic_Emoji:]", "[:InPC=Top:]", "[:Indic_Syllabic_Category=Vowel:]",
        "[:Vertical_Orientation=U:]", "[:Age=5.0:]", "[:Line_Break=Alphabetic:]", "[:Word_Break=Katakana:]",
        "[:East_Asian_Width=Wide:]", "[:Joining_Type=Dual_Joining:]", "[:Numeric_Value=1:]", "[:ccc=230:]",
        "[:NFKC_Quick_Check=No:]", "[:Changes_When_NFKC_Casefolded:]", "[:Identifier_Type=Recommended:]",
        "[:Unified_Ideograph:]", "[:Lu:]", "[:alnum:]", "[[:Latin:]&[:Ll:]]"]
RULES = ["&a < 9 < 8", "&c < ch <<< Ch <<< CH", "[reorder Grek Latn]", "&[before 1]a < ä", "[import de-u-co-phonebk]",
         "[import und-u-co-search]&a < 9"]
RULES += [f"&a < 9 [{option} [{characters}]]" for characters in SETS for option in ("optimize", "suppressContractions")]
for rules in RULES:
    chars, count = utf16(rules)
    status = ctypes.c_int(0)
    collator = ucol_openRules(chars, count, -1, -1, None, ctypes.byref(status))
    parts = [u_errorName(status.value)]
    if collator:
        key = ctypes.create_string_buffer(4096)
        for text_chars, text_count in units:
            parts.append(key.raw[: ucol_getSortKey(collator, text_chars, text_count, key, 4096)])
        ucol_close(collator)
    record("rules " + rules, parts)

# The converters of Postgres' server encodings.
for name in ENCODINGS:
    status = ctypes.c_int(0)
    converter = ucnv_open(name.encode(), ctypes.byref(status))
    parts = [u_errorName(status.value)]
    if converter:
        parts.append(ucnv_getName(converter, ctypes.byref(status)))
        encoded, decoded = ctypes.create_string_buffer(8192), (ctypes.c_uint16 * 4096)()
        for chars, count in units:
            status = ctypes.c_int(0)
            size = ucnv_fromUChars(converter, encoded, 8192, chars, count, ctypes.byref(status))
            parts.append(encoded.raw[: max(size, 0)])
            back = ucnv_toUChars(converter, decoded, 4096, encoded, max(size, 0), ctypes.byref(status))
            parts += [bytes(decoded)[: 2 * max(back, 0)], status.value]
        ucnv_close(converter)
    record("converter " + name, parts)

# Case mapping by locale, and title case through the break iterator.
for locale in (b"", b"en", b"tr", b"az", b"lt", b"el", b"nl", b"de", b"th", b"zh", b"ja", b"km", b"my", b"lo"):
    status = ctypes.c_int(0)
    case_map = ucasemap_open(locale, 0, ctypes.byref(status))
    parts = [u_errorName(status.value)]
    mapped = ctypes.create_string_buffer(16384)
    for function in CASE_MAPS:
        for text in utf8:
            status = ctypes.c_int(0)
            size = function(case_map, mapped, 16384, text, len(text), ctypes.byref(status))
            parts += [mapped.raw[: max(size, 0)], status.value]
    title = (ctypes.c_uint16 * 8192)()
    for chars, count in units:
        status = ctypes.c_int(0)
        size = u_strToTitle(title, 8192, chars, count, None, locale, ctypes.byref(status))
        parts += [bytes(title)[: 2 * max(size, 0)], status.value]
    ucasemap_close(case_map)
    record("case " + locale.decode(), parts)
folded = (ctypes.c_uint16 * 8192)()
parts = []
for chars, count in units:
    status = ctypes.c_int(0)
    size = u_strFoldCase(folded, 8192, chars, count, 0, ctypes.byref(status))
    parts += [bytes(folded)[: 2 * max(size, 0)], status.value]
record("fold case", parts)

# Every code point: its case mappings, its classes, and the binary properties.
for at, function in enumerate(CHAR_MAPS):
    record(f"character map {at}", [function(c) for c in range(0x110000)])
record("character fold", [u_foldCase(c, 0) for c in range(0x110000)])
for at, function in enumerate(CLASSES):
    record(f"character class {at}", bytes(function(c) & 0xFF for c in range(0x110000)))
for which in range(0, 80):
    record(f"binary property {which}", bytes(u_hasBinaryProperty(c, which) & 1 for c in range(0, 0x110000, 3)))

with open(output, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print(len(lines), "groups;", len(corpus), "strings;", len(tags), "collators")
