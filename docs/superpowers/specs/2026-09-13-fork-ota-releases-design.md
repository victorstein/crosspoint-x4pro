# Over-the-air updates from this fork's own releases

**Date:** 2026-09-13
**Status:** Design v1
**Target:** CrossPoint Reader firmware, all boards built from this fork
**Delivery:** `origin` (victorstein/crosspoint-x4pro), branch `feature/fork-ota-releases`

## Goal

Flashing currently needs a USB cable. The device already has everything required to update itself
over WiFi — it just points at the wrong repository and there is nothing there to find.

Make **Settings → check for updates** install this fork's builds, so a tagged release is the whole
release ritual and the cable stops being part of the loop.

## Non-goals

- **Pushing firmware directly from a developer machine.** A `POST /firmware` endpoint duplicates the
  existing SD path and adds a way to brick the device over the network. Out.
- **Delta / partial updates.** Full image only, as today.
- **Changing the flashing mechanism.** `firmware_flash` and the dual-slot partition scheme are
  untouched.

---

## What already works

The device is fully OTA-capable today and none of this needs building:

- **Dual OTA partitions.** `partitions.csv` has `app0`/`app1` at `0x640000` (6.55 MB) each plus
  `otadata`. The current image is 5.62 MB, so it fits with ~14% headroom.
- **`OtaUpdateActivity`** checks, downloads, flashes and reboots, with progress.
- **Board-specific assets.** `OtaUpdater.cpp:35-42` builds the asset name from the compiled board
  tag — `firmware-x4pro.bin` here — and a mismatched image is refused with `WRONG_DEVICE_ERROR`
  (`:208`), so a sticky build cannot land on an X4 Pro.
- **`SdFirmwareUpdateActivity`** flashes a validated `.bin` from the SD card, which combined with the
  web server's `/upload` endpoint is already a cable-free path.

## Three things are missing

### 1. It points at upstream

`OtaUpdater.cpp:22`:

```cpp
constexpr char latestReleaseUrl[] =
    "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/latest";
```

This fork carries features upstream does not have (Bible navigation, meeting downloads). Upstream
firmware would not merely fail to update them — **it would remove them.** Pointing OTA at this fork
is therefore the correct behaviour, not a convenience.

**Make it a build flag, not an edit.** This repo tracks `upstream`, so a hardcoded change to
`OtaUpdater.cpp` is a permanent merge conflict. Instead:

```cpp
#ifndef OTA_RELEASE_REPO
#define OTA_RELEASE_REPO "crosspoint-reader/crosspoint-reader"
#endif
constexpr char latestReleaseUrl[] =
    "https://api.github.com/repos/" OTA_RELEASE_REPO "/releases/latest";
```

with `-DOTA_RELEASE_REPO=\"victorstein/crosspoint-x4pro\"` added to **`[base] build_flags`** in
`platformio.ini`, so every environment built from this fork updates from this fork — including the
other boards, whose binaries this fork also publishes. Upstream's default is preserved when the flag
is absent, which keeps the source change upstreamable rather than fork-only.

### 2. No GitHub Release ever gets created

`release.yml` triggers on `push: tags: '*'` and builds all four boards, but it ends at
`actions/upload-artifact@v4` and declares `permissions: contents: read`. Its own header comment says
to *"attach the artifacts' .bin files to the release"* — a manual human step that has never been
done in this fork. `releases/latest` therefore returns nothing for OTA to find.

**Add a publish job** to `release.yml`:

```yaml
publish-release:
  needs: build-release
  runs-on: ubuntu-latest
  permissions:
    contents: write        # the existing top-level `contents: read` is not enough
  steps:
    - uses: actions/download-artifact@v4    # all four board artifacts
    - run: gh release create "$GITHUB_REF_NAME" --generate-notes firmware*.bin
```

All four binaries are attached, not just x4pro: the asset name is chosen by the *device* from its own
board tag, so publishing only one board would leave the others' OTA finding no asset. The job must be
`needs: build-release` so it cannot publish a partial set when one board fails.

### 3. A `v`-prefixed tag silently reads garbage

`OtaUpdater.cpp:81-88`:

```cpp
int currentMajor, currentMinor, currentPatch;   // uninitialised
int latestMajor, latestMinor, latestPatch;      // uninitialised
sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
sscanf(currentVersion,        "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);
```

**Neither return value is checked**, and `latestVersion` is the raw `tag_name` (`:65`). Compiled and
run against the real libc:

```
tag 'v1.5.1' -> sscanf returned 0, values left untouched (stack garbage in real code)
tag '1.5.1'  -> sscanf returned 3, values 1.5.1
```

So tagging `v1.6.0` makes the update decision depend on whatever was on the stack. This is a latent
bug in existing code, not one this change introduces, but this change is what would trigger it.

**Harden the parser:** skip an optional leading `v`/`V`, check that both `sscanf` calls returned 3,
and treat a version that does not parse as **no update available** rather than comparing garbage.
Both `1.6.0` and `v1.6.0` then work, and a malformed tag fails safe.

## The release ritual, once this lands

1. Bump `[crosspoint] version` in `platformio.ini`.
2. Commit, tag with the **same** value, push the tag.
3. CI builds four boards and publishes the release.
4. On the device: Settings → check for updates → install.

The device reports `<version>-x4pro` while the tag is bare `<version>`; the string equality check at
`:77` therefore does not short-circuit, and the semver compare resolves them as equal. Keeping the
tag and `crosspoint.version` identical is what makes "already up to date" correct — **a tag that
disagrees with the compiled version will offer an update that installs the same build.**

## Risks

- **`release.yml` is upstream content.** Adding a job to it will conflict on the next upstream sync.
  Accepted deliberately: a separate workflow cannot easily reach another workflow's artifacts, and
  duplicating the four-board build to avoid a conflict is worse. Keep the job at the end of the file
  so the conflict is trivial to resolve.
- **OTA becomes fork-locked.** After this, the device only ever sees this fork's releases. That is
  intended, but it means upstream fixes arrive only when merged here and re-released.
- **A bad release is installed over WiFi with no cable in the loop.** The `app0`/`app1` scheme means a
  failed flash falls back to the previous slot, and `WRONG_DEVICE_ERROR` guards board mismatch — but
  a *successfully flashed, genuinely broken* build is recovered only over USB. Tag deliberately.
- **Image size.** 5.62 MB of 6.55 MB used. OTA needs the new image to fit the inactive slot; there is
  headroom now, but a release that crosses 6.55 MB fails at install time, not at build time.

## Testing

**Host** — the version comparison is the testable part and is currently untestable in place. Extract
it as a free function `bool isNewerVersion(const char* latest, const char* current)` into a header
with no firmware includes, and add `test/ota_version/` registered in `test/CMakeLists.txt` on the
`test/return_stack/` shape. Cover:
- `1.6.0` vs `1.5.0-x4pro` → newer; `1.5.0` vs `1.5.0-x4pro` → not newer
- `v1.6.0` accepted identically to `1.6.0`
- `1.5.1` vs `1.5.0` and `2.0.0` vs `1.9.9` → newer
- garbage (`""`, `"abc"`, `"1.5"`) → **not newer**, never a garbage comparison
- a *lower* tag than the device → not newer

**CI** — tag a throwaway release on a branch and confirm the job attaches four `.bin` files and the
release is public before touching the device.

**Device** (flag for the user):
1. With a release published, Settings → check for updates offers it; installing reboots into the new
   build and the version in Settings matches the tag.
2. Re-check immediately → "no update available" (the equal-version path).
3. Publish a release whose tag is *older* than the device → no update offered.
4. Confirm a `firmware-sticky.bin`-only release offers nothing to the X4 Pro rather than erroring.

**Build:** `pio run -e x4pro` after the last edit. `./bin/clang-format-fix -g`.

## Open questions

- **Whether release notes should be generated or written.** `--generate-notes` is assumed above; if
  you want curated notes, the job should create a draft instead and leave publishing manual, which
  costs the hands-off property.
