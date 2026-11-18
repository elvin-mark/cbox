#!/usr/bin/env bash
set -euo pipefail

BIN="${1:-./cbox}"
PASS=0

assert_eq() {
    local actual="$1"
    local expected="$2"
    local msg="$3"
    if [[ "$actual" != "$expected" ]]; then
        echo "[FAIL] $msg: expected '$expected', got '$actual'" >&2
        exit 1
    fi
    PASS=$((PASS + 1))
    echo "  [PASS] $msg"
}

assert_contains() {
    local haystack="$1"
    local needle="$2"
    local msg="$3"
    if [[ "$haystack" != *"$needle"* ]]; then
        echo "[FAIL] $msg: expected output to contain '$needle'" >&2
        echo "  Output was: $haystack" >&2
        exit 1
    fi
    PASS=$((PASS + 1))
    echo "  [PASS] $msg"
}

echo "=== Running Integration Tests ($BIN) ==="

# 1. Basic command output
out="$($BIN run -- /bin/echo "hello-toy")"
assert_contains "$out" "hello-toy" "basic echo runs inside container"

# 2. Exit code propagation
set +e
$BIN run -- /bin/sh -c 'exit 42' >/dev/null 2>&1
rc=$?
set -e
assert_eq "$rc" "42" "container exit code propagates to host"

# 3. PID 1 isolation
pid_out="$($BIN run -- /bin/sh -c 'echo "PID=$$"')"
assert_contains "$pid_out" "PID=1" "entrypoint runs as PID 1 in PID namespace"

# 4. --init PID 1 zombie reaper & exit code
set +e
$BIN run --init -- /bin/sh -c 'sleep 0.05 & exit 7' >/dev/null 2>&1
rc=$?
set -e
assert_eq "$rc" "7" "--init PID 1 reaper propagates workload exit code"

# 5. Environment variables (-e)
env_out="$($BIN run -e APP_ENV=production -e FOO=bar -- /bin/sh -c 'echo "ENV=$APP_ENV:$FOO"')"
assert_contains "$env_out" "ENV=production:bar" "-e sets environment variables inside container"

# 6. -v without -r must be refused so host '/' is not mutated
set +e
$BIN run -v /tmp:/should_fail -- /bin/true >/dev/null 2>&1
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
    echo "[FAIL] -v without -r should fail safely" >&2
    exit 1
fi
PASS=$((PASS + 1))
echo "  [PASS] -v without -r is safely refused"

# 7. --no-new-privs and --drop-caps
sec_out="$($BIN run --no-new-privs --drop-caps -- /bin/sh -c 'grep NoNewPrivs /proc/self/status')"
assert_contains "$sec_out" "NoNewPrivs:	1" "--no-new-privs sets PR_SET_NO_NEW_PRIVS=1"

# 8. OCI runtime lifecycle (create -> state -> start -> state -> kill -> delete)
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

STATE_ROOT="$TMP_DIR/state"
BUNDLE="$TMP_DIR/bundle"
mkdir -p "$BUNDLE"

cat > "$BUNDLE/config.json" <<'EOF'
{
  "ociVersion": "1.0.2",
  "hostname": "oci-test",
  "process": {
    "args": ["/bin/sleep", "30"],
    "env": ["PATH=/bin:/usr/bin", "OCI_TEST=yes"],
    "noNewPrivileges": true
  },
  "linux": {
    "namespaces": [
      {"type": "pid"},
      {"type": "ipc"},
      {"type": "uts"},
      {"type": "user"}
    ]
  }
}
EOF

CID="test-oci-1"
$BIN --root "$STATE_ROOT" create --bundle "$BUNDLE" --pid-file "$TMP_DIR/cid.pid" "$CID"

created_pid="$(cat "$TMP_DIR/cid.pid")"
if [[ -z "$created_pid" || "$created_pid" -le 0 ]]; then
    echo "[FAIL] OCI create did not write valid pid-file" >&2
    exit 1
fi
PASS=$((PASS + 1))
echo "  [PASS] OCI create wrote pid-file ($created_pid)"

state1="$($BIN --root "$STATE_ROOT" state "$CID")"
assert_contains "$state1" '"status": "created"' "OCI state reports 'created' before start"

$BIN --root "$STATE_ROOT" start "$CID"
state2="$($BIN --root "$STATE_ROOT" state "$CID")"
assert_contains "$state2" '"status": "running"' "OCI state reports 'running' after start"

$BIN --root "$STATE_ROOT" kill "$CID" SIGKILL
sleep 0.1
state3="$($BIN --root "$STATE_ROOT" state "$CID")"
assert_contains "$state3" '"status": "stopped"' "OCI state reports 'stopped' after SIGKILL"

$BIN --root "$STATE_ROOT" delete "$CID"
if [[ -d "$STATE_ROOT/$CID" ]]; then
    echo "[FAIL] OCI delete did not remove state directory" >&2
    exit 1
fi
PASS=$((PASS + 1))
echo "  [PASS] OCI delete cleaned up state directory"

echo "=== All $PASS Integration Tests Passed ==="
