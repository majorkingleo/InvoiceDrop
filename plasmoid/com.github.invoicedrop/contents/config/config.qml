import QtQuick

import org.kde.plasma.configuration

// The applet's configuration dialog. Plasma 6 finds the pages through this model
// and nowhere else: a `config.qml` that is a plain page loads without a single
// warning and is then never shown, because the ConfigModel it was expected to be
// has no categories in it. That is exactly how this file spent its first release.
//
// `source` is resolved against `contents/ui/`, not against this folder, which is
// why the page it names lives in `contents/ui/config/`.
ConfigModel {
    ConfigCategory {
        name: i18n("General")
        icon: "settings-configure"
        source: "config/ConfigGeneral.qml"
    }
}
