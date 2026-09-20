.pragma library

// Pure helpers, kept out of the QML so they can be exercised on their own. The
// widget is a drop target and a list; everything with a decision in it lives
// here.

/// Wraps a value in single quotes for the shell, escaping an embedded quote.
function quote(value) {
    return "'" + String(value).replace(/'/g, "'\\''") + "'";
}

/// The first line of a message, which is the one worth showing.
///
/// A command that failed explains itself on its first line and fills the rest with
/// whatever it was doing, and the widget's status row has room for one line.
function firstLine(text) {
    if (text === undefined || text === null)
        return "";

    const trimmed = String(text).trim();
    if (trimmed.length === 0)
        return "";

    return trimmed.split("\n")[0].trim();
}

/// Builds the command line the widget runs.
///
/// The CLI answers in JSON, one object per bill per line, and hands the work to
/// a running daemon by itself, so the same command is fast when a daemon is up
/// and correct when none is.
///
/// `notify` is passed on rather than acted on here: the toast is raised by
/// whichever process reads the document, and when a daemon does that, this flag
/// is the only thing that can silence it.
function buildCommand(cliPath, model, archiveAfterReading, notify, path) {
    const parts = [quote(cliPath), "--json"];
    if (model)
        parts.push("--model", quote(model));
    if (archiveAfterReading)
        parts.push("--move");
    if (!notify)
        parts.push("--no-notify");
    parts.push(quote(path));
    return parts.join(" ");
}

/// The command that reads one document again, ignoring the store.
///
/// `--no-cache` is the whole point: a retry is asked for because the answer on
/// screen is wrong, and the stored answer that produced it would only be handed
/// back a second time. It also settles where the work happens — the CLI reads a
/// `--no-cache` request itself rather than handing it to a running daemon — which
/// is what makes this slower than a drop and worth a second thought before use.
///
/// `--move` is left out, unlike in `buildCommand`. After a first read the
/// original may already be in the archive, and a retry that asked for it again
/// would fail on a document that is not there any more.
function retryCommand(cliPath, model, notify, path) {
    const parts = [quote(cliPath), "--json", "--no-cache"];
    if (model)
        parts.push("--model", quote(model));
    if (!notify)
        parts.push("--no-notify");
    parts.push(quote(path));
    return parts.join(" ");
}

/// Parses the reply: one JSON object per line, one line per bill.
///
/// Returns `{ bills: [...], broken: n }`, where `broken` counts lines that did
/// not yield a bill. A silent gap in the list would be worse than a visible one,
/// so the count is handed back rather than swallowed.
function parseBills(stdout) {
    const bills = [];
    let broken = 0;

    const lines = String(stdout).split("\n");
    for (let i = 0; i < lines.length; ++i) {
        const line = lines[i].trim();
        if (line.length === 0)
            continue;
        try {
            const parsed = JSON.parse(line);
            // A line that parses into something other than an object is not a
            // bill, and must not become a card with nothing in it. The widget runs
            // more than one command through the same data source, so a reply of
            // `0` from the store check would otherwise land in the list as a
            // number, which every later read of a field turns into `undefined`.
            if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
                broken += 1;
                continue;
            }
            bills.push(parsed);
        } catch (error) {
            broken += 1;
        }
    }

    return { bills: bills, broken: broken };
}

/// The path of each command that is still in flight.
///
/// The data engine reports the source string it was handed rather than anything
/// that was passed alongside it, so the path has to be remembered. Recovering it
/// by parsing the command back out was tried and is wrong: a path containing an
/// apostrophe is escaped as `'\''`, and every attempt to read that back picked
/// the last quoted fragment instead of the path.
var _inFlight = {};

function rememberPath(command, path) {
    _inFlight[command] = path;
}

/// Returns the path for a finished command and forgets it.
function takePath(command) {
    const path = _inFlight[command];
    delete _inFlight[command];
    return path === undefined ? "" : path;
}

/// The command that asks the CLI how many bills are stored.
///
/// `history --count` opens the database and prints the number alone: no model, no
/// daemon, nothing to load. The widget cannot be told about a wipe directly, since
/// the wipe happens in another process in another terminal, so it asks about the
/// one thing both sides share.
function storedCountCommand(cliPath) {
    return quote(cliPath) + " history --count";
}

/// The stored bill count from that command's stdout.
///
/// Returns -1 when the answer is not a bare number, and that is deliberately not
/// 0: a CLI that is missing, replaced or told to print a usage message must not
/// look like an empty store, or a typo would empty the list on screen.
function parseStoredCount(stdout) {
    const text = String(stdout).trim();
    return /^\d+$/.test(text) ? Number(text) : -1;
}

/// Whether a stored count means the cards on screen are gone.
///
/// Only a plain 0 with something on screen does. An empty store and an empty list
/// have nothing to reconcile, and -1 is "the CLI did not answer with a number",
/// which has to leave the list alone rather than clear it.
function wiped(count, shown) {
    return count === 0 && shown > 0;
}

/// The command that empties the store, the archive and the inbox.
///
/// The same three places the CLI's `--wipe` takes, asked for from the widget
/// instead of from a terminal. It names no files and it is not delegated to a
/// running daemon, so the reply is the three lines the CLI prints rather than any
/// JSON, and the list on screen is reconciled afterwards by asking how many bills
/// are stored.
function wipeCommand(cliPath) {
    return quote(cliPath) + " --wipe";
}

// ------------------------------------------------------------ reading again

/// Whether a bill can be read again: only when the CLI sent its document's path.
///
/// The file name alone is not a path. The process that would do the reading runs
/// in `$HOME`, so a bare name resolves to nothing there, and a bill the CLI
/// reported always carries an absolute path; one without it did not come through
/// this pipeline, and a button that asks for it would fail every time.
function retryable(bill) {
    if (bill === null || bill === undefined)
        return false;
    if (bill.path === undefined || bill.path === null)
        return false;
    return String(bill.path).length > 0;
}

/// Puts a reply into the list, replacing a document that was read before rather
/// than adding a second copy of it.
///
/// A retry is not an append. The cards of the file were the wrong ones, and
/// leaving them next to the new ones would show the same invoice twice, one of
/// them holding the answer the retry was asked to replace. The fresh bills go to
/// the top, which is where a read belongs, and the file's older cards are dropped
/// — all of them, because a file is read as a whole and only one of its pages may
/// have been the bad one.
function mergeBills(bills, fresh, limit) {
    if (!fresh || fresh.length === 0)
        return bills;

    const key = fileKey(fresh[0]);
    const kept = [];
    for (let i = 0; i < bills.length; ++i) {
        if (key.length > 0 && fileKey(bills[i]) === key)
            continue;
        kept.push(bills[i]);
    }

    const merged = fresh.concat(kept);
    return limit > 0 ? merged.slice(0, limit) : merged;
}

/// The bill a detail view should show after its document was read again.
///
/// The reply carries new objects, so the bill on screen would otherwise be a copy
/// that is no longer in the list: it would keep showing the answer that was just
/// replaced, and no later read could reach it. The same page of the same file is
/// the same bill. When that page is not in the answer any more, `null` closes the
/// view instead of leaving a stale one up.
function reopenedBill(open, bills) {
    if (open === null || open === undefined || !bills)
        return null;

    const key = fileKey(open);
    for (let i = 0; i < bills.length; ++i) {
        if (fileKey(bills[i]) === key && bills[i].bill === open.bill)
            return bills[i];
    }
    return null;
}

/// `file.pdf` or `file.pdf, S. 2` when a file holds more than one bill.
function describePlace(bill) {
    const name = bill.file !== undefined ? bill.file : "";
    const page = bill.bill !== undefined ? bill.bill : 1;
    const count = bill.bill_count !== undefined ? bill.bill_count : 1;
    return count > 1 ? (name + ", S. " + page) : name;
}

// ----------------------------------------------------------------- subtotals

/// What identifies the file a bill came from. The path is unique, the file name
/// is not, so the path wins when the CLI sent one.
function fileKey(bill) {
    if (bill.path !== undefined && bill.path !== "")
        return String(bill.path);
    return bill.file !== undefined ? String(bill.file) : "";
}

/// The amount of one bill, or null when there is none. An absent key is
/// `undefined` and a missing total is `null`, and both have to stay out of a
/// sum rather than become zero.
function billAmount(bill) {
    if (bill.gross_total === null || bill.gross_total === undefined)
        return null;
    const amount = Number(bill.gross_total);
    return isNaN(amount) ? null : amount;
}

/// One amount as a bare number, for the desktop's locale: `179,76`.
///
/// No currency code, which is what a form with its own currency field wants.
/// `moneyText` is this with the code appended.
function bareMoneyText(amount, locale) {
    return locale ? Number(amount).toLocaleString(locale, 'f', 2)
                  : Number(amount).toFixed(2);
}

/// One amount, formatted for the desktop's locale: `179,76 EUR`. A missing
/// currency code leaves the bare number rather than a trailing space.
function moneyText(amount, currency, locale) {
    const number = bareMoneyText(amount, locale);
    const code = currency !== undefined && currency !== null ? String(currency) : "";
    return code.length > 0 ? (number + " " + code) : number;
}

/// Formats amounts for the desktop's locale. `179,76 EUR`, and for a file that
/// mixes currencies `179,76 EUR + 12,00 USD`, because adding the two would give
/// a number that is not money.
function formatMoney(parts, locale) {
    const rendered = [];
    for (let i = 0; i < parts.length; ++i)
        rendered.push(moneyText(parts[i].cents / 100, parts[i].currency, locale));
    return rendered.join(" + ");
}

/// Adds up a set of bills, one group per currency, in cents.
///
/// The arithmetic both totals share, so a file sum and a list sum cannot drift
/// apart in the four things that matter: what is skipped, what is counted, what
/// is added to what, and how it is rounded.
///
/// Summed in cents, like the CLI does. Adding the amounts as they arrive gives
/// 0.1 + 0.2 = 0.30000000000000004, and the widget would print the tail of it.
///
/// `counted` is how many bills went into the sum and `failed` how many were left
/// out of it because the CLI could not read them, so a caller can say what the
/// number covers without walking the bills a second time.
function sumAmounts(bills) {
    const parts = [];
    let counted = 0;
    let failed = 0;

    for (let i = 0; i < bills.length; ++i) {
        const bill = bills[i];

        if (bill.status !== "ok") {
            failed += 1;
            continue;
        }

        const amount = billAmount(bill);
        if (amount === null)
            continue;

        const currency = bill.currency !== undefined ? String(bill.currency) : "";

        let slot = null;
        for (let p = 0; p < parts.length; ++p) {
            if (parts[p].currency.toLowerCase() === currency.toLowerCase()) {
                slot = parts[p];
                break;
            }
        }
        if (slot === null) {
            slot = { currency: currency, cents: 0 };
            parts.push(slot);
        }

        slot.cents += Math.round(amount * 100);
        counted += 1;
    }

    return { parts: parts, counted: counted, failed: failed };
}

/// The sum of one set of groups as the two forms a row needs, given what the sum
/// was expected to cover.
///
/// `money` carries the currencies and is what the row shows. `amount` is the same
/// sum as a bare number, for a clipboard that was asked for no currency, and it is
/// empty when the set mixes currencies: `10,00 + 5,00` is not money, so there the
/// copy keeps the codes.
function totalOf(sum, expected, locale) {
    return {
        bills: expected,
        expected: expected,
        counted: sum.counted,
        failed: sum.failed,
        complete: sum.counted > 0 && sum.failed === 0 && sum.counted >= expected,
        money: formatMoney(sum.parts, locale),
        amount: sum.parts.length === 1
            ? bareMoneyText(sum.parts[0].cents / 100, locale)
            : ""
    };
}

/// Adds up one file's bills.
///
/// `expected` comes from the file's own page count, not from the number of bills
/// on screen, so a list that was cut to the history limit can tell that it is
/// showing part of a file.
function totalFor(bills, locale) {
    let expected = 0;

    for (let i = 0; i < bills.length; ++i) {
        const pages = bills[i].bill_count !== undefined ? Number(bills[i].bill_count) : 0;
        if (pages > expected)
            expected = pages;
    }

    if (expected < bills.length)
        expected = bills.length;

    return totalOf(sumAmounts(bills), expected, locale);
}

/// What the whole list adds up to, for the row at the foot of the popup.
///
/// The same sum as a file's, over every bill on screen instead of one file's.
/// The coverage is the number of rows in the list rather than a page count, and
/// that is not the same thing: eight drops of one page each are eight bills, and
/// no document has a page count that could contradict it. So a list is complete
/// when every bill it shows went into the sum, and `failed` counts the ones the
/// CLI could not read.
///
/// The sum follows the list, not the store: a file dropped twice is on screen
/// twice and is in the total twice. That is what the row claims -- a total over
/// all listed positions -- and a number that quietly folded two of them together
/// would disagree with the cards above it.
function listTotalFor(bills, locale) {
    const listed = bills ? bills.length : 0;
    return totalOf(sumAmounts(bills || []), listed, locale);
}

/// How many documents the list holds.
///
/// Runs of bills sharing a file, not distinct keys: the list is newest first, so
/// the same file dropped twice appears as two runs, and a count that folded them
/// together would say one document where two are on screen.
function fileCount(bills) {
    if (!bills || bills.length === 0)
        return 0;

    let count = 1;
    for (let i = 1; i < bills.length; ++i) {
        if (fileKey(bills[i - 1]) !== fileKey(bills[i]))
            count += 1;
    }
    return count;
}

/// Subtotals per file, as an array parallel to `bills`.
///
/// Every entry is `null` except the last bill of a file, which carries that
/// file's total. The list is walked in runs rather than summed card by card,
/// because a total only means something next to the group it covers.
///
/// A run of one gets no total: it would repeat the amount on the card above it.
function subtotalsFor(bills, locale) {
    const marks = [];
    if (!bills) {
        return marks;
    }

    for (let i = 0; i < bills.length; ++i) {
        marks.push(null);
    }

    let run = [];
    const closeRun = function (endIndex) {
        if (run.length > 1)
            marks[endIndex] = totalFor(run, locale);
        run = [];
    };

    for (let i = 0; i < bills.length; ++i) {
        if (run.length > 0 && fileKey(run[run.length - 1]) !== fileKey(bills[i]))
            closeRun(i - 1);
        run.push(bills[i]);
    }
    closeRun(bills.length - 1);

    return marks;
}

// ------------------------------------------------------------------ one bill

/// A `file://` URL for a path, with every segment escaped.
///
/// Real invoices are called `2025-05-Oebb-Rechnung 9864858445.PDF`, so the space
/// is the common case. Unescaped it usually survives, and a `#` in a name does
/// not: it ends the URL early and something else is opened, silently. Built from
/// the path the CLI sent, which is always absolute.
function fileUrl(path) {
    if (path === undefined || path === null)
        return "";

    const text = String(path);
    if (text.length === 0)
        return "";

    const segments = text.split("/");
    for (let i = 0; i < segments.length; ++i)
        segments[i] = encodeURIComponent(segments[i]);
    return "file://" + segments.join("/");
}

/// Escapes the three characters that would otherwise be read as markup.
///
/// Only text content is built from a value here, never an attribute, so the two
/// quote characters are left alone. A vendor called `Müller & Söhne <GmbH>` is
/// what this exists for: unescaped, everything from the `&` on is a different
/// string than the one the CLI sent, and the field shows a mangled vendor with
/// nothing to say why.
function escapeHtml(text) {
    return String(text)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;");
}

/// The entries with the paragraph breaks added as empty entries between groups.
///
/// Twenty `Label: value` lines in a row are a wall, and the group is what the eye
/// uses to find the date and the amount in it. The break is decided here rather
/// than in the QML because the field shows the block and copies the block out, and
/// both have to break the same way — and between two entries that exist, so a
/// group with nothing in it leaves no stray blank line behind.
function withBreaks(entries) {
    const out = [];
    for (let i = 0; i < entries.length; ++i) {
        if (i > 0 && entries[i].group !== entries[i - 1].group)
            out.push({ text: "", group: entries[i].group, strong: false });
        out.push(entries[i]);
    }
    return out;
}

/// Every value the CLI reported for one bill, as `{ text, group, strong }`.
///
/// `group` is the paragraph a line belongs to, `strong` marks the two lines the
/// detail view draws in bold: the date and the amount, which are what a bill is
/// copied into a bookkeeping program for.
///
/// Three rules: an absent or empty value is left out rather than printed as `-`,
/// the order is fixed so the same bill always reads the same way, and a key this
/// function does not know yet is appended at the end, so a field added to the CLI
/// turns up here without a change.
///
/// The labels are literals and not `i18n` calls: this is a `.pragma library`
/// file, and Plasma injects `i18n` into QML files only. Having the text here is
/// what makes it testable at all, and a label nobody translated is still better
/// than a block nobody can check.
function billLines(bill, locale) {
    if (!bill)
        return [];

    // What this is and whether it worked, then who issued it and for how much,
    // then what only a machine cares about.
    const head = 0;
    const facts = 1;
    const rest = 2;

    const entries = [];
    const put = function (group, text, strong) {
        entries.push({ text: text, group: group, strong: strong === true });
    };
    const add = function (group, label, value, strong) {
        if (value === undefined || value === null)
            return;
        const text = String(value);
        if (text.length === 0)
            return;
        put(group, label + ": " + text, strong);
    };
    const addAmount = function (group, label, value, strong) {
        if (value === null || value === undefined)
            return;
        add(group, label, moneyText(value, bill.currency, locale), strong);
    };
    const yesNo = function (value) {
        if (value === undefined || value === null)
            return null;
        return value ? "ja" : "nein";
    };
    const milliseconds = function (value) {
        if (value === undefined || value === null)
            return null;
        return value + " ms";
    };

    add(head, "Datei", describePlace(bill));
    if (bill.path !== undefined && bill.path !== "" && bill.path !== bill.file)
        add(head, "Pfad", bill.path);
    add(head, "Status", bill.status);
    add(head, "Fehler", bill.error);
    add(head, "Hinweis", bill.quality_warning);

    add(facts, "Aussteller", bill.vendor);
    add(facts, "Adresse", bill.vendor_address);
    add(facts, "Rechnungsnummer", bill.invoice_number);
    add(facts, "Datum", bill.date, true);
    add(facts, "Fällig", bill.due_date);
    addAmount(facts, "Netto", bill.net_total);
    addAmount(facts, "Steuer", bill.tax_total);
    addAmount(facts, "Betrag", bill.gross_total, true);

    add(rest, "IBAN", bill.iban);
    add(rest, "Konfidenz", bill.confidence);
    add(rest, "Textlayer", yesNo(bill.has_text_layer));
    add(rest, "Aus dem Cache", yesNo(bill.from_cache));
    add(rest, "Extrahiert", milliseconds(bill.extract_ms));
    add(rest, "Modell", milliseconds(bill.inference_ms));

    const notes = bill.notes;
    if (notes !== undefined && notes !== null && notes.length > 0) {
        for (let i = 0; i < notes.length; ++i)
            put(rest, "Notiz: " + notes[i]);
    }

    // Whatever the CLI added that this function does not know. One line per key,
    // sorted, so an unknown field is visible rather than dropped.
    const known = [
        "file", "path", "bill", "bill_count", "status", "error", "quality_warning",
        "vendor", "vendor_address", "invoice_number", "date", "due_date",
        "currency", "net_total", "tax_total", "gross_total", "iban", "confidence",
        "has_text_layer", "from_cache", "extract_ms", "inference_ms", "notes"
    ];
    const unknown = [];
    for (const key in bill) {
        if (known.indexOf(key) !== -1)
            continue;
        const value = bill[key];
        if (value === undefined || value === null || value === "")
            continue;
        unknown.push(key + ": " + (typeof value === "object" ? JSON.stringify(value)
                                                             : String(value)));
    }
    unknown.sort();
    for (let i = 0; i < unknown.length; ++i)
        put(rest, unknown[i]);

    return entries;
}

/// The block as plain text, one line per value.
///
/// What a selection out of the detail field becomes when it is copied, and what
/// the tests compare against: the amount, the invoice number or the whole block
/// can be taken out by hand, which is what one does with an invoice, and rarely
/// with the four fields a card has room for.
function billText(bill, locale) {
    const entries = withBreaks(billLines(bill, locale));
    const lines = [];
    for (let i = 0; i < entries.length; ++i)
        lines.push(entries[i].text);
    return lines.join("\n");
}

/// The same block as markup: the same lines, the same breaks, the two `strong`
/// ones in bold.
///
/// Joined with `<br>` and not wrapped in paragraphs, because the plain text of a
/// paragraph block has no empty line in it. A selection copied out of the rich
/// field would then lose the break the field shows, which is the half of this
/// that matters when the block is pasted into a form.
///
/// Bold is the whole of the emphasis: a colour would have to be a literal in here,
/// and the theme's colour is not knowable from a `.pragma library` file.
function billHtml(bill, locale) {
    const entries = withBreaks(billLines(bill, locale));
    const parts = [];
    for (let i = 0; i < entries.length; ++i) {
        const text = escapeHtml(entries[i].text);
        parts.push(entries[i].strong ? ("<b>" + text + "</b>") : text);
    }
    return parts.join("<br>");
}
