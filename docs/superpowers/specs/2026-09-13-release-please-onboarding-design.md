# Automated releases via stein-infra's release-please

**Date:** 2026-09-13
**Status:** Design v1
**Target:** `victorstein/crosspoint-x4pro` + `victorstein/stein-infra`
**Delivery:** two coordinated PRs, app repo first (see Ordering)
**Builds on:** `2026-09-13-fork-ota-releases-design.md` (merged as #16, `729a1de6`)

## Goal

Merging a PR lands code and nothing else. `release.yml` fires only on `push: tags`, so every
release is a manual tag, and `[crosspoint] version` has to be kept in step with that tag by hand.
Put this repo on the same automated release process the rest of the estate uses, so a merged
`feat:`/`fix:` produces a versioned release with binaries attached and the device can see it.

## Non-goals

- **A bespoke release mechanism.** release-please here is **tofu-managed**; this design opts in.
- **Publishing to a Homebrew tap or npm.** No `TAP_GITHUB_TOKEN` equivalent is needed.
- **Changing OTA behaviour.** #16's flag, size guard and version comparator stand.

---

## The one thing not to do

stein-infra's `onboarding-a-managed-repo` skill names it as the #1 mistake:

> **release-please is tofu-managed, not hand-rolled per repo.** A repo opts in by adding a
> `release_please` block to its `local.repos` entry; `tofu/release-please.tf` then pushes
> `.github/workflows/release-please.yml` + `release-please-config.json` +
> `.release-please-manifest.json` (+ `version.txt`) into the repo. **Do NOT hand-commit those files
> into the app repo** — that creates drift and double-management.

So three of the four files land by `tofu apply`, not by us. Our app-repo PR contributes exactly one
workflow and one version marker.

## The collateral: this repo publishes artifacts on release

From the skill's collateral table:

> **publishes artifacts on release** → *lose the publish (release-please's `GITHUB_TOKEN` tag doesn't
> cascade)* → convert its release workflow to a `workflow_dispatch` **`release-publish.yml`**; the
> mechanism's chain step fires it.

That is `release.yml` exactly. A tag created with `GITHUB_TOKEN` does not trigger other workflows,
so under release-please our four-board publish would **silently never run**. The chain step already
exists in the shared workflow and fires unconditionally:

```bash
gh workflow run release-publish.yml -R "$REPO" || true
```

The `|| true` means a repo without the file is fine — and also that a *broken* file fails silently.

### `release.yml` → `release-publish.yml`

Model it on `tawtui`'s working version:

```yaml
on:
  workflow_dispatch: {}
permissions:
  contents: write
jobs:
  publish:
    steps:
      - name: Resolve latest release tag        # release-please just cut it
        run: TAG=$(gh release view --repo "$GITHUB_REPOSITORY" --json tagName -q .tagName)
      - uses: actions/checkout@v4
        with: { ref: "${{ steps.tag.outputs.tag }}", submodules: recursive }
      # ... existing four-board matrix build, unchanged ...
      - run: gh release upload "$TAG" dist/*/firmware*.bin --repo "$GITHUB_REPOSITORY" --clobber
```

Three changes from what #16 merged:

1. **Trigger** becomes `workflow_dispatch` only. The `tags: ['[0-9]+.[0-9]+.[0-9]+']` filter is
   **deleted**, not adjusted — see the tag-format trap below.
2. **`gh release create` becomes `gh release upload --clobber`.** release-please owns the tag,
   release and changelog now; this workflow only attaches binaries. Creating would fail against the
   release that already exists.
3. **The build must check out the released tag**, not the default branch, or it publishes binaries
   built from whatever `main` happens to be.

The matrix, the artifact layout and the `dist/*/firmware*.bin` glob carry over unchanged — that part
of #16 was correct and is what makes the upload work.

### The tag-format trap

The tofu config template hardcodes **`"include-v-in-tag": true`**, and tawtui's releases confirm the
result: `v0.3.2`, `v0.3.1`, `v0.3.0`.

So release-please will cut **`v1.5.1`**, and #16's filter `'[0-9]+.[0-9]+.[0-9]+'` **rejects a
`v` prefix**. Leaving that filter in place while adding release-please yields a repo where nothing
ever publishes and no error is raised. Deleting the trigger removes the hazard entirely.

This also retroactively vindicates #16's parser hardening: `isNewerVersion` skips an optional leading
`v` and refuses to compare on a failed parse. Without it, **every release-please tag would have
failed `sscanf` and compared uninitialised memory.**

## The version of record

`release_type` must be **`simple`** — this is firmware, nothing is published to npm, and `simple`
makes **`version.txt`** the version release-please bumps.

But our version of record is `[crosspoint] version` in `platformio.ini`, interpolated in **11**
`build_flags` sites to produce `CROSSPOINT_VERSION` — the string the device compares against the
tag. Left alone, release-please would bump `version.txt` while `platformio.ini` sat at `1.5.0`
forever. That is worse than today's manual drift, because it would look automated and correct.

### Decision: teach the tofu template `extra-files`

`tofu/files/release-please-config.json.tftpl` currently emits a fixed config with no `extra-files`
key. Add an optional passthrough:

```hcl
release_please = {
  release_type = "simple"
  package_name = "crosspoint-x4pro"
  seed_version = "1.5.0"
  extra_files  = ["platformio.ini"]   # new, optional, defaults to []
}
```

and in `platformio.ini`:

```ini
[crosspoint]
version = 1.5.0 # x-release-please-version
```

release-please's generic updater rewrites any line carrying that annotation, so `version.txt` and
`platformio.ini` move together and the 11 interpolations keep working untouched.

**Rejected alternative: make `platformio.ini` read `version.txt`.** PlatformIO's ini cannot read a
file, so it would need a `pre:` script injecting `-DCROSSPOINT_VERSION`, which means deleting the
macro from all 11 `build_flags` sites and reconstructing the board/rc suffix variants
(`-x4pro`, `-rc+hash`) in Python. That is a riskier change to the build for a worse outcome, and it
helps no other repo. `extra_files` is additive, defaults empty, and any future non-npm repo whose
version lives outside `version.txt` gets it free.

## Ordering — and why it matters

The skill is explicit: *"App-repo guard/publish PRs merge **first**"*, and *"Merging the stein-infra
onboarding before the app-repo guard/publish PR → double-release or a redeploy."*

1. **App-repo PR (this repo):** `release.yml` → `release-publish.yml`, plus the
   `x-release-please-version` marker. Merge and confirm green.
2. **stein-infra PR:** the `release_please` block for `crosspoint-x4pro`, plus `extra_files` support
   in the template. PR-on-plan / merge-to-apply, per stein-infra's own CLAUDE.md.
3. **Verify**, then cut the first real release.

Reversing 1 and 2 leaves a window where release-please can cut a tag with no `release-publish.yml`
to fire — a release with a changelog and **no binaries**, which the device would offer and fail to
install.

## Collateral checks (from the skill's table)

| Check | This repo |
| --- | --- |
| Deploys on push to `main`? | **No.** `ci.yml` is PR/branch CI only; nothing deploys. |
| Publishes artifacts on release? | **Yes** — handled above, the whole reason for step 1. |
| Approval rulesets blocking bot self-merge? | **No** `rulesets` block in its `local.repos` entry (`tofu/repos.tf:394-398`), so no `bypass_actors` change needed. |

`seed_version = "1.5.0"` — the fork's current `[crosspoint] version`, with no tags in the repo (the
36 inherited upstream ones were deleted). The skill warns never to regress it. **Upstream being at
1.6.0 is irrelevant**: this fork's version line is its own, and seeding 1.6.0 to "catch up" would
claim a version this tree has never shipped.

## Risks

- **The chain step's `|| true` hides a broken `release-publish.yml`.** A syntax error or a bad glob
  produces a release with no binaries and no failure signal. Verify by watching the first dispatch,
  not by assuming.
- **`extra_files` touches shared infra.** It is additive and optional, but it is stein-infra's
  template and every managed repo renders it. The plan must show **0 to destroy** and no other repo
  in the diff.
- **First-release numbering.** A repo whose history since seed is only `chore:`/`ci:` cuts no
  release; the first `feat:`/`fix:` does. Our recent merges are `feat:`, so the first Release PR will
  want `1.6.0` — which collides numerically with upstream's 1.6.0 without being the same code. Worth
  a conscious choice at the time rather than a surprise.
- **Devices in the field.** Nothing reaches a device until it is flashed once with a build carrying
  #16's `OTA_RELEASE_REPO` flag. That bootstrap flash is still required and is unrelated to this.

## Testing

**App repo, before the stein-infra PR:**
- `release-publish.yml` parses: `gh workflow list` shows it, and `actionlint` if available.
- It must **not** be firable into a wrong state: with no release present, the resolve step exits
  non-zero with a clear message rather than uploading to nothing.

**After both merge (the skill's verification):**
```bash
R=victorstein/crosspoint-x4pro
gh api repos/$R/actions/permissions/workflow --jq '{perms:.default_workflow_permissions, approve:.can_approve_pull_request_reviews}'   # {write, true}
gh workflow run release-please.yml -R $R    # opens + self-merges a Release PR, then cuts a tag
gh release list -R $R                        # expect vX.Y.Z with four .bin assets
```
Confirm all four `firmware*.bin` are attached, and that `version.txt` **and** `platformio.ini` both
moved to the new version in the Release PR's diff.

**Device:** after a bootstrap flash, Settings → check for updates offers the release and installs it.

## Open questions

- **Whether the first automated version should be `1.6.0`.** It follows from conventional commits,
  but shares a number with upstream's unrelated 1.6.0. Bumping `seed_version` is not the fix
  (it would claim unshipped versions); overriding the first Release PR's version is.
