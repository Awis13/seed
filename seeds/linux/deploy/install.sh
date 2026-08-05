#!/usr/bin/env bash
#
# install.sh — install or upgrade the Linux seed on this box.
#
# Idempotent: the binary, source copy and systemd unit are replaced; data
# files (token, drop.jsonl, drop-cursors.json, notes.json, config.md) are
# never touched. Degrades gracefully on boxes without a running systemd
# (containers): everything is installed and manual next-steps are printed.
set -u

if [ "$(id -u)" -ne 0 ]; then
    echo "install.sh: must run as root — it writes /opt/seed and /etc/systemd/system" >&2
    exit 1
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../seed.c"
unit="$here/seed.service"
install_dir=/opt/seed

[ -f "$src" ] || { echo "install.sh: cannot find $src" >&2; exit 1; }
[ -f "$unit" ] || { echo "install.sh: cannot find $unit" >&2; exit 1; }
command -v gcc >/dev/null 2>&1 || { echo "install.sh: gcc is required" >&2; exit 1; }

mkdir -p "$install_dir"

# Build to a temp name, then atomic rename — safe while the service runs.
echo "Compiling seed..."
if ! gcc -O2 -Wall -Wextra -o "$install_dir/seed.install-tmp" "$src"; then
    rm -f "$install_dir/seed.install-tmp"
    echo "install.sh: compile failed" >&2
    exit 1
fi
chmod 755 "$install_dir/seed.install-tmp"
mv -f "$install_dir/seed.install-tmp" "$install_dir/seed"

# A node may have grown its own source via /firmware/source — keep a copy
# before overwriting, or the grown code is lost silently on re-install.
# First backup wins: it is the oldest grown source, never clobbered.
if [ -f "$install_dir/seed.c" ] && [ ! -f "$install_dir/seed.c.pre-install" ]; then
    cp "$install_dir/seed.c" "$install_dir/seed.c.pre-install" || exit 1
fi

# The node serves and rebuilds its own source via /firmware/source.
cp -f "$src" "$install_dir/seed.c" || exit 1

cp -f "$unit" /etc/systemd/system/seed.service || exit 1

if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
    systemctl enable seed
    # restart (not start): picks up the new binary when already running
    if ! systemctl restart seed; then
        echo "install.sh: systemctl restart seed FAILED — check: journalctl -u seed -n 50" >&2
        exit 1
    fi
    echo ""
    echo "Seed installed and started (unit: seed.service, enabled at boot)."
else
    echo ""
    echo "systemd is not running here — installed, but not started."
    echo "  start manually:      $install_dir/seed 8080 &"
    echo "  or under systemd:    systemctl daemon-reload && systemctl enable --now seed"
fi

echo ""
echo "Token (auth for non-loopback):  $install_dir/token  (created 0600 on first start)"
echo "Health check:                   curl http://localhost:8080/health"
echo "Agent skill file:               curl -H \"Authorization: Bearer \$(cat $install_dir/token)\" http://localhost:8080/skill"
