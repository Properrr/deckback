#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
preset="${1:-dev}"
[ -f "args/${preset}.gn" ] || die_usage "no args/${preset}.gn (expected dev|deck|asan)"
require_cobalt_checkout
bt="$(preset_buildtype "$preset")"

# Cobalt's gn.py writes args.gn for the platform+build_type into out/<preset>.
info "gn.py -p ${COBALT_PLATFORM} -c ${bt} -> out/${preset} ..."
in_container bash -c "cd ${CTR_TREE} && python3 cobalt/build/gn.py --no-rbe -p ${COBALT_PLATFORM} -c ${bt} out/${preset}"

# Layer args/common.gn, then args/<preset>.gn: OVERRIDES on top of Cobalt's config, not full arg sets.
extra="$( { cat args/common.gn args/"${preset}".gn 2>/dev/null; } | grep -vE '^\s*#|^\s*$' || true)"
if [ -n "$extra" ]; then
  info "Layering extra args from args/common.gn + args/${preset}.gn ..."
  argsgn="${COBALT_TREE}/out/${preset}/args.gn"
  # Replace the block appended by a previous run; the marker delimits it.
  sed -i '/^# --- deckback extra args/,$d' "$argsgn"
  printf '\n# --- deckback extra args (common + %s) ---\n%s\n' "$preset" "$extra" >> "$argsgn"
  in_container bash -c "cd ${CTR_TREE} && gn gen out/${preset}"
fi
info "Generated out/${preset} (${COBALT_PLATFORM}/${bt})."
