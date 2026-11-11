import QtQuick
import QtQuick.Controls

// A plugin's mark on a file or folder ({tone, badge, label}, see
// Plugins::folderMarks): a round badge with its letter, or a small dot when
// it has none or `dot` asks for one, in its tone's color. What isn't
// tracked is an outline. Its label is its tooltip and what a screen reader
// reads.
Rectangle {
    id: mark

    property var of
    property bool dot: false
    readonly property bool letter: !dot && !!of && !!of.badge
    readonly property bool outlined: !!of && of.tone === "untracked"
    readonly property color tone: (of && theme.tones[of.tone]) || theme.secondaryForeground

    visible: !!of
    implicitWidth: letter ? 16 : 8
    implicitHeight: implicitWidth
    radius: width / 2
    color: outlined ? theme.background : tone
    border.width: outlined ? 1.5 : 0
    border.color: tone

    Text {
        visible: mark.letter
        anchors.centerIn: parent
        text: mark.of ? mark.of.badge : ""
        color: mark.outlined ? mark.tone : theme.background
        font.pixelSize: 10
        font.bold: true
    }

    Accessible.role: Accessible.StaticText
    Accessible.name: of ? of.label : ""
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered && Accessible.name.length > 0
    ToolTip.text: Accessible.name
    ToolTip.delay: 600
}
