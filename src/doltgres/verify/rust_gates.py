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
"""rust_gates.py <workspace> <scratch folder>: the queue's gates over the fork's Cargo workspace, which holds only the
Rust that tests the C (host, pg, sim, and simc) and Doltgrust's copied tests: the test Rust's build hygiene, its crates'
vetting, the vendored and copied files' pins, and the C's own lists (calls.list's callers, frontend-calls.list's
callees, tokens.tsv, and pending.tsv), printing each problem and failing when there is one. Every gate fails closed: a
tool that can't run, or output it can't read, is a problem. The tables beside this file change only through the
coordinator's refs."""

import collections
import difflib
import hashlib
import json
import os
import posixpath
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SUPPLY_TOOLS = {"deny": "cargo-deny 0.20.2", "vet": "cargo-vet 0.10.3"}
DOLTGRUST_COPY = "test/doltgrust"  # Doltgrust's tests, copied byte-identical (the owner, 2026-11-15)
VENDORED = {"third_party", DOLTGRUST_COPY}  # the folders of others' code, which only the pins gates read
COPY_OWN = {"Cargo.lock", "Cargo.toml", "README.md", "clippy.toml", "pins.tsv", "rust-toolchain.toml"}  # the copy's own
COPY_REPLACED = {"crates/harness/Cargo.toml", "crates/harness/src/order.rs", "crates/harness/src/pgx/stream.rs",
                 "crates/harness/src/prepared.rs"}  # the fork's replacements of Doltgrust's files, its README says why
C_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
C_KEYWORDS = {"if", "while", "for", "return", "sizeof", "switch"}
CALLED = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def read_table(name):
    """Returns a table's rows beside this file, each a list of its tab-separated fields, comments and blanks left out."""
    with open(os.path.join(HERE, name), encoding="utf-8") as f:
        return [line.rstrip("\n").split("\t") for line in f if line.strip() and not line.startswith("#")]


def run(args, cwd, env=None, stdout_only=False):
    """Runs a command and returns (exit status, its output, or its standard output alone when asked)."""
    try:
        done = subprocess.run(args, cwd=cwd, env={**os.environ, **(env or {})}, capture_output=True, text=True, encoding="utf-8")
    except OSError as error:
        return 127, str(error)
    return done.returncode, done.stdout if stdout_only else done.stdout + done.stderr


def members(workspace, target):
    """Returns the workspace's members, each a dict of its name and folder."""
    code, output = run(["cargo", "metadata", "--format-version", "1", "--locked", "--no-deps"], workspace,
                       {"CARGO_TARGET_DIR": target}, stdout_only=True)
    if code != 0:
        raise RuntimeError(f"cargo metadata failed with exit status {code}")
    return [{"name": package["name"], "folder": os.path.dirname(package["manifest_path"])}
            for package in json.loads(output)["packages"]]


def ported_diff(old, new):
    """Returns the SHA-256 of a ported file's unified diff from Doltgrust's bytes to the fork's, as doltgrust.tsv
    records it: Python's difflib with the labels a and b and three lines of context, over lines that keep their
    endings."""
    lines = difflib.unified_diff(old.decode().splitlines(True), new.decode().splitlines(True), "a", "b")
    return hashlib.sha256("".join(lines).encode()).hexdigest()


def doltgrust_problems(workspace):
    """Returns how doltgrust.tsv fails to account for the files copied from Doltgrust into the fork, every row checked,
    read from git on both sides, Doltgrust's checkout (DOLTGRUST_DIR) only read: each file has one row; a same row's
    bytes are equal; a changed row's diff is the one recorded; an added row is the fork's alone; and a removed row is
    Doltgrust's alone."""
    table = os.path.join(workspace, "doltgrust.tsv")
    if not os.path.exists(table):
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


def tokens_problems():
    """Returns each row of tokens.tsv whose compile-fail case has no C form yet, its form still `outstanding`, so the
    port lands only once every case the Rust's compiler refuses has its C counterpart."""
    if not os.path.exists(os.path.join(HERE, "tokens.tsv")):
        return []
    return [f"tokens.tsv: {row[1]} is still outstanding" for row in read_table("tokens.tsv")
            if len(row) > 3 and row[3] == "outstanding"]


def c_texts(workspace):
    """Returns the committed C sources and headers of the fork (under the workspace and, beside it, include/doltgres,
    the vendored folders left out), each with its comments blanked out."""
    include = os.path.join("..", "include", "doltgres")
    code, listed = run(["git", "ls-files", "-z", "--", "*.c", "*.h",
                        *([include] if os.path.isdir(os.path.join(workspace, include)) else [])], workspace,
                       stdout_only=True)
    if code != 0:
        raise RuntimeError("git can't list the fork's C files")
    texts = []
    for name in listed.split("\0"):
        if (name.endswith((".c", ".h")) and name.split("/")[0] not in VENDORED
                and not name.startswith(DOLTGRUST_COPY + "/")):
            with open(os.path.join(workspace, name), errors="replace", encoding="utf-8") as f:
                texts.append(C_COMMENT.sub(" ", f.read()))
    return texts


def calls_problems(workspace, texts):
    """Returns each row of pg's calls.list and test-calls.list whose wrapper's C form, pg_calls_<name>, the fork's C
    never names, so a row that a merge of the lists kept after its last caller went is refused."""
    found = []
    joined = "\n".join(texts)
    for listing in ("calls.list", "test-calls.list"):
        path = os.path.join(workspace, "pg", listing)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            names = [line.split("\t")[0] for line in f if line.strip() and not line.startswith("#")]
        found += [f"{listing}'s {name} isn't called by the C" for name in names
                  if not re.search(rf"\bpg_calls_{re.escape(name)}\b", joined)]
    return found


def callee_problems(workspace):
    """Returns each name a row's C in pg's frontend-calls.list calls (an identifier before an opening parenthesis, but
    C's keywords) that frontend-callees.tsv doesn't list, so no wrapper reaches what exits or allocates through
    common's pg_malloc family, and each row without its six fields."""
    listing = os.path.join(workspace, "pg", "frontend-calls.list")
    if not os.path.isfile(listing):
        return []
    with open(listing, encoding="utf-8") as f:
        rows = [line.rstrip("\n").split("\t") for line in f if line.strip() and not line.startswith("#")]
    callees = {row[0] for row in read_table("frontend-callees.tsv")}
    found = []
    for row in rows:
        if len(row) != 6:
            found.append(f"frontend-calls.list's row {row[0]} doesn't have six fields")
            continue
        body = re.sub(r'"(?:\\.|[^"\\])*"' + r"|'(?:\\.|[^'\\])'", '""', row[4])
        found += [f"frontend-calls.list's {row[0]} calls {name}, which frontend-callees.tsv doesn't list"
                  for name in sorted({m.group(1) for m in CALLED.finditer(body)} - C_KEYWORDS - callees)]
    return found


def pending_problems(workspace, texts):
    """Returns each check pending.tsv lists that the tree needs before the check is live."""
    found = []
    for check, trigger, piece in read_table("pending.tsv"):
        kind, _, value = trigger.partition(":")
        if kind == "file":
            needed = os.path.exists(os.path.join(workspace, value))
        elif kind == "table":
            needed = bool(read_table(value))
        else:
            needed = any(re.search(value, t) for t in texts)
        if needed:
            found.append(f"the tree needs {check}, which isn't live yet (due before {piece})")
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


def tool_problems(workspace, target, features):
    """Returns how rustfmt, clippy with warnings as errors, and the workspace's tests fail."""
    found = []
    env = {"CARGO_TARGET_DIR": target}
    for label, args in (("rustfmt", ["cargo", "fmt", "--all", "--check"]),
                        ("clippy", ["cargo", "clippy", "--locked", "--workspace", "--all-targets", *features,
                                    "--profile", "cassert", "--", "-D", "warnings"]),
                        ("the workspace's tests", ["cargo", "test", "--locked", "--workspace", *features, "--profile",
                                                   "cassert"])):
        code, output = run(args, workspace, env)
        if code != 0:
            found.append(f"{label} fails: " + (output.strip().splitlines() or ["no output"])[-1][:300])
    return found


def problems(workspace, scratch):
    """Returns every problem the gates find in a workspace; the checks that compile it run once with the default
    features and once with every feature, which test-only code sits behind, each problem only the second run finds
    marked so."""
    target = os.path.join(scratch, "target")
    try:
        crates = members(workspace, target)
        texts = c_texts(workspace)
    except (RuntimeError, ValueError) as error:
        return [str(error)]
    found = [f"{crate['name']} is a member under verify, which the gates don't read" for crate in crates
             if os.path.relpath(crate["folder"], workspace).split(os.sep)[0] == "verify"]
    found += (doltgrust_problems(workspace) + calls_problems(workspace, texts) + callee_problems(workspace)
              + pending_problems(workspace, texts) + pins_problems(workspace) + copy_problems(workspace)
              + tokens_problems() + supply_problems(workspace))
    first = None
    for features in ([], ["--all-features"]):
        compiled = tool_problems(workspace, target, features)
        found += compiled if first is None else [f"{p} (with every feature)" for p in compiled if p not in first]
        first = compiled
    return found


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
