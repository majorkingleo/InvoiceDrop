# InvoiceDrop

Read invoices and receipts from PDF, JPEG and PNG files with a local AI model,
and report the shop, the date and the sum on one line.

```
$ invoicedrop bill1.pdf
bill1.pdf  Unser Lagerhaus Warenhandels ges.m.b.H.  2025-07-17  18,48 EUR
```

Everything runs on your machine. Inference goes to a local
[Ollama](https://ollama.com) instance, text extraction and rasterising use MuPDF,
Leptonica and Tesseract. No document and no field ever leaves the machine.

**Status:** every phase in `docs/plan.md` is finished. The command line tool, the
cache, the daemon, the Plasma widget and the package all work. One thing is
currently red: the `bills` suite, which compares what the model read against the
expected results in `tests/testdata/`, and which fails on the model's reading
rather than on the code's. `docs/plan.md` records what each phase delivered and
`docs/architecture.md` how it works inside.

## Vibecoded

This project was written in a chat, not by hand. The code, the tests and this
document were produced by **DeepSeek V4 Flash**, through GitHub Copilot in VS Code
1.137.0 on CachyOS, over the 10th and 11th of September 2026. The model was
reached through the `vizards.deepseek-v4-for-copilot` extension, version 0.8.2.
The requirements, the real invoices, the hardware and every decision along the way
came from a human, who ran the result and judged it.

That is worth knowing before reading the rest, because it changes what this prose
is evidence of. Three rules were followed while writing:

* **A claim that says "measured" was measured.** The default for `--think`, the
  choice of `gemma4:latest` over `minicpm-v:8b`, the decision to leave OCR off, the
  quality warning threshold — each of those came from running the thing against
  the documents in `tests/testdata/` and writing down what came out.
* **What could not be checked says so.** `makepkg` inside a clean chroot, for one,
  has not been run, and `docs/plan.md` says that rather than implying it works.
* **Each phase records what it got wrong.** The interesting parts of
  `docs/plan.md` are the bugs, and most were found by running a flag rather than
  by re-reading the code: a page rendered at twice its size, a `--pages` limit
  that invented bills for pages it had skipped, a `check()` that passed while
  testing nothing.

What it does not mean is that any of this is right because it compiles. Nobody has
read the code line by line, and an assistant is very good at producing something
that merely looks considered. The tests are the guard: seven suites, six of which
need neither a model nor a network and finish in about five seconds
(`ctest --test-dir build -E bills`). Where the reasoning is checkable it is
written down in `docs/architecture.md`, so it can be argued with.

If a paragraph here reads like a confident guess, treat it as one. The documents
try to say which ones are.

## Requirements

CachyOS or Arch, plus Ollama with a model that reports the `vision` capability.

```fish
sudo pacman -S --needed base-devel cmake ninja pkgconf \
    qt6-base libmupdf leptonica tesseract tesseract-data-deu tesseract-data-eng
```

No KDE Frameworks are needed, in any phase built so far. Notifications go
straight to `org.freedesktop.Notifications` and the D-Bus adaptor is plain Qt.

MuPDF, Leptonica and Tesseract are all needed to build. At runtime Tesseract is
reached only through `--ocr`, so a document with a text layer, and a page image
sent to the model, are both read without it.

Ollama itself comes from the AUR or the CachyOS repository; pick the variant that
matches the GPU (`ollama-cuda`, `ollama-rocm`, or plain `ollama` for CPU). Then
pull a vision model:

```fish
ollama pull gemma4:latest
```

Any vision model works. `gemma4:latest` is the default because it was measured
against the alternatives: see [Choosing a model](#choosing-a-model).

## Build

```fish
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary lands in `build/src/invoicedrop` and is used from there during
development. To put it on the PATH:

```fish
sudo cmake --install build     # installs invoicedrop and a InvoiceDrop symlink
```

`cmake` defaults to a Debug build if no build type is given.

## Install

Into your home directory, which needs no root and is the way to try a change:

```fish
cmake -B build-local -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-local
cmake --install build-local
```

In VS Code this is **Tasks: Run Task → Install: local (build + install to
~/.local)**, with two companions: **Install: local (verify)** prints what landed
and runs `doctor` through the installed binary, and **Install: local (uninstall)**
takes it all away again using the manifest CMake wrote, leaving your invoices and
history alone.

The separate `build-local` directory is not a preference. `data/*.service.in` are
`configure_file` templates and `ExecStart` is baked in from
`CMAKE_INSTALL_FULL_BINDIR` at configure time, so `cmake --install --prefix
"$HOME/.local"` on the regular build would write a unit whose `ExecStart` points
at `/usr/local/bin/invoicedrop`, where nothing was installed. The prefix has to
be known when CMake configures.

A prefix under `$HOME` also moves the systemd unit: it goes to
`share/systemd/user/`, not `lib/systemd/user/`. `systemd-analyze --user
unit-paths` lists `~/.local/share/systemd/user` and does not list
`~/.local/lib/systemd/user`, so a unit in the latter is written, looks correct
and is never read. The package, installed under `/usr`, keeps `lib/systemd/user`.

As a package, which is the normal way:

```fish
cd packaging
makepkg -si
```

The `PKGBUILD` builds from this checkout -- there is no release tarball yet, so
it has no `source=()` and no checksums. Run `makepkg` from inside `packaging/`;
it reads the source directory from `$PWD` and fails with "The source directory
/usr does not appear to contain CMakeLists.txt" if you run it from elsewhere. Set
`INVOICEDROP_SOURCE` to point it at a checkout in a different place.

What the package puts on disk:

| Path | What |
|------|------|
| `bin/invoicedrop`, `bin/InvoiceDrop` | the binary and a symlink |
| `lib/systemd/user/invoicedrop.service` | starts the daemon at login |
| `share/dbus-1/services/org.kde.invoicedrop.service` | starts the daemon on the first CLI call |
| `share/applications/com.github.invoicedrop.desktop` | application menu entry |
| `share/icons/hicolor/scalable/apps/com.github.invoicedrop.svg` | the icon |
| `share/plasma/plasmoids/com.github.invoicedrop/` | the widget |
| `share/licenses/invoicedrop/LICENSE` | GPL-3.0-or-later |

**InvoiceDrop in the application menu runs `doctor`, not the widget.** A fresh
install that does not work yet is the only interesting case, so the menu entry
opens a terminal, prints what InvoiceDrop can and cannot do, and waits for enter
before closing. The widget is added to a panel through Plasma's own *Add
Widgets* dialog; nothing installs it there for you.

After installing, check the ground before blaming the tool:

```fish
invoicedrop doctor
```

It reports the PDF engine, OCR, the Ollama endpoint, the model, the database and
the inbox, and prints the command that fixes whatever is missing. Exit code `0`
is fine, `2` means something required is broken, which makes it usable as
`invoicedrop doctor; or echo "not ready"`.

## Quick start

```fish
invoicedrop rechnung.pdf
invoicedrop rechnung.pdf ~/scans/*.jpg
```

## The result line

One line per bill, so it composes with `xargs`, `find -exec` and shell loops:

```
<file>  <shop>  <date>  <sum> <currency>
```

A file with more than one page is labelled `file.pdf:2`, because a file is not a
bill. A two page scan produces two lines.

* `<shop>` is the vendor as printed on the document, casing included. A receipt
  from `hofer` stays `hofer`.
* `<date>` is the issue date, normalised to ISO 8601 (`2025-07-17`).
* `<sum>` is the final amount payable, formatted for your locale.
* Missing values print as `-` rather than `0`, so an unreadable field cannot be
  mistaken for a real one.

Problems go to stderr, results to stdout, so redirecting the results never
swallows a warning.

## The file total

After the bills of a file that holds more than one, the file is added up:

```
GuSp_SoLa2025_Tanken_Rechnungen.pdf:1  Tank Roth GmbH       2023-07-15   65,50 EUR
GuSp_SoLa2025_Tanken_Rechnungen.pdf:2  Eni Service-Station  2025-07-26   90,91 EUR
GuSp_SoLa2025_Tanken_Rechnungen.pdf:3  Tank Roth GmbH       2022-07-27   21,35 EUR
GuSp_SoLa2025_Tanken_Rechnungen.pdf  sum  177,76 EUR  (3 bills)
```

A file with a single bill gets no total line: it would repeat the line above it
word for word. The same rule applies to the widget, where the total is drawn as a
tinted row under the last card of a file, and to the notification.

**The count in brackets is the point of the line.** `177,76 EUR` on its own claims
nothing; `177,76 EUR  (3 bills)` claims that three bills are inside it, and that
claim can be checked against the file. When it does not hold, the line says so
instead of quietly summing the part it has:

```
tanken.pdf  sum  156,41 EUR  (2 of 5 bills)
tanken.pdf  sum   15,50 EUR  (2 of 3 bills, 1 unread)
tanken.pdf  sum   15,00 EUR  (2 of 3 bills, 1 without an amount)
```

* `2 of 5 bills` — the file holds five, the page limit or the history cut it
  down. `--pages 2` on a five page file reports two of five.
* `1 unread` — a bill that could not be read at all. Worth retrying.
* `1 without an amount` — the page was read, the model found no total on it.

Two further rules that keep the number from lying:

* **Currencies are never added together.** A file with a euro bill and a dollar
  bill prints `10,00 EUR + 5,00 USD`. Adding them would produce a number that is
  not money.
* **Amounts are summed in cents.** Adding the amounts as they arrive gives
  `0.1 + 0.2 = 0.30000000000000004`, and a total that is wrong in the last digit
  is a total nobody checks twice.

`--json` is unchanged: one object per bill per line, with no summary line. The
widget adds up in JavaScript from the bills it received, and a consumer that wants
a total from the JSON can ask `jq` for one. Putting a second kind of object on the
wire would make every consumer learn to skip it.

## JSON output

`--json` prints **one object per line**, whatever the number of files. That is
what makes `jq` behave the same interactively and in a loop:

```fish
invoicedrop --json rechnung.pdf | jq '.vendor, .gross_total'
invoicedrop --json ~/Rechnungen/*.pdf | jq -r '.vendor'
```

```json
{
  "file": "2025-08-oebb_9865091376.PDF",
  "path": "/home/u/Rechnungen/2025-08-oebb_9865091376.PDF",
  "bill": 1,
  "bill_count": 1,
  "status": "ok",
  "has_text_layer": true,
  "from_cache": false,
  "extract_ms": 4,
  "inference_ms": 1603,
  "notes": ["MuPDF: 1 page(s), reading 1", "text layer used, nothing rasterised"],

  "vendor": "ÖBB-Personenverkehr AG",
  "vendor_address": "Postfach 222, 1020 Wien",
  "invoice_number": "9865091376",
  "date": "2025-07-31",
  "due_date": "",
  "currency": "EUR",
  "net_total": null,
  "tax_total": 1.92,
  "gross_total": 21.12,
  "iban": "",
  "confidence": null
}
```

| Field | Meaning |
|-------|---------|
| `status` | `ok`, or `error` with `error` set to the reason |
| `bill` | one based page number: a bill is a page, not a file |
| `bill_count` | pages read from this file |
| `has_text_layer` | the page text came from the PDF, not from OCR or an image |
| `from_cache` | answered from the store, so no model was called |
| `extract_ms`, `inference_ms` | where the time went |
| `notes` | why the reader chose the route it chose |
| `quality_warning` | present when the source is too coarse to trust |

The invoice fields are flat, so `jq '.vendor'` works without digging. For
several files use `jq -s` when you need them as one array.

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | every file was read |
| `1` | usage error, for example no files given |
| `2` | at least one file failed, or Ollama or the model was unavailable |

```fish
invoicedrop *.pdf; and echo "alle gelesen"
```

## Options

### Input

| Option | Effect |
|--------|--------|
| `--dpi N` | raster resolution for PDF pages, default 200 |
| `--pages N` | maximum pages to read per file, default 4 |
| `--long-edge N` | downscale images so the long edge is at most this, default 1600 |
| `--text-only` | use the PDF text layer only, never rasterise |
| `--images-only` | always rasterise, ignore the text layer |

### Model

| Option | Effect |
|--------|--------|
| `--model NAME` | which Ollama model to use |
| `--ollama-url URL` | base URL, defaults to `$OLLAMA_HOST` or `127.0.0.1:11434` |
| `--timeout SEC` | how long to wait for one answer, default 300 |
| `--think` | let a reasoning model deliberate first; slower, not more accurate |
| `--lang CODE` | language the model writes free text in, default `de` |

### Output

| Option | Effect |
|--------|--------|
| `--json` | machine readable output |
| `--verbose` | extraction notes and timings on stderr |
| `--extract-only`, `-e` | print what the extractor found, skip the model |
| `--dump-images DIR` | write the rasterised pages as JPEG, for inspection |
| `--db PATH` | database file, defaults to `~/.local/share/invoicedrop/invoicedrop.db` |
| `--no-cache` | read the document again instead of using the store |
| `--move` | move the original into the archive once every bill was read |
| `--limit N` | how many bills `history` lists, default 20 |
| `--local` | read here instead of asking a running daemon to do it |

### Daemon

| Option | Effect |
|--------|--------|
| `--inbox DIR` | folder to watch, default `~/.local/share/invoicedrop/inbox` |
| `--once` | read the inbox and exit instead of watching |
| `--no-notify` | send no desktop notifications |

### OCR tuning

Off by default. These only matter with `--ocr`.

| Option | Effect |
|--------|--------|
| `--ocr` | run Tesseract over rasterised pages |
| `--ocr-lang LANGS` | Tesseract packs, for example `deu+eng` |
| `--ocr-short-edge N` | short edge to scale to before recognising, default 1400 |
| `--psm MODE` | Tesseract page segmentation mode, 0 to 13, default 6 |
| `--no-normalise` | skip adaptive contrast normalisation |

`-v` is not available for `--verbose`; Qt reserves it for `--version`.

## Examples

Read a folder and keep going after a bad file:

```fish
for f in ~/Rechnungen/*.pdf
    invoicedrop $f
end
```

Build a CSV:

```fish
invoicedrop --json ~/Rechnungen/*.pdf \
  | jq -r '[.file, .vendor, .date, .gross_total, .currency] | @csv'
```

Sum the month. `jq -s` slurps the stream into an array first:

```fish
invoicedrop --json --pages 1 ~/Rechnungen/*.pdf | jq -s '[.[].gross_total] | add'
```

Find out why a scan came back empty, and look at what the model saw:

```fish
invoicedrop --verbose --dump-images /tmp/pages scan.pdf
ls /tmp/pages
```

See the raw text layer without touching the model, which is instant:

```fish
invoicedrop --extract-only --pages 1 rechnung.pdf
```

Compare two models on your own documents:

```fish
invoicedrop --model gemma4:latest --json rechnung.pdf | jq '.gross_total'
invoicedrop --model minicpm-v:8b     --json rechnung.pdf | jq '.gross_total'
```

## The Plasma widget

A panel widget that takes the same drop. Drag an invoice onto it and a card
appears with the shop, the date and the sum; a document with several pages
produces several cards, because a bill is a page, and a tinted row under the last
of them carries the file's total.

The widget owns no logic. It builds the same command the shell would and reads
the same JSON, so a drop cannot behave differently from a terminal run.

The total under a file is the one number the widget works out itself, because the
JSON deliberately carries no summary line. It follows the same rules as the
command line: never across currencies, in cents, only for a file with more than
one bill, and with the count beside it. A file that was cut by the list's history
limit is summed over the part that is on screen and says so, since the file's own
page count is in every reply.

```fish
kpackagetool6 --type Plasma/Applet --install plasmoid/com.github.invoicedrop
plasmawindowed com.github.invoicedrop     # try it in a window first
```

Then add it: right click the panel, **Add Widgets**, search for `InvoiceDrop`.
For a system-wide install, `cmake --install` places the package under
`share/plasma/plasmoids`.

The widget calls `invoicedrop`, so that binary has to be on the PATH Plasma sees.
`cmake --install` puts it in `/usr/local/bin`. To check:

```fish
plasmawindowed com.github.invoicedrop    # watch for errors in the terminal
```

Settings are available in the widget's own configuration dialog: the binary path,
the model, how many bills to keep in the list, and whether a document should be
moved into the archive once it was read.

### Reloading the widget

Plasma compiles a widget's QML once and keeps it, so a changed `main.qml` does
nothing until the shell reads the package again. There is no per-applet reload.
Logging out is not needed; restarting the shell does the same thing in a second:

```fish
systemctl --user restart plasma-plasmashell.service
```

The panels are recreated, running programs are untouched, and the widget keeps its
place, because the layout lives in `plasma-org.kde.plasma.desktop-appletsrc`
rather than in the shell. In VS Code this is **Tasks: Run Task → Plasma: reload
widget**, which installs the current tree first and then restarts the shell. That
is the whole loop: change the QML, run the task, click the widget.

`plasmawindowed com.github.invoicedrop` is the faster loop and needs neither an
install nor a restart, because it reads the package from disk on every start. It
shows the widget in its own window instead of in the panel, which is the only
difference that matters while working on the widget itself.

## The daemon

Reading a document in a fresh process pays the model load every time, which
dominates the wall clock for a single file. The daemon pays it once and then
watches a folder:

```fish
invoicedrop daemon                    # watch ~/.local/share/invoicedrop/inbox
invoicedrop daemon --once             # read what is there and exit
invoicedrop daemon --inbox ~/Belege   # watch somewhere else
```

Drop a file into the folder and it is read, stored, announced with a desktop
notification, and printed as JSON on stdout. `--no-notify` silences the toast.

While the daemon runs, the CLI hands its work over instead of reading locally,
because the daemon already has the model resident:

```
$ invoicedrop rechnung.pdf          # 14 ms, the daemon had it cached
$ invoicedrop --local rechnung.pdf  # 3.7 s, read here
```

The output and the exit code are identical either way; only the work moves.
`--local`, `--no-cache` and `--dump-images` always read here, since the daemon
cannot honour them.

The first call of a session starts the daemon through D-Bus activation, so it
never has to be enabled by hand for the tool to be quick. This is also what makes
the widget's first drop the only slow one.

### Starting it automatically

`cmake --install` places two files so the daemon can be started by the system or
by the bus:

| File | Goes to |
|------|---------|
| `invoicedrop.service` | `lib/systemd/user` under a system prefix, `share/systemd/user` under `$HOME` |
| `org.kde.invoicedrop.service` | `share/dbus-1/services`, for on-demand activation |

The two systemd destinations are not a slip: `systemd-analyze --user unit-paths`
lists `~/.local/share/systemd/user` and does not list `~/.local/lib/systemd/user`,
so a unit for a `$HOME` prefix has to land in `share`. A package under `/usr`
keeps `lib`.

```fish
systemctl --user enable --now invoicedrop     # after installing
systemctl --user status invoicedrop
```

Without enabling anything, the first CLI call starts the daemon through D-Bus
activation, because `<standard_session_servicedirs/>` covers
`$XDG_DATA_HOME/dbus-1/services` as well as the system directories.

## Cache and history
Every bill that is read is stored in SQLite, so the second run of a document
costs nothing: extraction drops to 0 ms and the model is not called at all.

```fish
invoicedrop rechnung.pdf      # 2 s, reads and stores
invoicedrop rechnung.pdf      # instant, answered from the store
invoicedrop history           # everything stored, newest first
invoicedrop history --json    # one JSON object per line
```

The cache is keyed on the hash of the file **and** on the settings that change
the answer — model, page limit, dpi, OCR switches. Re-reading a file with
`--model` or `--pages` therefore does the work again instead of serving an answer
that was produced under other conditions. `--no-cache` forces it either way:

```fish
invoicedrop --no-cache --model gemma4:26b rechnung.pdf
```

`history` prints one line per bill:

```
2026-09-10 21:02  hofer                 2025-07-09     11,91 EUR  p1 Rechnung 2.jpg
```

To move originals out of the way once they are read, so the same folder can be
dropped again without a cache hit hiding new content:

```fish
invoicedrop --move ~/Rechnungen/*.pdf
```

Originals land in `~/.local/share/invoicedrop/archive/`. A file is only moved
when every one of its pages was read.

## How a document is read

| Input | Route |
|-------|-------|
| PDF with a text layer | text is read directly, nothing is rasterised, under 5 ms |
| PDF without a text layer | pages are rendered, downscaled, and sent to the model |
| JPEG, PNG, TIFF, WebP, BMP | decoded, EXIF rotation applied, downscaled, sent to the model |

**A bill is a page, not a file.** A two page scan holds two receipts and a
collection PDF holds twenty one, so every page is analysed on its own and yields
its own record. The page number appears in the result as `file.pdf:2` when a file
has more than one page.

Text and images go to the model in the same message, so one vision model serves
all three kinds of input. The reply is constrained to a JSON schema, so the model
cannot answer with prose.

Two details that matter in practice:

* **The date is read from the text directly when the model omits it.** Given the
  same input the model returns the date in some runs and drops it in the next,
  even at temperature 0. A date is a regular expression rather than a judgement
  call, so the labelled date is taken from the text instead of asking twice.
* **A result built from an unreadably small scan is flagged.** Below 400 px on
  the short edge every model tested invents a vendor, a date and a total. Those
  results carry `quality_warning` and the warning is printed on stderr.

## Choosing a model

The model must report `vision`:

```fish
curl -s localhost:11434/api/show -d '{"model":"gemma4:latest"}' | jq .capabilities
```

Measured over the twelve real documents in `tests/testdata/`:

| Model | Total time | Problems |
|-------|-----------|----------|
| `gemma4:latest` | 27.8 s | none |
| `minicpm-v:8b` | 175.4 s | one hard failure, two invented totals |

`gemma4:latest` is the default. `--model` switches, nothing else changes. The
first document of a run also pays the model load, which is why a single file
takes about 2 s and the second one is faster.

## Troubleshooting

**`no Ollama at http://127.0.0.1:11434 - start it with: systemctl start ollama`**
Ollama is not running. Start it, or point `--ollama-url` at another host.

**`model 'gemma4:latest' is not installed - run: ollama pull gemma4:latest`**
The model is not pulled. The message also lists what is installed.

**The sum is wrong on a photographed receipt.**
Look at what the model saw:

```fish
invoicedrop --verbose --dump-images /tmp/pages receipt.jpg
```

If `/tmp/pages` shows a legible image, try another model. If it shows a narrow
strip, the source itself is clipped and no model can fix that.

**`quality_warning: the source is only 174 px across`**
The scan is too small to read. Rescan at 300 dpi. This is the one case where the
model produces plausible-looking numbers that are simply wrong.

**No date, or a date that looks like a service period.**
Check what the text layer actually contains:

```fish
invoicedrop --extract-only rechnung.pdf | head -40
```

**Every page of a long PDF is not read.**
`--pages` defaults to 4, and a collection of receipts needs a higher value. The
file's own total then says what it covers: a five page file read with
`--pages 2` reports `sum ... (2 of 5 bills)` rather than claiming to be the whole
file.

**Nothing works and you do not know where to start.**

```fish
invoicedrop doctor
```

It checks every moving part and prints the command that fixes the broken one.
It never needs a document, so it works before you have anything to read.

**`file does not exist`, for a file that is right there.**
This was a bug, and the message is worth knowing about because it was misleading.
A relative path is only meaningful next to the shell it was typed in, and the
daemon that may end up doing the reading is started by the session bus in `$HOME`.
It cannot see your directory, so `invoicedrop tests/testdata/rechnung.pdf` reached
it as a path that existed nowhere. Paths are now made absolute in the CLI and again
in `analyseFile`, so every caller is covered. If you still see this, the file
really is not where the path says: the label next to the message is only the file
name, so check the whole path by hand.

**Reading is slow.**
Reasoning models are the usual cause; leave `--think` off. The second document in
a run is much faster than the first because the weights stay resident.

## Project layout

```
plasmoid/               the Plasma widget, QML only
data/                   the systemd unit, the D-Bus service and the desktop entry
packaging/              PKGBUILD and the install script for CachyOS/Arch
src/                    the binary
  cli.cpp               options, output, exit codes, delegation to the daemon
  daemon.{h,cpp}        inbox watching, the DBus interface, notifications
  inboxwatcher.{h,cpp}  settle delay and the handled list
  notifier.{h,cpp}      freedesktop notifications over DBus
  analysis.{h,cpp}      the pipeline: read, cache, infer, one record per bill
  invoice.{h,cpp}       the invoice model and all format normalisation
  invoice-schema.h      the JSON schema sent to the model
  totals.{h,cpp}        what one file adds up to, and what is missing from it
  ollama.{h,cpp}        the HTTP client
  store.{h,cpp}         SQLite: documents, bills, the settings fingerprint
  extract/              document reading: MuPDF, Leptonica, Tesseract
tests/                  seven suites, run with ctest
  testdata/             real invoices plus their expected results
docs/architecture.md    how it works and why
docs/plan.md            what each phase delivered
```

Build and test:

```fish
cmake --build build
ctest --test-dir build --output-on-failure
```

In VS Code, `Ctrl+Shift+B` builds and **Tasks: Run Test Task** runs the suites.
`Test: run (verbose)` shows the per-document timings. A second build directory
exists for `build-local`, which is what the install tasks use; it is ignored by
git alongside `build/`.

## Licence

GPL-3.0-or-later. See `LICENSE`.
