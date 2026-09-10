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

## Repository layout

```
CMakeLists.txt                  top level, CMake only

plasmoid/com.github.invoicedrop/          QML only, no C++ plugin
  metadata.json                 KPackageStructure Plasma/Applet
  contents/config/main.xml      KConfigXT schema
  contents/config/config.qml    settings UI
  contents/ui/main.qml          PlasmoidItem, DropArea, ListView
  contents/ui/InvoiceCard.qml   one result row

src/                            C++20 / Qt 6 / KF 6
  CMakeLists.txt
  main.cpp                      QCoreApplication, dispatch daemon mode vs CLI mode
  application.{h,cpp}           service object, job queue, lifetime
  cli.{h,cpp}                   QCommandLineParser, thin DBus client
  settings.{h,cpp}              KConfigXT generated config
  dbus-adaptor.{h,cpp}          org.kde.invoicedrop at /InvoiceDrop
  org.kde.invoicedrop.xml       DBus interface description
  inboxwatcher.{h,cpp}          QFileSystemWatcher + settle-on-write delay
  notifier.{h,cpp}              KNotification
  ollama.{h,cpp}                QNetworkAccessManager, /api/chat, schema format
  invoice.{h,cpp}               fromJson/toJson, number and date normalisation
  invoice-schema.h              JSON schema sent to the model
  store.{h,cpp}                 QtSql QSQLITE, dedupe by SHA-256
  version.h.in
  jobs/analyzejob.{h,cpp}       QRunnable: extract on the pool, then infer
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
| `--move` | archive the original after success |
| `--verbose` | extraction notes and timings on stderr |

`-v` is not free: `QCommandLineParser` claims it for `--version`, so the verbose
flag is long form only.

Subcommands: `history` lists what is stored. `daemon` and `doctor` follow in
phases 4 and 6. The implementation order is in `docs/plan.md`.

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
8. The daemon emits a KNotification and keeps the last N records in memory; the
   CLI prints the JSON, the applet renders vendor, date, total and currency.

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

The model is a settings value in both `invoicedropd` (KConfig) and the plasmoid
configuration dialog; they write the same key.

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

Optional. The CLI is fully functional without a daemon — this interface exists so
that the plasmoid, the inbox watcher and a concurrent CLI call share one job
queue and one warm model. It arrives in phase 4 of `docs/plan.md`.

The daemon owns the session bus name `org.kde.invoicedrop` and exports
`/InvoiceDrop` with `org.kde.invoicedrop.Control`:

| Method | Signature | Purpose |
|--------|-----------|---------|
| `Analyze` | `as -> s` | analyse paths, returns a JSON array of records |
| `History` | `u -> s` | last N records as JSON |
| `Status` | `-> a{sv}` | busy flag, queue depth, active worker threads, model, ollama reachability |
| `Reload` | `-> ` | re-read configuration |

The service is `SystemdService=invoicedrop.service` in its `.service.in`, so the
first call from the CLI starts it without the user enabling anything manually.

## Build

One toolchain, one binary.

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

* `src/CMakeLists.txt` builds `invoicedropd`. `find_package` pulls Qt6 `Core
  Network Sql DBus`, KF6 `CoreAddons Config ConfigWidgets I18n Notifications`,
  plus `ECM`. C++20 is required.
* MuPDF, Tesseract and Leptonica are located with `pkg_check_modules` and
  `find_library`. ECM provides the KDE install layout, `KDE_INSTALL_LIBEXECDIR`
  for the binary and the session bus interface directory.
* The DBus interface XML is compiled into an adaptor with `qt6_add_dbus_adaptor`
  and installed into the interface directory, which is what lets DBus activate
  the service on the first CLI call. The KConfigXT class is generated from
  `src/invoicedrop.kcfg`.
* The systemd unit, the `.desktop` file and the icon are installed by the same
  CMake project. The plasmoid stays pure QML and is installed separately with
  `kpackagetool6`; it never needs rebuilding for a daemon change.

Build-time only: `cmake`, `ninja`, `extra-cmake-modules`, `pkgconf`.
Runtime: `mupdf`, `tesseract`, `tesseract-data-deu`, `tesseract-data-eng`,
`leptonica`, Qt6 and KF6 libraries, and a running Ollama.
