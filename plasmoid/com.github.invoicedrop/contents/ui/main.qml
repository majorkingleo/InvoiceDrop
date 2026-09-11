import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.plasma5support as P5Support
import org.kde.kirigami as Kirigami
import "invoicelogic.js" as Logic

PlasmoidItem {
    id: root

    readonly property string cliPath: Plasmoid.configuration.cliPath || "invoicedrop"
    readonly property int historyLimit: Plasmoid.configuration.historyLimit || 8

    /// An unset key has to mean "on", not "off": a configuration written before
    /// this setting existed must not silently start swallowing notifications.
    readonly property bool notify: Plasmoid.configuration.notify !== false

    /// What a copy puts on the clipboard, and whether the widget says so
    /// afterwards. Both defaults are the ones the entries in `main.xml` declare,
    /// and the absent key is read the same way as `notify` is: on.
    readonly property bool copyWithoutCurrency: Plasmoid.configuration.copyWithoutCurrency === true
    readonly property bool showCopyNotice: Plasmoid.configuration.showCopyNotice !== false

    /// Bills, newest first. One entry per page, because a bill is a page.
    property var bills: []
    property int running: 0
    property string statusText: ""

    /// The bill the detail view is showing, or null. Set by clicking a card.
    property var openBill: null

    /// One entry per bill, `null` except at the last bill of a file, where the
    /// file's total sits. Recomputed whenever the list changes, which is also
    /// what keeps it correct when the history limit cuts a file in half.
    readonly property var subtotals: Logic.subtotalsFor(bills, Qt.locale())

    preferredRepresentation: fullRepresentation

    // --------------------------------------------------------------- pipeline

    /// Runs the CLI on one file. It answers in JSON, one object per bill, and
    /// hands the work to a running daemon on its own, so a warm daemon makes
    /// this a few milliseconds and a cold one pays the model load.
    function analyse(path) {
        if (!path)
            return;

        // A new document replaces the list the detail view was opened from, so
        // it closes rather than showing a bill that is no longer in it.
        openBill = null;

        const command = Logic.buildCommand(cliPath, Plasmoid.configuration.model,
                                           Plasmoid.configuration.archiveAfterReading,
                                           notify, path);

        running += 1;
        statusText = i18n("Lese %1 …", baseName(path));
        Logic.rememberPath(command, path);
        executable.connectSource(command);
    }

    function handleReply(source, stdout, stderr, exitCode) {
        running = Math.max(0, running - 1);

        const path = Logic.takePath(source);
        const file = baseName(path);
        const parsed = Logic.parseBills(stdout);

        if (parsed.bills.length === 0) {
            statusText = String(stderr).trim().split("\n")[0];
            if (statusText.length === 0)
                statusText = i18n("Kein Ergebnis für %1 (Beendigungscode %2)", file, exitCode);
            return;
        }

        bills = parsed.bills.concat(bills).slice(0, historyLimit);

        if (parsed.broken > 0) {
            statusText = i18n("%1 Antwortzeile(n) unlesbar", parsed.broken);
            return;
        }

        const failed = parsed.bills.filter(function (bill) {
            return bill.status !== "ok";
        }).length;

        if (failed > 0)
            statusText = i18n("%1 von %2 Belegen nicht gelesen", failed, parsed.bills.length);
        else if (parsed.bills.length === 1)
            statusText = i18n("Fertig");
        else
            statusText = i18n("%1 Belege gelesen", parsed.bills.length);
    }

    function baseName(path) {
        const parts = String(path).split("/");
        return parts[parts.length - 1];
    }

    function acceptDrop(drop) {
        if (!drop || !drop.urls)
            return;
        for (let i = 0; i < drop.urls.length; ++i) {
            const url = String(drop.urls[i]);
            if (url.indexOf("file://") === 0)
                analyse(decodeURIComponent(url.substring(7)));
        }
    }

    P5Support.DataSource {
        id: executable
        engine: "executable"
        connectedSources: []

        onNewData: (source, data) => {
            disconnectSource(source);
            root.handleReply(source, data["stdout"] || "", data["stderr"] || "",
                             data["exit code"]);
        }
    }

    // ------------------------------------------------------------ panel icon

    compactRepresentation: MouseArea {
        id: compact

        onClicked: root.expanded = !root.expanded
        hoverEnabled: true

        Kirigami.Icon {
            anchors.centerIn: parent
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: implicitWidth
            // The package's own mark, resolved by path. KPlugin.Icon in
            // metadata.json has to be a theme icon name, and no theme ships one
            // called invoice, so the two are deliberately different.
            source: root.running > 0
                ? "view-refresh"
                : Qt.resolvedUrl("../icons/invoice.svg")
            active: compact.containsMouse
        }

        // A widget in the panel is small, and still has to accept a drop.
        DropArea {
            anchors.fill: parent
            onDropped: function (drop) { root.acceptDrop(drop) }
        }
    }

    // ----------------------------------------------------------- popup view

    fullRepresentation: Item {
        implicitWidth: Kirigami.Units.gridUnit * 22
        implicitHeight: Kirigami.Units.gridUnit * 20

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing

            PlasmaComponents.Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                opacity: 0.7
                visible: root.bills.length === 0
                text: i18n("Rechnungen hierher ziehen")
            }

            PlasmaComponents.BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: root.running > 0
                visible: running
            }

            ListView {
                id: list

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 0
                model: root.bills
                visible: root.bills.length > 0

                delegate: ColumnLayout {
                    width: list.width
                    spacing: 0

                    InvoiceCard {
                        Layout.fillWidth: true
                        bill: modelData
                        // No line before a subtotal. The sum belongs to the cards
                        // above it, and a divider there would cut the group apart
                        // and leave the total looking like the next file's first
                        // row.
                        showDivider: index < list.count - 1
                                     && root.subtotals[index] === null
                        onClicked: root.openBill = modelData
                    }

                    FileSum {
                        Layout.fillWidth: true
                        visible: root.subtotals[index] !== null
                                 && root.subtotals[index] !== undefined
                        total: root.subtotals[index] !== undefined
                               ? root.subtotals[index]
                               : null
                        withoutCurrency: root.copyWithoutCurrency
                        showCopyNotice: root.showCopyNotice
                    }
                }
            }

            PlasmaComponents.Label {
                Layout.fillWidth: true
                visible: root.statusText.length > 0
                elide: Text.ElideRight
                opacity: 0.7
                font: Kirigami.Theme.smallFont
                text: root.statusText
            }
        }

        DropArea {
            id: dropArea

            anchors.fill: parent
            onDropped: function (drop) { root.acceptDrop(drop) }

            Rectangle {
                anchors.fill: parent
                visible: dropArea.containsDrag
                color: Kirigami.Theme.highlightColor
                opacity: 0.25
                radius: Kirigami.Units.smallSpacing
                border.width: 2
                border.color: Kirigami.Theme.highlightColor
            }
        }

        /// Over everything, including the drop highlight: a bill is open, and
        /// that is what the popup is about until it is closed. A drop still
        /// works while it is up, because neither this item nor the list accepts
        /// drops, so the search for a drop target carries on past them.
        BillDetails {
            anchors.fill: parent
            visible: root.openBill !== null
            bill: root.openBill
            withoutCurrency: root.copyWithoutCurrency
            showCopyNotice: root.showCopyNotice
            onClosed: root.openBill = null
        }
    }
}
