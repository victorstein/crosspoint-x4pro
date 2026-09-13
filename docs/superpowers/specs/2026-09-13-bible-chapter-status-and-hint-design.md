# Chapter number in the status bar, and a discoverable verse list

**Date:** 2026-09-13
**Status:** Design v1
**Target:** CrossPoint Reader firmware, `x4pro` build target (UC8179 panel, ESP32-S3, 8MB PSRAM)
**Delivery:** `origin` (victorstein/crosspoint-x4pro), branch `feature/bible-chapter-status`
**Builds on:** `2026-09-12-bible-verse-navigation-design.md` (merged as #8, `421bcda9`)

## Goal

Two defects found by reading on the device:

1. **The status bar names the book but not the chapter.** Reading Exodus 5 shows `Éxodo`. There is
   no way to tell which chapter you are in without scrolling to a verse marker.
2. **The verse list is unreachable unless you already know the gesture.** It opens only on a
   long-press of a chapter row, and nothing on screen says so. Reported verbatim: *"I just did not
   know how to use it."* That is a design defect in #8, not a user error.

## Non-goals

- **Showing the verse** (`Éxodo 5:22`). Considered and declined: it needs a full scan of the
  chapter file on load plus ~1.4 KB of anchors held resident and a lookup per page turn. Revisit
  separately if wanted.
- **Changing the status bar for non-Bible books.** They gain nothing and must pay nothing.
- **Replacing the long-press.** It stays the fast path; only its discoverability changes.

---

## Feature 1: `Éxodo 5` in the status bar

### Why the chapter is missing today

`renderStatusBar()` in `CHAPTER_TITLE` mode maps the spine item to its covering TOC entry
(`EpubReaderActivity.cpp:1708-1716`):

```cpp
const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
if (tocIndex != -1) title = epub->getTocItem(tocIndex).title;
```

For the NWT that entry is the **book** — `Éxodo` pointing at `biblechapternav2.xhtml` — because the
TOC has no per-chapter entries. The chapter *is* the spine item, and its number appears only inside
the file, in the `chapter<C>_verse<V>` marker ids. So the number has to be read from the content.

### Design

Add to `EpubReaderActivity`:

```cpp
int chapterNumber_ = -1;      // -1 = unknown / not a Bible
int chapterNumberSpine_ = -1; // spine the number belongs to
```

Resolved **once per section change**, not per page turn, and consumed in `renderStatusBar()`:
when `chapterNumber_ > 0`, render `title + ' ' + std::to_string(chapterNumber_)`; otherwise render
exactly what it renders today.

**Gate it on the Bible check.** `epub->getBibleBookNavSpineIndex() >= 0` is memoised (added in #8),
so a non-Bible book pays one cached int comparison and never reads a file.

### Reading the number cheaply — early exit

Do **not** scan the whole chapter. Stream the inflated XHTML through `VerseAnchors::Scanner` and
**stop at the first anchor**: the first `chapter<C>_verse<V>` marker sits near the top of the file,
so this touches a few hundred bytes of a file that can be 69 KB.

`Scanner::take()` returns the anchors collected so far and does not require a final chunk
(`VerseAnchors.h:46-47`), so the loop can call it after each chunk and stop as soon as it is
non-empty, taking `[0].chapter`. Every one of the 1,189 chapter files has at least one marker, so
a file with none means "not a Bible chapter" — leave `chapterNumber_` at -1 and render today's
title.

Reuse `BibleNavigationActivity`'s existing `streamSpineHtml` shape rather than re-inflating from the
zip: going through `Section` means the HTML cache the reader is about to need is the one this
produces. Since #8 introduced that helper on the activity, extracting it to a shared place — or
adding a small `Section`-level helper — is part of this change; **do not duplicate it.**

**Cost:** one file open plus a few hundred bytes read, once per chapter change, on a path that is
already loading and often laying out a section. Nothing is held resident.

## Feature 2: a footer hint on the chapter list

`BibleNavigationActivity` currently overrides `drawChrome()` (`:435`) but **not** `drawFooter()`, so
it inherits the base hints and advertises nothing about the long-press.

Add a `drawFooter()` override following `EpubReaderFootnotesActivity.cpp:92-96`:

```cpp
const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
```

and, **at the Chapter level only**, surface the long-press. `mapLabels` is the established way to
render hints for this hardware; the hint text is a new `tr()` string, `STR_HOLD_FOR_VERSES`
("Hold a chapter for verses" / Spanish equivalent), added to **both** `english.yaml` and
`spanish.yaml`, then `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`. Commit YAML only.

Book and Verse levels keep plain Back/Select hints — there is no long-press to advertise on the
Verse level, and on the Book level it only applies to the five single-chapter books, which is too
conditional to be worth a hint that would be wrong for the other 61.

## Risks

- **The early-exit scan runs on a path that may already be slow.** A cold chapter pays an inflate
  before the scan can read anything. It is the *same* inflate the page build needs next, so the work
  is not duplicated — but the scan must come after `hasHtmlCache()`/`ensureHtmlCache`, never trigger
  a second inflate, and must not run for non-Bibles.
- **`streamSpineHtml` currently lives on `BibleNavigationActivity`.** Sharing it with the reader is
  the part of this change most likely to be done by copy-paste. It must be extracted once.
- **Status bar width.** `Éxodo 5` is two characters longer than `Éxodo`; a long book name plus a
  three-digit chapter (`Salmos 119`) must not collide with the clock. Check the longest case in all
  four orientations.
- **Title mode is a setting.** The suffix applies only in `CHAPTER_TITLE` mode; `BOOK_TITLE` mode
  shows `epub->getTitle()` and must be left alone.

## Testing

**Host** — the chapter-number extraction is the only piece with logic worth isolating. Add
`test/bible_chapter_number/` (registered in `test/CMakeLists.txt`, modelled on
`test/verse_anchors/CMakeLists.txt`, which already compiles expat with `-DXML_GE=0`):
- a chapter fixture whose first marker is `chapter5_verse1` → 5
- a fixture with no markers → not found (and the reader falls back to today's title)
- a fixture where the first marker is far into the file → still found
- chunk boundaries at 1, 7 and 4096 bytes, including a boundary splitting the id attribute

**Device** (flag for the user):
1. Open Exodus 5 → status bar reads `Éxodo 5`.
2. Page forward into Exodus 6 → it updates to `Éxodo 6`.
3. Open Psalm 119 → `Salmos 119` fits without colliding with the clock, in all four orientations.
4. Open a non-Bible EPUB → status bar is unchanged, and opening a chapter is no slower than before.
5. Switch the status bar to Book title mode → unchanged behaviour.
6. Open a Bible chapter list → the footer advertises the verse-list gesture, and holding a row
   still opens the verse list.

**Build:** `pio run -e x4pro` after the last edit. `./bin/clang-format-fix -g`.

## Open questions

None.
