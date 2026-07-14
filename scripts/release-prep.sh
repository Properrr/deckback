#!/usr/bin/env bash
# Prepare a release on a dedicated branch: bump VERSION, roll the CHANGELOG's [Unreleased] section
# into a dated version, and add a matching AppStream <release> entry. It does NOT build, tag, or
# push — it produces one reviewable commit on a `release/vX.Y.Z` branch that you merge into main and
# then tag (see RELEASING.md).
#
#   scripts/release-prep.sh <version>        # version WITHOUT a leading 'v', e.g. 0.0.2
#
# Exit: 0 ok · 1 usage · 2 tree/input wrong.
. "$(dirname "$0")/lib.sh"

ver="${1:-}"
[ -n "$ver" ] || die_usage "usage: release-prep.sh <version>   (e.g. 0.0.2, no leading v)"
case "$ver" in
  v*) die_usage "pass the version without a leading 'v' (got '$ver')" ;;
  [0-9]*.[0-9]*.[0-9]*) : ;;
  *) die_usage "version must look like X.Y.Z (got '$ver')" ;;
esac

[ -z "$(git status --porcelain)" ] || die_assert "working tree is dirty — commit or stash first"

# Never cut a release branch from a tree that would land red in CI. Same gate as the pre-push hook,
# one definition (scripts/preflight.sh): shellcheck + harness suite + clang-format-18 + launcher
# gcc/clang builds + gn-args. Cheap insurance before the bump commit that a human then tags.
info "Preflight (the checks CI runs) ..."
"$(dirname "$0")/preflight.sh" all || die_assert "preflight failed — fix before cutting a release"

today="$(date +%F)"
branch="release/v${ver}"
metainfo="flatpak/assets/io.github.properrr.deckback.metainfo.xml"

git rev-parse --verify "$branch" >/dev/null 2>&1 && die_assert "branch $branch already exists"
info "Creating $branch ..."
git checkout -q -b "$branch"

printf '%s\n' "$ver" > VERSION

# CHANGELOG: turn [Unreleased] into [ver] - today, open a fresh empty [Unreleased], and fix the two
# link-reference lines at the bottom.
python3 - "$ver" "$today" <<'PY'
import re, sys
ver, today = sys.argv[1], sys.argv[2]
p = "CHANGELOG.md"
s = open(p).read()
if f"## [{ver}]" in s:
    sys.exit(f"CHANGELOG already has a [{ver}] section")
if "## [Unreleased]" not in s:
    sys.exit("CHANGELOG has no [Unreleased] section to roll")
new_unreleased = ("## [Unreleased]\n\n"
                  "## [%s] - %s" % (ver, today))
s = s.replace("## [Unreleased]", new_unreleased, 1)
# Link refs: repoint [Unreleased] to compare vVER...HEAD and add a [VER] tag link.
s = re.sub(r"\[Unreleased\]: .*",
           f"[Unreleased]: https://github.com/properrr/deckback/compare/v{ver}...HEAD\n"
           f"[{ver}]: https://github.com/properrr/deckback/releases/tag/v{ver}",
           s, count=1)
open(p, "w").write(s)
print(f"CHANGELOG rolled to {ver} ({today})")
PY

# AppStream: insert a new <release> right after <releases>. Notes are intentionally terse and point at
# the changelog; AppStream release descriptions are a summary, not the full log.
python3 - "$ver" "$today" "$metainfo" <<'PY'
import sys
ver, today, p = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
entry = (f'  <releases>\n'
         f'    <release version="{ver}" date="{today}">\n'
         f'      <url>https://github.com/properrr/deckback/releases/tag/v{ver}</url>\n'
         f'      <description><p>See the changelog for details.</p></description>\n'
         f'    </release>')
if '  <releases>' not in s:
    sys.exit("metainfo has no <releases> element")
s = s.replace('  <releases>', entry, 1)
open(p, "w").write(s)
print(f"metainfo: added <release {ver}>")
PY

git add -A
git commit -q -m "Release v${ver}"
info "Committed 'Release v${ver}' on ${branch}."
cat >&2 <<EOF

Next (see RELEASING.md):
  1. Review the diff, edit the [${ver}] changelog notes if you want fuller prose.
  2. Merge to main:   git checkout main && git merge --no-ff ${branch}
  3. Tag + push:      git tag -a v${ver} -m "Deckback v${ver}" && git push origin main v${ver}
  4. Build + draft the release:   just release v${ver}
  5. Publish the GitHub Release → the release-pages workflow deploys the updated repo.
EOF
