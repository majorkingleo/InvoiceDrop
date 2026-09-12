# Pitfalls, with the symptom that points at them

Every entry was hit while building the InvoiceDrop widget on Plasma 6.7.5 / Qt
6.11 (Wayland), September 2026. The symptom is the part worth remembering: the
cause is usually obvious once you know which symptom to look for.

## 1. The settings dialog has no page, silently

**Symptom:** the dialog opens on the keyboard-shortcuts tab with an *About* tab
next to it and nothing else. No warning in the shell's log, nothing in
`plasmawindowed`.

**Cause:** `contents/config/config.qml` is a plain `Item` or `Rectangle` instead
of a `ConfigModel`. Plasma reads the dialog's pages from the model and shows
nothing for anything else.

**Fix:** make it a `ConfigModel` with one `ConfigCategory` per page, and open the
dialog by hand once — `plasmawindowed` does not load the config page, so no
amount of widget iteration catches this.

## 2. A `cfg_` alias that matches nothing

**Symptom:** the page opens, the control is there, and changing it writes nothing.
Or: a setting exists that cannot be reached from any page.

**Cause:** the alias name does not match the `entry name` in `main.xml`, in one
direction or the other. Plasma binds `cfg_<entry>` by convention and reports
neither an extra alias nor a missing one.

**Fix:** check both directions mechanically: `./scripts/check-package.cmake`, wired
into CTest as the `plasmoid-config` test.

## 3. `ConfigCategory.source` resolved against the wrong folder

**Symptom:** the dialog has a category, and the page under it is empty or the
whole dialog breaks.

**Cause:** `source: "config/ConfigPage.qml"` is resolved against
`contents/ui/`, not against `contents/config/` where `config.qml` lives.

**Fix:** keep pages under `contents/ui/config/` and write the `source` relative
to `contents/ui/`.

## 4. A changed `main.qml` that changes nothing on screen

**Symptom:** the edit is on disk, the package reinstalled, the widget identical.

**Cause:** Plasma compiled the QML once and holds it. There is no per-applet
reload, and reinstalling does not invalidate the running copy.

**Fix:** restart the shell (see the next two entries). `plasmawindowed` is the
loop that does not need this, because it reads the package on every start.

## 5. `systemctl --user restart plasma-plasmashell.service` reports success and does nothing

**Symptom:** exit status 0, `Started KDE Plasma Workspace` in the journal, and
the old shell still running — the changed QML is not picked up. Repeated, so the
reload looks flaky rather than broken.

**Cause:** the shell was not started by the unit (a manual reload leaves it
started by `kstart`). The unit is still *defined*, so `restart` is accepted; the
process it spawns exits at once because `org.kde.plasmashell` is already owned,
the exit status is 0, `Restart=on-failure` never retries, and the unit ends up
inactive.

**Fix:** treat the unit as one of two paths, and check the outcome instead of the
exit status.

```bash
systemctl --user is-active --quiet plasma-plasmashell.service \
    && systemctl --user restart plasma-plasmashell.service
```

Then compare PIDs before and after. If the unit does not own the shell, replace it
by hand: `kquitapp6 plasmashell`, wait until the process is gone, `kstart
plasmashell`. `./scripts/reload-plasmoid.sh` does all of this and prints both PIDs,
which is the only evidence that anything happened.

## 6. Starting the new shell before the old one has exited

**Symptom:** the panel comes back empty, or the widget is gone from it.

**Cause:** `kstart plasmashell` while the previous shell is still shutting down.

**Fix:** poll for the old process to disappear — up to around ten seconds — and
only then start the new one. Never start a second shell "to be sure".

## 7. The widget shows nothing after a drop

**Symptom:** the command runs, the reply arrives, and no item appears.

**Cause (a):** the path was recovered by parsing the command string. The data
engine reports only the source string, so a file whose name contains an
apostrophe comes back mangled (`it's.pdf` → `s.pdf`), and the lookup misses.

**Cause (b):** the wrong data engine keys. They are `stdout`, `stderr`,
`exit code`, `exit status` — not `status`, not `output`.

**Fix:** keep a map from command to path, and print `Object.keys(data)` once from
a probe rather than guessing key names.

## 8. `Unable to assign [undefined] to QString`

**Symptom:** a QML warning per item, and a field that shows stale or odd text.

**Cause:** an optional JSON key, bound straight to a `text` property. An absent
key is `undefined`, not `""`.

**Fix:** fold optional fields into one string with defaults in the `.js` module,
and bind the folded value. Keep every decision there so it can be tested.

## 9. An empty square where the icon should be

**Symptom:** the panel shows a blank cell. No error, no warning.

**Cause:** either a missing icon *name* in QML, or a package-local SVG referenced
as if it were a theme icon.

**Fix:** `KPlugin.Icon` in `metadata.json` must be a theme icon name; a file in
the package is `Qt.resolvedUrl("../icons/x.svg")`. The two are deliberately
different, and neither resolves the other.

## 10. The dialog or the popup is cut off

**Symptom:** the popup extends off the screen or is clipped by the panel.

**Cause:** the representation was sized in raw pixels.

**Fix:** size the full representation in `Kirigami.Units.gridUnit`, and set
`preferredRepresentation: fullRepresentation` so a click opens a popup instead of
expanding inside the panel.

## 11. A drop works in `plasmawindowed` and not in the panel

**Symptom:** the widget works in its own window and ignores drops once it is in
the panel.

**Cause:** the compact representation has no `DropArea`. In the panel, the small
icon is the only thing on screen.

**Fix:** a `DropArea` in `compactRepresentation` *and* one in the popup.

## 12. QML logic cannot be tested

**Symptom:** nothing to run: the decisions live inside `PlasmoidItem`, which
needs a running shell.

**Cause:** command building and reply parsing written inline in `main.qml`.

**Fix:** move them to a `.js` module and drive it from an `Item` harness under
`qmlscene6 -platform offscreen`. Two behaviours to design around, both measured:

- `qmlscene6` ignores `Qt.exit()`. End the harness with `Qt.callLater(Qt.quit)`,
  which does exit (exit code 0), and let CTest decide from the output with
  `PASS_REGULAR_EXPRESSION` / `FAIL_REGULAR_EXPRESSION` rather than from a QML
  error, which does not fail the process on its own. An ad-hoc probe belongs
  under `timeout` regardless: a QML error before the quit never exits.
- `i18n()` is `undefined` in this runner unless the file imports
  `org.kde.kirigami`. Keep translation out of the module under test and pass a
  `Qt.locale(...)` or a strings object in as a parameter, so the harness can pin
  the locale and assert on exact text.

The newer `qml` runner is stricter and rejects relative JavaScript imports that
`qmlscene6` accepts, so do not switch runners and assume the harness still works.

## 13. The command works in a terminal and fails from the widget

**Symptom:** `file does not exist` for a path that plainly exists.

**Cause:** the process Plasma starts has a different working directory (often
`$HOME`) and a different environment from the login shell. A relative path means
nothing there, and a command in `~/.local/bin` may not be on that PATH.

**Fix:** resolve paths to absolute in the widget before handing them over, and
configure the command's location explicitly instead of relying on PATH. Make it a
setting (`cfg_command`) so a wrong guess is fixable without reinstalling.

## 14. A signal handler that uses an injected parameter warns on every load

**Symptom:** this line in the shell's journal, at every shell start, with no
visible effect on the widget:

```
main.qml:44:5 Parameter "expanded" is not declared. Injection of parameters into
signal handlers is deprecated. Use JavaScript functions with formal parameters instead.
```

**Cause:** the handler refers to the signal's parameter by its implicit name —
`onExpandedChanged: { if (expanded) … }` — instead of declaring it. Qt 6 still
injects it, and warns.

**Fix:** declare the parameter:

```qml
onExpandedChanged: function (expanded) {
    if (expanded)
        refreshFromStore();
}
```

Then read the log rather than assuming a clean load, and **filter on the current
shell's PID**:

```bash
pid=$(pgrep -x plasmashell)
journalctl --user -b _PID=$pid --no-pager | grep -i qml
```

Without the PID filter, a warning from a shell that was replaced an hour ago is
still in the journal and looks like a current one — which is exactly how this was
first misread as unfixed.

## 15. A reply from one command is mistaken for another

**Symptom:** an empty card appears in a list that should not have grown, or a
value that is plainly a number is shown as a blank field.

**Cause:** two commands run through the same `executable` data source, and the
reply is parsed by the wrong branch. JSON is forgiving: `0` and `"hello"` parse
successfully, so a strict "did it parse?" check accepts them and a
`JSON.parse()` result that is not an object becomes a record with no fields.

**Fix:** route replies on the exact command string that was connected (build both
sides from one function so they cannot drift), and accept only objects as
records — count anything else as a broken line rather than as data. Both are worth
a test in the harness.

