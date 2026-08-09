# URL Shortener & Link Analytics

A small C++17 service that turns long URLs into short codes, redirects visitors,
and keeps click analytics. SQLite for storage, cpp-httplib for HTTP,
nlohmann/json for serialisation, doctest for tests.

---

## Quick start

**Prerequisites:** a C++17 compiler, CMake ≥ 3.16, and SQLite development headers.

```bash
# Ubuntu / Debian
sudo apt install build-essential cmake libsqlite3-dev

# macOS
brew install cmake sqlite

# Windows (vcpkg)
vcpkg install sqlite3
```

`nlohmann/json`, `cpp-httplib` and `doctest` are header-only and are fetched
automatically at configure time if they are not already installed, so the first
configure needs network access.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/url_shortener
```

The server listens on `http://localhost:8080` and writes `urlshortener.db` in
the working directory.

### Tests

```bash
cd build && ctest --output-on-failure
# or, for the full per-assertion output:
./build/tests/urlshort_tests
```

66 test cases, ~2200 assertions. They run against a real in-memory SQLite
database and, for the HTTP layer, a real server on an ephemeral port — there are
no mocks. Expect roughly 15 seconds, most of it in the concurrency and
distribution tests.

There is also `scripts/smoke_test.sh` for watching the round-trip happen with
real curl output against a running server.

### Configuration

Everything is environment-driven; a malformed value stops the process rather
than silently falling back.

| Variable | Default | Notes |
|---|---|---|
| `URLSHORT_HOST` | `0.0.0.0` | |
| `URLSHORT_PORT` | `8080` | |
| `URLSHORT_DB` | `urlshortener.db` | `:memory:` works |
| `URLSHORT_BASE_URL` | `http://localhost:$PORT` | only affects the `short_url` field |
| `URLSHORT_CODE_LENGTH` | `7` | |
| `URLSHORT_MAX_URL_LENGTH` | `2048` | |
| `URLSHORT_BLOCK_PRIVATE_HOSTS` | `true` | see [SSRF](#refusing-private-targets) |
| `URLSHORT_THREADS` | `8` | |

---

## API

### `POST /shorten`

```jsonc
{
  "url": "https://example.com/a/very/long/path",
  "custom_alias": "team-docs",   // optional; "alias" also accepted
  "force_new": false             // optional; opt out of deduplication
}
```

`201 Created` for a new link, `200 OK` when an existing one was reused. Both
carry a `Location` header.

```json
{
  "code": "aB3xY7z",
  "short_url": "http://localhost:8080/aB3xY7z",
  "url": "https://example.com/a/very/long/path",
  "normalized_url": "https://example.com/a/very/long/path",
  "custom_alias": false,
  "created_at": "2026-08-09T10:31:00Z",
  "reused": false
}
```

### `GET /{code}`

`301 Moved Permanently` with `Location: <original url>`, or `404` with a JSON
body. Records a click as a side effect.

### `GET /api/stats/{code}?days=7`

```json
{
  "code": "aB3xY7z",
  "short_url": "http://localhost:8080/aB3xY7z",
  "url": "https://example.com/a/very/long/path",
  "total_clicks": 42,
  "last_clicked_at": "2026-08-09T12:00:00Z",
  "window_days": 7,
  "top_referrers": [{ "host": "news.ycombinator.com", "clicks": 30 }],
  "clicks_by_day": [{ "day": "2026-08-09", "clicks": 12 }]
}
```

### `GET /healthz` → `{"status":"ok"}`

### Errors

Every failure is JSON: `{"error": "<machine code>", "message": "<human text>"}`.

| `error` | Status | |
|---|---|---|
| `invalid_url`, `unsupported_scheme`, `url_too_long`, `blocked_host`, `invalid_alias`, `reserved_alias`, `malformed_request` | 400 | bad input |
| `not_found` | 404 | unknown code |
| `alias_taken` | 409 | namespace conflict, retry with a different name |
| `code_exhausted` | 503 | ours, and retryable |
| `internal_error` | 500 | |

---

## Design decisions

### Why the short codes won't collide

Two mechanisms, and only the second one is load-bearing.

**Probabilistic.** Codes are 7 characters of Base62 — 62⁷ ≈ 3.52 × 10¹²
possibilities, about 41.7 bits. Each character is drawn from `std::random_device`
(a CSPRNG on every platform we target) using **rejection sampling**, because
`rng() % 62` is biased: 2³² is not a multiple of 62, so the first `2³² mod 62`
symbols would come up slightly more often. At 10⁸ stored links the keyspace is
0.003% full, so a fresh draw collides with probability ≈ 3 × 10⁻⁵.

**Structural, and this is what actually guarantees correctness.** `links.code`
carries a `UNIQUE` constraint. An insert that loses a race is *rejected by the
database* and we redraw — there is no check-then-insert window. Correctness does
not depend on the entropy argument at all; the entropy argument only explains why
the retry loop essentially never runs. `test_link_store.cpp` hammers a single
code from eight threads and asserts exactly one winner.

**Why not a counter?** A monotonic counter Base62-encoded would make collisions
structurally impossible and produce shorter codes. It also makes every link
enumerable (`/1`, `/2`, `/3`…), which leaks both the content of other people's
links and our total volume, and it needs a shared allocator the moment there is
more than one process. Unpredictability was worth 41 bits.

**Why Base62 and not base64url?** `-` and `_` survive round-trips badly: text
editors and chat clients break on them, double-click-to-select stops at them, and
they are awkward to read aloud. The knowing cost is that visually confusable
pairs (`0`/`O`, `1`/`l`) remain; a Crockford-style alphabet would fix that at the
price of a slightly smaller keyspace.

### Duplicate URLs: idempotent by default, opt out per request

| Request | Result |
|---|---|
| Same URL twice | Same code, `200`, `reused: true` |
| `force_new: true` | A second, independently tracked code, `201` |
| Custom alias | Always a new row; never dedupe-matched |

Pasting the same link twice is the most common accidental request, and returning
a stable code makes retries safe and keeps the table free of synonyms. But
"idempotent, always" would be wrong too: separate codes for the same destination
are exactly how you attribute an email campaign against a Twitter post. So the
default is the safe, cheap behaviour and the other one is one flag away.

Deduplication keys on a **normalised** URL, and normalisation only folds what
provably cannot change the response:

- scheme and host lower-cased (case-insensitive per RFC 3986)
- default ports dropped (`:80` for http, `:443` for https)
- empty path → `/`
- fragment dropped — it is never sent to the origin server
- empty query (`?`) dropped

It deliberately does **not** sort query parameters or strip `utm_*`. Both are
tempting and both are wrong: parameter order is significant to some servers, and
stripping campaign tags silently rewrites where someone's link points.
`test_url_validator.cpp` pins this so nobody "helpfully" adds it later.

### Custom aliases

3–32 characters of `[A-Za-z0-9_-]`, not starting or ending with punctuation, and
not one of the reserved names (`api`, `healthz`, `shorten`, `stats`, `favicon`, …)
that would shadow a real route. Checked case-insensitively, because a browser
will happily request `/API`.

Taking an alias that already exists is **409, even when it points at the same
URL**. Silently succeeding would tell the second caller they own a name that
someone else created and can rename or revoke.

Aliases never participate in deduplication **in either direction**: shortening a
URL that happens to have an alias gives you a fresh generated code, not
someone else's named link. The partial index `ON links (normalized_url) WHERE
custom_alias = 0` enforces this in the schema, so the policy cannot be violated
by a careless query.

### Refusing private targets

`URLSHORT_BLOCK_PRIVATE_HOSTS` defaults to **on**. A shortener that will redirect
to `169.254.169.254` or `10.0.0.5` is an SSRF gadget: it lets an outsider borrow
the trust of anything that follows short links. The check covers loopback,
RFC 1918, link-local, CGNAT, multicast, `localhost`/`.local`/`.internal`, and the
IPv6 equivalents.

This is a *first-hop* check only. It does not follow redirects or re-resolve DNS,
so a hostname that resolves publicly now and privately later still gets through —
see "what's missing" in `WRITEUP.md`.

We also reject `javascript:`, `data:` and `file:` (a shortener that redirects to
them is a stored-XSS delivery service), URLs with embedded credentials
(`https://user:pass@host` leaks them to everyone who clicks), and any URL
containing control characters or spaces (request smuggling).

### 301 versus 302

The brief asks for 301, so it is 301. It is worth naming the cost: a permanent
redirect is cached by the browser, so **repeat visits from the same browser never
reach us and never appear in the analytics**. We send `Cache-Control: no-store`
to ask browsers not to cache it, but compliance is inconsistent. If click
accuracy mattered more than the SEO/latency benefits of a permanent redirect,
302 would be the right call.

### What we do not store

No IP addresses. Referrers are reduced to a **bare host** before storage — the
path of the referring page is frequently sensitive (internal wikis, password
reset links) and we have no use for it. User agents are truncated to 512 bytes.
This is a deliberate floor on what the analytics can answer: no geography, no
unique-visitor counts.

### Data model

```sql
links  (id, code UNIQUE, original_url, normalized_url,
        custom_alias, created_at, click_count)
clicks (id, link_id → links.id ON DELETE CASCADE,
        clicked_at, referrer_host, user_agent)
```

`original_url` is what we redirect to; `normalized_url` is what we index for
deduplication. Keeping both means normalisation can get smarter later without
rewriting history or silently changing where existing links point.

`click_count` is denormalised so the common read is O(1) instead of a `COUNT(*)`
over the event log. It is only trustworthy because the insert and the increment
happen in one `BEGIN IMMEDIATE` transaction — the concurrency test asserts 200
concurrent clicks all land.

### Concurrency

One SQLite connection opened with `SQLITE_OPEN_FULLMUTEX`, plus WAL journaling
and a 5-second busy timeout. SQLite serialises access internally, which is a real
ceiling on read parallelism and a conscious choice for a 3–4 hour exercise. What
matters is that the invariants live in the schema, so no amount of concurrency
can corrupt them. A connection pool is the obvious next step.

`PRAGMA synchronous = NORMAL` trades "lose the last few click rows if the machine
loses power" for a large write speedup — acceptable for analytics, wrong if this
were billing data.

### Structure

```
include/urlshort/    public headers
src/
  config.cpp         environment parsing
  errors.cpp         ErrorCode ↔ HTTP status, in one place
  url_validator.cpp  parsing, validation, normalisation
  code_generator.cpp Base62 + rejection sampling
  sqlite_link_store.cpp   the LinkStore implementation
  shortener_service.cpp   all the policy, no HTTP
  http_api.cpp       routes and serialisation
tests/               one file per unit + an HTTP integration suite
```

`ShortenerService` knows nothing about HTTP and `LinkStore` is an interface, so
every policy decision above is directly testable without a running server. The
library is built separately from `main.cpp` so the tests link the same code the
binary runs.

---

## Publishing this repo

The exercise is marked confidential, so this repo has **no git remote
configured**. If you push it, push it somewhere private:

```bash
gh repo create url-shortener --private --source=. --remote=origin --push
# or
git remote add origin git@github.com:<you>/url-shortener.git   # private repo
git push -u origin main
```

---

See `WRITEUP.md` for how this was built, where the AI's output was overridden,
and what is still missing.
