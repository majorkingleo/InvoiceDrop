#include <QtTest>

#include "log.h"

using namespace InvoiceDrop;

/// The logger's decisions, which are three: how much is on, whether a tag is
/// coloured, and how a block is laid out. All three are pure functions, which is
/// the only reason they can be checked here -- what the lines look like on a
/// terminal is not something a test can assert, so the format is asserted instead
/// and the terminal was looked at by hand.
class TestLog : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void offByDefault();
    void levelsAreOrdered();
    void notesAreKeptWhenNothingIsPrinted();

    void tagIsPaddedToAColumn();
    void colourWrapsTheTagOnly();
    void colourFollowsTheTerminal();
    void noColourWinsOverATerminal();

    void blockPrefixesEveryLine();
    void blockWithoutABody();
};

void TestLog::init()
{
    Log::setLevel(Log::Level::Off);
}

void TestLog::cleanup()
{
    Log::setLevel(Log::Level::Off);
    Log::setColour(Log::Colour::Auto);
}

void TestLog::offByDefault()
{
    // The default has to be silence: the widget, the daemon and every other test
    // run the same code, and a stage narration on stderr in any of them is a bug.
    QCOMPARE(Log::level(), Log::Level::Off);
    QVERIFY(!Log::shows(Log::Level::Steps));
    QVERIFY(!Log::shows(Log::Level::Detail));
}

void TestLog::levelsAreOrdered()
{
    Log::setLevel(Log::Level::Steps);
    QVERIFY(Log::shows(Log::Level::Steps));
    QVERIFY(!Log::shows(Log::Level::Detail));

    // Detail is above Steps, not beside it: --debug prints everything --verbose
    // does and then some, which is why one flag implies the other.
    Log::setLevel(Log::Level::Detail);
    QVERIFY(Log::shows(Log::Level::Steps));
    QVERIFY(Log::shows(Log::Level::Detail));
}

void TestLog::notesAreKeptWhenNothingIsPrinted()
{
    // The notes are what the JSON, the store and `history` carry, and they do not
    // depend on whether anyone asked to watch the run.
    QStringList notes;
    Log::note(&notes, "render", QStringLiteral("page 1: downscale failed"));
    QCOMPARE(notes, QStringList { QStringLiteral("page 1: downscale failed") });

    // A null list is allowed: the readers take `QStringList *` and some callers
    // pass nothing, which must not crash for the sake of a log line.
    Log::note(nullptr, "render", QStringLiteral("and this one goes nowhere"));
}

void TestLog::tagIsPaddedToAColumn()
{
    // The messages line up because every tag is padded to the same column.
    QCOMPARE(Log::format(Log::Kind::Step, "scan", "PDF, 2 pages", false),
             QStringLiteral("[scan]    PDF, 2 pages"));
    QCOMPARE(Log::format(Log::Kind::Step, "render", "page 1", false),
             QStringLiteral("[render]  page 1"));

    // A stage name longer than the column pushes the message instead of being cut
    // off, and still gets one space rather than none.
    const QString longTag = Log::format(Log::Kind::Step, "averylongstage", "text", false);
    QVERIFY(longTag.startsWith(QStringLiteral("[averylongstage] text")));
}

void TestLog::colourWrapsTheTagOnly()
{
    const QString plain = Log::format(Log::Kind::Model, "ai", "answer 1", false);
    const QString coloured = Log::format(Log::Kind::Model, "ai", "answer 1", true);

    QVERIFY(!plain.contains(QLatin1Char('\033')));
    QVERIFY(coloured.startsWith(QStringLiteral("\033[35m[ai]\033[0m ")));
    // The message is left alone, because a prompt or a model reply has to stay
    // copyable and a colour would end up in whatever it was pasted into.
    QVERIFY(coloured.endsWith(QStringLiteral("answer 1")));

    // Every kind has its own colour, which is the point of having kinds.
    const QString warn = Log::format(Log::Kind::Warn, "ai", "rejected", true);
    QVERIFY(warn.contains(QStringLiteral("\033[33m[ai]")));
    QVERIFY(warn != coloured);
}

void TestLog::colourFollowsTheTerminal()
{
    const QString term = QStringLiteral("xterm-256color");

    QVERIFY(Log::colourWanted(Log::Colour::Always, false, term, false));

    // Auto is the default, and it asks the terminal: this is the case that makes
    // `invoicedrop --verbose … > log.txt` produce a file with no escape codes.
    QVERIFY(Log::colourWanted(Log::Colour::Auto, true, term, false));
    QVERIFY(!Log::colourWanted(Log::Colour::Auto, false, term, false));
}

void TestLog::noColourWinsOverATerminal()
{
    const QString term = QStringLiteral("xterm-256color");

    // The convention rather than an invention: NO_COLOR set to anything at all
    // turns colours off, and a dumb terminal cannot show them in the first place.
    QVERIFY(!Log::colourWanted(Log::Colour::Auto, true, term, true));
    QVERIFY(!Log::colourWanted(Log::Colour::Auto, true, QStringLiteral("dumb"), false));
    QVERIFY(!Log::colourWanted(Log::Colour::Auto, true, QStringLiteral("DUMB"), false));

    // --no-color beats a terminal, which is the whole reason the flag exists.
    QVERIFY(!Log::colourWanted(Log::Colour::Never, true, term, false));

    // And an explicit Always beats the environment, since it was asked for by
    // name.
    QVERIFY(Log::colourWanted(Log::Colour::Always, false, QStringLiteral("dumb"), true));
}

void TestLog::blockPrefixesEveryLine()
{
    const QString block = Log::formatBlock(Log::Kind::Detail, "ai", "prompt, 12 characters",
                                           QStringLiteral("line one\nline two"), false);

    QCOMPARE(block, QStringLiteral("[ai]      prompt, 12 characters\n"
                                   "[ai]      | line one\n"
                                   "[ai]      | line two"));
}

void TestLog::blockWithoutABody()
{
    // An empty prompt would otherwise draw nothing at all under its title, which
    // reads as a truncated log rather than as an empty string that was sent.
    const QString block =
        Log::formatBlock(Log::Kind::Detail, "ai", "prompt, 0 characters", QString(), false);
    QCOMPARE(block, QStringLiteral("[ai]      prompt, 0 characters\n"
                                   "[ai]      | (empty)"));

    // The trailing newline of a real prompt does not become a stray line.
    const QString trailing =
        Log::formatBlock(Log::Kind::Detail, "ai", "prompt", QStringLiteral("only line\n"), false);
    QCOMPARE(trailing, QStringLiteral("[ai]      prompt\n"
                                      "[ai]      | only line"));
}

QTEST_MAIN(TestLog)

#include "tst_log.moc"
