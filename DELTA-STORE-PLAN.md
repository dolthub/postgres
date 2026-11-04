# Delta store plan: Doltgrust's write path in the fork

Status, 2026-11-04: experiment on branch `zachmu/doltgrust-deltas`, not to be
merged into `zachmu/doltgres-rewrite` until reviewed. It supersedes the row
journal (`dg_journal.inc`, `pg_dolt_journal`), which this branch removes, and
[WRITE-PERF-PLAN.md](WRITE-PERF-PLAN.md) step 3 for ordinary DML.

## Status (2026-11-04)

Milestones 1–4 are done; milestone 5 (merge-on-read) was pulled into 3:

- **Row journal removed** (`c2d99e6f27d`).
- **Memtable and WAL** (`e58ac8f3ca8`), then the keyed form: a skiplist per
  tree in a DSA area, with lazy readers (`843b09eb31c`).
- **Writes and reads** (`843b09eb31c`): batches append versions; saves write
  a root row only for a tree with none, and otherwise append their diff; no
  commit-time publication.
- **Folding and pruning** (`1f43a48779c`): a launcher, per-database workers,
  `dolt_fold()` and `dolt_delta_stats()`.
- **Standby pruning, throttling, install locks** (`f66090ab917`): a FOLD WAL
  record carries each fold's horizon; writers to a full store wait for folds;
  `dg_history_install` takes ExclusiveLock on the table.
- **Hot-path fixes** (after profiling TPC-C):
  - cursors read the delta in chunks of 4 doubling to 256, bounded by their
    stop key, rather than to the end of the tree;
  - each storage has two words, one moved by handle commits and one by delta
    appends, so appends no longer invalidate handle caches and batches;
  - a claim of a rolled-back claimant can be retaken;
  - each storage's slot remembers its last 8 handle committers, and a delta
    of handles read under the latest snapshot serves any MVCC snapshot that
    agrees with it on them. Repeatable-read transactions no longer reread
    `pg_dolt_handle` for every load;
  - the checkpoint streams `delta.state` in 1 MB chunks instead of building
    it in memory.
- **Results.** test_prolly, test_doltgres and test_dolt_chunk fail exactly
  what the merge base fails. TPC-C at scale 1: 1 thread 130 → 163 tps;
  10 threads 178 → 471 tps (new orders 110 → 279 per second). Every
  consistency check passes. Sysbench writes are 1.5–2.5× faster, reads
  unchanged.

Lessons:

- **The launcher is not in the proc array.** `TransactionIdIsInProgress` skips
  the caller's own array slot, and for a process outside the array that is slot
  0, so the launcher took some running transactions for rolled-back ones and
  pruned their versions. Pruning now decides "ended" by the xid horizon alone.
- **Loads must not copy the delta.** The first merge-on-read collected a
  state's whole delta at load, and a 20,000-row UPDATE, which loads per row,
  ran out of memory. Hence the shared skiplist and lazy views.
- **Caches keyed on "nothing committed since" fail under concurrency.** With
  ten writers, some transaction always commits between a snapshot and the
  next load. A cache must ask whether the commits it missed touched what it
  holds.
- **Larger fold thresholds help less than expected.** Folding every 262,144
  versions rather than 4,096 gained ~15%, but a fold worker then needs
  ~200 MB, and a 60 s TPC-C run on this box ran out of memory. The
  out-of-memory kills came from catalog-view rebuilds in every backend after
  autovacuum's statistics updates (a fork-wide cost, not the delta store's)
  plus the old in-memory checkpoint.

Open:

- **Folds materialize a tree's whole visible delta** in one `pl_apply`.
  Doltgrust folds a batch of keys at a time; bounded batches would make large
  thresholds safe.
- **Branch rename, branch drop and DROP DATABASE** leave their trees in memory.
- **TRUNCATE and large installs** append a diff of every row; replacing the
  root row would be O(1).
- **Remaining TPC-C costs** are mostly the fork's own: row-lock waits (half of
  backend time, as in heap but longer-held), catalog binding per statement,
  and node-cache copies.

## Why

Every commit that writes a prolly table publishes at PRE_COMMIT. It locks the
table's `pg_dolt_root` row for (storage, branch), merges three ways onto the
latest roots, writes the merged nodes, updates the row, and holds the lock
until commit. A table behaves like heap's single hottest row. At scale 1, TPC-C
gets 72 tps at 1 thread and 89 at 10, against 887 and 2,735 for stock
PostgreSQL ([three-way performance](/workspace/reports/three-way-performance-2026-11-02/index.html),
[write serialization](/workspace/reports/prolly-write-serialization.html)).

The row journal tried to defer tree building, but writers folded it into
their own roots, so it still serialized folds through the root row, and
folded far too often. Concurrent writers lost updates
([KNOWN-ISSUES.md](KNOWN-ISSUES.md)).

Doltgrust (`/workspace/dolthub/doltgrust`, `crates/txn`) has no commit-time
tree work at all. Its parts are:

1. **N trees, each with its own history of bases.** Every table and index tree
   of every branch is a base plus a delta. A tree keeps several bases, each
   tagged with the commit point it folds through. A reader uses the newest
   base its snapshot may see, plus the delta versions that base lacks.
2. **A shared, multi-version, durable delta.** A write appends a version to
   the tree's in-memory delta and to the WAL, where Postgres writes its heap
   and WAL. Versions are stamped with their writer; visibility is the
   snapshot's. Conflicts are row-level, at write time.
3. **Commit is a WAL commit record.** No root is built, merged, or locked.
4. **Background folding.** Workers fold committed versions into new bases in
   large batches, per tree, off the commit path. They install the bases
   without blocking writers and prune versions no snapshot needs.
5. **Captures.** A version-control operation that needs a real root folds what
   its own transaction sees into a private tree.
6. **Checkpoint and recovery.** Recovery starts from the bases and replays the
   delta's WAL.

This plan maps each part onto the fork. Commit then does no prolly work, and
write concurrency is limited by row locks, as in heap.

## What the fork keeps

- Prolly trees, the chunk store, history independence, and every version
  control format. Trees and roots stay canonical: no delta ever enters a hash.
- `pg_dolt_root` as the durable home of each tree's newest base.
- Row identity: derived handles and `pg_dolt_handle`. Row locks: lock records
  in `pg_dolt_lock`, via heap tuple locking. Unique keys: reservations. All
  three already work at row level and need no change. They live in heap
  catalogs whose writes commit with the transaction.
- Batches. A command's edits are already per-key signed occurrence counts
  (`dg_batch.inc`). Every tree's value is "count, then row", and every merge
  adds counts (`merge_counts`). That makes the delta's operations commutative:
  a version is a count delta, so any visible set folds to the same tree
  whatever its order.

## Design

### 1. The memtable (part 2)

Shared memory, in a DSA area created in place in the main segment (like the
node cache), so the startup process can replay into it and the checkpointer
can save it.

- **Trees.** A tree is keyed by `(database, tablespace, relfilenumber,
  branch)`, the existing `RootKey` plus the database. A partitioned hash table
  maps tree keys to tree headers. Each header has its own LWLock and a list of
  versions in arrival order.
- **Versions.** Each version holds:
  - top-level xid, (sub)xid and command id;
  - the WAL position of its record;
  - the tree key bytes, a signed count delta, and the row bytes;
  - hint bits: committed, aborted, and pruned.
- **Writes.** A command's batch flush appends its versions in one call: one
  WAL record per (tree, batch) and one tree lock acquisition.
- **Reads** filter versions by snapshot:
  - **Own versions** are visible by command id, unless their subtransaction
    aborted.
  - **Other transactions' versions** are visible if their top-level xid isn't
    in the snapshot and their (sub)xid committed.
- **Budget.** `dolt_delta_memory` bounds memory. Over budget, writers are
  throttled while the fold catches up, as Doltgrust does.

### 2. WAL and recovery (part 6)

- **A new resource manager, `RM_DOLT_DELTA_ID`.** It has one record kind,
  "versions". The record carries the tree key and a list of (key, delta, row)
  entries, plus the top-level xid, subxid and command id.
- **Insert order.** Insertion uses `DELAY_CHKPT_START`, as commit records do:
  allocate the versions, insert the WAL record, then link the versions with
  their LSN. A checkpoint therefore never starts between the two.
- **Checkpoint.** `CheckPointDoltDelta`, called from `CheckPointGuts`, writes
  every unpruned, unaborted version whose LSN precedes the redo point to
  `pg_dolt/delta.state`, then fsyncs it. Versions at or after the redo point
  come back from WAL replay.
- **Startup** loads the file, then redo replays the records after it. Commit
  and abort records need no handling: visibility reads the clog.
- **Hot standby.** The standby replays the same records into its own
  memtable. Its readers see the versions their snapshots allow. A fold's
  "base advanced" record lets the standby prune.
- **Two-phase commit.** A prepared transaction's versions stay in the
  memtable (and in the checkpoint file) until COMMIT or ROLLBACK PREPARED
  settles their xid. Nothing blocks other writers.

### 3. Bases (part 1)

A base is a `pg_dolt_root` row version. A fold writes a new row version with:

- the new root;
- `rootxmin`, `rootxmax` and `rootxip`, the fold's snapshot (top-level xids
  only). The base contains exactly the versions that snapshot sees.

**Reader rule.** Under snapshot S, ordinary MVCC selects the root row version
S sees. If S sees fold F's commit, S was taken after F's snapshot, so S sees
everything the base folded. The reader then adds the versions that are
visible to S and not visible to the base's fold snapshot. Each `pg_dolt_root`
row's version chain is the tree's base history. PostgreSQL MVCC does what
Doltgrust's per-tree list of CSN-tagged bases does, with no CSN.

**Pruning.** A version leaves the memtable once a base folded it and every
running snapshot sees that base. That holds when the fold's xid precedes the
oldest xmin. Aborted versions go at once.

**First write to a tree with no root row** (new storage, or a branch without
roots yet). The writer creates the row with the base readers would compute
(`dg_branch_base`), under a lock held to commit, as first publishers did.
This happens once per tree. Without the row, the base would follow the
branch's HEAD, and a `dolt_commit` would apply the delta twice.

**Pins.** Between selecting a base and collecting its delta, a reader must
not lose the versions a newer fold holds. Each backend publishes the oldest
xmin it loaded under, in shared memory, until its transaction ends. Pruning
waits for every pin, not only for registered snapshots.

### 4. Write path (parts 2, 3)

- **Batch flush** (`batch_flush_table` and `batch_flush_index`) appends each
  pending row and index entry as a version, instead of applying it with
  `pl_apply` and pushing an overlay. Handle allocations still go to
  `pg_dolt_handle`.
- **`dg_save` callers that build whole trees** (version-control installs,
  DDL rewrites, `dg_history_install`, native catalog mutations) emit
  `diff(view they loaded, tree they built)` as versions, unless the tree has
  no root row yet (section 5). The overlay stack, `publish_all`,
  `roots_lock`, `roots_merge` and the commit-time root write all go away.
- **Version-control installs** (merge, reset, checkout, cherry-pick, revert)
  take ExclusiveLock on the tables they install into. Their count deltas then
  can't race concurrent row edits into negative counts. Readers still read
  during an install. Doltgrust also locks tables it installs into.
- **Snapshot writes** (`dg_snapshot_writes`) and batch staleness go away.
  Writes no longer build on a tree.

### 5. Read path (parts 1, 2)

Settled 2026-11-04: merge on read from the start. Materializing on every load
would cost O(delta) node hashing per statement, which is exactly the hot-path
cost the delta store exists to remove.

- **Loading.** A load reads the base row its snapshot selects, then collects
  the delta versions that snapshot sees and the base doesn't hold. They go
  into a backend-local array sorted by key, with each key's net count change.
  `DGState.tree` is then the base, and `DGState.delta` the array.
- **Hot readers** go through `dg_get`, `dg_scan`, `DGCursor` and `dg_count`,
  which merge the base with the array the way Doltgrust's readers do:
  table scans, fetches, index scans, handle lookups, unique checks and
  batches. They never hash a node.
- **Readers that need a real tree** (version control, diff, merge, export,
  DDL rewrites) call `dg_content`, which applies the array with `pl_apply`
  first. Renaming the field from `content` to `tree` made the compiler find
  every such use.
- **Next.** Collecting is O(delta) per load. Folds keep deltas small, and a
  per-backend cache or a shared ordered index per tree (Doltgrust's memtable
  key ranges) can replace collection if profiles show it.

**Saves.** `dg_save` writes a root row only when the tree has none yet:
fresh storage from CREATE, rewrites and index builds, or a branch's first
write. That root row has no horizon, because no versions can exist without
it. Every other save appends `diff(view loaded, tree saved)` as versions. A
DML batch appends its pending rows and index entries directly. Only folds
replace existing root rows, so only folds' rows carry a horizon.

### 6. Folding (part 4)

- **Workers.** A launcher bgworker (shared memory only) watches per-database
  delta pressure. It starts per-database dynamic fold workers, as autovacuum
  does.
- **Triggers.** A tree is folded when its delta passes `dolt_fold_size`, or
  its oldest version is older than `dolt_fold_timeout`. When over budget, the
  largest trees go first.
- **A fold is one transaction per tree.**
  1. Take a conditional lock on a fold tag, so only one fold per tree runs.
  2. Take a snapshot, read the base row, and apply the versions visible to the
     snapshot but not to the base's fold snapshot.
  3. Write the new nodes to the chunk store, then update the root row with
     the new root and fold snapshot.
  4. Commit.

  Nothing a writer does waits on a fold. Writers never touch root rows
  (except the first-write case), and versions are immutable.
- **Version-control row writes.** If a version-control transaction holds the
  root row's tuple lock, the fold skips the tree and retries later.

### 7. Captures (part 5)

`dolt_commit`, `dolt_add`, diff, merge and branch creation need roots. They
read the materialized view of their snapshot (5A), which is Doltgrust's
capture. Its nodes reach the chunk store only when a commit or ref names them.
Branch creation copies the source's view root as the new branch's base. That
is O(delta), not O(rows).

### 8. Things to settle while building

- Branch rename rekeys memtable trees in the same transaction as its root
  rows. Branch and storage drops discard their trees once their transaction
  commits.
- SSI: the existing serializable writer registry stays. Predicate locks on
  versions are the same as on today's states.
- Logical decoding of prolly writes is out of scope, as today.

## Milestones

Each one ends with a checkpoint commit and an update to this file.

1. **Remove the row journal.** Delete `dg_journal.inc`, `pg_dolt_journal`,
   `dolt_row_journal`, `036_row_journal.pl`, and the fold hooks in loads and
   saves. *Gate:* test_prolly and test_doltgres pass as on the merge base.
2. **Memtable and WAL.** Add the shared structure, the resource manager with
   redo and desc, the checkpoint file, and startup load. Writes are still
   unused. *Gate:* unit-level TAP test: versions survive a crash and a
   restart, and replay on a standby.
3. **Writes to the delta, reads by materialization (5A), no publication.**
   Includes the root row fold-snapshot columns and the first-write base row.
   *Gate:*
   - test_prolly, including `050_concurrent_writes.pl`, which failed with the
     journal, and the isolation specs;
   - test_doltgres;
   - TPC-C consistency checks at 10 threads.
4. **Fold workers and pruning.** *Gate:* memory stays bounded under a
   10-minute TPC-C run. Restart and standby tests pass with folds in flight.
5. **Merge-on-read (5B).** *Gate:* sysbench single-thread at or below the
   merge base on every test. TPC-C scale 1 at 10 threads beats 1 thread by a
   wide margin.
6. **Version-control installs as deltas, with table locks.** *Gate:* version
   control suites, and concurrent DML during merge and reset.

## Measurement

- Percona sysbench-tpcc, scale 1, 10 threads and 1 thread, repeatable read,
  foreign keys on, 60 s timed, consistency checks after every run.
- Doltgres CI sysbench (19 tests, 10,000 rows, 1 thread, 20 s).
- Baselines on the merge base, built the same way, interleaved. Report new
  orders per second as well as tps.

## Known risks

- Hashing in phase A: every backend applies everyone's committed versions to
  its own view. That is O(backends × writes) of node hashing. Phase B removes
  it, and phase A is only a correctness step.
- Long snapshots pin versions in memory. Doltgrust gives an old snapshot a
  private base. Here the budget throttles writers until the snapshot ends; a
  private base can follow.
- Count deltas assume every tree is a counted multiset. A future keyed format
  ([keyed rows](/workspace/reports/keyed-rows/index.html)) would need a version
  order per key. Its row locks already serialize writers of one key, so a
  per-key sequence number taken under the lock would suffice.
