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

PG = ROOT + ("\n/// Exports an entry.\n#[macro_export]\nmacro_rules! export_entries {\n    () => {\n"
             "        #[unsafe(no_mangle)]\n        pub extern \"C\" fn doltgres_one() -> u32 {\n"
             "            unsafe { core::hint::unreachable_unchecked() }\n        }\n    };\n}\n")
PG_DEPENDENCY = '[dependencies]\npg = { path = "../pg" }\n\n'


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
    """Builds a synthetic workspace: its manifest, toolchain pin, clippy.toml holding every door, and the files given."""
    shutil.rmtree(folder, ignore_errors=True)
    write(os.path.join(folder, "Cargo.toml"),
          f'[workspace]\nmembers = {members!r}\nresolver = "3"\n\n[workspace.lints.rust]\nunsafe_code = "forbid"\n\n'
          '[workspace.lints.clippy]\ndisallowed_macros = "deny"\ndisallowed_methods = "deny"\n'
          'disallowed_types = "deny"\n\n[profile.cassert]\ninherits = "release"\ndebug-assertions = true\n'
          .replace("'", '"'))
    write(os.path.join(folder, "rust-toolchain.toml"),
          '[toolchain]\nchannel = "1.99.1"\ncomponents = ["clippy", "rustfmt", "llvm-tools"]\n')
    kinds = {"method": "disallowed-methods", "type": "disallowed-types", "macro": "disallowed-macros"}
    lines = []
    for kind, key in kinds.items():
        entries = [f'    {{ path = "{row[1]}", reason = "a door", allow-invalid = true }},'
                   for row in rust_gates.read_table("doors.tsv") if row[0] == kind]
        lines.append(f"{key} = [\n" + "\n".join(entries) + "\n]\n")
    write(os.path.join(folder, "clippy.toml"), "\n".join(lines))
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


UNSAFE_MANIFEST = manifest("pg").replace("[lints]\nworkspace = true\n", '[lints.clippy]\nundocumented_unsafe_blocks = "deny"\n')
PG_TEST = (LICENSE + "\n//! A synthetic crate.\n\n#[cfg(test)]\nmod tests {{\n{attribute}    #[test]\n"
           "    fn reads_past_the_end() {{\n        let v = [1u8, 2];\n"
           "        // SAFETY: none; the case reads past the end on purpose. {tested}\n"
           "        std::hint::black_box(unsafe {{ *v.as_ptr().add({index}) }});\n    }}\n}}\n")
TESTED = "Tested by tests::reads_past_the_end."
MIRI_IGNORE = '    #[cfg_attr(miri, ignore = "calls C")]\n'
VERUS_MANIFEST = manifest("encoding", '[package.metadata.verus]\nverify = true\n\n[dependencies]\n'
                                      'vstd = "=0.0.0-2026-11-01-0251"\n\n')
VERUS_LIB = (LICENSE + "\n//! A synthetic crate.\n\n#![forbid(unsafe_code)]\n\nuse vstd::prelude::*;\n\nverus! {\n\n"
             "/// One plus one.\nproof fn lemma_one()\n    ensures\n        1 + 1 == 2,\n{\n}\n{extra}\n} // verus!\n")
LEMMA_ONE = "encoding/src/lib.rs\tlemma_one\tproof fn lemma_one() ensures 1 + 1 == 2,\n"
VERUS_TABLES = {"verus-floor.tsv": "encoding\t1\n", "verus-proofs.tsv": LEMMA_ONE}
HOST = next(line[6:] for line in subprocess.run(["rustc", "-vV"], capture_output=True, text=True, encoding="utf-8").stdout.splitlines()
            if line.startswith("host: "))
DOLTGRUST_MANIFEST = '[package]\nname = "trees"\nversion = "0.0.0"\nedition = "2024"\n'
PORTED_ROWS = ("trees/src/lib.rs\tcrates/trees/src/lib.rs\tHEAD\tsame\t-\t-\n"
               "trees/Cargo.toml\tcrates/trees/Cargo.toml\tHEAD\tchanged\t"
               + rust_gates.ported_diff(DOLTGRUST_MANIFEST.encode(), manifest("trees").encode())
               + "\tCargo.toml inherits the workspace's lints\n")
TOKEN = ROOT + "\n/// A token only this crate makes.\npub struct Token(());\n"
FORGED = "tuple struct constructor `Token` is private"


def fail_cases(cases, expected="forged\t" + FORGED + "\n"):
    """Returns doltgres with a token and a cases workspace in doltgres/fail holding the cases given, by name and
    source, each depending on doltgres, with an expected.tsv."""
    files = {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": TOKEN,
             "doltgres/fail/Cargo.toml": f'[workspace]\nmembers = {sorted(cases)!r}\nresolver = "3"\n'.replace("'", '"'),
             "doltgres/fail/expected.tsv": "# Copyright 2026 Dolthub, Inc.\n\n" + expected}
    for case, source in cases.items():
        files[f"doltgres/fail/{case}/Cargo.toml"] = (f'[package]\nname = "{case}"\nversion = "0.0.0"\nedition = "2024"\n'
                                                     'publish = false\n\n[dependencies]\ndoltgres = { path = "../.." }\n')
        files[f"doltgres/fail/{case}/src/lib.rs"] = source
    return files


FORGE = "//! A case.\n\n/// Forges a token.\npub fn forge() -> doltgres::Token {\n    doltgres::Token(())\n}\n"
FE_WORKSPACE = ('[workspace]\nmembers = ["fehost"]\nresolver = "3"\n\n[workspace.lints.rust]\nunsafe_code = "forbid"\n'
                'unexpected_cfgs = { level = "warn", check-cfg = ["cfg(doltgres_frontend)"] }\n\n[workspace.lints.clippy]\n'
                'disallowed_macros = "deny"\ndisallowed_methods = "deny"\ndisallowed_types = "deny"\n\n[profile.cassert]\n'
                'inherits = "release"\ndebug-assertions = true\n')
FE_PG_MANIFEST = UNSAFE_MANIFEST + '\n[lints.rust]\nunexpected_cfgs = { level = "warn", check-cfg = ["cfg(doltgres_frontend)"] }\n'
FE_PG = (LICENSE + "\n//! A synthetic crate.\n\n/// The frontend wrapper of `size`.\n#[cfg(doltgres_frontend)]\npub fn size() -> u32 {\n"
         "    1\n}\n\n/// A backend item.\n#[cfg(not(doltgres_frontend))]\npub fn backend() -> u32 {\n    2\n}\n\n"
         "#[cfg(test)]\nmod tests {\n    #[cfg(doltgres_frontend)]\n    #[test]\n    fn a_file_is_sized() {\n"
         "        {check}\n    }\n}\n")
FE_OPENS = 'assert!(include_str!("../frontend-calls.list").contains("result = 1;"));'
FE_LIB = ROOT + "\n/// Sizes through the frontend wrapper.\npub fn sizes() -> u32 {\n    pg::size()\n}\n"
FE_CALLS = "size\tint64\tvoid\terrno\t{body}\ttests::a_file_is_sized\n"
FE_TABLES = {"frontend.list": "fe\n"}


def frontend(lib=FE_LIB, check=FE_OPENS, body="result = 1;", extra=None):
    """Returns a case's files for a root workspace of pg and a second workspace `fe` of fehost under the frontend's
    cfg, as frontend.list lists it, with the parts a case changes."""
    files = {"pg/Cargo.toml": FE_PG_MANIFEST, "pg/src/lib.rs": FE_PG.replace("{check}", check),
             "pg/frontend-calls.list": FE_CALLS.format(body=body), "pg/entries.list": "# none\n",
             "fe/Cargo.toml": FE_WORKSPACE,
             "fe/.cargo/config.toml": '[build]\nrustflags = ["--cfg", "doltgres_frontend"]\n',
             "fe/fehost/Cargo.toml": manifest("fehost", '[dependencies]\npg = { path = "../../pg" }\n\n'),
             "fe/fehost/src/lib.rs": lib}
    return {**files, **(extra or {})}


CASES = {
    "clean": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT}, None),
    "no forbid": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                 "doltgres/src/lib.rs": ROOT.replace("#![forbid(unsafe_code)]\n\n", "")},
                  "doesn't forbid unsafe code"),
    "no workspace lints": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres").replace(
        "[lints]\nworkspace = true\n", ""), "doltgres/src/lib.rs": ROOT}, "doesn't take the workspace lints"),
    "unsafe through a macro": (
        ["doltgres", "helper"],
        {"helper/Cargo.toml": manifest("helper"),
         "helper/src/lib.rs": LICENSE + "\n//! A synthetic crate.\n\n#![forbid(unsafe_code)]\n\n"
                              "/// Expands to an unsafe block.\n#[macro_export]\nmacro_rules! sneak {\n"
                              "    () => {\n        unsafe { core::hint::unreachable_unchecked() }\n    };\n}\n",
         "doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\nhelper = { path = "../helper" }\n\n'),
         "doltgres/src/lib.rs": ROOT + "\n/// Never returns.\npub fn never() -> ! {\n    helper::sneak!()\n}\n"},
        "doltgres's expanded source holds unsafe code"),
    "a dropped door": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                                      "clippy.toml": "disallowed-methods = []\n"}, "doesn't refuse the door std::fs::read"),
    "an unlisted exemption": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                             "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods)]\n"
                                                                    "/// Returns two.\npub fn two() -> u32 {\n    2\n}\n"},
                              "isn't a listed exemption"),
    "an unstable feature": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                           "doltgres/src/lib.rs": ROOT.replace(
                                               "#![forbid(unsafe_code)]", "#![forbid(unsafe_code)]\n#![feature(test)]")},
                            "the unstable feature test"),
    "an unlisted crate": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres",
                                                                         '[dependencies]\noutside = { path = "../../outside" }\n\n'),
                                         "doltgres/src/lib.rs": ROOT,
                                         "../outside/Cargo.toml": '[package]\nname = "outside"\nversion = "0.1.0"\n'
                                                                  'edition = "2024"\n',
                                         "../outside/src/lib.rs": "pub fn f() {}\n"},
                          "the crate outside 0.1.0 isn't on the allowlist"),
    "unformatted source": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                          "doltgres/src/lib.rs": ROOT.replace("    1\n", "1\n")}, "rustfmt fails"),
    "a crate-level allow": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                           "doltgres/src/lib.rs": ROOT.replace(
                                               "#![forbid(unsafe_code)]",
                                               "#![forbid(unsafe_code)]\n#![allow(clippy::disallowed_methods)]")},
                            "doesn't refuse the door std::fs::read"),
    "a group allow": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                     "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::style)]\n"
                                                            "/// Returns two.\npub fn two() -> u32 {\n    2\n}\n"},
                      "isn't a listed exemption"),
    "a member's own clippy.toml": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                  "doltgres/src/lib.rs": ROOT,
                                                  "doltgres/clippy.toml": "disallowed-methods = []\n"},
                                   "doesn't refuse the door std::fs::read"),
    "a cargo config's flags": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                              "doltgres/src/lib.rs": ROOT,
                                              ".cargo/config.toml": '[build]\nrustflags = ["-A", '
                                                                    '"clippy::disallowed_methods"]\n'},
                               "doesn't refuse the door std::fs::read"),
    "a commented-out door": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                                            "clippy.toml": "# std::fs::read\ndisallowed-methods = []\n"},
                             "doesn't refuse the door std::fs::read"),
    "a build script": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                                      "doltgres/build.rs": "fn main() {}\n"}, "has a build script"),
    "unsafe in a doc comment only": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                    "doltgres/src/lib.rs": ROOT.replace("/// Returns one.",
                                                                                        "/// Returns one, never unsafe.")},
                                     None),
    "a door clippy is told to ignore": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                      "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods)]\n"
                                                                             "/// Reads a file.\npub fn sneak() -> bool {\n"
                                                                             "    std::fs::read(\"x\").is_ok()\n}\n"},
                                        "doltgres references"),
    "remove_file under an allow": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                       "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods, clippy::disallowed_types)]\n"
                                                              "/// Touches a file.\npub fn sneak() -> bool {\n"
                                                              "    std::fs::remove_file(\"x\").is_ok()\n}\n"},
                         "doltgres references"),
    "Path.exists under an allow": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                       "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods, clippy::disallowed_types)]\n"
                                                              "/// Touches a file.\npub fn sneak() -> bool {\n"
                                                              "    std::path::Path::new(\"x\").exists()\n}\n"},
                         "doltgres references"),
    "OpenOptions.open under an allow": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                       "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods, clippy::disallowed_types)]\n"
                                                              "/// Touches a file.\npub fn sneak() -> bool {\n"
                                                              "    std::fs::OpenOptions::new().read(true).open(\"x\").is_ok()\n}\n"},
                         "doltgres references"),
    "a dependency's door": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres",
                                                                           '[dependencies]\noutside = { path = "../../outside" }\n\n'),
                                           "doltgres/src/lib.rs": ROOT,
                                           "../outside/Cargo.toml": '[package]\nname = "outside"\nversion = "0.1.0"\n'
                                                                    'edition = "2024"\n',
                                           "../outside/src/lib.rs": "pub fn f() -> bool {\n    std::fs::remove_file(\"x\").is_ok()\n}\n"},
                            "outside references"),
    "a pending check": (["doltgres", "store"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
                                                "store/Cargo.toml": manifest("store"), "store/src/lib.rs": ROOT},
                        "which isn't live yet",
                        {"pending.tsv": "a case's check that waits for the store crate\tcrate:store\ta case's piece\n"}),
    "unsafe after a quote character": (
        ["doltgres", "helper"],
        {"helper/Cargo.toml": manifest("helper"),
         "helper/src/lib.rs": LICENSE + "\n//! A synthetic crate.\n\n#![forbid(unsafe_code)]\n\n"
                              "/// Expands to a quote character, then an unsafe block.\n#[macro_export]\n"
                              "macro_rules! sneak {\n    () => {{\n        let _q = '\"';\n"
                              "        unsafe { core::hint::unreachable_unchecked() }\n    }};\n}\n",
         "doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\nhelper = { path = "../helper" }\n\n'),
         "doltgres/src/lib.rs": ROOT + "\n/// Never returns.\npub fn never() -> ! {\n    helper::sneak!()\n}\n"
                                "\n/// Returns a.\npub fn a() -> &'static str {\n    \"a\"\n}\n"},
        "doltgres's lib doltgres's expanded source holds unsafe code"),
    "set_current_dir under an allow": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                           "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods, clippy::disallowed_types)]\n"
                                                                  "/// Moves.\npub fn sneak() -> bool {\n"
                                                                  "    std::env::set_current_dir(\"x\").is_ok()\n}\n"},
                                       "doltgres references"),
    "a Condvar wait under an allow": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                          "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods, clippy::disallowed_types)]\n"
                                                                 "/// Waits.\npub fn sneak(m: &std::sync::Mutex<bool>, c: &std::sync::Condvar) "
                                                                 "-> bool {\n    c.wait(m.lock().unwrap()).is_ok()\n}\n"},
                                      "doltgres references"),
    "an mpsc wait under an allow": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                        "doltgres/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods, clippy::disallowed_types)]\n"
                                                               "/// Waits.\npub fn sneak(r: &std::sync::mpsc::Receiver<u8>) -> bool {\n"
                                                               "    r.recv().is_ok()\n}\n"},
                                    "doltgres references"),
    "a dependency's bundled C": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres",
                                                                                '[dependencies]\nsneak = { path = "../../sneak" }\n\n'),
                                                "doltgres/src/lib.rs": ROOT,
                                                "../sneak/Cargo.toml": '[package]\nname = "sneak"\nversion = "0.1.0"\n'
                                                                       'edition = "2024"\n\n[build-dependencies]\ncc = "1"\n',
                                                "../sneak/build.rs": 'fn main() {\n    cc::Build::new().file("sneak.c").compile("sneak");\n}\n',
                                                "../sneak/sneak.c": ("#ifdef _WIN32\n#include <io.h>\n#define unlink _unlink\n#else\n#include <unistd.h>\n#endif\n"
                                                                     "int sneak(void) { return unlink(\"x\"); }\n"),
                                                "../sneak/src/lib.rs": "pub fn f() {}\n"},
                                 "sneak references"),
    "a listed macro's unsafe": (["pg", "doltgres"], {"pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": PG,
                                                     "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY),
                                                     "doltgres/src/lib.rs": ROOT + "\npg::export_entries!();\n"},
                                "!doltgres's lib"),
    "an unlisted macro's unsafe": (["pg", "doltgres"], {"pg/Cargo.toml": manifest("pg"),
                                                        "pg/src/lib.rs": PG.replace("export_entries", "sneak"),
                                                        "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY),
                                                        "doltgres/src/lib.rs": ROOT + "\npg::sneak!();\n"},
                                   "doltgres's lib doltgres's expanded source holds unsafe code"),
    "unsafe beside a listed macro": (["pg", "doltgres"], {"pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": PG,
                                                          "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY),
                                                          "doltgres/src/lib.rs": ROOT + "\npg::export_entries!();\n"
                                                                                 "\n/// Returns two.\n#[unsafe(no_mangle)]\n"
                                                                                 "pub extern \"C\" fn two() -> u32 {\n    2\n}\n"},
                                     "doltgres's lib doltgres's expanded source holds unsafe code"),
    "shmem before pg": (["shmem"], {"shmem/Cargo.toml": manifest("shmem"), "shmem/src/lib.rs": ROOT},
                        "shmem doesn't deny clippy's undocumented_unsafe_blocks"),
    "pg alone, no storage checks": (["pg"], {"pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": ROOT},
                                    "!doltgrust.tsv's diff"),
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
    "a capability in a static": (["pg", "doltgres"], {"pg/Cargo.toml": manifest("pg"),
                                                      "pg/src/lib.rs": ROOT + "\n/// A capability.\npub struct MayRaise(());\n",
                                                      "pg/entries.list": "# No entries.\n",
                                                      "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY),
                                                      "doltgres/src/lib.rs": ROOT + "\n/// Holds a capability.\n"
                                                                             "pub static HELD: Option<&pg::MayRaise> = None;\n"},
                                 "the typed lint: a static holds a capability"),
    "undefined behavior in pg": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST,
                                          "pg/src/lib.rs": PG_TEST.format(attribute="", tested=TESTED, index=2)}, "Miri fails"),
    "an unlisted Miri opt-out": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST,
                                          "pg/src/lib.rs": PG_TEST.format(attribute=MIRI_IGNORE, tested=TESTED, index=2)},
                                 "the test reads_past_the_end opts out of Miri, which miri-ignored.tsv doesn't list"),
    "a listed Miri opt-out": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST,
                                       "pg/src/lib.rs": PG_TEST.format(attribute=MIRI_IGNORE, tested=TESTED, index=2)},
                              ["!opts out of Miri", "!Miri fails"],
                              {"miri-ignored.tsv": "pg/src/lib.rs\treads_past_the_end\tcalls C\n"}),
    "a test hidden from Miri": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST,
                                         "pg/src/lib.rs": PG_TEST.format(attribute="    #[cfg(not(miri))]\n", tested=TESTED, index=2)},
                                "pg/src/lib.rs mentions miri outside a test's opt-out"),
    "an untested contract": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST,
                                      "pg/src/lib.rs": PG_TEST.format(attribute="", tested="", index=1)},
                             "pg/src/lib.rs: a SAFETY comment names no test"),
    "a contract naming a missing test": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST, "pg/src/lib.rs": PG_TEST.format(
        attribute="", tested="Tested by tests::nothing.", index=1)}, "tests::nothing names no test of pg"),
    "a contract naming its tests": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST, "pg/src/lib.rs": PG_TEST.format(
        attribute="", tested="Tested by tests::reads_past_the_end; test/t/001_read.pl: reads the array's inside.",
        index=1), "test/t/001_read.pl": "ok(1, 'reads the array\\'s inside');\n"}, ["!names no test", "!Miri fails"]),
    "a plain ignore": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST,
                                "pg/src/lib.rs": PG_TEST.format(attribute="    #[ignore]\n", tested=TESTED, index=1)},
                       "pg/src/lib.rs ignores a test outside miri-ignored.tsv's opt-outs"),
    "undocumented unsafe allowed": (["pg"], {"pg/Cargo.toml": manifest("pg").replace("[lints]\nworkspace = true\n", ""),
                                             "pg/src/lib.rs": PG_TEST.format(attribute="", tested=TESTED, index=1)},
                                    "pg doesn't deny clippy's undocumented_unsafe_blocks"),
    "a vet policy": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT},
                     "cargo-vet's config.toml holds a policy",
                     {"supply-chain/config.toml": '\n[policy.doltgres]\ncriteria = "safe-to-run"\n'}),
    "a member under verify": (["doltgres", "verify/tool"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                            "doltgres/src/lib.rs": ROOT,
                                                            "verify/tool/Cargo.toml": manifest("tool"),
                                                            "verify/tool/src/lib.rs": ROOT},
                              "tool is a member under verify"),
    "a label inside another": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST, "pg/src/lib.rs": PG_TEST.format(
        attribute="", tested="Tested by test/t/001_read.pl: reads the array.", index=1),
        "test/t/001_read.pl": "ok(1, 'reads the array\\'s inside');\n"}, "001_read.pl: reads the array names no test"),
    "a build script's missing output": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST, "pg/build.rs": "fn main() {}\n",
                                                 "pg/src/lib.rs": PG_TEST.format(attribute="", tested=TESTED, index=1)},
                                        "the Rust pg's build script generates wasn't found"),
    "a problem behind a feature": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres", "[features]\nextra = []\n\n"),
                                                  "doltgres/src/lib.rs": ROOT + '\n/// Behind a feature.\n#[cfg(feature = "extra")]\n'
                                                                         "pub fn extra() {\n    let unused = 1;\n}\n"},
                                   "(with every feature)"),
    "a listed macro behind a feature": (["pg", "doltgres"], {"pg/Cargo.toml": manifest("pg", "[features]\nextra = []\n\n"),
                                                             "pg/src/lib.rs": PG,
                                                             "doltgres/Cargo.toml": manifest(
                                                                 "doltgres", PG_DEPENDENCY + '[features]\nextra = ["pg/extra"]\n\n'),
                                                             "doltgres/src/lib.rs": ROOT + "\npg::export_entries!();\n"},
                                        "!doltgres's lib"),
    "shmem alone, no storage checks": (["shmem"], {"shmem/Cargo.toml": manifest("shmem"), "shmem/src/lib.rs": ROOT},
                                       "!doltgrust.tsv's diff"),
    "a storage crate reaching pg": (["pg", "host", "trees"], {"pg/Cargo.toml": UNSAFE_MANIFEST, "pg/src/lib.rs": ROOT,
                                                              "host/Cargo.toml": manifest("host", PG_DEPENDENCY),
                                                              "host/src/lib.rs": ROOT,
                                                              "trees/Cargo.toml": manifest(
                                                                  "trees", '[dependencies]\nhost = { path = "../host" }\n\n'),
                                                              "trees/src/lib.rs": ROOT},
                                    "trees reaches pg, which only host's implementation may use"),
    "a clock read outside host": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                 "doltgres/src/lib.rs": ROOT + "\n/// Reads the clock.\npub fn now() -> u64 {\n"
                                                                        "    std::time::Instant::now().elapsed().as_secs()\n}\n"},
                                  "clippy fails"),
    "an elapsed time outside host": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                    "doltgres/src/lib.rs": ROOT + "\n/// Measures from an instant.\npub fn since("
                                                                           "t: std::time::Instant) -> u64 {\n"
                                                                           "    t.elapsed().as_secs()\n}\n"},
                                     "clippy fails"),
    "a Verus crate within its floor": (["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST,
                                                      "encoding/src/lib.rs": VERUS_LIB.replace("{extra}", "")},
                                       ["!Verus fails", "!verus-proofs.tsv", "!trusted.tsv", "!below its floor"],
                                       VERUS_TABLES),
    "an unlisted proof": (["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST,
                                         "encoding/src/lib.rs": VERUS_LIB.replace("{extra}", "")},
                          "encoding/src/lib.rs: lemma_one (proof fn lemma_one() ensures 1 + 1 == 2,) isn't in verus-proofs.tsv",
                          {"verus-floor.tsv": "encoding\t1\n"}),
    "an unlisted trusted statement": (["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST, "encoding/src/lib.rs": VERUS_LIB.replace(
        "{extra}", "\n/// Assumes.\nproof fn lemma_two()\n    ensures\n        false,\n{\n    assume(false);\n}\n")},
                                      "the trusted statement assume(false); isn't in trusted.tsv",
                                      {**VERUS_TABLES, "verus-proofs.tsv": LEMMA_ONE + "encoding/src/lib.rs\tlemma_two\t"
                                                                           "proof fn lemma_two() ensures false,\n"}),
    "a Verus crate below its floor": (["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST,
                                                     "encoding/src/lib.rs": VERUS_LIB.replace("{extra}", "")},
                                      "Verus verifies 1 in encoding, below its floor of 9",
                                      {**VERUS_TABLES, "verus-floor.tsv": "encoding\t9\n"}),
    "a floor without Verus metadata": (["encoding"], {"encoding/Cargo.toml": manifest("encoding"), "encoding/src/lib.rs": ROOT},
                                       "encoding has a floor in verus-floor.tsv but no Verus metadata",
                                       {"verus-floor.tsv": "encoding\t1\n"}),
    "a second copy of a trusted statement": (["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST, "encoding/src/lib.rs": VERUS_LIB.replace(
        "{extra}", "\n/// Assumes.\nproof fn lemma_two()\n    ensures\n        false,\n{\n    assume(false);\n    assume(false);\n}\n")},
                                             "the trusted statement assume(false); isn't in trusted.tsv as many times",
                                             {**VERUS_TABLES, "trusted.tsv": "encoding/src/lib.rs\tassume(false);\n",
                                              "verus-proofs.tsv": LEMMA_ONE + "encoding/src/lib.rs\tlemma_two\t"
                                                                  "proof fn lemma_two() ensures false,\n"}),
    "a dropped ensures": (["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST, "encoding/src/lib.rs": VERUS_LIB.replace(
        "{extra}", "\n/// Returns its argument.\nfn same(x: u8) -> u8 {\n    x\n}\n")},
                          "verus-proofs.tsv's same (fn same(x: u8) -> (r: u8) ensures r == x,) is gone or changed",
                          {**VERUS_TABLES, "verus-proofs.tsv": LEMMA_ONE + "encoding/src/lib.rs\tsame\t"
                                                                    "fn same(x: u8) -> (r: u8) ensures r == x,\n"}),
    "a trusted attribute outside verus!": (["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST,
                                                          "encoding/src/lib.rs": VERUS_LIB.replace("{extra}", "").replace(
                                                              "verus! {", "verus!{"),
                                                          "encoding/src/other.rs": "#[verifier::external_body]\npub fn other() {}\n"},
                                           "the trusted statement #[verifier::external_body] / pub fn other() {} isn't in trusted.tsv",
                                           VERUS_TABLES),
    "another build's generated Rust": (["pg"], {"pg/Cargo.toml": UNSAFE_MANIFEST,
                                                 "pg/build.rs": 'fn main() {\n    let out = std::env::var("OUT_DIR").unwrap();\n'
                                                                '    std::fs::write(format!("{out}/calls.rs"), "// SAFETY: fine. '
                                                                'Tested by tests::reads_past_the_end.\\n").unwrap();\n}\n',
                                                 "pg/src/lib.rs": PG_TEST.format(attribute="", tested=TESTED, index=1),
                                                 f"../scratch/target/{HOST}/cassert/build/pg-stale/out/calls.rs":
                                                     "// SAFETY: stale. Tested by tests::nothing.\n"},
                                        ["!tests::nothing", "!build script generates wasn't found"]),
    "a Verus crate checked again (twice)": (["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST,
                                                           "encoding/src/lib.rs": VERUS_LIB.replace("{extra}", "")},
                                            ["!Verus fails", "!below its floor"], VERUS_TABLES),
    "a derived Clone of a Copy type": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                      "doltgres/src/lib.rs": ROOT + "\n/// A kind.\n#[derive(Clone, Copy)]\n"
                                                                             "pub enum Kind {\n    /// One.\n    One,\n}\n"},
                                       "!expanded source holds unsafe code"),
    "an unsafe impl beside a derived Clone": (["doltgres", "helper"], {
        "helper/Cargo.toml": manifest("helper"),
        "helper/src/lib.rs": LICENSE + "\n//! A synthetic crate.\n\n#![forbid(unsafe_code)]\n\n/// Writes an unsafe impl.\n"
                             "#[macro_export]\nmacro_rules! sneak {\n    ($t:ty) => {\n        unsafe impl Send for $t {}\n    };\n}\n",
        "doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\nhelper = { path = "../helper" }\n\n'),
        "doltgres/src/lib.rs": ROOT + "\n/// A kind.\n#[derive(Clone, Copy)]\npub struct Kind(*const u8);\n\nhelper::sneak!(Kind);\n"},
        "doltgres's lib doltgres's expanded source holds unsafe code"),
    "a debug_assert in a ported crate": (["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT.replace(
        "    1\n", "    debug_assert!(true);\n    1\n")}, "trees/src/lib.rs uses the debug_assert family"),
    "an unlisted cfg(debug_assertions)": (["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT.replace(
        "    1\n", "    if cfg!(debug_assertions) { 1 } else { 1 }\n")},
                                          "trees/src/lib.rs has 1 cfg(debug_assertions), more than debug-assertions.tsv's 0"),
    "a listed cfg(debug_assertions)": (["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT.replace(
        "    1\n", "    if cfg!(debug_assertions) { 1 } else { 1 }\n")}, "!more than debug-assertions.tsv",
                                       {"debug-assertions.tsv": "trees/src/lib.rs\t1\n"}),
    "a ported crate without rows": (["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT},
                                    "trees/Cargo.toml isn't in doltgrust.tsv"),
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
    "a Doltgrust file left out": (["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT,
                                              "doltgrust.tsv": PORTED_ROWS,
                                              "../doltgrust/crates/trees/Cargo.toml": DOLTGRUST_MANIFEST,
                                              "../doltgrust/crates/trees/src/lib.rs": ROOT,
                                              "../doltgrust/crates/trees/src/sim.rs": "pub fn sim() {}\n"},
                                  "Doltgrust's crates/trees/src/sim.rs at HEAD isn't in doltgrust.tsv"),
    "a cfg(not(debug_assertions))": (["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT + (
        "\n/// Only without assertions.\n#[cfg(any(not(debug_assertions), test))]\npub fn quiet() {}\n")},
                                     "trees/src/lib.rs has 1 cfg(debug_assertions), more than debug-assertions.tsv's 0"),
    "a member postgres doesn't link": (["doltgres", "sim"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                             "doltgres/src/lib.rs": ROOT,
                                                             "sim/Cargo.toml": manifest("sim"),
                                                             "sim/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods, "
                                                                                "clippy::disallowed_types)]\n/// Removes a file.\n"
                                                                                "pub fn sneak() -> bool {\n"
                                                                                "    std::fs::remove_file(\"x\").is_ok()\n}\n"},
                                       ["!sim references", "!compiled library wasn't found"]),
    "a member doltgres links": (["doltgres", "trees"], {"doltgres/Cargo.toml": manifest("doltgres",
                                                                                      '[dependencies]\ntrees = { path = "../trees" }\n\n'),
                                                        "doltgres/src/lib.rs": ROOT,
                                                        "trees/Cargo.toml": manifest("trees"),
                                                        "trees/src/lib.rs": ROOT + "\n#[allow(clippy::disallowed_methods, "
                                                                               "clippy::disallowed_types)]\n/// Removes a file.\n"
                                                                               "pub fn sneak() -> bool {\n"
                                                                               "    std::fs::remove_file(\"x\").is_ok()\n}\n"},
                                "trees references"),
    "a listed attribute over several lines": (["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"),
                                                             "doltgres/src/lib.rs": ROOT + "\n#[allow(\n    clippy::disallowed_methods\n)]\n"
                                                                                    "/// Returns two.\npub fn two() -> u32 {\n    2\n}\n"},
                                              "!isn't a listed exemption",
                                              {"lint-exempt-allowed.tsv": "doltgres/src/lib.rs\t#[allow( clippy::disallowed_methods )]\t"
                                                                          "pub fn two() -> u32\ttest\n"}),
    "doltgres linking sim": (["doltgres", "sim"], {"doltgres/Cargo.toml": manifest("doltgres",
                                                                                 '[dependencies]\nsim = { path = "../sim" }\n\n'),
                                                   "doltgres/src/lib.rs": ROOT, "sim/Cargo.toml": manifest("sim"),
                                                   "sim/src/lib.rs": ROOT},
                             "the doltgres library links sim, which only tests may use"),
    "host's testing feature in the server": (["doltgres", "host"], {
        "doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\nhost = { path = "../host", features = ["testing"] }\n\n'),
        "doltgres/src/lib.rs": ROOT, "host/Cargo.toml": manifest("host", "[features]\ntesting = []\n\n"),
        "host/src/lib.rs": ROOT}, "host's testing feature reaches the server's default build"),
    "host's testing feature in tests only": (["doltgres", "host"], {
        "doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\nhost = { path = "../host" }\n\n'
                                                    '[dev-dependencies]\nhost = { path = "../host", features = ["testing"] }\n\n'),
        "doltgres/src/lib.rs": ROOT, "host/Cargo.toml": manifest("host", "[features]\ntesting = []\n\n"),
        "host/src/lib.rs": ROOT}, "!testing feature"),
    "host's testing feature in the server (with a space)": (["doltgres", "host"], {
        "doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\nhost = { path = "../host", features = ["testing"] }\n\n'),
        "doltgres/src/lib.rs": ROOT, "host/Cargo.toml": manifest("host", "[features]\ntesting = []\n\n"),
        "host/src/lib.rs": ROOT}, ["host's testing feature reaches the server's default build",
                                   "host's testing feature reaches the server's build with every feature"]),
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
    "a compile-fail case failing as expected": (["doltgres"], fail_cases({"forged": FORGE}), None),
    "a compile-fail case that compiles": (["doltgres"], fail_cases({"forged": "//! A case.\n"}),
                                          "doltgres/fail/forged compiles, and a compile-fail case must not"),
    "a compile-fail case failing for another reason": (
        ["doltgres"], fail_cases({"forged": FORGE.replace("doltgres::Token(())", "doltgres::Tokn(())")}),
        ["fails without 'tuple struct constructor", "fails with an unexpected error"]),
    "a case folder outside its cases workspace": (
        ["doltgres"], {**fail_cases({"forged": FORGE}), "doltgres/fail/stray/src/lib.rs": FORGE},
        "doltgres/fail/stray isn't a member of its cases workspace"),
    "a compile-fail case without its expected error": (["doltgres"], fail_cases({"forged": FORGE}, ""),
                                                       "doltgres/fail/forged has no expected error"),
    "a cases workspace locking a crate the workspace doesn't": (
        ["doltgres"], {**fail_cases({"forged": FORGE}), "doltgres/fail/forged/Cargo.toml": (
            '[package]\nname = "forged"\nversion = "0.0.0"\nedition = "2024"\npublish = false\n\n[dependencies]\n'
            'doltgres = { path = "../.." }\nvstd = "=0.0.0-2026-11-01-0251"\n')},
        "doltgres/fail locks vstd 0.0.0-2026-11-01-0251, which the workspace's Cargo.lock doesn't lock alike"),
    "a lint table changed between runs (moved)": (
        ["doltgres", "trees"],
        {"doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\ntrees = { path = "../trees" }\n\n'),
         "doltgres/src/lib.rs": ROOT, "pg/entries.list": "# none\n", "trees/Cargo.toml": manifest("trees"),
         "trees/src/lib.rs": ROOT + "\n/// Checks nothing when dropped.\npub struct Quiet;\n\nimpl Drop for Quiet {\n"
                             "    fn drop(&mut self) {\n        let _ = core::str::from_utf8(&[]);\n    }\n}\n"},
        "`std::str::from_utf8` has no body to follow",
        {"lint/non-allocating.list": "std::str::from_utf8\tthe first run's row, which the second run's tables lack\n"}),
    "a lint cache that can't be cleared (locked)": (
        ["doltgres", "trees"],
        {"doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\ntrees = { path = "../trees" }\n\n'),
         "doltgres/src/lib.rs": ROOT, "pg/entries.list": "# none\n", "trees/Cargo.toml": manifest("trees"),
         "trees/src/lib.rs": ROOT},
        "the typed lint's cached build can't be cleared"),
    "a storage crate testing through sim": (["trees", "sim", "pg"], {
        "trees/Cargo.toml": manifest("trees", '[dev-dependencies]\nsim = { path = "../sim" }\n\n'),
        "trees/src/lib.rs": ROOT, "sim/Cargo.toml": manifest("sim", '[dependencies]\npg = { path = "../pg" }\n\n'),
        "sim/src/lib.rs": ROOT, "pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": ROOT},
        ["!trees reaches pg", "!reach shmem through sim"]),
    "sim reaching shmem": (["trees", "sim", "shmem"], {
        "trees/Cargo.toml": manifest("trees", '[dev-dependencies]\nsim = { path = "../sim" }\n\n'),
        "trees/src/lib.rs": ROOT, "sim/Cargo.toml": manifest("sim", '[dependencies]\nshmem = { path = "../shmem" }\n\n'),
        "sim/src/lib.rs": ROOT, "shmem/Cargo.toml": manifest("shmem"), "shmem/src/lib.rs": ROOT},
        "trees's tests reach shmem through sim"),
    "a storage crate linking sim": (["trees", "sim", "pg"], {
        "trees/Cargo.toml": manifest("trees", '[dependencies]\nsim = { path = "../sim" }\n\n'),
        "trees/src/lib.rs": ROOT, "sim/Cargo.toml": manifest("sim", '[dependencies]\npg = { path = "../pg" }\n\n'),
        "sim/src/lib.rs": ROOT, "pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": ROOT},
        "trees reaches pg, which only host's implementation may use"),
    "pg's outside_postgres in the server": (["doltgres", "pg"], {
        "doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\npg = { path = "../pg", features = ["outside_postgres"] }\n\n'),
        "doltgres/src/lib.rs": ROOT, "pg/Cargo.toml": manifest("pg", "[features]\noutside_postgres = []\n\n"),
        "pg/src/lib.rs": ROOT}, "pg's outside_postgres feature reaches the server's default build"),
    "a crate depending on one that tests through sim": (["trees", "chunks", "sim", "pg"], {
        "trees/Cargo.toml": manifest("trees", '[dependencies]\nchunks = { path = "../chunks" }\n\n'),
        "trees/src/lib.rs": ROOT,
        "chunks/Cargo.toml": manifest("chunks", '[dev-dependencies]\nsim = { path = "../sim" }\n\n'),
        "chunks/src/lib.rs": ROOT, "sim/Cargo.toml": manifest("sim", '[dependencies]\npg = { path = "../pg" }\n\n'),
        "sim/src/lib.rs": ROOT, "pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": ROOT},
        ["!trees reaches pg", "!chunks reaches pg"]),
    "a clean frontend workspace": (["pg"], frontend(), None, FE_TABLES),
    "pg's outside_postgres in a frontend library": (["pg"], frontend(extra={
        "pg/Cargo.toml": FE_PG_MANIFEST.replace("[lints.clippy]", "[features]\noutside_postgres = []\n\n[lints.clippy]"),
        "fe/fehost/Cargo.toml": manifest("fehost", '[dependencies]\npg = { path = "../../pg", features = ["outside_postgres"] }\n\n')}),
        "fe: pg's outside_postgres feature reaches fehost's default build", FE_TABLES),
    "pg's outside_postgres in a frontend library's tests": (["pg"], frontend(extra={
        "pg/Cargo.toml": FE_PG_MANIFEST.replace("[lints.clippy]", "[features]\noutside_postgres = []\n\n[lints.clippy]"),
        "fe/fehost/Cargo.toml": manifest("fehost", '[dependencies]\npg = { path = "../../pg" }\n\n[dev-dependencies]\n'
                                                   'pg = { path = "../../pg", features = ["outside_postgres"] }\n\n')}),
        None, FE_TABLES),
    "a frontend workspace frontend.list names but that isn't there": (["pg"], {"pg/Cargo.toml": manifest("pg"),
                                                                             "pg/src/lib.rs": ROOT},
                                                                      "frontend.list names fe, which holds no Cargo "
                                                                      "workspace", FE_TABLES),
    "a backend item used under the frontend's cfg": (["pg"], frontend(lib=ROOT + "\n/// Calls the backend.\npub fn "
                                                                          "calls() -> u32 {\n    pg::backend()\n}\n"),
                                                     "fe: clippy fails", FE_TABLES),
    "a build script that sets the frontend's cfg": (["pg"], frontend(extra={
        "pg/build.rs": LICENSE + '\nfn main() {\n    println!("cargo:rustc-cfg=doltgres_frontend");\n}\n'}),
                                                    "pg/build.rs sets the frontend's cfg", FE_TABLES),
    "a frontend lock that pins another version": (["pg"], frontend(extra={"fe/Cargo.lock": (
        'version = 4\n\n[[package]]\nname = "outside"\nversion = "9.9.9"\nsource = '
        '"registry+https://github.com/rust-lang/crates.io-index"\nchecksum = "00"\n')}),
                                                  "fe/Cargo.lock locks outside 9.9.9, which the root's Cargo.lock "
                                                  "doesn't lock alike", FE_TABLES),
    "a failing frontend unsafe test": (["pg"], frontend(check="assert_eq!(super::size(), 2);"),
                                       "fe: the workspace's tests fails", FE_TABLES),
    "a wrapper calling pg_strdup": (["pg"], frontend(body='result = pg_strdup("x") != NULL;'),
                                    "frontend-calls.list's size calls pg_strdup, which frontend-callees.tsv doesn't "
                                    "list", FE_TABLES),
    "a wrapper another frontend crate calls": (["pg"], frontend(extra={
        "fe/Cargo.toml": FE_WORKSPACE.replace('members = ["fehost"]', 'members = ["fehost", "other"]'),
        "fe/other/Cargo.toml": manifest("other", '[dependencies]\npg = { path = "../../pg" }\n\n'),
        "fe/other/src/lib.rs": FE_LIB}), "other calls pg::size, which only fehost may call", FE_TABLES),
    "a wrapper fehost doesn't call": (["pg"], frontend(lib=ROOT), "frontend-calls.list's size isn't called by fehost",
                                      FE_TABLES),
    "a wrapper whose test passes with its C broken": (["pg"], frontend(check="assert_eq!(super::size(), 1);"),
                                                      "tests::a_file_is_sized passes with the wrapper's C made to fail",
                                                      FE_TABLES),
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
CASES["vendored Rust with an unstable feature"] = (
    ["doltgres"], vendored(extra={"third_party/zlib/bench.rs": "#![feature(test)]\n"}), ["!the unstable feature test"])
COPIED = "// Doltgrust's test.\n#![feature(test)]\n"


def copied(content=COPIED, pinned=COPIED):
    """Returns doltgres with a file of Doltgrust's tests in test/doltgrust, pinned to the blob of pinned, beside
    Doltgrust's checkout holding COPIED at that path."""
    blob = hashlib.sha1(b"blob %d\0" % len(pinned.encode()) + pinned.encode()).hexdigest()
    return {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT,
            "test/doltgrust/pins.tsv": "# path\tupstream\tbranch\tcommit\tupstream path\tobject\n"
                                       f"tests/a.rs\thttps://example.com/doltgrust\tmain\tHEAD\tcrates/tests/a.rs\t{blob}\n",
            "test/doltgrust/tests/a.rs": content, "../doltgrust/crates/tests/a.rs": COPIED}


CASES["Doltgrust's copied tests as pinned"] = (["doltgres"], copied(), ["!test/doltgrust", "!the unstable feature test"])
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
CASES["an unstable feature in a test folder of the fork's own"] = (
    ["doltgres"], {**copied(), "test/other/a.rs": COPIED}, "test/other/a.rs: the unstable feature test")
CASES["an unpinned project's build"] = (
    ["doltgres"], vendored(extra={"third_party/other/meson.build": "project('other', 'c')\n"}),
    "third_party/other/meson.build is neither pinned nor one of the fork's own files")
CASES["a derived PartialEq of an enum with data"] = (
    ["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT
                   + "\n/// A kind.\n#[derive(PartialEq)]\npub enum Kind {\n    /// One.\n    One(u8),\n"
                     "    /// Two.\n    Two(u16),\n}\n"},
    "!expanded source holds unsafe code")
CASES["an unlisted broadcast axiom group"] = (
    ["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST, "encoding/src/lib.rs": VERUS_LIB.replace(
        "{extra}", "broadcast use vstd::std_specs::btree::group_btree_axioms;\n")},
    "the trusted statement broadcast use vstd::std_specs::btree::group_btree_axioms; isn't in trusted.tsv",
    VERUS_TABLES)
CASES["a build script's generated Rust (with a space)"] = (
    ["pg"], {path: text for path, text in CASES["another build's generated Rust"][1].items() if not path.startswith("../")},
    ["!build script generates wasn't found", "!names no test"])
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

CASES["a member silencing its doors behind a dependency's canary"] = (
    ["host", "pghost"],
    {"host/Cargo.toml": manifest("host"), "host/src/lib.rs": ROOT,
     "pghost/Cargo.toml": manifest("pghost", '[dependencies]\nhost = { path = "../host" }\n\n'),
     "pghost/src/lib.rs": ROOT.replace("#![forbid(unsafe_code)]",
                                       "#![forbid(unsafe_code)]\n#![allow(clippy::disallowed_methods)]")},
    "pghost doesn't refuse the door std::fs::read")

LINKED_PG = PG.replace("unsafe { core::hint::unreachable_unchecked() }", "crate::entries::one()")
LINKED_SAME = "\n/// Returns its argument.\npub fn same(x: u8) -> (r: u8)\n    ensures\n        r == x,\n{\n    x\n}\n"
LINKED_TABLES = {"verus-floor.tsv": "encoding\t2\n",
                 "verus-proofs.tsv": LEMMA_ONE + "encoding/src/lib.rs\tsame\tpub fn same(x: u8) -> (r: u8) ensures r == x,\n"}
UNLINKED = ("encoding/src/lib.rs: Verus proves same (pub fn same(x: u8) -> (r: u8) ensures r == x,), which nothing the "
            "server's entries reach calls")


def linked(entry, extra="", features=""):
    """Returns a workspace whose doltgres library exports one entry, with the entry's body given, beside a Verus crate
    holding a function with a proved clause, and more of the library's source and its manifest's features when given."""
    assert PG != LINKED_PG
    return {"pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": LINKED_PG, "pg/entries.list": "# none\n",
            "encoding/Cargo.toml": VERUS_MANIFEST, "encoding/src/lib.rs": VERUS_LIB.replace("{extra}", LINKED_SAME),
            "doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\nencoding = { path = "../encoding" }\n'
                                                        'pg = { path = "../pg" }\n\n' + features),
            "doltgres/src/lib.rs": ROOT + "\n/// The entries.\npub mod entries {\n    /// Returns one.\n"
                                          f"    pub fn one() -> u32 {{\n{entry}    }}\n}}\n{extra}\npg::export_entries!();\n"}


CASES["a proof of a function an entry calls"] = (
    ["pg", "encoding", "doltgres"], linked("        u32::from(encoding::same(1))\n"),
    ["!which nothing the server's entries reach calls", "!Verus fails", "!the typed lint"], LINKED_TABLES)
CASES["a proof of a function nothing calls"] = (
    ["pg", "encoding", "doltgres"], linked("        1\n"), UNLINKED, LINKED_TABLES)
CASES["a proof of a function only a test calls"] = (
    ["pg", "encoding", "doltgres"],
    linked("        1\n", "\n#[cfg(test)]\nmod tests {\n    #[test]\n    fn same_is_same() {\n"
                          "        assert_eq!(encoding::same(1), 1);\n    }\n}\n"),
    UNLINKED, LINKED_TABLES)
CASES["a proof of a function reached through a closure"] = (
    ["pg", "encoding", "doltgres"],
    linked("        [1u8].iter().map(|x| u32::from(encoding::same(*x))).sum()\n"),
    ["!which nothing the server's entries reach calls", "!the typed lint"], LINKED_TABLES)
CASES["a proof of a function reached as a function value"] = (
    ["pg", "encoding", "doltgres"],
    linked("        let same: fn(u8) -> u8 = encoding::same;\n        u32::from(same(1))\n"),
    ["!which nothing the server's entries reach calls", "!the typed lint"], LINKED_TABLES)
CASES["a proof of a function only a feature's code calls"] = (
    ["pg", "encoding", "doltgres"],
    linked("        if cfg!(feature = \"extra\") { extra() } else { 1 }\n",
           "\n/// Returns one, through the proved function.\n#[cfg(feature = \"extra\")]\nfn extra() -> u32 {\n"
           "    u32::from(encoding::same(1))\n}\n\n/// Returns one.\n#[cfg(not(feature = \"extra\"))]\nfn extra() -> u32 {\n"
           "    1\n}\n", "[features]\nextra = []\n\n"),
    UNLINKED, LINKED_TABLES)
CASES["a proof without a server library"] = (
    ["encoding"], {"encoding/Cargo.toml": VERUS_MANIFEST, "encoding/src/lib.rs": VERUS_LIB.replace("{extra}", LINKED_SAME)},
    UNLINKED, LINKED_TABLES)


def linked_feature():
    """Returns linked()'s workspace with the entry calling a function of the Verus crate that calls the proved one only
    under the crate's own feature, which its dev-dependency on itself turns on, as the ported crates' test features
    are; a run that builds tests beside the library turns it on in the library's dependencies too."""
    files = linked("        u32::from(encoding::through(1))\n")
    files["encoding/Cargo.toml"] = VERUS_MANIFEST.replace(
        "[lints]", '[dev-dependencies]\nencoding = { path = ".", features = ["testing"] }\n\n[features]\ntesting = []\n\n[lints]')
    files["encoding/src/lib.rs"] += (
        "\n/// Returns its argument, through the proved function in tests.\npub fn through(x: u8) -> u8 {\n    helper(x)\n}\n"
        "\n/// Returns its argument through the proved function.\n#[cfg(feature = \"testing\")]\nfn helper(x: u8) -> u8 {\n"
        "    same(x)\n}\n\n/// Returns its argument.\n#[cfg(not(feature = \"testing\"))]\nfn helper(x: u8) -> u8 {\n    x\n}\n")
    return files


def linked_line():
    """Returns linked()'s workspace with the proved function sharing its line with another, which alone the entry
    calls."""
    files = linked("        u32::from(encoding::other(1))\n")
    files["encoding/src/lib.rs"] = VERUS_LIB.replace(
        "{extra}", "\n/// Returns its argument, twice over.\npub fn same(x: u8) -> (r: u8) ensures r == x, { x } "
                   "pub fn other(x: u8) -> u8 { x }\n")
    return files


CASES["a proof of a function only its crate's test feature calls"] = (
    ["pg", "encoding", "doltgres"], linked_feature(), UNLINKED, LINKED_TABLES)
CASES["a proof of a function beside a reached one on its line"] = (
    ["pg", "encoding", "doltgres"], linked_line(),
    ["encoding/src/lib.rs: Verus proves same (", "which nothing the server's entries reach calls"],
    {"verus-floor.tsv": "encoding\t2\n", "verus-proofs.tsv": LEMMA_ONE})

UNLINKED_ROW = {"unlinked.tsv": "encoding/src/lib.rs\tsame\tpub fn same(x: u8) -> (r: u8) ensures r == x,\n"}
CASES["a proof of a function nothing calls yet, which the list names"] = (
    ["pg", "encoding", "doltgres"], linked("        1\n"),
    ["!which nothing the server's entries reach calls", "!unlinked.tsv lists", "!the typed lint"],
    {**LINKED_TABLES, **UNLINKED_ROW})
CASES["a proof of a function the list names though an entry calls it"] = (
    ["pg", "encoding", "doltgres"], linked("        u32::from(encoding::same(1))\n"),
    "encoding/src/lib.rs: unlinked.tsv lists same (pub fn same(x: u8) -> (r: u8) ensures r == x,), which the server's "
    "entries reach now", {**LINKED_TABLES, **UNLINKED_ROW})
CASES["a proof of a function the list names that is gone"] = (
    ["doltgres"], {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT},
    "encoding/src/lib.rs: unlinked.tsv lists same (", UNLINKED_ROW)

CASES["a proof of a function the list gains though main already proves it"] = (
    ["pg", "encoding", "doltgres"], linked("        1\n"),
    "encoding/src/lib.rs: unlinked.tsv gains same, which main already proves",
    {**LINKED_TABLES, **UNLINKED_ROW, "base/verus-proofs.tsv": LINKED_TABLES["verus-proofs.tsv"]})
CASES["a proof of a function the list gains though main proves it with another clause"] = (
    ["pg", "encoding", "doltgres"], linked("        1\n"),
    "encoding/src/lib.rs: unlinked.tsv gains same, which main already proves",
    {**LINKED_TABLES, **UNLINKED_ROW,
     "base/verus-proofs.tsv": LEMMA_ONE + "encoding/src/lib.rs\tsame\tpub fn same(x: u8) -> (r: u8) ensures r <= x,\n"})
CASES["a proof of a function the list names when the walk can't run"] = (
    ["pg", "encoding", "doltgres"],
    linked("        1\n", "\n#[cfg(not(debug_assertions))]\ncompile_error!(\"only the release profile fails\");\n"),
    "the walk from the server's entries didn't run", {**LINKED_TABLES, **UNLINKED_ROW})
CASES["a proof of a function the list gains with the crate that brings it"] = (
    ["pg", "encoding", "doltgres"], linked("        1\n"), ["!unlinked.tsv gains", "!which nothing the server's entries"],
    {**LINKED_TABLES, **UNLINKED_ROW, "base/verus-proofs.tsv": LEMMA_ONE})
CASES["a proof of a function an entry calls, walked again where an earlier run's check is kept (twice)"] = (
    ["pg", "encoding", "doltgres"], linked("        u32::from(encoding::same(1))\n"),
    ["!which nothing the server's entries reach calls", "!the walk from the server's entries didn't run",
     "!the typed lint"], LINKED_TABLES)
CAPTURE = ("    let held: Option<&pg::MayRaise> = None;\n    let asks = move || held.is_none();\n")
CAPTURED = {"pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": ROOT + "\n/// A capability.\npub struct MayRaise(());\n",
            "pg/entries.list": "# No entries.\n"}
CASES["a capability captured in a linked member's test"] = (
    ["pg", "doltgres"],
    {**CAPTURED, "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY),
     "doltgres/src/lib.rs": ROOT + "\n#[cfg(test)]\nmod tests {\n    #[test]\n    fn asks() {\n"
                            + CAPTURE.replace("    ", "        ") + "        assert!(asks());\n    }\n}\n"},
    "!the typed lint")
CASES["a capability captured in a linked member's library code"] = (
    ["pg", "doltgres"],
    {**CAPTURED, "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY),
     "doltgres/src/lib.rs": ROOT + "\n/// Returns whether no capability is held.\npub fn asks() -> bool {\n" + CAPTURE
                            + "    asks()\n}\n"},
    "the typed lint: a capability is held in a field, element, argument, capture, or coroutine")
CASES["a capability captured in a linked member and in a member that depends on it"] = (
    ["pg", "catalog", "doltgres"],
    {**CAPTURED, "catalog/Cargo.toml": manifest("catalog", PG_DEPENDENCY),
     "catalog/src/lib.rs": ROOT + "\n/// Returns whether no capability is held.\npub fn asks() -> bool {\n" + CAPTURE
                           + "    asks()\n}\n",
     "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY + 'catalog = { path = "../catalog" }\n'),
     "doltgres/src/lib.rs": ROOT + "\n/// Returns whether no capability is held.\npub fn asks() -> bool {\n" + CAPTURE
                            + "    asks() && catalog::asks()\n}\n"},
    ["coroutine (design item 17) (catalog/src/lib.rs:", "coroutine (design item 17) (doltgres/src/lib.rs:"])
CASES["a capability captured in a linked member's library code behind a feature"] = (
    ["pg", "doltgres"],
    {**CAPTURED, "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY + "[features]\nextra = []\n\n"),
     "doltgres/src/lib.rs": ROOT + "\n/// Returns whether no capability is held.\n#[cfg(feature = \"extra\")]\n"
                            "pub fn asks() -> bool {\n" + CAPTURE + "    asks()\n}\n"},
    ["capture, or coroutine (design item 17) (doltgres/src/lib.rs:", ") (with every feature)"])
CASES["a capability captured in a linked member and in a member it depends on, whose name sorts after its own"] = (
    ["pg", "trees", "doltgres"],
    {**CAPTURED, "trees/Cargo.toml": manifest("trees", PG_DEPENDENCY),
     "trees/src/lib.rs": ROOT + "\n/// Returns whether no capability is held.\npub fn asks() -> bool {\n" + CAPTURE
                         + "    asks()\n}\n",
     "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY + 'trees = { path = "../trees" }\n'),
     "doltgres/src/lib.rs": ROOT + "\n/// Returns whether no capability is held.\npub fn asks() -> bool {\n" + CAPTURE
                            + "    asks() && trees::asks()\n}\n"},
    ["coroutine (design item 17) (doltgres/src/lib.rs:", "coroutine (design item 17) (trees/src/lib.rs:"])
CASES["a linked member whose library has another name"] = (
    ["doltgres", "trees"],
    {"doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\ntrees = { path = "../trees" }\n\n'),
     "doltgres/src/lib.rs": ROOT, "trees/Cargo.toml": manifest("trees", '[lib]\nname = "leaves"\n\n'),
     "trees/src/lib.rs": ROOT},
    "trees's compiled library wasn't found for the symbol check")
CASES["a member postgres doesn't link, checked in a scratch folder named after sim"] = (
    ["doltgres", "sim"],
    {"doltgres/Cargo.toml": manifest("doltgres"), "doltgres/src/lib.rs": ROOT, "sim/Cargo.toml": manifest("sim"),
     "sim/src/lib.rs": ROOT},
    ["!compiled library wasn't found"])

MINTS = "".join(f"#[macro_export]\nmacro_rules! {name} {{\n    () => {{\n        unsafe {{ core::hint::unreachable_unchecked() }}\n"
                f"    }};\n}}\n" for name in ("mint", "forge"))
OUTSIDE = {"doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\noutside = { path = "../../outside" }\n\n'),
           "../outside/Cargo.toml": '[package]\nname = "outside"\nversion = "0.1.0"\nedition = "2024"\n',
           "../outside/src/lib.rs": MINTS}
MINTED = ROOT + "\n/// Never returns.\npub fn never() {\n    if false {\n        outside::mint!()\n    }\n}\n"
CASES["a listed macro of a crate outside the workspace"] = (
    ["doltgres"], {**OUTSIDE, "doltgres/src/lib.rs": MINTED},
    ["!expanded source holds unsafe code", "!couldn't be expanded"],
    {"macros.tsv": "outside::mint\ta case's macro, whose expansion holds unsafe code\t() => { () };\n"})
CASES["a macro of a crate outside the workspace that isn't listed"] = (
    ["doltgres"], {**OUTSIDE, "doltgres/src/lib.rs": MINTED}, "doltgres's lib doltgres's expanded source holds unsafe code")
CASES["an unlisted macro of a crate outside the workspace beside a listed one"] = (
    ["doltgres"], {**OUTSIDE, "doltgres/src/lib.rs": MINTED.replace("mint!", "forge!")},
    "doltgres's lib doltgres's expanded source holds unsafe code",
    {"macros.tsv": "outside::mint\ta case's macro, whose expansion holds unsafe code\t() => { () };\n"})
TOKENS = {**OUTSIDE, "../outside/src/lib.rs": MINTS + "\n/// A token only `token!` makes.\npub struct Token(());\n\n"
          "impl Token {\n    /// Makes a token.\n    ///\n    /// # Safety\n    ///\n    /// Only `token!` calls it.\n"
          "    pub unsafe fn new() -> Token {\n        Token(())\n    }\n}\n\n#[macro_export]\nmacro_rules! token {\n"
          "    () => {\n        unsafe { $crate::Token::new() }\n    };\n}\n",
          "doltgres/src/lib.rs": ROOT + "\n/// Takes a token.\npub fn take(_: outside::Token) {}\n\n/// Takes a made token.\n"
                                 "pub fn made() {\n    take(outside::token!())\n}\n"}
CASES["a listed macro of a crate outside the workspace whose stand-in rule calls an item its line adds"] = (
    ["doltgres"], TOKENS, ["!expanded source holds unsafe code", "!couldn't be expanded"],
    {"macros.tsv": "outside::token\ta case's macro, whose expansion holds unsafe code\t() => { $crate::stand_in_token() };"
                   "\tpub fn stand_in_token() -> Token { unsafe { Token::new() } }\n"})
CASES["a listed macro of a crate outside the workspace whose stand-in rule calls an item no line adds"] = (
    ["doltgres"], TOKENS, "doltgres's lib doltgres couldn't be expanded",
    {"macros.tsv": "outside::token\ta case's macro, whose expansion holds unsafe code\t() => { $crate::stand_in_token() };\n"})
CASES["a ported file with two rows"] = (
    ["trees"], {**CASES["a ported crate with its rows"][1], "doltgrust.tsv": PORTED_ROWS + PORTED_ROWS.split("\n")[0] + "\n"},
    "doltgrust.tsv: trees/src/lib.rs has more than one row")
CASES["a listed call nothing calls"] = (
    ["pg", "doltgres"], {"pg/Cargo.toml": manifest("pg"), "pg/src/lib.rs": PG + "\n/// Calls x.\npub fn x() {}\n",
                         "pg/calls.list": "# A call.\nx\tvoid\t\tnoraise\tnocatalog\tx()\n",
                         "doltgres/Cargo.toml": manifest("doltgres", PG_DEPENDENCY), "doltgres/src/lib.rs": ROOT},
    "calls.list's x isn't called by any crate")
CASES["a listed call another crate calls"] = (
    ["pg", "doltgres"], {**CASES["a listed call nothing calls"][1],
                         "doltgres/src/lib.rs": ROOT + "\n/// Calls pg's x.\npub fn y() {\n    pg::x();\n}\n"},
    "!isn't called by any crate")
CASES["a debug_assert in a ported crate's own third_party folder"] = (
    ["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT,
                "trees/third_party/x.rs": ROOT.replace("    1\n", "    debug_assert!(true);\n    1\n")},
    "trees/third_party/x.rs uses the debug_assert family")
CASES["a debug_assert in a ported crate's own pinned third_party folder"] = (
    ["trees"], {"trees/Cargo.toml": manifest("trees"), "trees/src/lib.rs": ROOT,
                "trees/third_party/pins.tsv": "# path\tupstream\ttag\tcommit\tupstream path\tobject\n",
                "trees/third_party/x.rs": ROOT.replace("    1\n", "    debug_assert!(true);\n    1\n")},
    "trees/third_party/x.rs uses the debug_assert family")
CASES["doltgres linking simc"] = (
    ["doltgres", "simc"], {"doltgres/Cargo.toml": manifest("doltgres", '[dependencies]\nsimc = { path = "../simc" }\n\n'),
                           "doltgres/src/lib.rs": ROOT, "simc/Cargo.toml": manifest("simc"), "simc/src/lib.rs": ROOT},
    "the doltgres library links simc, which only tests may use")


OUTPUT_NAMED = re.compile(r'def main\(\):\n(?:    """(?:(?!""").)*"""\n)?    for stream in \(sys\.stdout, sys\.stderr\):\n'
                          r'        stream\.reconfigure\(encoding="utf-8", errors="backslashreplace"\)\n', re.S)


def own_tables(work):
    """Returns a copy of the gates' folder for the cases to read their tables from, with what names the real
    workspace's own files taken out: the rows of the Verus tables and of unlinked.tsv, and deny.toml's skips of crate
    versions. A case's workspace is a scratch one, so those rows would each be reported in it as gone, and a case that
    must be clean would fail whenever a piece adds one."""
    folder = os.path.join(work, "the-gates-own-tables")
    shutil.rmtree(folder, ignore_errors=True)
    shutil.copytree(rust_gates.HERE, folder, ignore=shutil.ignore_patterns("target", "__pycache__"))
    for name in ("verus-proofs.tsv", "verus-floor.tsv", "trusted.tsv", "unlinked.tsv", "frontend.list",
                 "frontend-callees.tsv"):
        with open(os.path.join(folder, name), encoding="utf-8") as f:
            kept = [line for line in f if line.startswith("#") or not line.strip()]
        write(os.path.join(folder, name), "".join(kept))
    with open(os.path.join(folder, "deny.toml"), encoding="utf-8") as f:
        deny = f.read()
    write(os.path.join(folder, "deny.toml"), re.sub(r"(?m)^skip = \[\n(?:.*\n)*?\]\n", "", deny))
    return folder


def own_failures(work):
    """Checks the gates' own scripts and returns how many checks fail: every file they open as text and every output
    they decode names UTF-8, so a run reads the same text whatever the platform's encoding is (bugs row 29); and a tool
    is run by the path the PATH it is handed finds it on, since Windows looks a child up on its parent's (row 30); and
    every script names its output's encoding as it starts, so a finding that quotes a character the platform's code
    page lacks is printed (row 31)."""
    failures = 0
    scripts = sorted(os.path.join(rust_gates.HERE, name) for name in os.listdir(rust_gates.HERE) if name.endswith(".py"))
    for path in scripts + [os.path.join(rust_gates.HERE, "lint", "run_cases.py")]:
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
    folder = os.path.join(work, "frontend-locks")
    package = ('[[package]]\nname = "outside"\nversion = "1.0.0"\n'
               'source = "registry+https://github.com/rust-lang/crates.io-index"\nchecksum = "{}"\n')
    write(os.path.join(folder, "Cargo.lock"), "version = 4\n\n" + package.format("aa"))
    for checksum, wanted in (("aa", []), ("bb", ["fe/Cargo.lock locks outside 1.0.0, which the root's Cargo.lock "
                                                   "doesn't lock alike"])):
        write(os.path.join(folder, "fe", "Cargo.lock"), "version = 4\n\n" + package.format(checksum))
        if rust_gates.lock_problems(folder, "fe") != wanted:
            failures += 1
            print(f"a frontend lock with checksum {checksum} against the root's aa gives "
                  f"{rust_gates.lock_problems(folder, 'fe')}, not {wanted}")
    folder = os.path.join(work, "tool-by-path")
    name = "gate-tool-case" + (".bat" if os.name == "nt" else "")
    write(os.path.join(folder, name), "#!/bin/sh\n")
    os.chmod(os.path.join(folder, name), 0o755)
    if rust_gates.found_tool(name, {"PATH": folder}) != os.path.join(folder, name):
        failures += 1
        print("a tool on the PATH it is handed isn't run by its path")
    if rust_gates.found_tool(name, {"PATH": os.path.join(folder, "none")}) != name:
        failures += 1
        print("a tool on no PATH isn't left to its name")
    folder = os.path.join(work, "output-in-a-code-page")
    workspace(os.path.join(folder, "ws"), ["doltgres"],
              {"doltgres/Cargo.toml": manifest("doltgres"),
               "doltgres/src/lib.rs": ROOT + '\n#[allow(clippy::disallowed_methods, reason = "\u03a9")]\n'
                                             "/// Returns two.\npub fn two() -> u32 {\n    2\n}\n"})
    done = subprocess.run([sys.executable, os.path.join(rust_gates.HERE, "rust_gates.py"), os.path.join(folder, "ws"),
                           os.path.join(folder, "scratch")], capture_output=True,
                          env=dict(os.environ, PYTHONIOENCODING="cp1252"))
    printed = done.stdout.decode("utf-8", "replace")
    if not re.search("\u03a9.* isn't a listed exemption", printed) or "the Rust gates fail: " not in printed:
        failures += 1
        print("a finding that quotes a character the output's code page lacks isn't printed")
    return failures


def main():
    """Checks every case, printing each mismatch; a case expects no problems, a problem holding its text, or, when its
    text starts with "!", no problem holding the rest (or several such texts, all of which must hold), and may append
    text to the gates' tables in a copy of them, a table under "base/" being main's copy of it for the checks that
    compare with main; a case whose name ends "(twice)" is checked a second time over the
    same scratch folder, as the queue reuses it, and one whose name ends "(in a subfolder)" has its workspace in a
    folder of its repository, as src/doltgres is, one whose name ends "(with a space)" has its workspace under a
    folder whose name holds a space, and one whose name ends "(moved)" is checked first with its appended tables and
    then over the same scratch folder with the gates' own tables, from another path, as each queue cycle copies main's
    gates to a new folder, and one whose name ends "(locked)" is checked with its scratch folder's built lint made
    read-only, so its cache can't be cleared."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    work = os.path.abspath(sys.argv[1])
    failures = 0
    here = rust_gates.HERE = own_tables(work)
    for name, (members, files, expected, *tables) in CASES.items():
        folder = os.path.join(work, name.replace(" ", "-"), *(["repo"] if name.endswith("(in a subfolder)") else []),
                              *(["a space"] if name.endswith("(with a space)") else []), "ws")
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
                if path.startswith("base/"):
                    write(os.path.join(work, name.replace(" ", "-"), path), text)
                else:
                    with open(os.path.join(rust_gates.HERE, path), "a", encoding="utf-8") as f:
                        f.write(text)
        base = os.path.join(work, name.replace(" ", "-"), "base")
        shutil.rmtree(base, ignore_errors=True) if not tables or not any(p.startswith("base/") for p in tables[0]) else None
        os.environ.pop("DOLTGRES_GATES_BASE", None)
        if os.path.isdir(base):
            os.environ["DOLTGRES_GATES_BASE"] = base
        locked = os.path.join(work, name.replace(" ", "-"), "scratch", "lint", "debug")
        if name.endswith("(locked)"):
            os.makedirs(os.path.join(locked, "deps"), exist_ok=True)
            write(os.path.join(locked, "doltgres-lint"), "an earlier build")
            os.chmod(locked, 0o500)
        found = rust_gates.problems(folder, os.path.join(work, name.replace(" ", "-"), "scratch"))
        if name.endswith("(locked)"):
            os.chmod(locked, 0o700)
        if name.endswith("(moved)"):
            rust_gates.HERE = here
            found = rust_gates.problems(folder, os.path.join(work, name.replace(" ", "-"), "scratch"))
        if name.endswith("(twice)"):
            found = rust_gates.problems(folder, os.path.join(work, name.replace(" ", "-"), "scratch"))
        rust_gates.HERE = here
        os.environ.pop("DOLTGRES_GATES_BASE", None)
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
