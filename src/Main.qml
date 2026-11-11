import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtCore

// The window: one bar across the top, under it the sidebar, one folder pane
// or two side by side (Ctrl+Alt+N splits, Ctrl+W closes the right one),
// and the details pane. Each pane's path sits in the bar above its files;
// the sidebar and the details follow the pane used last.
ApplicationWindow {
    id: win
    width: 1280
    height: 800
    minimumWidth: 720
    minimumHeight: 460
    visible: true
    title: activePane && activePane.paneBackend.title.length > 0 ? activePane.paneBackend.title : "Files"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.foreground: theme.foreground
    Material.background: theme.background
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool previewVisible: true
    // Columns on a first launch; after that, whichever view was used last.
    property string viewMode: "columns"
    property real zoom: 1
    // The bookmarks bar under the path (Ctrl+Shift+B), and how much room it
    // takes, which the sidebar, panes and details make below it.
    property bool bookmarksBarVisible: true
    // Hidden with the keys on it (any way: shortcut, its own menu): they go
    // to the files.
    onBookmarksBarVisibleChanged: if (!bookmarksBarVisible && bookmarksBar.activeFocus && activePane) activePane.focusView()
    readonly property int bookmarksBarHeight: bookmarksBarVisible ? bookmarksBar.implicitHeight : 0
    // Bookmark dialogs, the bubble, or the keyboard on the bar (F6): the
    // panes' single-key shortcuts stand aside.
    readonly property bool bookmarking: bookmarkDialog.visible || bookmarkConfirm.visible || bookmarkBubble.visible
                                        || bookmarksBar.activeFocus
    // The folder the star last put a bookmark in, as a browser remembers.
    property string lastBookmarkFolder: ""

    // The pane keys, the sidebar and the details go to.
    property var activePane: leftPane
    // The right pane while the window is split.
    property var rightPane: null
    readonly property bool split: rightPane !== null
    // Whether keys are going into a text field. The right pane can go before
    // the window does when it closes, leaving activePane null.
    readonly property bool typing: (!!activePane && activePane.typing) || bookmarking
    // The rightmost pane's path bar reaches over the details column.
    readonly property int detailsWidth: details.visible ? details.width : 0
    // Whether the window is wide enough for the details column at all.
    readonly property bool detailsFit: width >= (split ? 1400 : 960)

    // Remembers the view between launches (~/.config/folio/folio.conf).
    Settings {
        property alias viewMode: win.viewMode
        property alias zoom: win.zoom
        property alias bookmarksBarVisible: win.bookmarksBarVisible
        property alias lastBookmarkFolder: win.lastBookmarkFolder
    }

    // Ctrl+B or the star: bookmarks a path right away and opens the bubble
    // to name it, or edits its bookmark when it has one, as a browser does.
    function bookmark(path, anchor) {
        if (!bookmarkBubble.openFor(path, anchor || (activePane ? activePane.bookmarkStar : null)) && activePane)
            activePane.paneBackend.showMessage(bookmarks.problem.length > 0 ? bookmarks.problem
                                                                            : qsTr("Couldn't bookmark %1.").arg(path))
    }
    // A folder of bookmarks asks first; a bookmark just goes.
    function deleteBookmark(id) {
        const item = bookmarks.get(id)
        if (item.type === "folder" && item.children.length > 0)
            bookmarkConfirm.ask(qsTr("Delete this folder?"),
                                qsTr("%1 and the %n bookmarks and folders in it will be deleted.", "", item.children.length).arg(item.name),
                                qsTr("Delete"), () => bookmarks.remove(id))
        else
            bookmarks.remove(id)
    }

    // Ctrl+Alt+N: a second pane on the right, starting where the active
    // one is. Pressed again, it moves the keyboard there.
    function openSplit() {
        if (!rightPane) {
            rightPane = windows.openSplit(win, activePane.paneBackend.path)
            if (!rightPane)
                return
            rightPane.parent = splitHost
            rightPane.anchors.fill = splitHost
            rightPane.closable = true
            rightPane.viewMode = activePane.viewMode
            rightPane.zoom = activePane.zoom
            rightPane.barExtension = Qt.binding(() => win.detailsWidth)
            rightPane.closeRequested.connect(closeSplit)
        }
        rightPane.focusView()
    }
    function closeSplit() {
        if (!rightPane)
            return
        activePane = leftPane
        rightPane = null
        windows.closeSplit(win)
        leftPane.focusView()
    }

    Shortcut { sequence: "?"; enabled: !win.typing || win.helpVisible; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; enabled: !win.typing; onActivated: win.close() }
    Shortcut { sequence: "Ctrl+Alt+N"; enabled: !win.typing; onActivated: win.openSplit() }
    Shortcut { sequence: "Ctrl+W"; enabled: win.split; onActivated: win.closeSplit() }
    Shortcut { sequence: "Ctrl+I"; onActivated: win.previewVisible = !win.previewVisible }
    // Also with the keys on the bar (F6), as a browser's bookmark keys work
    // from its toolbar.
    Shortcut {
        sequence: "Ctrl+B"
        enabled: (!win.typing || bookmarksBar.activeFocus) && !!win.activePane
        onActivated: win.bookmark(win.activePane.paneBackend.path)
    }
    Shortcut {
        sequence: "Ctrl+Shift+B"
        enabled: !win.typing || bookmarksBar.activeFocus
        onActivated: {
            // Hiding the bar the keys are on hands them back to the files.
            if (bookmarksBar.activeFocus && win.activePane)
                win.activePane.focusView()
            win.bookmarksBarVisible = !win.bookmarksBarVisible
        }
    }
    // F6, as in a browser: the keyboard to the bookmarks bar.
    Shortcut { sequence: "F6"; enabled: !win.typing && win.bookmarksBarVisible; onActivated: bookmarksBar.focusBar() }
    // Tab moves the keyboard: sidebar, left pane, right pane, and around.
    Shortcut {
        sequences: ["Tab", "Backtab"]
        enabled: !win.typing
        onActivated: {
            if (sidebar.activeFocus)
                leftPane.focusView()
            else if (win.split && win.activePane === leftPane)
                win.rightPane.focusView()
            else
                sidebar.focusSidebar()
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        // The bar across the top; the panes' path bars draw over it.
        readonly property int barHeight: 60
        Rectangle {
            width: parent.width
            height: parent.barHeight
            color: theme.background
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }
        }

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Sidebar {
                id: sidebar
                target: win.activePane ? win.activePane.paneBackend : backend
                onFinished: win.activePane.focusView()
                onHelpRequested: win.helpVisible = !win.helpVisible
                Layout.topMargin: parent.parent.barHeight + win.bookmarksBarHeight
                Layout.preferredWidth: 190
                Layout.fillHeight: true
            }
            Rectangle { Layout.topMargin: parent.parent.barHeight + win.bookmarksBarHeight; Layout.fillHeight: true; implicitWidth: 1; color: Qt.alpha(theme.foreground, 0.12) }

            FolderPane {
                id: leftPane
                barExtension: win.split ? 0 : win.detailsWidth
                barLeft: sidebar.width + 1
                Layout.fillWidth: true
                Layout.fillHeight: true
                focus: true
            }

            // openSplit() puts the right pane of a split here. Between the
            // panes the divider runs up through the top bar, so each path
            // reads as belonging to the files below it.
            Rectangle { visible: win.split; Layout.fillHeight: true; implicitWidth: 1; color: Qt.alpha(theme.foreground, 0.12) }
            Item {
                id: splitHost
                visible: win.split
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            PreviewPane {
                id: details
                visible: win.previewVisible && win.detailsFit
                Layout.topMargin: parent.parent.barHeight + win.bookmarksBarHeight
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                folderPane: win.activePane
            }
        }

        // Across the whole window, under the path bars, over the panes'
        // room for it (FolderPane leaves bookmarksBarHeight free).
        BookmarksBar {
            id: bookmarksBar
            y: parent.barHeight
            width: parent.width
            visible: win.bookmarksBarVisible
            target: win.activePane ? win.activePane.paneBackend : null
            targetFileOps: win.activePane ? win.activePane.paneFileOps : null
            onAddRequested: (path, parentId, index) => bookmarkDialog.add(path, parentId, index)
            onEditRequested: (id) => bookmarkDialog.edit(id)
            onNewFolderRequested: (parentId, index) => bookmarkDialog.newFolder(parentId, index)
            onDeleteRequested: (id) => win.deleteBookmark(id)
            onHideRequested: win.bookmarksBarVisible = false
            onDone: if (win.activePane) win.activePane.focusView()
            quiet: bookmarkDialog.visible || bookmarkConfirm.visible || bookmarkBubble.visible
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onDismissed: win.helpVisible = false
        }

        BookmarkDialog {
            id: bookmarkDialog
            anchors.fill: parent
            completer: win.activePane ? win.activePane.paneBackend : null
            onVisibleChanged: if (!visible && win.activePane) win.activePane.focusView()
        }
        BookmarkBubble {
            id: bookmarkBubble
            parent: Overlay.overlay
            lastFolder: win.lastBookmarkFolder
            onFolderUsed: (id) => win.lastBookmarkFolder = id
            onClosed: if (win.activePane) win.activePane.focusView()
        }
        ConfirmDialog {
            id: bookmarkConfirm
            anchors.fill: parent
            onVisibleChanged: if (!visible && win.activePane) win.activePane.focusView()
        }
    }
}
