#!/usr/bin/env bats
load $BATS_TEST_DIRNAME/setup/common.bash

setup() {
    setup_common
}

teardown() {
    teardown_common
}

@test 'fsck: a data directory with rewritten, merged, keyless, collated, and every kind of indexed table checks clean' {
    start_sql_server
    query_server -v ON_ERROR_STOP=1 <<'SQL'
CREATE TABLE r (id INT PRIMARY KEY, v INT, t TEXT);
INSERT INTO r SELECT g, g, 't' || g FROM generate_series(1, 1000) g;
ALTER TABLE r ALTER COLUMN v TYPE BIGINT;
CREATE TABLE k (a INT, b TEXT);
INSERT INTO k SELECT g % 7, 'x' FROM generate_series(1, 500) g;
CREATE INDEX k_a ON k (a);
CREATE TABLE c (id INT PRIMARY KEY, name TEXT COLLATE "en-US-x-icu");
INSERT INTO c SELECT g, 'Name ' || g FROM generate_series(1, 1000) g;
CREATE INDEX c_name ON c (name);
CREATE TABLE g (id INT PRIMARY KEY, p POINT, n INT, d TSVECTOR, h TEXT);
INSERT INTO g SELECT x, point(x, x % 50), x, to_tsvector('word' || (x % 30) || ' other'), 'h' || x FROM generate_series(1, 1000) x;
CREATE INDEX g_p ON g USING gist (p);
CREATE INDEX g_sp ON g USING spgist (p);
CREATE INDEX g_n ON g USING brin (n);
CREATE INDEX g_d ON g USING gin (d);
CREATE INDEX g_h ON g USING hash (h);
CREATE TABLE tr (id INT PRIMARY KEY);
INSERT INTO tr SELECT generate_series(1, 500);
TRUNCATE tr;
INSERT INTO tr VALUES (1);
SELECT dolt_commit('-Am', 'base');
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
INSERT INTO r SELECT g, g, 'o' FROM generate_series(1001, 1500) g;
UPDATE c SET name = 'Other ' || id WHERE id % 3 = 0;
INSERT INTO k SELECT g % 5, 'o' FROM generate_series(1, 300) g;
SELECT dolt_commit('-am', 'other');
SELECT dolt_checkout('main');
INSERT INTO r SELECT g, g, 'm' FROM generate_series(1501, 2000) g;
DELETE FROM c WHERE id % 3 = 1;
INSERT INTO k SELECT g % 5, 'm' FROM generate_series(1, 200) g;
SELECT dolt_commit('-am', 'main');
SELECT dolt_merge('other');
SQL
    stop_sql_server 1
    run doltgrust fsck --data-dir .
    log_status_eq 0
    log_output_has "0 findings"
}

@test 'fsck: tables and types a role owns check clean' {
    start_sql_server
    query_server -v ON_ERROR_STOP=1 <<'SQL'
CREATE USER regress_fsck_owner;
CREATE TABLE owned (id INT PRIMARY KEY);
ALTER TABLE owned OWNER TO regress_fsck_owner;
SQL
    stop_sql_server 1
    run doltgrust fsck --data-dir .
    log_status_eq 0
    log_output_has "0 findings"
}

@test 'fsck: a data directory with a stash checks clean' {
    start_sql_server
    query_server -v ON_ERROR_STOP=1 <<'SQL'
CREATE TABLE t (id INT PRIMARY KEY);
SELECT dolt_commit('-Am', 'base');
INSERT INTO t VALUES (1);
SELECT dolt_stash('push', 'stash1');
SQL
    stop_sql_server 1
    run doltgrust fsck --data-dir .
    log_status_eq 0
    log_output_has "0 findings"
    start_sql_server
    run query_server -t -A -c "SELECT count(*) FROM dolt_stashes;"
    log_status_eq 0
    log_output_has "1"
}
