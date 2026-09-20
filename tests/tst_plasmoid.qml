import QtQuick
import "../plasmoid/com.github.invoicedrop/contents/ui/invoicelogic.js" as Logic

// Checks the widget's decision logic. The drag itself cannot be automated, but
// the command that a drop builds and the reply that comes back can be, and those
// are where the mistakes live.
//
// Run with: qmlscene6 -platform offscreen tests/tst_plasmoid.qml
//
// Qt.exit() is ignored by qmlscene, so the result is reported in the output and
// CTest decides from that via PASS_REGULAR_EXPRESSION and FAIL_REGULAR_EXPRESSION.
Item {
    id: harness

    property int failures: 0

    function check(name, actual, expected) {
        if (JSON.stringify(actual) === JSON.stringify(expected)) {
            console.log("PASS  " + name);
            return;
        }
        console.log("FAIL  " + name + "\n        expected " + JSON.stringify(expected)
                    + "\n        actual   " + JSON.stringify(actual));
        harness.failures += 1;
    }

    /// One reply object as the CLI writes it, for the subtotal cases.
    function bill(path, page, pageCount, amount) {
        return {
            path: path,
            file: path,
            bill: page,
            bill_count: pageCount,
            status: "ok",
            gross_total: amount,
            currency: "EUR"
        };
    }

    Component.onCompleted: {
        // ---------------------------------------------------------- quoting
        check("plain path", Logic.quote("/tmp/a.pdf"), "'/tmp/a.pdf'");
        check("path with a space", Logic.quote("/tmp/mein beleg.pdf"), "'/tmp/mein beleg.pdf'");
        check("path with a quote",
              Logic.quote("/tmp/it's.pdf"),
              "'/tmp/it'\\''s.pdf'");

        // ------------------------------------------------------- the command
        check("command without a model",
              Logic.buildCommand("invoicedrop", "", false, true, "/tmp/a.pdf"),
              "'invoicedrop' --json '/tmp/a.pdf'");

        check("command with a model and a space in the path",
              Logic.buildCommand("/opt/invoicedrop", "gemma4:latest", false, true,
                                 "/tmp/mein beleg.pdf"),
              "'/opt/invoicedrop' --json --model 'gemma4:latest' '/tmp/mein beleg.pdf'");

        check("command with archiving",
              Logic.buildCommand("invoicedrop", "", true, true, "/tmp/a.pdf"),
              "'invoicedrop' --json --move '/tmp/a.pdf'");

        check("command with notifications off",
              Logic.buildCommand("invoicedrop", "", false, false, "/tmp/a.pdf"),
              "'invoicedrop' --json --no-notify '/tmp/a.pdf'");

        check("archiving and silence together",
              Logic.buildCommand("invoicedrop", "", true, false, "/tmp/a.pdf"),
              "'invoicedrop' --json --move --no-notify '/tmp/a.pdf'");

        // ---------------------------------------------------- reading a reply
        const one = Logic.parseBills('{"bill":1,"vendor":"HOFER","gross_total":11.91}\n');
        check("one bill parsed", one.bills.length, 1);
        check("nothing broken", one.broken, 0);
        check("the amount survives", one.bills[0].gross_total, 11.91);

        const three = Logic.parseBills(
            '{"bill":1,"gross_total":1}\n{"bill":2,"gross_total":2}\n{"bill":3,"gross_total":3}\n');
        check("three lines become three bills", three.bills.length, 3);
        check("in order", [three.bills[0].bill, three.bills[2].bill], [1, 3]);

        const blank = Logic.parseBills("\n\n  \n");
        check("blank output is no bill", blank.bills.length, 0);
        check("blank output is not broken", blank.broken, 0);

        const mixed = Logic.parseBills('{"bill":1}\nnot json\n{"bill":2}\n');
        check("unparsable lines are counted, not dropped", mixed.broken, 1);

        // A line that parses into something that is not an object is not a bill.
        // The store check answers with a bare number through the same data source,
        // and a number that reached the list would be a card with nothing in it.
        check("a bare number is not a bill", Logic.parseBills("0\n7\n").bills.length, 0);
        check("a bare number is counted as broken", Logic.parseBills("0\n").broken, 1);
        check("a bare string is not a bill", Logic.parseBills('"hello"').broken, 1);
        check("null is not a bill", Logic.parseBills("null").broken, 1);
        check("an array is not a bill", Logic.parseBills("[1,2]").broken, 1);
        check("the bills around it still arrive",
              Logic.parseBills('{"bill":1}\n0\n{"bill":2}\n').bills.length, 2);
        check("the parsable ones survive", mixed.bills.length, 2);

        // ------------------------------------------------- recovering the file
        // Recovering the path by parsing the command back out was tried and is
        // wrong, because an apostrophe is escaped and the last quoted fragment
        // wins. The path is remembered instead.
        Logic.rememberPath("'invoicedrop' --json '/tmp/mein beleg.pdf'",
                           "/tmp/mein beleg.pdf");
        check("file for a command",
              Logic.takePath("'invoicedrop' --json '/tmp/mein beleg.pdf'"),
              "/tmp/mein beleg.pdf");
        check("taken once only",
              Logic.takePath("'invoicedrop' --json '/tmp/mein beleg.pdf'"), "");

        const quoted = Logic.buildCommand("invoicedrop", "", false, true, "/tmp/it's.pdf");
        Logic.rememberPath(quoted, "/tmp/it's.pdf");
        check("path with an apostrophe survives", Logic.takePath(quoted), "/tmp/it's.pdf");

        // ------------------------------------------------------------ labels
        check("single page place",
              Logic.describePlace({ file: "a.pdf", bill: 1, bill_count: 1 }), "a.pdf");
        check("second of three",
              Logic.describePlace({ file: "a.pdf", bill: 2, bill_count: 3 }),
              "a.pdf, S. 2");

        // --------------------------------------------------------- subtotals
        // de_DE is pinned rather than taken from the environment, so the
        // separators in these strings are the same wherever the suite runs.
        const german = Qt.locale("de_DE");

        const tanken = [
            bill("tanken.pdf", 1, 3, 65.50),
            bill("tanken.pdf", 2, 3, 90.91),
            bill("tanken.pdf", 3, 3, 21.35)
        ];
        const tankenSums = Logic.subtotalsFor(tanken, german);
        check("a total only at the last bill of the file",
              [tankenSums[0], tankenSums[1], tankenSums[2] !== null], [null, null, true]);
        check("the amounts add up",
              tankenSums[2].money, "177,76 EUR");
        check("the file's own page count is the coverage",
              [tankenSums[2].counted, tankenSums[2].expected], [3, 3]);
        check("a full sum is complete", tankenSums[2].complete, true);

        // Two files in one list, each with its own total.
        const both = Logic.subtotalsFor([
            bill("oebb.pdf", 1, 1, 80.64),
            bill("eintritte.pdf", 1, 2, 22.40),
            bill("eintritte.pdf", 2, 2, 41.60)
        ], german);
        check("a one bill file gets no total row",
              [both[0], both[1]], [null, null]);
        check("the second file is totalled",
              both[2].money, "64,00 EUR");

        // The history limit can cut a file in half. The sum then has to say what
        // it covers instead of looking like a full one.
        const cut = Logic.subtotalsFor([
            bill("tanken.pdf", 1, 3, 65.50),
            bill("tanken.pdf", 2, 3, 90.91)
        ], german);
        check("a cut file is not complete", cut[1].complete, false);
        check("a cut file reports its coverage",
              [cut[1].counted, cut[1].expected, cut[1].money], [2, 3, "156,41 EUR"]);

        const broken = Logic.subtotalsFor([
            bill("mixed.pdf", 1, 3, 10.00),
            { path: "mixed.pdf", file: "mixed.pdf", bill: 2, bill_count: 3, status: "error" },
            bill("mixed.pdf", 3, 3, 5.50)
        ], german);
        check("a failed bill is counted, not summed",
              [broken[2].counted, broken[2].failed, broken[2].complete], [2, 1, false]);
        check("the sum skips the bill it could not read",
              broken[2].money, "15,50 EUR");

        // Cents, not floats: 0.1 + 0.2 is 0.30000000000000004 in binary.
        const thirds = Logic.subtotalsFor([
            bill("klein.pdf", 1, 2, 0.10),
            bill("klein.pdf", 2, 2, 0.20)
        ], german);
        check("cents stay exact", thirds[1].money, "0,30 EUR");

        const mixedCurrency = Logic.subtotalsFor([
            { path: "urlaub.pdf", file: "urlaub.pdf", bill: 1, bill_count: 2,
              status: "ok", gross_total: 10, currency: "EUR" },
            { path: "urlaub.pdf", file: "urlaub.pdf", bill: 2, bill_count: 2,
              status: "ok", gross_total: 5, currency: "USD" }
        ], german);
        check("currencies are never added together",
              mixedCurrency[1].money, "10,00 EUR + 5,00 USD");

        const noAmount = Logic.subtotalsFor([
            { path: "leer.pdf", file: "leer.pdf", bill: 1, bill_count: 2, status: "ok" },
            { path: "leer.pdf", file: "leer.pdf", bill: 2, bill_count: 2, status: "ok" }
        ], german);
        check("a sum with nothing in it is not complete", noAmount[1].complete, false);
        check("a sum with nothing in it is empty", noAmount[1].money, "");

        // The same sum as a bare number, which is what a copy puts on the
        // clipboard when the settings ask for no currency. A file that mixes
        // currencies has none to give — `10,00 + 5,00` is not money — so the copy
        // keeps the codes there instead of printing a number that means nothing.
        check("a single currency sum has a bare form", tankenSums[2].amount, "177,76");
        check("a mixed currency sum has no bare form", mixedCurrency[1].amount, "");
        check("a sum with nothing in it has no bare form", noAmount[1].amount, "");

        check("no bills is no marks", Logic.subtotalsFor([], german).length, 0);

        // A run is broken by the file, not by the bill number: the same name on
        // page 1 again is a new drop of the same file.
        const twice = Logic.subtotalsFor([
            bill("a.pdf", 1, 1, 1.00),
            bill("b.pdf", 1, 1, 2.00),
            bill("a.pdf", 1, 1, 3.00)
        ], german);
        check("the same file twice is two runs",
              [twice[0], twice[1], twice[2]], [null, null, null]);

        // --------------------------------------------------------- the list sum
        // The row at the foot of the popup. It is about the whole list rather
        // than one file, so its coverage is the number of rows on screen and not
        // a page count: eight drops of one page each are eight bills, and no
        // document has a page count that could contradict that.
        const everyListed = Logic.listTotalFor(tanken.concat([bill("oebb.pdf", 1, 1, 80.64)]),
                                               german);
        check("the list sum crosses files", everyListed.money, "258,40 EUR");
        check("the coverage is the number of rows",
              [everyListed.counted, everyListed.expected], [4, 4]);
        check("a list that was read in full is complete", everyListed.complete, true);
        check("the list sum has a bare form", everyListed.amount, "258,40");

        const singleRow = Logic.listTotalFor([bill("a.pdf", 1, 1, 11.91)], german);
        check("one listed bill still sums", singleRow.money, "11,91 EUR");

        // One document on screen is the case the row is hidden for: the file's
        // own row says the same number, and `fileCount` is what decides that.
        check("one document is one run", Logic.fileCount(tanken), 1);
        check("two documents are two runs",
              Logic.fileCount([bill("a.pdf", 1, 1, 1), bill("b.pdf", 1, 1, 2)]), 2);
        check("the same file twice is two runs on screen, not one",
              Logic.fileCount([bill("a.pdf", 1, 1, 1), bill("b.pdf", 1, 1, 2),
                               bill("a.pdf", 1, 1, 3)]), 3);
        check("an empty list holds no document", Logic.fileCount([]), 0);

        const oneUnread = Logic.listTotalFor([
            bill("a.pdf", 1, 2, 10.00),
            { path: "a.pdf", file: "a.pdf", bill: 2, bill_count: 2, status: "error" }
        ], german);
        check("a listed bill that was not read is counted as such",
              [oneUnread.counted, oneUnread.failed, oneUnread.complete], [1, 1, false]);
        check("and it is left out of the sum", oneUnread.money, "10,00 EUR");
        check("the coverage is still the whole list", oneUnread.expected, 2);

        const mixedList = Logic.listTotalFor([
            { path: "a.pdf", file: "a.pdf", bill: 1, bill_count: 1, status: "ok",
              gross_total: 10, currency: "EUR" },
            { path: "b.pdf", file: "b.pdf", bill: 1, bill_count: 1, status: "ok",
              gross_total: 5, currency: "USD" }
        ], german);
        check("the list never adds two currencies together",
              mixedList.money, "10,00 EUR + 5,00 USD");
        check("and has no bare form", mixedList.amount, "");

        check("an empty list sums to nothing",
              [Logic.listTotalFor([], german).counted,
               Logic.listTotalFor([], german).complete], [0, false]);
        check("an empty list covers nothing", Logic.listTotalFor([], german).expected, 0);
        check("an absent list is not an error", Logic.listTotalFor(null, german).money, "");

        // ------------------------------------------------------------ one bill
        check("an amount for the desktop locale",
              Logic.moneyText(11.91, "EUR", german), "11,91 EUR");
        check("an amount without a currency",
              Logic.moneyText(11.91, "", german), "11,91");
        // The same number without the code, for the settings that ask a copy for
        // the number alone.
        check("a bare amount for the desktop locale",
              Logic.bareMoneyText(21.35, german), "21,35");
        check("a bare amount rounds like the labelled one",
              Logic.bareMoneyText(179.764, german), "179,76");

        check("a space in a path becomes a URL",
              Logic.fileUrl("/tmp/mein beleg.pdf"), "file:///tmp/mein%20beleg.pdf");
        check("a hash does not end the URL early",
              Logic.fileUrl("/tmp/a#b.pdf"), "file:///tmp/a%23b.pdf");
        check("an empty path makes no URL", Logic.fileUrl(""), "");
        check("an absent path makes no URL", Logic.fileUrl(undefined), "");

        const receipt = {
            path: "/tmp/belege/tanken.pdf",
            file: "tanken.pdf",
            bill: 1,
            bill_count: 1,
            status: "ok",
            gross_total: 21.35,
            net_total: null,
            currency: "EUR",
            vendor: "Tank Roth GmbH",
            invoice_number: "1702012570",
            date: "2022-07-27",
            due_date: "",
            iban: "",
            has_text_layer: false,
            extract_ms: 12,
            notes: ["answered from the cache", "second note"]
        };

        const lines = Logic.billText(receipt, german).split("\n");
        check("the file line comes first", lines[0], "Datei: tanken.pdf");
        check("the path is shown when it adds something", lines[1],
              "Pfad: /tmp/belege/tanken.pdf");
        check("the vendor is labelled and there",
              lines.indexOf("Aussteller: Tank Roth GmbH") > 1, true);
        check("the amount is formatted for the locale",
              lines.indexOf("Betrag: 21,35 EUR") > 1, true);
        check("an empty value gets no line at all", lines.indexOf("IBAN: "), -1);
        check("a null value gets no line at all", lines.indexOf("Netto: "), -1);
        check("a false flag is shown rather than dropped",
              lines.indexOf("Textlayer: nein") > 1, true);
        check("every note gets its own line",
              lines.indexOf("Notiz: second note") > 1, true);
        check("one line per note, not a joined one",
              lines.filter(function (line) { return line.indexOf("Notiz:") === 0; }).length, 2);

        // A key the widget has never heard of is shown, not dropped. That is the
        // whole reason this block is built from the reply instead of from a list
        // of fields somebody has to remember to extend.
        const extended = {
            path: "/tmp/a.pdf", file: "a.pdf", bill: 1, bill_count: 1,
            status: "ok", gross_total: 1, currency: "EUR",
            some_new_field: "hello"
        };
        check("an unknown field still turns up",
              Logic.billText(extended, german).split("\n")
                   .indexOf("some_new_field: hello") > 0, true);

        const failedBill = {
            path: "/tmp/a.pdf", file: "a.pdf", bill: 1, bill_count: 1,
            status: "error", error: "the model did not answer",
            gross_total: null, currency: "EUR"
        };
        const failedLines = Logic.billText(failedBill, german).split("\n");
        check("a failure says so",
              failedLines.indexOf("Fehler: the model did not answer") > 0, true);
        check("no amount, no amount line", failedLines.indexOf("Betrag: -"), -1);

        check("nothing to show, nothing shown", Logic.billText(null, german), "");

        // --------------------------------------------------- one block, marked up
        // The block is grouped into paragraphs, and the group holding the date and
        // the amount is the one an eye has to find in it.
        check("the facts are their own paragraph",
              [lines[lines.indexOf("Aussteller: Tank Roth GmbH") - 1],
               lines[lines.indexOf("Betrag: 21,35 EUR") + 1]], ["", ""]);
        check("the paragraph starts after the file block",
              lines[lines.indexOf("Aussteller: Tank Roth GmbH") - 2], "Status: ok");
        check("and the technical tail follows it",
              lines[lines.indexOf("Betrag: 21,35 EUR") + 2], "Textlayer: nein");
        check("no blank line before the first line", lines[0], "Datei: tanken.pdf");

        // A bill with neither a date nor an amount is one paragraph. A break that
        // is decided by the group number alone would leave two stray empty lines
        // where the missing group was.
        check("an absent group leaves no blank line",
              Logic.billText(failedBill, german).indexOf("\n\n"), -1);

        const html = Logic.billHtml(receipt, german);
        check("the date is pulled out of the markup",
              html.indexOf("<b>Datum: 2022-07-27</b>") > 0, true);
        check("so is the amount",
              html.indexOf("<b>Betrag: 21,35 EUR</b>") > 0, true);
        check("and nothing else is",
              html.match(/<b>/g).length, 2);
        // A break, not a paragraph tag: the plain text of a paragraph block has no
        // empty line in it, and a selection copied out of the field would lose the
        // break the field shows.
        check("the paragraph break is a line break",
              html.indexOf("<br><br>") > 0, true);
        check("the markup says exactly what the text says",
              html.replace(/<br>/g, "\n").replace(/<\/?b>/g, ""),
              Logic.billText(receipt, german));
        check("nothing to show, no markup shown", Logic.billHtml(null, german), "");

        // A value is data, not markup. A vendor with a `&` or a `<` in it is what
        // this exists for: unescaped, the field shows a string the CLI never sent.
        check("ampersand first, then the brackets",
              Logic.escapeHtml("<a & b>"), "&lt;a &amp; b&gt;");

        const sharp = {
            path: "/tmp/a.pdf", file: "a.pdf", bill: 1, bill_count: 1, status: "ok",
            vendor: "Müller & Söhne <GmbH>", gross_total: 1, currency: "EUR"
        };
        check("the plain block keeps the vendor as it was read",
              Logic.billText(sharp, german)
                   .indexOf("Aussteller: Müller & Söhne <GmbH>") > 0, true);
        check("the markup escapes it instead of reading it",
              Logic.billHtml(sharp, german)
                   .indexOf("Aussteller: Müller &amp; Söhne &lt;GmbH&gt;") > 0, true);

        // ------------------------------------------------------- the store check
        // The list on screen is built from drops and lives in this shell only, so
        // a `--wipe` in a terminal cannot reach it. The stored count is what tells
        // the widget its cards are gone.
        check("the count command",
              Logic.storedCountCommand("invoicedrop"), "'invoicedrop' history --count");
        check("a path with a space is quoted like everywhere else",
              Logic.storedCountCommand("/opt/my tools/invoicedrop"),
              "'/opt/my tools/invoicedrop' history --count");

        check("nothing stored is zero", Logic.parseStoredCount("0\n"), 0);
        check("the number with whitespace around it", Logic.parseStoredCount("  7  "), 7);

        // Anything that is not a bare number is -1, and -1 never clears the list.
        // A missing binary, a usage message and the human listing all have to look
        // like "no answer" rather than like an empty store.
        check("no answer is not a wipe", Logic.parseStoredCount(""), -1);
        check("whitespace is not a wipe", Logic.parseStoredCount("   \n"), -1);
        check("the human listing is not a count",
              Logic.parseStoredCount("1 of 3 stored bill(s), database /tmp/x.db"), -1);
        check("an empty store sentence is not a count",
              Logic.parseStoredCount("nothing stored yet in /tmp/x.db"), -1);
        check("a negative number is not a count", Logic.parseStoredCount("-1"), -1);
        check("a count with a unit is not a count", Logic.parseStoredCount("3 bills"), -1);

        // What the count means for the list on screen.
        check("an empty store with cards up clears them", Logic.wiped(0, 3), true);
        check("an empty store with nothing up changes nothing", Logic.wiped(0, 0), false);
        check("bills still stored leave the list alone", Logic.wiped(7, 3), false);
        check("and so does an answer that is not a number", Logic.wiped(-1, 3), false);

        // -------------------------------------------------------- reading again
        // The clear button runs the `--wipe` a terminal runs, and it is not handed
        // to a daemon: the reply is text rather than bills, so it is routed on the
        // exact command string, the way the store check is.
        check("the wipe command",
              Logic.wipeCommand("invoicedrop"), "'invoicedrop' --wipe");
        check("a path with a space is quoted there too",
              Logic.wipeCommand("/opt/my tools/invoicedrop"),
              "'/opt/my tools/invoicedrop' --wipe");

        // A retry reads the document instead of taking the stored answer back: that
        // answer is the one it was asked to replace.
        check("the retry command",
              Logic.retryCommand("invoicedrop", "", true, "/tmp/a.pdf"),
              "'invoicedrop' --json --no-cache '/tmp/a.pdf'");
        check("a retry keeps the model and the silence",
              Logic.retryCommand("/opt/invoicedrop", "gemma4:latest", false,
                                 "/tmp/mein beleg.pdf"),
              "'/opt/invoicedrop' --json --no-cache --model 'gemma4:latest'"
              + " --no-notify '/tmp/mein beleg.pdf'");
        // And it does not ask for `--move`: after a first read with archiving on,
        // the original is in the archive, so a retry that moved it again would fail
        // on a document that is not there any more.
        check("a retry never moves the document",
              Logic.retryCommand("invoicedrop", "", true, "/tmp/a.pdf").indexOf("--move"), -1);
        check("a retry is not the command a drop builds",
              Logic.retryCommand("invoicedrop", "", true, "/tmp/a.pdf")
              === Logic.buildCommand("invoicedrop", "", true, true, "/tmp/a.pdf"), false);

        // Nothing without a path. The file name alone is not one, and the process
        // that would do the reading works in `$HOME`.
        check("a path can be read again", Logic.retryable({ path: "/tmp/a.pdf" }), true);
        check("a file name alone cannot", Logic.retryable({ file: "a.pdf" }), false);
        check("an empty path cannot", Logic.retryable({ path: "" }), false);
        check("no bill at all cannot", Logic.retryable(null), false);

        // A document that is read again replaces its cards. Two copies of one
        // invoice would be that invoice twice, and one of them would be the answer
        // the retry was asked to replace.
        const before = [bill("a.pdf", 1, 1, 1.00), bill("b.pdf", 1, 1, 2.00)];
        const replaced = Logic.mergeBills(before, [bill("b.pdf", 1, 1, 9.00)], 8);
        check("the file is not in the list twice",
              [replaced.length, replaced[0].gross_total, replaced[1].gross_total],
              [2, 9.00, 1.00]);
        check("the file that was not read again keeps its place", replaced[1].path, "a.pdf");

        // The whole file is replaced, not the page that was clicked: one page may
        // have been the reason, but the document is what is read.
        const pages = [bill("a.pdf", 1, 3, 1.00), bill("a.pdf", 2, 3, 2.00),
                       bill("a.pdf", 3, 3, 3.00), bill("c.pdf", 1, 1, 4.00)];
        const fresh = [bill("a.pdf", 1, 3, 7.00), bill("a.pdf", 2, 3, 8.00),
                       bill("a.pdf", 3, 3, 9.00)];
        const merged = Logic.mergeBills(pages, fresh, 8);
        check("every page of the file is replaced",
              [merged.length, merged[0].gross_total, merged[3].path], [4, 7.00, "c.pdf"]);
        check("the list keeps its limit", Logic.mergeBills(before, fresh, 3).length, 3);
        check("an answer with no bill is no change", Logic.mergeBills(before, [], 8).length, 2);

        // The detail view follows its page through a re-read: the reply carries new
        // objects, so the bill on screen would otherwise be a copy that no later
        // read could reach.
        check("the view stays on its page",
              Logic.reopenedBill(before[0],
                                 [bill("b.pdf", 1, 1, 2.00), bill("a.pdf", 1, 1, 5.00)])
                  .gross_total, 5.00);
        check("a page that is gone closes the view",
              Logic.reopenedBill(before[0], [bill("b.pdf", 1, 1, 2.00)]), null);
        check("no open bill, nothing to reopen", Logic.reopenedBill(null, before), null);

        // One line for the status row: a failing command explains itself on its
        // first line and fills the rest with what it was doing.
        check("the first line of a message",
              Logic.firstLine("da ist etwas schiefgelaufen\n\nmehr Text"),
              "da ist etwas schiefgelaufen");
        check("a message of whitespace has no line", Logic.firstLine("  \n \n"), "");
        check("an absent message has no line", Logic.firstLine(null), "");

        console.log(harness.failures === 0 ? "ALL PASSED"
                                           : (harness.failures + " CHECK(S) FAILED"));
        Qt.callLater(Qt.quit);
    }
}
