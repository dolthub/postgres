# Bats tests

Each test runs in its own data directory, `$BATS_TMPDIR/dolt-repo-$$`, which `setup_common` makes the working directory
and `DOLTGRUST_DATA_DIR`. `teardown_common` stops the test's server, waits for it to exit, and checks the data directory
with `doltgrust fsck` when a server bootstrapped it: a finding, or a directory fsck can't check, fails the test with
fsck's lines. Files a crash leaves are settled lines, which pass. A server the test stopped without waiting gets up to
60 seconds to exit before the check, and one that takes longer fails the test.

A test that removes or replaces its data directory on purpose sets `FSCK_DATA_DIR_REMOVED=1` to leave it unchecked. No
test does so now, and no test kills a server: every test stops its server with `stop_sql_server`, which sends SIGTERM.
