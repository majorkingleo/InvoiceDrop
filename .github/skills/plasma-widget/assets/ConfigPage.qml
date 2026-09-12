import QtQuick
import QtQuick.Controls as QQC

import org.kde.kirigami as Kirigami

// One page of the configuration dialog. Plasma sets one `cfg_<entry>` property
// per entry in `contents/config/main.xml`, so the aliases below are the whole
// interface: the name after `cfg_` has to match the entry name exactly, or the
// dialog opens on an empty page and the setting is never written.
//
// The file lives under `contents/ui/` because that is what the `source` in
// `contents/config/config.qml` is resolved against.
Kirigami.FormLayout {
    id: page

    property alias cfg_command: commandField.text
    property alias cfg_limit: limitSpin.value
    property alias cfg_notify: notifyCheck.checked

    QQC.TextField {
        id: commandField
        Kirigami.FormData.label: i18n("Command:")
        placeholderText: "hello"
    }

    QQC.Label {
        Kirigami.FormData.isSection: false
        wrapMode: Text.Wrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: i18n("Only needed when the command is not on the PATH that Plasma sees.")
    }

    QQC.SpinBox {
        id: limitSpin
        Kirigami.FormData.label: i18n("Entries to keep:")
        from: 1
        to: 50
    }

    QQC.CheckBox {
        id: notifyCheck
        Kirigami.FormData.label: i18n("Notifications:")
        text: i18n("Announce a finished job with a desktop notification")
    }
}
