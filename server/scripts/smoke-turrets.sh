#!/usr/bin/env bash
# Smoke test for the M3 stub endpoint. Assumes the server is already
# running on :8182. Mirrors what `make verify` does for /turrets.
set -euo pipefail

EMAIL="smoke+$(date +%s)@example.com"
PW='smoke test password'
BASE='http://localhost:8182'

echo "==> signup ($EMAIL)"
curl -sS -o /dev/null -w "  status: %{http_code}\n" \
  -X POST "$BASE/auth/signup" \
  -H 'Content-Type: application/json' \
  -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}"

echo "==> login"
LOGIN_RESP=$(curl -sS -X POST "$BASE/auth/login" \
  -H 'Content-Type: application/json' \
  -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}")
TOKEN=$(echo "$LOGIN_RESP" | sed -E 's/.*"token":"([^"]+)".*/\1/')
if [ -z "$TOKEN" ]; then
  echo "  FAIL: no token in response: $LOGIN_RESP"
  exit 1
fi
echo "  token (truncated): ${TOKEN:0:30}..."

echo "==> GET /turrets without token (expect 401)"
curl -sS -o /dev/null -w "  status: %{http_code}\n" "$BASE/turrets"

echo "==> GET /turrets with token (expect 200, empty array)"
RESP=$(curl -sS -w "\n%{http_code}" "$BASE/turrets" \
  -H "Authorization: Bearer $TOKEN")
BODY=$(echo "$RESP" | head -n -1)
STATUS=$(echo "$RESP" | tail -n 1)
echo "  status: $STATUS"
echo "  body:   $BODY"
if [ "$STATUS" != "200" ]; then
  echo "  FAIL: expected 200, got $STATUS"
  exit 1
fi
if ! echo "$BODY" | grep -q '"turrets":\[\]'; then
  echo "  FAIL: expected empty turrets array, got: $BODY"
  exit 1
fi
echo "  OK"
