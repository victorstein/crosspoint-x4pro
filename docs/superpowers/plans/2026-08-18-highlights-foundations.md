# Highlights Foundations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the five prerequisites that tagged highlights depend on — a rect-invert primitive, a fixed reader menu, and a durable JSON save path — without touching the highlights feature itself.

**Architecture:** Each task is independent and independently shippable. Tasks 1–2 add a new drawing primitive with its bit-twiddling extracted into a pure, host-testable helper. Task 3 fixes an out-of-bounds write that exists today on every device. Tasks 4–5 make JSON persistence crash-safe and make a parse failure distinguishable from an absent file, which is what currently allows silent data loss.

**Tech Stack:** C++20, PlatformIO (ESP32-S3 / `x4pro` env), ArduinoJson 7.4.2, host-side CMake + GoogleTest 1.17.

**Upstream note:** Tasks 3, 4, and 5 fix real bugs affecting all devices and are independent of highlights. Offer them upstream as standalone PRs.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/GfxRenderer/BitBlit.h` (create) | Pure 1bpp bit-range operations, no display/Arduino deps |
| `lib/GfxRenderer/BitBlit.cpp` (create) | Implementation |
| `test/bit_blit/BitBlitTest.cpp` (create) | Host tests for the above |
| `test/bit_blit/CMakeLists.txt` (create) | Suite registration |
| `test/CMakeLists.txt` (modify) | Add the new subdirectory |
| `lib/GfxRenderer/GfxRenderer.h` / `.cpp` (modify) | Public `invertRect`, clipping + rotation + strip mode |
| `src/activities/reader/EpubReaderMenuActivity.h` / `.cpp` (modify) | Raise the item cap, clamp the two unclamped loops |
| `lib/Serialization/DocReadStatus.h` (create) | Read-outcome enum + classifier, host-safe (no Arduino) |
| `lib/Serialization/PersistableStore.h` / `.cpp` (modify) | Atomic write, and a read that reports *why* it failed |
| `test/doc_read_status/` (create) | Host tests for the classifier |

**Verification note:** Tasks 1 and 5 are fully host-testable. Tasks 2, 3, and 4 touch code whose dependency closure (`HalDisplay`, `BoardConfig`, `HalStorage`, Arduino) cannot build on the host, so their verification is a compile plus a stated on-device check. Do not fake unit tests for them.

---

### Task 1: Pure bit-range invert helper

The framebuffer is 1bpp, MSB-first within a byte (`x` → bit `7 - (x & 7)`), rows of `stride` bytes, where `0 = black` and `1 = white`. Extracting the byte loop from the renderer is what makes it testable at all.

**Files:**
- Create: `lib/GfxRenderer/BitBlit.h`
- Create: `lib/GfxRenderer/BitBlit.cpp`
- Create: `test/bit_blit/BitBlitTest.cpp`
- Create: `test/bit_blit/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the header**

```cpp
// lib/GfxRenderer/BitBlit.h
#pragma once

#include <cstdint>

// Bit-level framebuffer operations, deliberately free of display and Arduino
// dependencies so they can be unit tested on the host. The layout matches
// GfxRenderer's framebuffer: 1bpp, MSB-first within a byte (x -> bit 7 - (x & 7)),
// rows of `stride` bytes.
namespace bitblit {

// Flips every bit in the inclusive pixel range [x0, x1] on each row in [y0, y1].
// Coordinates are physical framebuffer coordinates and MUST already be clipped:
// out-of-range input writes out of bounds.
void invertRect(uint8_t* buf, int32_t stride, int x0, int y0, int x1, int y1);

}  // namespace bitblit
```

- [ ] **Step 2: Write the failing test**

```cpp
// test/bit_blit/BitBlitTest.cpp
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "GfxRenderer/BitBlit.h"

namespace {

// 4 bytes per row (32 px), 4 rows.
constexpr int32_t kStride = 4;
constexpr int kRows = 4;

std::array<uint8_t, kStride * kRows> makeBuf(const uint8_t fill) {
  std::array<uint8_t, kStride * kRows> buf{};
  buf.fill(fill);
  return buf;
}

}  // namespace

TEST(BitBlit, InvertsASinglePixel) {
  auto buf = makeBuf(0x00);
  bitblit::invertRect(buf.data(), kStride, 0, 0, 0, 0);
  EXPECT_EQ(buf[0], 0x80) << "x=0 must be the most significant bit";
  EXPECT_EQ(buf[1], 0x00);
}

TEST(BitBlit, InvertsAPartialRangeInsideOneByte) {
  auto buf = makeBuf(0x00);
  // Pixels 2..5 -> bits 5,4,3,2 -> 0b00111100.
  bitblit::invertRect(buf.data(), kStride, 2, 0, 5, 0);
  EXPECT_EQ(buf[0], 0x3C);
}

TEST(BitBlit, InvertsARangeSpanningThreeBytes) {
  auto buf = makeBuf(0x00);
  // Pixels 4..20: tail of byte 0, all of byte 1, head of byte 2.
  bitblit::invertRect(buf.data(), kStride, 4, 0, 20, 0);
  EXPECT_EQ(buf[0], 0x0F);
  EXPECT_EQ(buf[1], 0xFF);
  EXPECT_EQ(buf[2], 0xF8);
  EXPECT_EQ(buf[3], 0x00);
}

TEST(BitBlit, OnlyTouchesRowsInRange) {
  auto buf = makeBuf(0x00);
  bitblit::invertRect(buf.data(), kStride, 0, 1, 31, 2);
  EXPECT_EQ(buf[0], 0x00) << "row 0 untouched";
  EXPECT_EQ(buf[kStride], 0xFF);
  EXPECT_EQ(buf[kStride * 2], 0xFF);
  EXPECT_EQ(buf[kStride * 3], 0x00) << "row 3 untouched";
}

TEST(BitBlit, IsItsOwnInverse) {
  auto buf = makeBuf(0xA5);
  const auto original = buf;
  bitblit::invertRect(buf.data(), kStride, 3, 0, 27, 3);
  EXPECT_NE(buf, original);
  bitblit::invertRect(buf.data(), kStride, 3, 0, 27, 3);
  EXPECT_EQ(buf, original) << "inverting twice must restore the buffer exactly";
}

TEST(BitBlit, IgnoresEmptyRanges) {
  auto buf = makeBuf(0x5A);
  const auto original = buf;
  bitblit::invertRect(buf.data(), kStride, 5, 0, 4, 0);
  bitblit::invertRect(buf.data(), kStride, 0, 3, 7, 2);
  EXPECT_EQ(buf, original);
}
```

- [ ] **Step 3: Register the suite**

```cmake
# test/bit_blit/CMakeLists.txt
add_executable(BitBlitTest
  BitBlitTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitBlit.cpp
)

target_include_directories(BitBlitTest PRIVATE
  ${REPO_ROOT}/lib
)

target_link_libraries(BitBlitTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BitBlitTest)
```

Add this line to the end of the `add_subdirectory` list in `test/CMakeLists.txt`:

```cmake
add_subdirectory(bit_blit)
```

- [ ] **Step 4: Run the tests to verify they fail**

```bash
cmake -S test -B build/test
cmake --build build/test --target BitBlitTest
```

Expected: FAIL at link time with an undefined reference to `bitblit::invertRect`.

- [ ] **Step 5: Write the implementation**

```cpp
// lib/GfxRenderer/BitBlit.cpp
#include "BitBlit.h"

namespace bitblit {

void invertRect(uint8_t* buf, const int32_t stride, const int x0, const int y0, const int x1, const int y1) {
  if (x1 < x0 || y1 < y0) return;

  const int byteStart = x0 >> 3;
  const int byteEnd = x1 >> 3;  // inclusive
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (x0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (x1 & 7)));

  for (int y = y0; y <= y1; y++) {
    uint8_t* row = buf + static_cast<int32_t>(y) * stride;
    if (byteStart == byteEnd) {
      row[byteStart] ^= static_cast<uint8_t>(headMask & tailMask);
      continue;
    }
    row[byteStart] ^= headMask;
    for (int b = byteStart + 1; b < byteEnd; b++) {
      row[b] ^= 0xFFu;
    }
    row[byteEnd] ^= tailMask;
  }
}

}  // namespace bitblit
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build/test --target BitBlitTest
ctest --test-dir build/test --output-on-failure -R BitBlit
```

Expected: PASS, 6 tests.

- [ ] **Step 7: Commit**

```bash
git add lib/GfxRenderer/BitBlit.h lib/GfxRenderer/BitBlit.cpp test/bit_blit test/CMakeLists.txt
git commit -m "feat(gfx): add host-tested 1bpp rect invert helper"
```

---

### Task 2: `GfxRenderer::invertRect`

Wire the helper into the renderer, mirroring `fillRectImpl`'s clipping, rotation, and strip-mode handling. Do not re-derive that logic — copy its structure so the two stay comparable.

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.h` (declare next to `fillRect`, around line 243)
- Modify: `lib/GfxRenderer/GfxRenderer.cpp`

- [ ] **Step 1: Declare the method**

In `lib/GfxRenderer/GfxRenderer.h`, immediately after the `fillRectDither` declaration:

```cpp
  // Flips every pixel in the rect. On a 1bpp panel this turns black text on a
  // white ground into white text on black without reloading or redrawing any
  // glyphs — unlike the fill-then-redraw-white approach in
  // DictionaryWordSelectActivity, which pays an SD glyph load per word.
  void invertRect(int x, int y, int width, int height) const;
```

- [ ] **Step 2: Implement it**

Add to `lib/GfxRenderer/GfxRenderer.cpp`, directly after `fillRectImpl`:

```cpp
void GfxRenderer::invertRect(const int x, const int y, const int width, const int height) const {
  if (width <= 0 || height <= 0) return;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Clip in logical space.
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int lx0 = std::max(0, x);
  const int ly0 = std::max(0, y);
  const int lx1 = std::min(screenW, x + width);
  const int ly1 = std::min(screenH, y + height);
  if (lx0 >= lx1 || ly0 >= ly1) return;

  // Rotation is rigid, so the bbox of the two opposing corners IS the rect.
  int paX, paY, pbX, pbY;
  rotateCoordinates(orientation, lx0, ly0, &paX, &paY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &pbX, &pbY, panelWidth, panelHeight);

  const int phyX0 = std::min(paX, pbX);
  const int phyX1 = std::max(paX, pbX);  // inclusive
  int phyY0 = std::min(paY, pbY);
  int phyY1 = std::max(paY, pbY);

  // Strip mode: clip to the active band and redirect writes into the strip buffer.
  uint8_t* target = getWriteTarget();
  const int originY = getWriteOriginY();
  const int writeRows = getWriteRows();
  phyY0 = std::max(phyY0, originY);
  phyY1 = std::min(phyY1, originY + writeRows - 1);
  if (phyY0 > phyY1) return;

  bitblit::invertRect(target, static_cast<int32_t>(panelWidthBytes), phyX0, phyY0 - originY, phyX1,
                      phyY1 - originY);
}
```

Add the include at the top of `GfxRenderer.cpp`, with the other project includes:

```cpp
#include "BitBlit.h"
```

- [ ] **Step 3: Verify it compiles**

```bash
pio run -e x4pro
```

Expected: `SUCCESS`. There is no host test for this step — `GfxRenderer`'s closure pulls `HalDisplay` and `BoardConfig`, which do not build on the host. The bit logic it delegates to is already covered by Task 1.

- [ ] **Step 4: Commit**

```bash
git add lib/GfxRenderer/GfxRenderer.h lib/GfxRenderer/GfxRenderer.cpp
git commit -m "feat(gfx): add GfxRenderer::invertRect"
```

- [ ] **Step 5: On-device check (record the result in the PR)**

Flash and confirm a call such as `renderer.invertRect(0, 0, 100, 40)` produces a clean inverted block with no fringing at the left and right edges — the head/tail masks are the likely failure point, and rotation means the physical edges are not always the logical ones.

---

### Task 3: Fix the reader menu out-of-bounds write

`menuRowItems` is a fixed 16-element array. An X4 Pro reading a book with footnotes and bookmarks, with the frontlight present, already builds exactly 16 items. `buildMenuRowItems` clamps, but the refresh loop and the item count do not — so one more entry is an out-of-bounds write **and** an out-of-bounds read by the list widget.

This is a live bug today, before any highlights work.

**Files:**
- Modify: `src/activities/reader/EpubReaderMenuActivity.h:52`
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp:176`, `:191`

- [ ] **Step 1: Raise the cap**

In `src/activities/reader/EpubReaderMenuActivity.h`, replace line 52:

```cpp
  static constexpr size_t MAX_MENU_ITEMS = 16;
```

with:

```cpp
  // 13 unconditional items + FOOTNOTES + BOOKMARKS + FRONTLIGHT already reaches
  // exactly 16 on an X4 Pro, leaving no headroom. Sized with room to grow; the
  // loops below clamp regardless, so this is a capacity, not a contract.
  static constexpr size_t MAX_MENU_ITEMS = 24;
```

- [ ] **Step 2: Clamp the refresh loop and the item count**

In `src/activities/reader/EpubReaderMenuActivity.cpp`, replace the loop that begins at line 176:

```cpp
  for (size_t i = 0; i < menuItems.size(); i++) {
```

with:

```cpp
  // menuRowItems is fixed-size; menuItems is not. Clamp both the refresh loop
  // and the count handed to the list widget, or an overlong menu writes and
  // reads past the array.
  const size_t rowCount = std::min(menuItems.size(), MAX_MENU_ITEMS);
  if (menuItems.size() > MAX_MENU_ITEMS) {
    LOG_ERR("MENU", "Reader menu has %u items, capped at %u", static_cast<unsigned>(menuItems.size()),
            static_cast<unsigned>(MAX_MENU_ITEMS));
  }
  for (size_t i = 0; i < rowCount; i++) {
```

Then replace line 191:

```cpp
  props.count = static_cast<uint16_t>(menuItems.size());
```

with:

```cpp
  props.count = static_cast<uint16_t>(rowCount);
```

Ensure `<algorithm>` is included at the top of the file for `std::min`; add it if absent.

- [ ] **Step 3: Verify it compiles**

```bash
pio run -e x4pro
```

Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/activities/reader/EpubReaderMenuActivity.h src/activities/reader/EpubReaderMenuActivity.cpp
git commit -m "fix(reader): clamp reader menu rows to the array bound

menuRowItems is a fixed MAX_MENU_ITEMS array, but the value-refresh loop
and props.count both used menuItems.size() unclamped. An X4 Pro with
footnotes, bookmarks and a frontlight already builds exactly 16 items,
so one more entry writes and reads past the end."
```

- [ ] **Step 5: On-device check**

Open a book that has footnotes, add a bookmark, and open the reader menu on a device with a frontlight. Confirm the menu renders correctly and scrolls to the last item.

---

### Task 4: Atomic JSON writes

`writeDocToFile` serialises to a `String` and calls `Storage.writeFile`, which truncates in place. An interrupted write leaves a torn file. `ProgressFile::writeAtomic` already solved this for `progress.bin`; generalise the same temp-then-rename discipline for JSON.

**Files:**
- Modify: `lib/Serialization/PersistableStore.h`
- Modify: `lib/Serialization/PersistableStore.cpp`

- [ ] **Step 1: Declare the method**

In `lib/Serialization/PersistableStore.h`, directly after the `writeDocToFile` declaration:

```cpp
  // Crash-safe variant of writeDocToFile: serializes to `<path>.tmp`, closes it,
  // then renames it over `path`. An interrupted write damages only the temp file
  // instead of tearing the real one. Same discipline as ProgressFile::writeAtomic.
  // Prefer this for any file whose loss matters (annotations, user data).
  static bool writeDocToFileAtomic(const char* path, const JsonDocument& doc);
```

- [ ] **Step 2: Implement it**

In `lib/Serialization/PersistableStore.cpp`, after `writeDocToFile`:

```cpp
bool PersistableStoreBase::writeDocToFileAtomic(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  const std::string finalPath = path;
  const std::string tmpPath = finalPath + ".tmp";

  String json;
  serializeJson(doc, json);

  if (!Storage.writeFile(tmpPath.c_str(), json)) {
    LOG_ERR("PERSIST", "Failed to write temp file %s", tmpPath.c_str());
    return false;
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // file first. The brief window where neither exists reads as "no data yet",
  // which is recoverable; a torn file is not.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PERSIST", "Failed to rename %s into place", finalPath.c_str());
    return false;
  }
  return true;
}
```

Add `#include <string>` at the top of the file if it is not already present.

- [ ] **Step 3: Verify it compiles**

```bash
pio run -e x4pro
```

Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add lib/Serialization/PersistableStore.h lib/Serialization/PersistableStore.cpp
git commit -m "feat(serialization): add crash-safe writeDocToFileAtomic"
```

Do **not** switch existing callers to it in this task. Changing bookmark or settings persistence is a separate, individually-testable change.

---

### Task 5: Distinguish "file absent" from "file broken"

`readDocFromFile` returns `false` for three very different outcomes: the file does not exist (normal on first boot), the read came back empty, and the JSON failed to parse. Callers therefore treat a corrupt file as "no data", and the next save overwrites it. Combined with `SDCardManager`'s silent 50 KB read truncation, that destroys the file.

This task adds the distinction. Task 3 of the *feature* plan will consume it.

**Files:**
- Create: `lib/Serialization/DocReadStatus.h`
- Modify: `lib/Serialization/PersistableStore.h`
- Modify: `lib/Serialization/PersistableStore.cpp`
- Create: `test/doc_read_status/DocReadStatusTest.cpp`
- Create: `test/doc_read_status/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Add the status type in its own host-safe header**

`PersistableStore.h` includes `<Arduino.h>`, so it can never compile on the host. The
status type and its classifier therefore live in a new dependency-free header.

```cpp
// lib/Serialization/DocReadStatus.h
#pragma once

#include <cstdint>

// Why a document read did not yield a usable document. Callers that own user
// data MUST distinguish Missing (safe to treat as empty and overwrite) from
// Unreadable / ParseError (never overwrite — the data may still be there).
enum class DocReadStatus : uint8_t {
  Ok,
  Missing,
  Unreadable,
  ParseError,
};

// Classifies a read attempt from its three observable outcomes. Deliberately
// free of Arduino and storage dependencies so it can be unit tested on the host.
constexpr DocReadStatus classifyDocRead(const bool exists, const bool contentEmpty, const bool parseFailed) {
  if (!exists) return DocReadStatus::Missing;
  if (contentEmpty) return DocReadStatus::Unreadable;
  if (parseFailed) return DocReadStatus::ParseError;
  return DocReadStatus::Ok;
}
```

In `lib/Serialization/PersistableStore.h`, include it and declare the new method next to
the existing read method:

```cpp
#include "DocReadStatus.h"
```

```cpp
  // As readDocFromFile, but reports why the read failed.
  static DocReadStatus readDocFromFileChecked(const char* path, JsonDocument& doc);
```

- [ ] **Step 2: Write the failing test**

```cpp
// test/doc_read_status/DocReadStatusTest.cpp
#include <gtest/gtest.h>

#include "Serialization/DocReadStatus.h"

TEST(DocReadStatus, AbsentFileIsMissing) {
  EXPECT_EQ(classifyDocRead(false, false, false), DocReadStatus::Missing);
  EXPECT_EQ(classifyDocRead(false, true, true), DocReadStatus::Missing)
      << "absence dominates: nothing else was observed";
}

TEST(DocReadStatus, EmptyContentIsUnreadable) {
  EXPECT_EQ(classifyDocRead(true, true, false), DocReadStatus::Unreadable);
}

TEST(DocReadStatus, ParseFailureIsReported) {
  EXPECT_EQ(classifyDocRead(true, false, true), DocReadStatus::ParseError);
}

TEST(DocReadStatus, GoodReadIsOk) {
  EXPECT_EQ(classifyDocRead(true, false, false), DocReadStatus::Ok);
}

TEST(DocReadStatus, OnlyMissingIsSafeToOverwrite) {
  // The rule the caller depends on: exactly one failure status means "no data
  // was ever there". The other two mean data may exist and must be preserved.
  EXPECT_EQ(classifyDocRead(false, false, false), DocReadStatus::Missing);
  EXPECT_NE(classifyDocRead(true, true, false), DocReadStatus::Missing);
  EXPECT_NE(classifyDocRead(true, false, true), DocReadStatus::Missing);
}
```

- [ ] **Step 3: Register the suite**

```cmake
# test/doc_read_status/CMakeLists.txt
add_executable(DocReadStatusTest
  DocReadStatusTest.cpp
)

target_include_directories(DocReadStatusTest PRIVATE
  ${REPO_ROOT}/lib
)

target_link_libraries(DocReadStatusTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DocReadStatusTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(doc_read_status)
```

- [ ] **Step 4: Run the tests to verify they fail**

```bash
cmake -S test -B build/test
cmake --build build/test --target DocReadStatusTest
```

Expected: FAIL with `Serialization/DocReadStatus.h: No such file or directory`, until Step 1's header exists. If it instead fails with an `Arduino.h` error, the test is including `PersistableStore.h` somewhere — remove that include; the test must depend only on `DocReadStatus.h`.

- [ ] **Step 5: Implement the checked read**

In `lib/Serialization/PersistableStore.cpp`:

```cpp
DocReadStatus PersistableStoreBase::readDocFromFileChecked(const char* path, JsonDocument& doc) {
  if (!Storage.exists(path)) {
    return DocReadStatus::Missing;  // Expected on first boot — not an error.
  }
  String json = Storage.readFile(path);
  if (json.isEmpty()) {
    LOG_ERR("PERSIST", "Failed to read %s (empty)", path);
    return DocReadStatus::Unreadable;
  }
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return DocReadStatus::ParseError;
  }
  return DocReadStatus::Ok;
}
```

Then reduce the existing method to a wrapper so there is one implementation:

```cpp
bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  return readDocFromFileChecked(path, doc) == DocReadStatus::Ok;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build/test --target DocReadStatusTest
ctest --test-dir build/test --output-on-failure -R DocReadStatus
```

Expected: PASS, 5 tests.

- [ ] **Step 7: Verify the firmware still builds**

```bash
pio run -e x4pro
```

Expected: `SUCCESS`. All existing `readDocFromFile` callers keep their current behaviour, since the wrapper preserves the old contract.

- [ ] **Step 8: Commit**

```bash
git add lib/Serialization/DocReadStatus.h lib/Serialization/PersistableStore.h lib/Serialization/PersistableStore.cpp test/doc_read_status test/CMakeLists.txt
git commit -m "feat(serialization): report why a document read failed

readDocFromFile collapsed missing, unreadable and unparseable into a
single false, so callers treat a corrupt file as absent and overwrite
it. readDocFromFileChecked reports which occurred; readDocFromFile is
now a wrapper and its contract is unchanged."
```

---

### Task 6: Run the full suite and confirm no regressions

- [ ] **Step 1: Run every host test**

```bash
cmake -S test -B build/test
cmake --build build/test
ctest --test-dir build/test --output-on-failure -j
```

Expected: all suites pass, including the two new ones.

- [ ] **Step 2: Build the firmware**

```bash
pio run -e x4pro
```

Expected: `SUCCESS`, producing `.pio/build/x4pro/firmware.bin`.

- [ ] **Step 3: Confirm the branch is clean**

```bash
git status --short
git log --oneline upstream/master..HEAD
```

Expected: a clean tree and five feature commits.

---

## What this plan deliberately does not do

- **No highlights.** No new file format, activity, or data model. That is the feature plan.
- **No per-word offsets.** The `TextBlock` change and `SECTION_FILE_VERSION` bump are the anchoring plan, and are the riskiest part of the project.
- **No caller migrations.** `writeDocToFileAtomic` and `readDocFromFileChecked` are added but unused. Switching bookmarks or settings onto them is a separate change with its own on-device verification.
- **No snapshot-buffer resize.** `SNAPSHOT_CAPACITY` only matters once multi-line selection exists; it belongs with the feature that needs it.

## Next plans

1. **Anchoring** — persist per-word visible offsets in `TextBlock`, bump `SECTION_FILE_VERSION` 40→41 and `SECTION_FILE_PARTIAL_VERSION` in lockstep, using `ParsedText`'s uint16_t-delta-plus-sparse-rebase encoding. Requires a host shim for `GfxRenderer`/`Storage` before the repagination-invariance test can run.
2. **Feature** — `HighlightFile` on the atomic write path, `PassageSelectActivity`, `TagPickerActivity`, `HighlightsActivity`, and the render pass applied in the B/W *and* both grayscale planes.
