<!--
Copyright 2026 Dolthub, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Doltgrust's tests

Tests copied from Doltgrust, the Rust Postgres server whose storage design the fork follows, so the fork's CI runs
them against the fork's server from its own tree.

- **Source:** `dolthub/doltgrust`'s `presentation` branch at commit `1cc06b328cd943dc7d26c05234e91b3bb433f591`.
- **Copied byte-identical:** `bats/` (Doltgrust's `testing/bats`), `LICENSE`, and under `crates/`, Doltgrust's tests
  (`crates/tests`: the engine-test corpus in `tests/scripts`, the server tests, the dump imports, and their testdata),
  its harness (`crates/harness`), its wire codec (`crates/pgproto`), and the dumps the imports load (`testing/dumps`,
  where the harness looks for them). `pins.tsv` lists every copied file with its
  source path and blob, so a copy that drifts from Doltgrust shows. A copied file is never edited; a newer copy
  replaces the files and their rows together.
- **The fork's own:** this workspace's `Cargo.toml`, `rust-toolchain.toml` (the fork's pin, which a tree with only the
  C keeps here), and `clippy.toml` (empty, so clippy lints the copy as Doltgrust does instead of by the fork's doors),
  and four files of the harness that replace Doltgrust's, each saying which file it replaces. Doltgrust's harness
  starts the server it tests as a separate process, as Doltgres' Go tests do, and reaches Doltgrust's engine crates
  only to parse SQL and convert client encodings, so the fork's `crates/harness/Cargo.toml` drops those crates,
  `order.rs` decides row orders with the `pg_query` crate (Postgres' own grammar), `pgx/stream.rs` converts client
  encodings with `encoding_rs`, and `prepared.rs` stops a prepared run, which isn't ported yet.
- **Running:** `.github/workflows/pgfork-ci.yml` runs them once a tree's build installs a server: `bats/` on Ubuntu
  and macOS, with `.github/workflows/pgfork/doltgrust` starting the fork's server where the tests start Doltgrust's,
  and on Ubuntu `cargo test -p doltgrust-tests`, the corpus, server tests, and dump imports, with
  `DOLTGRUST_TEST_TARGET` naming the fork's install and a template cluster, the harness's `postgres:` target, as
  Doltgrust runs them against a real Postgres. Tests of Doltgrust's own commands (`doltgrust fsck`, `--help`, and the
  like) and of Dolt's version control fail until the fork has them, on main and on a pull request alike.

## Pending

Doltgrust's suites not run here yet, each a later step:

- **The SQL server driver tests** (`crates/driver` and `integration-tests/sql-server-driver`): not copied yet, and
  mostly of Dolt's features (garbage collection, clusters).
- **The client tests** (`testing/postgres-client-tests`): Docker images that build Doltgrust's server, which need a
  form that builds the fork's instead.
- **The prepared mode** (`DOLTGRUST_TEST_PREPARED`): `crates/harness/src/prepared.rs` stops such a run until its
  binding is ported onto `pg_query`.
- **MULE_INTERNAL:** the one client encoding `encoding_rs` lacks, which a test of it reports as unsupported.
