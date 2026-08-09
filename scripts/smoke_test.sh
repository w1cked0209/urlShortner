#!/usr/bin/env bash
#
# End-to-end smoke test against a running server. The automated suite already
# covers all of this; this script exists so a human can watch the round-trip
# happen with real curl output.
#
#   ./build/url_shortener &
#   ./scripts/smoke_test.sh
#
set -euo pipefail

BASE="${BASE:-http://localhost:8080}"
pass=0
fail=0

check() {
  local label="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  ok   %-52s %s\n' "$label" "$actual"
    pass=$((pass + 1))
  else
    printf '  FAIL %-52s expected %s, got %s\n' "$label" "$expected" "$actual"
    fail=$((fail + 1))
  fi
}

json_field() { grep -o "\"$1\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" | head -1 | sed 's/.*: *"//; s/"$//'; }

echo "==> health"
check "GET /healthz" 200 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/healthz")"

echo "==> shorten"
body=$(curl -s -X POST "$BASE/shorten" -H 'Content-Type: application/json' \
       -d '{"url":"https://example.com/a/very/long/path?utm_source=smoke"}')
code=$(echo "$body" | json_field code)
echo "  code = $code"
[[ -n "$code" ]] || { echo "  FAIL no code returned"; exit 1; }

echo "==> redirect"
check "GET /$code status"   301 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/$code")"
check "GET /$code location" "https://example.com/a/very/long/path?utm_source=smoke" \
      "$(curl -s -o /dev/null -D - "$BASE/$code" | grep -i '^location:' | tr -d '\r' | cut -d' ' -f2)"

echo "==> unknown code"
check "GET /zzzzzzz" 404 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/zzzzzzz")"

echo "==> duplicate url is idempotent"
second=$(curl -s -X POST "$BASE/shorten" -H 'Content-Type: application/json' \
         -d '{"url":"https://example.com/a/very/long/path?utm_source=smoke"}')
check "same code returned" "$code" "$(echo "$second" | json_field code)"

echo "==> force_new mints a different code"
forced=$(curl -s -X POST "$BASE/shorten" -H 'Content-Type: application/json' \
         -d '{"url":"https://example.com/a/very/long/path?utm_source=smoke","force_new":true}')
forced_code=$(echo "$forced" | json_field code)
if [[ "$forced_code" != "$code" && -n "$forced_code" ]]; then
  printf '  ok   %-52s %s\n' "distinct code" "$forced_code"; pass=$((pass + 1))
else
  printf '  FAIL %-52s got %s\n' "distinct code" "$forced_code"; fail=$((fail + 1))
fi

echo "==> custom alias"
alias_name="smoke-$RANDOM"
check "POST alias" 201 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/shorten" \
      -H 'Content-Type: application/json' \
      -d "{\"url\":\"https://example.com/docs\",\"custom_alias\":\"$alias_name\"}")"
check "GET /$alias_name" 301 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/$alias_name")"
check "alias conflict" 409 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/shorten" \
      -H 'Content-Type: application/json' \
      -d "{\"url\":\"https://example.com/other\",\"custom_alias\":\"$alias_name\"}")"

echo "==> validation"
check "bad url"        400 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/shorten" \
      -H 'Content-Type: application/json' -d '{"url":"not a url"}')"
check "javascript:"    400 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/shorten" \
      -H 'Content-Type: application/json' -d '{"url":"javascript:alert(1)"}')"
check "reserved alias" 400 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/shorten" \
      -H 'Content-Type: application/json' -d '{"url":"https://example.com","custom_alias":"api"}')"

echo "==> analytics"
curl -s -o /dev/null -H 'Referer: https://news.example.org/story' "$BASE/$code"
curl -s -o /dev/null -H 'Referer: https://news.example.org/other' "$BASE/$code"
stats=$(curl -s "$BASE/api/stats/$code")
echo "  $stats" | tr -d '\n' | head -c 400; echo
clicks=$(echo "$stats" | grep -o '"total_clicks"[^,]*' | grep -o '[0-9]*')
if [[ "${clicks:-0}" -ge 3 ]]; then
  printf '  ok   %-52s %s clicks\n' "clicks recorded" "$clicks"; pass=$((pass + 1))
else
  printf '  FAIL %-52s got %s\n' "clicks recorded" "${clicks:-none}"; fail=$((fail + 1))
fi

echo
echo "passed: $pass   failed: $fail"
[[ "$fail" -eq 0 ]]
