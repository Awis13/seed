#!/bin/sh
set -eu

plugin_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$plugin_dir/bin"
swiftc \
  -O \
  -framework AppKit \
  -framework ApplicationServices \
  "$plugin_dir/companion/CodexPagerCompanion.swift" \
  -o "$plugin_dir/bin/codex-pager-companion"
