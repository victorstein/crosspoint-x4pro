# Host test shim for pagination logic — feasibility spike

Question: can we exercise `ParsedText` pagination on the host cheaply enough to
test repagination invariance for tagged highlights, and if so, how?

**Answer: yes, and it is much cheaper than expected. Recommendation: shim now,
via approach (a).** A working end-to-end probe was built and run during this
spike (throwaway, outside the repo); no production code was written.

Method: transitive include analysis, `nm --undefined-only` over the existing
`.pio/build/x4pro` objects (the authoritative link closure, not a guess), and
an actual host compile+link+run of `ParsedText::layoutAndExtractLines` across
35 font-size/viewport combinations.

---

## 1. Approach (a) — link-time substitute

### Verdict: works, first try, ~55 lines of test-only glue.

#### Arduino can be excluded entirely — no fake `Arduino.h` is needed

This was the load-bearing surprise. `Arduino.h` is reachable from
`ParsedText.cpp` only through exactly three headers:

| Gateway header | Pulls in | Reached from |
| --- | --- | --- |
| `lib/hal/HalDisplay.h` | `Arduino.h`, `EInkDisplay.h` → `FreeInkDisplay.h`, `BoardConfig.h`, `EpdBus.h`, `SPI.h`, `driver/gpio.h`, `esp_rom_sys.h` | `GfxRenderer.h` |
| `lib/hal/HalStorage.h` | `Print.h`, `freertos/semphr.h`, `common/FsApiConstants.h` | `TextBlock.h`, `Bitmap.h` |
| `lib/Logging/Logging.h` | `Arduino.h`, `HardwareSerial.h`, `HWCDC.h` | `ParsedText.cpp`, `BidiUtils.cpp`, `TextBlock.cpp` |

All three are included as `<HalDisplay.h>` / `<HalStorage.h>` / `<Logging.h>` —
by bare basename, resolved off the include path. They live in `lib/hal/` and
`lib/Logging/`, directories the host test targets **never put on the include
path** (`crosspoint_test_common` adds only `${REPO_ROOT}` and `${REPO_ROOT}/lib`,
and the real headers are one level deeper). So a test-only `stubs/` directory is
not *shadowing* the real headers — it is the *sole provider* of those names.
There is no include-order hazard to get wrong.

**The pattern already exists in this repo.** `test/minibidi_arabic/stubs/Logging.h`
is a 4-line no-op stub with the comment "real one needs Arduino". It should be
promoted to a shared `test/stubs/` directory and joined by two siblings.

Measured stub sizes (all three written and compiled during this spike):

| Stub | Lines | Content |
| --- | --- | --- |
| `Logging.h` | 4 | three no-op `LOG_*` macros (already exists) |
| `HalDisplay.h` | 10 | `RefreshMode` enum + 4 `static constexpr` dimensions |
| `HalStorage.h` | 12 | `class HalFile` with 5 declared methods |
| **total** | **26** | |

`GfxRenderer.h` itself compiles unmodified against the 10-line `HalDisplay.h`:
it uses `HalDisplay` for one reference member, four default member initializers,
and `HalDisplay::RefreshMode` in signatures. `HalStorage.h` is even easier —
`TextBlock.h`, `Bitmap.h` and `ImageBlock.h` only take `HalFile&` in
declarations, so an incomplete type would nearly suffice; the 5 methods are
needed because `TextBlock.cpp` (serialization) calls them.

#### Source closure: 9 repo sources, 7 of which are already host-proven

Taken from `nm --undefined-only -C ParsedText.cpp.o` — 14 non-libc undefined
symbols: `BidiUtils::computeVisualWordOrder`, `BidiUtils::startsWithRtl`,
`GfxRenderer::{getKerning, getSpaceAdvance, getSpaceWidth, getTextAdvanceX,
ensureSdCardFontReady(deque)}`, `Hyphenator::breakOffsets`, `isExplicitHyphen`,
`isSoftHyphen`, `logPrintf`, `TextBlock::TextBlock`, `utf8ComposeNfc`,
`utf8NextCodepoint`.

Note `isSdCardFont` never appears: it is inline in the header, so only **five**
of the six `GfxRenderer` methods are link-time symbols at all.

Resolving those transitively:

| Source | Status |
| --- | --- |
| `lib/Epub/Epub/ParsedText.cpp` | **new to host** |
| `lib/Epub/Epub/blocks/TextBlock.cpp` | **new to host** |
| `lib/MiniBidi/BidiUtils.cpp` | already compiled by `minibidi_arabic` |
| `lib/MiniBidi/minibidi.c` | already compiled by `minibidi_arabic` |
| `lib/Utf8/Utf8.cpp` | already compiled by 3 suites |
| `lib/Epub/Epub/hyphenation/HyphenationCommon.cpp` | already compiled by `hyphenation_eval` |
| `lib/Epub/Epub/hyphenation/Hyphenator.cpp` | already compiled by `hyphenation_eval` |
| `lib/Epub/Epub/hyphenation/LanguageRegistry.cpp` | already compiled by `hyphenation_eval` |
| `lib/Epub/Epub/hyphenation/LiangHyphenation.cpp` | already compiled by `hyphenation_eval` |

**Nine sources, only two of them new.** No expat, no zip, no filesystem, no
`Section`, no `Page`, no `Epub`. All nine compiled clean on the host under
`-std=c++20 -Wall -Wextra` with zero warnings and zero source edits. Additional
include dirs beyond the existing convention: `lib/Serialization`, `lib/Memory`,
`lib/Epub/Epub` (for `TextBlock.cpp`).

#### The fake renderer: 11 methods, ~30 lines

`GfxRendererFake.cpp` must define, as link-time substitutes:

- for `ParsedText`: `getKerning`, `getSpaceWidth`, `getSpaceAdvance`,
  `getTextAdvanceX`, `ensureSdCardFontReady(const std::deque<std::string>&, …)`
- for `TextBlock`: `getTextWidth`, `drawText`, `drawLine`,
  `getFontAscenderSize`, `isFontCacheScanning`
- for the inline `~GfxRenderer()`: `freeBwBufferChunks`
- plus `HalFile::read` / `HalFile::write` no-ops (2 more)

It keeps the **real** `GfxRenderer.h`, so if layout code starts calling a
seventh renderer method the test link fails loudly rather than silently
diverging. Fails closed — a good property.

#### Blockers found

None. Two papercuts, both one-liners:

1. `TextBlock.cpp` includes `<Memory.h>`; on a case-insensitive macOS filesystem
   this resolves oddly unless `lib/Memory` is on the include path explicitly.
2. `TextBlock.cpp` includes `"../../../../src/fontIds.h"` by relative path, so
   the repo root must be reachable — it already is.

#### Effort estimate

**Half a day, and that is generous.** The spike above — three stub headers, the
fake renderer, compiling nine sources, linking, and running a real invariance
probe over 35 configurations — took under an hour of wall-clock, most of it
spent on analysis rather than typing. What remains for a production suite is a
`test/pagination/CMakeLists.txt` (~25 lines, copied from `hyphenation_eval`),
moving `minibidi_arabic/stubs/Logging.h` up to a shared `test/stubs/`, and
writing the actual assertions.

Runtime cost is negligible: the existing 13 suites / 152 tests build
incrementally in 0.16 s and run in 0.06 s.

---

## 2. Approach (b) — templating `ParsedText` on the renderer

### Verdict: strictly worse. Do not do this.

**Invasiveness.** Nine member functions take or use `const GfxRenderer&`:
`calculateRubyExtraStartOffset`, `calculateRubyExtraEndOffset`,
`resolveFirstLineIndent`, `computeLineBreaks`, `computeHyphenatedLineBreaks`,
`hyphenateWordAtIndex`, `extractLine`, `calculateWordWidths`,
`layoutAndExtractLines`.

**Polymorphism: not a blocker.** `ParsedText` has no base class and no virtual
functions, and is used in only two places — `std::unique_ptr<ParsedText>
currentTextBlock` in `ChapterHtmlSlimParser.h:41`, and a stack instance in
`src/activities/settings/TextSettingsPreview.cpp:37`. Neither is polymorphic.
`ChapterHtmlSlimParser` would have to either become a template itself or hardcode
`ParsedText<GfxRenderer>`; hardcoding is fine.

**It does force the implementation into a header.** All 1579 lines of
`ParsedText.cpp` move to `ParsedText.h` (or a `.tpp` included from it).

**Build cost.** Transitive fan-out of `ParsedText.h` is bounded but non-trivial:
`ParsedText.h` → `ChapterHtmlSlimParser.h` → `Section.h` → `EpubReaderActivity.h`,
reaching roughly 8 translation units (`TextSettingsPreview.cpp`,
`ChapterHtmlSlimParser.cpp`, `Section.cpp`, `DictHtmlPages.cpp`,
`KOReaderSyncActivity.cpp`, `ProgressMapper.cpp`, `ReaderActivity.cpp`,
`EpubReaderActivity.cpp`) out of 484 objects per env. There are **13** envs in
`platformio.ini` (4 ESP32-C3: `default`, `gh_release`, `gh_release_rc`, `slim`;
9 ESP32-S3), not 7 — worth correcting in the plan. That is ~104 extra
recompiles of a 1579-line header per full matrix build, on the order of
4 minutes added, ~20 s per single-env build.

**Flash cost.** Close to zero in principle — firmware instantiates exactly one
`ParsedText<GfxRenderer>`. The real risk is second-order: moving 1579 lines into
a header changes GCC's cross-TU inlining decisions at `-Os`, which can move
flash in either direction by an unpredictable amount. On the C3 that is a risk
nobody wants to take for a test-only benefit.

**Conclusion.** Approach (b) touches production headers, perturbs the firmware
build, and buys nothing that approach (a) — which touches zero production
files — does not already deliver.

---

## 3. Scope: can we exclude `Section`?

### Yes. Test at the `ParsedText` level and exclude `Section` entirely.

`Section`'s host closure is an order of magnitude larger. From
`nm --undefined-only -C Section.cpp.o`, it needs `Epub` (zip + expat + OPF/NCX
parsing), `ChapterHtmlSlimParser`, `CssParser`, `Page`, `delay`, and — the real
killer — a **genuine filesystem-backed `HalStorage`/`HalFile`**:
`HalStorage::{instance, exists, mkdir, remove, rename, openFileForRead,
openFileForWrite}` and `HalFile::{ctor, dtor, close, operator bool, position,
read, seek, size, write}`, because `Section` serializes paginated pages to SD.
That is a real stdio-backed HAL implementation (150–300 lines), not a stub, plus
`lib/expat` (5 sources), `lib/ZipFile`, `lib/InflateReader`, `lib/miniz` (3),
`lib/uzlib`, and most of `lib/Epub`'s 23 sources.

**And it would not test the property anyway.** Repagination invariance is a
statement about *layout*, and layout is `ParsedText`'s job, not `Section`'s.
`Section` orchestrates: it feeds HTML to `ChapterHtmlSlimParser`, which builds
`ParsedText` objects, which do the breaking. The visible-offset machinery —
`pushVisibleOffset`, `insertVisibleOffset`, `eraseVisibleOffsetPrefix`, the
`uint16_t`-delta + sparse-rebase encoding, and `visibleOffsetAt(lastBreakAt)`
that produces the per-line anchor handed to `processLine` — lives entirely
inside `ParsedText.cpp`. Everything font-size-dependent happens there.

What `Section` *would* add is coverage of offset **assignment** from HTML (does
the parser hand `addWord` the right `visibleTextOffset` for markup, entities,
ruby, footnotes?). That is a genuinely separate property, it is not
font-size-dependent, and it belongs to `ChapterHtmlSlimParser`, not `Section`.
If it needs coverage later, spike `ChapterHtmlSlimParser` on its own — it needs
expat but likely not the zip/SD stack. **Not verified in this spike; flagged as
a follow-up.**

---

## 4. Metrics: is a fixed-width fake sufficient?

### For invariance, yes — with one correction to the naive design, and with a stated limit.

**The correction:** the fake must make advance a function of `fontId`, not a
global constant. A literally fixed `N` px per glyph makes font size a no-op and
the test asserts nothing. In the spike, `advance(fontId) = fontId` was enough.

**Evidence it is sufficient.** The spike laid out a 22-word paragraph with
hyphenation enabled across 7 font sizes × 5 viewport widths = 35 configurations.
It produced 4 to 27 lines per configuration and, critically, drove
**hyphenation to fire between 0 and 9 times** per layout — meaning
`hyphenateWordAtIndex` → `insertVisibleOffset` (the most fragile offset path,
where a split must produce `head offset + head codepoint count`) was exercised
repeatedly and at different places under different sizes. Line-start anchors
were strictly monotonic in all 35 runs.

That is the whole point. What determines *whether* a break happens is whether
accumulated width crosses the viewport — and a fake with size-proportional
advances reaches every break pattern you want by varying word length and
viewport. It reaches the same code paths real metrics do. Meanwhile the property
under test — "the offset assigned to a word does not depend on where the breaks
landed" — is *by construction* independent of the widths that caused the breaks.
Realistic metrics would move the breaks around; they would not reach a path a
fixed-width fake cannot.

**Cheap improvement worth taking:** give the fake a small per-codepoint-class
width table (narrow `i l t`, wide `m w W`) instead of one uniform advance, ~5
extra lines. Asymmetric widths catch order-dependent bugs — e.g. an offset
computed from a *visual*-order index after BiDi reordering rather than a logical
one — that uniform widths can mask.

**What the fake explicitly does NOT cover, and must stay device-verified:**

- *Fidelity*, as opposed to invariance: whether the device breaks lines where
  the host says it does. The fake asserts self-consistency, not agreement with
  real font metrics.
- fp4 fixed-point rounding — specifically `getSpaceAdvance`'s documented
  single-snap-vs-separate-snap ±1 px behaviour.
- Real kerning (the fake returns 0). Safe today because kerning affects x
  positions only, never offset bookkeeping — but that is an invariant the fake
  itself cannot police.
- SD-card font paths: `isSdCardFont` returns false against the fake's empty map,
  so `ensureSdCardFontReady` prewarming and SD-font scaling are untested.

So: the fake is sufficient for the property this test exists to protect, and
insufficient as a substitute for on-device verification. Both remain true; ship
both.

---

## 5. Recommendation

# Shim now — approach (a).

The cost is two new sources added to the host build, 26 lines of stub headers,
a ~30-line fake renderer, and a ~25-line `CMakeLists.txt`. Zero production files
change. The stub-header pattern already exists in this repo
(`test/minibidi_arabic/stubs/Logging.h`) and 7 of the 9 required sources are
already proven host-compilable by existing suites. The whole thing was built and
run end to end during this spike, in under an hour, with no blockers and no
warnings.

Set against that: repagination invariance is the single correctness property the
tagged-highlights feature stands on, its fragile paths (hyphen-split offset
insertion, BiDi reordering, the `uint16_t`-delta rebase encoding) are exactly
the kind that survive manual on-device spot-checks and then fail on someone's
Russian EPUB at font size 20, and on-device verification cannot practically
sweep 35 font/viewport combinations per commit — the host suite does it in
0.06 s.

Concretely:

1. Promote `test/minibidi_arabic/stubs/Logging.h` to `test/stubs/`, add
   `HalDisplay.h` (10 lines) and `HalStorage.h` (12 lines) beside it, and point
   `minibidi_arabic` at the shared directory.
2. Add `test/pagination/` with the 9 sources, `GfxRendererFake.cpp`, and a
   `CMakeLists.txt` modelled on `hyphenation_eval`.
3. Assert, over a font-size × viewport sweep: line-start anchors strictly
   increasing; every anchor is either a word start or a hyphen-split point equal
   to `word offset + head codepoints`; anchors partition the paragraph with no
   gap or overlap; and the same anchor set is reachable at every font size.
4. Leave fidelity, fp4 rounding, and SD-font metrics to on-device verification.
   Say so in the suite's header comment so nobody mistakes the fake for a
   simulator.

Do **not** template `ParsedText` (approach (b)): it touches production headers,
adds ~4 minutes to a full 13-env matrix build, carries unquantified `-Os`
inlining risk on the flash-tight C3, and delivers nothing approach (a) does not.
