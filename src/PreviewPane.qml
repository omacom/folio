import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A larger look at the selected item and the facts about it, with the one
// action that matters: open it.
Rectangle {
    id: pane

    property var item: ({})
    readonly property bool hasItem: item !== undefined && item.path !== undefined
    // An item in the trash shows where it came from and when it was
    // deleted, and offers Restore rather than Open.
    readonly property bool trashed: hasItem && fileOps.inTrash(item.path) && fileOps.originalPath(item.path).length > 0
    function homeless(path) { return path.startsWith(places.home + "/") ? "~" + path.slice(places.home.length) : path }
    readonly property var info: hasItem ? backend.details(item.path, item.kind, item.mime) : ({})

    color: theme.darkBackground

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    function formatSize(bytes) {
        const units = [qsTr("B"), qsTr("KB"), qsTr("MB"), qsTr("GB"), qsTr("TB")]
        let i = 0
        let value = bytes
        while (value >= 1000 && i < units.length - 1) { value /= 1000; ++i }
        return value.toLocaleString(Qt.locale(), "f", i === 0 ? 0 : 1) + " " + units[i]
    }

    function thumbUrl(file, type, size) {
        return "image://thumb/" + size + "/" + encodeURIComponent(type) + "/" + encodeURIComponent(file)
    }

    Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: pane.tint(theme.foreground, 0.12) }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 22
        visible: pane.hasItem

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width * 0.75)

            Image {
                id: big
                anchors.fill: parent
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                source: pane.hasItem && !pane.item.isDir && pane.item.kind !== "text" ? pane.thumbUrl(pane.item.path, pane.item.mime, 512) : ""
                visible: status === Image.Ready
            }

            // Text files show their opening lines instead of a picture.
            Rectangle {
                anchors.fill: parent
                visible: pane.hasItem && pane.item.kind === "text"
                color: theme.background
                border.width: 1
                border.color: pane.tint(theme.foreground, 0.12)
                clip: true
                Text {
                    anchors.fill: parent
                    anchors.margins: 14
                    text: pane.info.text || ""
                    color: theme.foreground
                    font.pixelSize: 10
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                }
            }

            Glyph {
                anchors.centerIn: parent
                visible: !big.visible && pane.hasItem && pane.item.kind !== "text"
                name: pane.hasItem && pane.item.isDir ? "folder" : "file"
                size: 72
                stroke: 1.2
                color: pane.hasItem && pane.item.isDir ? theme.accent : theme.secondaryForeground
            }
        }

        ColumnLayout {
            spacing: 4
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: pane.item.name || ""
                color: theme.brightForeground
                font.pixelSize: 15
                font.weight: Font.DemiBold
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: !pane.hasItem ? ""
                    : pane.item.isDir ? (pane.item.itemCount === 0 ? qsTr("Folder · Empty") : pane.item.itemCount > 0 ? qsTr("Folder · %n items", "", pane.item.itemCount) : qsTr("Folder"))
                    : (pane.info.type || pane.item.mime) + " · " + pane.formatSize(pane.item.size)
                color: theme.secondaryForeground
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            rowSpacing: 12
            columnSpacing: 16

            Text { visible: pane.trashed; text: qsTr("From"); color: theme.secondaryForeground; font.pixelSize: 12 }
            Text {
                visible: pane.trashed
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.trashed ? pane.homeless(fileOps.originalPath(pane.item.path).replace(/\/[^\/]*$/, "") || "/") : ""
                color: theme.foreground
                font.pixelSize: 12
                elide: Text.ElideMiddle
            }
            Text { text: pane.trashed ? qsTr("Deleted") : qsTr("Modified"); color: theme.secondaryForeground; font.pixelSize: 12 }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: !pane.hasItem ? ""
                    : (pane.trashed ? fileOps.deletedAt(pane.item.path) : pane.item.modified).toLocaleString(Qt.locale(translator.language), Locale.ShortFormat)
                color: theme.foreground
                font.pixelSize: 12
            }
            Text { visible: !!pane.info.dimensions; text: qsTr("Dimensions"); color: theme.secondaryForeground; font.pixelSize: 12 }
            Text {
                visible: !!pane.info.dimensions
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.info.dimensions || ""
                color: theme.foreground
                font.pixelSize: 12
            }
        }

        Item { Layout.fillHeight: true }

        AbstractButton {
            id: openButton
            Layout.fillWidth: true
            implicitHeight: 40
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            text: pane.trashed ? qsTr("Restore") : pane.hasItem && pane.item.isDir ? qsTr("Open folder") : qsTr("Open")
            Accessible.name: text
            onClicked: pane.trashed ? fileOps.restore([pane.item.path]) : backend.open(pane.item.path)
            background: Rectangle {
                radius: Math.min(4, theme.radius)
                color: openButton.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent
            }
            contentItem: RowLayout {
                Text { Layout.leftMargin: 14; Layout.fillWidth: true; text: openButton.text; color: theme.accentForeground; font.weight: Font.DemiBold }
                Text { Layout.rightMargin: 14; text: "⏎"; color: theme.accentForeground }
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: !pane.hasItem
        text: qsTr("Nothing selected")
        color: theme.secondaryForeground
    }
}
