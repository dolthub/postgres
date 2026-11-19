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

"""run_cases.py <scratch folder>: builds the typed lint and runs it over its synthetic cases, failing when a case
reports other findings than cases/expected.tsv lists, a clean case reports any, or the lint runs over a workspace
without `pg`'s entry list."""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")


def expected():
    """Returns each case crate's expected findings, from cases/expected.tsv."""
    found = {}
    with open(os.path.join(CASES, "expected.tsv"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            crate, text = line.rstrip("\n").split("\t", 1)
            found.setdefault(crate, []).append(text)
    return found


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    scratch = os.path.abspath(sys.argv[1])
    env = dict(os.environ, RUSTC_BOOTSTRAP="1")
    lint_target = os.path.join(scratch, "lint")
    subprocess.run(["cargo", "build", "-q"], cwd=HERE, env=dict(env, CARGO_TARGET_DIR=lint_target), check=True)
    sysroot = subprocess.run(["rustc", "--print", "sysroot"], cwd=HERE, capture_output=True, text=True, encoding="utf-8",
                             check=True).stdout.strip()
    lib = os.path.join(sysroot, "lib")
    run_env = dict(env, RUSTC_WRAPPER=os.path.join(lint_target, "debug", "doltgres-lint"),
                   DOLTGRES_LINT_TABLES=CASES, DOLTGRES_WORKSPACE=CASES, DYLD_LIBRARY_PATH=lib, LD_LIBRARY_PATH=lib,
                   PATH=os.path.join(sysroot, "bin") + os.pathsep + os.environ["PATH"])
    wanted = expected()
    with open(os.path.join(CASES, "Cargo.toml"), encoding="utf-8") as f:
        members = [m.strip(' "') for m in f.read().split("members = [", 1)[1].split("]", 1)[0].split(",")]
    problems = []
    for crate in members:
        if crate in ("pg", "host", "smuggle", "vstd", "short"):
            continue
        target = os.path.join(scratch, "cases", crate)
        shutil.rmtree(target, ignore_errors=True)
        out = subprocess.run(["cargo", "check", "-q", "-p", crate], cwd=CASES, capture_output=True, text=True, encoding="utf-8",
                             env=dict(run_env, CARGO_TARGET_DIR=target))
        findings = [l for l in out.stderr.splitlines() if l.startswith("error: ") and "could not compile" not in l]
        for text in wanted.get(crate, []):
            if not any(text in f for f in findings):
                problems.append(f"{crate}: no finding containing {text!r}")
        for f in findings:
            if not any(text in f for text in wanted.get(crate, [])):
                problems.append(f"{crate}: unexpected {f}")
        if not findings and crate in wanted:
            problems.append(f"{crate}: reported nothing:\n{out.stderr}")
    empty = os.path.join(scratch, "empty")
    os.makedirs(empty, exist_ok=True)
    target = os.path.join(scratch, "cases", "missing")
    shutil.rmtree(target, ignore_errors=True)
    out = subprocess.run(["cargo", "check", "-q", "-p", "clean"], cwd=CASES, capture_output=True, text=True, encoding="utf-8",
                         env=dict(run_env, CARGO_TARGET_DIR=target, DOLTGRES_WORKSPACE=empty))
    if out.returncode == 0 or "doltgres-lint: cannot read" not in out.stderr:
        problems.append(f"a workspace without pg/entries.list didn't stop the lint:\n{out.stderr}")
    for p in problems:
        print(p)
    print("the lint's cases pass" if not problems else f"{len(problems)} problems")
    sys.exit(1 if problems else 0)


main()
