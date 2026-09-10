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
| PDF, text layer ≥ 120 chars/page | MuPDF `fz_stext_page` text, no raster |
| PDF, no text layer | MuPDF renders leading pages to JPEG, Leptonica downscales, Tesseract OCRs |
| JPEG/PNG/TIFF/WebP/BMP | Leptonica `pixRead`, EXIF orientation, downscale, optional OCR |

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
| `--ollama-url URL` | override the endpoint |
| `--lang de` | language for free text fields |
| `--pages N` | page cap for PDFs |
| `--dpi N` | raster resolution |
| `--extract-only` | print extracted text, skip the model (phases 1 debugging) |
| `--no-cache` | re-analyse even if the hash is known |
| `--move` | archive the original after success |
| `-v` | verbose logging on stderr |

Subcommands arrive with their phases: `history`, `daemon`, `doctor`. The
implementation order is in `docs/plan.md`.

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
  "model": "qwen2.5vl:7b",
  "stream": false,
  "keep_alive": "30m",
  "format": { …invoice JSON schema… },
  "options": { "temperature": 0, "num_ctx": 8192 },
  "messages": [
    { "role": "system", "content": "Extract invoice fields. Reply with JSON only…" },
    { "role": "user",   "content": "…extracted text…",
      "images": ["<base64 jpeg>", "…"] }
  ]
}
```

Notes on the transport:

* `QNetworkAccessManager` only. One `QNetworkReply` in flight at a time; the
  daemon keeps a FIFO queue, because a single GPU cannot serve parallel
  generations usefully.
* `format` carries the JSON schema, which constrains decoding for models that
  support it. When a model ignores it, the daemon reparses the text, and on a
  parse failure retries once with the error appended to the prompt.
* `keep_alive` keeps the weights resident, so the first drop after a pause is not
  a cold start. Timeout is configurable, default 300 s.
* `/api/tags` is polled on startup and on `doctor` to verify the model exists and
  to report a missing pull.

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

Default: **`qwen2.5vl:7b`** (Qwen2.5-VL, Q4_K_M, ~6 GB VRAM).

* Reads images directly, so scans, photos and rasterised PDF pages all work
  without a separate OCR model.
* Strong on dense tables and small print, which is what invoices are.
* Solid German and English, plus most European invoice layouts.
* Ships with Ollama, so `ollama pull qwen2.5vl:7b` is the whole setup.

Alternatives, same client code, config-only change:

| Model | When |
|-------|------|
| `qwen2.5vl:3b` | ≤6 GB VRAM, or CPU-only; noticeably weaker on tables |
| `qwen2.5vl:32b` | ≥24 GB VRAM, best accuracy on messy scans |
| `gemma3:12b` | strong multilingual text, weaker layout reasoning |
| `minicpm-v:8b` | good OCR, small footprint |
| `llama3.2-vision:11b` | alternative, weaker German |
| `qwen2.5:7b-instruct` | text-layer fast path only, no images |

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
