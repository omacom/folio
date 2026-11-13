import QtQuick

// A folder files can be dropped on. While they hover it knows whether letting go moves or copies them.
DropArea {
    id: area
    property string folder
    property bool copies: false
    // Whether the hovering files all come from other drives. It asks the drives, so it is read once a drag, never on the timer.
    property bool otherDrive: false
    // The drag loop takes the key events, so Ctrl pressed or let go with the pointer still is seen only by asking again.
    readonly property int ctrlCheckMs: 100

    keys: ["text/uri-list"]
    function recheck() { copies = otherDrive || fileOps.ctrlHeld() }
    onEntered: (drag) => { otherDrive = fileOps.dropCopies(drag.urls, folder, false); recheck() }
    onPositionChanged: recheck()

    Timer { interval: area.ctrlCheckMs; repeat: true; running: area.containsDrag; onTriggered: area.recheck() }
}
