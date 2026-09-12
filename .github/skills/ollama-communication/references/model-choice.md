# Choosing a model, and what each failure looks like

All numbers are from the InvoiceDrop project's twelve documents in
`tests/testdata/` — digital PDFs, scans and photographs — measured September 2026
on the development machine, which is not a benchmark and is not meant to be one.
They are here because a model that looks fine on a clean text-layer invoice can be
six times slower and less accurate on a photograph.

## The comparison that decided the default

| Model | Twelve documents | Result |
|-------|------------------|--------|
| `gemma4:latest` | **27.8 s** | No mistakes the input allowed |
| `minicpm-v:8b` | **175.4 s** | One hard failure (unterminated JSON after 118 s), two invented totals |

The smaller model also returned nothing at all for a photographed receipt that
`gemma4` read down to the cent, including `18,48 EUR` and `2025-07-17`.

Read that as a shape, not as a ranking: *"fewer parameters"* and *"faster"* are
not the same axis, and the vision-capable small models tend to be slower per
document and worse at arithmetic on low-quality input. Measure on the documents
that actually matter before promoting one.

## Alternatives, all the same client code

| Model | When |
|-------|------|
| `minicpm-v:8b` | Measured and rejected: six times slower, less accurate |
| `qwen3.8:27b` | Vision capable, more headroom when a smaller model stalls |
| `gemma4:26b` | The larger sibling of the default, for documents it misreads |
| `qwen2.5:7b-instruct` | Text-layer fast path only, no images |

Any model that lists `vision` in `/api/show` capabilities works with the same
request. Check before assuming:

```bash
curl -s localhost:11434/api/show -d '{"model":"NAME"}' | jq -c '.capabilities'
```

## The single decision worth 10x

A reasoning trace, measured on one receipt:

| `think` | Time | Answer |
|---------|------|--------|
| `true` | 10.2 s | — |
| `false` | **1.0 s** | Identical, byte for byte |

Extraction, classification and formatting are not tasks that benefit from
deliberation. Send `think: false` by default and expose it as an opt-in flag for
the cases that need it, rather than sending it and letting the user wonder why one
document took ten seconds.

## Failure modes, per situation

| Situation | What the caller sees | What to do |
|-----------|----------------------|------------|
| Server not running | `ConnectionRefusedError`, HTTP status 0 | Say how to start it; do not report "connection refused" |
| Model not pulled | HTTP 4xx with `{"error": "... not found ..."}` | Say `ollama pull <model>` |
| First call after a start | One call much slower than the rest | `load_duration` shows it; `keep_alive` prevents the repeat |
| Model still loading | No reply within the timeout | Say the model may still be loading, with the duration |
| Reply lacks required fields | Parseable but incomplete | Retry once with the complaint appended, then report what was read |
| Unterminated JSON | Seen with `minicpm-v:8b` after 118 s | Not retryable in practice: report it, and consider the model |
| Coarse raster | A confident, wrong answer | Below 400 px on the short edge with no text layer, mark the result untrustworthy |
| Long document | Latency grows with pages | Cap the pages and say so, rather than silently reading the first N |

Two of these are about *not* presenting a number as fact. A model that cannot read
the total will still return one, and a caller that stores it without a warning is
the part of the system that is wrong.

## What the model is not asked to do

- **Dates.** Even at `temperature: 0` the model returns the issue date in some
  runs and drops it in others. Read it out of the text with a regular expression
  and use that when the field is absent — but only from a line that carries a date
  label, because the earliest date on a document is often a service period.
- **Line items.** They multiply output tokens and latency, and nothing displays
  them. Dropped from the schema deliberately.
- **Arithmetic.** Totals, sums and currency conversion are done in code, in cents,
  never by the model.
- **PDFs.** There is no PDF endpoint. Rasterise first, and the digital invoice
  with a text layer never touches the GPU at all.
