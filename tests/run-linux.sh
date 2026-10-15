#!/bin/sh
# Starts Nexus on a Linux test VM from an SSH session, inside the logged-in desktop session.
#   sh ~/nexus/run.sh [--test]     (restarts it if it already runs)
for pid in $(pgrep -f "[n]exus-venv/bin/python -m nexus"); do kill "$pid"; done
sleep 3  # lets the desktop portal close the old session first; restarting at once wedged it
uid=$(id -u)
export XDG_RUNTIME_DIR=/run/user/$uid DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus
export WAYLAND_DISPLAY=wayland-0 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=KDE QT_QPA_PLATFORM=wayland
cd "$(dirname "$0")/linux" || exit 1
nohup ~/nexus-venv/bin/python -m nexus "$@" > /tmp/nexus.log 2>&1 &
sleep 5
tail -n 20 /tmp/nexus.log
pgrep -f "[n]exus-venv/bin/python -m nexus" >/dev/null && echo "nexus running" || echo "nexus NOT running"
