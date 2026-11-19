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
"""rust_gates.py <workspace> <scratch folder>: the queue's gates over the fork's Cargo workspace (out/VERIFICATION_PLAN.md's
"The gates"), printing each problem and failing when there is one. Every gate fails closed: a tool that can't run, or
output it can't read, is a problem. The tables beside this file change only through the coordinator's refs."""

import collections
import difflib
import glob
import hashlib
import io
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
UNSAFE_CRATES = {"pg", "shmem", "simc"}
TEST_ONLY = {"sim", "simc"}
FORK_CRATES = {"doltgres", "pg", "shmem", "host", "pghost", "access", "catalog", "sim", "fsck", "fehost", "simc"}  # the fork's own, by item 11
FORBID = re.compile(r"^\s*#!\[forbid\(unsafe_code\)\]", re.M)
FEATURE = re.compile(r"#!\[feature\(([^)]*)\)\]")
UNSAFE = re.compile(r"\bunsafe\b|#\[\s*(?:no_mangle|export_name|link_section)\b")
# The unsafe std's derives write (Rust 1.99.1), which #![forbid(unsafe_code)] allows only from a builtin derive: the
# empty marker impl derive(Clone) writes for a Copy type, and the arm derive(PartialEq) writes past an enum's matched
# discriminants.
DERIVED_CLONE = re.compile(r"#\[doc\(hidden\)\]\s*unsafe impl(?:<[^{}]*>)? ::core::clone::TrivialClone for [^{}]+ \{ \}"
                           r"|_ => unsafe \{ ::core::intrinsics::unreachable\(\) \}")
SILENCER = re.compile(r"#!?\[(?:cfg_attr\([^\]]*)?(?:allow|expect)\([^\]]*clippy::(?:disallowed_[a-z_]+|all|style)\b[^\]]*\]")
ITEM = re.compile(r"^\s*(?:pub(?:\([^)]*\))?\s+)?(?:fn|struct|enum|impl|mod|static|const|trait|type|use)\b[^{;=]*", re.M)
DOOR_MESSAGE = re.compile(r"use of a disallowed (?:method|type|macro) `([^`]+)`")
BOOTSTRAP = {"RUSTC_BOOTSTRAP": "pg"}
SAFETY = re.compile(r"^[ \t]*//[ \t]*SAFETY:(.*(?:\n[ \t]*//(?![ \t]*SAFETY:).*)*)", re.M)
MIRI_OPT_OUT = re.compile(r'#\[cfg_attr\(\s*miri\s*,\s*ignore\s*=\s*"[^"]*"\s*\)\]\s*(?:#\[[^\]]*\]\s*)*fn\s+(\w+)')
VERUS_TOOLS = {"verus": "Version: 0.2026.11.02.8bf68ee", "verusfmt": "verusfmt 0.7.4"}
TRUSTED = re.compile(r"\b(?:assume|admit)\s*\(|\bassume_new\b|#\[verifier::external\w*|\bassume_specification\b|"
                     r"\buninterp\s+spec\s+fn\b|\w*axiom\w*")
VERUS_FN = re.compile(r"\bfn\s+(\w+)")
SPEC_CLAUSE = re.compile(r"\b(?:requires|ensures|recommends|decreases)\b")
GHOST_FN = re.compile(r"\b(?:proof|spec)\s+fn\b")
SUPPLY_TOOLS = {"deny": "cargo-deny 0.20.2", "vet": "cargo-vet 0.10.3"}
DOLTGRUST_COPY = "test/doltgrust"  # Doltgrust's tests, copied byte-identical (the owner, 2026-11-15)
VENDORED = {"third_party", DOLTGRUST_COPY}  # the folders of others' code, which only the pins gates read
COPY_OWN = {"Cargo.lock", "Cargo.toml", "README.md", "clippy.toml", "pins.tsv", "rust-toolchain.toml"}  # the copy's own
COPY_REPLACED = {"crates/harness/Cargo.toml", "crates/harness/src/order.rs", "crates/harness/src/pgx/stream.rs",
                 "crates/harness/src/prepared.rs"}  # the fork's replacements of Doltgrust's files, its README says why


def posix_relative(path, start):
    """Returns a path relative to a folder with '/' between its parts, as the tables write them on every platform."""
    return os.path.relpath(path, start).replace(os.sep, "/")


def package_folder(package_id):
    """Returns the folder of a path package from cargo's package id (`path+file:///…#name@version`), its URL's
    escapes undone and its case as the platform compares it, or nothing for another package."""
    if not package_id.startswith("path+file:"):
        return None
    url = urllib.parse.urlparse(package_id[len("path+"):].split("#", 1)[0])
    return os.path.normcase(os.path.normpath(urllib.request.url2pathname(url.path)))


def read_table(name):
    """Returns a table's rows beside this file, each a list of its tab-separated fields, comments and blanks left out."""
    with open(os.path.join(HERE, name), encoding="utf-8") as f:
        return [line.rstrip("\n").split("\t") for line in f if line.strip() and not line.startswith("#")]


def base_tables(*names):
    """Returns main's copies of tables beside this file, as read_table gives them, or None when there is no main to read
    them from: the gates' cases name a folder of them (DOLTGRES_GATES_BASE), a checkout reads origin/main's (a table
    main lacks is empty), and a copy of main's own gates outside a repository, as the queue runs them over a ref that
    changes no gate, has no other main than itself."""
    rows = []
    for name in names:
        folder = os.environ.get("DOLTGRES_GATES_BASE")
        if folder:
            path = os.path.join(folder, name)
            text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        else:
            inside = subprocess.run(["git", "-C", HERE, "rev-parse", "--verify", "--quiet", "origin/main"],
                                    capture_output=True, text=True, encoding="utf-8")
            if inside.returncode != 0:
                return None
            shown = subprocess.run(["git", "-C", HERE, "show", f"origin/main:./{name}"], capture_output=True, text=True, encoding="utf-8")
            text = shown.stdout if shown.returncode == 0 else ""
        rows.append([line.rstrip("\n").split("\t") for line in text.splitlines()
                     if line.strip() and not line.startswith("#")])
    return rows


def run(args, cwd, env=None, stdout_only=False):
    """Runs a command and returns (exit status, its output, or its standard output alone when asked)."""
    try:
        done = subprocess.run(args, cwd=cwd, env={**os.environ, **(env or {})}, capture_output=True, text=True, encoding="utf-8")
    except OSError as error:
        return 127, str(error)
    return done.returncode, done.stdout if stdout_only else done.stdout + done.stderr


def found_tool(name, env):
    """Returns a tool's full path as the PATH of the environment it will run under finds it, or its name when that
    PATH doesn't hold it. Windows looks a child up on its parent's PATH, never on the one the child is handed, so a
    tool found only through that one is run by its path."""
    return shutil.which(name, path=env.get("PATH", os.environ.get("PATH", ""))) or name


def members(workspace, target, paths=False):
    """Returns the workspace's members, each a dict of its name, folder, crate roots, targets, build script,
    dependencies' names, and features, and its other packages as (name, version, source); with `paths`, a path package
    outside the workspace's members is given as a member is, since a second workspace builds the root's crates."""
    code, output = run(["cargo", "metadata", "--format-version", "1", "--locked"], workspace,
                       {"CARGO_TARGET_DIR": target}, stdout_only=True)
    if code != 0:
        raise RuntimeError(f"cargo metadata failed with exit status {code}")
    data = json.loads(output)
    own = set(data["workspace_members"])
    crates, others = [], []
    for package in data["packages"]:
        if package["id"] not in own and not (paths and package.get("source") is None):
            others.append((package["name"], package["version"], package.get("source") or "path"))
            continue
        targets = package["targets"]
        crates.append({"name": package["name"], "folder": os.path.dirname(package["manifest_path"]),
                       "manifest": package["manifest_path"],
                       "roots": [t["src_path"] for t in targets if set(t["kind"]) & {"lib", "staticlib", "bin"}],
                       "targets": [(t["kind"][0], t["name"]) for t in targets
                                   if t["kind"][0] in ("lib", "staticlib", "bin", "test", "example")],
                       "build_script": any("custom-build" in t["kind"] for t in targets),
                       "deps": {d["name"] for d in package["dependencies"]},
                       "link_deps": {d["name"] for d in package["dependencies"] if d.get("kind") != "dev"},
                       "features": sorted(f for f in package["features"] if f != "default")})
    return crates, others


def code_only(source, lines=False):
    """Returns Rust source with its comments, string literals (plain, byte, C, and raw), and character literals blanked
    out, telling a character literal from a lifetime, so that a scan of what remains sees only code; with `lines`, a
    blanked comment or literal keeps its line breaks, so every line keeps its number."""
    out, i, n = [], 0, len(source)
    while i < n:
        c, start = source[i], i
        if source.startswith("//", i):
            end = source.find("\n", i)
            i = n if end < 0 else end
        elif source.startswith("/*", i):
            depth, i = 1, i + 2
            while i < n and depth:
                if source.startswith("/*", i):
                    depth, i = depth + 1, i + 2
                elif source.startswith("*/", i):
                    depth, i = depth - 1, i + 2
                else:
                    i += 1
            out.append("\n" * source.count("\n", start, i) if lines else "")
        elif re.match(r'(?:br|cr|r)#*"', source[i:]) and (i == 0 or not (source[i - 1].isalnum() or source[i - 1] == "_")):
            m = re.match(r'(?:br|cr|r)(#*)"', source[i:])
            close = '"' + m.group(1)
            end = source.find(close, i + m.end())
            i = n if end < 0 else end + len(close)
            out.append('""' + ("\n" * source.count("\n", start, i) if lines else ""))
        elif c == '"' or (c in "bc" and source.startswith('"', i + 1) and
                          (i == 0 or not (source[i - 1].isalnum() or source[i - 1] == "_"))):
            i += 1 if c == '"' else 2
            while i < n and source[i] != '"':
                i += 2 if source[i] == "\\" else 1
            i += 1
            out.append('""' + ("\n" * source.count("\n", start, i) if lines else ""))
        elif c == "'" or (c == "b" and source.startswith("'", i + 1) and
                          (i == 0 or not (source[i - 1].isalnum() or source[i - 1] == "_"))):
            start = i + (1 if c == "'" else 2)
            m = re.match(r"(?:\\(?:u\{[0-9a-fA-F_]+\}|x[0-9a-fA-F]{2}|.)|[^\\\n])'", source[start:])
            if m:
                i = start + m.end()
                out.append("' '")
            else:
                out.append(c)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def unsafe_crate(workspace, crate):
    """Returns whether a member is one of the crates that may hold unsafe code, by its folder."""
    return os.path.relpath(crate["folder"], workspace) in UNSAFE_CRATES


def stand_in_copy(workspace, scratch, crates, inside=""):
    """Returns a scratch copy of the workspace in which each member that macros.tsv names is replaced by a stand-in: a
    crate of the same name that re-exports the real one, renamed, forwards each of its features, and defines each listed
    macro of it by its stand-in rules, which expand to safe code, beside the items its line adds, so that any unsafe
    code in another crate's expansion there came from elsewhere (design item 17). A crate outside the workspace that
    macros.tsv names is copied, each listed macro of it renamed and defined again by its stand-in rules beside its
    line's items, and the members are given the copy: a path
    dependency beside the workspace is copied to where their paths name it from the workspace's copy, listed or not,
    and a registry one is given by a patch of crates.io. With `inside`, a second workspace's folder under the workspace,
    the packages are that workspace's, and its manifest takes the patches."""
    copy = os.path.join(scratch, "stand-in")
    shutil.rmtree(copy, ignore_errors=True)
    shutil.copytree(workspace, copy, ignore=shutil.ignore_patterns("target", ".git"))
    listed = {}
    for path, _, rules, *items in read_table("macros.tsv"):
        owner, _, name = path.partition("::")
        listed.setdefault(owner, []).append((name, rules, "".join(items)))
    for crate in crates:
        if crate["name"] not in listed:
            continue
        folder = os.path.join(copy, os.path.relpath(crate["folder"], workspace))
        real = folder + "-stand-in-real"
        os.rename(folder, real)
        with open(os.path.join(real, "Cargo.toml"), encoding="utf-8") as f:
            manifest = f.read()
        with open(os.path.join(real, "Cargo.toml"), "w", encoding="utf-8") as f:
            f.write(re.sub(r'^name\s*=\s*"[^"]*"', f'name = "{crate["name"]}-stand-in-real"', manifest, count=1,
                           flags=re.M))
        macros = "".join(f"\n#[macro_export]\nmacro_rules! {name} {{\n    {rules}\n}}\n{items}\n"
                         for name, rules, items in listed[crate["name"]])
        os.makedirs(os.path.join(folder, "src"))
        with open(os.path.join(folder, "Cargo.toml"), "w", encoding="utf-8") as f:
            f.write(f'[package]\nname = "{crate["name"]}"\nversion = "0.0.0"\nedition = "2024"\n\n[dependencies]\n'
                    f'real = {{ package = "{crate["name"]}-stand-in-real", path = "../{os.path.basename(real)}" }}\n\n'
                    "[features]\n" + "".join(f'{name} = ["real/{name}"]\n' for name in crate["features"]))
        with open(os.path.join(folder, "src", "lib.rs"), "w", encoding="utf-8") as f:
            f.write(f"//! The stand-in for {crate['name']}.\n\n#![forbid(unsafe_code)]\n\npub use real::*;\n{macros}")
    own = {crate["name"] for crate in crates}
    code, output = run(["cargo", "metadata", "--format-version", "1", "--locked"], os.path.join(workspace, inside),
                       stdout_only=True)
    packages = json.loads(output)["packages"] if code == 0 else []
    patches = []
    for package in sorted(packages, key=lambda p: p["name"]):
        folder = os.path.dirname(package["manifest_path"])
        beside = package["source"] is None and os.path.relpath(folder, workspace).startswith("..")
        if package["name"] in own or not (beside or package["name"] in listed):
            continue
        if beside:
            target = os.path.normpath(os.path.join(copy, os.path.relpath(folder, workspace)))
        else:
            target = os.path.join(scratch, "stand-in-outside", package["name"])
            patches.append(f'{package["name"]} = {{ path = "{target.replace(os.sep, "/")}" }}\n')
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(folder, target, ignore=shutil.ignore_patterns("target", ".git"))
        if package["name"] not in listed:
            continue
        root = next(t["src_path"] for t in package["targets"] if "lib" in t["kind"])
        root = os.path.join(target, os.path.relpath(root, folder))
        with open(root, encoding="utf-8") as f:
            source = f.read()
        for name, rules, items in listed[package["name"]]:
            source = re.sub(rf"\bmacro_rules!\s*{re.escape(name)}\b", f"macro_rules! {name}_stand_in_real", source)
            source += f"\n#[macro_export]\nmacro_rules! {name} {{\n    {rules}\n}}\n{items}\n"
        with open(root, "w", encoding="utf-8") as f:
            f.write(source)
    if patches:
        with open(os.path.join(copy, inside, "Cargo.toml"), "a", encoding="utf-8") as f:
            f.write("\n[patch.crates-io]\n" + "".join(patches))
    return copy


def unsafe_problems(workspace, scratch, crates, features, inside=""):
    """Returns the crates outside pg, shmem, and simc that don't forbid unsafe code at their roots and through the
    workspace lints, that have a build script, or whose macro-expanded source in any target, as the building toolchain
    expands it with macros.tsv's stand-ins, holds unsafe code or an unsafe attribute outside strings and doc comments;
    with `inside`, cargo runs in that second workspace's folder of the copy."""
    copy = stand_in_copy(workspace, scratch, crates, inside)
    found = []
    for crate in crates:
        if unsafe_crate(workspace, crate):
            continue
        name = crate["name"]
        with open(crate["manifest"], encoding="utf-8") as f:
            if not re.search(r"^\[lints\]\s*\nworkspace\s*=\s*true", f.read(), re.M):
                found.append(f"{name} doesn't take the workspace lints")
        if crate["build_script"]:
            found.append(f"{name} has a build script")
        for root in crate["roots"]:
            with open(root, encoding="utf-8") as f:
                if not FORBID.search(f.read()):
                    found.append(f"{posix_relative(root, workspace)} doesn't forbid unsafe code")
        for kind, target_name in crate["targets"]:
            selector = ["--lib"] if kind in ("lib", "staticlib") else [f"--{kind}", target_name]
            code, output = run(["cargo", "rustc", "--offline", "-p", name, *selector, *features, "--profile", "cassert",
                                "--", "-Zunpretty=expanded"], os.path.join(copy, inside),
                               {"CARGO_TARGET_DIR": os.path.join(scratch, "stand-in-target"), "RUSTC_BOOTSTRAP": "1"},
                               stdout_only=True)
            if code != 0:
                found.append(f"{name}'s {kind} {target_name} couldn't be expanded")
            elif UNSAFE.search(DERIVED_CLONE.sub("", code_only(output))):
                found.append(f"{name}'s {kind} {target_name}'s expanded source holds unsafe code")
    return found


def source_files(folder, workspace=False):
    """Returns every Rust source file under a folder, outside build folders, the gates' own tools in verify, which
    are no member of the workspace and are checked by their own cases, and, when the folder is the workspace, its
    vendored folders, whose upstream bytes the pins gates check and no member builds."""
    files = []
    for path, folders, names in os.walk(folder):
        folders[:] = [d for d in folders if d not in ("target", ".git")
                      and not (path == folder and d == "verify")
                      and not (workspace and posix_relative(os.path.join(path, d), folder) in VENDORED)]
        files += [os.path.join(path, n) for n in names if n.endswith(".rs")]
    return sorted(files)


def ported_diff(old, new):
    """Returns the SHA-256 of a ported file's unified diff from Doltgrust's bytes to the fork's, as doltgrust.tsv
    records it: Python's difflib with the labels a and b and three lines of context, over lines that keep their
    endings."""
    lines = difflib.unified_diff(old.decode().splitlines(True), new.decode().splitlines(True), "a", "b")
    return hashlib.sha256("".join(lines).encode()).hexdigest()


def doltgrust_problems(workspace, crates):
    """Returns how doltgrust.tsv fails to account for the crates ported from Doltgrust (members outside FORK_CRATES) and
    for the files copied from Doltgrust into the fork's own crates, every row checked, read from git on both sides,
    Doltgrust's checkout (DOLTGRUST_DIR) only read: every committed file under a ported crate's folder and every file
    under Doltgrust's crates/<crate> at the row's commit has one row; a same row's bytes are equal; a changed row's diff
    is the one recorded; an added row is the fork's alone; and a removed row is Doltgrust's alone."""
    ported = [crate for crate in crates if crate["name"] not in FORK_CRATES]
    table = os.path.join(workspace, "doltgrust.tsv")
    if not ported and not os.path.exists(table):
        return []
    doltgrust = os.environ.get("DOLTGRUST_DIR")
    if not doltgrust:
        return ["DOLTGRUST_DIR names no Doltgrust checkout, so doltgrust.tsv can't be checked"]
    code, prefix = run(["git", "-C", workspace, "rev-parse", "--show-prefix"], workspace, stdout_only=True)
    if code != 0:
        return ["the workspace isn't in git, so its committed files can't be compared with Doltgrust's"]
    prefix = prefix.strip()
    rows = []
    if os.path.exists(table):
        with open(table, encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f if line.strip() and not line.startswith("#")]

    def blob(repo, rev, path):
        try:
            return subprocess.run(["git", "-C", repo, "show", f"{rev}:{path}"], capture_output=True,
                                  check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            return None

    found = [f"doltgrust.tsv: a row with {len(row)} fields, not 6" for row in rows if len(row) != 6]
    rows = [row for row in rows if len(row) == 6]
    counts = collections.Counter([row[0] for row in rows if row[0] != "-"] +
                                 [f"Doltgrust's {row[1]} at {row[2][:12]}" for row in rows if row[1] != "-"])
    found += [f"doltgrust.tsv: {name} has more than one row" for name, count in sorted(counts.items()) if count > 1]
    by_fork = {row[0]: row for row in rows if row[0] != "-"}
    by_doltgrust = {(row[1], row[2]): row for row in rows if row[1] != "-"}
    for crate in ported:
        folder = posix_relative(crate["folder"], workspace)
        code, listing = run(["git", "-C", workspace, "ls-tree", "-r", "--name-only", "HEAD", "--", folder], workspace,
                            stdout_only=True)
        fork_files = listing.split()
        found += [f"{f} isn't in doltgrust.tsv" for f in fork_files if f not in by_fork]
        for commit in sorted({row[2] for row in rows if row[0].startswith(folder + "/") or
                              row[1].startswith(f"crates/{crate['name']}/")}):
            code, theirs = run(["git", "-C", doltgrust, "ls-tree", "-r", "--name-only", commit, "--",
                                f"crates/{crate['name']}"], doltgrust, stdout_only=True)
            if code != 0:
                found.append(f"Doltgrust's {commit[:12]} can't be read")
                continue
            found += [f"Doltgrust's {f} at {commit[:12]} isn't in doltgrust.tsv" for f in theirs.split()
                      if (f, commit) not in by_doltgrust]
    for fork_path, doltgrust_path, commit, state, diff, _ in rows:
        mine = blob(workspace, "HEAD", prefix + fork_path) if fork_path != "-" else None
        theirs = blob(doltgrust, commit, doltgrust_path) if doltgrust_path != "-" else None
        name = fork_path if fork_path != "-" else doltgrust_path
        if state == "same" and (mine is None or mine != theirs):
            found.append(f"doltgrust.tsv: {name} isn't the same as Doltgrust's")
        elif state == "changed" and (mine is None or theirs is None or ported_diff(theirs, mine) != diff):
            found.append(f"doltgrust.tsv: {name}'s change from Doltgrust isn't the one recorded")
        elif state == "added" and (mine is None or doltgrust_path != "-"):
            found.append(f"doltgrust.tsv: {name} is recorded as added but isn't the fork's alone")
        elif state == "removed" and (theirs is None or fork_path != "-"):
            found.append(f"doltgrust.tsv: {name} is recorded as removed but isn't Doltgrust's alone")
        elif state not in ("same", "changed", "added", "removed"):
            found.append(f"doltgrust.tsv: {name} has the unknown state {state}")
    return found


def assertion_problems(workspace, crates):
    """Returns each raw debug_assert, debug_assert_eq, or debug_assert_ne in a crate ported from Doltgrust (a member
    outside FORK_CRATES), whose invariants are held by construction and whose outer-layer checks run in every build, and
    each of their files whose count of debug_assertions in code (in cfg!, an attribute, or any not, any, or all inside
    them) exceeds its line in debug-assertions.tsv (none allowed without one)."""
    allowed = {row[0]: int(row[1]) for row in read_table("debug-assertions.tsv")}
    found = []
    for crate in crates:
        if crate["name"] in FORK_CRATES:
            continue
        for path in source_files(crate["folder"]):
            relative = posix_relative(path, workspace)
            with open(path, encoding="utf-8") as f:
                code = code_only(f.read())
            if re.search(r"\bdebug_assert(?:_eq|_ne)?!", code):
                found.append(f"{relative} uses the debug_assert family, which the storage crates deny")
            count = len(re.findall(r"\bdebug_assertions\b", code))
            if count > allowed.get(relative, 0):
                found.append(f"{relative} has {count} cfg(debug_assertions), more than debug-assertions.tsv's "
                             f"{allowed.get(relative, 0)}")
    return found


PINS_OWN = {"pins.tsv", ".gitattributes", ".gitignore"}
MESON_CALLS = {"project", "files", "include_directories", "static_library", "declare_dependency", "dependency",
               "meson.override_dependency", "meson.get_compiler", "meson.current_source_dir", "meson.project_version",
               "get_id", "get_argument_syntax", "host_machine.system", "host_machine.cpu_family"}
MESON_CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\s*\(")
MESON_PATHLIKE = re.compile(r"/|\.(?:c|h|S|s|asm|inc|cc|cpp)$")
MESON_FLAG = re.compile(r"/arch:\w+")  # MSVC's instruction set flag, which reads as a path
NAMES_UNREADABLE = re.compile(r"[^\x20-\x7e\n]|\\|\?\?")  # a line splice, a trigraph's start, or a control character
NAMES_LEAD = re.compile(r"\s*(?:/\*.*?\*/\s*)*", re.S)  # the comments that open a file, which C ends as this does
NAMES_LINE = re.compile(r"#(?:define|pragma redefine_extname) (_?)(\w+) \1dg_\2|#ifdef __ASSEMBLER__|#else|#endif")


def copy_problems(workspace):
    """Returns how test/doltgrust, Doltgrust's tests copied byte-identical, strays from its pins.tsv: each row's path
    must be, as committed, the git object it records, which must be Doltgrust's at the row's commit and path in
    Doltgrust's checkout (DOLTGRUST_DIR, only read), so a file edited with its row still shows; every other committed file
    under the folder must be one of the fork's own, COPY_OWN, or a replacement in COPY_REPLACED that names the file it
    replaces as "Doltgrust's <path> at <the pinned commit's first 12 digits>"."""
    folder = os.path.join(workspace, DOLTGRUST_COPY)
    if not os.path.isdir(folder):
        return []
    try:
        with open(os.path.join(folder, "pins.tsv"), encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f if line.strip() and not line.startswith("#")]
    except OSError:
        return [f"{DOLTGRUST_COPY} holds no pins.tsv, so its copied files aren't pinned"]
    doltgrust = os.environ.get("DOLTGRUST_DIR")
    if not doltgrust:
        return [f"DOLTGRUST_DIR names no Doltgrust checkout, so {DOLTGRUST_COPY}'s pins can't be checked"]
    found = []
    for row in rows:
        if len(row) != 6:
            found.append(f"{DOLTGRUST_COPY}/pins.tsv's row for {row[0]} doesn't have its six fields")
            continue
        code, out = run(["git", "rev-parse", f"HEAD:./{DOLTGRUST_COPY}/{row[0]}"], workspace, stdout_only=True)
        if code != 0 or out.strip() != row[5]:
            found.append(f"{DOLTGRUST_COPY}/{row[0]} isn't the object pins.tsv records for it ({row[5][:12]})")
        code, out = run(["git", "-C", doltgrust, "rev-parse", f"{row[3]}:{row[4]}"], workspace, stdout_only=True)
        if code != 0 or out.strip() != row[5]:
            found.append(f"{DOLTGRUST_COPY}/pins.tsv's row for {row[0]} isn't Doltgrust's {row[4]} at {row[3][:12]}")
    _, listing = run(["git", "ls-files", DOLTGRUST_COPY], workspace, stdout_only=True)
    pinned = {row[0] for row in rows} | COPY_OWN | COPY_REPLACED
    found += [f"{path} is neither pinned nor one of the fork's own files" for path in listing.splitlines()
              if path[len(DOLTGRUST_COPY) + 1:] not in pinned]
    commits = {row[3][:12] for row in rows if len(row) == 6}
    for path in sorted(COPY_REPLACED):
        if os.path.isfile(os.path.join(folder, path)):
            with open(os.path.join(folder, path), encoding="utf-8", errors="replace") as f:
                text = f.read()
            if not any(f"Doltgrust's {path} at {commit}" in text for commit in commits):
                found.append(f"{DOLTGRUST_COPY}/{path} doesn't name the file it replaces at the pinned commit")
    return found


def pins_problems(workspace):
    """Returns how third_party, the vendored upstream code of design item 15, strays from pins.tsv: each row's path must
    be, as committed, the git object it records (upstream's at the tag, byte for byte); every committed file under the
    folder must lie under a pinned path or be one of the fork's own (pins.tsv, .gitattributes, .gitignore, and each
    pinned project's meson.build, its names.h, which may only give names the prefix dg_, and the headers in its
    include folder, each only names.h and then the pinned header of its own name, both in plain text without a
    backslash or a trigraph and with comments only before the first directive, since the compiler splices lines before
    it reads comments and puts a space where one stood, so that what follows the opening comments is read here exactly
    as the compiler reads it); and each meson.build may call only
    what a vendored build needs (MESON_CALLS, and dependency('threads') alone), and every path it names (in files() and
    include_directories(), or any string that looks like a path) must be its names.h, its include folder, or lie under
    its own pinned paths, so no patched copy elsewhere escapes the pin."""
    folder = os.path.join(workspace, "third_party")
    if not os.path.isdir(folder):
        return []
    try:
        with open(os.path.join(folder, "pins.tsv"), encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f if line.strip() and not line.startswith("#")]
    except OSError:
        return ["third_party holds no pins.tsv, so its vendored code isn't pinned"]
    found, pinned = [], []
    for row in rows:
        if len(row) != 6:
            found.append(f"pins.tsv's row for {row[0]} doesn't have its six fields")
            continue
        code, out = run(["git", "rev-parse", f"HEAD:./third_party/{row[0]}"], workspace, stdout_only=True)
        if code != 0 or out.strip() != row[5]:
            found.append(f"third_party/{row[0]} isn't the object pins.tsv records for it ({row[5][:12]})")
        pinned.append(row[0])

    def under_pin(path):
        return any(path == p or path.startswith(p + "/") for p in pinned)

    projects = {p.split("/")[0] for p in pinned}
    _, listing = run(["git", "ls-files", "third_party"], workspace, stdout_only=True)
    for path in listing.splitlines():
        inner = path.split("/", 1)[1] if "/" in path else ""
        if inner in PINS_OWN or inner in {f"{p}/meson.build" for p in projects} or under_pin(inner):
            continue
        project, _, name = inner.partition("/")
        header = re.fullmatch(r"include/([^/]+\.h)", name)
        if project not in projects or not (name == "names.h" or header):
            found.append(f"{path} is neither pinned nor one of the fork's own files")
            continue
        with open(os.path.join(workspace, path), errors="replace", encoding="utf-8") as f:
            text = f.read()
        if NAMES_UNREADABLE.search(text):
            found.append(f"{path} holds a backslash, a trigraph, or a character that isn't plain text, which the "
                         "compiler could read otherwise than this check does")
            continue
        body = text[NAMES_LEAD.match(text).end():]
        if any(mark in body for mark in ("/*", "*/", "//")):
            found.append(f"{path} holds a comment after its first directive, where only the comments that open the "
                         "file are read as this check reads them")
            continue
        lines = [line.strip() for line in body.splitlines() if line.strip()]
        if header:
            wrapped = re.fullmatch(r'#include "\.\./names\.h"\n#include "\.\./((?:[^"/]+/)*%s)"'
                                   % re.escape(header.group(1)), "\n".join(lines))
            if not wrapped or not under_pin(posixpath.normpath(f"{project}/{wrapped.group(1)}")):
                found.append(f"{path} isn't names.h and then the pinned header of its own name")
        else:
            found += [f"{path} holds a line that does more than give a name the prefix dg_: {line[:80]}"
                      for line in lines if not NAMES_LINE.fullmatch(line)]
    for project in sorted(projects):
        build = os.path.join(folder, project, "meson.build")
        if not os.path.exists(build):
            continue
        with open(build, encoding="utf-8") as f:
            text = "\n".join(line.split("#", 1)[0] for line in f.read().splitlines())
        for m in MESON_CALL.finditer(text):
            name = m.group(1)
            method = name.rsplit(".", 1)[-1] if "." in name and not name.startswith("meson.") else name
            if name not in MESON_CALLS and method not in MESON_CALLS:
                found.append(f"third_party/{project}/meson.build calls {name}(), which a vendored build may not")
        found += [f"third_party/{project}/meson.build depends on {d}, where a vendored build may take only threads"
                  for d in re.findall(r"\bdependency\(\s*'([^']*)'", text) if d != "threads"]
        strings = re.findall(r"'([^']*)'", text)
        named = [n for m in re.finditer(r"\b(?:files|include_directories)\(([^)]*)\)", text)
                 for n in re.findall(r"'([^']*)'", m.group(1))]
        found += [f"third_party/{project}/meson.build names {name}, outside its pinned paths"
                  for name in sorted(set(named) | {t for t in strings if MESON_PATHLIKE.search(t) and not MESON_FLAG.fullmatch(t)})
                  if name not in ("names.h", "include") and not under_pin(posixpath.normpath(f"{project}/{name}"))]
    return found


def text_problems(workspace):
    """Returns each unstable feature outside features.tsv's crates and features, and each attribute that silences a
    door lint (an allow or expect of a disallowed lint or of a group holding them, at any level or under cfg_attr) that
    lint-exempt-allowed.tsv doesn't list by its file, attribute, and the item it covers. These are early reports; the
    canary is what holds the doors."""
    features = {(row[0], row[1]) for row in read_table("features.tsv")}
    exempt = {(row[0], row[1], row[2]) for row in read_table("lint-exempt-allowed.tsv")}
    found = []
    for path in source_files(workspace, workspace=True):
        relative = posix_relative(path, workspace)
        crate = relative.split("/")[0]
        with open(path, errors="replace", encoding="utf-8") as f:
            text = f.read()
        for match in FEATURE.finditer(text):
            for feature in (f.strip() for f in match.group(1).split(",")):
                if (crate, feature) not in features:
                    found.append(f"{relative}: the unstable feature {feature}")
        for match in SILENCER.finditer(text):
            item = ITEM.search(text, match.end())
            covered = "the crate" if match.group(0).startswith("#!") else (
                " ".join(item.group(0).split()) if item else "nothing")
            attribute = " ".join(match.group(0).split())
            if (relative, attribute, covered) not in exempt:
                found.append(f"{relative}: {attribute} over {covered} isn't a listed exemption")
    return found


def doors_for(crate):
    """Returns the doors that apply to a member on this platform: every door for any platform, the platform's own, and
    those of the crates it depends on."""
    platform = "windows" if sys.platform.startswith("win") else "unix"
    return [row for row in read_table("doors.tsv") if row[2] in ("any", platform) or row[2] in crate["deps"]]


def canary_problems(workspace, scratch, crates, features, inside=""):
    """Returns each door that clippy, run as the gates run it, doesn't refuse in a member crate, by compiling a canary
    module that uses every door into each member of a scratch copy of the workspace, one member at a time: the canary
    is taken out again before the next member's turn, and a door counts as refused only by a message about that
    member's own canary, since clippy also lints the members a member depends on, whose canaries would otherwise stop
    the build before the member's own is read and answer for it. It fails by effect whatever silences a door: a
    dropped entry, a member's own clippy.toml, a cargo config's flags, or an attribute. With `inside`, clippy runs in
    that second workspace's folder of the copy."""
    copy = os.path.join(scratch, "canary")
    shutil.rmtree(copy, ignore_errors=True)
    shutil.copytree(workspace, copy, ignore=shutil.ignore_patterns("target", ".git"))
    found = []
    for crate in crates:
        folder = os.path.join(copy, os.path.relpath(crate["folder"], workspace))
        doors = doors_for(crate)
        root = os.path.join(folder, os.path.relpath(crate["roots"][0], crate["folder"])) if crate["roots"] else None
        if root is None:
            continue
        body = "\n".join(f"    {row[3]}" for row in doors)
        canary = os.path.join(os.path.dirname(root), "gate_canary.rs")
        with open(canary, "w", encoding="utf-8") as f:
            f.write(f"#[allow(dead_code, unused, deprecated, unreachable_code)]\npub fn gate_canary() {{\n{body}\n}}\n")
        with open(root, encoding="utf-8") as f:
            original = f.read()
        with open(root, "w", encoding="utf-8") as f:
            f.write(original + "\nmod gate_canary;\n")
        code, output = run(["cargo", "clippy", "--locked", "-p", crate["name"], *features, "--profile", "cassert",
                            "--message-format=json", "--", "-D", "warnings"], os.path.join(copy, inside),
                           {"CARGO_TARGET_DIR": os.path.join(scratch, "canary-target"), **BOOTSTRAP})
        os.remove(canary)
        with open(root, "w", encoding="utf-8") as f:
            f.write(original)
        refused = set()
        for line in output.splitlines():
            if not line.startswith("{"):
                continue
            try:
                message = json.loads(line).get("message") or {}
            except ValueError:
                return found + [f"clippy printed a message the canary couldn't read for {crate['name']}"]
            if any(os.path.normpath(os.path.join(copy, inside, s.get("file_name", ""))) == os.path.normpath(canary)
                   for s in message.get("spans", [])):
                m = DOOR_MESSAGE.search(message.get("message", ""))
                if m:
                    refused.add(m.group(1))
        found += [f"{crate['name']} doesn't refuse the door {row[1]}" for row in doors if row[1] not in refused]
    return found


def linked_members(crates):
    """Returns the names of the members the doltgres library links, through dependencies of any kind but dev ones, or
    every member when there's no doltgres; sim and other test-only crates fall outside it."""
    by_name = {crate["name"]: crate for crate in crates}
    linked, todo = set(), ["doltgres"] if "doltgres" in by_name else list(by_name)
    while todo:
        name = todo.pop()
        if name in by_name and name not in linked:
            linked.add(name)
            todo += list(by_name[name]["link_deps"])
    return linked


def closure_problems(crates):
    """Returns each test-only member (sim and simc) the doltgres library would link through a dependency of any kind but dev
    ones, since only crates' tests may use it, never the server."""
    return [f"the doltgres library links {name}, which only tests may use" for name in sorted(linked_members(crates))
            if name in TEST_ONLY and any(crate["name"] == "doltgres" for crate in crates)]


TEST_FEATURES = {"host": "testing", "pg": "outside_postgres"}


def testing_problems(workspace, crates, package="doltgres"):
    """Returns how a test-only feature (host's testing, which mints Io for tests, and pg's outside_postgres, which
    links the stand-ins of a binary built without Postgres) reaches a shipped library's build, the doltgres library's
    or a frontend workspace's own crate's, with its default features or with every one, since only test crates may
    enable them, through dev-dependencies; each line of the tree leads with its features, since the package's folder
    that follows may hold spaces."""
    names = {crate["name"] for crate in crates}
    if package not in names or not names & set(TEST_FEATURES):
        return []
    build = "the server's" if package == "doltgres" else f"{package}'s"
    found = []
    for features in ([], ["--all-features"]):
        code, tree = run(["cargo", "tree", "--locked", "-p", package, *features, "-e", "normal,build", "--prefix",
                          "none", "-f", "{f} {p}"], workspace, stdout_only=True)
        if code != 0:
            found.append("cargo tree fails, so the test-only features can't be checked")
            continue
        for crate, feature in TEST_FEATURES.items():
            if any(line.partition(" ")[2].startswith(crate + " v") and feature in line.partition(" ")[0].split(",")
                   for line in tree.splitlines()):
                found.append(f"{crate}'s {feature} feature reaches {build} build with every feature" if features
                             else f"{crate}'s {feature} feature reaches {build} default build")
    return found


def symbol_problems(workspace, target, crates, features):
    """Returns each symbol a crate the server links references in its own compiled objects, in the build without link-
    time optimization, that symbols-refused.tsv refuses, by its demangled name, so that a door clippy can't see still
    holds; it covers every crate the doltgres library links, its members and their third-party crates, so a member
    postgres never links, such as sim, is left out (every member without a doltgres), each built for an explicit target,
    so that build scripts, their dependencies, and procedural macros, which run only at build time, are told apart by
    folder; which crates.tsv may give a reviewed exception, reads every object of an rlib (a -sys crate's bundled C
    included) but only the crate's own objects of a staticlib (which bundles std and every dependency), and reads them
    with the toolchain's own llvm-nm, since Apple's nm can't read the bitcode Rust embeds; it records each member's
    build-script output folder on the member, for the contract check."""
    code, sysroot = run(["rustc", "--print", "sysroot"], workspace, stdout_only=True)
    _, version = run(["rustc", "-vV"], workspace, stdout_only=True)
    host = re.search(r"^host: (\S+)", version, re.M)
    nm = os.path.join(sysroot.strip(), "lib", "rustlib", host.group(1) if host else "", "bin",
                      "llvm-nm" + (".exe" if os.name == "nt" else ""))
    if code != 0 or not os.path.exists(nm):
        return ["the toolchain's llvm-nm (the llvm-tools component) is missing, so the symbol check can't run"]
    linked = linked_members(crates)
    packages = ["-p", "doltgres"] if any(crate["name"] == "doltgres" for crate in crates) else ["--workspace"]
    code, output = run(["cargo", "build", "--locked", *packages, *features, "--profile", "cassert", "--target",
                        host.group(1), "--message-format=json"], workspace, {"CARGO_TARGET_DIR": target, **BOOTSTRAP},
                       stdout_only=True)
    if code != 0:
        return ["the build for the symbol check fails"]
    built = os.path.join(target, host.group(1)) + os.sep
    files = {}
    for line in output.splitlines():
        try:
            message = json.loads(line)
        except ValueError:
            continue
        if message.get("reason") == "build-script-executed":
            for crate in crates:
                if package_folder(message.get("package_id", "")) == os.path.normcase(os.path.normpath(crate["folder"])):
                    crate["out_dir"] = message.get("out_dir")
        if message.get("reason") == "compiler-artifact":
            files.setdefault(message["target"]["name"], []).extend(
                f for f in message.get("filenames", []) if f.startswith(built) and f.endswith((".rlib", ".a", ".lib")))
    refused = [(re.compile(row[0]), row[1]) for row in read_table("symbols-refused.tsv")]
    exceptions = {row[0]: re.compile(row[5]) for row in read_table("crates.tsv") if len(row) > 5 and row[5]}
    found = [f"{crate['name']}'s compiled library wasn't found for the symbol check" for crate in crates
             if crate["name"] in linked and not files.get(crate["name"])]
    for name, libraries in files.items():
        own = name.replace("-", "_") + "-"
        for library in libraries:
            code, listing = run([nm, "-u", "--demangle", library], workspace, stdout_only=True)
            if code != 0:
                found.append(f"llvm-nm couldn't read {name}'s library")
                continue
            member = None
            for line in listing.splitlines():
                if line.endswith(":") and not line.startswith(" "):
                    m = re.search(r"\(([^)]*)\):$", line)
                    member = m.group(1) if m else os.path.basename(line[:-1])
                    continue
                if not line.strip() or member is None or member == "lib.rmeta" or (
                        not library.endswith(".rlib") and not member.startswith(own)):
                    continue
                symbol = line.strip()
                if name in exceptions and exceptions[name].search(symbol):
                    continue
                for pattern, reason in refused:
                    if pattern.search(symbol):
                        found.append(f"{name} references {symbol[:160]} ({reason})")
    return sorted(set(found))


def lock_packages(workspace):
    """Returns Cargo.lock's packages as {(name, version): (source, checksum)}."""
    packages = {}
    try:
        with open(os.path.join(workspace, "Cargo.lock"), encoding="utf-8") as f:
            blocks = f.read().split("[[package]]")[1:]
    except OSError:
        return packages
    for block in blocks:
        fields = dict(re.findall(r'^(\w+) = "([^"]*)"', block, re.M))
        packages[(fields.get("name"), fields.get("version"))] = (fields.get("source", "path"),
                                                                 fields.get("checksum", ""))
    return packages


def crate_problems(workspace, others):
    """Returns each linked package that crates.tsv doesn't list by name, version, source, and Cargo.lock's checksum."""
    allowed = {(row[0], row[1], row[2], row[3]) for row in read_table("crates.tsv")}
    lock = lock_packages(workspace)
    found = []
    for name, version, _ in others:
        source, checksum = lock.get((name, version), ("unlocked", ""))
        if (name, version, source, checksum) not in allowed:
            found.append(f"the crate {name} {version} isn't on the allowlist")
    return found


def verus_crates(crates):
    """Returns the members whose manifests ask for Verus (package.metadata.verus's verify)."""
    verus = []
    for crate in crates:
        with open(crate["manifest"], encoding="utf-8") as f:
            if re.search(r"^\[package\.metadata\.verus\]\s*\nverify\s*=\s*true", f.read(), re.M):
                verus.append(crate)
    return verus


def verus_sources(crate):
    """Returns every source file of a Verus crate with its text, and whether it holds verus! blocks."""
    found = {}
    for path in source_files(crate["folder"]):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        found[path] = (text, bool(re.search(r"\bverus!\s*\{", text)))
    return found


def specified_functions(text):
    """Returns each function of a Verus source that is a proof function or carries requires, ensures, recommends, or
    decreases, as (name, its signature and clauses up to the body's brace, whitespace collapsed, the line of its name),
    since its clauses are what Verus proves and dropping one leaves its verified count unchanged."""
    lines = code_only(text, lines=True).splitlines()
    found = []
    for i, line in enumerate(lines):
        m = VERUS_FN.search(line)
        if not m:
            continue
        header = []
        for following in lines[i:]:
            if following.strip() == "{":
                break
            header.append(following)
            if following.rstrip().endswith("{"):
                break
        signature = " ".join(" ".join(header).split()).rstrip("{ ").strip()
        if re.search(r"\bproof\s+fn\b", signature) or SPEC_CLAUSE.search(signature):
            found.append((m.group(1), signature, i + 1))
    return found


def trusted_statements(text):
    """Returns the trusted statements a Verus source holds, each the code of its line, an attribute joined to the line
    it covers with " / ", as trusted.tsv lists them."""
    lines = code_only(text).splitlines()
    found = []
    for i, line in enumerate(lines):
        if TRUSTED.search(line):
            statement = line.strip()
            if statement.startswith("#[") and i + 1 < len(lines):
                statement += " / " + lines[i + 1].strip()
            found.append(statement)
    return found


def verus_problems(workspace, scratch, crates):
    """Returns how Verus fails over the crates whose manifests ask for it (package.metadata.verus's verify), on the
    pinned Verus (VERUS_DIR, or PATH), each cleaned first so Verus checks it again: each must verify with exit 0 and
    exactly one "N verified, 0 errors" result, N at
    least its floor in verus-floor.tsv; its verus! blocks must be as verusfmt writes them; its specified functions
    (proof functions and those with clauses), with their clauses, must be exactly verus-proofs.tsv's, counted, so a
    dropped proof or clause bounces; the trusted statements in every source of the crate must be exactly trusted.tsv's,
    counted, each the owner's to approve; and a crate verus-floor.tsv names must keep its metadata."""
    verus = verus_crates(crates)
    floors = {row[0]: int(row[1]) for row in read_table("verus-floor.tsv")}
    names = {crate["name"] for crate in verus}
    found = [f"{name} has a floor in verus-floor.tsv but no Verus metadata" for name in sorted(set(floors) - names)]
    listed_proofs = collections.Counter((row[0], row[1], row[2]) for row in read_table("verus-proofs.tsv"))
    listed_trusted = collections.Counter((row[0], row[1]) for row in read_table("trusted.tsv"))
    proofs, trusted = collections.Counter(), collections.Counter()
    for crate in verus:
        for path, (text, has_verus) in verus_sources(crate).items():
            relative = posix_relative(path, workspace)
            if has_verus:
                proofs.update((relative, name, spec) for name, spec, _ in specified_functions(text))
            trusted.update((relative, statement) for statement in trusted_statements(text))
    found += [f"{f}: {n} ({s[:80]}) isn't in verus-proofs.tsv as many times as the source holds it"
              for f, n, s in sorted(proofs - listed_proofs)]
    found += [f"{f}: verus-proofs.tsv's {n} ({s[:80]}) is gone or changed"
              for f, n, s in sorted(listed_proofs - proofs)]
    found += [f"{f}: the trusted statement {t[:80]} isn't in trusted.tsv as many times as the source holds it"
              for f, t in sorted(trusted - listed_trusted)]
    found += [f"{f}: trusted.tsv's statement {t[:80]} is gone" for f, t in sorted(listed_trusted - trusted)]
    if not verus:
        return found
    env = {"CARGO_TARGET_DIR": os.path.join(scratch, "verus")}
    if os.environ.get("VERUS_DIR"):
        env["PATH"] = os.environ["VERUS_DIR"] + os.pathsep + os.environ.get("PATH", "")
    for tool, pinned in VERUS_TOOLS.items():
        code, version = run([found_tool(tool, env), "--version"], workspace, env)
        if code != 0 or pinned not in version:
            return found + [f"the gates need {tool} at {pinned}, and found {version.strip()[:80] or 'none'}"]
    for crate in verus:
        run(["cargo", "clean", "-q", "-p", crate["name"]], workspace, env)
        try:
            done = subprocess.run([found_tool("cargo-verus", env), "verus", "verify", "--locked", "-p", crate["name"]],
                                  cwd=workspace,
                                  env={**os.environ, **env}, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, encoding="utf-8")
            code, output = done.returncode, done.stdout
        except OSError as error:
            code, output = 127, str(error)
        mine = output[output.find(f"ing {crate['name']} v"):] if f"ing {crate['name']} v" in output else output
        results = re.findall(r"verification results:: (.*)", mine)
        counted = re.fullmatch(r"(\d+) verified, 0 errors", results[0].strip()) if len(results) == 1 else None
        if code != 0 or not counted:
            found.append(f"Verus fails on {crate['name']} (exit {code}, results: {'; '.join(results) or 'none'})")
        elif crate["name"] not in floors:
            found.append(f"{crate['name']} has no floor in verus-floor.tsv")
        elif int(counted.group(1)) < floors[crate["name"]]:
            found.append(f"Verus verifies {counted.group(1)} in {crate['name']}, "
                         f"below its floor of {floors[crate['name']]}")
        files = sorted(path for path, (_, has_verus) in verus_sources(crate).items() if has_verus)
        code, _ = run([found_tool("verusfmt", env), "--verus-only", "--check", *files], workspace, env)
        if code != 0:
            found.append(f"verusfmt would reformat {crate['name']}'s verus! blocks")
    return found


def reached_functions(workspace, scratch, crates):
    """Returns the workspace's functions the doltgres library's exported functions reach, as (file, line of the name,
    name), by the typed lint's walk over a check of that library alone, in the release profile with its default
    features and no other target in the run, as the server builds it: with tests among a run's targets cargo turns
    their dev-dependencies' features on in the library's own dependencies, which would count test-only code as the
    server's. It returns nothing, and why, when there is no doltgres library or the walk can't run."""
    if not any(crate["name"] == "doltgres" for crate in crates):
        return set(), "the workspace has no doltgres library"
    driver, env = lint_run(workspace, scratch)
    if not os.path.exists(driver):
        return set(), "the typed lint isn't built"
    path = os.path.join(scratch, "reached.tsv")
    if os.path.exists(path):
        os.remove(path)
    run(["cargo", "clean", "-q", "--release", "-p", "doltgres"], workspace, env)
    code, output = run(["cargo", "check", "--locked", "-p", "doltgres", "--lib", "--release"], workspace,
                       {**env, "DOLTGRES_LINT_REACHED": path})
    if code != 0 or not os.path.exists(path):
        return set(), "the walk fails: " + (output.strip().splitlines() or ["no output"])[-1][:200]
    with open(path, encoding="utf-8") as f:
        rows = [line.rstrip("\n").split("\t") for line in f]
    return {(row[0], int(row[1]), row[2]) for row in rows if len(row) == 3}, None


def linked_problems(workspace, scratch, crates):
    """Returns each function Verus proves a specification of that the server can't run and unlinked.tsv doesn't list
    (out/VERIFICATION_PLAN.md: every proof is of a function the server links, or of one the list names until its caller
    lands): a function of a Verus crate with requires, ensures, recommends, or decreases whose code a build keeps (not a
    proof or spec function), which the typed lint's walk from the doltgres library's exported functions didn't reach,
    by its file, the line of its name, and its name; and each line of unlinked.tsv whose function is reached now, gone,
    or changed, so the list shrinks as callers land; and each line the tree adds to main's list beyond the rows it adds to main's verus-proofs.tsv
    for the same file and name, since a line comes only with the landing that brings its function, never to excuse a
    call taken away, whatever its clauses now read. It fails closed: without a walk, nothing is reached, and where there
    is a doltgres library a walk that didn't run is itself a problem, since a list that names every function would
    otherwise pass with nothing checked."""
    proved = []
    for crate in verus_crates(crates):
        for path, (text, has_verus) in sorted(verus_sources(crate).items()):
            if has_verus:
                proved += [(posix_relative(path, workspace), line, name, spec)
                           for name, spec, line in specified_functions(text) if not GHOST_FN.search(spec)]
    listed = collections.Counter((row[0], row[1], row[2]) for row in read_table("unlinked.tsv"))
    if not proved and not listed:
        return []
    reached, why = reached_functions(workspace, scratch, crates)
    unreached = collections.Counter((relative, name, spec) for relative, line, name, spec in proved
                                    if (relative, line, name) not in reached)
    found = [f"{f}: Verus proves {n} ({s[:80]}), which nothing the server's entries reach calls, and unlinked.tsv "
             "doesn't list it" + (f" ({why})" if why else "") for f, n, s in sorted(unreached - listed)]
    found += [f"{f}: unlinked.tsv lists {n} ({s[:80]}), which the server's entries reach now, or which is gone or "
              "changed" for f, n, s in sorted(listed - unreached)]
    if why and any(crate["name"] == "doltgres" for crate in crates):
        found.append(f"the walk from the server's entries didn't run ({why}), so nothing shows which of unlinked.tsv's "
                     "lines the server now reaches")
    base = base_tables("unlinked.tsv", "verus-proofs.tsv")
    if base is not None:
        def by_name(rows):
            return collections.Counter((row[0], row[1]) for row in rows)
        more_listed = by_name(read_table("unlinked.tsv")) - by_name(base[0])
        more_proved = by_name(read_table("verus-proofs.tsv")) - by_name(base[1])
        found += [f"{f}: unlinked.tsv gains {n}, which main already proves: a line comes only with the landing that "
                  "brings its function" for f, n in sorted(more_listed - more_proved)]
    return found


def supply_problems(workspace):
    """Returns how cargo-deny, with deny.toml, and cargo-vet, with the store in supply-chain, fail over the workspace's
    crates, each tool at its pinned version, and any exemption, criteria, policy, or audit-as-crates-io in the store,
    since every linked crate is vetted by an imported audit, the fork's own audit, or trust in a publisher whom at least
    two of the importing organizations trust for that crate."""
    found = []
    for tool, pinned in SUPPLY_TOOLS.items():
        code, version = run(["cargo", tool, "--version"], workspace, stdout_only=True)
        if code != 0 or version.strip() != pinned:
            found.append(f"the gates need {pinned}, and found {version.strip()[:80] or 'none'}")
    if found:
        return found
    code, output = run(["cargo", "deny", "--log-level", "error", "--locked", "--workspace", "--config",
                        os.path.join(HERE, "deny.toml"), "check", "-D", "warnings"], workspace)
    if code != 0:
        summary = re.findall(r"^advisories \S+, bans \S+, licenses \S+, sources \S+$", output, re.M)
        found.append("cargo-deny fails: " + (summary or output.strip().splitlines() or ["no output"])[-1][:300])
    store = os.path.join(HERE, "supply-chain")
    code, output = run(["cargo", "vet", "--locked", "--store-path", store], workspace)
    if code != 0:
        found.append("cargo-vet fails: " + " ".join(output.strip().splitlines()[:3])[:300])
    for name in ("config.toml", "audits.toml"):
        with open(os.path.join(store, name), encoding="utf-8") as f:
            text = f.read()
        found += [f"cargo-vet's {name} holds {form}, which could vet a crate without an audit"
                  for form, pattern in (("an exemption", r"^\[\[?exemptions\b"), ("its own criteria", r"^\[criteria\b"),
                                        ("a policy", r"^\[policy\b"), ("audit-as-crates-io", r"audit-as-crates-io"))
                  if re.search(pattern, text, re.M)]
    return found


def lint_run(workspace, scratch):
    """Returns the built typed lint's driver in the scratch folder and the environment that runs it as rustc's wrapper
    over the workspace, with the check's own target folder."""
    lint = os.path.join(HERE, "lint")
    _, sysroot = run(["rustc", "--print", "sysroot"], lint, stdout_only=True)
    lib = os.path.join(sysroot.strip(), "lib")
    driver = os.path.join(scratch, "lint", "debug", "doltgres-lint" + (".exe" if os.name == "nt" else ""))
    return driver, {"CARGO_TARGET_DIR": os.path.join(scratch, "lint-check"), "RUSTC_BOOTSTRAP": "1",
                    "RUSTC_WRAPPER": driver, "DOLTGRES_WORKSPACE": workspace, "DYLD_LIBRARY_PATH": lib,
                    "LD_LIBRARY_PATH": lib,
                    "PATH": os.path.join(sysroot.strip(), "bin") + os.pathsep + os.environ.get("PATH", "")}


def lint_findings(output):
    """Returns the typed lint's findings in cargo's output, each with the place rustc names under it, so one rule's
    findings in two members read apart."""
    found, lines = [], output.splitlines()
    for at, line in enumerate(lines):
        if (line.startswith(("error: ", "doltgres-lint: ")) and "could not compile" not in line
                and "process didn't exit successfully" not in line):
            place = next((under.strip()[4:] for under in lines[at + 1:at + 3] if under.strip().startswith("--> ")), "")
            found.append(f"the typed lint: {line.split(': ', 1)[1]}" + (f" ({place})" if place else ""))
    return found


def uncleared(packages, folder, target):
    """Returns the problem of a cached build of the packages given that cargo can't clear, or nothing."""
    code, output = run(["cargo", "clean", "-q", *packages], folder, {"CARGO_TARGET_DIR": target})
    return [] if code == 0 else ["the typed lint's cached build can't be cleared, so it could read old tables or skip "
                                 "a member: " + (output.strip().splitlines() or ["no output"])[-1][:200]]


def lint_problems(workspace, scratch, crates, features, inside=""):
    """Returns the typed lint's findings over the library targets of the members the doltgres library links (design
    item 17), once there's a member besides doltgres, building the lint beside this file and running it as rustc's
    wrapper, with a target folder of its own, since it compiles every crate with flags of its own; test-only crates
    such as sim, and the targets only cargo test builds, never run in a Postgres process, which the lint's rules
    protect, and the line is what cargo builds, never an attribute code puts on itself. It
    rebuilds the lint and rechecks the linked members on
    every run, since a cached lint reads the tables of the copy it was built from, and a member cached as passing would
    skip the lint under changed tables; a cache it can't clear fails the gate. When the check of the members together
    fails it checks each member by itself: cargo stops at the first member that fails and never reaches the members
    that depend on it, so one run would hide their findings until the first was fixed. It clears each member's cached
    check before that member's own: the lint checks only the packages cargo was asked for, so a member an earlier
    member's check compiled as a dependency passed there unread and would be skipped as fresh. With `inside`, cargo
    runs in that second workspace's folder, the tables still read from the workspace's."""
    cwd = os.path.join(workspace, inside)
    if not {crate["name"] for crate in crates} - {"doltgres"}:
        return []
    lint = os.path.join(HERE, "lint")
    lint_target = os.path.join(scratch, "lint")
    check_target = os.path.join(scratch, "lint-check")
    packages = [a for name in sorted(linked_members(crates)) for a in ("-p", name)]
    problem = uncleared(["-p", "doltgres-lint"], lint, lint_target) or uncleared(packages, cwd, check_target)
    if problem:
        return problem
    code, output = run(["cargo", "build", "--locked"], lint, {"CARGO_TARGET_DIR": lint_target, "RUSTC_BOOTSTRAP": "1"})
    if code != 0:
        return ["the typed lint doesn't build: " + (output.strip().splitlines() or ["no output"])[-1][:300]]
    driver, env = lint_run(workspace, scratch)
    code, output = run(["cargo", "check", "--locked", *packages, "--lib", *features], cwd, env)
    if code == 0:
        return []
    names = sorted(linked_members(crates))
    outputs = [output] if len(names) < 2 else []
    for name in [] if outputs else names:
        problem = uncleared(["-p", name], cwd, check_target)
        if problem:
            return problem
        outputs.append(run(["cargo", "check", "--locked", "-p", name, "--lib", *features], cwd, env)[1])
    findings = sorted({finding for text in outputs for finding in lint_findings(text)})
    return findings or ["the typed lint fails: " + (output.strip().splitlines() or ["no output"])[-1][:300]]


def contract_problems(workspace, scratch, crate):
    """Returns how an unsafe crate's contracts go untested (design item 17): its manifest must deny clippy's
    undocumented_unsafe_blocks, so every unsafe block states its contract in a SAFETY comment, and each SAFETY comment,
    in its sources and in the Rust its build script generated in this build (the folder cargo reported to the symbol
    check, never another build's), must end by naming the tests that exercise it ("Tested by tests::<fn>" for a test of
    the crate's own, or "Tested by test/t/<file>.pl: <label>" for a Postgres-side test whose file holds the label as a
    whole Perl string, quoted and escaped as Perl writes it), each of which must exist; a plain #[ignore] would hide a
    test from both runs."""
    found = []
    with open(crate["manifest"], encoding="utf-8") as f:
        if not re.search(r'^undocumented_unsafe_blocks\s*=\s*"deny"', f.read(), re.M):
            found.append(f"{crate['name']} doesn't deny clippy's undocumented_unsafe_blocks")
    sources = [path for path in source_files(crate["folder"]) if os.path.basename(path) != "build.rs"]
    out_dir = crate.get("out_dir")
    generated = sorted(glob.glob(os.path.join(out_dir, "*.rs"))) if out_dir else []
    if crate["build_script"] and not generated:
        found.append(f"the Rust {crate['name']}'s build script generates wasn't found, so its contracts weren't read")
    texts = {path: open(path, encoding="utf-8").read() for path in sources + generated}
    tests = {m.group(1) for path in sources
             for m in re.finditer(r"#\[test\]\s*(?:#\[[^\]]*\]\s*)*fn\s+(\w+)", code_only(texts[path]))}
    for path, text in texts.items():
        where = (posix_relative(path, workspace) if path in sources
                 else f"{crate['name']}'s generated {os.path.basename(path)}")
        if path in sources and re.search(r"#\[ignore\b", code_only(text)):
            found.append(f"{where} ignores a test outside miri-ignored.tsv's opt-outs")
        for m in SAFETY.finditer(text):
            comment = " ".join(line.strip().lstrip("/").strip() for line in m.group(1).splitlines())
            named = re.search(r"\bTested by (.+?)\.?\s*$", comment)
            if not named:
                found.append(f"{where}: a SAFETY comment names no test: {comment[:80]}")
                continue
            for name in named.group(1).split("; "):
                tap = re.fullmatch(r"(test/t/[\w.-]+\.pl): (.+)", name)
                if tap:
                    target = os.path.join(workspace, tap.group(1))
                    label = tap.group(2).replace("\\", "\\\\")
                    quoted = ("'" + label.replace("'", "\\'") + "'", '"' + label.replace('"', '\\"') + '"')
                    if not os.path.isfile(target) or not any(q in open(target, encoding="utf-8").read() for q in quoted):
                        found.append(f"{where}: {name} names no test that exists")
                elif not (re.fullmatch(r"tests::(\w+)", name) and name[7:] in tests):
                    found.append(f"{where}: {name} names no test of {crate['name']}")
    return found


def miri_problems(workspace, scratch, crates, features, inside=""):
    """Returns how Miri, on the pinned nightly, fails over the library and test targets of pg, shmem, and simc, the
    crates that hold unsafe code, and each mention of miri in their code but a test's opt-out that miri-ignored.tsv
    lists, so that only a test that calls C is hidden from Miri. On the MSVC target the Miri run itself doesn't run
    (the owner, 2026-11-09): with pg's global allocator forwarding to System, Miri interprets std's Windows dealloc of
    over-aligned blocks and reports it under both aliasing models before any test runs; the opt-out and contract checks
    still do. With `inside`, Miri runs in that second workspace's folder, so the code only its cfg builds is run too."""
    unsafe = [crate for crate in crates if unsafe_crate(workspace, crate)]
    if not unsafe:
        return []
    nightly = read_table("nightly.txt")[0][0]
    listed = {(row[0], row[1]) for row in read_table("miri-ignored.tsv")}
    found = []
    for crate in unsafe:
        for path in source_files(crate["folder"]):
            relative = posix_relative(path, workspace)
            with open(path, encoding="utf-8") as f:
                text = code_only(f.read())
            opt_outs = MIRI_OPT_OUT.findall(text)
            if len(re.findall(r"\bmiri\b", text)) != len(opt_outs):
                found.append(f"{relative} mentions miri outside a test's opt-out")
            found += [f"{relative}: the test {name} opts out of Miri, which miri-ignored.tsv doesn't list"
                      for name in opt_outs if (relative, name) not in listed]
    for crate in unsafe:
        found += contract_problems(workspace, scratch, crate)
    code, _ = run(["rustc", f"+{nightly}", "-vV"], workspace)
    if code != 0:
        return found + [f"the pinned nightly {nightly} isn't installed"]
    _, version = run(["rustc", "-vV"], workspace, stdout_only=True)
    if re.search(r"^host: \S*windows-msvc", version, re.M):
        return found
    code, output = run(["cargo", f"+{nightly}", "miri", "test", "--locked", "--lib", "--tests", *features,
                        *[a for crate in unsafe for a in ("-p", crate["name"])]], os.path.join(workspace, inside),
                       {"CARGO_TARGET_DIR": os.path.join(scratch, "miri"),
                        "MIRIFLAGS": "-Zmiri-strict-provenance -Zmiri-symbolic-alignment-check"})
    if code != 0:
        lines = output.strip().splitlines() or ["no output"]
        found.append("Miri fails: " + next((line for line in lines if "Undefined Behavior" in line or
                                            line.startswith("error")), lines[-1])[:300])
    return found


def fail_problems(workspace, scratch, crates):
    """Returns how a member's compile-fail cases go wrong: the member's fail folder is a workspace whose members are
    exactly its case folders, locking no registry crate the workspace's Cargo.lock doesn't lock alike, and each case
    must fail `cargo check` with every error holding one of its texts in the folder's expected.tsv and each text found,
    so that a case failing for another reason fails."""
    lock = lock_packages(workspace)
    found = []
    for crate in crates:
        folder = os.path.join(crate["folder"], "fail")
        if not os.path.isdir(folder):
            continue
        relative = posix_relative(folder, workspace)
        code, output = run(["cargo", "metadata", "--format-version", "1", "--locked", "--no-deps"], folder,
                           stdout_only=True)
        if code != 0:
            found.append(f"{relative} isn't a workspace that resolves with its Cargo.lock")
            continue
        cases = {os.path.basename(os.path.dirname(p["manifest_path"])): p["name"]
                 for p in json.loads(output)["packages"]}
        folders = {d for d in os.listdir(folder) if os.path.isdir(os.path.join(folder, d)) and d != "target"}
        found += [f"{relative}/{d} isn't a member of its cases workspace" for d in sorted(folders - set(cases))]
        found += [f"{relative} locks {name} {version}, which the workspace's Cargo.lock doesn't lock alike"
                  for (name, version), locked in lock_packages(folder).items()
                  if locked[0] != "path" and lock.get((name, version)) != locked]
        expected = {}
        try:
            with open(os.path.join(folder, "expected.tsv"), encoding="utf-8") as f:
                for line in f:
                    if line.strip() and not line.startswith("#"):
                        case, text = line.rstrip("\n").split("\t", 1)
                        expected.setdefault(case, []).append(text)
        except (OSError, ValueError):
            found.append(f"{relative}/expected.tsv is missing or malformed")
            continue
        found += [f"{relative}/expected.tsv names {case}, which isn't a case"
                  for case in sorted(set(expected) - set(cases))]
        target = os.path.join(scratch, "fail", crate["name"])
        for case, name in sorted(cases.items()):
            if case not in expected:
                found.append(f"{relative}/{case} has no expected error in expected.tsv")
                continue
            code, output = run(["cargo", "check", "--locked", "-q", "-p", name], folder, {"CARGO_TARGET_DIR": target})
            errors = [line for line in output.splitlines() if line.startswith("error")
                      and not line.startswith(("error: could not compile", "error: aborting due to"))]
            if code == 0:
                found.append(f"{relative}/{case} compiles, and a compile-fail case must not")
            found += [f"{relative}/{case} fails without {text!r}" for text in expected[case]
                      if not any(text in error for error in errors)]
            found += [f"{relative}/{case} fails with an unexpected {error[:160]}" for error in errors
                      if not any(text in error for text in expected[case])]
    return found


def graph_problems(crates):
    """Returns each crate ported from Doltgrust (a member outside FORK_CRATES) that reaches pg or shmem through the
    workspace's dependencies, its own of any kind and theirs normal or build ones (cargo never builds a dependency's
    dev-dependencies), since the storage crates reach Postgres only through host (design item 14's B7), but for the one
    path the owner allowed (2026-11-09): a dev-dependency on sim, which may reach pg (for libzstd and the stand-ins) and
    never shmem."""
    by_name = {crate["name"]: crate for crate in crates}

    def reached(start):
        seen, todo = set(), list(start)
        while todo:
            name = todo.pop()
            if name not in seen:
                seen.add(name)
                todo += list(by_name[name]["link_deps"]) if name in by_name else []
        return seen

    found = []
    for crate in crates:
        if crate["name"] in FORK_CRATES:
            continue
        through_sim = "sim" in crate["deps"] - crate["link_deps"]
        seen = reached(crate["deps"] - {"sim"} if through_sim else crate["deps"])
        found += [f"{crate['name']} reaches {name}, which only host's implementation may use" for name in
                  sorted(seen & {"pg", "shmem"})]
        if through_sim and "shmem" in reached({"sim"}):
            found.append(f"{crate['name']}'s tests reach shmem through sim, which may reach only pg")
    return found


def pending_problems(workspace, crates):
    """Returns each check pending.tsv lists that the tree needs before the check is live."""
    names = {crate["name"] for crate in crates}
    texts = None
    found = []
    for check, trigger, piece in read_table("pending.tsv"):
        kind, _, value = trigger.partition(":")
        if kind == "crate":
            needed = value in names
        elif kind == "members":
            needed = bool(names - {"doltgres"})
        elif kind == "ported":
            needed = bool(names - FORK_CRATES)
        elif kind == "file":
            needed = os.path.exists(os.path.join(workspace, value))
        elif kind == "table":
            needed = bool(read_table(value))
        else:
            if texts is None:
                texts = [open(p, errors="replace", encoding="utf-8").read() for p in source_files(workspace, workspace=True)]
            needed = any(re.search(value, t) for t in texts)
        if needed:
            found.append(f"the tree needs {check}, which isn't live yet (due before {piece})")
    return found


def tool_problems(workspace, target, features):
    """Returns how rustfmt, clippy with warnings as errors, and the workspace's tests fail."""
    found = []
    env = {"CARGO_TARGET_DIR": target, **BOOTSTRAP}
    for label, args in (("rustfmt", ["cargo", "fmt", "--all", "--check"]),
                        ("clippy", ["cargo", "clippy", "--locked", "--workspace", "--all-targets", *features,
                                    "--profile", "cassert", "--", "-D", "warnings"]),
                        ("the workspace's tests", ["cargo", "test", "--locked", "--workspace", *features, "--profile",
                                                   "cassert"])):
        code, output = run(args, workspace, env)
        if code != 0:
            found.append(f"{label} fails: " + (output.strip().splitlines() or ["no output"])[-1][:300])
    return found


FRONTEND_CFG = re.compile(r'^rustflags\s*=\s*\["--cfg",\s*"doltgres_frontend"\]\s*$', re.M)
CFG_SET = re.compile(r'rustc-cfg=doltgres_frontend|"--cfg"\s*,\s*"doltgres_frontend"|--cfg[= ]doltgres_frontend')
C_KEYWORDS = {"if", "while", "for", "return", "sizeof", "switch"}
CALLED = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def frontend_folders():
    """Returns the second workspaces frontend.list names, each a folder under the workspace."""
    return [row[0] for row in read_table("frontend.list")]


def out_dirs(cwd, target, crates):
    """Records on each crate with a build script the folder a build inside `cwd` gave its script's output, for the
    contract check, as the symbol check does for the root's build."""
    _, output = run(["cargo", "build", "--locked", *[a for crate in crates for a in ("-p", crate["name"])],
                     "--profile", "cassert", "--message-format=json"], cwd, {"CARGO_TARGET_DIR": target, **BOOTSTRAP},
                    stdout_only=True)
    for line in output.splitlines():
        try:
            message = json.loads(line)
        except ValueError:
            continue
        if message.get("reason") == "build-script-executed":
            for crate in crates:
                if package_folder(message.get("package_id", "")) == os.path.normcase(os.path.normpath(crate["folder"])):
                    crate["out_dir"] = message.get("out_dir")


def cfg_problems(workspace):
    """Returns each committed file outside verify that sets the frontend's cfg (a `--cfg doltgres_frontend` flag or a
    build script's `rustc-cfg`) but a listed second workspace's own .cargo/config.toml, its one definition, so no
    feature, build script, or flag of the root's turns it on and no build of the frontend sets it a second way."""
    code, listed = run(["git", "ls-files", "-z"], workspace, stdout_only=True)
    if code != 0:
        return ["the frontend's cfg can't be checked: git can't list the workspace's files"]
    allowed = {f"{folder}/.cargo/config.toml" for folder in frontend_folders()}
    found = []
    for name in sorted(n for n in listed.split("\0") if n and n.split("/")[0] != "verify" and n not in allowed):
        try:
            with open(os.path.join(workspace, name), encoding="utf-8") as f:
                text = f.read()
        except (OSError, UnicodeDecodeError):
            continue
        if CFG_SET.search(text):
            found.append(f"{name} sets the frontend's cfg, whose one definition is a listed workspace's "
                         ".cargo/config.toml")
    return found


def crate_texts(crates):
    """Returns each crate's Rust sources by its name, leaving out those of the crates nested in its folder."""
    folders = [crate["folder"] for crate in crates]
    return {crate["name"]: [open(p, encoding="utf-8").read() for p in source_files(crate["folder"])
                            if not any(f != crate["folder"] and f.startswith(crate["folder"] + os.sep)
                                       and p.startswith(f + os.sep) for f in folders)]
            for crate in crates}


def calls_problems(workspace, crates):
    """Returns each row of pg's calls.list and test-calls.list whose wrapper nothing calls: no other crate names it
    through `pg::` and pg's own code calls it nowhere, so a row that a merge of the lists kept after its last caller
    went is refused."""
    texts = {name: [code_only(text) for text in sources] for name, sources in crate_texts(crates).items()}
    found = []
    for listing in ("calls.list", "test-calls.list"):
        path = os.path.join(workspace, "pg", listing)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            names = [line.split("\t")[0] for line in f if line.strip() and not line.startswith("#")]
        for name in names:
            called = re.compile(rf"\bpg::{re.escape(name)}\b(?!::)")
            own = re.compile(rf"(?<![\w:])(?<!fn ){re.escape(name)}\s*\(")
            if not (any(called.search(text) for crate, sources in texts.items() if crate != "pg" for text in sources)
                    or any(own.search(text) for text in texts.get("pg", []))):
                found.append(f"{listing}'s {name} isn't called by any crate")
    return found


def wrapper_problems(workspace, folder, scratch, crates):
    """Returns how pg's frontend-calls.list goes unchecked in a second workspace: every name a row's C calls (an
    identifier before an opening parenthesis, but C's keywords) is a callee frontend-callees.tsv lists, so no wrapper
    reaches what exits or allocates through common's pg_malloc family; each row's wrapper is called through `pg::` by
    fehost and by no other crate the workspace builds but pg; and each row's test is a test of pg's own whose run
    inside the workspace fails when that row's C is made to fail, with the wrapper's result -1 and errno EIO."""
    listing = os.path.join(workspace, "pg", "frontend-calls.list")
    if not os.path.isfile(listing):
        return [f"{folder}: pg has no frontend-calls.list"]
    with open(listing, encoding="utf-8") as f:
        lines = f.read().splitlines(keepends=True)
    rows = [(at, line.rstrip("\n").split("\t")) for at, line in enumerate(lines)
            if line.strip() and not line.startswith("#")]
    callees = {row[0] for row in read_table("frontend-callees.tsv")}
    found = []
    for _, row in rows:
        if len(row) != 6:
            found.append(f"frontend-calls.list's row {row[0]} doesn't have six fields")
            continue
        body = re.sub(r'"(?:\\.|[^"\\])*"' + r"|'(?:\\.|[^'\\])'", '""', row[4])
        found += [f"frontend-calls.list's {row[0]} calls {name}, which frontend-callees.tsv doesn't list"
                  for name in sorted({m.group(1) for m in CALLED.finditer(body)} - C_KEYWORDS - callees)]
    texts = crate_texts(crates)
    for _, row in rows:
        call = re.compile(rf"\bpg::{re.escape(row[0])}\s*\(")
        own = re.compile(rf"(?<![\w:])(?<!fn ){re.escape(row[0])}\s*\(")
        if not (any(call.search(text) for text in texts.get("fehost", []))
                or any(own.search(code_only(text)) for text in texts.get("pg", []))):
            found.append(f"frontend-calls.list's {row[0]} isn't called by fehost or by pg's own frontend code")
        found += [f"{name} calls pg::{row[0]}, which only fehost may call" for name, sources in sorted(texts.items())
                  if name not in ("fehost", "pg") and any(call.search(text) for text in sources)]
    copy = os.path.join(scratch, "wrapper-canary")
    shutil.rmtree(copy, ignore_errors=True)
    shutil.copytree(workspace, copy, ignore=shutil.ignore_patterns("target", ".git"))
    target = os.path.join(scratch, "wrapper-canary-target")
    for at, row in rows:
        if len(row) != 6:
            continue
        test = re.fullmatch(r"tests::(\w+)", row[5])
        if not test:
            found.append(f"frontend-calls.list's {row[0]} names no test of pg's own: {row[5][:80]}")
            continue
        broken = lines[:at] + ["\t".join(row[:4] + ["errno = EIO; result = -1;", row[5]]) + "\n"] + lines[at + 1:]
        with open(os.path.join(copy, "pg", "frontend-calls.list"), "w", encoding="utf-8") as f:
            f.write("".join(broken))
        code, output = run(["cargo", "test", "--locked", "-p", "pg", "--lib", "--profile", "cassert", "--",
                            "--exact", row[5]], os.path.join(copy, folder), {"CARGO_TARGET_DIR": target, **BOOTSTRAP})
        if code == 0 and re.search(r"\b1 passed\b", output):
            found.append(f"frontend-calls.list's {row[0]}: {row[5]} passes with the wrapper's C made to fail")
        elif code == 0 or "panicked" not in output:
            found.append(f"frontend-calls.list's {row[0]}: {row[5]} didn't run and fail by its assertion with the "
                         "wrapper's C made to fail")
    return found


def frontend_tool_problems(workspace, sub, target, own, changed):
    """Returns how rustfmt, clippy, and the tests fail for a second workspace: rustfmt and clippy over every target of
    its members inside it; clippy over the library of each root package whose code its cfg changes from the root,
    given the second workspace's .cargo/config.toml (clippy lints only the members of the workspace it runs in, and the
    file stays the cfg's one definition); and the tests of its members and of each such package that has no
    dev-dependencies, the only ones cargo tests outside their own workspace (pg, whose unsafe the cfg changes, is
    one), inside it; the other packages' frontend code is run by the members' tests."""
    found = []
    env = {"CARGO_TARGET_DIR": target, **BOOTSTRAP}
    members_args = [a for crate in own for a in ("-p", crate["name"])]
    libs = [a for crate in changed for a in ("-p", crate["name"])]
    tested = [a for crate in changed if crate["deps"] == crate["link_deps"] for a in ("-p", crate["name"])]
    for label, args in (("rustfmt", ["cargo", "fmt", "--all", "--check"]),
                        ("clippy", ["cargo", "clippy", "--locked", *members_args, "--all-targets", "--profile",
                                    "cassert", "--", "-D", "warnings"]),
                        ("clippy over the root's crates", ["cargo", "clippy", "--locked", *libs, "--lib", "--profile",
                                                           "cassert", "--config", os.path.join(sub, ".cargo",
                                                                                                "config.toml"),
                                                           "--", "-D", "warnings"]),
                        ("the workspace's tests", ["cargo", "test", "--locked", *members_args, *tested, "--profile",
                                                   "cassert"])):
        if label == "clippy over the root's crates" and not libs:
            continue
        code, output = run(args, workspace if label == "clippy over the root's crates" else sub, env)
        if code != 0:
            found.append(f"{label} fails: " + (output.strip().splitlines() or ["no output"])[-1][:300])
    return found


def lock_problems(workspace, folder):
    """Returns each registry package a second workspace's Cargo.lock locks that the root's doesn't lock alike, by
    version, source, and checksum, so the frontend build links only what the root's supply-chain checks vet."""
    lock = lock_packages(workspace)
    return [f"{folder}/Cargo.lock locks {name} {version}, which the root's Cargo.lock doesn't lock alike"
            for (name, version), locked in sorted(lock_packages(os.path.join(workspace, folder)).items())
            if locked[0] != "path" and lock.get((name, version)) != locked]


def frontend_problems(workspace, scratch):
    """Returns the problems of each second workspace frontend.list names (design item D12's frontend build, whose cfg
    can't share a cargo run with the server's): its .cargo/config.toml sets the cfg; its Cargo.lock locks every package
    the root's locks alike; its crates pass the graph, crate, supply-chain, and compile-fail checks, and no test-only
    feature reaches its own crates' builds; and rustfmt, clippy, the tests, and the unsafe, door, lint, and Miri checks
    run inside its folder, where cargo reads that file, over its members and the root's path packages whose code the
    cfg changes, with pg's frontend wrappers checked."""
    found = []
    for folder in frontend_folders():
        sub = os.path.join(workspace, folder)
        work = os.path.join(scratch, "frontend", folder)
        target = os.path.join(work, "target")
        config = os.path.join(sub, ".cargo", "config.toml")
        if not os.path.isfile(os.path.join(sub, "Cargo.toml")):
            found.append(f"frontend.list names {folder}, which holds no Cargo workspace")
            continue
        with open(config, encoding="utf-8") if os.path.isfile(config) else io.StringIO("") as f:
            if not FRONTEND_CFG.search(f.read()):
                found.append(f"{folder}/.cargo/config.toml doesn't set the frontend's cfg")
        found += lock_problems(workspace, folder)
        try:
            crates, others = members(sub, target, paths=True)
        except (RuntimeError, ValueError) as error:
            found.append(f"{folder}: {error}")
            continue
        own = [crate for crate in crates if not os.path.relpath(crate["folder"], sub).startswith("..")]
        changed = [crate for crate in crates if crate not in own and any(
            "doltgres_frontend" in open(path, encoding="utf-8").read() for path in source_files(crate["folder"]))]
        chosen = own + changed
        found += [f"{folder}: {problem}" for problem in
                  graph_problems(crates) + crate_problems(sub, others) + supply_problems(sub)
                  + [problem for crate in own for problem in testing_problems(sub, crates, crate["name"])]
                  + fail_problems(sub, work, own)]
        out_dirs(sub, target, chosen)
        found += [f"{folder}: {problem}" for problem in
                  frontend_tool_problems(workspace, sub, target, own, changed)
                  + unsafe_problems(workspace, work, chosen, [], folder)
                  + canary_problems(workspace, work, own, [], folder)
                  + canary_problems(workspace, work, changed, ["--config", config])
                  + lint_problems(workspace, work, chosen, [], folder)
                  + miri_problems(workspace, work, chosen, [], folder)
                  + wrapper_problems(workspace, folder, work, chosen)]
    return found + cfg_problems(workspace)


def problems(workspace, scratch):
    """Returns every problem the gates find in a workspace; the checks that compile it run once with the default
    features, the server's build, and once with every feature, which test-only code sits behind, each problem only the
    second run finds marked so."""
    target = os.path.join(scratch, "target")
    try:
        crates, others = members(workspace, target)
    except (RuntimeError, ValueError) as error:
        return [str(error)]
    found = [f"{crate['name']} is a member under verify, whose files the text gate doesn't read" for crate in crates
             if os.path.relpath(crate["folder"], workspace).split(os.sep)[0] == "verify"]
    found += (graph_problems(crates) + assertion_problems(workspace, crates) + doltgrust_problems(workspace, crates)
              + closure_problems(crates) + testing_problems(workspace, crates)
              + pending_problems(workspace, crates) + calls_problems(workspace, crates)
              + text_problems(workspace) + pins_problems(workspace) + copy_problems(workspace)
              + crate_problems(workspace, others))
    found += supply_problems(workspace) + verus_problems(workspace, scratch, crates)
    found += fail_problems(workspace, scratch, crates)
    first = None
    for features in ([], ["--all-features"]):
        compiled = (tool_problems(workspace, target, features) + unsafe_problems(workspace, scratch, crates, features)
                    + canary_problems(workspace, scratch, crates, features)
                    + symbol_problems(workspace, target, crates, features)
                    + lint_problems(workspace, scratch, crates, features)
                    + miri_problems(workspace, scratch, crates, features))
        found += compiled if first is None else [f"{p} (with every feature)" for p in compiled if p not in first]
        first = compiled
    return found + linked_problems(workspace, scratch, crates) + frontend_problems(workspace, scratch)


def main():
    """Runs the gates over the workspace given and prints what they find."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    workspace, scratch = (os.path.abspath(a) for a in sys.argv[1:3])
    found = problems(workspace, scratch)
    for problem in found:
        print(problem)
    print("the Rust gates pass" if not found else f"the Rust gates fail: {len(found)} problems")
    sys.exit(1 if found else 0)


if __name__ == "__main__":
    main()
