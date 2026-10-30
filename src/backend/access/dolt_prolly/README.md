# Built-in PostgreSQL prolly access methods

This is the phase 2 single-branch integration baseline. All runtime code is C.
It is opt-in, with a deliberately limited SQL surface and substantial performance
and lifecycle limitations. Ordinary tables and system catalogs continue to use
heap. It is not a production storage engine or a complete phase 3 implementation.

## Build and use

Build and install this PostgreSQL fork normally, using Make or Meson. The AMs,
handlers and operator classes are initialized in `pg_catalog` by `initdb`; no
extension, shared module or `CREATE EXTENSION` is needed. Select prolly explicitly
with `USING dolt_prolly_table`. Heap remains the default table AM; all existing
PostgreSQL AMs and their operator classes remain available.

This catalog change requires a fresh cluster initialized by the new binary.
The catalog version was bumped; existing prototype clusters are not upgraded
in place. Bootstrap OIDs 9000–9010 are reserved for this fork's prolly objects
(with gaps for future use). Existing PostgreSQL catalog OIDs are unchanged.

The backend uses PostgreSQL's SHA-256 API and builds with or without OpenSSL.
The independently testable standalone library still uses OpenSSL. Build PostgreSQL
with `--with-libxml` (Make) or `-Dlibxml=enabled` (Meson) for XML value support.

```sql
SET max_parallel_workers_per_gather = 0;
CREATE TABLE example(id integer PRIMARY KEY, value text COLLATE "C")
  USING dolt_prolly_table;
INSERT INTO example VALUES (1, 'hello');
CREATE INDEX example_value ON example USING btree(value);
SELECT * FROM example WHERE id = 1;
```

A table-AM callback routes SQL `btree` indexes on these tables to
`dolt_prolly_index`. Other index families fail explicitly. This does not change
btree indexes on heap tables. Core catalogs supply six default operator classes
for its AM, reusing PostgreSQL's built-in operators/comparison support functions.
Custom comparator functions, nonstandard strategy operators and non-C index
collations are rejected.

The bootstrap-superuser-owned `pg_catalog.dolt_prolly_root(regclass)` diagnostic reports the
canonical content hash of a table or prolly index. PUBLIC execution is revoked by initdb;
this bypasses normal SQL row selection and must not become a hash oracle for
unauthorized tables. The function is a diagnostic, not a branch/ref API.

## Architecture and decisions

### Canonical and runtime trees

A table content tree maps a digest of canonical row bytes, SHA-256 truncated to
20 bytes as every content hash is, to an eight-byte occurrence count followed by
the packed tuple. Internal routing keys therefore stay fixed at 20 bytes; tuple payloads stay in leaves. A digest
collision between unequal rows is rejected. This multiset representation handles keyless duplicate rows and does not
change when rows are inserted in a different order. Primary-key enforcement is
performed by the unique ordered index, not by changing table content keys.

A separate runtime tree maps monotonically allocated 64-bit occurrence handles
to those 20-byte canonical row keys. Those handles are adapted into PostgreSQL ItemPointers;
they are not disk addresses. Updates allocate a new handle. The handle tree and
allocator counter are explicitly outside the canonical table root. Heap-style
XIDs, tuple padding and lock bits are not in canonical row encodings.
Explicit identifier-bearing SQL values are supported; incidental runtime identifiers are excluded from canonical encodings.
Row fetches resolve the handle to a digest, then look up the actual tuple in the
canonical content tree. Both trees physically live in the relation's chunk container.

Index keys encode indexed values in SQL order for the supported types, followed
by the canonical row digest as a tie-breaker. Values contain occurrence count and row
bytes. Null position and descending direction are part of the key encoding;
variable byte strings use escaped terminators. No TID enters the canonical index
root. This initially overstores whole rows, including columns not in the index.
Consequently updates to any column can change every index root; projection to
minimal row references is future work.

Index scans traverse the persisted ordered tree and resolve canonical row keys
to snapshot-visible table handles. Index-only scans can supply projected values,
but still consult the runtime handle map. They do not use the heap visibility map.
The initial scan implementation materializes all matching TIDs and reconciles
index rows with handles using nested loops; it is not a logarithmic point lookup.
Indexes are rebuilt from current canonical table content on mutation, with chunk
deduplication. Incremental index edits and bounded caching are future work.

### PostgreSQL pages, WAL and publication

The standalone library's `DGCHNK01` file backend remains available and tested.
The SQL adapter instead stores the same `DGPTR001` nodes in PostgreSQL-managed
relation pages. It does not open a second chunk file or publish an external
manifest. Generic WAL full-page records recover chunk fragments and root records;
PostgreSQL owns buffer writeback, WAL-before-data ordering, relation creation,
transactional truncate/drop, backup-visible files and commit durability.

Each page contains one item: a 56-byte adapter header followed by a chunk fragment
or an 80-byte root version. The header identifies record kind, chunk length,
fragment offset and SHA-256 address. A root version contains XID, command ID,
canonical content hash, runtime handle-tree hash and next handle. All encoded
integers are big-endian. This experimental container format is not frozen.

All new chunks are logged before their root version. Root records are provisional
until their transaction commits. Reads select the latest root visible to their
PostgreSQL snapshot; own writes obey command IDs. Aborted/subaborted and prepared
uncommitted roots remain invisible. Historical handle lookup supports executor
SnapshotAny requests such as DELETE RETURNING without reintroducing deleted
rows into ordinary scans.

Table and index root records, along with ordinary catalog/heap changes, share the
SQL transaction outcome. There is no independently mutable latest-root file and
no postcommit rename gap. This is the initial relation-local working-set
publication mechanism. Separate heap catalogs now provide explicit refs and
retained history objects (see below). Automatic working-set publication,
versioned schema resolution and historical SQL reads remain later work.

### Concurrency and retention

Canonical writes serialize per branch/resource for the transaction; different
branches can write shared files concurrently. A short file lock keeps each append
contiguous and is released before transaction end. Legacy unbound writes retain
table-wide serialization. Readers use immutable snapshot roots. Lock upgrades
can deadlock and PostgreSQL detects those deadlocks. See
[parallel canonical ALTER](../../../../doc/doltgres/canonical-alter.md) for the
metadata and immutable-object publication changes.
A writer encountering a committed root newer than its active snapshot raises a
serialization error instead of merging whole roots or exposing snapshot phantoms.
This is conservative rejection, not full PostgreSQL row-level conflict behavior.

All chunks and root versions are retained. There is no VACUUM/freezing, GC,
compaction or snapshot-aware reclamation. The relation retains its initial
relfrozenxid; this implementation must not be operated indefinitely across XID
wraparound. VACUUM fails explicitly, rather than pretending to advance a safe
freeze horizon. A production retention/freezing protocol is required before
long-lived deployments. Prepared-transaction recovery has a basic integration
test, not the complete phase 3 concurrency certification.

### Shared node cache

Every backend reads prolly nodes through one cache in main shared memory,
`dolt_node_cache_size` (128MB by default; 0 disables it; changing it requires a
restart), laid out like the buffer pool: slots with pin and usage counts, a
partitioned hash table from node hash to slot, and clock-sweep eviction. Node
bytes live in a dynamic shared memory area created in place in that segment.
There is no per-session node cache. The working store borrows nodes and reads
them in place, and returns every loan when the transaction ends, so idle
sessions pin nothing. It holds a transaction's unwritten nodes, so that only
the nodes reachable from the roots a transaction publishes reach the chunk
store, never its intermediate trees; at transaction end it keeps only pinned
unwritten nodes, such as the empty tree. A miss reads the chunk store, checks
the node's encoding once and adds it. A committing transaction adds the nodes
it wrote. A node the cache cannot take, because it is full of
pinned nodes, is copied privately instead. History imports leave subtrees the
chunk store already holds to be read through the cache. `dolt_node_cache_stats()`
reports the cache's use.

### Core interfaces

Two optional callbacks are added to TableAmRoutine:

* `relation_index_am`: select/validate an index AM before opclass selection.
* `relation_validate_alter`: validate ALTER TABLE before catalog changes.

The prolly table AM uses the second callback to reject tuple-schema changes whose
historical decoding has not been implemented. Metadata-only ownership changes, immediate primary/unique constraint creation,
and idempotent SET NOT NULL on already-NOT-NULL columns are allowed for ordinary
dump/restore. Other layout-changing ALTER TABLE operations remain rejected. Heap leaves both callbacks NULL.
Existing planner search, executor operators, lock tags, caches and transaction
commit logic are unchanged. Index planner integration supplies capabilities,
operator classes and a generic cost estimate. Estimates are deliberately crude.

### Tuple serialization and type coverage

`dg_codec.c` owns value serialization independently of ordered index keys. The
original boolean, int2/int4/int8, text and bytea encodings remain compatible.
Other scalar types use PostgreSQL binary send/receive functions with a `DGB1`
version envelope. These functions run with database encoding, then restore the
client's encoding even on errors. Binary XML reads use CONTENT mode so stored
fragments remain readable when a session selects DOCUMENT mode.

Supported table values include the portable PostgreSQL built-in scalar families:
booleans, integers, floats, numeric, money, character/string and binary types,
dates/times/intervals, bit strings, UUIDs, network addresses, geometric types,
JSON/JSONB/JSONPath, XML (with libxml), full-text values, and every
built-in range and multirange. Table values accept any column collation.

Arrays recursively encode dimensions, lower bounds, NULLs and element values;
named composites recursively encode their fields. Both use the versioned
`DGTUP001` packed-field envelope and avoid heap padding and embedded TOAST
pointers. Domains use their base-type codec: assignment still enforces domain
constraints, but reads do not revalidate already stored values. Enums use their
standard binary label serialization, which omits enum value OIDs. Only reviewed
built-in binary codecs may use the generic binary path. Opaque custom base-type
serializers remain rejected: a binary I/O pair alone does not establish a safe
encoding. User-defined ranges/multiranges with audited scalar subtypes are
supported. Range subtypes that delegate to PostgreSQL's array/composite binary
senders remain rejected because those senders embed incidental type OIDs.

Explicit exceptions to generic binary I/O:

* Empty `int2vector` and `oidvector` values retain the one-dimensional, zero-based catalog
  vector layout instead of the ordinary empty-array layout.
* PostgreSQL 18's binary `tsquery` receiver rejects a zero-item query. The adapter
  recognizes its exact four-byte zero-count payload and reconstructs an empty
  query; nonempty queries use the standard receiver.

History independence is about serialized value contents under fixed schema and
format definitions, not SQL equality. Numeric scales and signed floating-point
zeros can retain distinct bytes. Comparisons, ordering and uniqueness still
belong to PostgreSQL's operators and the index adapter.

Explicit user `oid` values are stored numerically and ordered as unsigned 32-bit
integers. The same applies inside domains, arrays, records and supported ranges.
Their numeric contents are not translated: changing a user-provided OID changes
the stored value. Incidental type OIDs in container headers are still stripped;
canonical metadata still uses symbolic identities.

These formats are experimental and tied to this PostgreSQL/type-codec version.
Explicit user values also support every `reg*` alias, `aclitem`,
`pg_node_tree`, `xid`, `xid8`, `cid`, `tid`, `pg_lsn`,
`txid_snapshot`, `pg_snapshot`, and `refcursor`, including nested
containers and catalog composites. These values retain their supplied identifiers;
they do not imply portable resolution of those identifiers on another server.
This permission does not extend to incidental identifiers in tuple/container
headers or canonical schema metadata. It also does not imply ordered index
support for every such type.

`aclitem` has no native binary I/O pair: its `DGA1` encoding stores a
four-byte marker, two unsigned 32-bit role identifiers, and an unsigned 64-bit
privilege mask, all integers in network byte order. `pg_node_tree` stores its
explicit text payload directly because its native receiver rejects input.

Validation recursively checks domains, array elements and composite fields at
table creation and before encoding/decoding or rebuilding an index. It validates
the schema even for NULL fields and empty arrays, and repeats the check after
nested type changes. Existing tables using now-rejected types are not migrated
or translated; affected reads/writes fail explicitly. Original data is not
rewritten. Changing a supported composite type's schema after storing values
still lacks historical schema decoding.

Enums, portable arrays/composites/domains and built-in ranges remain supported:
their storage codecs omit in-memory type/value OIDs. Plain integers, text,
bytea, UUIDs and other ordinary data are not inspected for numbers or bytes that
happen to resemble identifiers. Runtime TID/OID machinery outside canonical
contents remains necessary for PostgreSQL integration.

Incoming TOAST values are detoasted before serialization, including nested
values. Canonical payloads remain inline in the tuple leaf, fragmented over
relation pages as needed. The PLAN's content-addressed out-of-line payload chunks
are a separate, pending implementation. The standalone codec's 16 MiB packed
item limit remains; PostgreSQL's maximum varlena size is not yet supported.

## Supported and deferred surface

Tested: permanent tables with the value types described above; NULL and empty
values; duplicate rows; large inline canonical values; sequential scans;
ordered and index-only scans; simple multicolumn indexes with ASC/DESC, NULL
ordering and INCLUDE; immediate unique/primary-key constraints;
insert/update/delete and RETURNING; joins; transaction/savepoint rollback;
transactional TRUNCATE; retained reader snapshots and crash recovery.

Ordered index keys support integers, explicit OIDs, booleans, text/bytea,
floating-point/numeric, date/time/interval, UUID, money, MAC addresses, enums,
bit/varbit, inet/cidr, JSONB, records, ranges and multiranges. Arrays and domains
of supported types are supported recursively. Keys preserve PostgreSQL ordering
and normalize SQL-equal values for uniqueness independently of row serialization.
Non-C index collations and custom comparators remain rejected, including nested
record field/range collations. JSONB uses the database default collation and is
currently indexable only when that collation is C-like. Ranges require the
subtype's default comparison procedure. `json` has no native B-tree comparison
family and remains unindexable by this ordered AM.

Internal-only types `gtsvector`, `pg_ndistinct`, `pg_dependencies`, `pg_mcv_list`,
`pg_brin_bloom_summary` and `pg_brin_minmax_multi_summary` are explicitly rejected.
They do not provide general value I/O round trips; copying opaque native bytes
would not establish a portable storage codec. Pseudo-types are also unsupported;
PostgreSQL itself disallows ordinary table columns of these types.

Other explicitly unsupported features: expression/partial indexes, other index
families, deferred uniqueness, ON CONFLICT, row locking, concurrent index builds,
SERIALIZABLE isolation, parallel scans, TABLESAMPLE, ANALYZE, VACUUM/FULL, CLUSTER
and most ALTER TABLE. CTID/tableoid are available; heap transaction system
columns are not supplied by virtual slots. Temporary/unlogged tables are
rejected. Runtime tuple-version chain following is not implemented. General
concurrent update/recheck behavior, foreign keys, logical decoding/replication
and complete DDL coverage are not claimed.

Roots contain table/index contents, not the external canonical schema identity;
hash comparisons assume the same schema and encoding contract. Standalone Dolt
branch/diff/merge APIs are not yet exposed through SQL by these access methods.

## Tests

```
make -C /path/to/build/src/test/modules/test_prolly check
# Or, with Meson:
meson test -C /path/to/build --suite setup --suite test_prolly
```

The SQL regression covers supported operations and explicit rejections. TAP tests
start separate real servers, compare mixed edit histories with heap results,
check table/index canonical roots, retain a reader snapshot across another
backend's commit, reject a stale writer, exercise prepared mixed heap/prolly
transactions, and restart after immediate shutdown with committed, aborted and
fragmented data. Run the full core regression suite after changes to the policy
callbacks. The standalone C tree/file tests remain in `src/doltgres/prolly`.

`003_types.pl` audits built-in scalar/range coverage against `pg_type`, compares
heap and prolly values, tests arrays (including empty, multidimensional and
non-default lower bounds), composites/domains/enums, mixed edit histories,
index-only payloads, and WAL crash recovery. It also checks database/client
encoding isolation, XML parsing settings, binary float/numeric fidelity, and
unchanged domain constraint enforcement. XML cases run when PostgreSQL was built
with libxml.

`004_portability.pl` retains rejection tests for opaque custom senders and
range senders containing incidental array type identities.
`039_explicit_identifiers.pl` verifies explicit identifiers through domains and
nested composites, history and restart, preserves vector-domain identity, and
checks that incidental composite type OIDs do not affect serialized roots.

`005_native.pl` verifies built-in availability without extension installation,
unchanged heap/btree defaults and availability of upstream AMs, diagnostic
permissions, cross-type integer predicates, ownership changes and dump/restore
into a fresh database. The full core regression suite also checks catalog
consistency and ordinary heap/index behavior.

## Source layout

* `src/backend/access/dolt_prolly/`: runtime adapter and core build integration.
* `src/doltgres/prolly/`: independently testable C prolly library, compiled into
  the backend through small wrapper translation units.
* `src/include/catalog/pg_{am,proc,opfamily,opclass,amop,amproc}.dat`: additive
  built-in object definitions. The three integer opclasses share a complete
  cross-type integer family; boolean, text and bytea have separate families.
* `src/test/modules/test_prolly/`: SQL and TAP tests, with no installable module.

Registering another ordered operator family makes it visible to PostgreSQL's
existing equivalence-class planning. Complete cross-type integer membership
preserves normal integer equivalences. Tests that deliberately extend only the
btree family with incomplete custom alias operators can have different plans;
no planner algorithm or upstream AM implementation is changed by this packaging.

## Manifest foundation

The backend now also provides heap-backed per-database object/ref catalogs and
privileged `dolt_manifest_*` building blocks. See
[the publication contract](../../../../doc/doltgres/manifest.md) for formats,
APIs, snapshot/CAS behavior, recovery tests and boundaries. The exporter retains
canonical table/index chunks independently of their source relations. The
[first command slice](../../../../doc/doltgres/version-commands.md) builds limited
automatic schema capture, staging, commit, branch/tag creation, log and table diff
summary on these APIs. Checkout and live working-set publication on ordinary DML
remain future work. The table writer lock remains unchanged.
Physical backups include history; logical pg_dump currently omits it. This
catalog revision requires fresh initdb.


### Branch and merge milestone

Native checkout now selects independent session data roots. Native `dolt_merge`
handles fast-forward and unchanged-schema three-way data merges, returning
non-aborting conflict status with durable resolution state. See
`doc/doltgres/branch-merge.md` for the current contract, broad temporary schema
freeze, whole-table unique-conflict fallback, and verification. Earlier milestone
notes above describe their historical scope. This does not replace table writer
serialization or complete branch-specific schema catalogs. Fresh initdb is needed
for branch-tagged relation root records and the new built-in APIs/views.
