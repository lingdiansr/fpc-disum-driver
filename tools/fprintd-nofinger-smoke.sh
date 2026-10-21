#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# No-finger fprintd smoke test for the built libfprint.
#
# It uses a private D-Bus session and a test-only polkit authority, so it does
# not touch the system daemon, PAM, or /var/lib/fprint. The test covers device
# enumeration, empty-storage list/delete, verify-without-print, enroll
# cancellation, and daemon restart. A successful enrollment/verify requires a
# finger and is intentionally not part of this script.
#
# Usage:
#   tools/fprintd-nofinger-smoke.sh [BUILD_DIR]
#   FPC_LIBFPRINT_DIR=/path/to/build/libfprint tools/fprintd-nofinger-smoke.sh
#
# BUILD_DIR defaults to _build; libfprint is expected in BUILD_DIR/libfprint.

set -u

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- "$0" "$@"
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${1:-"$ROOT/_build"}
LIBFPRINT_DIR=${FPC_LIBFPRINT_DIR:-"$BUILD_DIR/libfprint"}
FPRINTD_BIN=${FPRINTD_BIN:-/usr/lib/fprintd}
USER_NAME=${FPRINTD_USER:-$(id -un)}
PYTHON=${PYTHON:-python3}
LOG_DIR=$(mktemp -d "${TMPDIR:-/tmp}/fpc-fprintd-nofinger.XXXXXX")
MOCK_LOG="$LOG_DIR/mock-polkit.log"
DAEMON_LOG="$LOG_DIR/fprintd.log"
FAILURES=0
MP=
FP=

cleanup() {
    [ -n "${FP:-}" ] && kill "$FP" 2>/dev/null
    [ -n "${MP:-}" ] && kill "$MP" 2>/dev/null
    wait "$FP" 2>/dev/null
    wait "$MP" 2>/dev/null
    rm -rf "$LOG_DIR"
}
trap cleanup EXIT

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    FAILURES=$((FAILURES + 1))
}

pass() {
    printf 'ok: %s\n' "$*"
}

wait_for_name() {
    local name=$1
    for _ in $(seq 1 50); do
        if busctl --address="$DBUS_SESSION_BUS_ADDRESS" list 2>/dev/null | grep -q "$name"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

start_daemon() {
    LD_LIBRARY_PATH="$LIBFPRINT_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$FPRINTD_BIN" -t >"$DAEMON_LOG" 2>&1 &
    FP=$!
    wait_for_name net.reactivated.Fprint || {
        fail "fprintd did not register net.reactivated.Fprint"
        return 1
    }
}

stop_daemon() {
    if [ -n "${FP:-}" ]; then
        kill "$FP" 2>/dev/null
        wait "$FP" 2>/dev/null
        FP=
    fi
}

if [ ! -x "$FPRINTD_BIN" ]; then
    echo "fprintd binary not found: $FPRINTD_BIN" >&2
    exit 2
fi
if [ ! -e "$LIBFPRINT_DIR/libfprint-2.so.2" ]; then
    echo "built libfprint not found under: $LIBFPRINT_DIR" >&2
    exit 2
fi
command -v busctl >/dev/null || { echo "busctl is required" >&2; exit 2; }
command -v fprintd-list >/dev/null || { echo "fprintd-list is required" >&2; exit 2; }

export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"

"$PYTHON" "$ROOT/tools/mock-polkit.py" >"$MOCK_LOG" 2>&1 &
MP=$!
wait_for_name org.freedesktop.PolicyKit1 || {
    echo "test polkit service did not start; see $MOCK_LOG" >&2
    exit 2
}

start_daemon || exit 1

# 1. The daemon sees the FPC device.
out=$(fprintd-list "$USER_NAME" 2>&1)
if [ $? -eq 0 ] && grep -q 'found 1 devices' <<<"$out"; then
    pass "device enumeration"
else
    fail "device enumeration: $out"
fi

# 2. Empty storage is reported, not treated as a fatal daemon error.
out=$(fprintd-list "$USER_NAME" 2>&1)
if [ $? -eq 0 ] && grep -qi 'no fingers enrolled' <<<"$out"; then
    pass "empty storage list"
else
    fail "empty storage list: $out"
fi

out=$(fprintd-delete "$USER_NAME" right-index-finger 2>&1)
if [ $? -eq 0 ] && grep -qi 'No fingerprints to delete' <<<"$out"; then
    pass "delete with no prints"
else
    fail "delete with no prints: $out"
fi

# 3. Starting an enrollment without touching must be cancellable; the daemon
#    must remain usable afterwards.
out=$(timeout -s INT 4 fprintd-enroll "$USER_NAME" 2>&1)
rc=$?
if [ "$rc" -eq 124 ] || [ "$rc" -eq 1 ]; then
    pass "enroll cancelled before touch (rc=$rc)"
else
    fail "enroll cancellation returned rc=$rc: $out"
fi
out=$(fprintd-list "$USER_NAME" 2>&1)
if [ $? -eq 0 ] && grep -q 'found 1 devices' <<<"$out"; then
    pass "device usable after enroll cancellation"
else
    fail "device unusable after enroll cancellation: $out"
fi

# 4. Verify without an enrolled print is a normal NoEnrolledPrints error.
out=$(timeout 15 fprintd-verify "$USER_NAME" 2>&1)
rc=$?
if [ "$rc" -ne 0 ] && grep -qi 'NoEnrolledPrints\|no fingers' <<<"$out"; then
    pass "verify without enrolled print rejected"
else
    fail "verify without enrolled print returned rc=$rc: $out"
fi

# 5. Restarting the daemon must re-enumerate the same device.
stop_daemon
start_daemon || exit 1
out=$(fprintd-list "$USER_NAME" 2>&1)
if [ $? -eq 0 ] && grep -q 'found 1 devices' <<<"$out"; then
    pass "device re-enumerated after daemon restart"
else
    fail "device missing after daemon restart: $out"
fi

# Record, but do not fail on, known fprintd cancellation diagnostics. The
# device must still be usable (checked above).
if grep -q 'CRITICAL' "$DAEMON_LOG"; then
    printf 'note: fprintd logged CRITICAL during cancellation (see log)\n'
fi

if [ "$FAILURES" -ne 0 ]; then
    echo "no-finger fprintd smoke: $FAILURES failure(s)" >&2
    echo "daemon log:" >&2
    sed -n '1,120p' "$DAEMON_LOG" >&2
    exit 1
fi

echo "no-finger fprintd smoke: all checks passed"
