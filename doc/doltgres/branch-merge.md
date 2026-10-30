# Branch checkout and data merge slice

This slice adds checkout, merge-base discovery, fast-forward/no-op and three-way
data merge over retained commit objects. PostgreSQL owns the atomic installation
of live data, rebuilt indexes, history objects, refs and pending merge metadata.

## Explicit compatibility decisions

A data merge conflict is a successful SQL function invocation with `conflicts=1`
in the Dolt-compatible `(hash, fast_forward, conflicts, message)` result. It must
not raise ERROR and abort the transaction. Persist the conflict evidence and
pending parents so the user can inspect, resolve, stage and commit. This
intentionally differs from the current Doltgres conflict-error behavior and its
skipped tests. Invalid arguments, unsupported operations and infrastructure
failures remain errors. Unresolved conflicts prevent a history commit.

Schema merges are excluded: differing namespace/table/index definitions in the
base/ours/theirs inputs must be rejected before installation. Primary-key rows
must merge by logical key, not row-content hash. Keyless tables track occurrence counts. Changes to the same row value on both
branches conflict, including identical cardinality changes, matching the Dolt
reference. Changes to different row values combine. Indexes derive from the resulting table contents and must pass
uniqueness validation; they are not independently merged.

The current standalone SELECT / SQL transaction boundary remains in force for
checkout and merge. Conflict metadata and pending merge state must survive restart
and abort/rollback consistently with live roots. Do not add a second publication
manifest or bypass PostgreSQL WAL.

## Branch binding and future cross-branch queries

This milestone requires independent session branch views. The selected session
branch supplies the default binding; it must not become the permanent identity
of every reference to a relation.

Ultimately a single query must be able to reference the same table on multiple
branches or retained commits, including joining those references. Resolve each
table reference to an explicit revision binding, carry that binding into its
plan and scan state, and select its schema and data roots under the appropriate
snapshot. Separate aliases of the same relation may have different bindings.
Index scans, tuple fetches and rescans must retain the binding of their own table
reference. A shared relcache entry or a backend-global branch variable alone
cannot represent this distinction. Cached plans must preserve explicit bindings
and revalidate default bindings when checkout changes the session context.

Cross-branch queries and revision-specific schema projection are outside this
milestone. The current relation lookup boundary is a transitional seam, not a
complete implementation of per-reference binding. Future writes must likewise
identify their target working set explicitly; retained commit views are read-only.


## Implemented command contract

- `active_branch()` returns the session default, initially `main`.
- `dolt_checkout('branch')` or `dolt_checkout('-b','branch'[, 'start'])`
  returns `(status bigint, message text)`. It changes the default on SQL commit;
  another session remains on its own branch. Uncommitted history working data
  and staging stay with their branch when switching away and back.
- `dolt_merge('revision'[, options...])` supports `--ff-only`, `--no-ff`,
  `-m/--message`, `--no-commit`, `--squash`, and standalone `--abort`.
  A fast-forward cannot be stopped by `--no-commit`; use `--no-ff` to retain a
  pending two-parent merge. Squash stages data without adding a second parent.
- `dolt_conflicts` summarizes pending conflict units by qualified table name.
  `dolt_merge_status` currently exposes only `is_merging`, not the full Dolt
  status schema. `dolt_conflict_rows('table')` is a prototype inspection API:
  packed key/base/ours/theirs bytea values plus `whole_table`.
- `dolt_conflicts_resolve('--ours'|'--theirs', 'table'[, ...])`, or `'.'`,
  installs the chosen values and removes their conflict markers. It can run
  inside a SQL transaction. Stage and commit afterward; manual edits can be
  made after choosing a side and before staging. Unresolved conflicts prevent
  `dolt_commit`. Dynamic `dolt_conflicts_<table>` relations are not implemented.

Checkout and merge require a standalone SELECT outside explicit transactions,
like commit. API authorization remains superuser-only. Mutation calls embedded
in scalar expressions are deliberately rejected under the agreed SQL transaction
contract. Commit hashes use 20-byte addresses rendered with Dolt's 32-character base-32 encoding. The underlying object format remains specific to this engine.

A merge requires clean working and staged contents. This conservative restriction
avoids overwriting unstaged work; reconciling dirty working sets is deferred to
row-level publication. Multiple best merge bases (criss-cross histories) are
explicitly rejected until recursive virtual bases are implemented.

## Storage and publication decisions

Relation root records now contain XID, command ID, canonical content root,
occurrence-handle root, next occurrence ID, and a 32-byte hash of the branch name.
Readers filter by branch before applying visibility and stale-writer checks.
Chunk pages are shared within the relation; canonical tuples and history objects
do not contain branch names, XIDs or TIDs. The changed physical record format
requires a fresh cluster; no in-place migration is provided.

A branch without local roots imports its retained table/index chunks from the
snapshot-visible branch HEAD. Occurrence handles are reconstructed from canonical
contents. Subsequent roots retain normal PostgreSQL snapshot visibility. Merge
installation allocates fresh handles, rebuilds indexes, and advances the command
counter after index writes so capture cannot serialize a stale index root.

SQL catalogs remain shared. Checkout checks definition identity and persists the
`views/enabled` marker. After activation, the utility guard rejects DDL and
TRUNCATE in that database. This intentionally broad prototype restriction also
covers unrelated schema operations; it is not branch-aware DDL. A database object
lock coordinates the guard and administrative branch operations without locking
the manifest relation against ordinary staging. Checkout/merge/resolution take
relation locks in OID order and refresh their snapshot after waiting.

The existing transaction-duration table writer locks remain, including between
writers on different branches. This milestone does not implement row-level MVCC,
row locks, SSI, or concurrent working-set reconciliation. No second manifest file
or custom WAL resource manager is introduced.

Pending merges use immutable `merge` objects (appended kind ID 8), format
`DGMERGE1`, and `merge/<branch>` CAS refs. Children are ordered ours/theirs/base
commits; payload contains the squash flag and conflict evidence. Existing object
kind IDs are unchanged. `views/enabled` points to a retained commit. All these
objects/refs use the existing heap/WAL infrastructure. Root installation, rebuilt
indexes, HEAD/stage/working changes and conflict state commit or abort together.
Resolving a conflict creates a new immutable merge object and advances its ref.
Aborting restores the clean destination state and clears the pending ref.

## Data merge granularity

Primary-key tables merge by packed primary-key columns. Independent column
changes combine; competing edits to a column, delete/update and divergent inserts
conflict. JSON/JSONB objects merge recursively by member; arrays and scalars are
atomic values. Keyless identity is row bytes plus cardinality, with concurrent
cardinality edits classified as conflicts as described above.

Indexes are derived from merged table contents. Supported unique opclasses are
validated before installation. If a candidate violates uniqueness, retain the
entire destination table and record one **whole-table conflict**, replacing its
row conflict entries. The retained commits contain all three versions. Resolving
that unit with `--ours` or `--theirs` replaces the entire table with that side,
including otherwise independent edits. This conservative behavior needs a later
row-level constraint-artifact design; it is not Dolt's full constraint-violation
system. Ordinary errors while applying a resolution still obey SQL rollback.

## Verification

Native TAP tests `009_merge.pl` and `010_branch_sessions.pl` cover data merging,
conflicts, crash recovery, prepared transactions, old snapshots, cached plans,
staging, writer waits and cancellation during publication. Doltgres
`testing/go/branch_merge_test.go` isolates supported data cases from the original
mixed schema scenarios. The unchanged simple and JSON `TestMerge` scripts run
against both engines. Explicit profile exclusions name the unsupported schema
cases; there is no skip-on-error behavior. See the workspace HTML milestone
report for exact runs and counts.
