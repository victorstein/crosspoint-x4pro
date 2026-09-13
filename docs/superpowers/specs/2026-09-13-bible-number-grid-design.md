# Number grids for chapters and verses, and tap-only navigation

**Date:** 2026-09-13
**Status:** Design v2 (revised after adversarial review)
**Target:** CrossPoint Reader firmware, `x4pro` build target (800x480 panel, controller resolved at boot, ESP32-S3, 8MB PSRAM)
**Delivery:** `origin` (victorstein/crosspoint-x4pro), branch `feature/bible-number-grid`
**Builds on:** #8 (`421bcda9`) and #12 (`2b427ba6`)

## Goal

Two problems from reading on the device:

1. **Reaching a high number takes forever.** A vertical list shows ~10 rows, so Salmos 119:145
   means scrolling through 150 chapters and then 176 verses, ten at a time. Reported as
   *"it takes ages scrolling since I can only see about 10 numbers at a time."*
2. **The verse list opens on a hold, not a tap.** The hold is the gesture nobody discovers — the
   footer hint added in #12 papers over it rather than removing the problem.

## Non-goals

- **Gridding the book list.** Book names are long and variable width; a grid would force hard
  truncation or abbreviations. It stays a vertical list.
- **A general grid list component.** This wires the SDK's existing `keyGrid` into two screens; it
  does not add a reusable app-level grid abstraction.
- **Changing how a chosen verse navigates.** `navigateTo` with `offsetJump` is untouched.

---

## Feature 1: tap-only navigation

Today `activateIndex` at `Level::Chapter` navigates to the chapter, and `onRowLongPress` opens the
verse list (`BibleNavigationActivity.cpp:239-263` and `:265-285`).

**Invert it and delete the hold:**

- `Level::Chapter` tap → open the verse grid.
- `onRowLongPress` override → **removed** for the Chapter level. Nothing is left behind it.
- The held-Confirm branch in `handleButtons()` (`:293-304`) and `OPEN_VERSE_LIST_MS` go with it.
- `wantsTouchLongPress` in the constructor becomes `false`, and `InputLongPress` comes out of the
  grid/list `inputMask`.

Reaching a chapter's start is now tap-chapter then tap `1`, since verse 1 is the first cell.

**Retire the hint.** `STR_HOLD_FOR_VERSES` and the chapter-level branch of `drawFooter()` (added in
#12) describe a gesture that no longer exists. Remove the key from **both** `english.yaml` and `spanish.yaml` and regenerate.

**There is no renumbering hazard, contrary to an earlier draft.** Removing a key does renumber every
later `StrId`, but nothing persists a `StrId` numerically — the language setting stores a code
string, not an index, and every other use is in-RAM. The build also runs the generator with
`strip_unused=True`, so a key left in the YAML with no `tr()` caller is dropped automatically with a
warning. The compile error runs the other way: code referencing a key that no longer exists. `drawFooter()`
keeps its plain Back/Select hints for all three levels.

**The five single-chapter books** currently reach their verses through `onRowLongPress` at the Book
level (`:272-277`). With the hold gone, a tap on those books must open the verse grid directly —
they have no chapter level, which is exactly what `bookIsDirect[]` already encodes.

## Feature 2: number grids

### Which levels

| Level | Presentation |
| --- | --- |
| Book | vertical list (unchanged) |
| Chapter | **grid** |
| Verse | **grid** |

### The component already exists, but the interaction table does not fit it

`freeink::ui::keyGrid` (`components/keyboard/key-grid.h`) renders `rows x cols` `KeyGridKey`s, each
carrying a `const char* label` and an `int16_t value`, under one shared `ActionId`. It lives under
`keyboard/` but is generic, it does **no** scrolling or paging of its own (a pure fixed-grid
painter that reads `keys[0 .. rows*cols-1]` unconditionally), and `UiScreen::frame()` is public
(`FreeInkApp.h:59`), so `buildScreen` can call it directly.

**The blocking constraint is the interaction budget.** `UiAppHost.h:29` is:

```cpp
using UiApp = freeink::ui::FreeInkApp<24, 6>;   // 24 interactions
```

`keyGrid` calls `button()` per cell and `button()` calls `frame.hit(...)` per enabled cell — **one
interaction each, no batching**. Past 24, `addInteraction` sets a flag and returns false
(`FreeInkUICore.h:1015-1023`): no assert, no log, no crash. The 25th and later cells simply get **no
hit rect and are silently untappable**, and `interactionOverflowed()` is checked in **zero** places
in `src/`. A 48-cell page would half-work — taps land on the first 24 numbers and do nothing on the
rest.

So this change **must** raise the budget:

- `UiApp` becomes `FreeInkApp<64, 6>`.
- `Interaction` is 16 B and the buffer is double-buffered, so 24 -> 64 costs
  **2 x 40 x 16 = 1,280 B**, paid by every FUI screen since `UiAppHost` is shared. Acceptable on a
  device reporting ~253 KB free, but it is a real cost and is not free for the other activities.
- Add a `LOG_ERR` on `app.interactionOverflowed()` after a build so this can never fail silently
  again. That guard is worth more than this feature.

### Dispatch, and the mirror-image trap

`KeyGridKey::value` reaches `ActionEvent::value`, and `UiListActivity::onRowAction`
(`UiListActivity.cpp:37-44`) already does `activeNav().selected = event.value;
activateIndex(event.value);`. Set each cell's `value` to its **absolute** index and taps work with no
new dispatch code. `int16_t` covers 150 chapters and 176 verses.

**But `selectedIndex` is page-relative, not absolute.** `keyGrid` compares `props.selectedIndex`
against `idx = row * cols + col`, which runs `0..cells-1` within the page. The list path sets
`props.selectedIndex = selected` absolutely, so mirroring the list code means that on page 2 and
beyond **no cell ever renders as selected** — it ships as "the highlight disappears after the first
page" rather than as a failure. Set it explicitly:

```cpp
props.selectedIndex = onThisPage(nav.selected) ? nav.selected - pageFirstCell : -1;
```

### Geometry

Derive `cols` and `rows` from the content rect at build time; the device has four orientations and
nothing may be hardcoded. **The real content area is much larger than a first estimate suggests**:
x4pro has touch, so `UITheme` zeroes `buttonHintsHeight` and the safe area is the full panel. Against
the 800x480 panel, minus the header band and spacing, the content rect is about **480 x 695
portrait** and **800 x 375 landscape**.

At `minCell = 56` that yields far more cells than a page should hold:

| orientation | gap | cols | rows | cells |
| --- | --- | --- | --- | --- |
| portrait 480x695 | 8 | 7 | 10 | 70 |
| landscape 800x375 | 8 | 12 | 5 | 60 |

Both exceed the interaction budget **and** a 64-entry array. So the clamp is not a formality — it is
what keeps the design inside both limits:

```cpp
static constexpr int16_t GRID_GAP = 8;
static constexpr int MIN_COLS = 4;
static constexpr int MAX_COLS = 8;
static constexpr int MAX_GRID_CELLS = 48;   // <= UiApp's interaction budget, minus chrome

cols = clamp((contentW + GRID_GAP) / (minCell + GRID_GAP), MIN_COLS, MAX_COLS);
rows = max(1, (contentH + GRID_GAP) / (minCell + GRID_GAP));
rows = min(rows, MAX_GRID_CELLS / cols);    // the clamp that prevents the overrun
```

`MAX_GRID_CELLS` is **48, not 64**: it must leave interaction slots for the header, footer and any
chrome inside the same frame, and 64 cells would exactly consume a 64-entry budget with nothing
spare. Clamping `rows` (not `cols`) preserves the column count the width earned, so cells stay
square-ish.

`keyGrid`'s own `idx` is `uint8_t`, capping a page at 255 — never the binding limit here, but the
clamp above must exist independently of it.

**`minCell` is not `KeyGridProps::minTouchSize`.** `keyGrid` derives cell size purely from the rect;
`minTouchSize` (default 28) is the floor passed to `ensureMinTouchRect`, which *expands a hit rect
beyond its visual cell*. If a cell ever rendered smaller than `minTouchSize`, adjacent hit rects
would overlap and taps would land on the wrong number. Set `props.minTouchSize` to at most the
computed cell size.

### Paging, and the `nav.top` contract

A page is exactly `rows * cols` cells. Page count is `ceil(count / cellsPerPage)`. At 48/page,
Psalm 119's 176 verses is **4 pages** against ~18 screens of scrolling today.

**`nav.top` cannot simply be reinterpreted as "first cell of the page".** `ListNav::reset()` sets
`visibleRows = 1`, and the only writer of `visibleRows` is `syncToProps`, reached solely through
`screen.list()` — which grid levels never call. With `visibleRows` stuck at 1, three base paths
quietly corrupt the page index:

1. `moveSelectionTo` -> `follow()` collapses `top` to `selected`.
2. `UiListActivity::loop()`'s swipe branch calls `scrollBy(+/-pageRows())` = +/-1, and it is
   **not virtual** — only `handleCustomInput()`, which runs first, can intercept it.
3. `enterLevel()` already calls `nav.follow(listCount())` and leaves `followPending` set forever,
   since `onListRendered` never runs at a grid level.

**The contract, stated so it cannot be got wrong:** immediately after `nav.reset()` in `enterLevel`,
set `nav.visibleRows = cellsPerPage` for grid levels, so `follow`, `scrollBy` and `pageRows` all
agree that one "row" is one page. Then override `handleCustomInput()` to consume swipes at grid
levels and page by a whole page.

**Buttons.** Up/Down move the selection by a row (`+/-cols`); PageBack/PageForward move a page. This
needs a `navigateButtons()` override, since the base steps by 1.

The **last page is padded** with `enabled = false`, `KeyKind::Disabled` cells so the array stays
rectangular. Padded cells register **no interaction**, so they cost nothing against the budget.

### Memory — additive, not a replacement

The Book level keeps its vertical list, so `windowLabels`/`windowItems` and `refreshRowWindow`
**stay**. The grid is therefore added on top:

```
KeyGridKey                48 B each (label + secondaryLabel + BitmapRef + AssetRef + 4 small fields)
cells[MAX_GRID_CELLS]     48 x 48 = 2,304 B
cellLabels[48][4]                     192 B   ("176" / "150" + NUL)
UiApp 24 -> 64 interactions         1,280 B   (shared by every FUI screen)
                                    -------
                                  ~3,776 B added
```

All fixed, no heap, no per-repaint allocation. An earlier draft claimed this *replaced* the row
window while also keeping it for the Book level — it does not, and the honest figure is above. Grid
labels are ASCII digits, so none of the CJK fallback-glyph prewarming the book list performs applies.

## Risks

- **`keyGrid` has no existing app-level caller.** `grep` finds no use in `src/`; only the keyboard
  consumes it. Its styling, touch-target behaviour and selection rendering under this app's theme are
  unverified outside a keyboard, so this is the first real exercise of it. Budget device iteration on
  cell size and gap.
- **Four orientations.** Geometry must be recomputed on every build, and `applyOrientation` must not
  leave `nav.top` pointing at a page that no longer exists — clamp after a geometry change.
- **Removing a `tr()` key touches generated files.** Only the YAML is committed; a missed regeneration
  shows up as a build error rather than a silent bug, which is the good failure mode.
- **The Book level keeps the list**, so `BibleNavigationActivity` now has two presentations in one
  class, and `nav` means different things at each. Keep the row-window code and the grid code
  separate rather than generalising one into the other, and make the `visibleRows` assignment
  conditional on the level.
- **Raising `MaxInteractions` touches every FUI screen**, not just this one. It is a shared
  instantiation; the 1,280 B is paid everywhere and any screen already near the old 24 would change
  behaviour (for the better — it would stop silently dropping interactions). Worth a quick check
  that no screen was relying on the old ceiling.

## Testing

**Host** — the arithmetic is the testable part. It must live as free functions in a **standalone
header, `src/activities/reader/NumberGridLayout.h`**, including nothing but `<algorithm>`/`<cstdint>`.
`test/stubs/` carries only `HalDisplay.h`, `HalStorage.h` and `Logging.h` — no FreeInkUI, no Arduino,
no `GfxRenderer` — so putting these in `BibleNavigationActivity.cpp`'s anonymous namespace (the
natural place) makes the suite unbuildable. Add `test/number_grid/` on the `test/return_stack/`
shape, registered in `test/CMakeLists.txt`:

- geometry: the real rects — **480x695 portrait and 800x375 landscape** — yield `cols`/`rows` whose
  product is `<= MAX_GRID_CELLS`; a very narrow rect still yields `cols >= MIN_COLS`; the `rows`
  clamp fires rather than the product overflowing
- paging: 176 items at 48/page → 4 pages; last page holds 176 − 3×48 = 32 real cells and 16 padded
- index mapping: page 3 cell 0 is absolute index 144; round-trips for every cell on every page
- **`selectedIndex` is page-relative**: absolute 144 on page 3 maps to 0, and an absolute index not
  on the current page maps to -1
- a count of exactly one full page → no empty trailing page
- counts of 1 and 0

**Device** (flag for the user):
1. Salmos → chapter grid → 119 reachable in ~3 pages instead of ~15 screens.
2. Salmos 119 → verse grid → 145 reachable in ~3 pages; tapping it lands on the right page of text.
3. Tapping a chapter opens the verse grid (no hold anywhere); the footer no longer mentions holding.
4. Obadiah / Philemon / 2 John / 3 John / Jude → a tap opens the verse grid directly.
5. All four orientations: cells stay square-ish, nothing clips, the last page pads cleanly.
6. Side buttons move a row at a time; page buttons move a page.
7. A non-Bible EPUB is unaffected.

**Build:** `pio run -e x4pro` after the last edit. `./bin/clang-format-fix -g`.

## Open questions

- **Cell size is a judgement call that only the panel settles.** 56 px is a starting point, not a
  measured value; if 6 columns feels cramped in portrait the fix is a larger `minCell`, which costs
  columns. Expect one round of adjustment after the first device run.
