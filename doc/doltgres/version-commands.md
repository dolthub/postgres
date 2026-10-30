# First version-control command slice

This is native backend code, with no extension installation. Fresh initdb is
required after this catalog change. APIs are temporarily superuser-only. The default session
branch is main. The subsequent [branch/merge slice](branch-merge.md) adds checkout,
independent session data views, merge and conflict resolution.

## Reference and SQL surface

Reference: Doltgres checkout 664df6bb, especially server/functions/dolt_procedures.go,
testing/go/dolt_procedures_record_test.go and testing/go/dolt_functions_test.go;
Dolt checkout 72e0a00a1a, sqle/dprocedures/{init,dolt_commit,dolt_branch,dolt_tag}.go,
sqle/dtablefunctions/dolt_diff_summary.go and sqle/dtables/log_table.go.

Commands use variadic text arguments and named OUT columns. Both scalar SELECT
and SELECT * FROM forms work. Content addresses are 20 bytes, displayed as 32 lowercase base-32 characters
using Dolt's `0123456789abcdefghijklmnopqrstuv` alphabet, without padding.
`dolt_hash_encode(bytea)` and `dolt_hash_decode(text)` convert addresses at the
SQL boundary. Full hashes and unique prefixes of at least eight characters
resolve revisions; malformed or ambiguous hashes are rejected. This shares
Dolt's string encoding, not necessarily its content hashing or object format. Unsupported flags raise errors.

| Function | Implemented arguments and result |
|---|---|
| `dolt_add(...)` | `.`, `-a`, `-A` or `--all`; stage all supported live objects, including empty namespaces; `status bigint` = 0. |
| `dolt_commit(...)` | Required `-m/--message`; optional `-A/--all` (also `-Am message` / `-am message`), `--author 'Name <email>'`, `--committer 'Name <email>'`, `--date`, `--allow-empty`, `--skip-empty`; `hash text`, NULL for skipped empty commit. |
| `dolt_branch(...)` | `name [start-point]`; create only; `status bigint` = 0. |
| `dolt_tag(...)` | `name [start-point]`, optional `-m/--message`, `--author`; create annotated tag; `status bigint` = 0. |
| `dolt_log(...)` | Optional revision, `-n/--max-count`, `--parents`, `--show-signature`; twelve-column history result. |
| `dolt_diff_summary(from,to[,table])` | Table-level added/dropped/modified summary; five-column result. |

Log columns match the reference: commit_hash, committer, email, date, message,
commit_order, parents, refs, signature, author, author_email, author_date.
commit_order is graph height. Parents are NULL unless requested, then comma-space
separated hashes; unsigned signature is NULL unless requested, then empty text.
The `pg_catalog.dolt_log` view exposes default history. `pg_catalog.dolt_tags`
has tag_name, tag_hash (target commit), tagger, email, date, message. No branch
listing view yet. Committer defaults to the SQL role and role@client-host (localhost for local connections); dates
are rendered as UTC timestamps. Like Dolt, `--author` also becomes the committer
unless `--committer 'Name <email>'` is given.

Diff columns are from_table_name, to_table_name, diff_type, data_change,
schema_change. Names are qualified; the missing side of an added/dropped table
is empty text. Schema changes include supported index definitions, constraints,
comments and grants. Namespace-only changes affect database identity but produce
no table diff row. Optional unqualified table names must be unambiguous and must
exist in either input, even when unchanged. This is not the row-level dolt_diff.

History revisions support HEAD, branch/tag names, full hashes and a single
`~N` (first-parent distance) or `^N` (parent ordinal) suffix. Diff also accepts
STAGED and WORKING. Short hashes, compound revision expressions and ranges are
not supported. initdb initializes the repository (`dolt_init()`) in template1,
so every database starts with main refs and one repository initialization
commit recording the initial namespaces; see [system tables](system-tables.md).
Doltgres additionally records CREATE DATABASE as an
artifact of the Dolt storage library; that extra commit is deliberately not
reproduced here. Cross-engine tests accept either bootstrap count while checking
user-created history independently. Existing history is not rewritten.

## Transaction and staging contract

Commit, branch and tag calls must be standalone SELECT commands outside explicit
transaction blocks. The approved first-slice contract uses PostgreSQL's normal
statement commit rather than attempting implicit commit of an already-open SQL
transaction. A single call may be wrapped in scalar expressions (casts,
`substring`, `length`); joins, subqueries and per-row invocation are rejected. dolt_add
also requires a standalone SELECT, but can run inside BEGIN and rolls back with
its transaction. Argument arrays and prepared-statement parameters are supported.

Commit without -A records the staged snapshot. -A stages and commits all live
supported contents. Later unstaged edits survive a staged commit. WORKING in diff
means a fresh live capture, not the last persisted working/<branch> ref. Ordinary
DML is not yet wired to working refs. All immutable objects and ref changes share
one PostgreSQL transaction/WAL outcome; expected revision tokens prevent stale
publishers from overwriting a concurrent move. Concurrent callers may need to
retry the entire transaction. This is not yet three-way working-set publication.

Created branches receive head, staged and working refs at the selected state;
tags reference immutable annotated tag objects. Neither operation changes live
SQL relations. Checkout now selects independent session data views; see the branch/merge contract.

## Capture and formats

`dg_version.c` owns the command and serializer layer; `dg_manifest.c` remains the
generic immutable-object and CAS layer. Capture coordinates catalog DDL with
ShareLocks, takes AccessShareLocks on source relations, then uses one refreshed
Read Committed snapshot for metadata, table roots and index roots. Data writers
can continue; the existing AM's snapshot rules select eligible roots. Export
uses that same snapshot. Once all immutable bytes are copied, capture releases
its catalog/relation lock acquisitions. Existing caller locks are preserved.
Broad catalog locks are a prototype correctness mechanism, not the final scalable
DDL protocol; concurrent DDL/capture can deadlock and require transaction retry.
Live capture rejects Repeatable Read and Serializable until historical schema
binding is implemented. Historical log/diff reads retain their SQL snapshot.

Supported capture: empty namespaces; persistent plain prolly tables with supported
built-in column types; their prolly indexes; primary/unique/not-null constraints;
symbolic owner/ACLs and table/column/namespace comments. Default ACLs are expanded,
ACL entries and members sorted by symbolic identity, and caller search_path and
quote_all_identifiers cannot alter canonical definitions.

Rejected: heap or other-AM user relations, views, sequences, partitioned/unlogged
tables, routines, custom types/collations/operators, defaults/generated/identity
or dropped columns, column ACL/options, triggers, RLS, other constraints and
relation options. Temporary and system namespaces are outside this versioned
subset. This is deliberately not a complete PostgreSQL schema serializer.
Type admission additionally follows the AM's portable-value checks.

Canonical JSONB text is encoded as UTF-8. DGNS1 namespace descriptors contain
symbolic identity, owner, comment and ACL. DGTABLE1 schema descriptors contain
qualified name, owner/ACL/comment, ordered column definitions, sorted constraints
and index definitions. Table/index member payloads contain kind, name, schema
SHA-256 and original canonical content-root hash; their object children point to
the enveloped schema and exported chunk objects. DGDB1 contains server encoding
and members sorted by kind/name, including namespace definitions. Its children
are a hash-sorted set of descriptors; member names come from payloads, not child
position. Local OIDs, executor handles and XIDs do not enter these payloads.
DGDB1 also lists, as `extensions`, the names (without versions) of the
extensions whose objects its definitions use, derived from their references
and omitted when there are none. A server without one of them refuses to read
that root, naming the extension to install.

DGCOMMIT1 contains author/committer metadata, microsecond timestamps and graph
height; children are database state followed by ordered parents. Namespace and
tag object kinds append IDs 6 and 7 without changing existing IDs. Tag children
contain the target commit. The lazy initial empty state has no captured schema;
the first capture adds public and all other eligible namespaces.

## Remaining gates and validation

No revision-qualified historical SQL resolution, cross-branch queries, row diff,
tracked-only/named-table staging, amend, branch/tag deletion/rename/force or
history GC/import exists yet. Full schema coverage and authorization remain
future work. Physical backup preserves history; logical pg_dump omits it. Table
writers still serialize for the transaction duration; this slice does not remove
that restriction or finish Phase 3 row-level concurrency.

`src/test/modules/test_prolly/t/007_commands.pl` exercises actual servers: API
shapes, staging/rollback, author/date/parents, refs, revision traversal, diff,
unsupported inputs, old snapshots, stale publication, DML/DDL after capture,
restart after dropping source storage, OID-independent reconstructed state,
empty namespaces, comments, formatting GUCs and permissions. The other prolly
TAP suites cover storage, crash recovery, concurrency and physical backups.

Publication hardening adds `008_publication.pl`: stage and data savepoint rollback,
prepared staging visibility and restart, cancellation/crash while blocked between
HEAD and staged-ref updates, absence of partially visible objects/refs, capture
error cleanup of snapshots/GUCs, and rejection of multi-statement implicit blocks.
These boundaries passed without changing the existing publication protocol.
