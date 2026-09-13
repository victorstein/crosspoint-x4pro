# Reader return stack: one navigation choke point

**Date:** 2026-09-12
**Status:** Design v2 (revised after adversarial review)
**Target:** CrossPoint Reader firmware, `x4pro` build target (UC8179 panel, ESP32-S3, 8MB PSRAM)
**Delivery:** `origin` (victorstein/crosspoint-x4pro), branch `fix/reader-return-stack`

## Goal

Following a citation and then selecting a different chapter strands the reader: Back keeps
throwing the user back to the chapter they read several jumps ago, and the only escape is
leaving the book entirely. Reported symptom:

> the footnote selection of chapters stops working after a couple of searches and it keeps
> routing back to the last book no matter if I select a new chapter from the menu. I have
> to use the capacitive key to exit out of the book entirely for this to work again.

## Non-goals

- **Raising `MAX_FOOTNOTE_DEPTH`.** Three slots stay three slots (decided).
- **Changing Back when the stack is empty.** `handleBackNavigation()` (`:589`) is untouched.
- **Re-anchoring saved positions to visible offsets.** Deferred — see Open questions.

---

## Why it breaks today

```cpp
struct SavedPosition { int spineIndex; int pageNumber; };
static constexpr int MAX_FOOTNOTE_DEPTH = 3;
SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
int footnoteDepth = 0;
```
— `EpubReaderActivity.h:68-74`

`navigateToHref(href, /*savePosition=*/true)` pushes (`:1760-1764`); a short Back pops via
`restoreSavedPosition()` (`:583-587` → `:1793-1807`).

**There are two independent causes of the reported symptom.** The original design named
only the first.

### Cause 1 — no deliberate jump clears the stack

Five navigation blocks move the reader by explicit user choice and every one writes
`currentSpineIndex` while leaving the counter alone:

| Block | Route |
| --- | --- |
| `:376-383` | `openHighlights()` — jump to a highlight |
| `:717-720` | `jumpToPercent()` (`:682`) — Go to percent |
| `:740-743` | `progressChangeResultHandler` — offset path |
| `:764-777` | `progressChangeResultHandler` — `ProgressMapper` fallback path |
| `:807-814` | `SELECT_CHAPTER` result |

`progressChangeResultHandler` (`:726`) has exactly **one** consumer — `BOOKMARKS` at `:935`.
The `SYNC` case (`:928-930`) is **not** a site: `launchKOReaderSync()` ends in
`activityManager.replaceActivity(...)` (`:984`), destroying the activity outright.

A full inventory of every mutation of `footnoteDepth` — `EpubReaderActivity.h:74` and
`.cpp:150, 583, 596, 1760-1763, 1777, 1794-1797` — contains only `++`, `--`, and the
failure rollback. **Nothing ever resets it to 0.** So after one citation, choosing a
chapter leaves the counter at 1, and the next short Back runs `restoreSavedPosition()`.
That branch (`:583-587`) pre-empts `handleBackNavigation()` and is *not* gated on
`SETTINGS.shortPwrBtn`, so it hijacks Back for every user. Only a **hold** past
`GO_BACK_OR_HOME_MS` (`:584`) falls through — which is exactly the "capacitive key to exit
out of the book" the report describes.

### Cause 2 — the destructor persists the footnote origin as reading progress

```cpp
if (footnoteDepth > 0 && epub) {
  const SavedPosition& origin = savedPositions[0];
  saveProgress(origin.spineIndex, origin.pageNumber, 0);
}
```
— `:150-153`

Leaving the book with a non-empty stack overwrites `progress.bin` with the *article*
position. Re-opening the book then lands on the old article regardless of what chapter was
last selected — the "keeps routing back" half of the report, surviving a full exit. Cause 1
alone does not explain that; both must be fixed.

### Alternatives ruled out

`clearDeferredReposition()` runs at `:809` before the chapter jump and
`applyDeferredReposition()` only acts when `currentSpineIndex == cachedSpineIndex`
(`:1429`). `pendingAnchor` is cleared on every section build (`:1285`). The end-of-book
path is gated by `isAtEndOfBook()` (`:1096`), unreachable mid-article.

## Design

### One choke point

Five blocks repeat the same `RenderLock` / `clearDeferredReposition()` /
`currentSpineIndex` / `section.reset()` / `requestUpdate()` shape. Extract it:

```cpp
enum class ReturnPolicy : uint8_t { Clear, Push, Preserve };
enum class SectionMode : uint8_t { Reset, ReuseIfSameSpine };

struct NavTarget {
  int spineIndex;
  int pageNumber = 0;
  std::optional<uint32_t> offsetJump;   // -> pendingOffsetJump
  std::string anchor;                   // -> pendingAnchor  (moved, never copied)
  std::optional<float> spineProgress;   // -> pendingSpineProgress + pendingPercentJump
  SectionMode sectionMode = SectionMode::Reset;
};

void navigateTo(NavTarget target, ReturnPolicy policy = ReturnPolicy::Clear);
```

`Clear` is the **default**, so a future navigation feature that does not think about the
return stack still behaves. Bible verse navigation
(`2026-09-12-bible-verse-navigation-design.md`) adds the sixth site.

### The section-reset rule (do not get this wrong)

`navigateToHref` resolves a same-file link to the current spine:

```cpp
bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);
```
— `:1772-1773`

An in-file citation (`href="#citation1"`, the dominant shape in JW publications) therefore
targets the *current* spine with a *live* section. A naive `navigateTo` that reuses the
section whenever the spine matches would take the fast path, set `currentPage = 0`, and
never consume `pendingAnchor` — which is only resolved inside the rebuild block
(`:1279-1286`). The citation would land on page 0 of the current chapter, and the stale
anchor would then hijack the *next* chapter's build and suppress its deferred reposition
(`:1173`). **That is a new bug in the exact feature being fixed.**

Hence the explicit rule, not an inference:

> `SectionMode::ReuseIfSameSpine` is legal **only** when `anchor` is empty, `offsetJump` is
> unset and `spineProgress` is unset. `navigateTo` asserts this. `pendingAnchor`,
> `pendingOffsetJump`, `pendingSpineProgress` and `pendingPercentJump` are assigned **only**
> on the reset branch; the reuse branch writes `section->currentPage` and nothing else.

Only two callers pass `ReuseIfSameSpine`: `openHighlights()` and the handler's offset path.
`SELECT_CHAPTER` is safe today only by accident — `:790-799` resets `section` before
launching the chooser, so its lambda always sees `section == nullptr`.

The two reuse callers differ in their fallback page: `openHighlights` uses
`page.value_or(0)` (`:378`), the handler uses `page.value_or(std::max(0, sync.page))`
(`:738`). A single `value_or(target.pageNumber)` reproduces both, with `openHighlights`
passing `pageNumber = 0`. The handler's third outcome — no section at all, set
`nextPageNumber` only (`:774-776`) — is `SectionMode::Reset` with no pending fields.

### Locking

`RenderLock` is a **non-recursive** mutex taken with `portMAX_DELAY`
(`ActivityManager.cpp:359-362`). `navigateTo` takes it internally, so **no caller may hold
one when calling it** — that deadlocks the reader permanently. Every field the render task
reads (`:1183`, `:1288-1292`) is written inside that lock, which is why `spineProgress`
lives in `NavTarget` rather than being set by the caller: hoisting `pendingPercentJump`
outside the lock would be an unsynchronized write to render-task state.

`target.anchor` is taken by value and **moved** into `pendingAnchor`, matching `:1784`, so
no allocation is added per navigation.

### The ring

Extract the stack into a dependency-free header, `src/activities/reader/ReturnStack.h`,
following the `TagRowMapping.h` precedent (whose own comment records that it exists because
"an adversarial review found" arithmetic that could not be tested on device):

```cpp
struct SavedPosition { int spineIndex; int pageNumber; };

class ReturnStack {
 public:
  static constexpr int CAPACITY = 3;
  void push(SavedPosition p) {
    slots_[top_] = p;
    top_ = (top_ + 1) % CAPACITY;
    if (count_ < CAPACITY) count_++;
  }
  bool pop(SavedPosition& out) {
    if (count_ == 0) return false;
    top_ = (top_ + CAPACITY - 1) % CAPACITY;
    count_--;
    out = slots_[top_];
    return true;
  }
  void unpush() { top_ = (top_ + CAPACITY - 1) % CAPACITY; if (count_ > 0) count_--; }
  void clear() { top_ = 0; count_ = 0; }
  int count() const { return count_; }
  // Physically-oldest retained entry. NOT slots_[0] once the ring has wrapped.
  const SavedPosition* oldest() const {
    return count_ == 0 ? nullptr : &slots_[(top_ - count_ + CAPACITY) % CAPACITY];
  }
 private:
  SavedPosition slots_[CAPACITY] = {};
  int top_ = 0, count_ = 0;
};
```

The pop formula is written out because a literal rename of the existing
`footnoteDepth--; savedPositions[footnoteDepth]` (`:1794-1796`) is **wrong** on a ring: after
four pushes (slots 0,1,2,0) the newest is slot 0 but index-by-count reads slot 2 —
reintroducing the off-by-one this change exists to remove.

**`oldest()` exists solely for the destructor** (Cause 2). `:150-153` must become
`stack.oldest()`, never `slots_[0]`, or exiting after four citations persists the *newest*
footnote as reading progress. The policy itself is unchanged and still wanted: leaving mid-
citation should save the article position, not the note.

**The eviction trade-off, stated honestly.** The ring evicts the **oldest** push — the
article origin, which is the position the user most wants back. At depth ≥ 4 the new
behaviour gives three correct one-step returns and then drops out of the book, where today's
gives four wrong-by-one returns that happen to terminate at the article. The new behaviour is
chosen because every individual Back is correct and depth ≥ 4 without an intervening Back is
rare; it is not a free win.

**RAM: +4 bytes.** `SavedPosition` is two `int` = 8 B, ×3 = 24 B, unchanged. `top_` +
`count_` (8 B) replace `footnoteDepth` (4 B). No heap, no allocation.

### Push rollback

`:1775-1779` rolls the push back when an href does not resolve. On a ring a push at capacity
has **already destroyed** the oldest entry, so rollback cannot restore it. **Resolve the
target first, push only on success** — `navigateTo` with `ReturnPolicy::Push` pushes after
`targetSpineIndex >= 0` is known, making `unpush()` unnecessary on that path. It is provided
anyway for any future caller that can fail later.

### Per-site policy

| Site | Policy | Section mode |
| --- | --- | --- |
| `:376-383` `openHighlights` | `Clear` | `ReuseIfSameSpine` |
| `:717-720` `jumpToPercent` | `Clear` | `Reset` (+`spineProgress`) |
| `:740-743` handler offset path | `Clear` | `ReuseIfSameSpine` |
| `:764-777` handler fallback | `Clear` | `Reset` |
| `:807-814` `SELECT_CHAPTER` | `Clear` | `Reset` |
| `:1757` `navigateToHref(_, true)` | `Push` | `Reset` |
| `:1757` `navigateToHref(_, false)` | `Clear` | `Reset` |
| `:1793` `restoreSavedPosition` | `Preserve` | `Reset` |

**Explicitly NOT routed through `navigateTo`, and why:**

- `pageTurn()` (`:1032`) and `skipPages()` (`:1073`) — sequential reading. Preserving the
  stack is what lets Back still return to the article after paging out of a citation target.
- **`onReturnFromEndOfBook()` (`:1098-1104`)** — writes `currentSpineIndex` *and*
  `pendingPageJump`, which `NavTarget` does not model. Reached from `ReaderActivity.cpp:111`
  and `:133`. It must keep `Preserve`; an implementer told "default is Clear, route every
  site that moves the reader" will get this wrong, so it is called out here.
- `loadBook()` progress restore (`:204-208`, `:225`) — fresh activity, count is already 0.
- `TEXT_SETTINGS` (`:841`), `applyOrientation` (`:1000`), `toggleAutoPageTurn` (`:1026`) —
  stay in place; must not clear.

`savePosition == false` is dead today (all three callers — `:600`, `:607`, `:826` — pass
`true`; the header default at `.h:114` is `false`). It maps to `Clear` rather than being
deleted, so a future in-text link behaves as a deliberate jump.

### Naming

`footnoteDepth` is replaced by the `ReturnStack` member. `MAX_FOOTNOTE_DEPTH` and
`SHORT_PWRBTN::FOOTNOTES` (`:593`) keep their names — following a citation remains the only
thing that pushes. Note `ReturnPolicy` is deliberately *not* called `ReturnStack`, to avoid
colliding with the class.

## Risks

- **Five blocks converted in a 1,936-line file.** Semantics must be identical apart from the
  stack policy: same `RenderLock` scope, same `clearDeferredReposition()`, same ordering. A
  behavioural diff anywhere else is a bug in this change.
- **The reuse/reset rule is the whole ballgame.** Getting it wrong breaks same-file
  citations, which is most of them. The assert is not decoration.
- **File collision.** This and Bible verse navigation both edit `EpubReaderActivity.cpp`.
  Verse navigation branches off this one after it merges.

## Testing

**Host** — `test/return_stack/`, registered with `add_subdirectory(return_stack)` in
`test/CMakeLists.txt` (the list currently ends at `tag_rows`). Testable because
`ReturnStack.h` has no firmware includes; `EpubReaderActivity.h:3-16` pulls in Arduino,
SdFat and `HalStorage`, and `test/stubs/` carries only `HalDisplay.h`, `HalStorage.h` and
`Logging.h`, so the ring could not otherwise be reached.

Cover: push/pop LIFO ordering; wrap at capacity (4 pushes → pop returns the 4th, then 3rd,
then 2nd); `oldest()` after wrap (returns the 2nd push, not slot 0); `clear()` on a partial
and a wrapped ring; `pop` on empty; `unpush` symmetry.

```
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j
```

**Device** (flag for the user). This unit has no physical Back/Confirm — the reader menu
opens on a centre-third screen tap, and citations are reached via **Reader menu → Footnotes**
(`:818-828`) or a Power short-press when `SHORT_PWRBTN` is `FOOTNOTES` (`:599-610`):

1. Follow three citations via the menu, Back three times — each returns one step, ending at
   the article.
2. Follow two citations, then Reader menu → Select Chapter → a different chapter. Back must
   leave the book, not return to the article.
3. Follow four citations, then Back — returns to the third's position, not the first's.
4. Repeat (2) via Bookmarks and Go to percent.
5. Follow a citation, page forward past the end of that spine item, then Back — still
   returns to the article.
6. **Cause 2:** follow a citation, select a different chapter, exit the book, reopen it —
   must reopen at the selected chapter, not the article.
7. Follow a same-file citation (`#citation…`) — must land on the note, not page 0.

**Build:** `pio run -e x4pro` after the last edit. `./bin/clang-format-fix -g`.

## Open questions

- **Saved positions are raw page indices and survive repagination.** `applyOrientation`
  (`:990-1007`) and `TEXT_SETTINGS` (`:831-847`) repaginate, so a Back across either restores
  a stale page number. The file already solves this elsewhere with `cachedVisibleTextOffset`
  (`:1470-1474`). Deliberately **deferred**: it is a pre-existing latent issue, not the
  reported bug, and the agreed scope was the minimal correctness fix. Recorded so the next
  person does not have to rediscover it.
- **`CLAUDE.md`'s Activity Lifecycle section cites `main.cpp:132-143` for activity teardown.**
  That range is `SilentRestart`/`BootResume`; the real teardown is
  `ActivityManager.cpp:180-186` (`exitActivity`) and `:148-153` (stack clear). Worth
  correcting in `CLAUDE.md` separately — it has already propagated into one spec.
