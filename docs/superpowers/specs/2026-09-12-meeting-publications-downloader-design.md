# Weekly meeting publications downloader

**Date:** 2026-09-12
**Status:** Design v2 (revised after adversarial review)
**Target:** CrossPoint Reader firmware, `x4pro` build target (UC8179 panel, ESP32-S3, 8MB PSRAM)
**Delivery:** `origin` (victorstein/crosspoint-x4pro), branch `feature/meeting-publications`
**Depends on:** nothing. Shares no file with the other two specs.

## Goal

Fetch the current week's meeting publications — the Watchtower Study edition and the Life and
Ministry Meeting Workbook — onto the SD card as EPUBs, in Spanish, without a computer.

## Non-goals

- **Browsing other weeks.** Current week only (decided).
- **A general publication picker** (`nwt`, `sjj`, `bhs`, …). Deferred.
- **Audio, video, PDF, JWPUB.** EPUB only.

---

## Link handling: external links exist, and are already inert

An earlier draft claimed these EPUBs contain **zero** external links. That was measured on
`w_E_202609.epub` alone, which is the one issue in the set that happens to have none. Across the
files this feature actually downloads:

| EPUB | external hrefs |
| --- | --- |
| `w_E_202609.epub` | 0 |
| `w_E_202607.epub` | **44** (`https://www.jw.org/finder?lank=…`) |
| `w_S_202607.epub` | **44** |
| `mwb_S_202609.epub` | **13** |

It is issue-dependent, not language-dependent. `jwpub://` is genuinely absent everywhere.

**No work is needed, but for a different reason than "there are none."**
`ChapterHtmlSlimParser.cpp:110-118` (`isInternalEpubLink`) rejects `http://`, `https://`,
`mailto:`, `ftp:`, `tel:` and `javascript:`, and `:922` gates link collection on it. External
hrefs therefore never reach `FootnoteEntry` or `navigateToHref()` — they render as plain text
and are not tappable. The internal citation machinery is untouched: `epub:type="noteref"` links
into `*-extracted.xhtml` are the dominant shape (164 in `w_E_202609.epub`'s
`OEBPS/2026560.xhtml`) and already work.

That 164-citation density is also why this feature depends in practice on
`2026-09-12-reader-return-stack-design.md`, even though the two share no file.

## The chain, verified end to end

Exercised live on 2026-09-12. **Nothing redirects** (empty `redirect_url` on all three).

**1. Device clock → ISO year and week.** See "Clock" below — this is the step the design
under-budgeted.

**2. ISO week → the week's issues.**
`GET https://wol.jw.org/en/wol/meetings/r1/lp-e/2026/37` → 200, **31,977 bytes**, under
"Other Meeting Publications":

```
/en/wol/library/…/watchtower/the-watchtower-2026/study-edition/july
/en/wol/library/…/meeting-workbooks/life-and-ministry-meeting-workbook-2026/september
```

→ Watchtower `202607`, Workbook `202609`. Exact, not a guessed offset: the page marks the study
article `pub-w docId-2026482`, and `w_E_202607.epub` was downloaded and confirmed to contain
`OEBPS/2026482.xhtml`.

**3. Issue → EPUB URL.**
`GET https://b.jw-cdn.org/apis/pub-media/GETPUBMEDIALINKS?output=json&pub=w&langwritten=S&fileformat=EPUB&issue=202607`
→ 958 B (the `mwb` response is 1,015 B). Confirmed Spanish targets:

| pub | issue | pubName | bytes |
| --- | --- | --- | --- |
| `w` | 202607 | La Atalaya (ed. estudio) Julio de 2026 | 3,718,294 |
| `mwb` | 202609 | Guía de actividades … Septiembre y octubre de 2026 | 3,335,824 |

`GETPUBMEDIA.json` **404s**; `docid=`-keyed EPUB lookups **404**. The issue number is the only
usable key.

**4. Download.** `HttpDownloader::downloadToFile(url, dest, progress, &cancelFlag)`
(`HttpDownloader.h:44-46`).

### Why the English page is fetched regardless of download language

The week→issue mapping is language-independent — the week of Sept 7 studies the July 2026
Watchtower in every language.

*Correcting an earlier claim:* a Spanish meetings page **does** exist — it is
`https://wol.jw.org/es/wol/meetings/**r4/lp-s**/2026/37` (200), not `r1/lp-s` (404), and it
yields `la-atalaya-2026/edición-de-estudio/julio` + `guía-de-actividades-2026/septiembre`, the
same issues. English is still the right choice, but because it keeps the scanner on ASCII
month names and needs no per-language rsconf table — not because no Spanish path exists.

## Design

### Never template the year into the scan prefix

**The year in the path is the publication's year, not the ISO year of the week.** Swept across
all 53 weeks of ISO-2026:

```
2026/01  → the-watchtower-2025/study-edition/october      + …workbook-2025/november
2026/02  → the-watchtower-2025/study-edition/november     + …workbook-2026/january
2026/37  → the-watchtower-2026/study-edition/july         + …workbook-2026/september
```

**Nine of 53 weeks (~17%, roughly 1 Jan – early March) reference the previous year.** A prefix
built as `the-watchtower-<deviceISOyear>/` finds nothing for two months every year — a failure
no September testing would ever surface.

**Scan the year-less prefixes** `the-watchtower-` and `life-and-ministry-meeting-workbook-`,
then parse four ASCII digits, `/`, and (for the Watchtower) `study-edition/`, then the month
name. The issue is `<parsed year><month>`. Week 2026/01 therefore yields `w` = `202510` and
`mwb` = `202511` — both confirmed to exist (`w_S_202510.epub` 3,729,863 B;
`mwb_S_202511.epub` 3,528,093 B).

### Each publication is independently optional

Week **2026/14** (Memorial week, 30 Mar – 5 Apr 2026) renders "Other Meeting Publications" with
a **Watchtower link only** — no workbook — at 29,002 bytes instead of ~32,000. `2027/01` is
likewise workbook-less. This recurs annually.

So phases are `Downloading(n of m)` with `m` derived from what the scan found, and a missing
workbook is a **normal outcome with an informational note**, not the "could not determine this
week's publications" error. That error is reserved for finding *neither*.

### Scanning the page

Streamed through `HttpDownloader::fetchUrl(url, DataCallback)` (`HttpDownloader.h:38-39`) with
a tail buffer of `kOverlap = 64` bytes carried across chunks.

**64 is sufficient, with 15 bytes to spare.** Longest full match is
`life-and-ministry-meeting-workbook-2026/september` = 49 bytes, +1 for the closing quote = 50;
a tail must retain ≥ 49. **If the prefix is ever widened** to include
`/all-publications/meeting-workbooks/` for disambiguation, the match becomes 85 bytes and 64
breaks silently — so the constant and the prefix must be changed together.

Disambiguation is not currently needed: on the live page each prefix occurs **exactly once**,
with no nav, breadcrumb or next-week duplicate, and the month is always a lowercase English
name across all 53 weeks — never a quarter or a date range.

**Two traps in the callback:**
- **Never return `false` to stop early.** `HttpDownloader.cpp:100` / `:258` turn that into
  `FILE_ERROR`, so a scanner that exits once it has both matches reports a network failure.
  Consume all 32 KB and always return `true`.
- **Nothing may ever add an `Accept-Encoding` header.** `SecureHttpClient` sends none
  (`SecureHttpClient.h:445-454`), so the server replies identity — verified: no header →
  `content-length: 31977`; with `Accept-Encoding: gzip` → 6,082 gzipped bytes. There is no
  decompressor, so adding that header would hand the scanner compressed bytes and break it
  silently. Chunked transfer *is* decoded (`readChunked`), and a `CrossPoint-ESP32-<version>`
  User-Agent is already sent.

### JSON

The parser is `lib/JsonParser/StreamingJsonParser.{h,cpp}` — `test/streaming_json_parser/` is
only its GTest suite. It is a **pure SAX tokenizer** (`onKey`/`onString`/`onNumber` function
pointers, so no `std::function` bloat) with **no path addressing**, so the caller must maintain
its own depth/path state machine. This is real work, not free reuse.

**The obvious shortcut is wrong:** `pubImage.url` appears *before* `files` in the response, so a
last-key-wins or first-`url`-wins matcher picks up an empty string. The wanted fields are
`files.<LANG>.EPUB[0].file.url` and `files.<LANG>.EPUB[0].filesize`, and the object also carries
`file.stream`, `file.checksum`, `trackImage.url` and `trackImage.checksum`. `TOKEN_BUF_SIZE = 512`
comfortably holds the ~52-character URL.

### Clock — new HAL work, not "already exists"

An earlier draft treated the date as available. It is not:

- **`HalClock` exposes no date.** Its entire surface is `begin()`, `isAvailable()`,
  `getTime(uint8_t& hour, uint8_t& minute)`, `formatTime(...)`, `syncFromNTP()`
  (`lib/hal/HalClock.h:19-43`). Hours and minutes only. The underlying
  `freeink::Rtc::DateTime` does carry `year`/`month`/`day`; `HalClock` never surfaces it.
- **Nothing seeds system time from the RTC at boot** — `settimeofday` appears nowhere in the
  tree. `syncFromNTP()` only goes SNTP → system → RTC. So `time(nullptr)` is 1970 on any boot
  that has not run a sync.
- **`clockHasBeenSynced` (`CrossPointSettings.h:225`, not `:223`) is the wrong gate** — it
  persists as 1 from a previous boot while the current boot's system time is still 1970.

Required: add a date accessor to `HalClock` backed by `Rtc::now()`, and gate on
**`halClock.isAvailable()` plus the RTC's own oscillator-stopped flag**, not on the settings
flag. x4pro has an RTC (`FREEINK_CAP_RTC` covers `FREEINK_DEVICE_X4PRO`), so this is workable —
but budget it as HAL work.

**ISO week arithmetic is settled: `%G`/`%V` are available.** The x4pro firmware links newlib
4.3.0 (`toolchain-xtensa-esp-elf/.../esp32s3/no-rtti/libc.a`, confirmed from
`.pio/build/x4pro/firmware.map`), whose `strftime` jump table dispatches `%G`, `%g`, `%V` and
`%u` to real handlers. No fallback needed. **Caveat:** they read `tm_wday`/`tm_yday`, so the
`struct tm` must come from `gmtime_r`/`localtime_r` — never be hand-filled from an
`Rtc::DateTime`.

### Activity

`MeetingDownloadActivity` in `src/activities/network/`, reached from
`NetworkModeSelectionActivity`, mirroring `OpdsBookBrowserActivity::downloadBook`
(`OpdsBookBrowserActivity.cpp:460-520`).

**Cancellability is not automatic.** `fetchUrl(url, DataCallback, …)` takes **no** cancel flag
and **no** progress callback — only `downloadToFile` does — and `HTTP_TIMEOUT_MS = 60000`
(`HttpDownloader.cpp:32`). So `ResolvingWeek` and `ResolvingMedia` block `loop()` for up to a
minute each with no Back handling. What makes OPDS cancellable is that its progress lambda calls
`mappedInput.update()` and `routeTouch()` *inside* the callback
(`OpdsBookBrowserActivity.cpp:500-503`); the download phases must do the same, and the two
resolve phases must either accept the block or gain a cancel path.

### Destination and cache invalidation

Reuses `SETTINGS.opdsDownloadFolder` (`char[64]`, `CrossPointSettings.h:266`, `""` = SD root)
with the same exists → mkdir → fall-back-to-root guard. Filenames are the CDN's own
(`w_S_202607.epub`), which are stable and self-describing.

**`clearBookCache(destPath)` must be called on every successful download**, exactly as OPDS does
at `OpdsBookBrowserActivity.cpp:524` → `BookCacheUtils.cpp:23-33`. An earlier draft argued
re-downloading was "harmless because the path is unchanged" — that is backwards. The cache is
keyed on the *path hash* and `book.bin` stores no size or mtime, so identical path + revised
bytes leaves stale `sections/*.bin` behind. The CDN does revise files
(`w_S_202607`'s `modifiedDatetime` is `2026-03-05 13:14:37`). This is CLAUDE.md's "Corrupt Cache
Files" crash class.

### TLS: unauthenticated, by design of the current transport

An earlier draft claimed verification against a CA bundle. **False on this target.**
`platformio.ini:41` sets `-DFREEINK_NET_WOLFSSL=1` in `[base]`, which `[env:x4pro]` extends, so
the compiled path calls `http.setInsecure()` unconditionally (`HttpDownloader.cpp:56`).
`SecureHttpClient.h:66-68` states it plainly: the wolfSSL transport has no CA bundle wired up.
The `crt_bundle_attach` path is dead code on x4pro.

Consequence: these transfers are **unauthenticated TLS and MITM-able**, and nothing will "fail
on first run" for a missing root. The available mitigation is the MD5 the API already returns
(`file.checksum`, e.g. `c0208a4acad782f4295753992134bad5`) — the earlier draft listed that field
and never used it. **Verify the downloaded file against it** and delete the file on mismatch.

### Strings

All user-facing text via `tr()` per CLAUDE.md rule 5 — the earlier draft specified bare English
literals for every phase and error. Reuse the existing `STR_CONNECTING`,
`STR_DOWNLOADING`, `STR_DOWNLOAD_FAILED`; add `STR_MEETING_PUBLICATIONS`,
`STR_RESOLVING_WEEK`, `STR_NO_PUBLICATIONS_FOUND`, `STR_CLOCK_NOT_SET`,
`STR_WORKBOOK_UNAVAILABLE`, `STR_CHECKSUM_MISMATCH` to `english.yaml` **and** `spanish.yaml`,
then `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`. Only YAML is committed.

`langwritten=S` is a `static constexpr`; the parser takes the language key as a parameter rather
than hardcoding `"S"` in two places.

## Risks

- **The WOL page is HTML, not an API.** Markup or URL-scheme changes break the scan. Fail with
  a specific message and log the fetched byte count so it is diagnosable from serial. A sturdier
  anchor exists if needed: the `<li class="… pub-w docId-2026482 pub-w26 …">` tokens.
- **7.1 MB of streaming.** `downloadToFile` streams to SD; nothing may buffer a body whole.
- **Two blocking resolve phases** of up to 60 s each (see Cancellability).

## Testing

**Host** — register in `test/CMakeLists.txt`:
- `test/wol_week_scan/` — both prefixes recovered from saved page fixtures, fed in **1-byte,
  7-byte and 4096-byte chunks**. Fixtures must include **week 2026/01** (previous-year
  publications), **week 2026/14** (Memorial, workbook absent) and week 2026/37 (both present).
  The 2026/01 fixture is the regression test for the year-templating bug.
- `test/month_name_map/` — all twelve names plus a rejected unknown.
- `test/pub_media_json/` — url and filesize from the real 958 B response, **asserting
  `pubImage.url` is not mistaken for the file url**.

```
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j
```

**Device** (flag for the user):
1. WiFi + valid RTC: both EPUBs land at the advertised sizes, MD5 verified.
2. Open the downloaded Watchtower: a scripture citation opens the extracted text and Back
   returns to the article (cross-check against the return-stack fix).
3. An external `jw.org/finder` link renders as plain text and is not tappable.
4. Cancel mid-download: no partial file the file browser will list.
5. No-WiFi and unset-clock paths each show their own `tr()` string.
6. Re-download over an existing copy, then open it — no stale-cache corruption
   (`clearBookCache` ran).
7. `ESP.getFreeHeap()` stays above 50 KB throughout.

**Build:** `pio run -e x4pro` after the last edit. `./bin/clang-format-fix -g`.

## Open questions

- **Skip-if-present.** Still always downloads. Deterministic filenames make an
  existing-file-of-advertised-size check cheap, saving ~7.1 MB per re-run. Deliberately deferred.
- **Whether the Workbook is wanted at all**, or only the Watchtower. Both are specified; the
  optional-publication design above makes dropping one trivial.
