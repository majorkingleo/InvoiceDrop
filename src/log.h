#pragma once

#include <QString>
#include <QStringList>

namespace InvoiceDrop::Log {

/// How much of the pipeline is worth telling the user about.
///
/// `Off` is the default, and that is deliberate: the widget, the daemon and every
/// test run through the same code, and none of them wants a stage narration on
/// stderr. The two levels above it are asked for explicitly. `--verbose` is the
/// story of one run -- what the reader decided, what was rasterised, what was
/// handed to the model and how long each step took. `--debug` adds the two things
/// that are actually sent and received, in full.
enum class Level {
    Off,
    Steps,
    Detail,
};

/// Whether escape sequences may be written.
///
/// `Auto` is the default and asks the terminal: a tty gets colours, a pipe or a
/// redirected file does not, and `NO_COLOR` or `TERM=dumb` turn them off on their
/// own. `--no-color` is for the case where a tty is attached and colours are
/// still not wanted.
enum class Colour {
    Auto,
    Always,
    Never,
};

/// Which colour a line's tag is drawn in, and what it means.
enum class Kind {
    Step,   ///< cyan: what the pipeline did
    Model,  ///< magenta: what was asked of the model and what came back
    Warn,   ///< yellow: something was skipped, capped or asked again
    Detail, ///< dim: only with `--debug`
};

void setLevel(Level level);
Level level();

void setColour(Colour colour);
Colour colour();

/// True when lines of this level would be written. Checked before an expensive
/// string is built.
bool shows(Level wanted);

/// The decision behind `Auto`, kept pure so it can be checked without a
/// terminal: on for `Always`, off for `Never`, and for `Auto` only when stderr is
/// a terminal and neither `NO_COLOR` nor `TERM=dumb` says otherwise.
bool colourWanted(Colour choice, bool isTerminal, const QString &term, bool noColourSet);

/// A `[stage]  message` line without the newline. The tag is padded to a fixed
/// column so the messages line up, and coloured when `colours` is true.
QString format(Kind kind, const QString &stage, const QString &text, bool colours);

/// A titled multi line block: the title as a line of its own, then one line per
/// `body` line, each prefixed with the tag and a `|`.
///
/// The point of the prefix is that a prompt or a model reply stays readable and
/// stays copyable: block out the prefix and what is left is what was sent, byte
/// for byte.
QString formatBlock(Kind kind, const QString &stage, const QString &title, const QString &body,
                    bool colours);

/// Writes it, if the level allows. Every line is flushed, so a run that takes
/// seconds shows its progress and cannot be reordered against the CLI's own
/// stderr, which flushes with every `Qt::endl`.
void step(const QString &stage, const QString &text);
void model(const QString &stage, const QString &text);
void warn(const QString &stage, const QString &text);
void detail(const QString &stage, const QString &text);
void block(const QString &stage, const QString &title, const QString &body);

/// Records one step in both places at once: the `notes` list, which the JSON, the
/// store and `history` keep, and the `--verbose` log. One call instead of two, so
/// the note and the line cannot come to say different things.
void note(QStringList *notes, const QString &stage, const QString &text);

} // namespace InvoiceDrop::Log
