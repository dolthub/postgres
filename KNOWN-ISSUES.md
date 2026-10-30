# Deferred defects

User-directed deferrals from func-review.txt (2026-10-23). These remain defects,
not supported semantics. Address independently with regression tests.

* **Prepared dynamic diff result layouts:** preparing dolt_diff, changing the
  table schema and executing the plan can crash the backend. Bind result schema
  dependencies and validate runtime descriptors before writing tuple arrays.
* **Analysis-only dolt_ignore creation:** EXPLAIN/PREPARE of the first INSERT can
  create the table during parsing. The new lazy docs/tests/query-catalog table
  adapters use the same mechanism. Move lazy creation to execution.
* **dolt_query_diff row equality:** unescaped text separators permit distinct
  rows to compare equal. Replace with typed length-delimited encoding.

The SQL reproductions are in /workspace/reports/rewrite-dolt-functions-*.sql.

* **Forced branch deletion and old sessions:** ordinary deletion rejects active
  sessions, and writes after a visible deletion fail. A transaction retaining an
  older snapshot, or a session surviving forced deletion and same-name recreation,
  still needs a commit-time authority check. This is the existing-session edge
  explicitly allowed to defer in func-review.txt. Branch creation itself now
  overwrites working refs and table roots, so it does not resurrect old contents.

* **Committed chunks stored twice under two hashes (2026-10-26):** a commit
  copies every node of every table tree from `pg_dolt_chunk` into
  `pg_dolt_object` as `chunk` objects keyed by a manifest envelope hash, not the
  node hash the tree itself uses. History reads import those copies one SPI
  statement per chunk. See [chunk store plan](CHUNK-STORE-PLAN.md) milestone 6;
  the analysis is in /workspace/reports/catalog-bind-analysis/index.html.

* **Schemas fingerprinted outside the hash namespace (2026-10-30):** a member
  descriptor (table, index, routine, view, type, trigger, sequence) and the
  database object's member list carry a `schema` field holding a hex SHA-256 of
  the definition text, computed in SQL at about eight call sites. It is a second
  hash of content the descriptor already names as a `schema` object child, and
  it is part of the hashed payloads, so it fixes descriptor, database and commit
  hashes. Diff, status and merge compare it to detect schema changes. A schema
  should be a chunk like any other: stored in the chunk store and referred to
  by the manifest by its chunk address, which those comparisons then use. Found
  while moving content hashes to 20 bytes on zachmu/short-hashes, which leaves
  these fingerprints at 32.
