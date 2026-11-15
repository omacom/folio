import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What a file chooser's window adds to a Folio window, along the bottom:
// the name to save under, the app's filters, Cancel, and the button that
// answers. The answer comes from the pane used last, with what's selected
// there; Return on a file or a double click answers with it, and so does
// Ctrl+Return from anywhere. It lies over the whole window, so that asking
// before replacing a file covers it.
Item {
    id: root

    required property var pane
    readonly property int barHeight: bar.height
    readonly property var host: ApplicationWindow.window
    readonly property bool naming: chooser.mode === "save"
    // Keys go to the name or the Replace prompt, not the window's shortcuts.
    readonly property bool typing: nameField.activeFocus || confirm.visible
    // The selection, or the item under the cursor, as it changes.
    readonly property var picked: pane ? (pane.paneBackend.folder.selectedCount, pane.selectedPaths()) : []

    function answer(items) {
        const backend = pane.paneBackend
        const paths = chooser.answer(backend.path, items, nameField.text)
        if (paths.length === 0) {
            // Nothing to answer with yet: a folder typed as the name is gone
            // into, and a folder picked to open is opened.
            if (naming && nameField.text.trim().length > 0) {
                // Read the way answer() reads it: a bare name exactly as
                // typed, and only a path the way the path bar does.
                const name = nameField.text
                const here = backend.path + "/" + name
                if (!name.includes("/") && !name.startsWith("~")) {
                    if (chooser.folders([here]).length > 0) {
                        backend.navigate(here)
                        nameField.text = ""
                    }
                } else if (backend.goTyped(name)) {
                    nameField.text = ""
                }
            } else if (pane.current.isDir) {
                backend.open(pane.current.path)
            }
            return
        }
        // The app names the files it saves, so a folder taking one of those
        // names here can't be asked about: the files go somewhere else.
        const folders = chooser.mode === "saveFiles" ? chooser.folders(paths) : []
        if (folders.length > 0) {
            backend.showNotice(qsTr("A folder here is already called %1.").arg(folders[0].slice(folders[0].lastIndexOf("/") + 1)))
            return
        }
        const there = chooser.mode.startsWith("save") ? chooser.existing(paths) : []
        if (there.length === 0) {
            chooser.accept(paths)
            return
        }
        const names = there.map(path => path.slice(path.lastIndexOf("/") + 1))
        confirm.ask(names.length === 1 ? qsTr("Replace %1?").arg(names[0]) : qsTr("Replace %Ln files?", "", names.length),
                    names.length === 1 ? qsTr("%1 already exists here.").arg(names[0])
                                       : qsTr("These already exist here: %1").arg(names.join(", ")),
                    qsTr("Replace"), () => chooser.accept(paths))
    }

    function focusName() {
        nameField.forceActiveFocus()
        const dot = fileOps.extensionStart(nameField.text, false)
        nameField.select(0, dot > 0 ? dot : nameField.text.length)
    }

    // Saving starts in the name, once the window has given the files the
    // keyboard.
    Connections {
        target: root.host
        function onSettledChanged() { if (root.naming) Qt.callLater(root.focusName) }
    }
    Connections {
        target: chooser
        function onDone() { root.host.close() }
    }
    // A file opened in the pane: the answer, first of the selection when
    // it's part of one; or when saving, the name to save under.
    Connections {
        target: root.pane ? root.pane.paneBackend : null
        function onChosen(path) {
            if (root.naming) {
                nameField.text = path.slice(path.lastIndexOf("/") + 1)
                root.focusName()
            } else if (chooser.mode === "open") {
                root.answer(root.picked.includes(path) ? [path].concat(root.picked.filter(p => p !== path)) : [path])
            }
        }
    }
    Shortcut { sequences: ["Ctrl+Return", "Ctrl+Enter"]; enabled: !confirm.visible; onActivated: root.answer(root.picked) }

    Rectangle {
        id: bar
        anchors.bottom: parent.bottom
        width: parent.width
        height: 64
        color: theme.background
        Rectangle { width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 20
            spacing: 10

            PlainField {
                id: nameField
                visible: root.naming
                Layout.fillWidth: true
                text: chooser.name
                placeholderText: qsTr("Name")
                onAccepted: root.answer(root.picked)
            }
            // Otherwise, what the button would answer with.
            Text {
                visible: !root.naming
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                text: !root.pane ? "" : chooser.answer(root.pane.paneBackend.path, root.picked, "")
                                             .map(path => path.slice(path.lastIndexOf("/") + 1) || path).join(", ")
                color: theme.secondaryForeground
                elide: Text.ElideRight
            }
            PlainButton {
                id: filterButton
                visible: chooser.filters.length > 0
                text: chooser.filters[chooser.filter] || ""
                onClicked: filterMenu.open()
                FolioMenu {
                    id: filterMenu
                    y: -height - 4
                    Repeater {
                        model: chooser.filters
                        FolioMenuItem {
                            required property int index
                            required property string modelData
                            text: modelData
                            checked: index === chooser.filter
                            onTriggered: chooser.filter = index
                        }
                    }
                }
            }
            PlainButton {
                text: qsTr("Cancel")
                onClicked: chooser.cancel()
            }
            PlainButton {
                primary: true
                text: chooser.acceptLabel || (chooser.mode.startsWith("save") ? qsTr("Save")
                                            : chooser.mode === "directory" ? qsTr("Choose") : qsTr("Open"))
                onClicked: root.answer(root.picked)
            }
        }
    }

    ConfirmDialog {
        id: confirm
        anchors.fill: parent
        onVisibleChanged: if (!visible && root.pane) root.pane.focusView()
    }
}
