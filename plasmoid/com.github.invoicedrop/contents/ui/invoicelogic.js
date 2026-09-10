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
