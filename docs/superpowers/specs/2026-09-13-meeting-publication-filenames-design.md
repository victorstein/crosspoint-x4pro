# Readable filenames and skip-if-present for meeting downloads

**Date:** 2026-09-13
**Status:** Design v2 (revised after adversarial review)
**Target:** CrossPoint Reader firmware, `x4pro` build target (UC8179 panel, ESP32-S3, 8MB PSRAM)
**Delivery:** `origin` (victorstein/crosspoint-x4pro), branch `feature/meeting-publication-filenames`
**Builds on:** `2026-09-12-meeting-publications-downloader-design.md` (merged as #7, `918eabda`)

## Goal

Two follow-ups the downloader deliberately deferred:

1. **Readable filenames.** Downloads currently land as the CDN's own `w_S_202607.epub` /
   `mwb_S_202609.epub`, derived by `filenameFromUrl()`
   (`MeetingDownloadActivity.cpp:232`). The file browser shows publication codes instead of
   publication names.
2. **Skip-if-present.** Every run re-downloads ~7.1 MB even when the issue is already on the
   card.

These are one change, not two: the skip check can only look for a file if it knows the name
the download would be written under, so the naming rule has to be settled first.

## Non-goals

- **Pruning old issues.** Nothing is deleted. Deletion stays manual (file browser long-press,
  which already clears the book cache via `removeDirFile` → `clearBookCache`).
- **Renaming unrelated books** on the card. Only the issue being downloaded is touched.
- **A user-configurable naming pattern.** One scheme, no setting.

---

## Naming

```
<pubName> <YYYY-MM>.epub
```

```
La Atalaya (ed. estudio) 2026-07.epub
Guía de actividades para la reunión Vida y Ministerio Cristianos 2026-09.epub
```

Publication first so the browser groups by publication; the numeric issue code second so
issues sort chronologically inside each group. Month *names* would sort alphabetically
("Julio" before "Marzo"), which is why the code is numeric.

### Where the pieces come from, and two traps

`pubName` and `formattedDate` are separate fields, and **`pubName` does not contain the
date** — measured live:

| pub | `pubName` | `formattedDate` |
| --- | --- | --- |
| `w` 202607 | `La Atalaya (ed. estudio)` | `Julio de&nbsp;2026` |
| `mwb` 202609 | `Guía de actividades para la reunión Vida y Ministerio Cristianos` | `Septiembre y octubre de 2026` |

**Trap 1 — `formattedDate` carries HTML entities.** `Julio de&nbsp;2026` decodes to a
U+00A0 non-breaking space. This design sidesteps it entirely: the `YYYY-MM` code is built
from the **issue number already in hand** (`202607` → `2026-07`), never from
`formattedDate`. Nothing needs `lookupHtmlEntity`, and no NBSP can reach a filename.
`formattedDate` is not read at all.

**Trap 2 — `pubName` is not currently parsed, and its buffer must be sized deliberately.** `PubMediaJsonParser` exposes only `url()`,
`checksum()`, `filesize()` and `found()` (`PubMediaJson.h:28-33`). It must gain a
`pubName()`, captured at the **root** level — note `pubName` sits beside `pubImage`, so the
existing depth/path state machine must match it at depth 1 — `currentNode() == Node::Root && keyIs("pubName")`,
which it can express (`PubMediaJson.cpp:44-45`).

The shadowing risk is **not** `pubImage`: every live response puts `pubName` first. The real
near-miss is **`parentPubName` sitting immediately beside it** — exact `strcmp` via `keyIs` is
correct, a prefix or `strstr` match is not.

**Buffer:** `pubName` reaches **196 bytes** (Khmer) and the Spanish workbook is 66. `safeCopy`
(`PubMediaJson.cpp:8-12`) truncates **byte-wise**, and a cut landing mid-sequence leaves a dangling
lead byte that `sanitizeFilename` copies through verbatim (`StringUtils.cpp:28-31`), handing SdFat
invalid UTF-8 under `USE_UTF8_LONG_NAMES=1`. Specify `char pubName_[208]` **and** require truncation
to land on a codepoint boundary. That is +208 B on a parser already heap-allocated via
`makeUniqueNoThrow` (`MeetingDownloadActivity.cpp:214`) — small, but it is a real allocation and is
justified by the 196-byte worst case. `TOKEN_BUF_SIZE = 512` (`StreamingJsonParser.h:21`) holds it,
and the token arrives in one `onString` callback, so no chunk splitting.

### Sanitising

Reuse `StringUtils::sanitizeFilename(name, 100)`, the same helper `opdsBookFilename`
(`OpdsFilename.cpp:22`) already uses. Verified behaviour (`StringUtils.cpp:7-40`):

- replaces `/ \ : * ? " < > |` with `_`
- **preserves codepoints ≥ 128 verbatim**, so `Guía` and `reunión` keep their accents
- drops control characters, trims leading/trailing spaces and dots
- truncates on a **UTF-8 codepoint boundary**, never mid-sequence

Measured: the workbook `pubName` is **66 bytes**, `<pubName> <YYYY-MM>` is **74 bytes** (what the
cap governs), and the full filename is 79. Inside the 100-byte cap today.

**Append the issue code AFTER sanitising.** Sanitise `pubName` to `100 - strlen(" YYYY-MM")`, then
append the code, so the code can never be the part that gets truncated. This is not theoretical:
fed through the real helper, Khmer and Georgian `mwb` names (196 B and 172 B) truncate to
**byte-identical filenames for issues 202609 and 202611** — two different issues, one name. 15 of
55 languages exceed the budget. `DOWNLOAD_LANGUAGE` is hardcoded `"S"` today
(`MeetingDownloadActivity.cpp:36`), so it is latent, but the failure mode is severe: same name plus
a coincidentally equal `filesize` makes the skip check serve the wrong issue permanently.

**Fallback — and the trap in it.** `sanitizeFilename` **never returns empty**: its last line is
`return result.empty() ? "book" : result;` (`StringUtils.cpp:43`). So a `.empty()` check on the
sanitised name is dead code, and an absent `pubName` would silently produce `book 2026-09.epub`
— which `w` and `mwb` would **both** claim in the same run, the second overwriting the first.
Branch on the **raw** `pubName` before sanitising:

```cpp
if (!pubName || !pubName[0]) return filenameFromUrl(url);
```

## Migrating an existing CDN-named copy

A card may already hold `w_S_202607.epub` from before this change. **The move must be lossless** —
the repo already has the pattern, in `moveFinishedBookToReadFolder`
(`EpubReaderActivity.cpp:125-145`), which moves a book without losing anything:

```cpp
Storage.rename(srcPath, dstPath);
const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
if (!oldCachePath.empty() && Storage.exists(oldCachePath)) Storage.rename(oldCachePath, newCachePath);
RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
if (APP_STATE.openEpubPath == srcPath) { APP_STATE.openEpubPath = dstPath; APP_STATE.saveToFile(); }
```

Renaming the **cache directory** carries `progress.bin` with it (progress lives at
`<cachePath>/progress.bin`), so reading position survives. An earlier draft called that loss
"unavoidable"; it is not, and the lossy version would also have broken two things it never
mentioned:

- **Recents.** `RecentBooksStore::isMissing` is `!Storage.exists(book.path)`
  (`RecentBooksStore.cpp:106`) and `pruneMissing()` erases such rows, so the entry would vanish.
- **The cover thumbnail.** `getCoverBmpPath` is `cachePath + "/cover.bmp"` (`Epub.cpp:559-562`) and
  `recent.json` stores that path; `clearBookCache` deletes the whole directory (`Epub.cpp:505-518`).

### Order of operations

For the issue about to be downloaded:

1. Compute the new name.
2. **New name exists** → hand to the skip check below.
3. **Else old CDN name exists** (`filenameFromUrl(url)`, same folder) → run the lossless move
   above, then **`clearBookCache(newPath)`**, then re-run the skip check.
4. Else download.

**Step 3's `clearBookCache(newPath)` is not optional.** The download path already guards this
(`MeetingDownloadActivity.cpp:305-308`: the cache is path-hash keyed and `book.bin` records no size
or mtime, so a revised issue would render from the previous issue's sections). The rename path
needs the same guard: if `/.crosspoint/epub_<hash(newPath)>/` already exists — left by a same-named
file deleted over the web server or WebDAV rather than the file browser, whose own delete does
clear it (`FileBrowserActivity.cpp:170`) — the renamed book renders from stale sections. Apply it
to the **new** path, after the move, so it cannot fight the cache-dir rename.

If the rename fails, log and download under the new name; the stale old file is then the user's to
delete.

**The cache-key rule** is `cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(filepath))`
over the full path as passed in (`Epub.h:48`) — not `docs/file-formats.md`, which only documents the
directory shape.

## Skip-if-present

Skip when **`filesize > 0` and the file exists and its on-disk size equals that `filesize`**.

The `filesize > 0` guard matters: `PubMediaJsonParser::filesize_` initialises to `0` and is only
set when the key appears (`PubMediaJson.cpp:29, 95-101`), so without it a **0-byte file plus an
absent `filesize`** would compare equal and be skipped forever, un-repairable. When `filesize` is
absent, always download.

**Reading the size — there is no `Storage` stat.** `HalStorage`'s public surface
(`HalStorage.h:15-48`) has `exists`, `open`, `remove`, `rename`, `mkdir`, `rmdir`,
`openFileForRead/Write`, `removeDir` and no size-of-path. Size exists only on an open handle
(`HalFile::fileSize()`, `HalStorage.h:77-79`). Follow the existing precedent at
`FontDownloadActivity.cpp:159-172`: `Storage.exists()` first (so `openFileForRead` does not log a
miss on every first run), then open, read `fileSize()`, and **close before anything else touches
the path** — SdFat's `rename` requires the file not be open. Note `fileSize()` is `size_t` while
`filesize()` is `uint64_t`; cast explicitly.

No file body is read, so a run that skips both publications costs no SD throughput. MD5 is
deliberately not re-verified on the skip path: that would mean reading 7.1 MB to decide not to
download.

**What the size check does and does not catch.** A mismatch detected *during* a download is
deleted (`MeetingDownloadActivity.cpp:298-300`), and `downloadToFile` removes its output on any
non-OK result including `ABORTED` (`HttpDownloader.cpp:286-289`) — so a cancelled transfer leaves
nothing, and the size check is not there to catch one. It is there for a file truncated by power
loss or a panic, and for a foreign file that happens to share the name. It does **not** catch later
corruption (SD bitrot, a PC edit), and it cannot catch a same-size corrupt file when the API
publishes no checksum, because `matchesChecksum` returns true without reading the file in that case
(`MeetingDownloadActivity.cpp:315-318`). A file that becomes corrupt after a clean run is only
repairable by deleting it.

On a skip, report it and continue to the next publication. When both are skipped the activity still
ends in its Done phase, distinguishing "already had both" from "downloaded both".

**Ordering:** the skip check runs after the media JSON fetch, because `filesize` comes from that
response. Only the two large transfers are avoided, which is where the time and bytes are.

## Strings

New `tr()` keys in **both** `english.yaml` and `spanish.yaml`, then
`python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` (commit YAML only):

- `STR_ALREADY_DOWNLOADED` — "Already on the card"
- `STR_RENAMED_EXISTING` — "Renamed an earlier copy"

Spanish wording is the implementer's to supply for both keys; #7 established that both YAMLs are
updated together.

## Risks

- **Renaming resets reading progress** for that issue, since progress is path-hash keyed.
  Called out above; acceptable for a weekly publication, and it happens once per issue at
  most.
- **`pubName` could change between issues**, giving two differently-named files for the same
  publication. Harmless — they are different issues anyway — but it means the skip check is
  per-issue, not per-publication.
- **A partially-written file of exactly the advertised size** would be skipped wrongly. Not
  reachable in practice: `downloadToFile` writes sequentially, and the MD5 check already
  deletes a completed-but-corrupt file (`MeetingDownloadActivity.cpp:298-300`), so a
  same-size wrong file cannot survive a previous run.
- **Orphan states the ordering can leave** (none are data loss, all are untidy): both old and new
  exist and new passes the size check → the 3.7 MB CDN-named copy and its cache stay forever; new
  exists at the wrong size while old also exists → the download overwrites new and old is orphaned;
  `SETTINGS.opdsDownloadFolder` changed between runs → the old file sits in the previous folder and
  is never found, and `resolveDownloadFolder()` silently falls back to `""` on mkdir failure
  (`MeetingDownloadActivity.cpp:46-53`), so "the same folder" is not stable across runs.
- **`StreamingJsonParser` does not decode `\uXXXX`** (`StreamingJsonParser.cpp:149-154`) and
  `sanitizeFilename` maps `\` to `_`. The live API sends raw UTF-8 in every sampled response, so
  this is latent only — but an escaped name would produce `_u00e1` in a filename.
- **Long names in other languages** truncate at 100 bytes. Clean by construction, but two
  issues of a very long publication could in principle collide after truncation. Appending the code after sanitising (above) makes the code
  itself un-truncatable, so two issues can still share a stem but never the same full name.

## Testing

**Host** — `test/meeting_filename/`, registered in `test/CMakeLists.txt`, modelled on
`test/opds_filename/CMakeLists.txt` (which already compiles `StringUtils.cpp` and
`lib/Utf8/Utf8.cpp` and links `crosspoint_test_common`). The name builder must be a **free
function over `(pubName, issue, url)`** — it needs the URL for the raw-`pubName` fallback, not a method on the activity, or it cannot be built on
host.

Cover:
- `("La Atalaya (ed. estudio)", 202607)` → `La Atalaya (ed. estudio) 2026-07.epub`
- the full workbook name → accents preserved, 79 bytes, no truncation
- a name containing `/` and `:` → both become `_`
- an over-long name → truncated on a codepoint boundary, never mid-sequence
- empty/whitespace **raw** `pubName` → falls back to the URL-derived name (and never to `"book"`)
- a name whose sanitised form would exceed the budget → the `YYYY-MM` code survives intact
- issue `202512` → `2025-12`; a malformed issue → fallback

Extend `test/pub_media_json/` with a `pubName` assertion, including a fixture carrying
`parentPubName` beside `pubName`, to prove the exact-match key rule and not a prefix match.

```
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j
```

**Device** (flag for the user):
1. Fresh card: both files land with readable names and accents intact.
2. Immediately re-run: both skip, nothing re-downloads, Done reports "already".
3. Truncate one file by hand, re-run: only that one re-downloads.
4. Put a `w_S_202607.epub` on the card, re-run: renamed in place, not re-downloaded, and no
   orphaned `.crosspoint/epub_<hash>/` left behind.
5. Open a renamed book: it paginates from scratch (expected) and reads correctly.

**Build:** `pio run -e x4pro` after the last edit. `./bin/clang-format-fix -g`.

## Open questions

- **Whether to shorten the workbook name.** It fits at 79 bytes and is used in full here, but
  it will visually elide in a file-browser row. An abbreviation would have to be
  per-publication and per-language, which rots; left long deliberately. Say so if the row
  looks bad on the device and the fix is a shorter render, not a shorter filename.
