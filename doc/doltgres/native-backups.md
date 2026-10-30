# Native file backups

`dolt_backup('sync-url', 'file:///absolute/directory')` writes
`manifest.bundle`; `add`/`sync` support named destinations.
`dolt_backup('restore', [ '--force', ] url, database)` restores a database.
The transfer format is fork-native, with no Dolt/NBS interoperability contract.
SQL commands and their semantics follow Doltgres.

A bundle contains canonical objects with their hashes, payload bytes and ordered
child hashes, all refs, freshly captured working sets for every branch, and
shared sequence allocation states (including definitions retained only in
history). Staged roots and commits retain their existing identities. Native
extension names, versions and installation schemas accompany the graph; those
extensions must be installed on the destination server. Runtime OIDs, relation
locators and ref CAS tokens are freshly allocated there. Explicit user OID
values remain user data and retain the existing non-portability contract.

Objects are read using one PostgreSQL statement snapshot. Sequence allocation
is deliberately non-MVCC, just as in ordinary PostgreSQL. The file has a format
marker and checksum, is written to a temporary file, fsynced, and atomically
renamed. Import verifies the checksum, every object hash, child existence and
kind, and graph acyclicity before publishing refs. This initial container is
buffered in memory, includes retained unreachable objects, and must fit below
PostgreSQL's single-allocation limit (1 GiB including JSON/base64 overhead).
Streaming, compression, reachability filtering and incremental transfers are
future transport optimizations, not changes to canonical object identity.

Restore uses internal background workers because a PostgreSQL connection
cannot switch databases. It creates a staging database with connections
disabled, imports the graph, and rebuilds schema bindings, table/index resources
and sequence counters through the canonical publisher. A final transaction
publishes the requested database name. With `--force`, an existing inactive
database is renamed in that same transaction and dropped in a later transaction;
failed validation/import leaves it unchanged. Restore refuses the connected
database, and native PostgreSQL checks protect databases used by other sessions.
Cancellation stops the worker before cleanup. A server crash can leave an
inaccessible staging database, or a replaced database awaiting cleanup; these
are identifiable by the `dolt_restore_` / `dolt_replaced_` prefixes.

Sync and restore require a standalone statement outside an explicit transaction,
as they publish external files or independently committed databases. The existing
superuser requirement applies. Destination encoding must match. Referenced roles
and extension binaries must already exist. This backs up versioned database
contents, not cluster configuration, roles, unversioned heap tables, or arbitrary
physical catalogs; use PostgreSQL physical backup tools for a complete cluster.
Custom casts depending on versioned types remain rejected by canonical DDL and
therefore cannot yet participate in a backup fixture.

Validation: native `045_backups` covers independent clusters, multiple dirty
branches, commit identity, historical reads, current and historical sequence
allocators, restart, force replacement and corrupt-file isolation. Doltgres
`TestDoltBackup` exercises the SQL contract, with native reconnects instead of
`USE`, native extension installation, and SQLSTATE-based FK diagnostics.
