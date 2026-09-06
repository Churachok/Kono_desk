#!/bin/sh
# Install Kono so SDDM can list it (needs root).
set -e
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
chmod +x "$root/kono" "$root/kono-session"
install -Dm755 "$root/kono" /usr/local/bin/kono
install -Dm755 "$root/kono-session" /usr/local/bin/kono-session
install -Dm644 "$root/kono.desktop" /usr/share/wayland-sessions/kono.desktop
echo "Installed. Kono should appear in SDDM after you return to the login screen."
