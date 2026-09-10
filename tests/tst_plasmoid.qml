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

    Component.onCompleted: {
        // ---------------------------------------------------------- quoting
        check("plain path", Logic.quote("/tmp/a.pdf"), "'/tmp/a.pdf'");
        check("path with a space", Logic.quote("/tmp/mein beleg.pdf"), "'/tmp/mein beleg.pdf'");
        check("path with a quote",
              Logic.quote("/tmp/it's.pdf"),
              "'/tmp/it'\\''s.pdf'");

        // ------------------------------------------------------- the command
        check("command without a model",
              Logic.buildCommand("invoicedrop", "", false, "/tmp/a.pdf"),
              "'invoicedrop' --json '/tmp/a.pdf'");

        check("command with a model and a space in the path",
              Logic.buildCommand("/opt/invoicedrop", "gemma4:latest", false, "/tmp/mein beleg.pdf"),
              "'/opt/invoicedrop' --json --model 'gemma4:latest' '/tmp/mein beleg.pdf'");

        check("command with archiving",
              Logic.buildCommand("invoicedrop", "", true, "/tmp/a.pdf"),
              "'invoicedrop' --json --move '/tmp/a.pdf'");

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

        const quoted = Logic.buildCommand("invoicedrop", "", false, "/tmp/it's.pdf");
        Logic.rememberPath(quoted, "/tmp/it's.pdf");
        check("path with an apostrophe survives", Logic.takePath(quoted), "/tmp/it's.pdf");

        // ------------------------------------------------------------ labels
        check("single page place",
              Logic.describePlace({ file: "a.pdf", bill: 1, bill_count: 1 }), "a.pdf");
        check("second of three",
              Logic.describePlace({ file: "a.pdf", bill: 2, bill_count: 3 }),
              "a.pdf, S. 2");

        console.log(harness.failures === 0 ? "ALL PASSED"
                                           : (harness.failures + " CHECK(S) FAILED"));
        Qt.callLater(Qt.quit);
    }
}
