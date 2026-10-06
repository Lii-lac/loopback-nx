#!/bin/bash
# Runs the update engine against a fake release server (tests/update_server.py): good and bad releases, a private repository, a cancel.
#   docker run --rm -v "<project>:/src" -w /src ubuntu:24.04 bash tests/run_update_test.sh
set -u
apt-get update -qq >/dev/null
apt-get install -y -qq gcc python3 libcurl4-openssl-dev >/dev/null 2>&1 || { echo "could not install the tools"; exit 1; }
PORT=18765
gcc -Wall -Wextra -Wno-deprecated-declarations -O1 -g -pthread -DUPD_ALLOW_HTTP -DUPD_REPO_URL="\"http://127.0.0.1:$PORT/Lii-lac/loopback-nx\"" \
    -o /tmp/update_test tests/update_test.c source/update.c -lcurl || exit 1

FAIL=0
# run <server mode> <running version> <expected check state> [<expected install state> [cancel]]
run() {
    local mode=$1 ver=$2; shift 2
    echo "== $mode (running $ver)"
    python3 tests/update_server.py "$mode" $PORT &
    local srv=$!
    sleep 0.7
    rm -rf /tmp/ut; mkdir -p /tmp/ut
    /tmp/update_test "$ver" /tmp/ut "$@" > /tmp/ut.out 2>&1 || FAIL=1
    sed 's/^/  /' /tmp/ut.out
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
}

run good     1.0.0 available ready
run good     1.0.1 current
run good     1.1.0 current
run badhash  1.0.0 available failed
run notnro   1.0.0 available failed
run nodl     1.0.0 available failed
run nosha    1.0.0 available failed
run private  1.0.0 failed
run empty    1.0.0 failed
run slow     1.0.0 available available cancel

[ $FAIL -eq 0 ] && echo "ALL OK" || echo "FAILURES"
exit $FAIL
