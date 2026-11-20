#!/usr/bin/env bats
load $BATS_TEST_DIRNAME/setup/common.bash

setup() {
    setup_common
}

teardown() {
    teardown_common
}

@test 'doltgrust: --help' {
    # just a smoke test
    doltgrust --help
}

@test 'doltgrust: --config-help' {
    # just a smoke test
    doltgrust --config-help
}

@test 'doltgrust: no arguments' {
    PORT=5432
    mkdir test-home
    # TODO: DOLT_ROOT_PATH behavior overrides the HOME behavior, which is confusing and not
    # applicable to Doltgrust, fix it
    HOME=test-home DOLTGRUST_DATA_DIR='' DOLT_ROOT_PATH='' doltgrust > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500

    cat server.out
    echo "$output"
    [ "$status" -eq 0 ]
    
    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"

    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false

    # databases should get created in home/doltgrust/databases by default
    [ -d test-home/doltgrust/databases/postgres ]
}

@test 'doltgrust: data-dir param' {
    PORT=5432
    DOLTGRUST_DATA_DIR=fake doltgrust --data-dir test > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500

    cat server.out
    echo "$output"
    [ "$status" -eq 0 ]
    
    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"

    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false

    [ -d test/postgres ]
}

@test 'doltgrust: data dir in env var' {
    PORT=5432
    DOLTGRUST_DATA_DIR=test doltgrust > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500

    cat server.out
    echo "$output"
    [ "$status" -eq 0 ]
    
    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"

    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false

    [ -d test/postgres ]
}

@test 'doltgrust: implicit config.yaml' {
    PORT=5434

    cat > config.yaml <<EOF
log_level: warn

behavior:
  read_only: false
  disable_client_multi_statements: false
  dolt_transaction_commit: false

user:
  name: "postgres"
  password: "password"

listener:
  host: localhost
  port: $PORT
  read_timeout_millis: 28800000
  write_timeout_millis: 28800000

data_dir: test
EOF

    doltgrust > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500

    cat server.out
    echo "$output"
    [ "$status" -eq 0 ]
    
    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"

    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false

    [ -d test/postgres ]
}

@test 'doltgrust: config.yaml without data dir' {
    PORT=5434

    cat > config.yaml <<EOF
log_level: warn

behavior:
  read_only: false
  disable_client_multi_statements: false
  dolt_transaction_commit: false

user:
  name: "postgres"
  password: "password"

listener:
  host: localhost
  port: $PORT
  read_timeout_millis: 28800000
  write_timeout_millis: 28800000

EOF

    mkdir test-home
    HOME=test-home DOLTGRUST_DATA_DIR='' DOLT_ROOT_PATH='' doltgrust --config config.yaml > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500

    cat server.out
    echo "$output"
    [ "$status" -eq 0 ]
    
    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"

    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false

    [ -d test-home/doltgrust/databases/postgres ]
}

@test 'doltgrust: config file override with explicit config.yaml' {
    PORT=5434

    cat > config-test.yaml <<EOF
log_level: warn

behavior:
  read_only: false
  disable_client_multi_statements: false
  dolt_transaction_commit: false

user:
  name: "postgres"
  password: "password"

listener:
  host: localhost
  port: $PORT
  read_timeout_millis: 28800000
  write_timeout_millis: 28800000

data_dir: test
EOF

    # The only supported override right now is the data dir, add more here as we add more overrides
    doltgrust --config config-test.yaml --data-dir local-override > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500

    cat server.out
    echo "$output"
    [ "$status" -eq 0 ]
    
    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"

    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false

    [ ! -d test/postgres ]
    [ -d local-override/postgres ]
}

@test 'doltgrust: config file override with implicit config.yaml' {
    PORT=5434

    cat > config.yaml <<EOF
log_level: warn

behavior:
  read_only: false
  disable_client_multi_statements: false
  dolt_transaction_commit: false

user:
  name: "postgres"
  password: "password"

listener:
  host: localhost
  port: $PORT
  read_timeout_millis: 28800000
  write_timeout_millis: 28800000

data_dir: test
EOF

    # The only supported override right now is the data dir, add more here as we add more overrides
    doltgrust --data-dir local-override > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500

    cat server.out
    echo "$output"
    [ "$status" -eq 0 ]
    
    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"

    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false

    [ ! -d test/postgres ]
    [ -d local-override/postgres ]
}

@test 'doltgrust: config file' {
    PORT=$( definePORT )
    CONFIG=$( defineCONFIG $PORT )
    echo "$CONFIG" > config.yaml

    cat config.yaml
    start_sql_server_with_args -config config.yaml > log.txt 2>&1
    
    run cat log.txt
    [[ ! "$output" =~ "Author identity unknown" ]] || false
    [ -d "postgres" ]

    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"
    
    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false
}

@test 'doltgrust: config file with all options' {
    PORT=$( definePORT )
    cat > config.yaml <<EOF
log_level: warn

behavior:
  read_only: false
  disable_client_multi_statements: false
  dolt_transaction_commit: false

user:
  name: "postgres"
  password: "password"

listener:
  host: localhost
  port: $PORT
  read_timeout_millis: 28800000
  write_timeout_millis: 28800000
  tls_key: null
  tls_cert: null
  require_secure_transport: null
  allow_cleartext_passwords: null

performance:
  query_parallelism: null

data_dir: .

cfg_dir: .doltcfg

metrics:
  labels: {}
  host: null
  port: -1

remotesapi: {}

privilege_file: .doltcfg/privileges.db

auth_file: .doltcfg/auth.db

branch_control_file: .doltcfg/branch_control.db

user_session_vars: []

jwks: []
EOF

    cat config.yaml

    start_sql_server_with_args -config config.yaml

    query_server -c "create table t1 (a int primary key, b int)"
    query_server -c "insert into t1 values (1,2)"
    
    run query_server -c "select * from t1" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "1 | 2" ]] || false

    run test -f ".doltcfg/auth.db"
    [ "$status" -eq 0 ]
}

@test 'doltgrust: DOLTGRUST_DATA_DIR set to current dir' {
    [ ! -d "postgres" ]
    export DOLTGRUST_DATA_DIR="$(pwd)"
    start_sql_server > log.txt 2>&1

    run cat log.txt
    [[ ! "$output" =~ "Author identity unknown" ]] || false
    [ -d "postgres" ]

    run query_server -c "\l"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "postgres" ]] || false
}

@test 'doltgrust: user name and pass via env' {
    export DOLTGRUST_USER="myuser"
    export DOLTGRUST_PASSWORD="mypass"

    [ ! -d "auth.db" ]
    
    start_sql_server "" log.txt myuser mypass
    cat log.txt

    # db matches user name since DOLTGRUST_DB was not set
    query_server_for_user_and_pass myuser mypass myuser -c "create table myTable (a int);"

    run query_server_for_user_and_pass myuser mypass myuser -c "insert into mytable values (1), (2)"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "INSERT" ]] || false

    run query_server_for_user_and_pass postgres password myuser -c "insert into mytable values (1), (2)"
    [ "$status" -ne 0 ]
}

@test 'doltgrust: default db via env' {
    [ ! -d "auth.db" ]
    
    start_sql_server mydb log.txt myuser
    cat log.txt

    query_server_for_user_and_pass myuser password mydb -c "create table myTable (a int);"

    run query_server_for_user_and_pass myuser password mydb -c "insert into mytable values (1), (2)"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "INSERT" ]] || false

    run query_server_for_user_and_pass postgres password mydb -c "insert into mytable values (1), (2)"
    [ "$status" -ne 0 ]
}

query_server_for_user_and_pass() {
    user=$1
    pass=$2
    db=$3
    shift
    shift
    shift

    nativevar PGPASSWORD "$pass" /w
    psql -U "$user" -h localhost -p $PORT "$@" $db
}

# Test for https://github.com/dolthub/doltgresql/issues/1863
@test 'doltgrust: connection to non-existent database fails' {
    start_sql_server

    # Connecting to the default postgres database should work
    run query_server -c "SELECT 1"
    [ "$status" -eq 0 ]

    # Connecting to a non-existent database should fail
    nativevar PGPASSWORD "password" /w
    run psql -U postgres -h localhost -p $PORT -c "SELECT 1" nonexistent_db
    [ "$status" -ne 0 ]
    [[ "$output" =~ "does not exist" ]] || false
}

@test 'doltgrust: CREATE SCHEMA works on valid database' {
    start_sql_server

    # CREATE SCHEMA should work on a valid database
    run query_server -c "CREATE SCHEMA test_schema_bats"
    [ "$status" -eq 0 ]

    # Verify schema was created
    run query_server -c "SELECT schema_name FROM information_schema.schemata WHERE schema_name = 'test_schema_bats'"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "test_schema_bats" ]] || false
}

@test 'doltgrust: SET CONSTRAINTS warns only outside a transaction block' {
    start_sql_server

    run query_server -c "SET CONSTRAINTS ALL IMMEDIATE"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "WARNING:  SET CONSTRAINTS can only be used in transaction blocks" ]] || false

    run query_server -c "SET CONSTRAINTS ALL IMMEDIATE; SELECT 1"
    [ "$status" -eq 0 ]
    [[ ! "$output" =~ "WARNING" ]] || false
}

@test 'doltgrust: refuses a Doltgres data directory' {
    mkdir -p doltgres-data/postgres/.dolt/noms
    run doltgrust --data-dir doltgres-data
    [ "$status" -ne 0 ]
    [[ "$output" =~ "FATAL:  database files are incompatible with server" ]] || false
    [[ "$output" =~ "DETAIL:  The data directory \"doltgres-data\" holds Doltgres databases (postgres)." ]] || false
    [[ "$output" =~ "doltgrust-convert doltgres-data" ]] || false
    [ ! -e doltgres-data/locks ]
    [ ! -e doltgres-data/postgresql.conf ]
}

@test 'doltgrust: configuration files' {
    PORT=$( definePORT )
    cat > config.yaml <<EOF
listener:
  port: $PORT
data_dir: test
system_variables:
  random_page_cost: 3
EOF
    doltgrust > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500
    [ "$status" -eq 0 ]
    [ -f test/postgresql.conf ]
    [ -f test/postgresql.auto.conf ]

    run query_server -c "SHOW DateStyle" -c "SHOW random_page_cost" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "ISO, MDY" ]] || false
    [[ "$output" =~ "3" ]] || false

    stop_sql_server 1
    mkdir test/conf.d
    echo "work_mem = '16MB'" > test/conf.d/memory.conf
    printf "include_dir = 'conf.d'\nrandom_page_cost = 5\nseq_page_cost = 2\n" >> test/postgresql.conf
    PORT=$(grep port: config.yaml | awk '{print $2}')
    doltgrust > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500
    [ "$status" -eq 0 ]

    run query_server -c "SHOW work_mem" -c "SHOW random_page_cost" -c "SHOW seq_page_cost" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "16MB" ]] || false
    [[ "$output" =~ "3" ]] || false
    [[ "$output" =~ "2" ]] || false
}

@test 'doltgrust: configuration file errors stop the server' {
    echo "work_mem = 1" > postgresql.conf
    run doltgrust --data-dir .
    [ "$status" -ne 0 ]
    [[ "$output" =~ "1 kB is outside the valid range for parameter \"work_mem\" (64 kB .. 2147483647 kB)" ]] || false
    [[ "$output" =~ "FATAL:  configuration file \"$(pwd -P)/postgresql.conf\" contains errors" ]] || false

    echo "no_such_setting = 1" > postgresql.conf
    run doltgrust --data-dir .
    [ "$status" -ne 0 ]
    [[ "$output" =~ "unrecognized configuration parameter \"no_such_setting\" in file \"$(pwd -P)/postgresql.conf\" line 1" ]] || false
}

@test 'doltgrust: SIGHUP reloads the configuration files' {
    skiponwindows "Windows has no SIGHUP"
    start_sql_server
    echo "work_mem = '24MB'" >> postgresql.conf
    echo "reflog_max_entries = 10" >> postgresql.conf
    kill -HUP $SERVER_PID
    sleep 1

    run query_server -c "SHOW work_mem" -c "SHOW reflog_max_entries" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "24MB" ]] || false
    [[ "$output" =~ "10" ]] || false
}

@test 'doltgrust: storage settings' {
    start_sql_server
    run grep -E "^shared_buffers = [0-9]+MB$" postgresql.conf
    [ "$status" -eq 0 ]

    run query_server -A -t -c "SHOW delta_memory" -c "SHOW materialize_timeout" -c "SHOW reflog" -c "SHOW reflog_max_entries" -c "SHOW reflog_max_age"
    [ "$status" -eq 0 ]
    [ "$output" = "$(printf -- '-1\n30s\non\n5000\n-1')" ]
}

@test 'doltgrust: idle timeouts end the session' {
    start_sql_server
    printf 'SET idle_session_timeout = 300;\n\\! sleep 1\nSELECT 1;\n' > idle.sql
    run query_server -f idle.sql
    [[ "$output" =~ "FATAL:  terminating connection due to idle-session timeout" ]] || false

    printf 'BEGIN;\nSET idle_in_transaction_session_timeout = 300;\n\\! sleep 1\nSELECT 1;\n' > idle.sql
    run query_server -f idle.sql
    [[ "$output" =~ "FATAL:  terminating connection due to idle-in-transaction timeout" ]] || false

    printf 'SET transaction_timeout = 600;\nBEGIN;\n\\! sleep 1\nSELECT 1;\n' > idle.sql
    run query_server -f idle.sql
    [[ "$output" =~ "FATAL:  terminating connection due to transaction timeout" ]] || false

    run query_server -c "SET idle_session_timeout = 300" -c "SELECT 1"
    [ "$status" -eq 0 ]
}

@test 'doltgrust: statement_timeout and cancel requests stop a lock wait' {
    start_sql_server
    query_server -c "CREATE TABLE lk (a int PRIMARY KEY)"
    printf 'BEGIN;\nLOCK lk;\n\\! sleep 4\nCOMMIT;\n' | query_server -q > /dev/null 2>&1 &
    sleep 1

    run query_server -c "SET statement_timeout = 300" -c "BEGIN" -c "LOCK lk"
    [[ "$output" =~ "ERROR:  canceling statement due to statement timeout" ]] || false

    psql -U postgres -h localhost -p $PORT -c "BEGIN" -c "LOCK lk" postgres > cancel.out 2>&1 &
    CANCELED=$!
    sleep 1
    kill -INT $CANCELED
    wait $CANCELED || true
    run cat cancel.out
    [[ "$output" =~ "ERROR:  canceling statement due to user request" ]] || false
}

@test 'doltgrust: garbage collection settings' {
    start_sql_server
    run query_server -A -t -c "SHOW gc_target_percent" -c "SHOW gc_young_size" -c "SHOW gc_min_free_disk" -c "SHOW gc_hard_free_disk" -c "SHOW gc_io_limit"
    [ "$status" -eq 0 ]
    [ "$output" = "$(printf -- '50\n-1\n-1\n-1\n0')" ]

    echo "gc_young_size = '64MB'" >> postgresql.conf
    kill -HUP $SERVER_PID
    sleep 1

    run query_server -A -t -c "SHOW gc_young_size"
    [ "$status" -eq 0 ]
    [ "$output" = "64MB" ]
}

@test 'doltgrust: shows its version' {
    run doltgrust --version
    [ "$status" -eq 0 ]
    [[ "$output" =~ ^"doltgrust (Doltgrust) "[0-9]+\.[0-9]+\.[0-9]+", PostgreSQL 18."[0-9]+$ ]] || false
    run doltgrust -V
    [ "$status" -eq 0 ]
    [[ "$output" =~ "doltgrust (Doltgrust)" ]] || false
}

@test 'doltgrust: a new data directory takes the time zone TZ names' {
    PORT=$( definePORT )
    printf 'listener:\n  port: %s\n' $PORT > tz.yaml
    TZ=Europe/Berlin doltgrust --data-dir tzdata --config tz.yaml > server.out 2>&1 &
    SERVER_PID=$!
    run wait_for_connection $PORT 7500
    [ "$status" -eq 0 ]
    run grep -E "^(log_)?timezone = 'Europe/Berlin'$" tzdata/postgresql.conf
    [ "${#lines[@]}" -eq 2 ]
    run query_server -c "SHOW timezone" -t
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Europe/Berlin" ]] || false
}
