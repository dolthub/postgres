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

"""link_threads.py <meson build folder>: the thread check of PLAN's Threads bullet. It relinks the build's postgres
with the linker reporting what keeps each thread-creation symbol live, and fails unless every object that reaches one
is the vendored zstd's own thread pool (lib/common/pool.c or threading.c, design item 15), so nothing else in the server
starts a thread. An import table can't say which object pulled a symbol in, and Rust's standard library references
pthread_create from objects the link then strips, so the check asks the linker what it kept."""

import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

NINJA = os.environ.get("NINJA", shutil.which("ninja") or "ninja")
THREADS = {"darwin": ["_pthread_create"]}
ALLOWED = re.compile(r"/third_party/zstd/.*\b(?:lib_common_)?(?:pool|threading)\.c\.o\)?$")


def thread_problems(link, cwd):
    """Returns each object that keeps a thread-creation symbol live in a link (its argument list, run in cwd), but the
    vendored zstd's thread pool, or why the check can't run on this platform or linker."""
    symbols = THREADS.get(sys.platform)
    if symbols is None:
        return [f"the thread check has no linker report for {sys.platform} yet, so it can't run here"]
    args = list(link)
    with tempfile.TemporaryDirectory() as scratch:
        if "-o" in args:
            args[args.index("-o") + 1] = os.path.join(scratch, "postgres")
        args += [f"-Wl,-why_live,{symbol}" for symbol in symbols]
        done = subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8")
    if done.returncode != 0:
        return ["the relink for the thread check fails: " + (done.stderr.strip().splitlines() or ["no output"])[-1][:300]]
    found, lines = [], (done.stdout + done.stderr).splitlines()
    for i, line in enumerate(lines):
        symbol = line.split(" from ", 1)[0]
        if symbol not in symbols or line.startswith(" "):
            continue
        referrer = lines[i + 1].strip() if i + 1 < len(lines) and lines[i + 1].startswith("  ") else ""
        source = referrer.split(" from ", 1)[1] if " from " in referrer else referrer
        if not ALLOWED.search(source):
            found.append(f"{symbol} is kept live by {referrer or 'nothing the linker names'}, outside the vendored zstd's"
                         " thread pool")
    return sorted(set(found))


def main():
    """Checks the postgres link of the meson build folder given, printing what it finds."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    build = os.path.abspath(sys.argv[1])
    target = "src/backend/postgres" + (".exe" if os.name == "nt" else "")
    done = subprocess.run([NINJA, "-C", build, "-t", "commands", target], capture_output=True, text=True, encoding="utf-8")
    commands = done.stdout.strip().splitlines()
    if done.returncode != 0 or not commands:
        print("the thread check can't find postgres' link command")
        sys.exit(1)
    found = thread_problems(shlex.split(commands[-1]), build)
    for problem in found:
        print(problem)
    print("the thread check passes" if not found else f"the thread check fails: {len(found)} problems")
    sys.exit(1 if found else 0)


if __name__ == "__main__":
    main()
