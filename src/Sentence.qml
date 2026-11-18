import QtQuick

// A sentence in the middle of an empty view: what it waits for, that it's empty, or why not.
Text {
    // Kept clear on either side.
    property int margin: 48

    anchors.centerIn: parent
    width: parent.width - 2 * margin
    horizontalAlignment: Text.AlignHCenter
    wrapMode: Text.Wrap
    color: theme.foreground
}
