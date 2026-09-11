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

        console.log(harness.failures === 0 ? "ALL PASSED"
                                           : (harness.failures + " CHECK(S) FAILED"));
        Qt.callLater(Qt.quit);
    }
}
