# Rebase, stash, query diffs, help and stored tests

Reference: Dolt `go/libraries/doltcore/sqle/dprocedures/{dolt_rebase,dolt_stash}.go`,
`rebase/rebase.go`, `sqle/dtables/{stashes_table,help_table,tests_table}.go`,
`sqle/dtablefunctions/{dolt_query_diff,dolt_test_run}.go`; Doltgres
`testing/go/dolt_tables_test.go` ("dolt rebase"), `auth_test.go`.

## dolt_rebase and the dolt.rebase table

`dolt_rebase('-i', upstream)` requires a clean working set and no pending
merge or rebase. It records the commits reachable from HEAD but not from the
upstream (oldest first) as `pick` steps in `pg_catalog.pg_dolt_rebase`, keyed
by the temporary branch `dolt_rebase_<branch>` that it creates at the
upstream commit and switches the session to, and stores the original branch
and upstream in a `rebase/<temporary branch>` manifest ref (object kind
`rebase`). The plan is visible as `dolt.rebase`, `dolt_rebase` and
`schema.dolt_rebase` (`rebase_order float8, action, commit_hash,
commit_message`), updatable through the auto-updatable views; the parser
guard `rebase_table_guard` reports "table not found" whenever the session
branch has no active rebase, as Dolt's working-set table does.

`dolt_rebase('--continue')` validates the plan (ascending order; `squash`
and `fixup` must follow a `pick` or `reword`; `edit` is not supported) and
applies it on the temporary branch with the cherry-pick machinery
(`install_database` of parent → commit onto the current head): `pick`
re-commits with the original message and author, `reword` uses the plan's
message, `drop` skips, `fixup` amends the previous commit, `squash` amends
it joining both messages with a blank line. Steps that become empty are
dropped. A conflicting step raises "data conflict detected while rebasing
commit …"; resolve by aborting (there is no conflict pause). On success the
original branch's refs move to the rebased head, its live tables are
restored from that state (`restore_table_on`), the session returns to it,
and the temporary branch and plan are deleted. `--abort` returns to the
original branch unchanged. Results are `(status, message)` records with
Dolt's wording.

## dolt_stash and dolt.stashes

`dolt_stash(push|pop|apply|drop|clear, name [, index])`. `push` captures the
working set, stores a `stash` object (children: stashed working database,
staged database, head commit; payload with branch, head and the `WIP on
<branch>: <hash7> <message>` text) under `stash/<name>/<seq>`, and restores
the tracked tables to HEAD. Only tracked tables are stashed; untracked
tables stay in place. `pop`/`apply` merge the stash three-way onto the
current working set (base: the stash's head) and error with Dolt's "would be
overwritten" message on conflicts; `pop` and `drop` remove the entry,
`clear` removes every entry of a name. `dolt.stashes` / `dolt_stashes` list
`name, stash_id (stash@{n}, newest first), branch, hash, commit_message`.

## dolt_query_diff

Both query strings are analyzed at parse time (`query_diff_layout`) so the
result has `from_<col>…, to_<col>…, diff_type`. Both queries run with the
caller's search path. Direct single-table projections containing a complete,
nondeferrable primary key pair rows by that key and report `modified` when
other output values differ. Key definitions come from each query's bound
revision. Different logical table names produce separate deletions/additions,
without rename fingerprints or persistent column tags.

Other results use whole-row multiset comparison: unmatched rows are `deleted`
or `added`, identical rows cancel, and duplicate multiplicity is preserved.
This fallback includes joins, aggregates, set operations, incomplete key
projections and inheritance scans (a parent's key does not constrain its
children). Typed, length-framed text values distinguish embedded delimiters,
NULLs and empty strings. Generic query lineage through views/subqueries is not
yet inferred.

## dolt.help

`dolt_help()` returns a static catalog (`name, type, synopsis,
short_description, long_description, arguments json`) of the implemented
procedures and functions, exposed as `dolt.help` / `dolt_help`.

## dolt.tests and dolt_test_run

`public.dolt_tests` (ordinary versioned prolly storage) backs `dolt.tests`
/ `dolt_tests` with Dolt's columns and check constraints on assertion types
(`expected_rows`, `expected_columns`, `expected_single_value`) and
comparators. `dolt_test_run([name | group | '*'])` runs each query read-only
in a subtransaction with the caller's search path and reports `PASS`/`FAIL`
with Dolt's messages ("Assertion failed: …", "Query error: …",
"expected_single_value expects exactly one cell …"); numeric values compare
numerically, others as text. An unknown selector errors with "could not find
tests for argument".

Tests: `src/test/modules/test_prolly/t/024_rebase_stash.pl`; Doltgres
`TestUserSpaceDoltTables/dolt_rebase` under the postgres profile.
