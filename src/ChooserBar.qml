import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The bar along the bottom of a window that is another app's Open or Save
// dialog: the name being saved, the app's own questions, the kinds of file
// to list, and the buttons that answer. Everything above it is Folio as it
// always is, so a file is found here the way it's found anywhere.
FocusScope {
    id: bar

    // The pane the answer is read from: the one used last.
    required property var pane
    readonly property bool saving: chooser.mode === "save"
    readonly property bool typing: nameField.activeFocus
    implicitHeight: 64

    // What the button would do as things stand (see Chooser::step): go into
    // a folder, answer, or nothing yet.
    readonly property var step: pane ? chooser.step(pane.paneBackend.path, pane.selectedPaths(), nameField.text) : ({})
    readonly property var paths: step.paths || []

    // Answers with `picked` (or what's picked in the pane). Worked out again
    // as it's pressed: a file may have appeared since the button was drawn.
    function go(picked) {
        if (!pane)
            return
        const now = chooser.step(pane.paneBackend.path, picked || pane.selectedPaths(), nameField.text)
        if (now.enter) {
            pane.paneBackend.navigate(now.enter)
            if (saving)
                nameField.text = chooser.name
        } else if (now.paths && now.paths.length > 0) {
            if (now.replaces.length === 0) {
                chooser.accept(now.paths)
                return
            }
            asked = true
            pane.ask(now.replaces.length === 1 ? qsTr("Replace %1?").arg(now.replaces[0])
                                               : qsTr("Replace %Ln files?", "", now.replaces.length),
                     qsTr("Saving here writes over what's already there. This can't be undone."),
                     qsTr("Replace"), () => chooser.accept(now.paths))
        }
    }
    // A folder is being picked: arriving in one, the pane selects nothing
    // in it (see FolderPane), so the answer is the folder shown until one
    // inside it is picked on purpose.
    readonly property bool pickingFolder: chooser.mode === "folder" || chooser.mode === "saveInto"

    // Replace turned down, the name is what there is to change: the
    // keyboard goes back to it, not to the files.
    property bool asked: false
    Connections {
        target: bar.pane
        function onAskingChanged() {
            if (bar.pane.asking || !bar.asked)
                return
            bar.asked = false
            if (bar.saving)
                Qt.callLater(bar.focusKeyboard)
        }
    }

    // A save starts in the name, with the part before the extension
    // selected, ready to be typed over.
    function focusKeyboard() {
        if (!saving) {
            acceptButton.forceActiveFocus(Qt.TabFocusReason)
            return
        }
        nameField.forceActiveFocus()
        const dot = nameField.text.lastIndexOf(".")
        nameField.select(0, dot > 0 ? dot : nameField.text.length)
    }
    // Tab within the bar; false once it has run off either end.
    function stepKeyboard(forward) {
        const next = Window.activeFocusItem.nextItemInFocusChain(forward)
        for (let i = next; i; i = i.parent) {
            if (i === bar) {
                next.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                return true
            }
        }
        return false
    }

    Connections {
        target: chooser
        // Return or a double click on a file, or Open from its menu.
        // Saving, that's the file to write over, wherever it is. Opening,
        // it answers: with everything selected when it's part of that, and
        // otherwise (a file in another column) with the file itself.
        function onActivated(path) {
            if (!bar.pane)
                return
            if (bar.saving) {
                const here = bar.pane.paneBackend.path
                nameField.text = path.startsWith(here + "/") && path.indexOf("/", here.length + 1) < 0
                               ? path.slice(here.length + 1) : path
                bar.go()
            } else {
                const selected = bar.pane.selectedPaths()
                bar.go(selected.includes(path) ? selected : [path])
            }
        }
    }

    Rectangle { anchors.fill: parent; color: theme.background }
    Rectangle { width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        spacing: 10

        PlainField {
            id: nameField
            visible: bar.saving
            Layout.fillWidth: true
            Layout.maximumWidth: 420
            text: chooser.name
            placeholderText: qsTr("Name")
            Accessible.name: placeholderText
            onAccepted: bar.go()
        }
        // Where a folder's answer points, or how many files a pick holds.
        Text {
            visible: !bar.saving
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            elide: Text.ElideMiddle
            color: theme.secondaryForeground
            text: bar.paths.length > 1 ? qsTr("%Ln items", "", bar.paths.length)
                : bar.paths.length === 1 && chooser.mode === "folder" ? chooser.shown(bar.paths[0])
                : bar.paths.length === 1 && chooser.mode === "saveInto" ? chooser.shown(bar.paths[0].slice(0, bar.paths[0].lastIndexOf("/")) || "/")
                : ""
        }
        Item { visible: bar.saving; Layout.fillWidth: true }

        // The app's questions: a checkbox, or a pick from a few answers.
        // By count, so a choice made doesn't rebuild the buttons (and take
        // the keyboard from the one just pressed).
        Repeater {
            model: chooser.choices.length
            PlainButton {
                id: choice
                required property int index
                readonly property var modelData: chooser.choices[index]
                readonly property bool checkbox: modelData.options.length === 0
                readonly property var picked: modelData.options.find(o => o.id === modelData.value)
                text: checkbox ? (modelData.value === "true" ? "✓ " : "") + modelData.label
                               : modelData.label + (picked ? ": " + picked.label : "")
                onClicked: checkbox ? chooser.setChoice(modelData.id, modelData.value === "true" ? "false" : "true")
                                    : answers.popup(choice, 0, -answers.height)
                FolioMenu {
                    id: answers
                    Repeater {
                        model: choice.modelData.options
                        FolioMenuItem {
                            required property var modelData
                            text: modelData.label
                            checked: modelData.id === choice.modelData.value
                            onTriggered: chooser.setChoice(choice.modelData.id, modelData.id)
                        }
                    }
                }
            }
        }

        PlainButton {
            id: filterButton
            visible: chooser.filterNames.length > 0
            enabled: chooser.filterNames.length > 1
            text: chooser.filterNames[chooser.filterIndex] || ""
            Accessible.description: qsTr("Kinds of file listed")
            onClicked: filters.popup(filterButton, 0, -filters.height)
            FolioMenu {
                id: filters
                Repeater {
                    model: chooser.filterNames
                    FolioMenuItem {
                        required property string modelData
                        required property int index
                        text: modelData
                        checked: index === chooser.filterIndex
                        onTriggered: chooser.filterIndex = index
                    }
                }
            }
        }

        PlainButton {
            text: qsTr("Cancel")
            onClicked: chooser.cancel()
        }
        PlainButton {
            id: acceptButton
            primary: true
            enabled: !!bar.step.enter || bar.paths.length > 0
            opacity: enabled ? 1 : 0.4
            text: bar.step.enter && !bar.saving ? qsTr("Open")
                : chooser.acceptLabel.length > 0 ? chooser.acceptLabel
                : chooser.mode === "open" ? qsTr("Open")
                : chooser.mode === "folder" ? qsTr("Choose")
                : qsTr("Save")
            onClicked: bar.go()
        }
    }
}
