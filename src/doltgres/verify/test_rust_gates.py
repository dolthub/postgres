#!/usr/bin/env python3
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
"""test_rust_gates.py <work folder>: runs rust_gates.py over synthetic workspaces, a clean one and one per way of
cheating each gate, and fails unless the clean one passes and each other one reports the problem it plants. Runs
whenever the gates change."""

import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rust_gates  # noqa: E402

LICENSE = "// Copyright 2026 Dolthub, Inc.\n"
ROOT = LICENSE + "\n//! A synthetic crate.\n\n#![forbid(unsafe_code)]\n\n/// Returns one.\npub fn one() -> u32 {\n    1\n}\n"


def write(path, text):
    """Writes a file, making its folder."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def manifest(name, extra=""):
    """Returns a member crate's manifest, taking the workspace lints."""
    return (f'[package]\nname = "{name}"\nversion = "0.0.0"\nedition = "2024"\nlicense = "Apache-2.0"\npublish = false\n\n{extra}'
            "[lints]\nworkspace = true\n")


def workspace(folder, members, files):
    """Builds a synthetic workspace: its manifest, toolchain pin, and the files given."""
    shutil.rmtree(folder, ignore_errors=True)
    write(os.path.join(folder, "Cargo.toml"),
          f'[workspace]\nmembers = {members!r}\nresolver = "3"\n\n[workspace.lints.rust]\nunsafe_code = "forbid"\n\n'
          '[workspace.lints.clippy]\ndisallowed_macros = "deny"\ndisallowed_methods = "deny"\n'
          'disallowed_types = "deny"\n\n[profile.cassert]\ninherits = "release"\ndebug-assertions = true\n'
          .replace("'", '"'))
    write(os.path.join(folder, "rust-toolchain.toml"),
          '[toolchain]\nchannel = "1.99.1"\ncomponents = ["clippy", "rustfmt", "llvm-tools"]\n')
    for path, text in files.items():
        write(os.path.join(folder, path), text)
    for path in ["Cargo.toml", *(p for p in files if p.endswith("Cargo.toml") and files[p].startswith("[workspace]"))]:
        if os.path.join(os.path.dirname(path), "Cargo.lock").lstrip("/") not in files:
            subprocess.run(["cargo", "generate-lockfile", "--offline"], cwd=os.path.join(folder, os.path.dirname(path)),
                           capture_output=True)
    commit(folder)


def commit(folder):
    """Makes a folder a git repository holding one commit of everything in it, as the gates read committed files."""
    for args in (["init", "-q"], ["add", "-A"], ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q",
                                                 "--allow-empty", "-m", "c"]):
        subprocess.run(["git", "-C", folder, *args], capture_output=True)


DOLTGRUST_MANIFEST = '[package]\nname = "trees"\nversion = "0.0.0"\nedition = "2024"\n'
PORTED_ROWS = ("trees/src/lib.rs\tcrates/trees/src/lib.rs\tHEAD\tsame\t-\t-\n"
               "trees/Cargo.toml\tcrates/trees/Cargo.toml\tHEAD\tchanged\t"
               + rust_gates.ported_diff(DOLTGRUST_MANIFEST.encode(), manifest("trees").encode())
               + "\tCargo.toml inherits the workspace's lints\n")

FE_CALLS = "size\tint64\tvoid\terrno\t{body}\ttests::a_file_is_sized\n"

CASES = {
    "clean": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT}, None),
    "unformatted source": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                          "doltgres/src/lib.rs": ROOT.replace("    1\n", "1\n")}, "rustfmt fails"),
    "a pending check": (["doltgres", "store"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                                                "store/Cargo.toml": manifest("store"), "store/src/lib.rs": ROOT},
                        "which isn't live yet",
                        {"pending.tsv": "a case's check that waits for the store folder\tfile:store\ta case's piece\n"}),
    "a cargo warning": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                                       ".cargo/config.toml": "[build]\nbogus = 1\n"}, None),
    "a license not on the list": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres",
                                                                                 '[dependencies]\noutside = { path = "../../outside" }\n\n'),
                                                 "doltgres/src/lib.rs": ROOT,
                                                 "../outside/Cargo.toml": '[package]\nname = "outside"\nversion = "0.1.0"\n'
                                                                          'edition = "2024"\nlicense = "GPL-3.0-only"\n',
                                                 "../outside/src/lib.rs": "pub fn f() {}\n"},
                                  "cargo-deny fails"),
    "an unvetted crate": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\ninterpolator = "0.5"\n\n'),
                                         "doltgres/src/lib.rs": ROOT},
                          "cargo-vet fails"),
    "an exemption": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT},
                     "cargo-vet's config.toml holds an exemption",
                     {"supply-chain/config.toml": '\n[[exemptions.shlex]]\nversion = "1.3.0"\ncriteria = "safe-to-deploy"\n'}),
    "a vet policy": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT},
                     "cargo-vet's config.toml holds a policy",
                     {"supply-chain/config.toml": '\n[policy.doltgres]\ncriteria = "safe-to-run"\n'}),
    "a member under verify": (["doltgres", "verify/tool"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                            "doltgres/src/lib.rs": ROOT,
                                                            "verify/tool/Cargo.toml": manifest("tool"),
                                                            "verify/tool/src/lib.rs": ROOT},
                              "tool is a member under verify"),
    "a problem behind a feature": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres", "[features]\nextra = []\n\n"),
                                                  "doltgres/src/lib.rs": ROOT + '\n/// Behind a feature.\n#[cfg(feature = "extra")]\n'
                                                                         "pub fn extra() {\n    let unused = 1;\n}\n"},
                                   "(with every feature)"),
    "a ported crate with its rows": (["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT,
                                                 "doltgrust.tsv": PORTED_ROWS,
                                                 "../doltgrust/crates/trees/Cargo.toml": DOLTGRUST_MANIFEST,
                                                 "../doltgrust/crates/trees/src/lib.rs": ROOT},
                                     ["!doltgrust.tsv", "!DOLTGRUST_DIR"]),
    "a ported file edited after recording": (["trees"], {"trees/Cargo.toml": manifest("trees"),
                                                         "trees/src/lib.rs": ROOT.replace("    1\n", "    2\n"),
                                                         "doltgrust.tsv": PORTED_ROWS,
                                                         "../doltgrust/crates/trees/Cargo.toml": DOLTGRUST_MANIFEST,
                                                         "../doltgrust/crates/trees/src/lib.rs": ROOT},
                                             "doltgrust.tsv: trees/src/lib.rs isn't the same as Doltgrust's"),
    "a ported crate with its rows (in a subfolder)": (["trees"], {"trees/Cargo.toml": manifest("trees"),
                                                                  "trees/src/lib.rs": ROOT, "doltgrust.tsv": PORTED_ROWS,
                                                                  "../../doltgrust/crates/trees/Cargo.toml": DOLTGRUST_MANIFEST,
                                                                  "../../doltgrust/crates/trees/src/lib.rs": ROOT},
                                                      ["!doltgrust.tsv", "!DOLTGRUST_DIR"]),
    "a fork crate's copied file recorded wrongly": (["doltgres", "sim"], {
        "doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT, "sim/Cargo.toml": manifest("sim"),
        "sim/src/lib.rs": ROOT, "sim/src/faults.rs": "pub fn faults() {}\n",
        "doltgrust.tsv": "sim/src/faults.rs\tcrates/chunks/src/sim.rs\tHEAD\tchanged\t" + "0" * 64 + "\tmoved to sim\n",
        "../doltgrust/crates/chunks/src/sim.rs": "pub fn sim() {}\n"},
        "doltgrust.tsv: sim/src/faults.rs's change from Doltgrust isn't the one recorded"),
    "a wrapper calling pg_strdup": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                                                "pg/frontend-calls.list": FE_CALLS.format(body='result = pg_strdup("x") != NULL;')},
                                    "frontend-calls.list's size calls pg_strdup, which frontend-callees.tsv doesn't "
                                    "list"),
}
VENDORED = "int zlib_one(void) { return 1; }\n"


def vendored(content=VENDORED, build="files('lib.c')", extra=None):
    """Returns doltgres with a vendored zlib in third_party, pinned to VENDORED's blob, its build naming what's given."""
    blob = hashlib.sha1(b"blob %d\0" % len(VENDORED.encode()) + VENDORED.encode()).hexdigest()
    files = {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
             "third_party/pins.tsv": "# path\tupstream\ttag\tcommit\tupstream path\tobject\n"
                                     f"zlib/lib.c\thttps://example.com/zlib\tv1\t{'0' * 40}\tlib.c\t{blob}\n",
             "third_party/zlib/lib.c": content,
             "third_party/zlib/meson.build": f"project('zlib', 'c')\nzlib = static_library('z', {build})\n"}
    files.update(extra or {})
    return files


CASES["an outstanding compile-fail case"] = (
    ["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT},
    "tokens.tsv: chunks/fail/raw_forged is still outstanding",
    {"tokens.tsv": "chunks\tchunks/fail/raw_forged\t-\toutstanding\t-\t-\n"})
CASES["a compile-fail case with its C form"] = (
    ["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT}, ["!outstanding"],
    {"tokens.tsv": "chunks\tchunks/fail/raw_forged\tchunks::pack::Raw\topaque-storage\tits layout is pack.c's\t"
                   "raw_forged\n"})
CASES["vendored code as pinned"] = (["doltgres"], vendored(), ["!third_party"])
CASES["vendored code changed from its pin"] = (
    ["doltgres"], vendored(content=VENDORED.replace("1;", "2;")), "third_party/zlib/lib.c isn't the object pins.tsv")
CASES["an unpinned file in third_party"] = (
    ["doltgres"], vendored(extra={"third_party/zlib/patched.c": VENDORED}),
    "third_party/zlib/patched.c is neither pinned nor one of the fork's own files")
CASES["a vendored build naming a file outside its pin"] = (
    ["doltgres"], vendored(build="files('lib.c', '../patched.c')"),
    "third_party/zlib/meson.build names ../patched.c, outside its pinned paths")
CASES["a vendored build bringing in other code"] = (
    ["doltgres"], vendored(build="files('lib.c')\nsubdir('more')"),
    "third_party/zlib/meson.build calls subdir(), which a vendored build may not")
CASES["a vendored build holding a path in a variable"] = (
    ["doltgres"], vendored(build="files('lib.c')\npatched = '../../pg/patched.c'\nmore = files(patched)"),
    "third_party/zlib/meson.build names ../../pg/patched.c, outside its pinned paths")
CASES["a vendored build with a generator"] = (
    ["doltgres"], vendored(build="files('lib.c')\ngen = generator(find_program('cp'), output: '@PLAINNAME@')"),
    "third_party/zlib/meson.build calls generator(), which a vendored build may not")
CASES["a vendored build choosing sources by CPU, with MSVC's flag"] = (
    ["doltgres"], vendored(build="files('lib.c'), c_args: host_machine.cpu_family() == 'x86_64' ? ['/arch:AVX2'] : []"),
    ["!third_party"])
CASES["a vendored build naming an absolute path"] = (
    ["doltgres"], vendored(build="files('lib.c'), c_args: ['/usr/include/x.h']"),
    "third_party/zlib/meson.build names /usr/include/x.h, outside its pinned paths")
COPIED = "// Doltgrust's test.\n#![feature(test)]\n"


def copied(content=COPIED, pinned=COPIED):
    """Returns doltgres with a file of Doltgrust's tests in test/doltgrust, pinned to the blob of pinned, beside
    Doltgrust's checkout holding COPIED at that path."""
    blob = hashlib.sha1(b"blob %d\0" % len(pinned.encode()) + pinned.encode()).hexdigest()
    return {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
            "test/doltgrust/pins.tsv": "# path\tupstream\tbranch\tcommit\tupstream path\tobject\n"
                                       f"tests/a.rs\thttps://example.com/doltgrust\tmain\tHEAD\tcrates/tests/a.rs\t{blob}\n",
            "test/doltgrust/tests/a.rs": content, "../doltgrust/crates/tests/a.rs": COPIED}


CASES["Doltgrust's copied tests as pinned"] = (["doltgres"], copied(), "!test/doltgrust")
CASES["Doltgrust's copied tests changed from their pin"] = (
    ["doltgres"], copied(content=COPIED.replace("test.", "test, edited.")),
    "test/doltgrust/tests/a.rs isn't the object pins.tsv records for it")
CASES["Doltgrust's copied tests changed with their pin"] = (
    ["doltgres"], copied(content=COPIED.replace("test.", "test, edited."), pinned=COPIED.replace("test.", "test, edited.")),
    "test/doltgrust/pins.tsv's row for tests/a.rs isn't Doltgrust's crates/tests/a.rs at HEAD")
CASES["an unpinned file in Doltgrust's copied tests"] = (
    ["doltgres"], {**copied(), "test/doltgrust/tests/b.rs": COPIED},
    "test/doltgrust/tests/b.rs is neither pinned nor one of the fork's own files")
CASES["Doltgrust's copied tests with the fork's own harness file"] = (
    ["doltgres"], {**copied(), "test/doltgrust/crates/harness/src/order.rs":
                   "//! The fork's replacement for Doltgrust's crates/harness/src/order.rs at HEAD.\n"},
    ["!test/doltgrust"])
CASES["a replacement in Doltgrust's copied tests that names no file it replaces"] = (
    ["doltgres"], {**copied(), "test/doltgrust/crates/harness/src/order.rs": COPIED},
    "test/doltgrust/crates/harness/src/order.rs doesn't name the file it replaces at the pinned commit")
CASES["Doltgrust's copied tests without their pins"] = (
    ["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                   "test/doltgrust/tests/a.rs": COPIED},
    "test/doltgrust holds no pins.tsv, so its copied files aren't pinned")
CASES["a pin of Doltgrust's copied tests without its six fields"] = (
    ["doltgres"], {**copied(), "test/doltgrust/pins.tsv": "tests/a.rs\tx\n"},
    "test/doltgrust/pins.tsv's row for tests/a.rs doesn't have its six fields")
CASES["an unpinned project's build"] = (
    ["doltgres"], vendored(extra={"third_party/other/meson.build": "project('other', 'c')\n"}),
    "third_party/other/meson.build is neither pinned nor one of the fork's own files")
HEADER = "int zlib_one(void);\n"


def prefixed(names="#pragma redefine_extname zlib_one dg_zlib_one\n#define zlib_two dg_zlib_two\n",
             header='/* zlib\'s header. */\n#include "../names.h"\n#include "../lib.h"\n'):
    """Returns vendored()'s zlib with its header pinned too and built with the prefix dg_ off Windows, where no
    shared library's reference binds to the executable: a names.h and the header of its include folder, each holding
    what's given."""
    blob = hashlib.sha1(b"blob %d\0" % len(HEADER.encode()) + HEADER.encode()).hexdigest()
    files = vendored(build="files('lib.c'), c_args: host_machine.system() == 'windows' ? [] : "
                           "['-include', meson.current_source_dir() / 'names.h'], "
                           "include_directories: include_directories('include')")
    files["third_party/pins.tsv"] += f"zlib/lib.h\thttps://example.com/zlib\tv1\t{'0' * 40}\tlib.h\t{blob}\n"
    files.update({"third_party/zlib/lib.h": HEADER, "third_party/zlib/names.h": names,
                  "third_party/zlib/include/lib.h": header})
    return files


CASES["vendored names with the prefix dg_"] = (["doltgres"], prefixed(), ["!third_party"])
CASES["a names header that does more than prefix"] = (
    ["doltgres"], prefixed(names="#define zlib_one doltgres_zlib_one\n"),
    "third_party/zlib/names.h holds a line that does more than give a name the prefix dg_: #define zlib_one doltgres_")
CASES["a vendored header that includes another file"] = (
    ["doltgres"], prefixed(header='#include "../names.h"\n#include "../patched/lib.h"\n'),
    "third_party/zlib/include/lib.h isn't names.h and then the pinned header of its own name")
CASES["a names header hiding a directive behind a spliced comment"] = (
    ["doltgres"], prefixed(names="/* A comment. *\\\n/ #define malloc(n) dg_other(n)\n/* The end. */\n"
                                 "#define zlib_one dg_zlib_one\n"),
    "third_party/zlib/names.h holds a backslash, a trigraph, or a character that isn't plain text")
CASES["a names header with a trigraph"] = (
    ["doltgres"], prefixed(names="/* A comment. *??/\n/ #define malloc(n) dg_other(n)\n/* The end. */\n"
                                 "#define zlib_one dg_zlib_one\n"),
    "third_party/zlib/names.h holds a backslash, a trigraph, or a character that isn't plain text")
CASES["a vendored header with a spliced line"] = (
    ["doltgres"], prefixed(header='/* zlib\'s header. *\\\n/ #include "../patched.h"\n/* The end. */\n'
                                  '#include "../names.h"\n#include "../lib.h"\n'),
    "third_party/zlib/include/lib.h holds a backslash, a trigraph, or a character that isn't plain text")
CASES["a names header with a comment inside a directive"] = (
    ["doltgres"], prefixed(names="/* The names. */\n#define zlib_free/**/sizeof dg_zlib_freesizeof\n"),
    "third_party/zlib/names.h holds a comment after its first directive")
CASES["a vendored header with a comment after its includes"] = (
    ["doltgres"], prefixed(header='#include "../names.h"\n#include "../lib.h"\n/* The end. */\n'),
    "third_party/zlib/include/lib.h holds a comment after its first directive")
CASES["a vendored header with code of its own"] = (
    ["doltgres"], prefixed(header='#include "../names.h"\n#include "../lib.h"\n#define zlib_one zlib_two\n'),
    "third_party/zlib/include/lib.h isn't names.h and then the pinned header of its own name")


CASES["a ported file with two rows"] = (
    ["trees"], {**CASES["a ported crate with its rows"][1], "doltgrust.tsv": PORTED_ROWS + PORTED_ROWS.split("\n")[0] + "\n"},
    "doltgrust.tsv: trees/src/lib.rs has more than one row")
CASES["a listed call nothing calls"] = (
    ["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                   "pg/calls.list": "# A call.\nx\tvoid\t\tnoraise\tnocatalog\tx()\n",
                   "store/c/lib.c": "/* pg_calls_x() */\nint\ny(void)\n{\n\treturn 0;\n}\n"},
    "calls.list's x isn't called by the C")
CASES["a listed call the C calls"] = (
    ["doltgres"], {**CASES["a listed call nothing calls"][1],
                   "store/c/lib.c": "int\ny(void)\n{\n\treturn pg_calls_x();\n}\n"},
    "!isn't called by the C")
CASES["a listed call the C hands on"] = (
    ["doltgres"], {**CASES["a listed call nothing calls"][1],
                   "store/c/lib.c": "int\ny(void)\n{\n\treturn z(pg_calls_x);\n}\n"},
    "!isn't called by the C")


OUTPUT_NAMED = re.compile(r'def main\(\):\n(?:    """(?:(?!""").)*"""\n)?    for stream in \(sys\.stdout, sys\.stderr\):\n'
                          r'        stream\.reconfigure\(encoding="utf-8", errors="backslashreplace"\)\n', re.S)


def own_tables(work):
    """Returns a copy of the gates' folder for the cases to read their tables from, with the rows that name the real
    workspace's own files taken out: frontend-callees.tsv's and tokens.tsv's. A case's workspace is a scratch one, so a
    case that must be clean would fail whenever a piece adds one."""
    folder = os.path.join(work, "the-gates-own-tables")
    shutil.rmtree(folder, ignore_errors=True)
    shutil.copytree(rust_gates.HERE, folder, ignore=shutil.ignore_patterns("target", "__pycache__"))
    for name in ("frontend-callees.tsv", "tokens.tsv"):
        with open(os.path.join(folder, name), encoding="utf-8") as f:
            kept = [line for line in f if line.startswith("#") or not line.strip()]
        write(os.path.join(folder, name), "".join(kept))
    return folder


def own_failures(work):
    """Checks the gates' own scripts and returns how many checks fail: every file they open as text and every output
    they decode names UTF-8, so a run reads the same text whatever the platform's encoding is (bugs row 29); and every
    script names its output's encoding as it starts, so a finding that quotes a character the platform's code page
    lacks is printed (row 31)."""
    failures = 0
    scripts = sorted(os.path.join(rust_gates.HERE, name) for name in os.listdir(rust_gates.HERE) if name.endswith(".py"))
    for path in scripts:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        bare = len(re.findall("text=" + r"True(?!, encoding=)", text))
        for start in re.finditer(r"(?<![\w.])open\(", text):
            end, depth, quote = start.end(), 1, None
            while depth:
                if quote:
                    end += text[end] == "\\"
                    quote = None if text[end] == quote else quote
                elif text[end] in "\"'":
                    quote = text[end]
                else:
                    depth += (text[end] == "(") - (text[end] == ")")
                end += 1
            call = text[start.end():end - 1]
            bare += "encoding=" not in call and not re.search(r"""["'][rwa]b["']""", call)
        if bare:
            failures += 1
            print(f"{os.path.basename(path)}: {bare} opens or decodes that name no encoding")
        if not OUTPUT_NAMED.search(text):
            failures += 1
            print(f"{os.path.basename(path)}: its output's encoding isn't named as it starts")
    named = ('    for stream in (sys.stdout, sys.stderr):\n'
             '        stream.reconfigure(encoding="utf-8", errors="backslashreplace")\n')
    if OUTPUT_NAMED.search('def main():\n    """Runs."""\n    run()\n\n\ndef other():\n    """Runs."""\n' + named):
        failures += 1
        print("an output's encoding named at the start of another function passes for main's")
    folder = os.path.join(work, "output-in-a-code-page")
    workspace(os.path.join(folder, "ws"), ["doltgres"],
              {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
               "doltgrust.tsv": "\u03a9.rs\tcrates/x.rs\tHEAD\tsame\t-\t-\n"})
    os.makedirs(os.path.join(folder, "doltgrust"), exist_ok=True)
    commit(os.path.join(folder, "doltgrust"))
    done = subprocess.run([sys.executable, os.path.join(rust_gates.HERE, "rust_gates.py"), os.path.join(folder, "ws"),
                           os.path.join(folder, "scratch")], capture_output=True,
                          env=dict(os.environ, PYTHONIOENCODING="cp1252", DOLTGRUST_DIR=os.path.join(folder, "doltgrust")))
    printed = done.stdout.decode("utf-8", "replace")
    if not re.search("\u03a9.rs isn't the same as Doltgrust's", printed) or "the Rust gates fail: " not in printed:
        failures += 1
        print("a finding that quotes a character the output's code page lacks isn't printed")
    return failures


def main():
    """Checks every case, printing each mismatch; a case expects no problems, a problem holding its text, or, when its
    text starts with "!", no problem holding the rest (or several such texts, all of which must hold), and may append
    text to the gates' tables in a copy of them; a case whose name ends "(in a subfolder)" has its workspace in a
    folder of its repository, as src/doltgres is."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    work = os.path.abspath(sys.argv[1])
    failures = 0
    here = rust_gates.HERE = own_tables(work)
    for name, (members, files, expected, *tables) in CASES.items():
        folder = os.path.join(work, name.replace(" ", "-"), *(["repo"] if name.endswith("(in a subfolder)") else []), "ws")
        workspace(folder, members, files)
        if name.endswith("(in a subfolder)"):
            shutil.rmtree(os.path.join(folder, ".git"))
            commit(os.path.dirname(folder))
        doltgrust = os.path.join(work, name.replace(" ", "-"), "doltgrust")
        if not os.path.isdir(doltgrust):
            os.makedirs(doltgrust)
        commit(doltgrust)
        os.environ["DOLTGRUST_DIR"] = doltgrust
        if tables:
            rust_gates.HERE = os.path.join(work, name.replace(" ", "-"), "tables")
            shutil.rmtree(rust_gates.HERE, ignore_errors=True)
            shutil.copytree(here, rust_gates.HERE)
            for path, text in tables[0].items():
                with open(os.path.join(rust_gates.HERE, path), "a", encoding="utf-8") as f:
                    f.write(text)
        found = rust_gates.problems(folder, os.path.join(work, name.replace(" ", "-"), "scratch"))
        rust_gates.HERE = here
        if expected is None:
            failed = bool(found)
        else:
            failed = any(any(text[1:] in p for p in found) if text.startswith("!") else
                         not any(text in p for p in found) for text in ([expected] if isinstance(expected, str)
                                                                          else expected))
        if failed:
            failures += 1
            print(f"{name}: got {found}, expected {expected or 'no problems'}")
    failures += own_failures(work)
    print("Rust gate cases pass" if not failures else f"{failures} Rust gate cases fail")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
