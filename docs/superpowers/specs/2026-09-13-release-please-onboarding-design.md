# Automated releases via stein-infra's release-please

**Date:** 2026-09-13
**Status:** Design v2 (revised after adversarial review)
**Target:** `victorstein/crosspoint-x4pro` + `victorstein/stein-infra`
**Delivery:** two coordinated PRs, app repo first (see Ordering)
**Builds on:** `2026-09-13-fork-ota-releases-design.md` (merged as #16, `729a1de6`)

## Goal

Merging a PR lands code and nothing else. `release.yml` fires only on `push: tags`, so every
release is a manual tag, and `[crosspoint] version` has to be kept in step with that tag by hand.
Put this repo on the same automated release process the rest of the estate uses, so a merged
`feat:`/`fix:` produces a versioned release with binaries attached and the device can see it.

**This repo is not a GitHub fork.** `gh api ... --jq .fork` returns **false** with a null `parent`;
it is a standalone repo carrying upstream history, which `tofu/repos.tf:387-390` documents
deliberately. So none of the forked-repo restrictions on Actions, releases or the `gh release` API
apply — an assumption worth stating because it was asserted the other way earlier.

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
exists in the shared workflow, but it is **gated on `release_created`** (`release-please.yml:59-69`) —
not unconditional, as an earlier draft said. The `|| true` is on the command, not the step. So the
onboarding push opens the Release PR (no dispatch), auto-merges it, re-fires, and only that **second**
run cuts the tag and dispatches:

```bash
gh workflow run release-publish.yml -R "$REPO" || true
```

The `|| true` means a repo without the file is fine — and also that a *broken* file fails silently.

### `release.yml` → `release-publish.yml`

Model it on `tawtui`'s working version. **It is three jobs, not one** — the earlier draft collapsed
them and would have uploaded nothing:

```yaml
on: { workflow_dispatch: {} }
permissions: { contents: write }
jobs:
  resolve-tag:                       # NEW: the matrix needs the tag too
    outputs: { tag: "${{ steps.tag.outputs.tag }}" }
    steps:
      - id: tag
        env: { GH_TOKEN: "${{ secrets.GITHUB_TOKEN }}" }
        run: |
          TAG=$(gh release view --repo "$GITHUB_REPOSITORY" --json tagName -q .tagName)
          echo "tag=$TAG" >> "$GITHUB_OUTPUT"
  build-release:                     # #16's matrix job, two changes:
    needs: resolve-tag
    # checkout gains: ref: ${{ needs.resolve-tag.outputs.tag }}
  publish-release:                   # #16's job, one change:
    needs: [resolve-tag, build-release]
    # KEEP download-artifact@v4 with path: dist
    # gh release create||upload  ->  gh release upload "${{ needs.resolve-tag.outputs.tag }}" \
    #                                  dist/*/firmware*.bin --clobber
```

Why the single-job sketch fails: with a matrix on the publishing job the upload runs **four times**,
each leg seeing only its own board, and `dist/*/firmware*.bin` expands to nothing because `dist/`
only exists via the `download-artifact` step. The resolve step must be its own job so the build
matrix can consume its output.

**`GITHUB_REF_NAME` must go.** `release.yml:111-112` uses it for the tag; on `workflow_dispatch` it
is the **branch** (`main`), so left in place it would create a release tagged `main`. Replacing those
two lines is what removes the hazard — the earlier draft's "the matrix carries over unchanged" invited
leaving it.

`contents: write` does suffice for `gh release upload`; `download-artifact` in the same run uses the
runtime token. Copy tawtui's resolve step verbatim rather than the sketch above — note it needs the
`GH_TOKEN` env, an `id:`, and `$GITHUB_OUTPUT`, and this repo uses `actions/checkout@v6`.

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
key, and `tofu/release-please.tf:67-76` builds the `packages` object with `jsonencode()`. **Both need
changing**, and the earlier draft specified neither:

- In the `.tf`, read the new fields with **`try(each.value.extra_files, [])`** and
  `try(each.value.bootstrap_sha, null)`. A bare `each.value.extra_files` errors with *"This object
  does not have an attribute named extra_files"* on the six repos that lack it (seed, dotfiles,
  tawtui, WW-experience-migration, stein-home, nicaraguan-laws-MCP). The `for` expression itself is
  safe — `local.fanout_repos` already maps heterogeneous entries the same way in production.
- In the `.tftpl`, render both keys **conditionally**, so a repo without them produces
  byte-identical output.

**Why conditional rendering is not optional:** `github_repository_file.release_please_config` renders
one `templatefile(...)` per opted-in repo. An unconditional key changes the rendered content for all
**six** existing ones, pushing a config-sync commit to each `main` — which fires each repo's own
`release-please.yml`. That directly contradicts this spec's own acceptance gate and stein-infra's
rule that *"resources you didn't touch should not appear in the plan"*. **Expected plan: 1 repo
changed, not 7.**

Add an optional passthrough:

```hcl
release_please = {
  release_type = "simple"
  package_name = "crosspoint-x4pro"
  seed_version = "1.5.0"
  extra_files  = ["platformio.ini"]   # new, optional, defaults to []
}
```

and in `platformio.ini` — **the block form, not the inline one**:

```ini
[crosspoint]
# x-release-please-start-version
version = 1.5.0
# x-release-please-end
```

**An inline `# x-release-please-version` marker corrupts two of the four CI builds.** PlatformIO's
own parser strips inline comments, so the 11 `${crosspoint.version}` interpolations survive — but
there is a **12th consumer**: `scripts/git_branch.py` is a `pre:` script (`platformio.ini:127`) that
reads the same key with a **bare** `configparser.ConfigParser()` (`git_branch.py:71`), which keeps
inline comments. Verified against the real parsers:

```
bare parser (git_branch.py) -> '1.5.0 # x-release-please-version'
PlatformIO's parser         -> '1.5.0'
```

It injects that string as `-DCROSSPOINT_VERSION` for the `default` and `sticky` envs
(`git_branch.py:82-91`) — exactly two of the four boards `ci.yml` builds on every PR and every push
to `main`. The About screen and OTA's `strcmp` short-circuit would both see
`1.5.0 # x-release-please-version-dev-main-abc1234`.

The **block** form is immune: both markers are full-line comments every INI parser ignores, and
release-please's generic updater rewrites only the semver between them.

**Additionally, fix `git_branch.py:71`** to `ConfigParser(inline_comment_prefixes=("#", ";"))`,
matching PlatformIO's own parser. That is a latent bug independent of release-please — any inline
comment on that line breaks the dev version string today.

**Rejected alternative: make `platformio.ini` read `version.txt`.** PlatformIO's ini cannot read a
file, so it would need a `pre:` script injecting `-DCROSSPOINT_VERSION`, which means deleting the
macro from all 11 `build_flags` sites and reconstructing the board/rc suffix variants
(`-x4pro`, `-rc+hash`) in Python. That is a riskier change to the build for a worse outcome, and it
helps no other repo. `extra_files` is additive, defaults empty, and any future non-npm repo whose
version lives outside `version.txt` gets it free.

## The changelog needs a floor: `bootstrap-sha`

With **zero tags** in the repo, release-please's `backfillReleasesFromTags` finds no release for the
manifest's `v1.5.0`, sets `needsBootstrap`, and then walks the branch with none of its three break
conditions satisfied — bounded only by `commit-search-depth` (default **500**).

Measured on the real history: **1,248 commits on `main`**, of which **359 of the most recent 500**
are `feat:`/`fix:`. So the first Release PR and the newly created `CHANGELOG.md` would carry roughly
**430 entries of upstream crosspoint-reader history this tree never released**. (Reassuringly, there
are **0** `!`/`BREAKING CHANGE` markers in all 1,248 commits, so no surprise `2.0.0`.)

The fix is a root-level `"bootstrap-sha"`, which the tofu template also does not support — so this is
a **second** passthrough of equal weight to `extra_files`, not a footnote:

```hcl
release_please = {
  release_type  = "simple"
  package_name  = "crosspoint-x4pro"
  seed_version  = "1.5.0"
  extra_files   = ["platformio.ini"]
  bootstrap_sha = "af9e352f"   # one commit before PR #1
}
```

`af9e352f` ("chore: adopt the tofu-managed repo scaffolding") sits immediately before `34f169a4`
(PR #1), so the changelog starts at this tree's own work. It self-disables once the first Release PR
merges, so it can stay in the rendered config.

**`seed_version` and `bootstrap_sha` do different jobs.** `seed_version` sets the version baseline
(`1.5.0` + a `feat:` → `1.6.0`). `bootstrap_sha` bounds the *commit range*. Setting only the first —
as the earlier draft did — gets the right number attached to the wrong history.

## Make a dev build say which commit it is

Today there is no way to tell which commit a device is running. `/api/status` reports
`1.5.0-x4pro` for **every** x4pro dev build, so two builds four merges apart are indistinguishable
remotely — which already cost us a "did the flash take?" question we could not answer.

The machinery exists and simply excludes this board. `scripts/git_branch.py:78-91` builds
`{base}-dev-{branch}-{short_sha}` and injects it, but bails immediately:

```python
if env['PIOENV'] not in ('default', 'sticky'):
    return
```

because `[env:x4pro]` sets `-DCROSSPOINT_VERSION=\"${crosspoint.version}-x4pro\"` in its own
`build_flags`.

**The change is two edits that must happen together:**

1. Add `'x4pro'` to that tuple.
2. **Remove** the `-DCROSSPOINT_VERSION` line from `[env:x4pro]`'s `build_flags`. Leaving both in
   place defines the macro twice — `env.Append(CPPDEFINES=...)` plus the build flag — which is a
   redefinition, not an override.

**Do not touch `x4pro-gh_release`.** Release builds must keep reporting the bare
`${crosspoint.version}`: that string is what OTA compares against the tag, and a sha suffix there
would break the equality short-circuit in `OtaVersion.h`. The tuple gates on the env name, so the
release env is unaffected by construction — but it is the obvious thing to get wrong.

The dev string loses its `-x4pro` marker (becoming `1.5.0-dev-main-abc1234`, like the other dev
envs). That is fine: `/api/status` already reports `device: xteink_x4_pro` separately, so the board
is never ambiguous.

**Why it belongs in this PR:** once releases are automated, telling a local dev build from an
OTA-installed release becomes a routine question, and `-dev-` in the version answers it at a glance.

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
| Deploys on push to `main`? | **Nothing deploys**, so the skill's row is satisfied — but `ci.yml:3-10` *does* run on push to `main`, with a 4-board matrix and **no `concurrency:` block**. Onboarding pushes four files separately, so ~4 CI runs / ~16 firmware builds, and every version-bump merge re-runs it. Add `paths-ignore` for the release-please files to `ci.yml`'s push trigger, as `WW-experience-migration/deploy.yml` already does. |
| Publishes artifacts on release? | **Yes** — handled above, the whole reason for step 1. |
| Approval rulesets blocking bot self-merge? | **No** `rulesets` block in its `local.repos` entry (`tofu/repos.tf:394-398`), so no `bypass_actors` change needed. |

`seed_version = "1.5.0"` — this tree's current `[crosspoint] version`, with no tags in the repo (the
36 inherited upstream ones were deleted). The skill warns never to regress it. **Upstream being at
1.6.0 is irrelevant**: this tree's version line is its own, and seeding 1.6.0 to "catch up" would
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
- **A ~15-minute window with a release but no assets.** release-please cuts the release, then
  `release-publish` builds four boards. A check in that gap sees `/releases/latest` with no matching
  asset; the device degrades correctly to `NO_UPDATE` (`OtaUpdater.cpp:68-71`) but reports "no update"
  for a version that exists.
- **`allow_auto_merge` is false on the repo**, so `gh pr merge --squash --auto` always falls through
  to the immediate-merge fallback. The Release PR therefore merges **without waiting for `ci.yml`**.
  Works, but it is not the gate it looks like.
- **Doc drift to fix in the app-repo PR:** `AGENTS.md:921` still lists `release.yml` as "Release
  Build"; `AGENTS.md:713` says integration targets `develop`, but the mechanism hardcodes `main`
  everywhere, so a `develop`-targeted PR would be invisible to release-please. Also
  `OtaVersion.h:19-22`'s comment ("a device flashed over the air reports exactly the tag") becomes
  false under `v`-prefixed tags — the logic is still right (the triple comparison catches it), the
  comment is not.

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
