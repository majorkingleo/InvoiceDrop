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

**Status:** the command line tool is complete and usable. The Plasma widget is
phase 5 of the plan and does not exist yet. See `docs/plan.md` for what each
phase delivered and `docs/architecture.md` for how it works inside.

## Requirements

CachyOS or Arch, plus Ollama with a model that reports the `vision` capability.

```fish
sudo pacman -S --needed base-devel cmake ninja pkgconf \
    qt6-base libmupdf leptonica tesseract tesseract-data-deu tesseract-data-eng
```

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

## Quick start

```fish
invoicedrop rechnung.pdf
invoicedrop rechnung.pdf ~/scans/*.jpg
```

## The result line

One line per file, so it composes with `xargs`, `find -exec` and shell loops:

```
<file>  <shop>  <date>  <sum> <currency>
```

* `<shop>` is the vendor as printed on the document, casing included. A receipt
  from `hofer` stays `hofer`.
* `<date>` is the issue date, normalised to ISO 8601 (`2025-07-17`).
* `<sum>` is the final amount payable, formatted for your locale.
* Missing values print as `-` rather than `0`, so an unreadable field cannot be
  mistaken for a real one.

Problems go to stderr, results to stdout, so redirecting the results never
swallows a warning.

## JSON output

`--json` prints **one object per line**, whatever the number of files. That is
what makes `jq` behave the same interactively and in a loop:

```fish
invoicedrop --json rechnung.pdf | jq '.vendor'
invoicedrop --json ~/Rechnungen/*.pdf | jq -r '.vendor'
```

```json
{
  "file": "2025-08-oebb_9865091376.PDF",
  "path": "/home/u/Rechnungen/2025-08-oebb_9865091376.PDF",
  "kind": "pdf",
  "status": "ok",
  "has_text_layer": true,
  "page_count": 0,
  "text_chars": 1265,
  "elapsed_ms": 4,
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
| `kind` | `pdf` or `image` |
| `has_text_layer` | the PDF carried readable text and was not rasterised |
| `page_count` | how many page images were sent to the model |
| `text_chars` | characters of text sent along with the images |
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

## How a document is read

| Input | Route |
|-------|-------|
| PDF with a text layer | text is read directly, nothing is rasterised, under 5 ms |
| PDF without a text layer | pages are rendered, downscaled, and sent to the model |
| JPEG, PNG, TIFF, WebP, BMP | decoded, EXIF rotation applied, downscaled, sent to the model |

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
`--pages` defaults to 4. A collection of receipts needs a higher value, and note
that one file currently produces one record.

**Reading is slow.**
Reasoning models are the usual cause; leave `--think` off. The second document in
a run is much faster than the first because the weights stay resident.

## Project layout

```
src/                    the binary
  cli.cpp               options, output, exit codes
  invoice.{h,cpp}       the invoice model and all format normalisation
  invoice-schema.h      the JSON schema sent to the model
  ollama.{h,cpp}        the HTTP client
  extract/              document reading: MuPDF, Leptonica, Tesseract
tests/                  two QTest suites, run with ctest
  testdata/             real invoices and receipts used as regression input
docs/architecture.md    how it works and why
docs/plan.md            what each phase delivered
```

Build and test:

```fish
cmake --build build
ctest --test-dir build --output-on-failure
```

In VS Code, `Ctrl+Shift+B` builds and **Tasks: Run Test Task** runs the suites.
`Test: run (verbose)` shows the per-document timings.

## Licence

GPL-3.0-or-later. See `LICENSE`.
