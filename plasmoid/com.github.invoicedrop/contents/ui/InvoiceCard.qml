import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import "invoicelogic.js" as Logic

/**
 * One bill. A document with two pages produces two of these, because a bill is a
 * page and a file is not a bill.
 */
Item {
    id: card

    property var bill: ({})
    property bool showDivider: false

    /// The card has room for four fields and an invoice has twenty, so a click
    /// opens the bill in full instead of trying to fit more on the row.
    signal clicked()

    readonly property bool failed: bill.status !== "ok"
    readonly property bool unverified: bill.quality_warning !== undefined

    /// The keys are absent rather than empty when there is nothing to say, and
    /// an absent key is `undefined`, which a text property cannot take. Both are
    /// folded into one string here.
    readonly property string problemText: failed
        ? (bill.error !== undefined ? bill.error : "")
        : (bill.quality_warning !== undefined ? bill.quality_warning : "")

    implicitHeight: layout.implicitHeight + Kirigami.Units.smallSpacing * 2

    /// Behind the text, so a hover tints the row and not the label. This is the
    /// only hint that the row can be clicked, so it is worth the animation.
    Rectangle {
        anchors.fill: parent
        color: Kirigami.Theme.highlightColor
        opacity: mouse.containsMouse ? 0.12 : 0

        Behavior on opacity {
            NumberAnimation { duration: Kirigami.Units.shortDuration }
        }
    }

    ColumnLayout {
        id: layout

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            margins: Kirigami.Units.smallSpacing
        }
        spacing: Kirigami.Units.smallSpacing / 2

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Icon {
                Layout.alignment: Qt.AlignTop
                implicitWidth: Kirigami.Units.iconSizes.smallMedium
                implicitHeight: implicitWidth
                source: card.failed ? "dialog-error"
                                    : (card.unverified ? "dialog-warning" : "dialog-ok")
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font.bold: true
                    text: card.bill.vendor !== undefined && card.bill.vendor !== ""
                        ? card.bill.vendor
                        : i18n("Unbekannter Aussteller")
                }

                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    text: card.label()
                }
            }

            PlasmaComponents.Label {
                font.bold: true
                text: card.amount()
            }
        }

        PlasmaComponents.Label {
            Layout.fillWidth: true
            visible: card.problemText.length > 0
            wrapMode: Text.Wrap
            opacity: 0.8
            font: Kirigami.Theme.smallFont
            text: card.problemText
        }
    }

    Rectangle {
        visible: card.showDivider
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Kirigami.Theme.textColor
        opacity: 0.15
    }

    /// Last, so it sits over the row and over the divider. It does not prevent
    /// stealing, because a drag across the list has to stay a scroll.
    MouseArea {
        id: mouse

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: card.clicked()
    }

    function label() {
        const date = card.bill.date !== undefined && card.bill.date !== ""
            ? card.bill.date
            : "?";
        return date + "  ·  " + Logic.describePlace(card.bill);
    }

    function amount() {
        const total = card.bill.gross_total;
        if (total === null || total === undefined)
            return "—";

        // The locale decides the decimal separator, so 11,91 EUR in German and
        // 11.91 EUR in English, matching every other number on the desktop.
        const formatted = Number(total).toLocaleString(Qt.locale(), 'f', 2);
        const currency = card.bill.currency !== undefined ? card.bill.currency : "";
        return currency.length > 0 ? (formatted + " " + currency) : formatted;
    }
}
