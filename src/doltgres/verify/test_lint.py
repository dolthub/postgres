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

"""test_lint.py <scratch folder>: checks the typed lint under lint, the gates' rustc driver, as the queue checks the
gates' own code when a ref changes them: rustfmt and clippy with warnings as errors over it, and its synthetic cases."""

import os
import subprocess
import sys

LINT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lint")


def main():
    """Runs each check and fails when any does."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    scratch = os.path.abspath(sys.argv[1])
    env = dict(os.environ, RUSTC_BOOTSTRAP="1", CARGO_TARGET_DIR=os.path.join(scratch, "lint-clippy"))
    failed = False
    for label, args in (("rustfmt", ["cargo", "fmt", "--check"]),
                        ("clippy", ["cargo", "clippy", "--locked", "--all-targets", "--", "-D", "warnings"]),
                        ("the lint's cases", [sys.executable, os.path.join(LINT, "run_cases.py"), scratch])):
        done = subprocess.run(args, cwd=LINT, env=env, capture_output=True, text=True, encoding="utf-8")
        if done.returncode != 0:
            failed = True
            print(f"{label} fails:\n{done.stdout}{done.stderr}")
    print("the lint's checks fail" if failed else "the lint's checks pass")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
