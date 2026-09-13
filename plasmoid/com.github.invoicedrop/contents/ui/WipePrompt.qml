import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents

/**
 * The question that stands between the clear button and the store.
 *
 * `--wipe` empties the database, the archive and the inbox, and none of the three
 * comes back. A widget in a panel is a place where a click lands by accident, so
 * the button on its own would eventually delete somebody's archive; the CLI
 * cannot help either, because it has no interactive mode to ask with. The asking
 * therefore happens here, in front of the button.
 *
 * It is drawn into the popup rather than opened as a `Dialog`. The popup is
 * already the window the user is looking at, and a dialog opened from inside it
 * competes for the grab — the usual end of that is the popup closing and the
 * question going with it. This is an item over the list, laid out the way the
 * detail view is, and the list stays readable underneath as the thing the
 * question is about.
 *
 * Escape is the same answer as "Abbrechen": the answer that needs an aim is the
 * one that deletes.
 */
Item {
    id: prompt

    property string question: ""
    property string detail: ""
    property string acceptText: i18n("Löschen")
    property string rejectText: i18n("Abbrechen")

    /// Set while the work the answer asked for is running. The buttons go quiet
    /// rather than the prompt closing: the question is answered, and what is left
    /// is waiting.
    property bool busy: false

    signal accepted()
    signal rejected()

    /// The overlay is the only thing that can be answered while it is up, so it
    /// takes the keys. Without the focus the Escape handler never runs.
    focus: visible
    Keys.onEscapePressed: function (event) {
        prompt.rejected();
        event.accepted = true;
    }

    /// Over the list, not instead of it: the question is about what is behind it.
    Rectangle {
        anchors.fill: parent
        color: Kirigami.Theme.backgroundColor
        opacity: 0.85
    }

    Rectangle {
        id: card

        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.smallSpacing * 4,
                        Kirigami.Units.gridUnit * 18)
        height: content.implicitHeight + Kirigami.Units.largeSpacing * 2
        radius: Kirigami.Units.smallSpacing
        border.width: 1
        border.color: Kirigami.Theme.textColor
        // The popup inherits the panel's colours; a card drawn in the window
        // colours is what makes it read as being on top of the list rather than
        // as part of it.
        Kirigami.Theme.inherit: false
        Kirigami.Theme.colorSet: Kirigami.Theme.Window
        color: Kirigami.Theme.backgroundColor

        ColumnLayout {
            id: content

            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.largeSpacing * 2
            spacing: Kirigami.Units.smallSpacing

            PlasmaComponents.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.bold: true
                text: prompt.question
            }

            PlasmaComponents.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.8
                font: Kirigami.Theme.smallFont
                text: prompt.detail
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                // The text says what happens, the icon says that it cannot be
                // undone. Both buttons are the same size, because a question
                // where one answer is easier to hit than the other is not one.
                PlasmaComponents.Button {
                    Layout.fillWidth: true
                    text: prompt.acceptText
                    icon.name: "edit-delete"
                    enabled: !prompt.busy
                    onClicked: prompt.accepted()
                }

                PlasmaComponents.Button {
                    Layout.fillWidth: true
                    text: prompt.rejectText
                    enabled: !prompt.busy
                    onClicked: prompt.rejected()
                }
            }

            PlasmaComponents.BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: prompt.busy
                visible: prompt.busy
            }
        }
    }
}
