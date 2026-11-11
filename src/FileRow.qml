import QtQuick
import QtQuick.Layouts
import QtQuick.Effects

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
    required property bool selected

    property bool current: false
    // In a column to the left: the folder the next column is showing.
    property bool trail: false
    property bool detailed: false
    property bool chevron: false

    signal clicked(int modifiers)
    signal activated()

    implicitHeight: 32

    // Shared with the list's header so the columns line up. The name
    // always gets room: in a narrow window the date goes first, then the
    // size. (The icon says what kind a file is; there's no column for it.)
    readonly property int sizeWidth: 96
    readonly property int dateWidth: 96
    readonly property bool showDate: detailed && width >= 480
    readonly property bool showSize: detailed && width >= 380

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    // When it changed: the time for today, the date before that.
    function when(date) {
        if (!date)
            return ""
        const locale = Qt.locale(translator.language)
        const now = new Date()
        const today = date.getFullYear() === now.getFullYear() && date.getMonth() === now.getMonth() && date.getDate() === now.getDate()
        return today ? date.toLocaleTimeString(locale, Locale.ShortFormat) : date.toLocaleDateString(locale, Locale.ShortFormat)
    }

    function formatSize(bytes) {
        const units = [qsTr("B"), qsTr("KB"), qsTr("MB"), qsTr("GB"), qsTr("TB")]
        let i = 0
        let value = bytes
        while (value >= 1000 && i < units.length - 1) { value /= 1000; ++i }
        return value.toLocaleString(Qt.locale(), "f", i === 0 ? 0 : 1) + " " + units[i]
    }

    readonly property string glyph: row.isDir ? "folder"
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
        color: row.current || folderDrop.containsDrag ? row.tint(theme.accent, 0.18)
             : row.trail ? row.tint(theme.foreground, 0.08)
             : hover.hovered ? row.tint(theme.foreground, 0.05)
             : "transparent"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 12

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
                source: row.kind === "image" || row.kind === "video"
                    ? "image://thumb/128/" + encodeURIComponent(row.mime) + "/" + encodeURIComponent(row.path) : ""
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
        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            text: row.name
            color: row.current ? theme.brightForeground : theme.foreground
            elide: Text.ElideMiddle
        }
        Text {
            visible: row.showSize
            Layout.preferredWidth: row.sizeWidth
            horizontalAlignment: Text.AlignRight
            text: row.isDir ? (row.itemCount === 0 ? qsTr("Empty") : row.itemCount > 0 ? qsTr("%n items", "", row.itemCount) : "") : row.formatSize(row.size)
            color: row.current ? theme.foreground : theme.secondaryForeground
            font.pixelSize: 12
        }
        Text {
            visible: row.showDate
            Layout.preferredWidth: row.dateWidth
            Layout.leftMargin: 12
            text: row.when(row.modified)
            color: row.current ? theme.foreground : theme.secondaryForeground
            font.pixelSize: 12
            elide: Text.ElideRight
        }
        Glyph {
            visible: row.chevron && row.isDir
            name: "forward"
            size: 12
            color: theme.secondaryForeground
        }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: if (hovered) row.grabToImage(result => row.Drag.imageSource = result.url)
    }
    TapHandler {
        id: tap
        onTapped: row.clicked(tap.point.modifiers)
        onDoubleTapped: row.activated()
    }

    // Like the tiles: dragged, a row is a file for the sidebar or another app.
    Drag.active: drag.active
    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction | Qt.LinkAction
    // Dragging a selected item takes the whole selection along.
    Drag.mimeData: ({ "text/uri-list": (row.selected && backend.folder.selectedCount > 1 ? backend.folder.selectedPaths() : [row.path])
                                         .map(p => "file://" + encodeURI(p)).join("\r\n") })
    DragHandler { id: drag; target: null }
}
