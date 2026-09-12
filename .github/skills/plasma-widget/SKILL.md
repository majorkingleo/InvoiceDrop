---
name: plasma-widget
description: 'Build, install, test and reload a KDE Plasma 6 plasmoid: a QML panel widget with a popup, a drop target and a settings dialog. USE FOR: creating a plasmoid package and its metadata.json, PlasmoidItem with compact and full representations, DropArea drops on the panel, the executable data engine, KConfigXT main.xml plus ConfigModel config pages, iterating with plasmawindowed, installing with kpackagetool6, reloading the shell after a QML change, and diagnosing the failures Plasma reports silently. DO NOT USE FOR: QML applications that are not Plasma applets, C++ Plasma plugins, or packaging and CMake work.'
argument-hint: 'What should the widget do?'
---

# A KDE Plasma 6 widget (plasmoid)

A plasmoid is a directory of QML that Plasma loads as a package. There is no
compile step, no C++ plugin and no build system needed — which is exactly why so
much of it fails without a word being printed.

Provenance: written while building the InvoiceDrop widget on Plasma 6.7.5, Qt
6.11, Wayland, September 2026. The traps below were hit and measured there, not
copied out of a tutorial.

## When to use

- A panel widget that shows something and opens a popup on click.
- A drop target in the panel: drag a file onto the icon, something happens.
- A widget that shells out to a command and displays the result.
- Anything that needs a settings dialog of its own.

## The package

```
plasmoid/com.example.hello/            the directory kpackagetool6 is pointed at
  metadata.json                        id, name, icon, API version
  contents/config/main.xml             KConfigXT: the settings that exist
  contents/config/config.qml           ConfigModel: which pages the dialog has
  contents/ui/config/ConfigPage.qml    one page, one cfg_ alias per setting
  contents/ui/main.qml                 PlasmoidItem: icon, popup, drop target
  contents/icons/hello.svg             package-local icon, if it has one
```

Nothing here is generated and nothing is compiled. `contents/` is a fixed
structure that Plasma reads directly; every path below is relative to it.

## Procedure

### 1. Scaffold

```bash
mkdir -p plasmoid/com.example.hello/contents/{config,ui/config,icons}
cp .github/skills/plasma-widget/assets/metadata.json plasmoid/com.example.hello/
```

Start from the templates in `./assets/` and rename the id everywhere:
`metadata.json` (`KPlugin.Id`), the directory name, and any icon name. The
installed directory is the id, so the two disagreeing is a widget that installs
under a name you did not ask for.

### 2. `main.qml` is a `PlasmoidItem` with two representations

```qml
import QtQuick
import org.kde.plasma.plasmoid
import org.kde.kirigami as Kirigami

PlasmoidItem {
    id: root

    preferredRepresentation: fullRepresentation

    // The icon in the panel. Small, and still has to accept a drop.
    compactRepresentation: MouseArea {
        onClicked: root.expanded = !root.expanded
        hoverEnabled: true

        Kirigami.Icon {
            anchors.centerIn: parent
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: implicitWidth
            source: Qt.resolvedUrl("../icons/hello.svg")
            active: parent.containsMouse
        }

        DropArea {
            anchors.fill: parent
            onDropped: function (drop) { root.acceptDrop(drop) }
        }
    }

    fullRepresentation: Item {
        implicitWidth: Kirigami.Units.gridUnit * 22
        implicitHeight: Kirigami.Units.gridUnit * 20
        // ... popup content ...
    }
}
```

- A custom compact representation is only a drawing: give it the `MouseArea` that
  toggles `expanded`, because the default one does that for you. KDE's own setup
  page is explicit about this: *"If you change the compact representation, you
  will need to use a MouseArea to toggle the `plasmoid.expanded` property."*
- `preferredRepresentation` chooses which of the two views the applet prefers. In
  a panel the default is the compact icon, with the popup opening on click; the
  InvoiceDrop widget in this repository sets it to `fullRepresentation` because
  the popup is the view that matters there. Change it with a reason, and check
  the result on the panel — `plasmawindowed` does not place the widget in one.
- The compact representation is a few cells wide. A `DropArea` inside it works,
  and is the whole point of a drop target; give the popup one too, so a drop
  lands whether or not the popup happens to be open.
- Oversized popups are clipped by the shell. Size them in
  `Kirigami.Units.gridUnit`, not in pixels.

### 3. Settings are three files that must agree

`contents/config/main.xml` declares the entries:

```xml
<group name="General">
    <entry name="command" type="String">
        <label>Command to run</label>
        <default>hello</default>
    </entry>
</group>
```

`contents/config/config.qml` names the pages:

```qml
import org.kde.plasma.configuration

ConfigModel {
    ConfigCategory {
        name: i18n("General")
        icon: "settings-configure"
        source: "config/ConfigPage.qml"
    }
}
```

`contents/ui/config/ConfigPage.qml` wires each entry to a control:

```qml
Kirigami.FormLayout {
    property alias cfg_command: commandField.text

    QQC.TextField { id: commandField; Kirigami.FormData.label: i18n("Command:") }
}
```

Three things about this, none of which produce a warning:

- **`config.qml` must be a `ConfigModel`.** A plain `Item` loads fine and the
  settings dialog then shows the shortcuts and About tabs and nothing else. This
  is how a released widget shipped with no configuration page.
- **The name after `cfg_` must match the `entry name` exactly.** A missing alias
  is a setting the user cannot reach; an extra alias is a control that edits
  nothing. Both are silent, and so is a typo in either direction.
- **`ConfigCategory.source` is resolved against `contents/ui/`**, not against
  `contents/config/` where the model file lives. That is why the page sits in
  `contents/ui/config/`, one level down from the model that names it.

Check all three mechanically instead of by hand:

```bash
./scripts/check-package.sh plasmoid/com.example.hello
```

### 4. Put every decision in a JavaScript module

`import "logic.js" as Logic` from `main.qml`, and keep command building, reply
parsing and formatting there. A `.js` module can be run standalone:

```bash
qmlscene6 -platform offscreen tests/tst_plasmoid.qml
```

QML that lives in `PlasmoidItem` cannot be tested at all, because running it
needs a shell. Everything that has a decision in it is worth the extra file.

Two details that make the harness work:

- **`i18n()` is `undefined` under `qmlscene6`**, unless the file imports
  `org.kde.kirigami`, which installs it. So do not translate inside the `.js`
  module. Pass what it needs in as a parameter — a `Qt.locale("de_DE")` for
  formatting, a strings object for labels — and the harness can pin a locale and
  assert on the exact text.
- **The harness has to end the process itself.** `Qt.exit()` is ignored by
  `qmlscene6`; `Qt.callLater(Qt.quit)` exits cleanly (measured: exit 0). Wrap a
  scratch probe in `timeout` anyway, because a QML error before the quit leaves it
  running forever.

### 5. Iterate, then install

```bash
plasmawindowed com.example.hello            # window, no install, reads disk every start
```

This is the fast loop: it re-reads the package on every start, so it needs
neither an install nor a shell restart. It loads `contents/ui/main.qml` and
**nothing else** — not the settings dialog, not the panel's own loading path.

Then put it in the panel:

```bash
kpackagetool6 --type Plasma/Applet --install plasmoid/com.example.hello
# later, after edits:
kpackagetool6 --type Plasma/Applet --upgrade plasmoid/com.example.hello
```

Right click the panel → **Add Widgets** → search for the name. The layout is
remembered in `plasma-org.kde.plasma.desktop-appletsrc`, so the widget keeps its
place across shell restarts.

### 6. Reload the shell — the part that wastes an afternoon

Plasma compiles an applet's QML once and holds it. After editing the QML and
reinstalling, **nothing on screen changes**, and there is no per-applet reload to
ask for. Logging out does the same thing more slowly, so do not.

```bash
./scripts/reload-plasmoid.sh plasmoid/com.example.hello   # install, then restart the shell
```

What that script exists for: the obvious command is a silent no-op in one of the
two states a session can be in.

| State | How to tell | What works |
|-------|-------------|------------|
| The user unit owns the shell | `systemctl --user is-active plasma-plasmashell.service` | `systemctl --user restart plasma-plasmashell.service` |
| It does not (shell started by `kstart`, e.g. after a manual reload) | the same command reports `inactive` | quit it, wait, then `kstart plasmashell` |

In the second state, `systemctl --user restart` reports **success** and logs
`Started KDE Plasma Workspace`, and the running shell is untouched: the process
the unit spawns exits immediately because `org.kde.plasmashell` is already owned,
the exit status is 0 so `Restart=on-failure` never retries, and the unit ends up
inactive. Measured four times in the journal and once by hand.

So never trust the command's exit status. Record the PID before, look for a new
one after, and only then believe anything happened. When quitting the shell by
hand, wait until the old process is really gone before starting the new one —
`kstart` into a shell that is still shutting down is the one way this comes back
to an empty panel.

### 7. Calling a command and reading its answer

The data engine is `org.kde.plasma.plasma5support` even on Plasma 6:

```qml
import org.kde.plasma.plasma5support as P5Support

P5Support.DataSource {
    id: executable
    engine: "executable"
    connectedSources: []

    onNewData: (source, data) => {
        disconnectSource(source);
        // data["stdout"], data["stderr"], data["exit code"], data["exit status"]
    }
}
```

- The four keys are `stdout`, `stderr`, `exit code` and `exit status`. Verify
  them with a probe printing `Object.keys(data)` rather than guessing `status` or
  `output` — a guess produces a widget that silently displays nothing.
- **Remember the path, do not recover it.** `onNewData` reports the source string
  it was handed and nothing alongside it, so the tempting move is to parse the
  path back out of the command line. Shell quoting escapes an apostrophe as
  `'\''`, so a file called `it's.pdf` is read back as `s.pdf`. Keep a map from
  command string to path instead.
- Quote paths for the shell; the naive `'` + path + `'` breaks on a path that
  contains one.
- The process runs with Plasma's environment and working directory, not the
  shell's. Use absolute paths, and make sure the command is on the PATH Plasma
  sees (`~/.local/bin` is; a user unit started by D-Bus activation runs in
  `$HOME`).
- `connectSource(command)` re-runs the command if the same string is passed
  twice, which is the desired behaviour for a re-read but means a command has to
  be distinguishable from the previous one.

### 8. An absent JSON key is `undefined`, not empty

Only some replies carry an optional key. Binding it straight to a `text`
property produces `Unable to assign [undefined] to QString` on every item and
shows something arbitrary. Fold the optional fields into one string with a
default first, in the `.js` module, and bind that.

### 9. Verify before saying it works

- `plasmawindowed` prints QML warnings and errors on stderr. Read them; a QML
  warning is usually a property that silently stays at its default.
- **The settings dialog is not covered by `plasmawindowed`.** Open it once by
  hand on the installed widget, or the whole page can be missing and every test
  still passes. This is the failure the structural check exists for.
- `qmllint` catches syntax and some typing; it does not know about Plasma's
  conventions, so a clean lint is not a working widget.
- A missing icon **name** in QML draws an empty square with no error at all. If
  the icon is a file in the package, use `Qt.resolvedUrl("../icons/x.svg")`; if
  it is a theme icon, the name has to exist in the theme.
- Check the journal rather than guessing:

```bash
journalctl --user -b --no-pager | grep -i -e plasmashell -e plasmoid -e qml
```

## Checklist

- [ ] `KPlugin.Id` matches the package directory name.
- [ ] `plasmawindowed <id>` shows the widget with no QML warnings on stderr.
- [ ] `./scripts/check-package.sh` passes: config page wired to every entry, both directions.
- [ ] The settings dialog opens on the installed widget and shows the page.
- [ ] A drop works on the panel icon, not only in `plasmawindowed`.
- [ ] A reload actually replaced the shell (PIDs before and after differ).
- [ ] The command the widget runs is absolute-path safe and on Plasma's PATH.

## More

- `./references/pitfalls.md` — the full catalogue: symptom, cause, fix.
- `./assets/` — `metadata.json`, `config.qml`, `ConfigPage.qml` templates.
- `./scripts/reload-plasmoid.sh` — install and restart the shell, safely.
- `./scripts/check-package.cmake` — the structural config check, as used by CTest.
- In this repository: `plasmoid/com.github.invoicedrop/` is a working widget,
  `tests/tst_plasmoid.qml` the logic harness, `tests/check_plasmoid_config.cmake`
  the same structural check wired into CTest.
- Upstream, for the parts this skill does not cover: KDE's
  [widget tutorial](https://develop.kde.org/docs/plasma/widget/) — *Setup* for the
  package layout, *Testing* for `plasmawindowed`, *Configuration* for KConfigXT,
  *Widget Properties* for `metadata.json` and the panel properties, and
  *Porting Plasmoids to KF6* when adopting a Plasma 5 widget.
