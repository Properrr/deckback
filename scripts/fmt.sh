#!/usr/bin/env bash
# Format, in place, only the files git reports as modified or untracked: launcher C++ with the
# pinned clang-format (lib.sh), and patch-touched tree files with the tree's own clang-format and
# `gn format` inside the container. No arguments, no check mode (`just preflight` checks).
. "$(dirname "$0")/lib.sh"

if [ "$#" -gt 0 ]; then
  die_usage "fmt.sh takes no arguments (got: $*). It formats in place; there is no check mode."
fi

cf="$(clang_format_bin)"
changed=$(
  git diff --name-only -- 'launcher/*.cpp' 'launcher/*.hpp'
  git ls-files --others --exclude-standard -- 'launcher/*.cpp' 'launcher/*.hpp'
)
if [ -n "$changed" ]; then
  info "clang-format $CLANG_FORMAT_VERSION on modified launcher/ files ..."
  printf "%s\n" "$changed" | xargs -r "$cf" -i --style=file
else
  info "no modified launcher/*.{cpp,hpp} — nothing for clang-format to do."
fi

# In-tree patches + .gn: the tree's own clang-format / gn format inside the container, on the files
# `git diff` reports touched only (a dirty tree blocks `just sync`). Container failures propagate;
# grep's 1 on "no matching files" is the one expected non-zero.
if [ -d "$COBALT_TREE/.git" ]; then
  info "Tree clang-format + gn format on patch-touched files ..."
  # No trailing `exit`: `-l` makes this a login shell, and under `set -e` the failing last command of
  # Debian's ~/.bash_logout replaces the status. End on a plain successful command instead.
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

# Same rule for shell: only modified scripts (scripts/ as a whole is not shfmt-clean).
# -i 2: this repo indents shell with two spaces; shfmt's default is tabs.
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
