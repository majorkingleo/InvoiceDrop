# Architecture

## Overview

InvoiceDrop is written in **C++20** on Qt 6 and KDE Frameworks 6. One language,
one build system, one binary. No Python, no Zig, no scripting layer.

Two artefacts, one language:

| Artefact | Role |
|----------|------|
| `plasmoid/com.github.invoicedrop` | QML applet: drop target, result list |
| `invoicedrop` | one C++20 binary, two modes: one-shot CLI and long-running daemon |

The widget never parses documents. It forwards paths; everything else happens
inside the single C++ process.

### Why one process

* MuPDF, Tesseract and Leptonica are C libraries. Linking them into the Qt
  process removes a subprocess, a pipe protocol and a second build toolchain.
* Tesseract language models and MuPDF fonts are expensive to set up. Loaded once
  per thread, never per file.
* One binary to build, install, package and debug. `cmake --build build` is the
  whole build.
* Extraction must not block the event loop, but it does not need a second
  process to achieve that: extraction runs on a `QThreadPool` and the daemon's
  event loop stays responsive to DBus calls.

### Threading rules

* `QThreadPool` with one `AnalyzeJob` (`QRunnable`) per file.
* MuPDF needs an `fz_context` per thread, Tesseract one `TessBaseAPI` per thread.
  Both live in thread-local storage, created on first use in that thread.
* Worker threads never touch `QNetworkAccessManager` or any QObject owned by the
  main thread. Extraction produces a plain value struct; the request is
  marshalled back with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
* Image decoding uses Leptonica `pixRead`, which is already linked for Tesseract.
  That keeps the daemon a `QCoreApplication` with no QtGui and therefore no
  display dependency.

## The widget

QML only, no C++ plugin and no build step. `main.qml` is a `DropArea` and a
`ListView`, `InvoiceCard.qml` is one row, and the settings live in
`contents/config/config.qml`, which is a `ConfigModel` naming its pages, plus
`contents/ui/config/ConfigGeneral.qml`, which mirrors the KConfigXT keys in
`contents/config/main.xml`.

The two halves of that must match in both directions and neither mismatch is
reported: a `ConfigModel` that is missing costs the whole page without a warning,
a `cfg_` alias with no entry behind it is a control that edits nothing, and an
entry with no alias is a setting that cannot be reached. `plasmoid-config` checks
it. The `source` of a `ConfigCategory` is resolved against `contents/ui/`, which
is why the page is not next to the model that names it.

A drop builds one command and hands it to the `executable` data engine:

```
'invoicedrop' --json --model 'gemma4:latest' '/path/to/beleg.pdf'
```

That is the same interface the shell uses, so the widget cannot drift from the
CLI, and because the CLI hands the work to a running daemon by itself, a warm
daemon makes a drop a few milliseconds. The first call of a session activates the
daemon through D-Bus and pays the model load once.

`invoicelogic.js` holds everything with a decision in it — building the command,
parsing the newline separated JSON, remembering which path a command belongs to —
so it can be run under `qmlscene6` by `tests/tst_plasmoid.qml`. The drag itself is
not automatable; the command and the parsing are.

Two details that cost time:

* **An absent JSON key is `undefined`, not empty.** `quality_warning` is only
  present when there is a warning, and binding it straight to a `text` property
  warns and shows nothing sensible. Optional fields are folded into a string with
  a default first.
* **The path is remembered, not recovered.** The data engine reports the source
  string it was handed and nothing alongside it. Reading the path back out of the
  command fails on a path containing an apostrophe, which shell quoting escapes
  as `'\''`.

The engine hands over four keys: `stdout`, `stderr`, `exit code` and
`exit status`.

### The widget does not own the store, so it asks about it

The list on screen is built from drops and lives in the shell, while `--wipe` runs
in another process in another terminal. Nothing connects the two: deleting rows
cannot reach a `property var bills` that a drop filled an hour ago.

The store is the one thing both sides share, so opening the popup runs
`invoicedrop history --count` — a database open and a `COUNT(*)`, with no model and
no daemon behind it — and a `0` empties the list and closes the detail view. The
answer is parsed by `parseStoredCount` in `invoicelogic.js`, which returns `-1` for
anything that is not a bare number: a CLI that is missing or replaced must not look
like an empty store, or a typo in `cliPath` would wipe the list on screen. Only
the count that is exactly `0` clears, and only when there is something to clear.

The check runs when the popup opens, because that is when a stale card becomes
visible. It is not a watcher: a wipe while the popup is already open is seen the
next time it is opened.

## Repository layout

```
CMakeLists.txt                  top level, CMake only

plasmoid/com.github.invoicedrop/          QML only, no C++ plugin
  metadata.json                 KPackageStructure Plasma/Applet
  contents/config/main.xml      KConfigXT schema
  contents/config/config.qml    ConfigModel, names the dialog pages
  contents/ui/config/ConfigGeneral.qml  the one page, cfg_* aliases per entry
  contents/ui/main.qml          PlasmoidItem, DropArea, ListView
  contents/ui/InvoiceCard.qml   one result row
  contents/ui/invoicelogic.js   command building and reply parsing, testable

src/                            C++20 / Qt 6 / KF 6
  CMakeLists.txt
  main.cpp                      QCoreApplication, dispatch daemon mode vs CLI mode
  cli.{h,cpp}                   QCommandLineParser, output, delegation to the daemon
  daemon.{h,cpp}                the service and its DBus adaptor
  inboxwatcher.{h,cpp}          QFileSystemWatcher, settle-on-write, handled list
  notifier.{h,cpp}              freedesktop notifications over DBus
  paths.{h,cpp}                 ~/.local/share/invoicedrop and friends
  analysis.{h,cpp}              the pipeline: one bill per page, cache aware
  json.{h,cpp}                  the JSON shape the CLI and the daemon share
  hash.{h,cpp}                  SHA-256 of a file, chunked
  ollama.{h,cpp}                QNetworkAccessManager, /api/chat, schema format
  invoice.{h,cpp}               fromJson/toJson, number and date normalisation
  invoice-schema.h              JSON schema sent to the model
  store.{h,cpp}                 QtSql QSQLITE, bills keyed on (sha256, page)
  version.h.in
  extract/documentreader.{h,cpp}  document model, routing on file suffix
  extract/pdfreader.{h,cpp}     MuPDF: fz_context, text layer, page raster
  extract/imagereader.{h,cpp}   Leptonica pixRead, orientation, downscale
  extract/ocr.{h,cpp}           TessBaseAPI per thread, language selection
  extract/imageops.{h,cpp}      scale to long edge, JPEG encode, base64
  extract/threadctx.{h,cpp}     thread-local fz_context and TessBaseAPI

tests/
  CMakeLists.txt
  tst_invoice.cpp               QTest: number, date and currency normalisation
  tst_documentreader.cpp        QTest: fixture PDF and JPEG through the extractor
  testdata/                     real invoices: ten PDFs, two JPEG photos

scripts/
  install-deps-cachyos.sh
  build.sh                      cmake configure + build
  install-plasmoid.sh
  dev-run.sh

docs/architecture.md
```

## Extraction layer

No subprocess and no wire format. `DocumentReader::read()` is a blocking, pure
function called from `AnalyzeJob::run()` on the thread pool:

```cpp
struct PageImage {
    int index = 0;
    QByteArray jpeg;      // already downscaled to the long-edge limit
    int width = 0;
    int height = 0;
};

struct Document {
    QString path;
    Kind kind = Kind::Unknown;          // Pdf | Image
    bool hasTextLayer = false;
    QString text;
    QVector<PageImage> pages;           // empty when the text layer sufficed
    QStringList notes;                  // why this route was chosen
};

Document DocumentReader::read(const QString &path, const ReadOptions &opts);
```

`ReadOptions` carries dpi, maxPages, longEdge, languages and a `Want` enum
(`Text`, `Images`, `Both`), so the caller can force the cheap path.

Routing inside `read()`:

| Input | Route |
|-------|-------|
| PDF, text layer ≥ 120 chars/page | text layer, rebuilt into visual rows by y position, no raster |
| PDF, no text layer | MuPDF renders the leading pages, Leptonica downscales, Tesseract OCRs when `--ocr` is set |
| JPEG/PNG/TIFF/WebP/BMP | Leptonica `pixRead`, EXIF orientation, downscale, OCR only with `--ocr` |

The text layer is not emitted in content-stream order. `fz_print_stext_page_as_text`
returns blocks as they were drawn, which on a two-column SAP form puts every label
before every value and leaves `Datum:` a page away from `20250430`. Lines are
grouped by vertical position and sorted by x, so a label stays next to its value.

Two resolution rules protect legibility. A narrow page, such as an 82 mm receipt
roll, is rendered at a higher dpi until its short edge reaches `minShortEdge`,
because 200 dpi leaves it 440 px across. The downscale then refuses to shrink
below that same floor, so the long-edge limit cannot undo the render. On the
other side, `maxPixels` caps a poster-sized sheet.

`Document` is a value type and crosses the thread boundary by copy. There is no
serialization format to version, because there is no pipe.

## Command line interface

The CLI is the primary interface and works with **no daemon running**. Plain
mode, no subcommand needed:

```
$ invoicedrop bill1.pdf
bill1.pdf  Muster GmbH  2026-02-14  1190,00 EUR
```

One line per file, so it composes with `xargs`, `find -exec` and shell loops.
Errors go to stderr and set exit code 2. `--json` prints the full record for
scripting and for the plasmoid.

| Flag | Effect |
|------|--------|
| `--json` | machine readable output |
| `--model NAME` | override the model |
| `--ollama-url URL` | override the endpoint, defaults to `$OLLAMA_HOST` |
| `--timeout SEC` | how long to wait for one answer |
| `--think` | let a reasoning model deliberate first, off because it costs 10x |
| `--lang de` | language the model writes free text fields in |
| `--pages N` | page cap for PDFs |
| `--dpi N` | raster resolution |
| `--extract-only` | print extracted text, skip the model |
| `--ocr` | run Tesseract over the raster, off because it loses on photos |
| `--ocr-lang deu+eng` | Tesseract language packs for `--ocr` |
| `--no-cache` | read the document again instead of using the store |
| `--move` | move the original into the archive once every bill was read |
| `--db PATH` | database file, defaults to `~/.local/share/invoicedrop/invoicedrop.db` |
| `--limit N` | how many bills `history` lists |
| `--count` | with `history`: the stored bill count alone, for a caller that needs a number and not prose |
| `--local` | read here instead of asking a running daemon |
| `--verbose` | extraction notes and timings on stderr |
| `--wipe` | delete every bill, the archive and the inbox; takes no files |

`-v` is not free: `QCommandLineParser` claims it for `--version`, so the verbose
flag is long form only.

Subcommands: `history` lists what is stored, `daemon` watches the inbox. `doctor`
follows in phase 6. The implementation order is in `docs/plan.md`.

## Store

SQLite through `QtSql`, in `~/.local/share/invoicedrop/invoicedrop.db`.

| Table | Key | Holds |
|-------|-----|-------|
| `documents` | `sha256` | path, file name, page count, the settings fingerprint, model, timestamp |
| `bills` | `(sha256, page)` | one row per page: vendor, date, totals, the raw payload |

Bills are keyed on the page because a file is not a bill. Deleting a document
cascades to its bills.

### The cache key is not just the hash

The same file read with another model, another page limit or another dpi is a
different question. `Store::fingerprint()` hashes the settings that change the
answer — model, page limit, dpi, long edge, short edge floor, text layer
threshold, OCR switch and OCR languages — and a cache hit requires the hash *and*
the fingerprint to match. Without it, switching `--model` would serve the previous
model's answers from disk and look like a regression in the new one.

Lookup happens before the document is opened, because hashing a file is
milliseconds while rasterising and inferring are seconds. A document row without
bills is treated as a miss and read again, rather than reported as nothing.

### Starting over

`Store::clear()` empties both tables and vacuums the file, and the schema is left
in place so the handle that emptied it is the one the next read writes through.
VACUUM is not decoration: `DELETE` only marks pages free, so the vendor names and
the amounts stay readable in the file until something reuses them.

`--wipe` is the only command that names no files, which is why it is a flag and
not a subcommand: the store is not the only thing a reader wants gone. The
archive holds the originals `--move` set aside, and for a document that was only
ever dropped once that is the only copy left; the inbox holds what the daemon has
not read yet, and leaving it behind means the data returns on the next daemon
start. Both are emptied, but not removed: the daemon watches those directories,
and a missing inbox is a different problem than an empty one.

`--inbox` is deliberately outside all of this. It names a directory the daemon
happens to watch, which can be any folder the reader keeps documents in, so
`--wipe --inbox ~/Belege` must not delete `~/Belege`.

## Data flow

1. User drops one or more files onto the applet.
2. `main.qml` reads `drop.urls` and runs `invoicedrop --json <path>` through the
   Plasma `executable` data engine. If a daemon is already running the CLI
   delegates over the session bus and waits for the reply; otherwise it runs the
   whole pipeline in its own process. Either way the JSON lands on stdout, so the
   widget does not care which mode answered.
3. The daemon hashes the file (SHA-256) and returns the cached record on a hit.
4. One `AnalyzeJob` per file is queued on the `QThreadPool`.
5. `DocumentReader::read()` picks the route and returns a plain `Document`. No
   subprocess, no pipe, no JSON framing.
6. Back on the main thread the daemon calls Ollama `POST /api/chat`. Text and
   images ride in the same user message, so one vision model serves digital PDFs,
   scans and photos.
7. The reply is validated against the invoice JSON schema, normalised
   (decimal separator, ISO dates, currency) and written to SQLite.
8. The daemon sends a desktop notification over `org.freedesktop.Notifications`
   and prints the same JSON the CLI prints; the applet renders vendor, date, total
   and currency.

## What a file adds up to

A collection PDF holds twenty receipts, and the number anyone actually wants from
it is their sum. `totals.{h,cpp}` produces it, and the CLI, the notification and
the widget all go through it or through the same rules.

The sum is the easy part. The hard part is that a sum can be wrong while looking
perfectly reasonable, so most of the module is about what a total covers:

* **Currencies are never added together.** A file with a euro and a dollar bill
  reports `10,00 EUR + 5,00 USD`. `totalFor` keeps one running total per currency.
* **Amounts are summed in cents.** `totalFor` converts each amount with
  `qRound64(amount * 100)`. Adding doubles gives `0.1 + 0.2 =
  0.30000000000000004`, and the display would show the tail of it.
* **A bill that failed is counted, not summed.** It stays out of the total and
  lands in `failed`, which becomes `1 unread`. A bill that was read but had no
  total on it is a separate case and becomes `1 without an amount`, because one
  is worth retrying and the other is not.
* **A file that holds more than the sum covers says so.** `2 of 5 bills`. This is
  what the page count is for, and it is the reason the extraction layer had to
  start reporting the file's page count rather than the number of pages read.

That last point produced the only real bug in this feature. `Document` used to
expose only the pages that were read, since `ReadOptions::maxPages` caps them. A
sum built on that cannot tell a five page file read with `--pages 2` from a two
page file, so it would have claimed `2 bills` and looked complete. `Document` now
carries both numbers, and `analyseFile` keeps them apart:

* `Document::pageCount` — pages the file holds, straight from `fz_count_pages`.
* `Document::pages` — pages that were actually read.
* `BillResult::pageCount` — the file's count, which is what a total needs and
  what the `file.pdf:2` label is about.
* The loop that produces bills runs over the pages that were read.

Mixing the last two up was tried: iterating over the file's page count on a
`--pages 1` run asked the model about pages the page limit had deliberately
skipped, and answered with a failed bill for each. A page limit is a decision, not
an error.

The JSON contract is deliberately untouched by all of this. One object per bill
per line, no summary line: the widget adds up in JavaScript from the bills it
received, and a shell consumer can ask `jq`. A second kind of object on the wire
would force every consumer to learn to skip it, which is the sort of thing that
gets forgotten in one place and not another.

The widget's version of the same rules lives in `invoicelogic.js`, because it has
to work on what the reply contains, and the list it sums is not always the whole
file: the history limit can cut a group in half. `subtotalsFor` walks the list in
runs of one file, returns a parallel array of `null` with a total at the last bill
of each run, and reports `2 of 5` when the run is shorter than the file's page
count. It formats in the locale it is handed rather than calling `Qt.locale()`
itself, which is what lets `tst_plasmoid.qml` pin `de_DE` and assert on the
separators.

## Process boundaries

Every path that crosses a process boundary is made absolute first, because the
other side has no way to know where it came from.

`invoicedrop tests/testdata/rechnung.pdf` from the workspace root worked locally
and failed as soon as the daemon was on the bus: the CLI sent the argument over
D-Bus exactly as typed, the daemon had been started by the session bus with
`$HOME` as its working directory, and answered `file does not exist`. The path was
correct, the file was there, and the one piece of information needed to find it —
the caller's directory — is not part of a D-Bus call. Nothing on the receiving
side can recover it.

So `Paths::resolvePath` runs at the edge, in two places on purpose:

* In `runCli`, once the run is known to be about files. **After** the
  `history`/`daemon`/`doctor` comparison, because `resolvePath` turns a word into
  `<cwd>/word` when no such file exists, which would hide the subcommands from the
  comparison and break them.
* In `analyseFile`, which the CLI, the daemon and the tests all call. Resolving
  here means a new caller cannot reintroduce the bug by forgetting, and it is why
  the daemon needs no special handling for paths it receives.

An existing path is canonicalised, resolving symlinks and `..`; a missing one is
merely made absolute, because the useful thing to do with a path that does not
resolve is to name it in the error. The hash, the cache key and the archive all
work on the resolved path, so the same file reached by two spellings is one
document.

## Ollama integration

```http
POST /api/chat
{
  "model": "gemma4:latest",
  "stream": false,
  "think": false,
  "keep_alive": "30m",
  "format": { …invoice JSON schema… },
  "options": { "temperature": 0 },
  "messages": [
    { "role": "system", "content": "Extract invoice fields. Reply with JSON only…" },
    { "role": "user",   "content": "…extracted text…",
      "images": ["<base64 jpeg>", "…"] }
  ]
}
```

Notes on the transport:

* `QNetworkAccessManager`, synchronously. One reply in flight, because the CLI
  analyses one file and exits. Phase 4 wraps the same request builder in async
  calls for the daemon.
* `think: false`. Measured on a receipt: 10.2 s with a reasoning trace, 1.0 s
  without, identical answer. Reading an invoice is not a task that benefits from
  deliberation.
* `format` carries the JSON schema, which constrains decoding. A reply that
  parses but lacks the vendor or the total is retried exactly once with the
  complaint appended; a third attempt costs seconds and rarely helps.
* `keep_alive` keeps the weights resident, so the second document in a run is not
  a cold start.
* `/api/tags` is checked once before the first file, so a missing model produces
  one actionable line instead of the same failure per file.

### What the model is not asked to do

The date is read out of the text by the CLI when the model omits it. Given the
same text and `temperature: 0`, the model returns the issue date in some runs and
drops it in others; a date is a regular expression, not a judgement call. The
fallback only accepts a date on a line that carries a date label, because the
earliest date on a document is often a service period.

When the raster is too coarse to carry the text the model claims to have read —
below 400 px on the short edge, with no text layer — the result is marked with
`quality_warning`. Measured: at 174 px every model invents a vendor, a date and a
total, and one of them invented a currency that is not on the paper.

## Input handling and the PDF question

**Ollama has no PDF endpoint.** `/api/chat` accepts base64 images and text only.
Every backend that claims to "read PDFs" locally does the raster step in front of
the model:

| Backend | Native PDF | Native images | Consequence for InvoiceDrop |
|---------|-----------|---------------|-----------------------------|
| Ollama `/api/chat` | no | yes | PDF must be rasterised by our extractor |
| llama.cpp server (`--mmproj`) | no | yes | same, and less convenient model management |
| vLLM + Qwen2.5-VL | no | yes | same, heavier setup |
| Python stacks (docling, MinerU, olmOCR) | yes | yes | rejected: the stack must stay Python-free |

So the PDF support lives in our C++ extractor, not in the model. This is also
the faster design: a digital invoice with a text layer never touches the GPU.

## Model choice

Default: **`gemma4:latest`**, and it was chosen by measurement, not by reputation.

* Reads images directly, so scans, photos and rasterised PDF pages all work
  without a separate OCR model.
* Over the twelve documents in `tests/testdata/` it needed 27.8 s and made no
  mistakes that the input allowed. `minicpm-v:8b` needed 175.4 s on the same set,
  failed outright on one document — an unterminated JSON reply after 118 s — and
  invented two totals, including one on a receipt `gemma4` read down to the cent.
* `ollama pull gemma4:latest` is the whole setup.

Alternatives, same client code, `--model` only:

| Model | When |
|-------|------|
| `minicpm-v:8b` | measured in phase 2 and rejected: six times slower, less accurate |
| `qwen3.8:27b` | vision capable, more headroom if smaller models stall |
| `gemma4:26b` | the larger sibling, for documents the default misreads |
| `qwen2.5:7b-instruct` | text-layer fast path only, no images |

Any model that reports the `vision` capability in `/api/show` works. Check with
`invoicedrop doctor` once that subcommand exists; until then,
`curl -s localhost:11434/api/show -d '{"model":"NAME"}'` lists the capabilities.

The model is a settings value in both the daemon and the plasmoid configuration
dialog; they read the same key. Until the settings file lands, it is a flag:
`--model`.

## Concurrency and resources

* One generation at a time, queue depth 16, drop-oldest on overflow with a
  warning. Ollama is the bottleneck, not the extractor.
* Extraction runs on the thread pool with at most `min(4, cores)` concurrent
  jobs, so a second dropped file is already rasterised while the first is being
  inferred.
* Page cap 4 by default (an invoice beyond that is a contract, not an invoice);
  extras are noted in `notes` rather than silently dropped.
* Page images live only for the duration of `AnalyzeJob::run()`; the `Document`
  and its buffers are released as soon as the request body is built.
* Base64 inflates payloads by ~33 %; long edge 1600 px keeps a page around
  200–400 KB encoded, well inside the default 8 MB request body.
* Originals move to `~/.local/share/invoicedrop/archive/`; only hashes and
  extracted fields are indexed.

## Failure modes

| Situation | Behaviour |
|-----------|-----------|
| Ollama not running | `Analyze` returns `status:"error"`, applet shows "Ollama offline", exit code 2 |
| Model not pulled | error carries the `ollama pull <model>` hint |
| Extraction throws (broken PDF, out of memory) | caught inside the job, record stored as `status:"error"`, daemon survives |
| Encrypted or broken PDF | MuPDF error surfaces as `status:"error"`, record stored |
| Neither text nor images | request refused before touching the model |
| Model returns invalid JSON | one retry with the parse error appended, then stored as `status:"error"` |
| Duplicate drop | cached record returned, `status:"cached"` in JSON |

## DBus interface

The daemon owns the session bus name `org.kde.invoicedrop` and exports
`/InvoiceDrop` with `org.kde.invoicedrop.Control`. It is a `QDBusAbstractAdaptor`
with an inline introspection string: no XML file, no `qt6_add_dbus_adaptor`, and
no KDE Frameworks.

The CLI is fully functional without a daemon. When one is running the CLI hands
the paths over instead, because the daemon already has the model resident, and
then reformats the reply into the shape the run asked for. `--local` reads the
document here anyway.

| Method | Signature | Purpose |
|--------|-----------|---------|
| `Analyze` | `as, b -> s` | analyse paths, returns one JSON object per bill per line. The flag says whether the daemon should announce the result; the widget passes `false` when its notification switch is off, because the daemon is the process that raises the toast and the widget cannot take it back afterwards |
| `History` | `u -> s` | last N bills as newline separated JSON |
| `Status` | `-> a{sv}` | busy flag, model, endpoint, inbox, stored count, last file, pending |

D-Bus matches a method by its full argument signature, so adding the flag breaks
the call against a daemon that is still running from before the change. The CLI
therefore retries the one-argument form when the two-argument call fails and a
notification was wanted. Without that, every read for the rest of the session
would quietly fall back to a local one — correct output, but a model load per
drop for no reason other than a daemon that is one version behind.

Two files let the bus start the daemon on demand, so nothing has to be enabled by
hand after a login:

* `data/invoicedrop.service` — the systemd user unit, installed into
  `${CMAKE_INSTALL_LIBDIR}/systemd/user`.
* `data/org.kde.invoicedrop.service` — the D-Bus service file, with
  `SystemdService=invoicedrop.service`, installed into
  `${CMAKE_INSTALL_DATADIR}/dbus-1/services`.

## The daemon

`invoicedrop daemon` watches the inbox folder, reads what lands in it and says
so. `--once` drains the folder and exits.

**Notifications go to `org.freedesktop.Notifications` directly.** Sending one is
a single DBus call, and going straight to the interface avoids a KDE Frameworks
dependency for a toast. A missing or failing notification service is ignored: a
toast must never fail a run.

**A dropped file is not readable when it appears.** A copy from a camera or a
browser download is still being written, and reading it early yields a truncated
PDF and a confusing error. `InboxWatcher` therefore waits until the size has been
unchanged for two ticks before it reports the file, and remembers what it already
reported, keyed on size and modification time, so a rescan cannot hand the same
file over twice. Hidden names are skipped, because editors and download tools
write to a dot file and rename it into place.

**Reading a document blocks on a local event loop.** `OllamaClient` waits for the
HTTP reply with `QEventLoop`, so watcher signals arrive in the middle of a job.
The daemon refuses re-entrant work and queues it, and it prints the same JSON
lines the CLI prints, so a daemon run and a CLI run can be compared directly.

## Build

One toolchain, one binary.

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

* `src/CMakeLists.txt` builds `invoicedrop`. `find_package` pulls Qt6 `Core
  Network Sql DBus`, and `include(GNUInstallDirs)` supplies the install
  destinations. C++20 is required. No KDE Frameworks are linked and
  `extra-cmake-modules` is not used: the notification interface and the DBus
  adaptor are both plain Qt, and `GNUInstallDirs` already resolves `bin`, `lib`
  and `share` correctly on Arch.
* MuPDF, Tesseract and Leptonica are located with `pkg_check_modules`. They are
  linked, never executed, so there is no subprocess to version-check and nothing
  to shell out to.
* The DBus adaptor carries its introspection inline, so there is no XML file to
  compile and no code generator in the build. The D-Bus service file and the
  systemd user unit are configured from `data/*.in` with the real install paths.
* The systemd unit, the `.desktop` file, the icon and the plasmoid are installed
  by the same CMake project, so `cmake --install` and the package contain exactly
  the same files. The plasmoid stays pure QML and never needs rebuilding for a
  daemon change.

Build-time only: `cmake`, `ninja`, `pkgconf`.
Runtime: `mupdf`, `tesseract`, `tesseract-data-deu`, `tesseract-data-eng`,
`leptonica`, Qt6 libraries, and a running Ollama.

## Installation layout

`cmake --install` writes thirteen files. Nothing is placed outside the prefixes
`GNUInstallDirs` defines.

| Prefix | File | Purpose |
|--------|------|---------|
| `bin` | `invoicedrop` | the only executable |
| `bin` | `InvoiceDrop` | symlink, so both spellings work |
| `lib/systemd/user` | `invoicedrop.service` | starts the daemon at login |
| `share/dbus-1/services` | `org.kde.invoicedrop.service` | starts the daemon on the first CLI call |
| `share/applications` | `com.github.invoicedrop.desktop` | application menu entry |
| `share/icons/hicolor/scalable/apps` | `com.github.invoicedrop.svg` | the icon, under the reverse-DNS name |
| `share/plasma/plasmoids` | `com.github.invoicedrop/` | the widget, all of it |
| `share/licenses/invoicedrop` | `LICENSE` | MIT text |

**The systemd unit destination depends on the prefix, and `GNUInstallDirs` gets
it wrong on purpose.** It answers `lib` for every prefix, which is correct for
`/usr` and broken for `$HOME/.local`: `systemd-analyze --user unit-paths` lists
`~/.local/share/systemd/user` and does not list `~/.local/lib/systemd/user`, so a
unit installed under the latter is written, reads correctly, and is never
consulted. `systemctl --user start invoicedrop` then reports a unit that does not
exist, which is a confusing way to find out. `src/CMakeLists.txt` therefore maps
a system prefix to `lib/systemd/user` and anything else to `share/systemd/user`.
The D-Bus service file needs no such treatment, because
`<standard_session_servicedirs/>` covers `$XDG_DATA_HOME/dbus-1/services`, so
`~/.local/share/dbus-1/services` is found as it is.

**The icon is installed twice under two different names and that is deliberate.**
The theme gets a copy called `com.github.invoicedrop.svg` after the reverse-DNS
id, so `Icon=com.github.invoicedrop` in the desktop entry and in the notification
resolve through the icon theme. The plasmoid gets its own copy under its
`contents/icons/`, which it loads by relative path and which therefore has no
need of a theme lookup. Neither can use the other: a packaged plasmoid cannot
reach into `share/icons` by relative path, and a theme lookup cannot see inside a
package directory. Splitting them keeps both lookups simple, and the file is
1.2 kB.

No theme ships an icon named `invoice`. The widget's first version used that name
and drew an empty square with no error, because a missing icon name in QML is not
a problem the theme reports. Shipping the icon is what makes the panel show
anything at all.

**The desktop entry runs `doctor`, not the widget.** It exists so a user who has
just installed the package has a way to find out why nothing works: it opens a
terminal, prints the building blocks, the model, the database and the inbox, and
waits for enter so the output does not vanish. `Terminal=true` is required for
that, and `Categories=Office;Finance;` is what `desktop-file-validate` accepts --
two main categories are rejected.

`packaging/PKGBUILD` wraps the same `cmake` calls. It has no `source=()` and
therefore no checksums, because the project has no release tarball yet: it builds
from the checkout it sits in, and it reads `$PWD` rather than `$0`, since makepkg
sources the file and `$0` is `/usr/bin/makepkg`.

