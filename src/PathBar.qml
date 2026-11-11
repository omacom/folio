import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The breadcrumb path from the left edge; view buttons on the right. Back and
// forward are keys (Alt+← / Alt+→) and the mouse's thumb buttons.
Rectangle {
    id: bar

    property bool previewVisible: true
    property bool previewFits: true
    property string viewMode: "grid"
    // Ctrl+L: the path as text to edit. Enter goes there (or connects to a
    // server's address), Escape cancels, Tab completes a folder name.
    property bool editing: false
    // The right pane of a split has a close button (Ctrl+W).
    property bool closable: false
    signal closeRequested()
    signal togglePreview()
    signal cycleView()
    signal editDone()

    function editPath() {
        pathField.text = backend.typedPath()
        editing = true
        pathField.forceActiveFocus()
        pathField.selectAll()
    }
    function stopEditing() {
        editing = false
        editDone()
    }

    // Tab reaches the folder's chip while it does something; Escape hands
    // the keyboard back to the files.
    readonly property bool chipActs: chip.visible && chip.acts
    readonly property bool chipFocused: chip.activeFocus
    function focusChip() { chip.forceActiveFocus(Qt.TabFocusReason) }

    // /: search this folder and every folder below it, as you type. The
    // results replace the folder until Escape, or until going elsewhere.
    readonly property bool searching: backend.folder.query.length > 0 || searchFocused
    readonly property bool searchFocused: searchField.activeFocus
    function search() {
        searchField.forceActiveFocus()
        searchField.selectAll()
    }
    // S and Shift+S, through the sort menu's orders.
    function stepSort(by) { sortMenu.step(by) }
    function endSearch() {
        searchDelay.stop()
        searchField.text = ""
        backend.folder.query = ""
        editDone()
    }
    Timer {
        id: searchDelay
        interval: 150
        onTriggered: backend.folder.query = searchField.text.trim()
    }
    // Going to another folder ends the search there.
    Connections {
        target: backend.folder
        function onQueryChanged() { if (backend.folder.query === "" && !searchField.activeFocus) searchField.text = "" }
    }

    // Transparent: the window draws one bar across the whole top, so the
    // path bars of one or two panes read as part of it.
    color: "transparent"
    implicitHeight: 60

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 26
        anchors.rightMargin: 20
        spacing: 4


        PlainField {
            id: pathField
            Layout.fillWidth: true
            visible: bar.editing
            font.pixelSize: 14
            placeholderText: qsTr("A folder, or a server's address like sftp://host/")
            onAccepted: {
                if (backend.goTyped(text))
                    bar.stopEditing()
            }
            Keys.onEscapePressed: bar.stopEditing()
            Keys.onTabPressed: {
                text = backend.completePath(text)
                cursorPosition = text.length
            }
            onActiveFocusChanged: if (!activeFocus && bar.editing) bar.stopEditing()
        }

        PlainField {
            id: searchField
            Layout.fillWidth: true
            visible: bar.searching && !bar.editing
            font.pixelSize: 14
            // Room at the end for where it searches; padding isn't mirrored.
            leftPadding: LayoutMirroring.enabled ? scope.implicitWidth + 24 : 12
            rightPadding: LayoutMirroring.enabled ? 12 : scope.implicitWidth + 24
            // Where it searches, at the end of the field. Unlike a
            // placeholder, it stays while typing. Plugins' views have
            // nothing below them.
            Text {
                id: scope
                // Recents, Trash and plugins' views are named in C++, which
                // retranslating QML doesn't redo.
                readonly property string here: {
                    translator.language
                    return backend.crumbs.length > 0 ? backend.crumbs[backend.crumbs.length - 1].name : ""
                }
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: backend.path.startsWith("plugin:") ? qsTr("Search %1").arg(here) : qsTr("Search %1 and below").arg(here)
                color: theme.secondaryForeground
                font.pixelSize: 12
            }
            onTextEdited: searchDelay.restart()
            // Enter or ↓ goes to the results, leaving the search up.
            function toResults() {
                searchDelay.stop()
                backend.folder.query = text.trim()
                if (backend.folder.query === "")
                    bar.endSearch()
                else
                    bar.editDone()
            }
            onAccepted: toResults()
            Keys.onDownPressed: toResults()
            Keys.onEscapePressed: bar.endSearch()
        }

        Flickable {
            visible: !bar.editing && !bar.searching
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: Math.max(width, crumbs.implicitWidth)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // Long paths keep their end in view; that's where you are.
            // Mirrored, the end is on the left.
            function revealEnd() { contentX = LayoutMirroring.enabled ? 0 : Math.max(0, contentWidth - width) }
            onContentWidthChanged: revealEnd()
            onWidthChanged: revealEnd()
            LayoutMirroring.onEnabledChanged: revealEnd()

            Row {
                id: crumbs
                // A Row isn't placed by its Flickable, so it's moved to the
                // right edge by hand when mirrored.
                x: LayoutMirroring.enabled ? parent.width - width : 0
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Repeater {
                    // Recents, Trash and plugins' views are named in C++, which
                    // retranslating QML doesn't redo.
                    model: { translator.language; return backend.crumbs }
                    Row {
                        id: crumb
                        required property var modelData
                        required property int index
                        readonly property bool last: index === backend.crumbs.length - 1
                        // Centred on the row, which a folder's chip makes taller.
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10
                        Text {
                            // No separator after / itself: "/ run", not "/ / run".
                            visible: crumb.index > 0 && !(crumb.index === 1 && backend.crumbs[0].name === "/")
                            text: "/"
                            color: Qt.alpha(theme.foreground, 0.35)
                            font.pixelSize: 14
                        }
                        Text {
                            text: crumb.modelData.name
                            color: crumb.last ? theme.brightForeground : crumbMouse.containsMouse ? theme.foreground : theme.secondaryForeground
                            font.pixelSize: 14
                            font.weight: crumb.last ? Font.DemiBold : Font.Normal
                            MouseArea {
                                id: crumbMouse
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                // The folder you're in opens the path for typing, as Ctrl+L does.
                                cursorShape: crumb.last ? Qt.IBeamCursor : Qt.PointingHandCursor
                                onClicked: crumb.last ? bar.editPath() : backend.navigate(crumb.modelData.path)
                            }
                        }
                    }
                }

                // What a plugin says about the folder as a whole (the Git
                // branch it's on, and how much has changed), after where it
                // is. A button when the plugin gives it something to do.
                AbstractButton {
                    id: chip
                    readonly property var info: backend.chip
                    readonly property bool acts: !!info.action
                    visible: !!info.text
                    anchors.verticalCenter: parent.verticalCenter
                    implicitHeight: 24
                    implicitWidth: chipContent.implicitWidth + 20
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: acts
                    text: [info.text, info.detail, info.note].filter(part => !!part).join(" · ")
                    Accessible.role: acts ? Accessible.Button : Accessible.StaticText
                    Accessible.name: text
                    Accessible.description: info.actionLabel || ""
                    ToolTip.visible: hovered && acts
                    ToolTip.text: info.actionLabel || ""
                    ToolTip.delay: 600
                    onClicked: if (acts) backend.perform(info.plugin, info.action, [backend.path])
                    // Return and Space press it, as they do a dialog's buttons.
                    Keys.onShortcutOverride: (event) => event.accepted = !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier))
                                                                         && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter, Qt.Key_Escape].includes(event.key)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    Keys.onEscapePressed: bar.editDone()
                    // Gone with the keyboard on it (another folder), it
                    // hands the keyboard back.
                    onActsChanged: if (!acts && activeFocus) bar.editDone()
                    onVisibleChanged: if (!visible && activeFocus) bar.editDone()

                    background: Rectangle {
                        radius: theme.radius > 0 ? height / 2 : 0
                        color: Qt.alpha(theme.foreground, chip.hovered ? 0.12 : 0.06)
                        // The ring of the control with the keyboard.
                        Rectangle {
                            visible: chip.activeFocus
                            anchors.fill: parent
                            anchors.margins: -3
                            radius: parent.radius > 0 ? parent.radius + 3 : 0
                            color: "transparent"
                            border.width: 2
                            border.color: theme.accent
                        }
                    }
                    contentItem: Item {
                        Row {
                            id: chipContent
                            anchors.centerIn: parent
                            spacing: 6
                            Glyph {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !!chip.info.icon
                                name: chip.info.icon || "file"
                                size: 14
                                color: theme.secondaryForeground
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: chip.info.text || ""
                                color: theme.foreground
                                font.family: "monospace"
                                font.pixelSize: 12
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !!chip.info.detail
                                text: chip.info.detail || ""
                                color: theme.secondaryForeground
                                font.pixelSize: 12
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !!chip.info.note
                                text: "· " + (chip.info.note || "")
                                color: (chip.info.tone && theme.tones[chip.info.tone]) || theme.secondaryForeground
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
        }

        Item { implicitWidth: 16 }
        // Shows the current view; clicking moves to the next one.
        IconButton {
            glyph: bar.viewMode
            // Names what a click does, like the buttons beside it.
            text: bar.viewMode === "grid" ? qsTr("Show as list")
                : bar.viewMode === "list" ? qsTr("Show as columns")
                : qsTr("Show as grid")
            onClicked: bar.cycleView()
        }
        IconButton {
            id: sortButton
            // Shows the order in use, and says it on hover.
            glyph: sortMenu.current.glyph
            text: sortMenu.current.tip
            active: sortMenu.visible
            // Under the button, reaching toward the files.
            onClicked: sortMenu.popup(sortButton, LayoutMirroring.enabled ? sortButton.width - sortMenu.width : 0, sortButton.height + 4)
        }
        SortMenu { id: sortMenu }
        IconButton {
            glyph: "preview"
            // Only where the window has room to show them.
            visible: bar.previewFits
            text: qsTr("Details")
            active: bar.previewVisible
            onClicked: bar.togglePreview()
        }
        IconButton {
            visible: bar.closable
            glyph: "close"
            text: qsTr("Close this pane")
            onClicked: bar.closeRequested()
        }
    }

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }
}
