# Releasing Deckback

Deckback ships two ways from one release:

- a **`.flatpak` bundle** attached to a **GitHub Release** (manual install, no auto-update), and
- a **self-hosted Flatpak repo** on **GitHub Pages** (`https://properrr.github.io/deckback/`) that
  users add once and then get **automatic updates** from.

Both come from a single tagged build. The engine is built on the workstation/CI (never in GitHub
Actions — it's Chromium-scale); Actions only re-packages the finished bundle into the repo and deploys
Pages.

## Versioning & the release branch

`VERSION` is the single source of truth (`X.Y.Z`, no `v`). Every release is prepared on a short-lived
`release/vX.Y.Z` branch, merged to `main`, and tagged `vX.Y.Z`.

```
main ──●───────────────●─(merge)──●  tag v0.0.2
        \             /
         ●─(release/v0.0.2: bump)
```

### 1. Prepare the branch (bump version + changelog + metainfo)

```sh
just release-prep 0.0.2        # creates release/v0.0.2, bumps VERSION, rolls CHANGELOG [Unreleased]
                              #   into [0.0.2], adds the AppStream <release> entry, commits
```

Review the commit; flesh out the `## [0.0.2]` changelog notes with real prose if you like (they become
the GitHub Release body verbatim).

> **The release notes are a product surface — the in-app updater shows them.** In `notify` mode
> (the default), Deckback fetches the GitHub Releases of `properrr/deckback` and displays the notes
> for every version newer than the running one in its "Update available" card
> (`launcher/src/updateprompt.cpp`, findings `durable/self-update.md`). The contract the updater
> relies on, all of it already produced by these scripts — keep it intact:
>
> - **Tag = `v<X.Y.Z>`** (semantic version; the updater strips the leading `v`). `release-prep`
>   enforces the `X.Y.Z` shape.
> - **Body = the `## [X.Y.Z]` CHANGELOG section**, extracted verbatim by `scripts/release.sh`.
> - **Notes are Keep-a-Changelog markdown** (`### Added` / `### Fixed`, `-` bullets, occasional
>   `**bold**`/`` `code` ``). The updater normalises that to clean 10-foot text (`notes_to_plain`):
>   headings lose their hashes, bullets become `•`, emphasis markers drop. So write notes for a
>   human reading them on a couch — terse, user-facing, no internal jargon — not as raw diff shorthand.

### 2. Merge to main and tag

```sh
git checkout main
git merge --no-ff release/v0.0.2
git tag -a v0.0.2 -m "Deckback v0.0.2"
git push origin main v0.0.2
```

Then **wait for CI to go green on the tagged commit** — `just release` won't build without it (see
below). `just release-prep` already ran `just preflight` locally, but CI is the authority.

### 3. Build + draft the GitHub Release (workstation/CI)

```sh
just release v0.0.2           # gold+ThinLTO engine build → .flatpak bundle + engine tarball +
                              #   SHA256SUMS, then drafts the GitHub Release with changelog notes
```

`just release` refuses if the tag doesn't match `VERSION`, **and refuses to build unless every CI
check-run on the tagged commit is `completed/success`** — a Chromium-scale build is the wrong place
to discover a red tag. Override for an offline/emergency build with `FORCE=1 just release <tag>`. It
leaves the release as a **draft**.

### 4. Publish → the repo deploys itself

Review the draft on GitHub and click **Publish**. That fires
[`.github/workflows/release-pages`](.github/workflows/release.yml): it downloads the `.flatpak`,
imports it into an ostree repo, and deploys the site to GitHub Pages — so
`https://properrr.github.io/deckback/` now serves the new version and every user's `flatpak update`
picks it up.

## One-time setup

- **GitHub Pages**: repo Settings → Pages → Source = **GitHub Actions**.
- **(Recommended) sign the repo.** Generate a key and add it as the repo secret `FLATPAK_GPG_KEY`
  (ASCII-armored *private* key); the workflow then signs the repo and inlines the public key into the
  `.flatpakref`/`.flatpakrepo`:
  ```sh
  gpg --quick-generate-key "Deckback Repo <you@example.com>" default default never
  gpg --armor --export-secret-keys <KEYID>   # paste into the FLATPAK_GPG_KEY secret
  ```
  Unsigned works for testing but users must add the remote without gpg verification.

## Test the repo locally (no CI, no Pages)

Needs `flatpak` and the `ostree` CLI (`sudo apt install flatpak ostree` on Debian/Ubuntu — flatpak
alone pulls only the library, not the `ostree` binary the repo init needs).

```sh
just publish-repo                       # imports ./io.github.properrr.deckback.flatpak into
                                        #   flatpak/pages-site/ (repo + index.html + refs)
(cd flatpak/pages-site && python3 -m http.server 8000)
flatpak remote-add --user --from deckback-test http://localhost:8000/deckback.flatpakrepo
flatpak install --user deckback-test io.github.properrr.deckback
```

## Flathub

Deferred. When ready, see [`flathub/SUBMISSION.md`](flathub/SUBMISSION.md); `just flathub-prep <tag>`
produces the extra-data manifest + checksums from the same engine tarball this flow builds.
