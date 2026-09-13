#include "log.h"

#include <QTextStream>

#include <cstdio>
#include <unistd.h>

namespace InvoiceDrop::Log {
namespace {

/// Tags are padded to this column so the messages line up. Longest stage name in
/// use is `render`, which is `[render]` plus two spaces.
constexpr int kTagWidth = 10;

Level g_level = Level::Off;
Colour g_colour = Colour::Auto;

/// Resolved once per process: whether stderr is a terminal cannot change during a
/// run, and asking on every line would be silly.
bool g_coloursResolved = false;
bool g_colours = false;

QString ansi(int code)
{
    return QStringLiteral("\033[%1m").arg(code);
}

const QString &reset()
{
    static const QString code = ansi(0);
    return code;
}

QString colourFor(Kind kind)
{
    switch (kind) {
    case Kind::Step:
        return ansi(36); // cyan
    case Kind::Model:
        return ansi(35); // magenta
    case Kind::Warn:
        return ansi(33); // yellow
    case Kind::Detail:
        return ansi(90); // bright black, the dim one
    }
    return ansi(36);
}

QString tagFor(const QString &stage)
{
    return QStringLiteral("[%1]").arg(stage);
}

QTextStream &errorStream()
{
    // One stream for the whole process, and flushed after every line. The CLI
    // writes its own errors to the same file descriptor; an unflushed buffer here
    // would let a log line arrive before the error it explains.
    static QTextStream stream(stderr);
    return stream;
}

void write(const QString &text)
{
    QTextStream &stream = errorStream();
    stream << text << Qt::endl;
}

bool resolveColours()
{
    if (!g_coloursResolved) {
        const bool isTerminal = ::isatty(::fileno(stderr)) == 1;
        g_colours = colourWanted(g_colour, isTerminal,
                                 QString::fromLocal8Bit(qgetenv("TERM")),
                                 qEnvironmentVariableIsSet("NO_COLOR"));
        g_coloursResolved = true;
    }
    return g_colours;
}

} // namespace

void setLevel(Level level)
{
    g_level = level;
}

Level level()
{
    return g_level;
}

void setColour(Colour colour)
{
    g_colour = colour;
    // A new choice has to be resolved again: `--no-color` arrives after the first
    // line may already have been written, and the answer must not be cached from
    // before it.
    g_coloursResolved = false;
}

Colour colour()
{
    return g_colour;
}

bool shows(Level wanted)
{
    return static_cast<int>(g_level) >= static_cast<int>(wanted);
}

bool colourWanted(Colour choice, bool isTerminal, const QString &term, bool noColourSet)
{
    if (choice == Colour::Always)
        return true;
    if (choice == Colour::Never)
        return false;

    // The convention, rather than an invention: `NO_COLOR` set to anything at all
    // turns colours off, and a dumb terminal cannot show them.
    if (noColourSet)
        return false;
    if (term.compare(QStringLiteral("dumb"), Qt::CaseInsensitive) == 0)
        return false;
    return isTerminal;
}

QString format(Kind kind, const QString &stage, const QString &text, bool colours)
{
    const QString tag = tagFor(stage);

    // At least one space, whatever the tag's length: a stage name longer than the
    // column would otherwise run straight into its own message.
    const int padding = qMax(1, kTagWidth - tag.size());

    if (!colours)
        return tag + QString(padding, QLatin1Char(' ')) + text;

    // Only the tag is coloured. A message that carries a prompt or a model reply
    // has to stay copyable, and colouring a whole line would also colour the
    // padding, which shows as a stripe in any terminal that paints a background.
    QString head = colourFor(kind) + tag + reset();
    head += QString(padding, QLatin1Char(' '));
    return head + text;
}

QString formatBlock(Kind kind, const QString &stage, const QString &title, const QString &body,
                    bool colours)
{
    QString trimmed = body;
    while (trimmed.endsWith(QLatin1Char('\n')))
        trimmed.chop(1);

    const QString prefix = tagFor(stage).leftJustified(kTagWidth, QLatin1Char(' '))
        + QStringLiteral("| ");

    QStringList lines;
    lines.append(format(kind, stage, title, colours));
    if (trimmed.isEmpty()) {
        lines.append(prefix + QStringLiteral("(empty)"));
    } else {
        const QStringList bodyLines = trimmed.split(QLatin1Char('\n'));
        for (const QString &line : bodyLines)
            lines.append(prefix + line);
    }

    return lines.join(QLatin1Char('\n'));
}

void step(const QString &stage, const QString &text)
{
    if (!shows(Level::Steps))
        return;
    write(format(Kind::Step, stage, text, resolveColours()));
}

void model(const QString &stage, const QString &text)
{
    if (!shows(Level::Steps))
        return;
    write(format(Kind::Model, stage, text, resolveColours()));
}

void warn(const QString &stage, const QString &text)
{
    if (!shows(Level::Steps))
        return;
    write(format(Kind::Warn, stage, text, resolveColours()));
}

void detail(const QString &stage, const QString &text)
{
    if (!shows(Level::Detail))
        return;
    write(format(Kind::Detail, stage, text, resolveColours()));
}

void block(const QString &stage, const QString &title, const QString &body)
{
    if (!shows(Level::Detail))
        return;
    write(formatBlock(Kind::Detail, stage, title, body, resolveColours()));
}

void note(QStringList *notes, const QString &stage, const QString &text)
{
    if (notes)
        notes->append(text);
    step(stage, text);
}

} // namespace InvoiceDrop::Log
