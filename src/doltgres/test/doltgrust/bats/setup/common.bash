load setup/windows-compat
load setup/query-server-common

if [ -z "$BATS_TMPDIR" ]; then
    export BATS_TMPDIR=$HOME/batstmp/
    mkdir $BATS_TMPDIR
fi

nativebatsdir() { echo `nativepath $BATS_TEST_DIRNAME/$1`; }
batshelper() { echo `nativebatsdir helper/$1`; }

setup_common() {
    psql --version

    export PATH=$PATH:~/go/bin
    cd $BATS_TMPDIR

    # remove directory if exists
    # reruns recycle pids
    rm -rf "dolt-repo-$$"

    # Append the directory name with the pid of the calling process so
    # multiple tests can be run in parallel on the same machine
    mkdir "dolt-repo-$$"
    cd "dolt-repo-$$"
    nativevar DOLTGRUST_DATA_DIR "$(pwd)" /p

    if [ -z "$DOLT_TEST_RETRIES" ]; then
        export BATS_TEST_RETRIES="$DOLT_TEST_RETRIES"
    fi
}

teardown_common() {
    # rm -rf can fail with a "directory not empty" error in some cases. This seems to be a misleading
    # error message; the real error is that a file is still in use. Instead of waiting longer for
    # any processes to finish, we just ignore any error removing temp files, so that teardown_common
    # doesn't fail a test just because we couldn't delete any temporary test files.
    stop_sql_server 1
    local checked=0
    fsck_data_dir || checked=$?
    rm -rf "$BATS_TMPDIR/dolt-repo-$$"
    return $checked
}

# fsck_data_dir checks the test's data directory with doltgrust fsck once its server stopped, when a server
# bootstrapped it, failing on any finding or when fsck can't check it, unless the test set
# FSCK_DATA_DIR_REMOVED because it removes or replaces the directory on purpose. A server a test stopped without
# waiting gets up to 60 seconds to exit first, and a test whose server takes longer fails, since a slow stop is a
# finding of its own.
fsck_data_dir() {
    if [ -n "$FSCK_DATA_DIR_REMOVED" ] || [ ! -d "$BATS_TMPDIR/dolt-repo-$$/system" ]; then
        return 0
    fi
    local pid waited=0
    pid=$(cat "$DOLTGRUST_DATA_DIR/locks/postmaster.pid" 2>/dev/null)
    while [ -n "$pid" ] && ps -p "$pid" -o comm= 2>/dev/null | grep -q doltgrust; do
        if [ $waited -ge 600 ]; then
            echo "the server took more than 60 s to stop"
            return 1
        fi
        sleep .1
        waited=$((waited + 1))
    done
    local output
    if ! output=$(doltgrust fsck --data-dir "$DOLTGRUST_DATA_DIR" 2>&1); then
        echo "doltgrust fsck of the test's data directory failed:"
        echo "$output"
        return 1
    fi
}

query_server() {
    nativevar PGPASSWORD "password" /w
    psql -U "${SQL_USER:-postgres}" -h localhost -p $PORT "$@" postgres
}

query_server_for_db() {
    nativevar PGPASSWORD "password" /w
    local db_name=${1:-postgres}
    shift
    psql -U "${SQL_USER:-postgres}" -h localhost -p $PORT "$@" $db_name
}

log_status_eq() {
    if ! [ "$status" -eq $1 ]; then
        echo "status: expected $1, received $status"
        printf "output:\n$output"
        exit 1
    fi
}

log_output_has() {
    if ! [[ "$output" =~ $1 ]]; then
        echo "output did not have $1"
        printf "output:\n$output"
        exit 1
    fi
}

nativevar DOLT_ROOT_PATH $BATS_TMPDIR/config-$$ /p
