import QtQuick
import QtQuick.Controls
import "Format.js" as Format

// A menu entry: its label, the key that does the same on the right, and for
// a submenu Folio's own arrow. Highlighted (by mouse or keyboard) it takes a
// tint of the accent; no ripple, no animation.
MenuItem {
    id: entry

    property string keys
    // Said while it's highlighted, by the pointer or the arrow keys, for an
    // entry that needs a word more than its label.
    property string tip
    ToolTip.visible: tip.length > 0 && highlighted
    ToolTip.delay: 600
    ToolTip.text: tip
    // Hidden entries take no room, and the arrow keys pass over them: Menu
    // skips only entries that are disabled or out of the tab chain, so
    // Return would otherwise run one nobody can see.
    property bool shown: true
    // Room for a 16 px icon.source before the label, kept when there's none so labels line up.
    property bool iconColumn: false

    // Hover is off by default here; with it, the pointer highlights an
    // entry and opens a submenu on its own, as menus elsewhere do.
    hoverEnabled: true
    visible: shown
    height: shown ? implicitHeight : 0
    activeFocusOnTab: shown
    implicitHeight: 32
    leftPadding: 14
    rightPadding: 14

    background: Rectangle {
        implicitWidth: 220
        x: 4
        width: parent.width - 8
        radius: Math.min(4, theme.radius)
        color: entry.subMenu && entry.subMenu.visible ? Qt.alpha(theme.foreground, 0.08)
             : entry.highlighted && entry.enabled ? Qt.alpha(theme.accent, 0.18) : "transparent"
    }

    contentItem: Item {
        // The gap before a key or arrow only when there is one, so a plain row's width is its label and padding.
        implicitWidth: label.anchors.leftMargin + label.implicitWidth + (entry.keys || entry.subMenu ? hint.implicitWidth + 40 : 0)
        implicitHeight: label.implicitHeight
        Image {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            visible: entry.iconColumn
            width: 16
            height: 16
            sourceSize: Qt.size(16, 16)
            fillMode: Image.PreserveAspectFit
            // Synchronous: AppIconProvider calls QIcon::fromTheme, which belongs to the GUI thread.
            source: entry.iconColumn ? entry.icon.source : ""
        }
        Text {
            id: label
            anchors.left: parent.left
            anchors.leftMargin: entry.iconColumn ? 26 : 0
            anchors.verticalCenter: parent.verticalCenter
            text: entry.text
            textFormat: Text.PlainText
            color: !entry.enabled ? theme.secondaryForeground : entry.highlighted ? theme.brightForeground : theme.foreground
        }
        Text {
            id: hint
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: !entry.subMenu
            text: Format.keys(entry.keys)
            color: theme.secondaryForeground
            font.pixelSize: 11
        }
    }

    // Placed by hand, so mirrored by hand: the arrow and the check go to the
    // left, and the arrow points where the submenu opens.
    arrow: Glyph {
        x: entry.mirrored ? 14 : entry.width - width - 14
        anchors.verticalCenter: parent.verticalCenter
        visible: !!entry.subMenu
        name: entry.mirrored ? "back" : "forward"
        size: 12
        color: entry.enabled ? theme.foreground : theme.secondaryForeground
    }
    indicator: Glyph {
        x: entry.mirrored ? 14 : entry.width - width - 14
        anchors.verticalCenter: parent.verticalCenter
        visible: entry.checked
        name: "check"
        size: 14
        color: theme.accent
    }
}
