#!/usr/bin/env bash
# Host test entry point. The JSON and client binaries are built by the
# Makefile; the client binary starts tests/fake_devserv itself for each case.
set -euo pipefail

json_test="${1:?test_json binary is required}"
client_test="${2:?test_client binary is required}"

echo "== test_json =="
"$json_test"
echo "== test_client =="
# The client test has an internal 15 s deadlock watchdog; keep an outer guard
# too so a stuck fixture process fails rather than hanging CI.
if command -v timeout >/dev/null 2>&1; then
    timeout 180 "$client_test"
else
    "$client_test"
fi
echo "all host tests passed"
