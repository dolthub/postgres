-- Copyright 2026 Dolthub, Inc.
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- The test extension's functions of the modules that link simc, the Rust test crate, which meson puts in the
-- extension's script only where those modules build.

CREATE FUNCTION doltgres_cport_test_std(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_std', 'doltgres_cport_test_std'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_std_runner(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_std', 'doltgres_cport_test_std_runner'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_chunks(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_chunks', 'doltgres_cport_test_chunks'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_simc(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_simc', 'doltgres_cport_test_simc'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_trees(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_trees', 'doltgres_cport_test_trees'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_repo(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_repo', 'doltgres_cport_test_repo'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_txn(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_txn', 'doltgres_cport_test_txn'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_doltgres(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_doltgres', 'doltgres_cport_test_doltgres'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_store(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_store', 'doltgres_cport_test_store'
LANGUAGE C;
