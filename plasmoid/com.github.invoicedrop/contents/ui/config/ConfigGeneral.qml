import QtQuick
import QtQuick.Controls as QQC
import org.kde.kirigami as Kirigami

// The one page the configuration dialog has. Plasma sets one `cfg_<entry>`
// property per entry in `contents/config/main.xml`, so the aliases below are the
// whole interface: the name after `cfg_` has to match the entry name exactly, or
// the dialog opens on an empty page and the setting is never written.
//
// The file lives under `contents/ui/` because that is what the `source` in
// `contents/config/config.qml` is resolved against.
Kirigami.FormLayout {
    id: page

    property alias cfg_cliPath: cliPathField.text
    property alias cfg_model: modelField.text
    property alias cfg_historyLimit: historyLimitSpin.value
    property alias cfg_archiveAfterReading: archiveCheck.checked
    property alias cfg_notify: notifyCheck.checked
    property alias cfg_copyWithoutCurrency: copyWithoutCurrencyCheck.checked
    property alias cfg_showCopyNotice: copyNoticeCheck.checked

    QQC.TextField {
        id: cliPathField
        Kirigami.FormData.label: i18n("invoicedrop binary:")
        placeholderText: "invoicedrop"
    }

    QQC.Label {
        Kirigami.FormData.isSection: false
        wrapMode: Text.Wrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: i18n("Only needed when the binary is not on the PATH that Plasma sees.")
    }

    QQC.TextField {
        id: modelField
        Kirigami.FormData.label: i18n("Model:")
        placeholderText: "gemma4:latest"
    }

    QQC.SpinBox {
        id: historyLimitSpin
        Kirigami.FormData.label: i18n("Bills in the list:")
        from: 1
        to: 50
    }

    QQC.CheckBox {
        id: archiveCheck
        Kirigami.FormData.label: i18n("Archive:")
        text: i18n("Move the document into the archive once it was read")
    }

    QQC.CheckBox {
        id: notifyCheck
        Kirigami.FormData.label: i18n("Notifications:")
        text: i18n("Announce a finished document with a desktop notification")
    }

    QQC.CheckBox {
        id: copyWithoutCurrencyCheck
        Kirigami.FormData.label: i18n("Clipboard:")
        text: i18n("Copy amounts without the currency code")
    }

    QQC.CheckBox {
        id: copyNoticeCheck
        Kirigami.FormData.label: i18n("Confirmation:")
        text: i18n("Say in the widget that something was copied")
    }
}
