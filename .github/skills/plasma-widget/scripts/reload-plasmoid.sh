#!/usr/bin/env bash
#
# Install a plasmoid and make the running Plasma shell read it again.
#
# Plasma compiles an applet's QML once and holds it, so a changed main.qml does
# nothing until the shell restarts. Logging out would do the same thing more
# slowly, and reinstalling alone does not invalidate the running copy.
#
# Two states have to be told apart, because the obvious command is a silent no-op
# in one of them. While the user unit owns the shell, restarting the unit is the
# clean way. When it does not own it -- the shell was started by kstart, which is
# where a manual reload leaves it -- `systemctl --user restart` answers success,
# logs "Started KDE Plasma Workspace" and leaves the running shell untouched: the
# process it spawns exits at once because org.kde.plasmashell is already owned,
# the exit status is 0 so Restart=on-failure never retries, and the unit ends up
# inactive. So the unit is used only when it is the owner, and the outcome is
# checked rather than assumed: the old PID is looked for afterwards, and the
# shell is replaced by hand if it is still there. Both PIDs are printed, which is
# the only evidence that anything happened.
#
# Usage: reload-plasmoid.sh [PLASMOID_DIR]

set -uo pipefail

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

if [ "$#" -gt 1 ]; then
    die "usage: $0 [PLASMOID_DIR]"
fi

package="${1:-}"

if [ -n "$package" ]; then
    [ -d "$package" ] || die "no such directory: $package"
    # --upgrade fails when the package is not installed yet, which is the one
    # case where --install is the right call.
    if ! kpackagetool6 --type Plasma/Applet --upgrade "$package" 2>/dev/null; then
        kpackagetool6 --type Plasma/Applet --install "$package" \
            || die "cannot install $package"
    fi
    printf 'installed %s\n' "$package"
fi

command -v plasmashell >/dev/null || die "plasmashell is not on the PATH"
command -v pgrep >/dev/null || die "pgrep is not on the PATH"

before="$(pgrep -x plasmashell | head -n 1 || true)"
[ -n "$before" ] || printf 'no shell is running, starting one\n' >&2

if systemctl --user is-active --quiet plasma-plasmashell.service 2>/dev/null; then
    systemctl --user restart plasma-plasmashell.service
    sleep 1
    after="$(pgrep -x plasmashell | head -n 1 || true)"
    if [ -n "$after" ] && [ "$after" != "$before" ]; then
        printf 'the unit restarted the shell: %s -> %s\n' "${before:-none}" "$after"
        exit 0
    fi
    printf 'the unit is active but the shell did not change; replacing it by hand\n' >&2
fi

# The unit does not own the shell, or its restart was the no-op described above.
# Quit first and wait for the process to disappear: kstart into a shell that is
# still shutting down is the one way this comes back to an empty panel.
quit="$(command -v kquitapp6 || command -v kquitapp5 || true)"
[ -n "$quit" ] || die "neither kquitapp6 nor kquitapp5 is on the PATH"
"$quit" plasmashell || true

for _ in $(seq 40); do
    pgrep -x plasmashell >/dev/null || break
    sleep 0.25
done
if pgrep -x plasmashell >/dev/null; then
    die "the shell is still running; not starting a second one"
fi

kstart plasmashell
sleep 1

after="$(pgrep -x plasmashell | head -n 1 || true)"
[ -n "$after" ] || die "no plasmashell is running: the panel is gone, start one by hand"
printf 'replaced the shell by hand: %s -> %s\n' "${before:-none}" "$after"
