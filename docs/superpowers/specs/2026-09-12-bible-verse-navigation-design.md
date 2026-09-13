# Bible navigation: book → chapter → verse

**Date:** 2026-09-12
**Status:** Design v2 (revised after adversarial review)
**Target:** CrossPoint Reader firmware, `x4pro` build target (UC8179 panel, ESP32-S3, 8MB PSRAM)
**Delivery:** `origin` (victorstein/crosspoint-x4pro), branch `feature/bible-verse-navigation`
**Depends on:** `2026-09-12-reader-return-stack-design.md` — branches off it after merge (both
edit `EpubReaderActivity.cpp`) and uses its `navigateTo`.

## Goal

Reaching a specific verse takes far too long. Chapters run past 50 verses while a page holds
4-6, so the user pages a dozen times to land on a known reference. Give the Bible a
**book → chapter → verse** drill-down.

## Non-goals

- **A reference keypad** ("Ps 119:105" typed in). Lists only.
- **Verse navigation in non-Bible EPUBs.** Those keep today's flat TOC list.
- **Cross-book search.** Positional navigation only.
- **A new cache file format.** No version bump — see Risks.

---

## What the publication actually looks like

Measured against `nwt_E.epub` (15,652,378 B) and `nwt_S.epub` (14,945,282 B) from
`GETPUBMEDIALINKS?pub=nwt&fileformat=EPUB`.

- **Spine: 3,941 items (EN) / 3,937 (ES).** Of these, **exactly 1,189 are Bible chapters** —
  the canonical chapter count — resolved by walking all 61 chapternav files plus the 5 direct
  books. The other ~2,750 are front matter, appendices, study aids and indexes. *This feature
  only ever touches the 1,189.*
- **`OEBPS/biblebooknav.xhtml`** (6,353 B, same filename in EN and ES) — **66 `<a>` links** in
  canonical order, the authoritative book list. (The file has 67 `href` attributes; one is
  `css/epubs.css`, so the parser must scope to `<a>`.)
- **`OEBPS/biblechapternav<N>.xhtml`** — that book's chapters. Size ranges **738 B to 13,940 B**
  (Psalms), not the "~5 KB" a first pass assumed. The **first `<a>` is always the back-link to
  `biblebooknav.xhtml`** — verified 0 violations across all 61 files × 2 languages.
- **Verse markers: `<span id="chapter<C>_verse<V>">`.** Max **176** verses (Psalm 119,
  `1001061123-split119.xhtml`, **67,318 B EN / 69,065 B ES**). Max **150** chapters (Psalms).
- Every one of the 1,189 chapter files has **≥1 verse marker**, **exactly one** distinct
  `chapter<C>` value, and **no fragment** in its href. All verified exhaustively.

### Trap 1 — five books have no chapter-nav page

Only **61** `biblechapternav<N>.xhtml` exist; **31, 57, 63, 64, 65** are absent in both
languages. Those are the single-chapter books (Obadiah, Philemon, 2 John, 3 John, Jude), whose
`biblebooknav.xhtml` entry points **straight at the chapter spine item**
(`1001061135 / 1001061161 / 1001061167 / 1001061168 / 1001061169 .xhtml`). Filtering the TOC on
`biblechapternav` — the obvious first approach — silently drops five Bible books.

### Trap 2 — the TOC lists every book twice, and its hrefs are base-prefixed

`toc.xhtml` carries 392 links (EN) / 406 (ES); each book appears as an outline entry *and* a
book entry (`Genesis Outline` → `1001061300.xhtml`, `Genesis` → `biblechapternav1.xhtml`;
Spanish `Contenido de Abdías` / `Abdías`).

Book names are recovered by **joining booknav hrefs against TOC hrefs on filename only**. The
outline entry points at a different file and never collides. Measured across all 66 books in
both languages: **0 misses, 0 ambiguities, 0 duplicate TOC hrefs.** No string matching on
"Outline"/"Contenido", so it is language-independent.

**The join must normalise, not compare raw.** `TocNavParser.cpp:129` stores
`baseContentPath + href` run through `normalisePath(decodeUriEscapes(...))`, so
`BookMetadataCache::TocEntry::href` holds **`OEBPS/biblechapternav1.xhtml`** while booknav
yields bare `biblechapternav1.xhtml`. A literal `==` join matches **0 of 66** and every book row
renders blank. Compare `find_last_of('/')` tails on both sides — the same fallback
`Epub::resolveHrefToSpineIndex` already uses (`Epub.cpp:940-943`). **Host fixtures must use the
stored, prefixed form**, or the test passes while the firmware fails.

### Why anchors cannot be used

`ChapterHtmlSlimParser.cpp:108` — `isNonNavigableInlineElement(name) { return strcmp(name, "span") == 0; }`
— gates anchor recording at `:436`. Verse markers **are** spans, so verse ids never enter the
anchor map and `pendingAnchor` can never resolve them. (`MAX_ANCHORS_PER_CHAPTER = 1024` at `:44`
is not the binding constraint.)

The working route is the one highlights already use: **verse → visible offset →
`Section::getPageForVisibleTextOffset()` (`Section.h:162`) → `pendingOffsetJump`**, wired at
`EpubReaderActivity.cpp:1170-1277`.

## Design

### The I/O model — the part that decides whether this is feasible

`Epub::resolveHrefToSpineIndex` is a **linear scan with SD reads per iteration**
(`Epub.cpp:936-945`). Each `getSpineItem(i)` → `BookMetadataCache::getSpineEntry`
(`BookMetadataCache.cpp:508-525`) performs **two `HalFile::seek` calls** (each taking
`storageMutex`) and returns a heap `std::string`. One resolve over a 3,941-item spine is ~7,882
seeks.

Calling it **per href** — 66 for the book list, 150 for Psalms — would cost ~115,000 and
~186,000 seeks respectively, plus hundreds of thousands of transient `std::string`s through a
heap whose fragmentation CLAUDE.md explicitly prohibits. **That approach is rejected.**

Two rules make it affordable:

1. **One sweep per level, never N resolves.** Add to `Epub`:

   ```cpp
   // Single forward pass over the spine resolving many filenames at once.
   // `filenames` are bare basenames; `out` is filled with spine indices (-1 when absent).
   void resolveFilenamesToSpineIndices(const char* const* filenames, int* out, int count) const;
   ```

   One pass = one `resolveHrefToSpineIndex`-equivalent cost, and that cost is **already the
   established per-action price**: `navigateToHref` (`:1757`) pays exactly it on every
   cross-file citation tap. A full book→chapter→verse drill-down pays it **twice** (once
   resolving the 66 book targets, once resolving the selected book's chapters).

2. **Store no hrefs.** Chapter rows are literally `1..N`, so nothing but `N` and the resolved
   spine indices need keeping. Book rows keep only the joined display name.

**Detection is memoised, not repeated.** `resolveHrefToSpineIndex("biblebooknav.xhtml")` on a
**miss** walks the entire spine with no early exit, so probing it on every Select Chapter would
tax every non-Bible book. Resolve once when the book is opened and cache
`int bibleBookNavSpine_` (-1 = not a Bible) on `Epub`. The `SELECT_CHAPTER` case (`:782`) then
branches on a cached int.

### One activity, three levels

`BibleNavigationActivity : UiListActivity`, with `enum class Level : uint8_t { Book, Chapter, Verse }`.
`Confirm`/tap descends; `Back` ascends, cancelling out to the reader at `Book`.

**Rejected: three nested activities.** `ActivityManager` supports nesting
(`ActivityManager.h:42`, `protected`), and Back would pop levels for free — but it keeps three
activities plus three row-buffer sets resident and hand-propagates the verse result up two
intermediate handlers.

### Input — resolved, no new gesture needed

`UiListActivity` **already supports row long-press**: the `wantsTouchLongPress` constructor flag
(`UiListActivity.h:28-29`) and `virtual void onRowLongPress(int)` (`:43`), dispatched via
`UiAppHost::routeTouch` (`UiListActivity.cpp:40`). Six activities already use this two-input
pattern (`HighlightsActivity`, `TagPickerActivity`, `TagFilterActivity`,
`EpubReaderBookmarksActivity`, `FileBrowserActivity`).

So: **tap/Confirm a chapter → navigate to verse 1; long-press a chapter → open its verse list.**

- Construct with `wantsTouchLongPress = true`.
- `props.inputMask = fui::InputTouch | fui::InputLongPress` (cf. `HighlightsActivity.cpp:482`).
- For button hardware, route a held-Confirm release to `onRowLongPress`, matching
  `HighlightsActivity.cpp:427-438`.

`Button::Right` is **not** an option: it maps to `SETTINGS.frontButtonRight`
(`MappedInputManager.cpp:70-72`), a physical front button the X4 Pro does not have.

### Where each level's rows come from

| Level | Source | Parsed with |
| --- | --- | --- |
| Book | `biblebooknav.xhtml` → 66 hrefs, names joined from TOC | new `BibleNavScanner` |
| Chapter | `biblechapternav<N>.xhtml` → N hrefs (drop link 0) | same scanner |
| Verse | the chapter spine item → verse offsets | existing `VerseAnchors::Scanner` |

**`BibleNavScanner` is shaped like `VerseAnchors::Scanner`, not like `TocNavParser`.**
`TocNavParser` is *not* host-testable: it derives from Arduino `Print` (`TocNavParser.h:1-11`)
and calls into `BookMetadataCache`, which pulls in `HalStorage`; `test/stubs/` has no `Print`
stub. The scanner therefore lives in `lib/Epub/Epub/` (beside `VerseAnchors`, not in
`parsers/`), is chunk-fed `(const char*, size_t, bool isFinal)`, keeps expat behind an opaque
`void*` (`VerseAnchors.h:36-52`), and touches neither `Print`, `BookMetadataCache`, `HalStorage`
nor `FsHelpers`.

### `Section::ensureHtmlCache()` — not a verbatim extraction

The inflate block is **`Section.cpp:275-347`** (not 276-300). It cannot be lifted behind a
`bool`, because three values escape it and a **failed rename is a supported path**:

```cpp
if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) htmlCached = true;
else LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
…
ctx->parsePath = htmlCached ? htmlPath : tmpHtmlPath;   // Section.cpp:369
```

`reusedHtml`, `tmpHtmlPath` and `htmlCached` are consumed at `:349-351`, `:361` and `:366-369`,
and `suspendBuild()`/`abandonBuild()` branch on `build_->reusedHtml` / `->tmpHtmlPath`. A
bool-returning extraction would leave the nav caller streaming `htmlCachePath()` — a file that
does not exist whenever the rename failed.

Required signature:

```cpp
// Inflate this spine item's XHTML. On success `parsePath` names the readable file:
// htmlCachePath() when promoted, else an un-promoted temp at `tmpHtmlPath` that the
// caller owns and must remove.
bool ensureHtmlCache(std::string& parsePath, bool& promoted, std::string& tmpHtmlPath);
```

`startBuild()` calls it and keeps assigning `ctx->` from the outputs, so the build path is
unchanged. The unrelated `Storage.mkdir(sectionsDir)` at `:281-284` must **not** move into it.
Callers should short-circuit on the existing public `hasHtmlCache()` (`Section.h:127`) rather
than reimplementing the check.

**Constructing a bare `Section` is side-effect-free**: the constructor only builds `filePath`
(`Section.cpp:74-78`) and `~Section()` → `suspendBuild()` early-returns on `!build_` (`:646`).

### Row storage

```cpp
static constexpr int MAX_BOOKS = 66;
static constexpr int MAX_CHAPTERS = 150;      // Psalms
char bookName[MAX_BOOKS][48];                 // 3,168 B, no heap
int16_t bookTargetSpine[MAX_BOOKS];           //   132 B  (chapternav, or the chapter itself)
bool bookIsDirect[MAX_BOOKS];                 //    66 B  (the five single-chapter books)
int16_t chapterSpine[MAX_CHAPTERS];           //   300 B, filled by one sweep
```

**`48` bytes, not 32.** Longest joined TOC name measured in **UTF-8 bytes**: EN
`Song of Solomon` = 15 B; ES **`El Cantar de los Cantares` = 25 B**. The repo ships 32
translation YAMLs and detection fires on any-language NWT, where Cyrillic/Greek renderings run
38-42 B. Truncation must land on a **UTF-8 boundary** — a byte-cut feeds an invalid sequence to
`drawText`; reuse the existing UTF-8-safe summary helper (`test/utf8_summary`).

`int16_t` is sufficient: 3,941 < 32,767. Peak is ~3.7 KB, all fixed, no heap, and the three
tables are not simultaneously live with anything large.

Verse anchors reuse `VerseAnchors::scan`'s vector (8 B/entry, 176 max = 1,408 B). **Raise the
`reserve(64)` at `VerseAnchors.cpp:67` to 176** — this feature makes long chapters a routine
path, and 64→128→256 is three heap operations, which CLAUDE.md rule 7 exists to prevent.

Row labels reuse the windowed pattern of `EpubReaderChapterSelectionActivity.h:13-26`
(`TOC_WINDOW = 24` at `:21`), including the `prewarmFallbackText` batch.

### Navigating on selection

```cpp
navigateTo({.spineIndex = chapterSpine[i], .pageNumber = 0,
            .offsetJump = anchors[v].offset}, ReturnPolicy::Clear);
```

Chapter-level activation passes no `offsetJump`. `Clear` is the default from the return-stack
spec. *(That spec is not yet implemented; this snippet is provisional on its final shape.)*

### Strings

`STR_SELECT_BOOK`, `STR_SELECT_VERSE` via `tr()`, added to `english.yaml` **and**
`spanish.yaml`, then `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`. Only YAML is
committed; the three generated files are gitignored.

## Risks

- **Inflate cost is bounded only because this feature inflates chapter spine items exclusively.**
  Bible chapters top out at 69,065 B, but the largest spine item in this very EPUB is
  `1001061175.xhtml` at **236,207 B EN / 259,339 B ES**. The verse level must be reachable only
  from a chapternav link. Show the existing indexing popup if an inflate exceeds a threshold.
- **`Section::clearCache()` (`:226-243`) removes only `<spineIndex>.bin`, never
  `html/<spineIndex>.html`.** Inflating 62 nav pages leaves ~120 KB of nav HTML that only a full
  `Epub::clearCache()` (`Epub.cpp:504-515`) reclaims. Acceptable on SD; recorded so it is not a
  surprise.
- **`biblebooknav.xhtml` is an NWT-specific filename.** It is a feature gate, not a guess:
  absent → nothing changes. Another publisher's Bible simply does not get the feature.
- **No cache version bump is needed.** `SECTION_FILE_VERSION` is **36** (`Section.cpp:34`) and
  stays there. (CLAUDE.md's "Version 25" is stale — worth fixing separately.) An implementer
  editing a version constant has drifted from this design.
- **Every chapter file has ≥1 verse marker**, so the "no markers" fallback is defence, not a
  known case — it must still exist, but do not test-plan around it as an expected path.
- **File collision** with the return-stack change; branch off it after merge.

## Testing

**Host** — `add_subdirectory(bible_nav_scanner)` and `add_subdirectory(bible_book_join)` in
`test/CMakeLists.txt` (list currently ends at `tag_rows`). Model both on
`test/verse_anchors/CMakeLists.txt`, which compiles the repo's own
`lib/expat/{xmlparse,xmlrole,xmltok}.c` with `-DXML_GE=0 -DXML_CONTEXT_BYTES=1024` and links
`crosspoint_test_common` + `GTest::gtest_main`.

- `bible_nav_scanner` — 66 links from a `biblebooknav.xhtml` fixture with `css/epubs.css`
  present and excluded; the five non-chapternav hrefs classified as direct; a chapternav fixture
  yielding N chapters with link 0 dropped. Feed in 1-byte, 7-byte and 4096-byte chunks.
- `bible_book_join` — **the join must be a free function over two `(href, name)` sequences**, not
  a method on the activity, or it cannot be built on host. Fixtures use the **base-prefixed**
  TOC form (`OEBPS/…`) against bare booknav hrefs, asserting 66 matches and that the outline
  entry never wins, in EN and ES.

```
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j
```

**Device** (flag for the user):
1. Genesis → 50 chapters → long-press Genesis 3 → 24 verses; verse 24 lands correctly.
2. Psalms → 150 chapters → Psalm 119 → 176 verses (worst-case inflate, ~69 KB).
3. **Each of Obadiah, Philemon, 2 John, 3 John, Jude** skips the chapter level.
4. Tap (not long-press) a chapter → lands at verse 1.
5. Back from verse → chapter → book → reader, with no stranding.
6. A non-Bible EPUB shows today's flat list with **no added delay** on opening Select Chapter
   (time it — detection is memoised, so the full-spine miss-scan must happen at most once).
7. `ESP.getFreeHeap()` returns to baseline after a full drill-down.
8. Time the book list and the Psalms chapter list. Each should cost about one citation tap.

**Build:** `pio run -e x4pro` after the last edit. `./bin/clang-format-fix -g`.

## Open questions

None. The input gesture is settled (`onRowLongPress`, already in `UiListActivity`), the I/O
model is settled (one sweep per level), and the join is settled (filename-normalised, measured
at 0 misses across both languages).
