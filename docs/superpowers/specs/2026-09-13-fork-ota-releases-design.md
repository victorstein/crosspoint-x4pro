# Over-the-air updates from this fork's own releases

**Date:** 2026-09-13
**Status:** Design v2 (revised after adversarial review)
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

**The tag trigger must be narrowed first.** `release.yml` fires on `push: tags: '*'` — no filter —
and this clone inherited **36 upstream tags that were never on `origin`** (`1.4.1`, `1.6.0rc`,
`v1.5.0`, …). With a publish job in place, a single `git push --tags` would build 36 *upstream*
commits — none of which have this fork's features — and publish 36 releases. `1.6.0rc` parses as
`1.6.0`, beats the device's `1.5.0`, and whichever lands last becomes `releases/latest`: every fork
device would then OTA itself onto upstream firmware, deleting the features this flag exists to
protect. Restrict the trigger to bare semver and gate the job on this repository:

```yaml
on:
  push:
    tags: ['[0-9]+.[0-9]+.[0-9]+']
```

The inherited tags have been deleted from the local clone (recoverable with
`git fetch upstream --tags`); the filter is what makes that durable rather than a thing to remember.

`release.yml` builds all four boards, but it ends at
`actions/upload-artifact@v4` and declares `permissions: contents: read`. Its own header comment says
to *"attach the artifacts' .bin files to the release"* — a manual human step that has never been
done in this fork. `releases/latest` therefore returns nothing for OTA to find.

**Add a publish job** to `release.yml`:

```yaml
publish-release:
  needs: build-release
  runs-on: ubuntu-latest
  if: github.repository == 'victorstein/crosspoint-x4pro'
  permissions:
    contents: write        # a job-level block REPLACES the top-level `contents: read`
  steps:
    - uses: actions/download-artifact@v4
      with: { path: dist }               # NOT the default layout -- see below
    - env: { GH_TOKEN: "${{ secrets.GITHUB_TOKEN }}" }
      run: |
        gh release create "$GITHUB_REF_NAME" --generate-notes dist/*/firmware*.bin \
          || gh release upload "$GITHUB_REF_NAME" dist/*/firmware*.bin --clobber
```

**Three things here are not optional, and a naive version of this job fails on every release:**

1. **`path: dist` plus a `dist/*/` glob.** Artifacts are uploaded with `name: ${{ matrix.asset }}`,
   so `download-artifact` with no `name` creates a **directory per artifact** — `firmware-x4pro.bin/`
   is a folder containing `firmware-x4pro.bin`, `bootloader.bin`, `firmware.elf`, `firmware.map` and
   `partitions.bin`. A bare `firmware*.bin` glob therefore matches four **directories**, which `gh`
   cannot upload. `merge-multiple: true` is also wrong: the four artifacts all contain
   `bootloader.bin` and `partitions.bin`, which would collide.
2. **`GH_TOKEN` must be set explicitly.** Actions does not export it; `gh` fails with
   *"set the GH_TOKEN environment variable"*. Every existing `gh` call in this repo sets it
   (`release-fonts.yml:53, 76, 92`).
3. **`create` fails if the release already exists**, so a re-run or a moved tag hard-fails. The
   `|| gh release upload --clobber` fallback makes the job idempotent, mirroring what
   `release-fonts.yml:97-99` does for the same reason.

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

**A device updated over the air reports a bare version, not `-x4pro`.** `[env:x4pro]` (the dev
build, flashed over USB) compiles `${crosspoint.version}-x4pro`, but CI builds
`[env:x4pro-gh_release]`, which compiles `${crosspoint.version}` **bare**. So an OTA'd device
reports `1.5.0`, identical to the tag, and the string-equality check at `:77` **does** short-circuit
— the semver comparator is never reached on the common "already up to date" path. An earlier draft
described the opposite and was wrong.

Keeping the tag identical to `crosspoint.version` is what makes that short-circuit fire. **A tag
that disagrees with the compiled version offers an update that installs the same build.**

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
- **Image size is checked far too late.** `checkForUpdate` already stores `otaSize` (`:67`) from the
  release JSON, but `installUpdate` calls `esp_ota_begin(..., OTA_SIZE_UNKNOWN, ...)` (`:138`), erases
  the full 6.55 MB, and only discovers an oversize image when `esp_ota_write` returns
  `ESP_ERR_INVALID_SIZE` — after a multi-minute download. Compare `otaSize` against the inactive
  partition **at check time** and refuse before downloading. (Truncation itself *is* caught:
  `esp_ota_end` validates magic and SHA-256.)
- **The OTA transport does not verify certificates.** `[base]` sets `-DFREEINK_NET_WOLFSSL=1`, so
  `HttpDownloader.cpp:56` calls `setInsecure()` on every hop, including the redirect to
  `objects.githubusercontent.com` that serves the image. `esp_ota_end`'s SHA-256 proves the image is
  self-consistent, **not that it is ours** — an on-path attacker could serve a correctly-checksummed,
  correctly-board-tagged image. Accepted deliberately for a personal device on a home network, and no
  worse than the path upstream already ships; recorded here rather than left implicit. **The comment
  at `OtaUpdater.cpp:31` claiming a "verified-https GET" is false and must be corrected** as part of
  this change — a wrong comment is how this got overlooked in the first place.
- **Currently-deployed devices keep pointing at upstream** until they are flashed once over
  USB or SD. An x4pro asking for `firmware-x4pro.bin` finds nothing in upstream's releases, so it
  simply reports no update — safe, but by accident rather than design.

## Testing

**Host** — the version comparison is the testable part and is currently untestable in place. Extract
it as `bool isNewerVersion(const char* latest, const char* current)` into a header that includes
nothing beyond `<cstdio>`/`<cstring>` — **no `Logging.h`**, since `test/CMakeLists.txt` puts only
`${REPO_ROOT}` and `${REPO_ROOT}/lib` on the include path, and no reference to `CROSSPOINT_VERSION`,
which is undefined on host.

**The extraction has a contract that must be carried over intact, or RC builds break.** The
`-rc` tie-break at `:112-114` returns true when the numeric triples are *equal* and `current`
contains `-rc`. Today that is only safe because `:77`'s raw string equality catches the
identical-version case first. Move the tie-break without the string check and an RC device offers
itself its own running image forever. The function must be:

```
false             if latest is empty
false             if strcmp(latest, current) == 0        // must move with it
false             if either fails to parse 3 ints (after an optional leading v/V)
triple compare    otherwise
true              if triples are equal AND current contains "-rc"
```

The caller keeps the `updateAvailable` member guard. Add `test/ota_version/` registered in
`test/CMakeLists.txt` on the `test/return_stack/` shape. Cover:
- `1.5.0` vs `1.5.0` → **not** newer (the real OTA'd-device case, exercising the string check)
- `1.6.0` vs `1.5.0` → newer; `1.5.0` vs `1.5.0-x4pro` → not newer (the sideloaded-dev-build case)
- `v1.5.0` vs `1.5.0` → **not** newer (today this returns true and offers a reinstall)
- `1.5` vs `1.5.0` and `2.0` vs `1.9.9` → not newer (today both return true on a partial parse)
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

**Build:** `pio run -e x4pro-gh_release` after the last edit — that is the env CI ships, and it differs
from `x4pro` in exactly the macro this change touches. Also build `-e x4pro` for the dev path. `./bin/clang-format-fix -g`.

## Open questions

- **Whether release notes should be generated or written.** `--generate-notes` is assumed above; if
  you want curated notes, the job should create a draft instead and leave publishing manual, which
  costs the hands-off property.
