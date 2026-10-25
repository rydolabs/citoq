#!/usr/bin/env bash
set -euo pipefail

BIN="$1"
NAME="/cito-shm-smoke-$$"
COUNT=100

cleanup() {
  if [[ -n "${WRITER_PID:-}" ]]; then
    kill "${WRITER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${READER1_PID:-}" ]]; then
    kill "${READER1_PID}" 2>/dev/null || true
  fi
  if [[ -n "${READER2_PID:-}" ]]; then
    kill "${READER2_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

"$BIN" writer "$NAME" "$COUNT" >shm-writer.log 2>&1 &
WRITER_PID=$!

sleep 0.1

"$BIN" reader "$NAME" "$COUNT" >shm-reader1.log 2>&1 &
READER1_PID=$!

"$BIN" reader "$NAME" "$COUNT" >shm-reader2.log 2>&1 &
READER2_PID=$!

status=0
wait "$READER1_PID" || status=1
wait "$READER2_PID" || status=1
wait "$WRITER_PID" || status=1

cat shm-writer.log
cat shm-reader1.log
cat shm-reader2.log

grep -q "writer sent=100" shm-writer.log || status=1
grep -q "reader received=100 dropped=0" shm-reader1.log || status=1
grep -q "reader received=100 dropped=0" shm-reader2.log || status=1

exit "$status"
