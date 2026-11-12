import QtQuick
import QtQuick.Controls

// A square toolbar button holding one glyph. `text` is its accessible name
// and tooltip, never drawn.
AbstractButton {
    id: button

    property string glyph
    property bool active: false
    // An on/off switch: off draws in the secondary ink, so on and off differ by brightness alone.
    property bool toggle: false
    // Some themes set brightForeground dimmer than foreground; on takes whichever stands out more.
    readonly property color onInk: (theme.brightForeground.hslLightness > theme.foreground.hslLightness) !== theme.light
                                   ? theme.brightForeground : theme.foreground

    implicitWidth: 36
    implicitHeight: 36
    focusPolicy: Qt.NoFocus
    hoverEnabled: true

    Accessible.name: text
    // Its own tooltip, so it can appear without fading in or out.
    ToolTip {
        visible: button.hovered && button.text.length > 0
        delay: 600
        text: button.text
        enter: null
        exit: null
    }

    background: Rectangle {
        radius: Math.min(4, theme.radius)
        // A toggle says on by its ink alone; any other active button keeps the plate.
        color: button.active && !button.toggle ? Qt.alpha(theme.accent, 0.16)
             : button.hovered && button.enabled ? Qt.alpha(theme.foreground, 0.08)
             : "transparent"
    }

    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            name: button.glyph
            color: !button.enabled ? Qt.alpha(theme.foreground, 0.35)
                 : button.toggle ? (button.active ? button.onInk : theme.secondaryForeground)
                 : button.active ? theme.brightForeground : theme.foreground
        }
    }
}
