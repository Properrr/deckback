#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
preset="${1:-dev}"
[ -f "args/${preset}.gn" ] || die_usage "no args/${preset}.gn (expected dev|deck|asan)"
require_cobalt_checkout
bt="$(preset_buildtype "$preset")"

# Cobalt's gn.py generates args.gn for the platform+build_type into the given out dir (we keep our
# out/<preset> naming via its positional arg). Then we layer the preset's extra args (args/<preset>.gn
# holds only OVERRIDES on top of Cobalt's config — not a full standalone arg set) and re-run gn gen.
info "gn.py -p ${COBALT_PLATFORM} -c ${bt} -> out/${preset} ..."
in_container bash -c "cd ${CTR_TREE} && python3 cobalt/build/gn.py --no-rbe -p ${COBALT_PLATFORM} -c ${bt} out/${preset}"

# Layer deckback overrides: args/common.gn (all presets) first, then the preset's own args/<preset>.gn.
# Both hold only OVERRIDES on top of Cobalt's platform+build_type config — not full standalone sets.
extra="$( { cat args/common.gn args/"${preset}".gn 2>/dev/null; } | grep -vE '^\s*#|^\s*$' || true)"
if [ -n "$extra" ]; then
  info "Layering extra args from args/common.gn + args/${preset}.gn ..."
  argsgn="${COBALT_TREE}/out/${preset}/args.gn"
  # Drop any block we appended on a previous run before appending a fresh one. GN honours the last
  # assignment of a duplicated variable, so stacking blocks was harmless — but args.gn grew a new
  # copy on every `just gen`, and a hand-diff of it became unreadable. The marker delimits *our* block.
  sed -i '/^# --- deckback extra args/,$d' "$argsgn"
  printf '\n# --- deckback extra args (common + %s) ---\n%s\n' "$preset" "$extra" >> "$argsgn"
  in_container bash -c "cd ${CTR_TREE} && gn gen out/${preset}"
fi
info "Generated out/${preset} (${COBALT_PLATFORM}/${bt})."
