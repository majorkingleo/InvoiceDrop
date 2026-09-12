---
name: ollama-communication
description: 'Talk to a local Ollama server from code: /api/chat with a JSON schema, images and think:false, plus /api/tags, /api/ps and /api/show, streaming versus non-streaming, keeping a model resident, timeouts, error mapping, and measuring latency instead of guessing. USE FOR: wiring a local LLM into a tool, daemon or widget; schema-constrained JSON output; vision models reading scans, photos or rasterised PDFs; reasoning models that cost 10x; cold starts; "connection refused" and "model not found"; choosing a model by measurement. DO NOT USE FOR: hosted LLM APIs (OpenAI, Anthropic, Gemini), general prompt engineering, or training and fine-tuning.'
argument-hint: 'What should the model read or return?'
---

# Talking to a local Ollama server

`ollama serve` speaks plain HTTP on `127.0.0.1:11434`. There is no SDK in the way
and nothing to authenticate: a request is a JSON body, a reply is a JSON body.
That simplicity is why the details below matter — the API will happily let a
model answer in prose, take ten seconds to do it, and leave you with a string you
cannot parse.

Provenance: written while building the InvoiceDrop reader on Ollama, Plasma 6 / Qt
6.11, September 2026. The measurements are from that project's twelve test
documents; the API shapes were verified against a running server on 2026-09-12.

## When to use

- A local model has to return **structured data** a program can use.
- A model has to read **images**: scans, photographs, rasterised PDF pages.
- Latency matters and someone is about to guess why it is slow.
- A tool or daemon needs to know whether the server is up and the model is pulled.

## The four calls worth knowing

```bash
# Is it up, and what is pulled?  -> .models[].name
curl -s localhost:11434/api/tags | jq -r '.models[].name'

# What is loaded in memory right now?  -> .models[].name
curl -s localhost:11434/api/ps | jq -r '.models[].name'

# Can this model see?  -> .capabilities
curl -s localhost:11434/api/show -d '{"model":"gemma4:latest"}' | jq -c '.capabilities'

# The one that does the work
curl -s localhost:11434/api/chat -d @request.json | jq -r '.message.content'
```

Verified on 2026-09-12: `.capabilities` came back as
`["completion","vision","audio","tools","thinking"]` for `gemma4:latest`, with
`.details.parameter_size` next to it. A model without `vision` in that list will
not read an image, and it fails by ignoring the image rather than by erroring.

`OLLAMA_HOST` is Ollama's own convention for the endpoint, so reading it means a
remote or proxied server needs no new flag. It is usually `host:port` with no
scheme: prepend `http://` when it has none, and strip a trailing `/`, or every
request lands on `//api/chat`.

## The request that works

`./assets/chat-request.json`, with the reasoning behind each field:

```json
{
  "model": "gemma4:latest",
  "stream": false,
  "think": false,
  "keep_alive": "30m",
  "format": { "type": "object", "properties": { "vendor": { "type": "string" } },
              "required": ["vendor"] },
  "options": { "temperature": 0 },
  "messages": [
    { "role": "system", "content": "You read business documents and return structured data." },
    { "role": "user", "content": "Extract the fields.",
      "images": ["<base64 jpeg>", "<base64 jpeg>"] }
  ]
}
```

- **`format` carries a JSON schema** and constrains decoding: the model cannot
  answer in prose or with half an object. Request `{"type":"object",
  "properties":{...},"required":[...]}` and only the fields you will actually
  store — every field costs output tokens and therefore time.
- **`stream: false`** for a one-shot call. Streaming means parsing NDJSON line by
  line and reassembling; it is worth it only when you want to show progress.
- **`think: false`.** Measured on one receipt: **10.2 s with a reasoning trace,
  1.0 s without, the same answer byte for byte.** Extraction is not a task that
  benefits from deliberation. Check `.capabilities` for `thinking` before
  assuming a model has the switch at all.
- **`keep_alive: "30m"`** keeps the weights resident, so the second document in a
  run is not a cold start. Without it, every call pays the load again.
- **`temperature: 0`** for stability across runs. Any cache keyed on the answer
  depends on this being the same answer twice.
- **Images go in the `images` array of the `user` message**, base64, no data URL
  prefix. Base64 inflates by ~33%: a 1600 px page is 200–400 KB encoded, well
  inside the default 8 MB body limit.
- **`system` and `user` are separate messages.** Rules belong in the system
  prompt; the document goes in the user message.
- **There is no PDF endpoint.** `/api/chat` takes text and images only, so a PDF
  has to be rasterised before it is sent.

## Reading the reply

```jsonc
{
  "message": { "role": "assistant", "content": "{\"vendor\": \"Hofer\"}" },
  "done_reason": "stop",
  "eval_count": 8,
  "total_duration": 531825223,     // ns
  "load_duration": 314309172       // ns
}
```

- **`message.content` is a *string* that contains JSON**, not a JSON object. Parse
  it. Even with `format` set, treat it as text: strip a markdown fence first
  (````json … ````), because a model that was told to answer with an object will
  sometimes wrap it in one anyway.
- `total_duration` and `load_duration` are **nanoseconds**. Comparing them is how
  you tell a slow model from a cold start without reaching for a stopwatch.
- `eval_count` is the number of tokens generated: a large number for a small
  document means the model is rambling, which is a reason to tighten the schema.

## Models are sloppy — normalise on the way in

Everything below was observed, not feared:

- `1190,00`, `1.190,00`, `1190.00` and `€ 1.190,00` all mean the same amount. Strip
  the currency, drop thousands separators, treat a comma as a decimal point.
- Dates arrive as `2025-04-30`, `30.04.2025` and `20250430`. Normalise to ISO 8601
  in one place, and never compare the raw string.
- Currency arrives as `€`, `eur`, `EUR`, or is missing entirely.
- A field that could not be read comes back **absent or empty, not wrong**. An
  empty string is not `0`, and a missing amount must never be stored as `0.0`.

## What not to ask the model for

Two of these were learned the expensive way and are worth more than the API
details:

1. **A date is a regular expression, not a judgement call.** Given the same text
   and `temperature: 0`, the model returns the issue date in some runs and drops
   it in others. Read the labelled date out of the text yourself and use it when
   the model stays silent. Only accept a date on a line that carries a date
   label — the earliest date on a document is often a service period.
2. **Below a certain raster resolution every model invents.** Measured: a scan
   174 px wide (~3.6 px per character) produced a confident vendor, a date and a
   total from two different models, and both were wrong; one invented a currency
   that is not on the paper. Below 400 px on the short edge with no text layer,
   mark the result as untrustworthy instead of presenting it as fact.
3. **Do not ask for line items you do not display.** They multiply output tokens
   and latency, and nothing reads them.
4. **Do not ask a model to count, add up, or format money.** Do that in code.

## Failure handling that stays actionable

| Situation | What to do |
|-----------|------------|
| `ConnectionRefusedError`, no HTTP status | The server is not running: say `systemctl --user start ollama` (or `systemctl start ollama`, depending on how it was installed) instead of "connection refused" |
| HTTP 4xx/5xx with an `error` field in the body | Read it. If it contains `not found`, the model is not pulled: say `ollama pull <model>` |
| Timeout | Say so with the duration, and add that the model may still be loading — the first call after a start can take a minute |
| Reply parses, required fields missing | Retry **exactly once**, with the complaint appended to the prompt |
| Retry also bad | Report what *was* read. A vendor without a total is still useful; pretending the document was empty is not |

Ollama reports its own failures **inside a 200 or 4xx body** as
`{"error": "..."}`. A client that only looks at the status code misses every
model-not-found and shows nothing.

Check the model once, before the first document (`/api/tags`), not once per file:
otherwise a missing model produces the same failure N times instead of one
actionable line.

**Compare model names exactly.** A name is `<model>:<tag>`, and the tag selects a
different set of weights, not a version of the same one. Verified while writing
this: a check that compared base names matched `gemma4:26b` when `gemma4:latest`
was asked for — a different and much slower model, reported as present. Try the
exact name, and fall back to the base name only when nothing matches.

## Concurrency, and one trap

**Ollama serves one generation at a time.** A second concurrent request queues
behind the first; it does not run in parallel. So queue in your own code with a
bounded depth rather than firing twenty requests at the server.

The trap: **a blocking wait that pumps the event loop re-enters everything else.**
Waiting for the reply with a nested `QEventLoop` (or any modal wait) lets timers
and signals fire while the model is thinking. In the InvoiceDrop daemon this made
an inbox watcher settle the same file during its own first read and hand it over
again, so one dropped document was analysed twice. Guard against re-entrancy in
the code that *calls* the client: refuse re-entrant work and queue it, and keep a
list of what has already been handled — a folder scan has no memory of its own.

## Measure, do not guess

```bash
./scripts/ollama-probe.sh gemma4:latest
```

It reports reachability, pulled models, what is resident, the capabilities, and
one timed call. Two numbers to compare before blaming the code:

- `total_duration` against `load_duration` — where the time actually went.
- **one model against another on the same documents.** In this project
  `gemma4:latest` read twelve documents in 27.8 s with no mistakes the input
  allowed, while `minicpm-v:8b` took 175.4 s, failed outright on one document
  (unterminated JSON after 118 s) and invented two totals. A model that "looks
  fine" on a clean text-layer invoice can be six times slower and less accurate on
  photographs.

Record the wall clock per document when a model is chosen, and never promote a
model on reputation. `./references/model-choice.md` has the full comparison and
the failure modes per situation.

## Checklist

- [ ] `format` carries a schema, and only the fields that are stored.
- [ ] `think: false` unless the task genuinely needs deliberation.
- [ ] `keep_alive` set, so a batch is not a series of cold starts.
- [ ] `temperature: 0`, so a cached answer is the same answer.
- [ ] `message.content` parsed as text, fence stripped first.
- [ ] Amounts, dates and currency normalised in one place, with absent ≠ zero.
- [ ] Connection refused, model missing and timeout each produce a command the reader can run.
- [ ] `/api/tags` checked once per run, not once per file.
- [ ] The calling code is re-entrancy safe around the blocking wait.

## More

- `./assets/chat-request.json` — the request body as sent, ready to `curl -d @`.
- `./scripts/ollama-probe.sh` — reachability, capabilities, one timed call.
- `./references/model-choice.md` — measured model comparison, failure modes.
- In this repository: `src/ollama.cpp` (the client), `src/invoice-schema.h` (the
  schema), `src/invoice.cpp` (tolerant normalisation), `invoicedrop doctor`
  (checks endpoint and model in one command).
