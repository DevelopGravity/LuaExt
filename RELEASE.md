# Release checklist

The list a maintainer actually walks through to cut a release. The reasoning behind its
shape — why the tag is the trigger, why the GitHub release starts as a draft, why the
stub package goes last — lives in `CONTRIBUTING.md` under *Cutting a release*; this file
is deliberately just the steps.

Versions are plain semver with no `v` prefix — `1.2.0`, or `1.2.0-rc.4` for a
pre-release. The tag, `PHP_LUAEXT_VERSION`, and what Packagist will publish must all be
the same string; `release.yml` refuses a tag that disagrees with the header.

## 1. Before branching

On `develop`:

- [ ] Everything meant for this release is merged, and CI is green on the branch tip.
- [ ] `make dev` passes locally — it is the pre-push command, everything CI will do.
- [ ] The version is decided.

## 2. Cut the release branch

```bash
git flow release start <version>
```

- [ ] You are on `release/<version>`.

## 3. Fix things up on the branch

Release-only polish lands here — the version bump, the changelog, last small fixes.
Anything bigger than polish belongs on `develop` first, so it ships having lived on the
integration branch rather than only on a short-lived one.

- [ ] `PHP_LUAEXT_VERSION` in `src/php_luaext.h` is exactly `<version>`.
- [ ] `CHANGELOG.md` has an entry for `<version>`.
- [ ] `make dev` passes on the branch.

## 4. Finish

```bash
git flow release finish <version>
```

This merges the branch to `main` and back into `develop`, tags the `main` commit — the
empty `versiontag` prefix is what keeps the tag plain — and deletes the release branch.
Trust it, then verify it:

- [ ] `git log --oneline --decorate -1 main` shows the commit carrying tag `<version>`.
- [ ] `git branch --list 'release/*'` prints nothing.

## 5. Push — the tag is the trigger

```bash
git push origin main develop
git push origin <version>
```

- [ ] `release.yml` starts against the tag. There is nothing to click first.

## 6. Watch the build

`release.yml` rechecks that the tag and the header agree, builds the Windows DLLs at the
tag, installs from source on Linux and macOS, loads the built DLL on Windows, and only
then creates a **draft** release carrying those assets.

- [ ] The run is green and a draft release exists, assets attached.

A failed build leaves no release behind at all. A genuine failure means the fix goes
through a fresh release cut; an infrastructure flake can be rerun against the tag
itself: `gh workflow run release.yml --ref <version>`.

## 7. Publish by hand

- [ ] The draft's notes and assets look right.
- [ ] Published. Publishing is what fires `post-publish-verify` — the literal
  `pie install developgravity/lua-ext:<version>` on all three platforms — and it only
  fires for a human publish, because GitHub raises no workflow events for anything
  `GITHUB_TOKEN` does.
- [ ] `post-publish-verify` is green.

## 8. Publish the stub package

Only now, so the stub package never describes a version that turned out not to install:

```bash
git subtree split --prefix=stubs -b stubs-split
git push stubs stubs-split:main
git push stubs "$(git rev-parse stubs-split):refs/tags/<version>"
git branch -D stubs-split
```

The tag is pushed by SHA because the name already exists here, on the extension commit.
The split is deterministic, so both pushes always fast-forward. **If one is rejected, do
not force it** — someone committed directly to the stub repo, and the fix is to move
their change into `stubs/` here and reset the far side deliberately.

- [ ] Both pushes fast-forwarded; `stubs-split` is deleted.
- [ ] Packagist picked the tag up through the webhook — this prints the version, not
  nothing:

  ```bash
  curl -s https://repo.packagist.org/p2/developgravity/lua-ext-stubs.json \
    | grep -o '"version":"<version>"'
  ```

## Notes

- The `stubs` remote is `git@github.com:DevelopGravity/LuaExt-Stubs.git`. A fresh clone
  recreates it with two guards — `skipDefaultUpdate` keeps `git fetch --all` and IDE
  background fetches from dragging the split's branches into this repository, and
  `--no-tags` matters more than it looks, because the stub repo reuses this repository's
  tag names on different commits:

  ```bash
  git remote add stubs git@github.com:DevelopGravity/LuaExt-Stubs.git
  git config remote.stubs.skipDefaultUpdate true
  git config remote.stubs.tagOpt --no-tags
  ```

- The stub repo is generated. Never edit it directly; change `stubs/` here and let the
  next release republish it.
