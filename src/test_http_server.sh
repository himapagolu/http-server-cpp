#!/usr/bin/env bash
#
# Test cases for the HTTP server implemented so far:
#   GET  /
#   GET  /echo/{str}
#   GET  /user-agent
#   GET  /files/{filename}
#   POST /files/{filename}
#   404 fallback for unknown routes
#
# Usage:
#   Run it straight from the repo, no separate build step needed:
#     ./src/test_http_server.sh
#   It compiles src/main.cpp itself (g++ by default; set CXX to override)
#   and cleans up the binary and temp dirs when it's done.
#
set -u

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT=4221
HOST=127.0.0.1
FILES_DIR="$(mktemp -d)"
OUTSIDE_DIR="$(mktemp -d)"
SERVER_PID=""
PASS=0
FAIL=0

cleanup() {
  if [ -n "$SERVER_PID" ]; then
    kill "$SERVER_PID" >/dev/null 2>&1
    wait "$SERVER_PID" 2>/dev/null
  fi
  rm -rf "$FILES_DIR" "$OUTSIDE_DIR" "$ROOT_DIR/http-server"
}
trap cleanup EXIT

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }

assert_eq() {
  local desc="$1" expected="$2" actual="$3"
  if [ "$expected" == "$actual" ]; then
    pass "$desc"
  else
    fail "$desc (expected [$expected], got [$actual])"
  fi
}

assert_contains() {
  local desc="$1" haystack="$2" needle="$3"
  if [[ "$haystack" == *"$needle"* ]]; then
    pass "$desc"
  else
    fail "$desc (expected to find [$needle] in response)"
  fi
}

# ---------------------------------------------------------------------------
# Build + start server
# ---------------------------------------------------------------------------
echo "Building..."
CXX="${CXX:-g++}"
BUILD_LOG="$(mktemp)"
"$CXX" -std=c++17 -pthread "$ROOT_DIR/main.cpp" -lz -o "$ROOT_DIR/http-server" >"$BUILD_LOG" 2>&1 \
  || { echo "Build failed, see $BUILD_LOG"; cat "$BUILD_LOG"; exit 1; }

"$ROOT_DIR/http-server" --directory "$FILES_DIR" &
SERVER_PID=$!
sleep 0.5

# ---------------------------------------------------------------------------
# 1. GET / -> 200 OK
# ---------------------------------------------------------------------------
echo "Test: GET /"
resp=$(curl -s -o /dev/null -w "%{http_code}" http://$HOST:$PORT/)
assert_eq "GET / returns 200" "200" "$resp"

# ---------------------------------------------------------------------------
# 2. GET /echo/{str} -> 200, body echoes str
# ---------------------------------------------------------------------------
echo "Test: GET /echo/abc"
body=$(curl -s http://$HOST:$PORT/echo/abc)
assert_eq "GET /echo/abc body" "abc" "$body"

echo "Test: GET /echo/abc headers"
headers=$(curl -s -D - -o /dev/null http://$HOST:$PORT/echo/abc)
assert_contains "GET /echo/abc has Content-Type: text/plain" "$headers" "Content-Type: text/plain"

echo "Test: GET /echo/ (empty string)"
body=$(curl -s http://$HOST:$PORT/echo/)
assert_eq "GET /echo/ body is empty" "" "$body"

echo "Test: GET /echo/with spaces (URL-encoded)"
body=$(curl -s "http://$HOST:$PORT/echo/hello%20world")
assert_eq "GET /echo/hello%20world returns raw encoded string (no decoding implemented)" "hello%20world" "$body"

# ---------------------------------------------------------------------------
# 3. GET /user-agent -> 200, body echoes User-Agent header
# ---------------------------------------------------------------------------
echo "Test: GET /user-agent"
body=$(curl -s -A "test-agent/1.0" http://$HOST:$PORT/user-agent)
assert_eq "GET /user-agent echoes header" "test-agent/1.0" "$body"

# ---------------------------------------------------------------------------
# 4. POST /files/{filename} -> 201 Created, file written to disk
# ---------------------------------------------------------------------------
echo "Test: POST /files/test.txt"
status=$(curl -s -o /dev/null -w "%{http_code}" -X POST --data-binary "hello from test" http://$HOST:$PORT/files/test.txt)
assert_eq "POST /files/test.txt returns 201" "201" "$status"

if [ -f "$FILES_DIR/test.txt" ]; then
  content=$(cat "$FILES_DIR/test.txt")
  assert_eq "file written with correct content" "hello from test" "$content"
else
  fail "file was not created on disk"
fi

echo "Test: POST /files/test.txt overwrite"
curl -s -o /dev/null -X POST --data-binary "overwritten content" http://$HOST:$PORT/files/test.txt
content=$(cat "$FILES_DIR/test.txt")
assert_eq "file overwritten on second POST" "overwritten content" "$content"

echo "Test: POST /files/nested/path.txt (no subdirectory created)"
status=$(curl -s -o /dev/null -w "%{http_code}" -X POST --data-binary "nested" http://$HOST:$PORT/files/nested/path.txt)
if [ -f "$FILES_DIR/nested/path.txt" ]; then
  pass "nested file created (unexpected, no mkdir logic present)"
else
  fail "nested path silently failed (ofstream can't create missing subdir 'nested/') - status was $status, no error surfaced to client"
fi

# ---------------------------------------------------------------------------
# 5. GET /files/{filename} -> 200 with file content, or 404 if missing
# ---------------------------------------------------------------------------
echo "Test: GET /files/test.txt (exists)"
status=$(curl -s -o /dev/null -w "%{http_code}" http://$HOST:$PORT/files/test.txt)
assert_eq "GET /files/test.txt returns 200" "200" "$status"

body=$(curl -s http://$HOST:$PORT/files/test.txt)
assert_eq "GET /files/test.txt returns correct content" "overwritten content" "$body"

headers=$(curl -s -D - -o /dev/null http://$HOST:$PORT/files/test.txt)
assert_contains "GET /files/test.txt has octet-stream content-type" "$headers" "Content-Type: application/octet-stream"

echo "Test: GET /files/does_not_exist.txt (missing)"
status=$(curl -s -o /dev/null -w "%{http_code}" http://$HOST:$PORT/files/does_not_exist.txt)
assert_eq "GET /files/does_not_exist.txt returns 404" "404" "$status"

# ---------------------------------------------------------------------------
# 6. Directory traversal safety check (currently NOT protected against)
# ---------------------------------------------------------------------------
# OUTSIDE_DIR and FILES_DIR are sibling temp dirs (same parent), so this is a
# real containment check rather than a guess at repo layout.
echo "outside-secret" > "$OUTSIDE_DIR/secret.txt"
OUTSIDE_NAME="$(basename "$OUTSIDE_DIR")"

echo "Test: GET /files/../{outside_dir}/secret.txt (path traversal)"
body=$(curl -s "http://$HOST:$PORT/files/../$OUTSIDE_NAME/secret.txt")
if [ "$body" == "outside-secret" ]; then
  fail "path traversal is possible: escaped FILES_DIR and read a sibling directory (no sanitization of filename)"
else
  pass "path traversal blocked (server did not return the outside file's contents)"
fi

# ---------------------------------------------------------------------------
# 7. Unknown route -> 404
# ---------------------------------------------------------------------------
echo "Test: GET /nonexistent"
status=$(curl -s -o /dev/null -w "%{http_code}" http://$HOST:$PORT/nonexistent)
assert_eq "GET /nonexistent returns 404" "404" "$status"

headers=$(curl -s -D - -o /dev/null http://$HOST:$PORT/nonexistent)
assert_contains "404 response has Content-Length: 0" "$headers" "Content-Length: 0"

# ---------------------------------------------------------------------------
# 8. Large body upload (exceeds fixed 1024-byte recv buffer)
# ---------------------------------------------------------------------------
echo "Test: POST /files/large.txt with body > 1024 bytes"
large_body=$(head -c 2000 </dev/zero | tr '\0' 'a')
curl -s -o /dev/null -X POST --data-binary "$large_body" http://$HOST:$PORT/files/large.txt
if [ -f "$FILES_DIR/large.txt" ]; then
  written_size=$(wc -c < "$FILES_DIR/large.txt" | tr -d ' ')
  if [ "$written_size" == "2000" ]; then
    pass "large (2000 byte) body written fully"
  else
    fail "large body truncated: expected 2000 bytes, got $written_size (recv() only reads a fixed 1024-byte buffer once, doesn't loop until Content-Length is satisfied)"
  fi
else
  fail "large.txt was not created"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "============================="
echo "Passed: $PASS   Failed: $FAIL"
echo "============================="

[ "$FAIL" -eq 0 ]
