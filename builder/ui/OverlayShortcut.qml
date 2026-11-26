import QtQuick
import QtQuick.Controls.Basic

Row {
    id: shortcut
    required property var builder
    readonly property var selected: builder.menuShortcut.split("+")
    readonly property string first: selected[0]
    readonly property string second: selected.length > 1 ? selected[1] : ""
    spacing: 8

    ComboBox {
        objectName: "overlayFirstButton"
        width: 134
        Accessible.name: "Overlay shortcut first button"
        model: ["Default"].concat(shortcut.builder.menuButtons)
        currentIndex: shortcut.first ? model.indexOf(shortcut.first) : 0
        onActivated: function(index) {
            if (index === 0) {
                shortcut.builder.setMenuShortcut("")
                return
            }
            const chosen = model[index]
            const other = shortcut.second && shortcut.second !== chosen ? shortcut.second
                        : shortcut.first || (chosen === "L Stick" ? "R Stick" : "L Stick")
            shortcut.builder.setMenuShortcut(chosen + "+" + other)
        }
    }
    ComboBox {
        objectName: "overlaySecondButton"
        width: 134
        visible: shortcut.first !== ""
        Accessible.name: "Overlay shortcut second button"
        model: shortcut.builder.menuButtons.filter(button => button !== shortcut.first)
        currentIndex: model.indexOf(shortcut.second)
        onActivated: function(index) {
            shortcut.builder.setMenuShortcut(shortcut.first + "+" + model[index])
        }
    }
}
