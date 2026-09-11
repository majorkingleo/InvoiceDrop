import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents

/**
 * What one file adds up to, drawn under the last bill of that file.
 *
 * A file that holds a single bill gets no row: the total would be the amount
 * already on the card above, and repeating it makes the list longer without
 * making it clearer.
 *
 * The count next to the sum is not decoration. `179,76 EUR` on its own claims
 * nothing; `179,76 EUR  3 Belege` claims that three bills are inside it, and
 * when the file holds more than were read the row says so instead of quietly
 * summing the part it has.
 *
 * Clicking the row copies the sum. The row is one number and one action, and the
 * number is the action: what one does with a total is move it somewhere else.
 */
Item {
    id: sum

    /// The total for the file, or null. An absent key arrives as `undefined`,
    /// which is not `null`, and reading `.complete` off it would abort the card
    /// before it draws. Both are folded into one check here.
    property var total: null

    /// Set for a moment after a copy, and shown in place of the count: the row is
    /// one line high, so the confirmation takes the note's place rather than
    /// getting a line of its own.
    property string notice: ""

    /// From the widget's settings. `withoutCurrency` is what a click puts on the
    /// clipboard, `showCopyNotice` whether the row says so afterwards.
    property bool withoutCurrency: false
    property bool showCopyNotice: true

    readonly property bool hasTotal: sum.total !== null && sum.total !== undefined

    readonly property bool complete: sum.hasTotal && sum.total.complete

    /// The count, and the warning when the count is not the whole file. Written
    /// here rather than in the JavaScript so the phrasing goes through i18n like
    /// every other string in the widget.
    readonly property string note: {
        if (!sum.hasTotal)
            return "";

        if (sum.complete)
            return i18np("%1 Beleg", "%1 Belege", sum.total.counted);

        let text = i18n("%1 von %2 Belegen", sum.total.counted, sum.total.expected);
        if (sum.total.failed > 0)
            text += i18n(", %1 nicht gelesen", sum.total.failed);
        return text;
    }

    readonly property string money: {
        if (!sum.hasTotal || sum.total.money === undefined)
            return "";
        return sum.total.money.length > 0 ? sum.total.money : "—";
    }

    /// What a click copies: the sum as the row shows it, or the bare number when
    /// the settings ask for no currency. A mixed-currency sum has no bare form to
    /// give — `10,00 + 5,00` is not money — so it keeps its codes.
    readonly property string clipText: {
        if (!sum.hasTotal)
            return "";
        if (sum.withoutCurrency && sum.total.amount !== undefined
                && sum.total.amount.length > 0)
            return sum.total.amount;
        return sum.money;
    }

    /// What the row says in its left half: the coverage, or the confirmation
    /// after a copy. There is one place for it and the confirmation wins, because
    /// it is about what just happened and the coverage is about what is there.
    readonly property string caption: sum.notice.length > 0 ? sum.notice : sum.note

    implicitHeight: layout.implicitHeight + Kirigami.Units.smallSpacing * 1.5

    Rectangle {
        anchors.fill: parent
        radius: Kirigami.Units.smallSpacing
        color: Kirigami.Theme.highlightColor
        // The row has no icon of its own and no button, so the tint under the
        // pointer is the only thing that says it can be clicked. Slightly deeper
        // than the card's, because this row is already tinted at rest.
        opacity: mouse.containsMouse ? 0.22 : 0.10

        Behavior on opacity {
            NumberAnimation { duration: Kirigami.Units.shortDuration }
        }
    }

    RowLayout {
        id: layout

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            margins: Kirigami.Units.smallSpacing
        }
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Icon {
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: implicitWidth
            visible: !sum.complete
            source: "dialog-warning"
        }

        PlasmaComponents.Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            opacity: 0.8
            font: Kirigami.Theme.smallFont
            text: sum.caption
        }

        PlasmaComponents.Label {
            font.bold: true
            text: sum.money
        }
    }

    Clipboard {
        id: clipboard
    }

    Timer {
        id: noticeTimer

        interval: 1500
        onTriggered: sum.notice = ""
    }

    /// Puts the sum on the clipboard, as it is shown: the amount with its
    /// currency, in the desktop's format, so that what is pasted is what was read
    /// off the screen. A row with nothing to copy returns false and says nothing,
    /// rather than confirming a copy that did not happen.
    function copyTotal() {
        if (!clipboard.copy(sum.clipText))
            return;
        if (!sum.showCopyNotice)
            return;
        sum.notice = i18n("Kopiert");
        noticeTimer.restart();
    }

    /// Last, so it sits over the row. No `preventStealing`: a drag that starts on
    /// this row still has to scroll the list, and the only thing wanted here is a
    /// click that does not move.
    MouseArea {
        id: mouse

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: sum.clipText.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: sum.copyTotal()
    }
}
