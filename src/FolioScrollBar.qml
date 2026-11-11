import QtQuick
import QtQuick.Controls

// A thin scroll bar that shows while scrolling or hovered, and simply
// appears and disappears: no fading.
ScrollBar {
    id: bar
    padding: 2
    contentItem: Rectangle {
        implicitWidth: 4
        implicitHeight: 4
        radius: Math.min(2, theme.radius)
        // Shown at rest it is the only sign of more below, so it takes the theme's quiet text ink (3:1 at least).
        color: bar.policy === ScrollBar.AlwaysOn ? (bar.pressed ? theme.foreground : theme.secondaryForeground)
             : Qt.alpha(theme.foreground, bar.pressed ? 0.5 : 0.28)
        visible: bar.size < 1 && (bar.active || bar.hovered || bar.policy === ScrollBar.AlwaysOn)
    }
    background: null
    // Material fades the bar in and out of its active state; drop that.
    transitions: []
}
