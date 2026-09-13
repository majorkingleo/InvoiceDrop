import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import "invoicelogic.js" as Logic

/**
 * One bill in full: every value the CLI reported for it, in a field that can be
 * selected, next to the two things one does with an invoice.
 *
 * The card shows four fields because it has to fit in a panel popup. An invoice
 * has twenty, and the number that has to be typed into a bookkeeping program is
 * rarely the one the card chose to show — so the whole block is selectable, and
 * the amount and the document are one click away.
 *
 * The block is markup rather than plain text, for the two lines in it that matter:
 * the date and the amount are set apart by a blank line and drawn in bold, because
 * finding them in twenty `Label: value` lines was the one thing the field made
 * hard. The markup comes from `Logic.billHtml`, which is the same builder as the
 * plain text, so the two cannot drift apart.
 */
Item {
    id: details

    property var bill: null
    property string notice: ""
    property string body: Logic.billHtml(bill, Qt.locale())

    /// From the widget's settings: what the amount button copies, and whether it
    /// says so afterwards.
    property bool withoutCurrency: false
    property bool showCopyNotice: true

    /// True while the widget is reading a document, so the retry button cannot be
    /// pressed while its own answer, or another document, is still on the way.
    /// The popup covers the status row, so without this the button would look
    /// available for the whole of a read.
    property bool busy: false

    readonly property bool hasAmount: bill !== null
                                      && Logic.billAmount(bill) !== null

    signal closed()

    /// Asked for by the button that reads the document again, and carries the bill
    /// it was asked about: the button knows which page is on screen and the widget
    /// does not have to guess whether the detail view moved in between.
    signal retried(var bill)

    /// Both copy buttons go through here, so what they put on the clipboard is the
    /// value and nothing else.
    Clipboard {
        id: clipboard
    }

    /// Where the document is, or the file name when the CLI sent no path. The
    /// file name alone is not openable, and `fileUrl` returns nothing for an
    /// empty path rather than a URL that points at the current directory.
    function documentPath() {
        if (!bill)
            return "";
        if (bill.path !== undefined && bill.path !== "")
            return bill.path;
        return bill.file !== undefined ? bill.file : "";
    }

    function amountText() {
        if (!hasAmount)
            return "";
        // The block above always names the currency, because that is what the CLI
        // reported. This button is the one place the setting reaches.
        if (details.withoutCurrency)
            return Logic.bareMoneyText(bill.gross_total, Qt.locale());
        return Logic.moneyText(bill.gross_total, bill.currency, Qt.locale());
    }

    function copyText(text) {
        if (!clipboard.copy(text))
            return;
        if (!details.showCopyNotice)
            return;
        details.notice = i18n("Kopiert");
        noticeTimer.restart();
    }

    Timer {
        id: noticeTimer

        interval: 1500
        onTriggered: details.notice = ""
    }

    Rectangle {
        anchors.fill: parent

        Kirigami.Theme.inherit: false
        Kirigami.Theme.colorSet: Kirigami.Theme.Window
        color: Kirigami.Theme.backgroundColor
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            PlasmaComponents.ToolButton {
                icon.name: "go-previous"
                text: i18n("Zurück")
                display: PlasmaComponents.AbstractButton.IconOnly
                onClicked: details.closed()
            }

            PlasmaComponents.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.bold: true
                text: details.bill !== null && details.bill.vendor !== undefined
                      && details.bill.vendor !== ""
                    ? details.bill.vendor
                    : i18n("Unbekannter Aussteller")
            }

            /// The bill on screen is the one that can be wrong, so this is where a
            /// second opinion belongs. It is the only action in the widget that
            /// deliberately does not ask the store first: `--no-cache` is the whole
            /// of what a retry is, and the stored answer would be the one it is
            /// meant to replace.
            PlasmaComponents.ToolButton {
                icon.name: "view-refresh"
                text: i18n("Erneut lesen")
                display: PlasmaComponents.AbstractButton.IconOnly
                visible: !details.busy
                enabled: Logic.retryable(details.bill)
                onClicked: details.retried(details.bill)
            }

            /// In the button's place while the read is running. The popup covers
            /// the status row, and a retry pays for a model load more often than not,
            /// so a button that went grey and nothing else would read as broken.
            PlasmaComponents.BusyIndicator {
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: Kirigami.Units.iconSizes.smallMedium
                implicitHeight: implicitWidth
                visible: details.busy
                running: details.busy
            }
        }

        PlasmaComponents.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            PlasmaComponents.TextArea {
                id: bodyField

                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                text: details.body
                // The block arrives as markup from `Logic.billHtml`. A selection
                // copied out of this field therefore carries HTML as well as
                // plain text, which is what the field's own copy does; the two
                // buttons above copy a bare value through `Clipboard` instead.
                textFormat: TextEdit.RichText
                // Fixed width, because this block is meant to be read as columns
                // and copied out of, not to look like prose. The name is
                // `fixedWidthFont` here and `fixedFont` in PlasmaCore, which is
                // the kind of difference that only shows up under a running
                // Plasma: the wrong one is `undefined` and the field silently
                // keeps the default font.
                font: Kirigami.Theme.fixedWidthFont
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            PlasmaComponents.Button {
                Layout.fillWidth: true
                text: i18n("Betrag kopieren")
                icon.name: "edit-copy"
                enabled: details.hasAmount
                onClicked: details.copyText(details.amountText())
            }

            PlasmaComponents.Button {
                Layout.fillWidth: true
                text: i18n("Öffnen")
                icon.name: "document-open"
                enabled: Logic.fileUrl(details.documentPath()).length > 0
                onClicked: Qt.openUrlExternally(Logic.fileUrl(details.documentPath()))
            }
        }

        PlasmaComponents.Label {
            Layout.fillWidth: true
            visible: details.notice.length > 0
            horizontalAlignment: Text.AlignHCenter
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            text: details.notice
        }
    }
}
