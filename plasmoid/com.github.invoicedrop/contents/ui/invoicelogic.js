.pragma library

// Pure helpers, kept out of the QML so they can be exercised on their own. The
// widget is a drop target and a list; everything with a decision in it lives
// here.

/// Wraps a value in single quotes for the shell, escaping an embedded quote.
function quote(value) {
    return "'" + String(value).replace(/'/g, "'\\''") + "'";
}

/// Builds the command line the widget runs.
///
/// The CLI answers in JSON, one object per bill per line, and hands the work to
/// a running daemon by itself, so the same command is fast when a daemon is up
/// and correct when none is.
function buildCommand(cliPath, model, archiveAfterReading, path) {
    const parts = [quote(cliPath), "--json"];
    if (model)
        parts.push("--model", quote(model));
    if (archiveAfterReading)
        parts.push("--move");
    parts.push(quote(path));
    return parts.join(" ");
}

/// Parses the reply: one JSON object per line, one line per bill.
///
/// Returns `{ bills: [...], broken: n }`, where `broken` counts lines that did
/// not parse. A silent gap in the list would be worse than a visible one, so the
/// count is handed back rather than swallowed.
function parseBills(stdout) {
    const bills = [];
    let broken = 0;

    const lines = String(stdout).split("\n");
    for (let i = 0; i < lines.length; ++i) {
        const line = lines[i].trim();
        if (line.length === 0)
            continue;
        try {
            bills.push(JSON.parse(line));
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

/// Formats amounts for the desktop's locale. `179,76 EUR`, and for a file that
/// mixes currencies `179,76 EUR + 12,00 USD`, because adding the two would give
/// a number that is not money.
function formatMoney(parts, locale) {
    const rendered = [];
    for (let i = 0; i < parts.length; ++i) {
        const amount = parts[i].cents / 100;
        const number = locale ? Number(amount).toLocaleString(locale, 'f', 2)
                              : amount.toFixed(2);
        rendered.push(parts[i].currency.length > 0 ? (number + " " + parts[i].currency)
                                                   : number);
    }
    return rendered.join(" + ");
}

/// Adds up one file's bills.
///
/// Summed in cents, like the CLI does. Adding the amounts as they arrive gives
/// 0.1 + 0.2 = 0.30000000000000004, and the widget would print the tail of it.
///
/// `expected` comes from the file's own page count, not from the number of bills
/// on screen, so a list that was cut to the history limit can tell that it is
/// showing part of a file.
function totalFor(bills, locale) {
    const parts = [];
    let expected = 0;
    let counted = 0;
    let failed = 0;

    for (let i = 0; i < bills.length; ++i) {
        const bill = bills[i];

        const pages = bill.bill_count !== undefined ? Number(bill.bill_count) : 0;
        if (pages > expected)
            expected = pages;

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

    if (expected < bills.length)
        expected = bills.length;

    return {
        bills: bills.length,
        expected: expected,
        counted: counted,
        failed: failed,
        complete: counted > 0 && failed === 0 && counted >= expected,
        money: formatMoney(parts, locale)
    };
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
