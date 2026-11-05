# Chunk store plan

Status, 2026-10-25: milestone 1 is complete (commit `645794b3460` on branch
`zachmu/chunk-index`): a minimal catalog with native access, a node cache and
microbenchmarks. The heap design is accepted provisionally. We build on it only
behind a standard store interface (below), so a different implementation can
replace it later. Milestone 2, the interface, is complete (`b8885f6124d`).
Milestones 3 and 4, library changes and AM integration, are next. This plan supersedes step 1 of
[the write performance plan](WRITE-PERF-PLAN.md) and is the next focus of
[PLAN.md](PLAN.md).

## Problem

Two partial chunk stores exist today. Neither meets the requirements.

- **Prolly relation files.** Each table and each prolly index has an append-only
  file of nodes and root records, protected by WAL. There's no index by hash:
  every load scans the whole file. Each file is also scoped to one relation, so
  it can't answer "where is chunk X" for a database.
- **`pg_dolt_object`.** A heap catalog with a btree on `objhash`, so it's
  database-wide and persistent. But it's keyed by an envelope hash, not the prolly
  node hash. It holds only chunks exported at commit or capture, which duplicates
  relation data. It allows duplicate rows, writes through SPI, and TOASTs large
  payloads.

The pg_dolt catalog audit (workspace `reports/pg-dolt-catalogs.html`) lists
the consequences: copying on every capture, two hash
namespaces, and a full tree walk per capture.

## Requirements

1. Any chunk is locatable by hash quickly, across every table, branch and commit.
   Commands such as merge take arbitrary commit hashes.
2. The index is serialized to disk. No relation is scanned at startup or first
   access.
3. Cached chunks are shared across sessions. Chunks are immutable, so sharing is
   safe.
4. Row-level concurrent writers to one table on one branch must be supported
   ([row-level tracking](ROW-LEVEL-TRACKING-PLAN.md)). The current table-wide
   writer lock is temporary. There must be no hot page or relation-wide append
   lock on the write path.
5. One hash namespace: chunks are keyed by the prolly node hash, so tree
   pointers are store keys directly.
6. A chunk hash is unique by definition. Duplicates are bugs.

## Division of responsibility

**Table and index access methods stay as adapters into PostgreSQL's executor.**
Their own relation storage holds only non-canonical runtime state:
- which root is current per branch and snapshot (root records today);
- the mapping between TIDs and rows (derived from row digests since 2026-10-28,
  with `pg_dolt_handle` for the allocated remainder; see the row-level plan);
- any other per-transaction metadata.

This state is updated outside prolly content and has to live in a relation that
works with PostgreSQL's APIs. The AMs also own the tuple codec.

**The chunk store is database-wide and holds every canonical node:** table
content trees, index trees and schema trees, whether working or committed. A
commit records root hashes and copies nothing.

**Structured metadata objects stay separate:** commits, database states, table
and index descriptors, tags, merge, rebase, stash and routine objects. They stay
in `pg_dolt_object`, or a successor, rekeyed onto the same node-hash namespace
and with duplicates fixed.

## Selected design: heap catalog with a unique btree

```
pg_dolt_chunk (OID 9125)
    chunkhash  bytea  -- SHA-256 of the node bytes; primary key
    chunkdata  bytea  -- node bytes, inline, uncompressed
pg_dolt_chunk_hash_index (OID 9126): UNIQUE btree (chunkhash)
```

### Decisions

1. **A separate catalog for chunks**, distinct from metadata objects.
2. **Insert semantics:** transactional insert-if-absent versus immediately
   visible chunks is undecided, pending measurement. Milestone 1 uses
   transactional insertion following PostgreSQL's ON CONFLICT DO NOTHING
   protocol. That means:
   - a dirty-snapshot probe;
   - waiting for an in-progress inserter of the same hash;
   - a speculative heap insert;
   - a unique-index check that cancels and retries on conflict.

   Aborted inserts become dead tuples that VACUUM reclaims, with no chunk
   garbage collection needed. The alternative makes chunks visible immediately,
   avoiding waits between writers that create identical chunks. It moves cleanup
   of unreferenced chunks onto garbage collection.
3. **All data inline, no TOAST.** The catalog has no TOAST table and its data
   column uses plain storage, with no compression. A chunk must fit in one heap
   tuple, about 8,100 bytes of node data.
   - Today's chunker can exceed that. Nodes reach about 8,500 bytes, and single
     large values make larger ones.
   - Before AM integration, the prolly library must bound node size below the
     inline limit. Large values need their own chunked representation, as Dolt
     does for blobs.
   - Milestone 1 rejects chunks over the limit.
4. **Direct access in all cases:** heap and index C APIs, never SPI.
   - Reads probe the unique index with a non-MVCC snapshot, like TOAST. Chunk
     content is fixed by its hash, and any root a reader can see references
     only chunks that exist.
   - Writers probe with a dirty snapshot to dedupe.
5. **A per-backend node cache is required.** It's keyed by hash and bounded by
   `dolt_chunk_cache_size`. It never needs invalidation, because nodes are
   immutable. Every performance comparison includes it.
6. **One hash namespace:** the prolly node hash.

### Locking and maintenance

- Readers take ACCESS SHARE on the catalog and index, and writers take ROW
  EXCLUSIVE, so scans never block writes.
- Page-level content locks are short and never held while waiting on a
  heavyweight lock.
- Garbage collection deletes unreachable rows, and plain VACUUM reclaims space.
  Routine maintenance never takes ACCESS EXCLUSIVE on the chunk store, so no
  VACUUM FULL or CLUSTER.

### Transactions

The chunk store, the AM runtime stores and branch pointers can all change in one
PostgreSQL transaction. They share one commit record, one WAL stream and one
recovery path. Requirements that remain:

- **A garbage collection protocol.** A writer may skip inserting a chunk it sees
  and reference it, while garbage collection decides that chunk is unreachable.
  MVCC alone doesn't prevent deleting it. Dolt's garbage collection solves the
  same race with a safepoint protocol.
- **Branch-pointer publication** must not hold one hot row across transactions.
  It merges onto the latest root at commit
  ([row-level tracking §4](ROW-LEVEL-TRACKING-PLAN.md)).
- **No buffer or lightweight lock held while waiting on heavyweight locks.**
  PostgreSQL's deadlock detector doesn't see those.
- **Subtransaction abort** discards backend-local caches and pending state that
  belong to the aborted subtransaction.
- **Logical decoding** won't see prolly data written with generic WAL.

### Reading at access time

Table and index AMs fetch nodes from the chunk store while scanning, as heap
tuples read TOAST. Costs and mitigations:

- **Index probe plus heap fetch per uncached node**, a few microseconds. The node
  cache absorbs hot interior nodes.
- **Inline, uncompressed storage** lets a node be parsed from a pinned buffer.
- **Scattered leaves on a cold cache.** Mitigated with read-ahead from interior
  nodes through read streams.

### Measured limits (milestone 1)

Full results are in the workspace report `reports/chunk-store-benchmarks.html`.
Setup: an optimized build, 7 CPUs, 7.6 GB RAM, 2 GB shared_buffers.

- **Space.** Overhead is about 75 bytes per chunk. Mixed sizes from 512 to 8,000
  bytes pack into heap pages at about 90% efficiency.
- **Writes are bound by WAL volume.** Each chunk writes its own size to WAL plus
  about 5%.
  - One session does about 46,000 mixed-size puts per second at first, slowing
    to about 30,000 by 2 million chunks (9.5 GB) as heap write-back competes.
  - Four sessions top out near 45,000 per second, at about 215 MB/s of WAL.
- **The write curve bends at shared_buffers, not RAM.** With 128-byte chunks,
  puts run at 130,000 to 200,000 per second until the btree passes
  shared_buffers, at about 27 million chunks. They then fall steadily, to
  22,000 per second at 68 million chunks and 4.9 GB of index. Random-key inserts
  read and dirty random leaf pages. This curve is accepted for now.
- **Reads.**
  - A warm native lookup takes 1.5–3.4 µs, about 25% faster than SPI for mixed
    sizes.
  - A node-cache hit takes 0.06–0.1 µs.
  - Reads that miss memory cost 30–150 µs on either path.
- **Hint bits.** Data checksums are on by default in PostgreSQL 18. With them,
  the first existence probe of newly committed chunks sets hint bits and logs
  full-page images: about 850 MB of WAL per 200,000 probes. Plain reads skip
  visibility checks and avoid it.
  - Fix before AM integration: probe without setting hints, or freeze chunks
    promptly.
- **Write-path consequence for AM integration.** Without the statement-level
  deferred flush, each inserted row writes its leaf and the path above it, about
  14 KB of chunks. At about 200 MB/s of WAL, that caps inserts near 15,000 rows
  per second. The deferred flush
  ([write performance plan](WRITE-PERF-PLAN.md) step 3) therefore follows AM
  integration immediately.

## Store interface

Every consumer goes through one interface. Implementations sit behind a function
table, in the style of PostgreSQL's table AM routine. The heap catalog is the
first implementation. A packed journal, or anything else, must be able to
replace it without changing callers.

### Contract callers may rely on

- **Idempotent puts.** Putting a chunk that is already present is harmless.
- **Reachable chunks are readable.** A chunk referenced by any root visible to
  the caller's snapshot can be read.
- **Durability by commit.** A chunk put in a transaction is durable once that
  transaction commits.
- **Content is fixed by hash.** The bytes returned for a hash are the bytes that
  were put under it.

### Not part of the contract

Callers must not rely on these, even though the heap implementation shows them:

- whether a put disappears on rollback;
- whether a put waits for a concurrent inserter of the same hash;
- whether chunks put by other running transactions are visible;
- relation lock modes, catalog OIDs or snapshot types;
- how long results remain valid without an explicit pin.

### Interface shape

- **Open and close by intent,** read or write, not by lock mode. A handle and
  its pins last at most one transaction.
- **Batch get.** Returns each found chunk *pinned*. Callers release pins
  explicitly, and pins are dropped at transaction end. A single-chunk call is a
  convenience wrapper.
- **Batch existence check.**
- **Batch put.** The implementation may reorder a batch, for example by hash for
  index locality.
- **Lifecycle operations:**
  - enumerate and delete, for garbage collection;
  - the maximum chunk size, reported by the implementation;
  - store statistics.

### Above the interface

These layers wrap every implementation, so benchmarks compare stores rather
than caches:

- **The node cache.** Pinned entries are never evicted, and eviction uses only
  unpinned entries.
- **Optional hash verification** of puts and cache fills.
- **Statistics.**

Implementation selection is a single routine pointer. Tests can install another
implementation to prove the boundary holds.

## Alternative: custom packed journal with a serialized index

Kept in reserve in case write throughput or index scale outgrows the btree.
Modeled on Dolt's NBS journal and table files:

- **Chunk records** appended to relation pages, many records per page, with
  generic WAL deltas instead of full-page images.
- **Sorted runs** indexing those chunks, about 14 bytes per entry (an 8-byte hash
  prefix, block and item). Each run has a page directory and a Bloom filter, and
  is searched in place through shared_buffers.
- **Size-tiered merging.** Small merges happen inline at publication; large ones
  go to a background worker that appends output in small batches.
- **The list of live runs** is a heap catalog of run descriptors, inserted
  transactionally at publication. An earlier variant with a metapage in block 0
  was rejected: under row-level concurrency it becomes a hot page.
- **Database-wide index entries** carry a storage id, block and item.

| | Heap and btree (selected) | Packed journal |
|---|---|---|
| Write path | Random btree leaf per chunk, plus WAL full-page images after checkpoints | Sequential appends; index writes amortized by merging |
| Index size per chunk | ~50 B | ~14 B, plus ~1.2 B of Bloom filter |
| Lookup | One btree descent | A Bloom filter per run, then a binary search in one or two runs |
| Garbage collection and space | DELETE and VACUUM, space reused in place | Copy-forward compaction; runs rewritten |
| Concurrency, recovery, replication | Provided by PostgreSQL | Custom protocols to write and test |
| Implementation effort | Small | Large |

**Revisit when** measurements show the btree index outgrowing memory for real
workloads, or index maintenance dominating write throughput.

**Intermediate option before a full journal:** a small recent index plus a large
settled one, merged periodically. That approximates sorted runs using
PostgreSQL's own structures.

## Unlogged chunk journal (selected 2026-11-05)

Status, 2026-11-05: steps 1 and 2 are done on branch
`zachmu/unlogged-chunk-journal` (`b8b84528227`). TPC-C at 10 threads went
from 419 to 540 tps with every chunk logged, and to 683 tps with unlogged
folds. WAL fell from 239 KB to 41 KB per transaction: 33 KB of row edits and
8 KB of lock records
([report](/workspace/reports/unlogged-chunk-journal/index.html)). Step 3 is
next.

**Why.** Under TPC-C (scale 1, one thread), the heap store made the WAL
342 KB per transaction against stock's 5.5 KB
([measurements](/workspace/reports/tpcc-bottleneck/index.html)).
84% of it was chunks:

- the `pg_dolt_chunk` heap, which logs every node's bytes;
- full-page images from its hash index, whose random keys put every insert
  on a random page.

Commits flushed that WAL. The writes also churned the node cache, and readers
then missed and read chunks back.

**Principle.** Only row edits have to be in the WAL. A tree derived from a
base and a set of edits can be rebuilt from them. Prolly trees are history
independent, so the rebuild yields the same root hash. Fold output therefore
needs no WAL: only the edits and the fold's record do. Chunks that can't be
derived from edits keep going through the WAL:

- database roots;
- schema definitions;
- commit objects;
- trees built whole (rewrites, index builds, bundles and fetched data).

**Store.** This revives the journal of `zachmu/chunk-journal`, cluster-wide
and outside the buffer manager:

- **Files:**
  - `pg_dolt/chunks/journal`, a stream of records, each a header (CRC-32C,
    length, kind, hash) followed by the chunk's bytes;
  - `pg_dolt/chunks/index`, holding two alternating metapages and the sorted
    runs. A metapage gives the stream position the runs cover and the list of
    live runs, with a generation and a CRC.
- **Recent index:** records past the indexed position are indexed in shared
  memory, in the main segment. When that index fills, it's written as a run.
  When runs pile up, they're merged.
- **Writes and reads:**
  - Appends are plain file writes, serialized by one lock and written before
    they're indexed.
  - Readers take a short shared lock to locate a chunk, then read without it,
    since records never move.
  - No WAL and no shared buffers, so no full-page images.
- **Durability:**
  - **Logged puts** also write a `DoltChunk/PUT` WAL record holding the
    chunks. Redo appends any the journal lacks, and checkpoints fsync the
    journal.
  - **Unlogged puts** (folds) fsync the journal before the transaction's
    commit record, so a visible base always names durable chunks.
  - **Runs and metapages:** the stream is fsynced up to the indexed position
    before a metapage moves, and runs are fsynced before the metapage that
    names them.
- **Startup**, before redo: read the newer valid metapage, index the stream
  past it until a record fails its CRC, and truncate there.
- **Databases share one journal.** Chunks are content addressed, so
  `CREATE DATABASE` copies nothing, and base backups copy the files.
- **Garbage collection** later rewrites the whole journal from the live
  chunks. Splitting it into segment files waits until a database is too
  large to rewrite at once.

**Standbys** receive logged chunks through the WAL and rebuild fold output
themselves:

- A FOLD record carries the base root and the new root.
- The standby folds the edits that the record's snapshot can see into its
  base, and checks that it gets the logged root.
- It keeps those edits until its own fold finishes, and readers use the older
  base until then.
- A mismatch stops replay with an error.

**Steps:**

1. The journal store replaces the heap store and the `pg_dolt_chunk`
   catalog, with every put logged. Tests read chunks through
   `dolt_chunk_list()`. *Gate:*
   - test_dolt_chunk, test_prolly and test_doltgres as before;
   - crash tests for a torn tail and for runs written at a crash;
   - WAL bytes per chunk.
2. Unlogged fold output, behind `dolt_unlogged_folds` (off by default):
   folds fsync before commit. *Gate:* crash tests, and TPC-C windows and WAL
   per transaction.
3. Standbys re-fold, so unlogged folds become the default. *Gate:* the hot
   standby tests with unlogged folds on, including a deliberately mismatched
   root.
4. Captures write their trees unlogged and log what a standby needs to
   rebuild them.

## Milestones

1. **Minimal store (done, `645794b3460`):**
   - the catalog, insert-if-absent, lookup and existence check;
   - the node cache and `dolt_chunk_cache_size`;
   - correctness tests: dedup, abort, concurrent identical inserts, recovery, the
     size limit;
   - microbenchmarks of puts and gets with and without the cache at several
     scales, recording WAL bytes per chunk.
2. **Store interface (done, `b8885f6124d`):**
   - the function table, contract and batch and pin API above;
   - the cache, verification and statistics moved above it;
   - the heap implementation behind it;
   - a second, test-only implementation exercising the same contract tests;
   - an existence probe that doesn't set hint bits.

   As built:
   - `access/doltchunk.h` defines `DoltChunkStoreRoutine`, implemented in
     `dg_chunk_heap.c`.
   - `dg_chunk.c` holds the cache, pins, verification and dispatch.
   - `dolt_chunk_verify_hashes` controls verification.
   - `test_dolt_chunk` runs one contract file against the heap store and a
     non-transactional in-memory store. It uses pageinspect to prove probes
     leave hint bits unset.
3. **Library changes:**
   - **A fetch-on-miss callback** so trees read through the store and node cache.
     Nodes record whether they are already in the store, so flushing writes only
     new ones.
   - **A path-local apply.** It rebuilds from the first edited leaf until chunk
     boundaries resynchronize, and appends untouched subtrees by reference
     without fetching them. Today's apply visits every leaf.
   - **A diff that skips identical subtrees at any level,** which makes merge
     O(changes).
   - **No node-size bound in the library.** The heap store instead splits
     oversize chunks across several inline rows, keyed by (hash, part). That
     replaces the earlier plan to bound nodes and chunk large values.
4. **AM integration**, superseding the earlier per-head heap design (see
   [row-level tracking §2](ROW-LEVEL-TRACKING-PLAN.md)).
   - **Content and index trees** live in the chunk store. (The handle and
     key-to-handle trees did too until 2026-10-28; handles are now derived
     from content, see [row-level tracking](ROW-LEVEL-TRACKING-PLAN.md).)
   - **Working roots** live in a roots catalog keyed by (storage resource, branch
     name), not in `pg_dolt_binding`. Binding rows exist only for captured or
     canonical relations, and they are rewritten when schemas change.
   - **Relation files** no longer hold chunks or root records.
   - **Load** finds a root, then walks the tree through the store.
     - It checks the transaction overlay first: a per-backend stack of (command
       ID, subtransaction, base roots, roots) per (resource, branch).
     - Otherwise it reads the committed roots under the caller's snapshot.
     - If an overlay entry's base differs from the snapshot's committed roots,
       the view is the three-way merge of the two. This rebases Read Committed
       statements onto newer commits.
   - **Save** pushes roots onto the overlay; nothing is written then.
   - **Publish** happens just before commit or PREPARE.
     - In (resource, branch) order, lock or insert the root row, then merge the
       transaction's changes onto the latest committed roots.
     - Write the new nodes to the chunk store, then write the merged roots. The
       row lock is held until commit.
     - New nodes are also flushed early once their memory crosses a threshold.
   - **Abort and subtransaction rollback** pop the overlay.
   - **Snapshot correctness.** Each publisher merges onto its predecessor's root
     under the root row lock. That lock is released only after the publisher is
     visible to new snapshots. So any snapshot that sees a publisher also sees
     every earlier publisher, and MVCC on the root row selects a root containing
     exactly the visible transactions.
   - **Row-level writes** replace the table-wide writer lock. Updates, deletes
     and row locks lock the row's lock record and compare versions to decide
     between proceeding, waiting, rechecking (Read Committed) or a serialization
     error (Repeatable Read).
   - **Indexes are maintained incrementally,** and index scans use key bounds
     and derive each entry's handle from its digest (formerly the key-to-handle
     tree). Both replace whole-index rebuilds and nested-loop scans.
   As built (2026-10-24):
   - The roots catalog is `pg_dolt_root`, keyed by (tablespace, file number,
     branch), so TRUNCATE's new file starts empty and its rollback restores the
     old roots. Lock records are `pg_dolt_lock`.
   - New nodes are written only at publication; the early flush threshold is
     left to milestone 5.
   - Index maintenance is incremental for row writes. History installs (merge,
     reset, checkout) still rebuild each index in full.
   - [Row-level tracking §2](ROW-LEVEL-TRACKING-PLAN.md) lists the
     implementation amendments: a TID per row version, frozen lock records,
     unique-key reservations and waitless chunk puts.
5. **Deferred flush:**
   [write performance plan](WRITE-PERF-PLAN.md) step 3, required before the
   integrated write path is usable at volume.
6. **History.**
   - Commit and capture record roots without copying.
   - Rekey and dedupe `pg_dolt_object`.
     - Known defect (2026-10-26): every committed chunk is stored twice, under
       two different hashes. The live tree lives in `pg_dolt_chunk`, keyed by
       the SHA-256 of the node bytes, which is also the hash that interior
       nodes and the `content` field of table objects carry. `export_node` then
       copies each node into `pg_dolt_object` as a `chunk` object whose
       `objhash` is the SHA-256 of a `DGMOBJ01` envelope (kind, child count,
       child object hashes, payload), so the same bytes have a second address
       and content addressing cannot deduplicate across the two stores.
       `import_node` reads the copies back one SPI statement per chunk and
       materializes a whole tree before opening it. Committing one 5,000-row
       table created 541 chunk objects. The fix is to make `pg_dolt_object`
       chunk references use the node hash and read history through the chunk
       store, so a commit records roots without copying.
     - Plan (2026-11-04, branch `zachmu/commit-tree-roots`), measured first:
       committing a one-row UPDATE took 48 ms on a 1,000-row table and
       250–290 ms on a 200,000-row one, with 123k `pg_dolt_object` lookups
       for 18 new objects. The walk, not the change, set the cost.
       - **Tree edges name nodes.** A table or index member's second child,
         and a schema or namespace tree envelope's only child, is the root's
         node hash in `pg_dolt_chunk`, the same hash its `content` field
         and `pg_dolt_root` carry. `put_object` checks such an edge with a
         chunk existence probe. The `chunk` object kind is retired: no new
         chunk objects, and `pg_dolt_object` holds metadata only.
       - **Retaining a tree writes only its unbacked nodes.** A capture or
         DDL that names a tree persists the nodes of its working store that
         the chunk store lacks, as root rows do. That is the change's size,
         not the tree's. Schema trees, built in private stores, put only the
         nodes an existence probe misses.
       - **Captures load once.** The content root a capture reads from
         `pg_dolt_root` and the delta is retained from that same load, not
         reloaded to export.
       - **History reads open roots lazily** through the node cache and the
         chunk store, as live reads do, instead of importing a copy.
       - **Bundles** carry the chunks reachable from their objects' tree
         edges, and restore puts the missing ones before the objects.
       - The tree a capture builds is not yet reused as a base; that is the
         next, separate step (see [the delta store plan](DELTA-STORE-PLAN.md)
         §7).
     - Done (2026-11-05). Same benchmark, release build, one-row UPDATE
       then `dolt_commit('-am')` on the 200,000-row table once its delta is
       folded: 264 ms → 1.2 ms. A commit right after a bulk load still costs
       ~280 ms, applying the unfolded delta; reusing that tree as the base
       followed (see [the delta store plan](DELTA-STORE-PLAN.md) §7). The first commit of the 200,000-row load fell from
       1.2 s to 0.7 s.
   - Resolve the working-state duplication found in the audit, including branch
     rename losing uncommitted work.
7. **Garbage collection.** Define the reachability and safepoint protocol, then
   delete and VACUUM.

After milestone 5, re-evaluate
[the write performance plan](WRITE-PERF-PLAN.md) steps 2 and 4 against the new
storage. Packed appends may no longer apply to chunks.
