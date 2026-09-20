# Test data

Real invoices and receipts, used as regression input by two test suites.

`tst_documentreader` walks every document in this folder and only checks that the
reader produces something usable. It never contacts the model.

`tst_bills` checks what the model read. It runs for every
`<document>.expected-result.json` it finds, and compares each bill against the
expectation. It needs a running Ollama and skips itself when there is none.

## Writing an expected result

Name the file after the document, with `.expected-result.json` appended:

```
GuSp_SoLa2025_Bertahuette_Rechnungen.pdf
GuSp_SoLa2025_Bertahuette_Rechnungen.pdf.expected-result.json
```

One entry in `bills` per page of the document, in page order. **A bill is a
page**: a two page scan holds two receipts, and a collection PDF holds one bill
per page. The page limit is lifted for this test, so a 21 page collection expects
21 entries.

```json
{
    "bills": [
        {
            "bill": 1,
            "vendor": "BERTAHÜTTE",
            "date": "2025-07-22",
            "gross_total": 732.00,
            "currency": "EUR"
        },
        {
            "bill": 2,
            "vendor": "BERTAHÜTTE",
            "date": "2025-07-22",
            "gross_total": 24.20,
            "currency": "EUR"
        }
    ]
}
```

### Fields

| Key | Meaning |
|-----|---------|
| `bill` | page number, one based. Array order is used when it is left out |
| `vendor` | shop or company that issued the bill |
| `date` | issue date, `YYYY-MM-DD` |
| `gross_total` | final amount payable, a number, not a string |
| `currency` | ISO 4217 code |

### How each field is compared

| Field | Rule |
|-------|------|
| `gross_total` | exact, to the cent. `732.0` and `732.00` are the same number |
| `date` | exact |
| `currency` | exact, upper case |
| `vendor` | case, spacing and punctuation are folded away, then compared exactly. `BERNHARDT - CLAUDIA BERNHARDT` passes for `bernhardt-claudia bernhardt`, and `Unser Lagerhaus Warenhandels Ges.m.b.H.` for the same name written `... GesmbH` |

Use `"*"` as a value to skip a field entirely, and `null` to demand that the tool
reported nothing:

```json
{ "bill": 1, "vendor": "*", "date": "2025-07-22", "gross_total": 24.20, "currency": "EUR" }
```

Leaving a key out has the same effect as `"*"`: the field is not checked. Keeping
`"*"` explicit is easier to spot in a diff than a missing line.

### `vendor` patterns

Folding reaches case, spacing and punctuation. It cannot reach a letter the model
misread on a small logo or a word it left out, and those rows stay red for a
spelling nobody cares about. Write the name between slashes and it becomes a
regular expression, searched in the folded name (case is ignored in there too):

```json
{
    "bill": 3,
    "vendor": "/^ho[fl]+er/",
    "date": "2025-07-16",
    "gross_total": 177.16,
    "currency": "EUR"
}
```

`/^ho[fl]+er/` is `HOFER KG` as the paper and the model disagree about it: the
logo prints `HOFER` while the letterhead says `HOFER KG`, and the model also
answers `hoffer` and `hofler`.

Anchor the pattern with `^` when the difference it forgives comes after the name,
which is the usual case — a missing `KG`, a name the model wrote out in full. The
anchor is what keeps the row honest. These receipts have a name written on them by
hand above the printed vendor, and `HOFER`, `EUROSPAR` and `SPAR` are three
different shops, so the folded name has to *start* with the pattern: `Hanni` fails
against `/^ho[fl]+er/` and `Hanni EUROSPAR` against `/^eurospar/`. The `dm
drogerie markt` rows keep a plain name for the same reason, and a handwritten
name in front of it fails the plain comparison that a pattern without `^` would
have accepted, because a pattern that is not anchored is searched anywhere.

A pattern runs against the folded name, so it carries no hyphen, no run of spaces
and no full stop: `Ges.m.b.H.` is `gesmbh` once folded. A pattern that does not
compile is reported as a mistake in the file, not as a name the model got wrong.

## Why a bill can be marked unverified

Documents under 400 px on the short edge cannot carry the text a model claims to
read, and every model tested invents numbers for them. Those results come back
with `quality_warning` set. An expectation for such a document is a trap: the
model's answer is stable enough to pass once and fail the next time. Prefer
`"vendor": "*"`, or leave the document out of this suite.

## Regenerating an expectation

Print what the tool currently reads, then correct it by hand:

```fish
invoicedrop --json --pages 99 tests/testdata/dokument.pdf | jq
```

Do not paste the output in unchecked. An expectation that just records current
behaviour cannot fail, and therefore cannot catch anything.
