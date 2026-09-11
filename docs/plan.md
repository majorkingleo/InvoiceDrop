# Implementation plan

## Scope

Build order is **backend first, GUI last**. The interface for the first phases is
a command line tool, not a widget.

```
$ InvoiceDrop bill1.pdf
bill1.pdf  Muster GmbH  2026-02-14  1190,00 EUR
```

Same binary in every mode: one-shot analysis, later the background daemon, and
the DBus client that the plasmoid talks to. No second executable.

Non-goals until phase 4: no daemon, no DBus, no tray icon, no notification, no
file watcher. Non-goals until phase 5: no QML at all.

## Phases

| Phase | Deliverable | Interface | Status |
|-------|-------------|-----------|--------|
| 0 | Skeleton that compiles, prints version | `invoicedrop --version` | **done** |
| 1 | Extraction only, no AI | `invoicedrop --extract-only f.pdf` | **done** |
| 2 | **MVP**: shop, date, sum from a real invoice | `InvoiceDrop bill1.pdf` | **done** |
| 3 | SQLite store, hash cache, history | `invoicedrop history` | **done** |
| 4 | Daemon: inbox watch, DBus, notifications | `invoicedrop daemon` | **done** |
| 5 | Plasma widget | drag and drop in the panel | **done** |
| 6 | Packaging, PKGBUILD, doctor | `pacman -U` | **done** |
| 7 | Sum per file, in the CLI, the widget and the toast | `invoicedrop bills/*.pdf` | **done** |

Phase 2 is the first point where the tool is useful. Everything after that is
ergonomics, automation and presentation.

---

## Phase 0 — Skeleton that builds

**Goal:** an empty binary that compiles and links Qt and KF, so the toolchain is
proven before any real code exists.

Files:

```
CMakeLists.txt
src/CMakeLists.txt
src/main.cpp
src/version.h.in
.gitignore
.clang-format
```

Steps:

1. Top-level `CMakeLists.txt`: `project(InvoiceDrop VERSION 0.1.0 LANGUAGES CXX)`,
   `CMAKE_CXX_STANDARD 20`, `find_package(ECM REQUIRED NO_MODULE)`, then
   `find_package(Qt6 REQUIRED COMPONENTS Core)`. Start with **Qt Core only**; add
   Network, Sql and DBus in the phases that need them.
2. `include(KDEInstallDirs)`, `include(ECMQtDeclareLoggingCategory)`,
   `feature_summary(WHAT ALL)` so missing optional parts are visible at configure
   time rather than link time.
3. `src/CMakeLists.txt`: `qt_add_executable(invoicedrop main.cpp)`,
   `target_link_libraries(invoicedrop PRIVATE Qt6::Core)`, then
   `install(TARGETS invoicedrop RUNTIME DESTINATION ${KDE_INSTALL_BINDIR})`.
4. `main.cpp`: `QCoreApplication`, `QCommandLineParser`, `--version`, and a
   placeholder that prints the usage line.
5. Install a `InvoiceDrop` symlink next to `invoicedrop` in the install rule, so
   both spellings work on a case-sensitive filesystem.

Verify:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/src/invoicedrop --version
```

Done when the binary prints `InvoiceDrop 0.1.0` and `cmake --build` needs no
compiler flag fixes.

---

## Phase 1 — Extraction, no AI

**Goal:** prove the document side on its own. This is where the real risk is, and
it is testable without a GPU, without Ollama and without a prompt.

Files:

```
src/extract/documentreader.{h,cpp}   Document, PageImage, ReadOptions, routing
src/extract/pdfreader.{h,cpp}        MuPDF: open, page count, text, raster
src/extract/imagereader.{h,cpp}      Leptonica pixRead, orientation, downscale
src/extract/ocr.{h,cpp}              TessBaseAPI per thread
src/extract/imageops.{h,cpp}         long-edge scale, JPEG encode, base64
src/extract/threadctx.{h,cpp}        thread-local fz_context and TessBaseAPI
```

Steps:

1. Define the value types first, because everything else is written against them:
   `PageImage { int index; QByteArray jpeg; int width, height; }` and
   `Document { QString path; Kind kind; bool hasTextLayer; QString text;
   QVector<PageImage> pages; QStringList notes; }`.
2. `pdfreader`: create an `fz_context` per thread, `fz_open_document`, count
   pages, `fz_new_stext_page_from_page` and walk the text blocks. Set
   `hasTextLayer` when the average is at least 120 characters per page.
3. `pdfreader` raster path: `fz_new_pixmap_from_page_number` at the requested
   dpi, transform for rotation, then `fz_write_pixmap_as_jpeg` into an
   `fz_buffer`, copied out into `QByteArray`.
4. `imagereader`: `pixRead` handles JPEG, PNG, TIFF, BMP and WebP once Leptonica
   has the codecs. Read EXIF orientation, rotate with `pixRotate90` as needed,
   downscale with `pixScaleToSize` so the long edge hits the limit.
5. `ocr`: `TessBaseAPI::Init` with the tessdata prefix and the requested
   languages, one instance per thread, `SetImage` then `GetUTF8Text`.
6. `imageops`: the single place that turns a pix into a downscaled JPEG and then
   base64. Keep the base64 encoder here too, so payload size is measurable in one
   spot.
7. `DocumentReader::read()` implements the routing table from the architecture
   doc and appends a human-readable reason to `notes` on every branch.
8. `cli.cpp`: add `--extract-only`, which prints the extracted text and the notes
   and never contacts the model.

Also add reusable test binaries? Keep it simple: one test executable
`tst_documentreader` that walks every file in `tests/testdata/` through `read()`
and checks that each one yields text or page images.

Verify:

```fish
invoicedrop --extract-only tests/testdata/*.pdf     # text layer and scan paths
invoicedrop --extract-only tests/testdata/*.jpg     # pixRead, EXIF, OCR
invoicedrop --extract-only --pages 2 --dpi 150 tests/testdata/*.pdf
```

Done when every file in `tests/testdata/` yields text or page images and the
timing per page is recorded in the notes output. **If OCR quality on a scan is
bad, fix it here** — do not carry it into phase 2 and blame the model.

### What phase 1 actually found

All twelve documents in `tests/testdata/` are read, the suite runs in 0.37 s and
nothing is skipped. Two real defects surfaced, both of which would have been
blamed on the model in phase 2:

1. **The page was rendered with the zoom applied twice.** `fz_new_draw_device`
   already carries the page-to-pixmap matrix, and `fz_run_page` was handed the
   same matrix again. Every rasterised page was magnified about 2.4x and clipped
   at the right and bottom edge, so OCR and the model saw roughly the left 40 %
   of each receipt. Passing `fz_identity` to `fz_run_page` fixed it. This was
   caught by rendering the same page with `pdftoppm` and comparing.
2. **Narrow pages were rendered and then downscaled into illegibility.** A
   receipt is 82 mm wide, so at 200 dpi it is 440 px across, and the long-edge
   limit then kept it there. `ReadOptions::minShortEdge` now raises the render
   resolution until the short edge reaches 1000 px, and the downscale refuses to
   go below it. The same floor protects the EXIF-rotated photos.

Measured on the real documents:

| Route | Files | Result |
|-------|-------|--------|
| PDF with text layer | 4 | 1139–1726 characters, 0–3 ms, nothing rasterised |
| PDF without text layer | 6 | 1–2 pages rasterised, 31–65 ms |
| JPEG photo | 2 | 1 page image, 4–5 ms |

**Tesseract lost, and that is now the default.** On every photographed receipt
it produced confident nonsense: 4 characters from one, a page of noise from
another, and 7.5 s spent magnifying a 174 px thumbnail into more noise. The
vision model read the same 174 px image correctly, including the total. Since
wrong text is worse than no text — it reaches the model looking like evidence —
OCR is off by default and `--ocr` turns it on for the clean flatbed scans where
it does pay off. Raw page images remain the primary input for the model.

One data fact to carry into phase 2: the `GuSp_*` files are **collections**, not
single invoices — 21 pages for `Lebensmittel`, 3 for `Tanken`, 2 for most
others. A four-page default cap silently truncates them, and one dropped file can
contain several invoices. Phase 2 needs an answer for that before the widget
takes a single drop.

---

## Phase 2 — Ollama client and the MVP

**Goal:** `InvoiceDrop bill1.pdf` prints shop, date and sum. This is the point
where the tool replaces manual reading.

Files:

```
src/invoice-schema.h        the JSON schema, as one string constant
src/invoice.{h,cpp}         fromJson with tolerant normalisation, toJson
src/ollama.{h,cpp}          QNetworkAccessManager, POST /api/chat
src/cli.{h,cpp}             options, output formatting, exit codes
```

Steps:

1. `invoice-schema.h`: the schema for `vendor`, `invoice_number`, `date`,
   `due_date`, `currency`, `net_total`, `tax_total`, `gross_total`, `iban`,
   `line_items`, `confidence`. Required: `vendor`, `gross_total`, `currency`.
2. `invoice.cpp`: normalise on the way in, because models are sloppy. `1190,00`,
   `1.190,00`, `1190.00`, `€ 1.190,00` must all land on `1190.0`. Dates to
   `YYYY-MM-DD`. Currency to an upper case ISO code. Strip a markdown fence
   before parsing.
3. `ollama.cpp`: `POST /api/chat` with `stream:false`, `keep_alive:"30m"`,
   `format` set to the schema object, `temperature: 0` and `think: false`, and one
   user message carrying both the extracted text and the page images as base64.
   One reply in flight at a time.
4. Error mapping: `HTTP 404` becomes "model not pulled, run `ollama pull <model>`";
   `ConnectionRefused` becomes "ollama not running, start it with
   `systemctl --user start ollama`". Both exit 2 with the message on stderr.
5. Validation: parse the reply, check the required fields, and on a parse failure
   retry exactly once with the parse error appended to the prompt.
6. `cli.cpp`: the human output is one line per file, `file  shop  date  sum`.
   `--json` prints the full record. Exit 0 when every file succeeded, 2 otherwise.

Verify against real documents, not fixtures:

```bash
InvoiceDrop ~/Rechnungen/*.pdf
InvoiceDrop bill1.pdf --json | jq '.vendor, .gross_total'
```

Done when five real invoices — at least one digital PDF, one scan, one photo —
come back with the correct shop, date and sum, and the wall clock time per
invoice is recorded. Compare against the text-layer fast path: if the digital PDF
is not clearly faster, the routing in phase 1 is wrong.

### What phase 2 actually found

All twelve documents in `tests/testdata/` are read end to end. The reference run
with `gemma4:latest` takes 27.8 s for the set, about 2 s per document.

1. **`think: false` is worth 10x.** With a reasoning trace enabled, one receipt
   took 10.2 s. With it disabled, 1.0 s, same answer, byte for byte. Reading an
   invoice is not a task that benefits from deliberation, so the client sends
   `think: false` and `--think` turns it back on.
2. **The text layer came out in drawing order, not reading order.**
   `fz_print_stext_page_as_text` emits blocks as they appear in the content
   stream, so on a SAP invoice every label of a two-column form arrived before
   every value: `Datum:` and `20250430` were a page apart in the output and no
   date was ever reported. Lines are now grouped by their vertical position and
   sorted by x, so a label sits next to its value. The same change made the IBAN
   and the payment terms readable.
3. **The model drops the date at random.** Same text, same prompt,
   `temperature: 0`, and the model returns the issue date in some runs and omits
   it in the next. Measured across repeated runs, not guessed. A date is a
   regular expression rather than a judgement call, so when the model stays
   silent the labelled date is read out of the text directly. Three consecutive
   runs of the ÖBB invoice now all report `2025-04-30`.
4. **`minicpm-v:8b` is not a candidate.** Same twelve documents, 175.4 s against
   27.8 s, one hard failure where the JSON came back unterminated after 118 s,
   and two invented totals. `gemma4:latest` reads the photographed Lagerhaus
   receipt correctly down to the cent, including `18,48 EUR` and `2025-07-17`,
   where `minicpm-v` returned nothing at all.
5. **A 174 px scan makes every model lie.** `Rechnung 1 - gesamt.jpg` is 174 px
   wide, roughly 3.6 px per character. Both models returned a confident vendor, a
   date and a total, and both were wrong; one of them invented USD, which is not
   on the paper. The CLI now flags any result whose raster is below 400 px on the
   short edge with `quality_warning`, on stderr and in the JSON, instead of
   presenting the numbers as fact.

Known limits carried forward: the vendor name is sometimes a person rather than
a shop, casing follows the paper (`hofer` stays `hofer`), and a document holding
twenty-one receipts still yields one record for the first page only.

---

## Phase 3 — Storage and cache

**Goal:** no document is analysed twice, and there is a queryable history.

Files: `src/store.{h,cpp}`, additions to `src/cli.cpp`.

Steps:

1. `QSqlDatabase` with the `QSQLITE` driver, one connection per thread, schema
   created on open (idempotent `CREATE TABLE IF NOT EXISTS`).
2. Tables: `invoices` keyed on the SHA-256 of the file, plus `line_items` with a
   foreign key. Columns mirror the schema, `payload` holds the raw JSON.
3. Cache check before extraction, not after: hashing is cheap, rasterising is not.
4. Add the `history` subcommand and `--no-cache`.
5. Archive processed originals to `~/.local/share/invoicedrop/archive/` behind a
   `--move` flag.

Verify: the second run of the same file reports `cached`, `invoicedrop history`
lists both entries, and deleting the row makes the tool re-analyse.

### What phase 3 actually found

Phase 2 changed the unit of output from the file to the page, because one PDF can
hold twenty one bills. The store follows that: bills are keyed on
`(sha256, page)`, so a collection document is cached per bill and re-reading it
costs nothing.

1. **A hash alone is not a cache key.** The same file read with a different model,
a different page limit or a different dpi is a different question. A
   `fingerprint` of the settings that change the answer is stored alongside the
   hash, and a cache hit needs both. Without it, switching `--model` would serve
   the old model's answers from disk and look like a regression.
2. **`QStandardPaths::AppLocalDataLocation` appends the organisation and the
   application name.** With both set to `InvoiceDrop` the database landed in
   `~/.local/share/InvoiceDrop/InvoiceDrop/`, and `--move` reported the same
   doubled path. Everything now goes through `Paths::dataDir()`, which builds
   `~/.local/share/invoicedrop` explicitly, matching what the documents describe.
3. **The cache is checked before the file is opened.** Hashing is milliseconds,
   rasterising and inferring are seconds. A cache hit on the two page receipt
   drops from 4 s to 0 ms of extraction and 0 ms of inference.

Line items were dropped from the schema in phase 2 and are still absent. Nothing
reads them, and they multiply the output tokens of every request.

---

## Phase 4 — Background service

**Goal:** the inbox folder becomes the interface, and the CLI stops paying the
model load cost.

Files: `application`, `inboxwatcher`, `dbus-adaptor`, `notifier`, `settings`,
`org.kde.invoicedrop.xml`, the systemd unit, and the `daemon` subcommand.

Steps:

1. `QFileSystemWatcher` on the inbox, plus a settle delay so a file still being
   copied is not read half-written.
2. `QDBusConnection::sessionBus()` with `QDBusConnection::ExportAdaptors`, the
   adaptor generated from the interface XML by `qt6_add_dbus_adaptor`.
3. `KNotification` on success and on failure.
4. The CLI checks for the daemon first: if `org.kde.invoicedrop` is on the bus it
   sends `Analyze` and waits for the reply, otherwise it runs the pipeline
   in-process as in phase 2. Behaviour and output stay identical, which is why
   this is a config change and not a rewrite.
5. D-Bus activation: `SystemdService=invoicedrop.service` in the service file, so
   the first CLI call starts the daemon without the user enabling anything.

Verify: drop a file into `~/.local/share/invoicedrop/inbox/`, get a notification,
confirm with `systemctl --user status invoicedrop.service` — then confirm the CLI
result did not change.

### What phase 4 actually found

**No KDE Frameworks were added.** The plan called for `KNotification`,
`KDBusAddons` and a generated adaptor. None of that was needed: sending a
notification is one call to `org.freedesktop.Notifications`, which Plasma, GNOME
and every other desktop implements, and the adaptor is a `QDBusAbstractAdaptor`
with an inline introspection string. The build stays on Qt alone, and no package
was added to the machine. The systemd unit and the D-Bus service file are still
installed, so activation works the same.

1. **The blocking HTTP wait pumps the event loop.** `OllamaClient` waits for the
   reply with a local `QEventLoop`, and while it runs the inbox timer keeps
   firing. The watcher settled the same file during the first read and handed it
   over a second time, so one dropped document was analysed twice. Two fixes:
   the watcher now keeps a list of what it already reported, keyed on size and
   modification time, and the daemon refuses re-entrant work and queues it
   instead.
2. **A folder scan has no memory.** The first fix was not enough on its own,
   because `scan()` re-queued every file it saw on every tick. Without the
   handled list a rescan would re-report an untouched file forever.
3. **A file named `.pdf` has a suffix.** `QFileInfo::suffix()` returns `pdf` for
   it, so the watcher would have processed a hidden file. Editors and download
   tools write to a dot file and rename it into place, so hidden names are now
   skipped outright. Caught by a test that expected a rejection.
4. **Delegation must not change the output.** The daemon answers in JSON because
   that is the wire format, and the first implementation printed that JSON even
   when the caller had asked for the human line. Whether a daemon happened to be
   running would have changed what the shell sees. The reply is now reformatted
   into whichever shape the run asked for, and the exit code follows the bills.

Measured on the two page Bertahütte receipt: a delegated call that the daemon had
already cached returns in 14 ms, against 3.7 s for the same document read
locally, because the daemon keeps the model resident.

---

## Phase 5 — Plasma widget

Only now does QML appear. The widget is a drop target and a result list; it owns
no logic.

Files: `plasmoid/com.github.invoicedrop/` as described in the architecture doc.

Steps:

1. `metadata.json` with `KPackageStructure: Plasma/Applet` and
   `X-Plasma-API-Minimum-Version: 6.0`.
2. `main.qml` with a `DropArea`, mapping `drop.urls` to local paths.
3. Call `invoicedrop --json <path>` through the `executable` data engine and
   parse the JSON in QML. This is the same interface the shell uses, so the
   widget cannot drift from the CLI.
4. `InvoiceCard.qml` for one result row; settings page mirroring the KConfig keys.
5. Iterate with `plasmawindowed com.github.invoicedrop` before installing into the
   panel.

Verify: drag a JPEG onto the widget, a card appears with shop, date and sum, and
dragging the same file again is instant because of the cache.

### What phase 5 actually found

The widget is four QML files and no C++. `main.qml` is a drop target and a list,
`InvoiceCard.qml` is one row, `config.qml` mirrors the config keys, and
`invoicelogic.js` holds everything with a decision in it, so it can be tested on
its own. `tests/tst_plasmoid.qml` runs it under `qmlscene6` as a CTest entry.

1. **Recovering the path by parsing the command back is wrong.** The data engine
   reports the source string it was handed and nothing else, so the first attempt
   read the path back out of the command line. A path containing an apostrophe is
   escaped as `'\''`, and the regex then matched the last quoted fragment: an
   invoice in `/tmp/it's.pdf` was reported as `s.pdf`. The path is remembered in a
   map instead, keyed on the command.
2. **An absent JSON key is `undefined`, not empty.** Binding
   `bill.quality_warning` straight to a text property produced `Unable to assign
   [undefined] to QString` on every card. Only some bills carry that key, so the
   optional fields are folded into one string with a default first.
3. **The engine's data keys were verified, not assumed.** A probe printed
   `["exit code", "exit status", "stderr", "stdout"]`. Guessing `status` or
   `output` would have produced a widget that silently showed nothing.
4. **`Qt.exit()` is ignored by `qmlscene6`,** and the newer `qml` runner rejects
   relative JavaScript imports that `qmlscene6` accepts. The test therefore quits
   with `Qt.quit()` and CTest decides from the output with
   `PASS_REGULAR_EXPRESSION` and `FAIL_REGULAR_EXPRESSION`.
5. **The first CLI call now starts the daemon through D-Bus activation.** Without
   it the widget would pay a model load on every drop, which is exactly the cost
   the daemon exists to remove. With it, the first drop pays it once and the rest
   are milliseconds.

The one thing not automated is the drag itself. The command a drop builds, the
reply it parses and the labels it renders are all checked; the gesture is not.
Verified by hand instead with `plasmawindowed com.github.invoicedrop`, which
loads the package with no QML warnings.

**A changed widget needs the shell restarted, not the session.** Plasma compiles
an applet's QML once and holds it, so editing `main.qml` and reinstalling changes
nothing on screen, and there is no per-applet reload to ask for. On Plasma 6 the
shell is owned by a user unit, so

```bash
systemctl --user restart plasma-plasmashell.service
```

is the whole operation: about a second, panels recreated, running programs
untouched, and the widget keeps its place because the layout lives in
`plasma-org.kde.plasma.desktop-appletsrc`. The `Plasma: reload widget` task wraps
this and installs the tree first. `plasmawindowed` remains the faster loop while
working on the QML itself, since it rereads the package on every start, but it
does not exercise the panel's own loading path.

The widget needs `invoicedrop` on the PATH that Plasma sees. Either spelling
works: the package puts it in `/usr/local/bin`, and the local install task puts it
in `~/.local/bin`, which is on the PATH of a normal login shell.

---

## Phase 6 — Packaging

`PKGBUILD` for CachyOS/Arch, `.desktop` file, icon, install and uninstall scripts,
and the `doctor` subcommand that checks MuPDF, tesseract data, the Ollama
endpoint and the model in one shot. Verify with `makepkg` in a clean chroot.

Still not run: `makepkg` inside a clean chroot. The package has only been built
against the local system, so a missing dependency that happens to be installed
here would go unnoticed.

Delivered:

- `packaging/PKGBUILD`, `packaging/invoicedrop.install`, `data/*.desktop`
- `invoicedrop doctor`, which reports each building block and the command that
  fixes it. Exits 2 when something required is broken, 0 otherwise.
- `invoicedrop.svg` shipped and installed into `hicolor`, plus the plasmoid's own
  copy under `contents/icons/`.
- `LICENSE`, copied from the system's canonical GPL-3.0-or-later text.
- `cmake --install` places 13 files; `makepkg` produces a 179 kB package with 25
  entries.

What phase 6 actually found:

- **`Extract::checkExternalTools()` had no reason to exist.** The plan called for
  a function that runs `pdftoppm --version` and friends. There is nothing to run:
  MuPDF, Leptonica and Tesseract are linked libraries, not subprocesses. The only
  runtime checks that mean anything are "did `fz_new_context` work" and "did
  tesseract load `deu`", and those are answerable directly. `doctor` asks the
  libraries, not the shell.
- **No icon theme ships an `invoice` icon.** The widget's first icon name was the
  generic `invoice`, which resolves to nothing on Breeze and leaves a blank hole
  in the panel with no warning. There is no fallback for a missing icon name in
  QML beyond an empty rectangle. Fixed by shipping an SVG and installing it into
  `hicolor/scalable/apps`. Breeze's `index.theme` carries `Inherits=hicolor`, so
  a name that only exists in hicolor still resolves.
- **A missing LICENSE aborts the whole install, silently skipping what follows.**
  `install(FILES LICENSE ...)` without `OPTIONAL` is a hard error in CMake, and
  the abort happens partway through, after the binary was placed and before the
  plasmoid was. The plasmoid is installed last, so it was the casualty. This is
  easy to miss because the first lines of `cmake --install` output look fine.
- **`desktop-file-validate` rejects two main categories.** `Categories=Office;`
  alone is fine; `Categories=Office;Finance;` is not, because `Finance` is also a
  main category. The freedesktop spec allows exactly one. Fixed to
  `Office;Finance;` after checking which of the two the validator treats as main.
- **`$0` is useless inside a PKGBUILD.** makepkg sources the file rather than
  executing it, so `$0` is `/usr/bin/makepkg` and
  `realpath "$(dirname "$0")/.."` resolves to `/usr`. The first `makepkg` run
  failed with "The source directory /usr does not appear to contain
  CMakeLists.txt". The build now derives its source directory from `$PWD`, with
  `INVOICEDROP_SOURCE` as an override, and the PKGBUILD documents that makepkg
  must be started from inside `packaging/`.
- **A PKGBUILD for an unreleased project has no `source=()`.** There is no tarball
  to download and therefore no checksum to pin, so the package is built from the
  checkout it lives in. That is unusual for a real PKGBUILD and is only correct
  while the project has no release.
- **A prefix under `$HOME` needs the systemd unit in `share`, not `lib`.**
  `GNUInstallDirs` answers `lib` for every prefix, which suits `/usr` and hides
  the unit for `~/.local`: `systemd-analyze --user unit-paths` lists
  `~/.local/share/systemd/user` and not `~/.local/lib/systemd/user`. The file is
  written, `systemctl --user start` answers that the unit does not exist, and
  nothing in the install output hints at why. `src/CMakeLists.txt` now picks the
  directory from the prefix. The D-Bus service file needed no change, because
  `<standard_session_servicedirs/>` already covers `$XDG_DATA_HOME/dbus-1/services`.
- **`cmake --install --prefix` cannot be used on an existing build.** `ExecStart`
  in the systemd unit and `Exec` in the D-Bus service come from
  `CMAKE_INSTALL_FULL_BINDIR`, which `configure_file` resolves at configure time.
  A prefix passed to `--install` moves the files and not the paths written inside
  them, so the unit would name `/usr/local/bin/invoicedrop` while the binary sits
  in `~/.local/bin`. Local testing therefore uses its own `build-local` directory.
- **`install_manifest.txt` does not mention the `InvoiceDrop` symlink.** CMake
  creates it with `install(CODE ...)`, which leaves it untracked. Uninstalling
  from the manifest alone leaves a dangling symlink on the PATH, so the uninstall
  task removes it explicitly.
- **`check()` passed while testing nothing.** Without a `source=()` there is
  nothing for makepkg to unpack, so `$srcdir/build` survived from the previous run
  and CMake reused its cache: adding `-DBUILD_TESTING=ON` changed nothing, and
  `ctest` answered "No tests were found!!!" and exited 0. A green `check()` that
  ran zero tests is worse than no `check()`. The build now removes its build
  directory first, and `qt6-declarative` is a makedepend because without
  `qmlscene6` CMake silently drops the plasmoid suite instead of failing.

---

## Phase 7 — A sum per file

A collection PDF holds twenty receipts and the number anyone wants from it is
one total. Every file that holds more than one bill now gets a total line, and
the widget draws the same total as a row under the last card of a file.

Delivered:

- `src/totals.{h,cpp}` — `totalFor`, `formatTotal`, `formatCoverage`. Used by the
  CLI, by the delegated path and by the notification.
- The widget adds up in `invoicelogic.js` (`subtotalsFor`) and draws it in
  `contents/ui/FileSum.qml`.
- `tests/tst_totals.cpp`, plus new cases in `tst_plasmoid.qml` and
  `tst_daemon.cpp`. Six suites now.
- `Document::pageCount` — the file's own page count, which did not exist before.

What this phase actually found:

- **A total is worth nothing without its coverage.** Every sum is printed with
  the count it covers, and says `2 of 5 bills` when the count is not the whole
  file: cut by `--pages`, cut by the widget's history limit, or one bill that
  could not be read. The rule is that the number and its scope are printed or
  neither is.
- **The extraction layer only knew how many pages it had read.** `Document` was
  capped by `ReadOptions::maxPages`, so a five page file read with `--pages 2`
  looked like a two page file and its total looked complete. `Document::pageCount`
  now carries `fz_count_pages`. Without it this phase would have shipped a
  confident wrong number, which is worse than no sum at all.
- **Bills and file pages had to be separated, and the first version did not.**
  Iterating the file's page count meant `--pages 1` on a three page file produced
  three bills, two of them failures for pages the page limit had deliberately
  skipped. A page limit is a decision, not an error. It was caught within a
  minute of being written, by running the flag.
- **Currencies are not added together, and amounts are added in cents.** A mixed
  file prints `10,00 EUR + 5,00 USD`. Summing doubles gives 0.1 + 0.2 =
  0.30000000000000004 and the display shows the tail.
- **The JSON contract was left alone.** One object per bill per line, no summary
  object. The widget sums in JavaScript from what it received, and the earlier
  lesson about the shape of stdout is what settled it: a second kind of object on
  the wire is a rule every consumer has to remember.
- **`FileSum.qml` could not be tested outside Plasma.** `i18n` and `i18np` are
  injected by the Plasma runtime and do not exist under `qmlscene6`, so the suite
  covers the arithmetic and not the wording. The component was checked by running
  it in `plasmawindowed` in a throwaway plasmoid, which is the only way to reach
  the real runtime.

Decided, and easy to reverse if it turns out to be wrong: **a file with a single
bill gets no total.** The total would repeat the amount already on the line, on
the card and in the toast. The rule is in three places — `bills.size() > 1` in
`printFileSum`, `run.length > 1` in `subtotalsFor`, and `bills.size() > 1` in
`Notifier::bodyFor`.

Found after the phase was called done, by using it:

- **A relative path did not survive the trip to the daemon.** Run from the
  workspace root, `invoicedrop tests/testdata/x.pdf` printed `-  -  -` and then
  `file does not exist`, while `--local` worked and an absolute path worked. The
  CLI handed the argument to the daemon over D-Bus as typed, and the daemon was
  started by the session bus with `$HOME` as its working directory, so
  `tests/testdata/x.pdf` did not exist anywhere the daemon could look. No amount
  of care on the daemon side fixes it: the caller's directory is not part of the
  data. `Paths::resolvePath` now runs in the CLI — after the subcommand check, so
  that `history` is not turned into `<cwd>/history` — and again inside
  `analyseFile`, which is the one entry point the CLI, the daemon and the tests
  share, so a future caller cannot reintroduce it.
- **The first attempt at that fix broke the subcommands.** Resolving before the
  `history`/`daemon`/`doctor` comparison turned the word into an absolute path that
  matched nothing, and `invoicedrop history` stopped working. Caught by running
  the three subcommands, not by reading the diff.
- **`tst_paths` exists because two bugs had already happened there.** The relative
  path above, and the earlier doubled data directory from
  `QStandardPaths::AppLocalDataLocation`. Both are now assertions. The end to end
  case — relative path, daemon on the bus — cannot be a ctest entry without a
  running daemon, so it was verified by hand and the reasoning is recorded here.

---

## Build commands

Packages:

```bash
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules pkgconf \
    qt6-base kf6-kcoreaddons kf6-kconfig kf6-ki18n \
    mupdf tesseract tesseract-data-deu tesseract-data-eng leptonica
```

`kf6-knotifications`, `kf6-kdbusaddons` and `kf6-kconfigwidgets` are added in
phase 4. Ollama: `pacman -Ss ollama` and pick the variant matching the GPU
(`ollama-cuda`, `ollama-rocm`, or plain `ollama` for CPU).

Build:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/src/invoicedrop --version
```

Install, once phase 2 works:

```bash
sudo cmake --install build
```

Build the package (from inside the packaging directory, the PKGBUILD reads `$PWD`):

```bash
cd packaging
makepkg -f          # builds only
makepkg -si         # builds and installs with pacman
```

Install into `$HOME/.local` to exercise the installed layout without root:

```bash
cmake -B build-local -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DBUILD_TESTING=OFF
cmake --build build-local
cmake --install build-local
```

Same thing from VS Code: `Install: local (build + install to ~/.local)`,
`Install: local (verify)`, `Install: local (uninstall)`.

Iteration loop for phases 1 and 2: edit, `cmake --build build`, run against a real
invoice in `~/Rechnungen`. No install step needed, the binary is used directly
from the build tree.

## Testing

* `tst_invoice` — pure logic, no I/O: number, date and currency normalisation,
  markdown fence stripping, garbage input returning empty fields.
* `tst_documentreader` — every file in `tests/testdata/` through `read()`,
  asserting each one yields text or page images and that no step reports an error.
* `tst_store` — the cache key, saving and finding bills, replacing a document,
  forgetting one, and surviving a reopen.
* `tst_daemon` — the inbox watcher's settle delay and its handled list, and the
  notification text. No bus and no model needed.
* `tst_plasmoid.qml` — the widget's command builder and reply parser, run under
  `qmlscene6`.
* `tst_totals` — the per file sums: mixed currencies, a bill that failed inside a
  total that still adds up, a file cut by the page limit, and cent rounding.
* `tst_paths` — reading `paths.cpp`: a relative path becomes absolute, an existing
  one is canonicalised, a missing one is still made absolute so the error can name
  it, and the data directory is not doubled.
* `tst_bills` — an integration suite. It needs Ollama and skips itself without
  one, and it is the only suite that can be red for a model's reasons rather than
  the code's.

Everything except `tst_bills` runs offline in about five seconds.

## CLI contract

The CLI is the stable interface; the widget and the shell both consume it.

| Flag | Effect |
|------|--------|
| `--json` | machine readable output |
| `--model NAME` | override the model |
| `--ollama-url URL` | override the endpoint |
| `--lang de` | language for free text fields |
| `--pages N` | page cap for PDFs |
| `--dpi N` | raster resolution |
| `--extract-only` | print extracted text, skip the model |
| `--no-cache` | re-analyse even if the hash is known |
| `--move` | archive the original after success |
| `--verbose` | extraction notes and timings on stderr. `-v` belongs to `--version` |

Subcommands: `daemon` (phase 4), `history` (phase 3), `doctor` (phase 6).

## Definition of done for the MVP

Phase 2 is done when all of the following hold:

1. `InvoiceDrop bill1.pdf` prints shop, date and sum for a digital PDF, a scanned
   PDF and a photo of an invoice.
2. It works with the network down and no cloud endpoint configured.
3. Errors are actionable: the message names the missing thing and the command
   that fixes it.
4. Exit code 0 on success, 2 on failure, so it composes in shell pipelines.
5. Nothing in the run requires the GUI, a daemon or a running Plasma session.

## Open decisions

* **The `bills` suite is red, on purpose.** It expects `BERTAHÜTTE` and the model
  reads `BERTAHOTTE`, and the year of the Bertahütte receipt is flaky (2022
  against 2025). The paper says `BERTAHÜTTE` and `22/07/2025`, so the expectation
  is right and the model is wrong. A prompt addition asking for umlauts and
  careful year digits was measured and reverted: it did not fix the umlaut, left
  the year wrong and made the vendor field swallow the address block.
* **Multi-invoice documents: resolved as one bill per page.** The store, the test
  and the output all treat a page as a bill. A single invoice spread over two
  pages therefore yields two records, which is the known cost of the rule.
* **Resolved in phase 2: `gemma4:latest` is the default model.** `minicpm-v:8b`
  was measured against it and lost on accuracy, on stability and by a factor of
  six on time. Any model that reports the `vision` capability works; the choice
  is a `--model` flag, not a code change.
* **Resolved in phase 2: reasoning is off.** `think: false` is the default.
* **Resolved in phase 1: the Tesseract pre-pass is off by default.** It reads
  clean scans, but on every photographed receipt in the test set it produced
  confident garbage that would be fed to the model as evidence. `--ocr` opts in.
* Whether to also send page images when the text layer route fired. The ÖBB
  invoice is read correctly from text alone, so the token cost is currently not
  buying anything.
