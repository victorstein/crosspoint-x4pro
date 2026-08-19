# Tagged highlights for CrossPoint on the Xteink X4 Pro

**Date:** 2026-08-18
**Status:** Design v2 — revised after adversarial review against `crosspoint-reader@eef2050`
**Target:** CrossPoint Reader firmware, `x4pro` build target
**Delivery:** Fork (see "Delivery: fork" below)

> **v2 changes.** The v1 design claimed this feature was additive, low-risk, and mostly
> assembled from shipped parts. That was wrong on several counts. The anchoring
> mechanism does not exist in usable form, there is no rect-invert primitive, the reader
> menu is at its array cap, and the highlights file can silently destroy itself. All
> corrected below, with the real cost stated. Every claim here is cited to `file:line`.

## Goal

Let a reader select a passage in an EPUB, highlight it, and attach tags, then browse and
filter that book's highlights by tag. Modelled on JW Library's tagged-highlight
workflow, adapted to a 1-bit e-ink panel.

## Non-goals

- **Cross-book tags.** Tags and highlights are scoped to a single book.
- **Notes.** No typed note bodies. Tags only.
- **Tagging bookmarks or whole books.** Existing bookmarks are untouched.
- **Colour.** The panel is monochrome.

## Device facts

| Fact | Value | Evidence |
| --- | --- | --- |
| MCU | ESP32-S3, dual core | `platformio.ini:235-236` |
| RAM | 512KB SRAM + 8MB PSRAM | `platformio.ini:240`; `sdkconfig.x4pro:2115-2138` |
| Flash | 16MB | `platformio.ini:235` |
| Panel | 800×480, 1-bit | `BoardConfig.h:1367-1368`; `Ssd1677Driver.cpp:158-160` |
| Controller | **One of SSD1677 / UC8179 / UC8279**, auto-detected | `BoardConfig.h:113-116`, `:373-378` |
| Colour | Unavailable — M5-only | `BoardConfig.h:242-244` |
| Touch | GT911 @ 0x5D | `BoardConfig.h:1401-1420` |
| Buttons | **Up + Down + Power only** | `BoardConfig.h:1392-1398` |

Build verified on macOS this session: full build 5m23s, incremental 47s, producing a
5.3MB ESP32-S3 image via `pio run -e x4pro`. Pogo pins carry USB data and store units
ship factory-unlocked, so flashing is not a blocker.

### Input model — corrected

The X4 Pro has **no physical Back, Confirm, Left, or Right button.**

```
// {back, confirm, left, right, up, down, power, powerActiveHigh}
{PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 7, 3, false},
```
— `BoardConfig.h:1398`

Two nav keys (GPIO0/GPIO7) map to page prev/next; Back and Confirm come from the GT911
touchscreen and the capacitive Home key. **Selection is therefore a touch flow.** v1's
claim that anchor-then-extend "works with physical buttons for free" is withdrawn.

### Display path — corrected

Text anti-aliasing is **on by default** (`CrossPointSettings.h:219`, `textAntiAliasing = 1`),
and on a strip-grayscale panel the reader renders each page three times: a B/W pass, then
two more into the LSB/MSB grayscale planes (`EpubReaderActivity.cpp:1387-1446`), each
calling `page->render(...)` directly.

Consequences:

1. **A highlight overlay must be applied in all three passes**, or it vanishes under
   anti-aliasing. This is the single easiest way to ship a broken-looking feature.
2. v1's claim that grayscale is "unsuitable for inline text" was wrong — it is the
   shipped default text path on this device.
3. `supportsStripGrayscale()` is `true` for SSD1677 and UC8279 but **false for UC8179**
   (`Uc8179Driver.h:86`), so the render path differs by unit. Do not assume a variant.

## Decisions

| # | Decision | Status after review |
| --- | --- | --- |
| 1 | Tag **highlighted passages** | Unchanged |
| 2 | **Per-book** scope | Unchanged |
| 3 | Render as **inverted block** | Kept, but mechanism rewritten — see below |
| 4 | **Anchor-then-extend** selection | Kept; now understood as a touch flow |
| 5 | **Per-book tag palette** | Unchanged |
| 6 | **No on-page tag marker** | Unchanged |
| 7 | **Separate `.highlights.json`** | Kept; one rationale was false, see below |

### Decision 3 — there is no rect invert

v1 claimed inverting a span was "a bitwise NOT, so glyphs are never re-rendered." **False.**
The shipped highlight fills black and re-draws the glyphs in white:

```cpp
renderer.fillRect(hx, hy, hw, hh, true);
renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
```
— `DictionaryWordSelectActivity.cpp:314-315`

The only invert in the renderer is whole-screen (`GfxRenderer.cpp:1670-1674`). There is no
rect-scoped invert or XOR in the API.

**The decision survives, on better grounds than v1 gave.** The framebuffer is 1bpp and
byte-packed, so `invertRect(x,y,w,h)` is a byte loop with edge masks. And it is *faster*
than the shipped path, which reloads glyphs from SD per highlighted word
(`DictionaryWordSelectActivity.cpp:346-348`) — a page-long highlight would mean a page of
glyph reloads. A rect NOT has zero glyph cost.

**Work item:** add `GfxRenderer::invertRect`. New primitive, not a shipped part.

### Decision 7 — corrected rationale

v1 justified a separate file partly because the bookmark file "feeds KOReader progress
sync." **That is false.** KOReader sync uses `ProgressFile` → `progress.bin`
(`ProgressFile.h`, `KOReaderSyncActivity.cpp:323-368`); it never touches bookmarks.

The decision stands on the remaining argument alone: bookmarks are shipped and working,
and a bug in a shared save path would cost real user data.

## Anchoring — rewritten

A highlight must survive re-pagination. This is the hardest part of the feature and v1
described it incorrectly.

### What `visibleTextOffset` actually counts

Scope and repagination-immunity are **confirmed**: it is per spine item, it is the page's
*start* offset (`Page.h:82-85`), and `Section` exposes `getVisibleTextOffsetForPage` /
`getPageForVisibleTextOffset` (`Section.h:190-197`). It is immune to re-pagination because
it is driven by the HTML parse, not by layout.

But it is **not** "a count of visible codepoints" as v1 said. The counter runs *before*
the parser's skip guards (`ChapterHtmlSlimParser.cpp:1147-1165`), so it includes:

- collapsed source whitespace (indentation between tags),
- entire subtrees later skipped by layout (nested tables, `skipUntilDepth`).

It is a **parse-order counter over body character data**, not a count of anything drawn.
Additionally NFC composition (`ParsedText.cpp:399`) and hyphenation
(`ParsedText.cpp:1169-1187`) shift intra-word offsets.

**The load-bearing consequence: offsets can never be re-derived from rendered words.**
They must be carried through the data structures.

### Why v1's plan cannot work

Per-word offsets already exist — at layout time only, and they are explicitly discarded:

```
// Zero-based visible Unicode-codepoint offsets in the spine body, stored as
// uint16_t deltas from a shared base to keep this layout-only metadata small.
// Pathological spans wider than uint16_t use sparse rebases; rendered
// TextBlocks do not carry any of this metadata.
```
— `ParsedText.h:39-49`

`TextBlock` — the structure that lands in a `Page` and is serialized — has no offset
field. Only the line-start offset survives, to populate the page LUT
(`ParsedText.cpp:1241`).

### The required change

`TextBlock` must carry per-word visible offsets, and the section cache format must bump:

- `SECTION_FILE_VERSION` 40 → 41 (`Section.cpp:43`)
- `SECTION_FILE_PARTIAL_VERSION` changes in lockstep — it is derived
  (`Section.cpp:60`) and the comment at `:56` requires it

**Cost, stated plainly:**

- **Every cached section on every device is invalidated** and re-paginates once on first
  open after the update.
- Per-word RAM and flash cost in every `TextBlock`, on the C3 — which `SCOPE.md:25` names
  as the tightest target and the ceiling for the project.

**Mitigation:** reuse the encoding `ParsedText` already uses for exactly this data —
`uint16_t` deltas from a shared base with sparse rebases for pathological spans
(`ParsedText.h:41-49`). That keeps the per-word cost at 2 bytes rather than 4 and is
proven in-tree.

### Stored form

```json
{
  "tags": ["greek", "wt-study"],
  "highlights": [
    { "si": 3, "start": 9412, "end": 9598, "t": [0, 1], "text": "the spirit of Jehovah is upon me" }
  ]
}
```

At render time, words carry their own offsets, so the highlight pass selects words whose
offset falls in `[start, end)`. No screen coordinates are ever persisted; rectangles are
derived every render, which is what makes a highlight survive a font-size change.

### Ordering hazard

`TextBlock` word order is **visual, not logical** — `ParsedText.cpp:1310-1340` runs
`BidiUtils::computeVisualWordOrder`. Accumulating offsets across it runs backwards on RTL
text, and `SCOPE.md:77` shows RTL is actively supported. Carrying offsets per word (rather
than accumulating) makes this a non-issue, which is a further argument for the format bump.

Also note `DictionaryWordSelectActivity` filters its word list: non-selectable tokens are
skipped (`:78-79`) and only `TAG_PageLine` elements are walked (`:75-77`). The `WordBox`
list is a filtered, visually-ordered, layout-transformed view — never a token stream.

## Storage and durability

Path: `/.crosspoint/highlights/<flattened-book-path>.json`, mirroring `BookmarkUtil`.
The flattening helper must be **shared** with `BookmarkUtil`, not copy-pasted.

### Data-loss hazard — must be designed for

`SDCardManager::readFile` truncates at 50 KB with no error signal
(`SDCardManager.cpp:202-204`). A truncated JSON prefix fails to parse, so
`readDocFromFile` returns false (`PersistableStore.cpp:30-34`), the list loads **empty**,
and the next save overwrites the file via a plain non-atomic write
(`PersistableStore.cpp:10-19`). **The entire book's highlights are destroyed silently.**

At roughly 40 bytes plus up to 72 bytes of `text` per entry, the ceiling is about **430
highlights per book** — reachable for the study use this feature exists for.

Required, not optional:

1. Read via a streaming/buffered path, or cap the highlight count with a **user-visible**
   error before the limit.
2. Save with temp-file + rename, following `ProgressFile::writeAtomic` rather than the
   bookmark path. `PersistableStoreBase::writeDocToFileAtomic` now provides this.
3. Never overwrite a file that failed to parse — a parse failure must be distinguishable
   from "no highlights yet." `readDocFromFileChecked` now provides this: only
   `DocReadStatus::Missing` is safe to overwrite.
4. **Recover an orphaned `.tmp` on load.** `writeDocToFileAtomic` removes the destination
   *before* renaming. If the remove succeeds and the rename then fails, the only surviving
   copy of the data is `<path>.tmp` — and nothing in the codebase ever reads a `.tmp`.
   For a resume position (`ProgressFile`) that window costs a page number; for annotations
   it is silent, unrecoverable loss of the exact file the atomic write exists to protect.
   `HighlightFile::load` must therefore try `<path>.tmp` when the primary path reads
   `Missing`, and promote it if it parses.

v1's "treat a corrupt file as empty, exactly as bookmarks do" was data loss dressed as
graceful degradation.

### Format notes

Read with the `obj["k"] | default` idiom (`BookmarkFile.cpp:26-34`). Two caveats: `|` does
not work for arrays — `tags` and `t` need `.as<JsonArray>()`, where a missing key yields an
array that iterates zero times (`BookmarkFile.cpp:21-23`); and ArduinoJson 7's
`JsonDocument` is elastic, so there is no document cap — the 50 KB read limit is the real
ceiling.

`text` is a display label only, never used to locate a highlight. Produce it with
`BookmarkUtil::sanitizeBookmarkSummary` — but note it caps at **72 bytes, not characters**,
and `resize(72)` can split a UTF-8 sequence (`BookmarkUtil.cpp:33`). Highlighted passages
are far likelier to be non-ASCII than a page's first words, so **fix the truncation to a
codepoint boundary** before reuse. (`BookmarkUtil.cpp:23` also passes a possibly-negative
`char` to `std::isspace` — UB, unlike lines 27/29 which cast correctly.)

Tags are referenced by **index**, so deleting a tag must renumber every highlight's `t`.
This is the sharpest data-integrity edge in the feature and needs a dedicated test; storing
names instead is the fallback if it proves fragile.

### Inherited path quirk

`BookmarkUtil::getBookmarkPath` strips the extension using `find_last_of('.')` on the
*flattened* name, so any dot in the directory path truncates it: `/v1.0/mybook` →
`"v1"`. Every extensionless book under such a directory collides. Match the existing
behaviour for consistency, but this is a shared bug worth fixing once in `BookmarkUtil`
rather than propagating.

## Components

| Component | Kind | Notes |
| --- | --- | --- |
| Per-word offsets in `TextBlock` | **Format change** | Section version bump; the core prerequisite |
| `GfxRenderer::invertRect` | **New primitive** | 1bpp byte loop with edge masks |
| `HighlightEntry` / `HighlightFile` / `HighlightUtil` | New | Mirrors the bookmark trio; atomic save |
| `PassageSelectActivity` | New activity | Derived from `DictionaryWordSelectActivity`; touch-driven |
| Highlight render pass | Reader change | Must run in the B/W **and** both grayscale passes |
| `TagPickerActivity` | New activity | Palette list; "new tag" defers to `KeyboardEntryActivity` |
| `HighlightsActivity` | New activity | Browse and filter; jumps via the existing offset-jump path |

`ActivityManager` provides `startActivityForResult` / `setResult` / `finish` as described
(`Activity.cpp:17-24`), and `docs/activity-manager.md` matches the source at `eef2050`.
One friction point v1 missed: `ResultVariant` is a **closed** `std::variant`
(`ActivityResult.h:70-72`), so returning tag indices means adding an alternative to a
shared central type.

### Prerequisite: the reader menu is full

`MAX_MENU_ITEMS = 16` (`EpubReaderMenuActivity.h:52`). An X4 Pro reading a book with
footnotes and bookmarks, with the frontlight present, already builds exactly 16 items.
Worse, only the first of three loops clamps (`:32`); the loops at `:176` and `:191` do
not, so exceeding the cap is an **out-of-bounds write**.

Adding menu entries therefore requires first raising the cap and fixing those loops. This
is a prerequisite fix, not a free addition.

### Prerequisite: the selection snapshot buffer

`SNAPSHOT_CAPACITY = 4096` bytes (`DictionaryWordSelectActivity.h:79`). One full-width
800px line at ~40px height is 4000 bytes — it barely fits, and two lines do not, so
`readFramebufferRegion` refuses (`GfxRenderer.cpp:1702-1704`) and every selection
extension triggers a full two-pass page repaint with a glyph reload.

For multi-line passages this must be enlarged. With 8MB PSRAM that is cheap — but it is a
change, not an inherited capability.

### Interaction

1. Entry from the reader menu (after the cap fix) and optionally long-press.
2. Tap the first word, tap the last.
3. Selection-in-progress renders as an **outlined box**, distinct from a committed
   highlight's inverted block.
4. Action bar: Highlight / Tag / Cancel. Tag pushes `TagPickerActivity`.
5. On confirm the range is saved and the page repaints with the span inverted.

**Long-press is not free.** `SETTINGS.longPressMenuFunction` is a single mutually-exclusive
slot over `LP_MENU_BOOKMARK` / `LP_MENU_KOSYNC` / `LP_MENU_DICTIONARY` /
`LP_MENU_READER_MENU` / `LP_MENU_DISABLED` (`EpubReaderActivity.cpp:409-436`). Adding a
highlight option costs the user whichever long-press action they use today.

## Error handling

- **Parse failure ≠ empty.** Distinguish the two and never overwrite on failure.
- **Offset matching no words** (book replaced on the SD card): the highlight simply does
  not paint. Keep it in the file and in the browse list. Never delete data on a failed match.
- **Range spanning pages:** each page inverts only its own portion; no cross-page state.
- **SD write failure:** surface it. A silently lost highlight is worse than an error.

## Testing

v1 claimed the highest-risk logic needed no hardware. **Overstated.** `test/` is host-side
CMake+gtest, but every suite compiles only leaf, dependency-free sources
(`test/CMakeLists.txt:41-51`). No suite builds `Section.cpp`, `Page.cpp`, `ParsedText.cpp`,
or `ChapterHtmlSlimParser.cpp` — their closure pulls `GfxRenderer`, `HalStorage`,
`BoardConfig`, expat, and real font metrics.

**Host-testable today:**

1. Format round-trip: save → load → deep-equal, incl. empty palette and untagged highlight.
2. Backward compatibility: unknown future keys load without error.
3. Tag deletion renumbering: delete a middle tag, assert every `t` still resolves to the
   same tag name.
4. UTF-8-safe truncation of `text`.

**Requires a host shim for `GfxRenderer` / `Storage` first (a real work item):**

5. Repagination invariance — record a range at font size A, re-paginate at B, assert it
   resolves to the same words. *This is the critical test and it is not free.*
6. Offset continuity across page boundaries and spine items.

**Requires hardware:**

7. Ghosting behaviour of inverted blocks, and highlight correctness under the grayscale
   passes on each panel variant.

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Section format bump invalidates all caches | High | One-time re-pagination; communicate in release notes |
| Per-word offsets cost RAM on the C3 | High | uint16_t deltas + sparse rebases, as `ParsedText` already does |
| Highlight missing from grayscale passes | High | Apply overlay in all three passes; test with AA on |
| Menu array overflow | High | Fix the cap and unclamped loops *first* |
| 50KB truncation destroys a book's highlights | High | Atomic save; never overwrite an unparsed file |
| Tag index renumbering corrupts assignments | Medium | Dedicated test; names as fallback |
| Panel variant differences (UC8179 has no strip grayscale) | Medium | Detect, don't assume |
| Ghosting from inverted blocks | Medium | Measure on hardware; may need full refresh on change |

## Cost summary

This is **not** the additive, low-risk feature v1 described. Before any highlight can be
drawn, the following must land: a section cache format bump, a new renderer primitive, a
reader-menu cap fix, a snapshot-buffer enlargement, and a host test shim. The
feature-specific work (file, three activities, tag palette) is the smaller half.

## Delivery: fork

**Decided: this ships as a fork, not an upstream PR.**

The considerations that pointed here: `SCOPE.md:48` does list in-reader interactions as in
scope, but `SCOPE.md:21-28` says the project is *intentionally narrowing* toward memory and
flash footprint, with `:25` naming the C3 as the ceiling. Adding a per-word array to every
`TextBlock` and bumping the section format runs against that, and the X4 Pro's 8MB PSRAM is
irrelevant to their binding constraint. Typing tag names via `KeyboardEntryActivity` also
sits close to `SCOPE.md:57` ("No typed notes, journals, or editors").

Consequences of forking, which the plan must account for:

- **The format bump is ours to make.** No upstream negotiation needed on `SECTION_FILE_VERSION`.
- **We can target the X4 Pro specifically.** The per-word offset cost no longer has to fit
  the C3's budget, so the encoding can be chosen for correctness first — though reusing
  `ParsedText`'s uint16_t-delta scheme is still the right default.
- **Upstream drift is now a standing cost.** `upstream/develop` moves (it changed the day
  this spec was written). Rebase against **`upstream/develop`, not `upstream/master`** —
  `master` is stale and does not even contain the commit this fork was taken from. Keep our
  changes in identifiable commits so a rebase stays tractable.
- **Nothing goes upstream.** Decided: the work stays on this fork. The prerequisite fixes
  are genuine device-independent bugs — the unclamped menu loops were a real out-of-bounds
  write, and the non-atomic save path a real data-loss bug — but they are not being
  contributed back. Do not shape the code around upstream reviewability, and do not keep
  changes artificially separable for that reason.
- **Fork-only removes a constraint on the format bump.** With no upstream review to satisfy,
  `SECTION_FILE_VERSION` and the `TextBlock` layout can be chosen for correctness on the
  X4 Pro rather than negotiated against the C3's budget.

## Deferred: SSH terminal

Recorded so the research is not re-derived:

- **wolfSSL 5.7.2 is already in every build** (`platformio.ini:146`, with a
  `patch_wolfssl.py` pre-script at `:119`) with TLS 1.3, ECC and Curve25519 — what
  `curve25519-sha256` and ed25519 need. wolfSSH layers on top.
- Crypto memory was already engineered for the C3's ~50KB free heap (`MEMFIX-PORT`,
  single-precision ECC). The X4 Pro has 8MB PSRAM.
- `KeyboardEntryActivity` is 1,103 lines and already handles QWERTY, symbols, and touch.
- BLE HID input is proven for page-turners (`thedrunkpenguin/crosspoint-reader-ble`);
  extending to a full keymap is the delta. An external BLE keyboard is the intended input.
- `SCOPE.md:56,57,58` rejects it three ways, so it must be a fork.
- Genuinely unsolved: VT100/ANSI emulation, and terminal rendering where a scroll is a
  full repaint — made worse by the three-pass grayscale text path documented above.
