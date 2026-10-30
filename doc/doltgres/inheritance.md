# Versioned inherited tables

Traditional PostgreSQL inheritance uses the native CREATE/ALTER machinery over
provisional branch catalog tuples. Canonical table definitions store ordered
parent names, column `attislocal`/`attinhcount`, and constraint inheritance flags.
The provider reconstructs pg_inherits and dependency rows and derives each
parent's relhassubclass flag. No additional physical catalog or per-revision
storage resource is allocated for inheritance.

PostgreSQL expands parent scans and DML into parent/child relations. Every child
uses its own branch-bound schema and prolly roots; ONLY excludes descendants.
Historical parent references expand within the historical catalog context.

Recursive ALTER maintains separate source/destination state for each affected
table. Native validation and rewrites use that table's original tuple layout.
Publication installs the changed definitions, remapped rows and rebuilt indexes
under one SQL transaction. Column renames include grandchildren; native DROP
COLUMN/CONSTRAINT handles inheritance and CASCADE instead of the earlier generic
dependency-only deletion shortcut. Different selected branches can perform these
operations concurrently. Branch administration retains its existing locking.

A table-wise merge can combine individually valid changes into an invalid graph
(for example, a newly created child and an independently added parent column).
The merger checks inherited layouts, constraints and cycles. On incompatibility
it reports a schema conflict and retains the target's connected inheritance
component. Resolving that conflict with --theirs replaces the component's schema
and data together. It does not synthesize missing inherited columns or apply
new defaults to incoming child rows. Independent, compatible changes merge
normally. Group resolution intentionally selects the entire component, including
its data, rather than only the table named by the conflict row.

This implements traditional inheritance for supported permanent prolly tables.
It does not add declarative partitioning, temporary/unlogged prolly storage,
expression/partial indexes, exclusion constraints, or additional privilege DDL.
Those existing limitations also apply to inherited tables.

Coverage lives in test_prolly/047_inheritance.pl and Doltgres's
TestCreateTableInherit, TestPostgresInheritanceDDL and
TestPostgresForkVersionedInheritance. The native DDL test runs unchanged against
stock PostgreSQL and the fork. PostgreSQL's unchanged inherit regression also
passes with heap storage and its required tenk1 indexes present.
