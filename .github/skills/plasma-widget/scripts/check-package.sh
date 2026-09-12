#!/usr/bin/env sh
#
# Run the structural plasmoid checks on a package directory.
#
# Usage: check-package.sh PLASMOID_DIR
#
# Checks what Plasma reports silently: that config.qml is a ConfigModel, that
# every page it names exists, and that the main.xml entries and the cfg_ aliases
# on the pages match in both directions.

set -eu

if [ "$#" -ne 1 ]; then
    printf 'usage: %s PLASMOID_DIR\n' "$0" >&2
    exit 2
fi

package="$1"
[ -d "$package" ] || { printf 'no such directory: %s\n' "$package" >&2; exit 2; }

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec cmake -DPACKAGE_DIR="$package" -P "$here/check-package.cmake"
