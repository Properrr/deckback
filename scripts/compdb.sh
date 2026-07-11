#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
require_cobalt_checkout
info "Generating compile_commands.json from out/dev ..."
in_container bash -lc "cd $CTR_TREE && ninja -C out/dev -t compdb cc cxx > compile_commands.json"
ln -sf "$COBALT_TREE/compile_commands.json" "$REPO_ROOT/compile_commands.json"
info "Symlinked compile_commands.json to repo root for clangd."
