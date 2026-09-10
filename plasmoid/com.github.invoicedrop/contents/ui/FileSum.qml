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
 */
Item {
    id: sum

    /// The total for the file, or null. An absent key arrives as `undefined`,
    /// which is not `null`, and reading `.complete` off it would abort the card
    /// before it draws. Both are folded into one check here.
    property var total: null

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

    implicitHeight: layout.implicitHeight + Kirigami.Units.smallSpacing * 1.5

    Rectangle {
        anchors.fill: parent
        radius: Kirigami.Units.smallSpacing
        color: Kirigami.Theme.highlightColor
        opacity: 0.10
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
            text: sum.note
        }

        PlasmaComponents.Label {
            font.bold: true
            text: sum.money
        }
    }
}
