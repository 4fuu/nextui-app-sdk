#!/usr/bin/env bash
# Host test entry point. The JSON and client binaries are built by the
# Makefile; the client binary starts tests/fake_devserv itself for each case.
set -euo pipefail

json_test="${1:?test_json binary is required}"
client_test="${2:?test_client binary is required}"

echo "== test_json =="
"$json_test"
echo "== test_client =="
"$client_test"
echo "all host tests passed"
