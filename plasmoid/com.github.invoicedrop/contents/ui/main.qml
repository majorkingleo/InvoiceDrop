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

    /// Whether the question about emptying the store is on screen, and whether the
    /// wipe it asked for is running. The question stays up while the work runs, so
    /// the answer and its consequence are one gesture instead of a question that
    /// vanishes and a list that empties later for no visible reason.
    property bool confirmingWipe: false
    property bool wiping: false

    /// How many bills the CLI last said are stored, or -1 when it did not answer.
    /// The question names the number, so it says what is about to go.
    property int storedCount: -1

    /// One entry per bill, `null` except at the last bill of a file, where the
    /// file's total sits. Recomputed whenever the list changes, which is also
    /// what keeps it correct when the history limit cuts a file in half.
    readonly property var subtotals: Logic.subtotalsFor(bills, Qt.locale())

    preferredRepresentation: fullRepresentation

    /// Opening the popup is the moment a stale list becomes visible, so it is the
    /// moment to ask about the store. The cards on screen outlive nothing: they
    /// are built from drops and live in this shell, while `--wipe` runs in a
    /// terminal in another process and cannot reach them.
    ///
    /// The parameter is declared rather than left to the signal's implicit
    /// injection: referencing `expanded` without declaring it is deprecated in Qt 6
    /// and warns at load time, in the shell's log where nothing else does.
    onExpandedChanged: function (expanded) {
        if (expanded)
            refreshFromStore();
    }

    // --------------------------------------------------------------- pipeline

    /// Asks the CLI how many bills are stored.
    ///
    /// One cheap call: `history --count` opens the database and answers, so there
    /// is no model, no daemon and no wait behind it.
    function refreshFromStore() {
        executable.connectSource(Logic.storedCountCommand(cliPath));
    }

    /// Drops the list when the store has nothing left in it.
    ///
    /// The decision is in the logic module and the effects are here: only a plain
    /// zero clears, and only when something is on screen. The detail view goes
    /// with the list, because the bill it was showing is no longer stored either.
    function applyStoredCount(count) {
        if (count >= 0)
            storedCount = count;

        if (!Logic.wiped(count, bills.length))
            return;

        bills = [];
        openBill = null;
        statusText = i18n("Keine Belege mehr gespeichert");
    }

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

    /// Reads one document again, ignoring the store.
    ///
    /// The bill keeps its place in the detail view, so the new answer can be read
    /// against the old one without leaving the view and finding the card again.
    function retry(bill) {
        if (!Logic.retryable(bill))
            return;

        const path = String(bill.path);
        const command = Logic.retryCommand(cliPath, Plasmoid.configuration.model,
                                           notify, path);
        running += 1;
        statusText = i18n("Lese %1 erneut …", baseName(path));
        Logic.rememberPath(command, path);
        executable.connectSource(command);
    }

    /// Runs `--wipe`, which empties the database, the archive and the inbox.
    ///
    /// The list is not cleared here. It goes when the store answers that it is
    /// empty, which is the road a wipe in a terminal already travels, and it means
    /// a wipe that failed leaves the cards on screen, where they are still true.
    function wipe() {
        wiping = true;
        running += 1;
        statusText = i18n("Lösche alles Gespeicherte …");
        executable.connectSource(Logic.wipeCommand(cliPath));
    }

    function applyWipe(stderr, exitCode) {
        wiping = false;
        confirmingWipe = false;

        if (exitCode !== 0) {
            const message = Logic.firstLine(stderr);
            statusText = message.length > 0
                ? message
                : i18n("Löschen fehlgeschlagen (Beendigungscode %1)", exitCode);
            return;
        }

        statusText = i18n("Alles gelöscht");
        refreshFromStore();
    }

    function handleReply(source, stdout, stderr, exitCode) {
        // The store check is not a drop: it has no path to look up, it must not
        // count as work in progress, and its reply is a number rather than a list
        // of bills.
        if (source === Logic.storedCountCommand(cliPath)) {
            applyStoredCount(Logic.parseStoredCount(stdout));
            return;
        }

        running = Math.max(0, running - 1);

        // A wipe is not a read either: it names no document, and its reply is the
        // three lines the CLI prints rather than a bill.
        if (source === Logic.wipeCommand(cliPath)) {
            applyWipe(stderr, exitCode);
            return;
        }

        const path = Logic.takePath(source);
        const file = baseName(path);
        const parsed = Logic.parseBills(stdout);

        if (parsed.bills.length === 0) {
            // The first line and not the whole message: a failing CLI explains
            // itself there, and the status row has room for one line.
            statusText = Logic.firstLine(stderr);
            if (statusText.length === 0)
                statusText = i18n("Kein Ergebnis für %1 (Beendigungscode %2)", file, exitCode);
            return;
        }

        // Reading a document again replaces its cards rather than adding a second
        // copy of them, so a retry leaves the answer it replaced out of the list.
        bills = Logic.mergeBills(bills, parsed.bills, historyLimit);

        // A detail view that was up stays up, on the same page of the same
        // document: a retry that closed it would hide the answer it was asked for.
        const openKey = openBill !== null ? Logic.fileKey(openBill) : "";
        if (openKey.length > 0 && openKey === Logic.fileKey(parsed.bills[0]))
            openBill = Logic.reopenedBill(openBill, bills);

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

            /// The status line, and the one button that acts on the whole list
            /// rather than on a card.
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    visible: root.statusText.length > 0
                    elide: Text.ElideRight
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    text: root.statusText
                }

                /// Emptying the store is the only thing here that cannot be undone,
                /// so this button asks before it does anything. The asking cannot
                /// live in the CLI: it has no interactive mode, and the reason the
                /// button exists is a list that is wrong, which is a thing only the
                /// widget knows.
                ///
                /// Disabled while a document is being read, because a wipe in the
                /// middle of a read would delete the bills that read is about to
                /// store.
                PlasmaComponents.ToolButton {
                    Layout.alignment: Qt.AlignRight
                    icon.name: "edit-delete"
                    text: i18n("Alles löschen")
                    display: PlasmaComponents.AbstractButton.IconOnly
                    visible: root.bills.length > 0
                    enabled: root.running === 0
                    onClicked: root.confirmingWipe = true
                }
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
            busy: root.running > 0
            onClosed: root.openBill = null
            onRetried: function (bill) { root.retry(bill) }
        }

        /// Over even the detail view, because the question is about the whole list,
        /// and a card that reads wrong is exactly what a wipe is asked for.
        WipePrompt {
            anchors.fill: parent
            visible: root.confirmingWipe
            question: i18n("Alle Belege löschen?")
            detail: root.storedCount >= 0
                ? i18n("Gespeicherte Belege: %1. Datenbank, Archiv und Eingangsordner werden geleert.", root.storedCount)
                : i18n("Datenbank, Archiv und Eingangsordner werden geleert. Das lässt sich nicht rückgängig machen.")
            acceptText: i18n("Alles löschen")
            busy: root.wiping
            onAccepted: root.wipe()
            onRejected: root.confirmingWipe = false
        }
    }
}
