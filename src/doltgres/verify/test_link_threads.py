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

"""test_link_threads.py <work folder>: runs link_threads.py's check over synthetic links, one whose thread comes from
the vendored zstd's pool, one from another object, one with no thread, and one whose thread the link strips, and fails
unless only the second is refused. Runs whenever the gates change."""

import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link_threads  # noqa: E402

MAIN = "int start(void);\nint main(void) { return start(); }\n"
THREAD = ("#include <pthread.h>\nstatic void *run(void *a) { return a; }\n"
          "int start(void) { pthread_t t; return pthread_create(&t, 0, run, 0); }\n")
QUIET = "int start(void) { return 0; }\n"
STRIPPED = ("#include <pthread.h>\nstatic void *run(void *a) { return a; }\n"
            "int unused(void) { pthread_t t; return pthread_create(&t, 0, run, 0); }\n"
            "int start(void) { return 0; }\n")
CASES = {
    "the vendored zstd's pool": ("third_party/zstd/libzstd.a.p/lib_common_pool.c", THREAD, None),
    "another object": ("src/backend/thread.c", THREAD, "kept live by"),
    "no thread": ("src/backend/quiet.c", QUIET, None),
    "a thread the link strips": ("src/backend/stripped.c", STRIPPED, None),
}


def main():
    """Checks every case, printing each mismatch."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    work = os.path.abspath(sys.argv[1])
    failures = 0
    for name, (source, text, expected) in CASES.items():
        folder = os.path.join(work, name.replace(" ", "-").replace("'", ""))
        shutil.rmtree(folder, ignore_errors=True)
        objects = []
        for path, code in (("src/backend/main.c", MAIN), (source, text)):
            full = os.path.join(folder, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as f:
                f.write(code)
            subprocess.run(["cc", "-c", "-ffunction-sections", full, "-o", full + ".o"], check=True)
            objects.append(full + ".o")
        found = link_threads.thread_problems(["cc", "-o", "postgres", *objects, "-Wl,-dead_strip"], folder)
        if (expected is None and found) or (expected is not None and not any(expected in p for p in found)):
            failures += 1
            print(f"{name}: got {found}, expected {expected or 'no problems'}")
    print("thread check cases pass" if not failures else f"{failures} thread check cases fail")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
