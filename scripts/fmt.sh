#!/usr/bin/env bash
# Rewrite sources in place: clang-format over launcher/, shfmt over scripts/, and the tree's own
# clang-format + `gn format` inside the container.
#
# THIS SCRIPT TAKES NO ARGUMENTS AND HAS NO CHECK MODE. It used to accept and silently ignore them,
# so `scripts/fmt.sh check` — which reads as "tell me what is unformatted" — instead reformatted the
# whole world, including 148 .gn/.gni files in the Cobalt checkout, leaving a dirty tree that
# `just sync` then refuses. Reject what we do not implement rather than doing the opposite of it.
#
# Every step is scoped to files `git` reports as modified or untracked. It used to format everything
# it could reach — all of launcher/, all of scripts/, every .gn/.gni in cobalt/src — which meant one
# run rewrote ~160 files nobody had opened and left the Cobalt tree dirty enough that `just sync`
# refuses to run. A formatter whose blast radius is the whole tree is a formatter nobody dares run.
#
# clang-format is PINNED to 18 (via lib.sh's pinned_tool), the same version preflight/CI check, so
# `just fmt` can never write a style the gate then rejects. A host v20/v21 disagrees with v18 and
# would put you in an endless fmt→preflight loop.
. "$(dirname "$0")/lib.sh"

if [ "$#" -gt 0 ]; then
  die_usage "fmt.sh takes no arguments (got: $*). It formats in place; there is no check mode."
fi

# Launcher: LLVM-style clang-format (out-of-tree code), pinned to 18 to match preflight/CI exactly.
#
# Only files you TOUCHED. Formatting the whole of launcher/ would rewrite ten files you never opened
# and hide your actual change inside the churn.
cf="$(pinned_tool clang-format-18 clang-format clang-format==18.1.8)"
changed=$(
  git diff --name-only -- 'launcher/*.cpp' 'launcher/*.hpp'
  git ls-files --others --exclude-standard -- 'launcher/*.cpp' 'launcher/*.hpp'
)
if [ -n "$changed" ]; then
  info "clang-format-18 on modified launcher/ files ..."
  printf "%s\n" "$changed" | xargs -r "$cf" -i --style=file
else
  info "no modified launcher/*.{cpp,hpp} — nothing for clang-format to do."
fi

# In-tree patches + .gn: use the tree's own clang-format / gn format inside the container.
# The checkout guard is the *only* reason to skip. A failure inside the container (gn parse error,
# missing clang-format, container crash) must propagate: this block used to swallow every one of them
# under `|| info "(tree fmt skipped — no checkout yet)"`, which reported a real breakage with a
# message saying nothing was wrong. `grep` returning 1 on "no matching files" is the one expected
# non-zero, so it is neutralized explicitly rather than by silencing the whole block.
#
# Both steps format only what `git diff` says you TOUCHED. `git ls-files "*.gn"` used to feed every
# .gn/.gni in the tree to `gn format`, which rewrote 148 of them (import reordering, list collapsing)
# and left a dirty checkout that `just sync` then refuses. A formatter whose blast radius is the
# whole tree is a formatter nobody dares run.
if [ -d "$COBALT_TREE/.git" ]; then
  info "Tree clang-format + gn format on patch-touched files ..."
  # No trailing `exit`. `-l` makes this a LOGIN shell, so bash runs ~/.bash_logout on the way out;
  # Debian's ends with `[ -x /usr/bin/clear_console ] && ...`, which does not exist in our image and
  # returns 1. Under `set -e` that 1 replaces your status — even a deliberate `exit 3`. End on a
  # plain successful command instead (finding F8).
  in_container bash -lc '
    set -euo pipefail
    cd /src/src
    changed=$(git diff --name-only)
    cc=$(printf "%s\n" "$changed" | grep -E "\.(cc|h|cpp|hpp)$" || true)
    if [ -n "$cc" ]; then printf "%s\n" "$cc" | xargs -r buildtools/linux64/clang-format -i; fi
    gnf=$(printf "%s\n" "$changed" | grep -E "\.gni?$" || true)
    if [ -n "$gnf" ]; then printf "%s\n" "$gnf" | xargs -r gn format; fi
    echo "tree format: ${cc:-no c++} / ${gnf:-no gn} touched"
  ' || die "tree format failed (see the container output above)"
else
  info "No Cobalt checkout — skipping tree clang-format / gn format."
fi

# Same rule for shell. `scripts/` predates shfmt and is not shfmt-clean; a bare `shfmt -w scripts/*.sh`
# rewrites all 25 of them (~200 lines) and buries the one you actually edited. Formatting the rest is
# a reviewed change of its own, not a side effect of `just fmt`.
#
# -i 2 because this repo indents shell with two spaces; shfmt's default is tabs.
if command -v shfmt >/dev/null 2>&1; then
  changed=$(
    git diff --name-only -- 'scripts/*.sh'
    git ls-files --others --exclude-standard -- 'scripts/*.sh'
  )
  if [ -n "$changed" ]; then
    printf "%s\n" "$changed" | xargs -r shfmt -i 2 -w || die "shfmt failed"
  else
    info "no modified scripts/*.sh — nothing for shfmt to do."
  fi
else
  info "shfmt not found on host — skipping scripts/ formatting."
fi
info "Format pass done."
