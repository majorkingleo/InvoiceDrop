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
| 2 | **MVP**: shop, date, sum from a real invoice | `InvoiceDrop bill1.pdf` | to do |
| 3 | SQLite store, hash cache, history | `invoicedrop history` | to do |
| 4 | Daemon: inbox watch, DBus, notifications | `invoicedrop daemon` | to do |
| 5 | Plasma widget | drag and drop in the panel | to do |
| 6 | Packaging, PKGBUILD, doctor | `pacman -U` | to do |

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
   `format` set to the schema object, `temperature:0`, `num_ctx:8192`, and one
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

---

## Phase 6 — Packaging

`PKGBUILD` for CachyOS/Arch, `.desktop` file, icon, install and uninstall scripts,
and the `doctor` subcommand that checks MuPDF, tesseract data, the Ollama
endpoint and the model in one shot. Verify with `makepkg` in a clean chroot.

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

Iteration loop for phases 1 and 2: edit, `cmake --build build`, run against a real
invoice in `~/Rechnungen`. No install step needed, the binary is used directly
from the build tree.

## Testing

* `tst_invoice` — pure logic, no I/O: number, date and currency normalisation,
  markdown fence stripping, garbage input returning empty fields.
* `tst_documentreader` — every file in `tests/testdata/` through `read()`,
  asserting each one yields text or page images and that no step reports an error.
* Manual acceptance for phase 2: a fixed set of real invoices, checked by eye
  once, then kept as a regression list.
* No test ever needs the network. The Ollama client gets an injectable base URL so
  a stub can be pointed at it.

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
| `-v` | verbose logging on stderr |

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

* **Resolved in phase 1: the Tesseract pre-pass is off by default.** It reads
  clean scans, but on every photographed receipt in the test set it produced
  confident garbage that would be fed to the model as evidence. `--ocr` opts in.
  Revisit only with a real flatbed-scan set to measure against.
* Whether to send extracted text *and* images when the text layer route did fire.
  Sending both costs tokens but helps when a number is ambiguous. Measure in
  phase 2, then fix the default.
* **Multi-invoice documents.** A single PDF can hold 21 receipts. Phase 2 must
  decide between per-page extraction, a page cap that reports truncation, or
  several records per file. Currently the cap truncates silently.
* Page cap default of 4. Revisit once the collection files above are handled.
* Model default `qwen2.5vl:7b`. Revisit once real invoices have been through it.
