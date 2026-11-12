import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import "Format.js" as Format

// One item as a row, for the list and column views: a small picture or
// glyph, the name, and in the list the size and date beside it.
Item {
    id: row

    required property int index
    required property string name
    required property string path
    required property bool isDir
    required property string kind
    required property string mime
    required property string extension
    required property var size
    required property var modified
    required property int itemCount
    required property string icon
    required property bool selected
    required property int depth
    required property bool open
    required property string lines

    property bool current: false
    // In a column to the left: the folder the next column is showing.
    property bool trail: false
    property bool detailed: false
    property bool chevron: false
    // In the list with the tree on: rows indented by how deep in the tree
    // they are, joined by thin lines to the folder they're in, and an arrow
    // on folders to open them in place. Off, the row is as it always was.
    property bool tree: false

    signal clicked(int modifiers)
    signal activated()
    // The arrow was clicked (or pressed by a screen reader).
    signal toggled()

    implicitHeight: 32

    // The name always gets room: in a narrow window the date goes first,
    // then the size. (The icon says what kind a file is; there's no column
    // for it.)
    readonly property bool showDate: detailed && width >= 480
    readonly property bool showSize: detailed && width >= 380

    readonly property string glyph: row.isDir ? "folder"
                                  : row.kind === "item" ? row.icon
                                  : row.kind === "image" ? "pictures"
                                  : row.kind === "video" ? "videos"
                                  : row.kind === "audio" ? "music"
                                  : row.kind === "pdf" || row.kind === "text" ? "documents"
                                  : "file"

    // A folder row takes files dropped on it, lighting up while they hover.
    DropArea {
        id: folderDrop
        anchors.fill: parent
        enabled: row.isDir
        keys: ["text/uri-list"]
        onEntered: (drag) => { drag.accepted = fileOps.canDrop(drag.urls, row.path) }
        onDropped: (drop) => { fileOps.dropInto(drop.urls, row.path); drop.accept(Qt.MoveAction) }
    }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        radius: Math.min(4, theme.radius)
        border.width: folderDrop.containsDrag ? 2 : 0
        border.color: theme.accent
        color: row.current || folderDrop.containsDrag ? Qt.alpha(theme.accent, 0.18)
             : row.trail ? Qt.alpha(theme.foreground, 0.08)
             : hover.hovered ? Qt.alpha(theme.foreground, 0.05)
             : "transparent"
    }

    // The lines, one 16 px step for each level in, like the tree command
    // draws them: the vertical ones run the row's full height so they join
    // the rows above and below. Drawn left to right and flipped whole when
    // mirrored, so they lead from the arrows on the other side.
    Item {
        objectName: "treeLines"
        visible: row.tree && row.depth > 0
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: row.depth * 16 + 16
        transform: Scale { xScale: row.LayoutMirroring.enabled ? -1 : 1; origin.x: (row.depth * 16 + 16) / 2 }
        Repeater {
            model: row.tree ? row.lines.split("") : []
            Item {
                required property int index
                required property string modelData
                readonly property bool elbow: modelData === "├" || modelData === "└"
                x: index * 16
                width: 16
                height: parent.height
                Rectangle {
                    objectName: "treeGuide"
                    visible: modelData !== " "
                    x: 7
                    width: 1
                    height: modelData === "└" ? Math.round(parent.height / 2) : parent.height
                    color: Qt.alpha(theme.foreground, 0.25)
                }
                // Over to this row's arrow, or for a file, which has none, on
                // through the arrow's room to its icon.
                Rectangle {
                    visible: parent.elbow
                    x: 8
                    y: Math.round(parent.height / 2)
                    width: row.isDir ? 6 : 20
                    height: 1
                    color: Qt.alpha(theme.foreground, 0.25)
                }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 12

        // One step in for each open folder above, then the arrow. A file has
        // the same room without one, so the names line up.
        Item {
            visible: row.tree
            implicitWidth: row.depth * 16 + 16
            implicitHeight: 16
            Item {
                id: disclosure
                objectName: "disclosure"
                visible: row.isDir
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                Accessible.role: Accessible.Button
                Accessible.name: row.open ? qsTr("Collapse %1").arg(row.name) : qsTr("Expand %1").arg(row.name)
                Accessible.onPressAction: row.toggled()
                // Points along the row, and down once the folder is open.
                Glyph {
                    anchors.centerIn: parent
                    name: row.LayoutMirroring.enabled ? "back" : "forward"
                    rotation: row.open ? (row.LayoutMirroring.enabled ? -90 : 90) : 0
                    size: 12
                    color: theme.secondaryForeground
                }
            }
        }
        Item {
            implicitWidth: 20
            implicitHeight: 20
            Image {
                id: thumb
                anchors.fill: parent
                layer.enabled: theme.radius > 0 && status === Image.Ready
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: rowMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1
                }
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                source: row.kind === "image" || row.kind === "video" ? Format.thumb(row.path, row.mime, 128, row.modified) : ""
                visible: status === Image.Ready
            }
            Rectangle {
                id: rowMask
                width: 20
                height: 20
                radius: Math.min(4, theme.radius)
                visible: false
                layer.enabled: true
            }
            Glyph {
                anchors.centerIn: parent
                visible: !thumb.visible
                name: row.glyph
                size: 16
                color: row.isDir ? theme.accent : theme.secondaryForeground
            }
        }
        // The name and the date are aligned explicitly, so that they're
        // mirrored: left as they are, they'd follow their own script, and a
        // Latin name would sit at the far end of a right-to-left row.
        Text {
            objectName: "rowName"
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            horizontalAlignment: Text.AlignLeft
            text: row.name
            color: row.current ? theme.accent : theme.foreground
            elide: Text.ElideMiddle
        }
        Text {
            visible: row.showSize
            Layout.preferredWidth: 96
            horizontalAlignment: Text.AlignRight
            text: row.kind === "item" ? row.extension : row.isDir ? Format.count(row.itemCount) : Format.size(row.size)
            color: theme.secondaryForeground
            font.pixelSize: 12
        }
        Text {
            visible: row.showDate
            Layout.preferredWidth: 96
            Layout.leftMargin: 12
            horizontalAlignment: Text.AlignLeft
            text: {
                translator.language // Re-read when the language (and so the default locale) changes.
                return Format.when(row.modified)
            }
            color: theme.secondaryForeground
            font.pixelSize: 12
            elide: Text.ElideRight
        }
        // Points to where the next column opens.
        Glyph {
            visible: row.chevron && row.isDir
            name: row.LayoutMirroring.enabled ? "back" : "forward"
            size: 12
            color: theme.secondaryForeground
        }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: if (hovered) row.grabToImage(result => row.Drag.imageSource = result.url)
    }
    // A click on the arrow (or a little either side of it, the row's full
    // height) only opens or closes the folder: one handler for the whole
    // row, so the click is never taken as both.
    function atArrow(position) {
        if (!row.tree || !disclosure.visible)
            return false
        const at = disclosure.mapFromItem(row, position.x, position.y)
        return at.x >= -6 && at.x < disclosure.width + 6
    }
    TapHandler {
        id: tap
        onTapped: row.atArrow(tap.point.position) ? row.toggled() : row.clicked(tap.point.modifiers)
        onDoubleTapped: if (!row.atArrow(tap.point.position)) row.activated()
    }

    // Like the tiles: dragged, a row is a file for the sidebar or another app.
    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction | Qt.LinkAction
    // The pointer holds the picture by its icon and name, which are at its
    // right end when mirrored.
    Drag.hotSpot.x: LayoutMirroring.enabled ? row.width : 0
    DragHandler {
        target: null
        enabled: row.kind !== "item"
        // Dragging a selected item takes the whole selection along. The list
        // is built as the drag starts, not again on every change of selection.
        onActiveChanged: {
            if (active)
                row.Drag.mimeData = { "text/uri-list": fileOps.uriList(row.selected && backend.folder.selectedCount > 1 ? backend.folder.selectedPaths() : [row.path]) }
            row.Drag.active = active
        }
    }
}
