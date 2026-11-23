import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import QtQuick.Window

// The list's hierarchy lines, in the style picked in the top bar: from each
// open folder a guide runs down under its arrow and turns with a quarter
// circle into each of its items. They're worked out from the rows' depths,
// not the delegates, since a guide crosses rows the list hasn't made.
Item {
    id: lines

    // The list; its model gives the rows.
    required property var view

    // Off in a window that keeps no line style.
    readonly property string style: ApplicationWindow.window?.effectiveLineStyle ?? "off"

    // What the drawing binds to: the lines' width, opacity and colours, and
    // for trace the route drawn over them. Quiet and trace need more of the
    // colour on a light theme; graph colours each level apart at one opacity.
    readonly property var spec: {
        switch (style) {
        case "graph":
            return { width: 1.5, opacity: 0.78, colors: [theme.accent, theme.magenta, theme.cyan, theme.orange], route: null }
        case "trace":
            return { width: 1, opacity: theme.light ? 0.24 : 0.14, colors: [theme.foreground], route: { width: 1.5, opacity: 0.9, color: theme.accent } }
        default:
            return { width: 1, opacity: theme.light ? 0.30 : 0.18, colors: [theme.foreground], route: null }
        }
    }

    // FileRow's measures: the row, its picture and how far in each level is.
    readonly property int picture: Math.round(20 * view.zoom)
    // Not called rowHeight: the tests find the list by that property's name.
    readonly property int rowStep: Math.round(32 * view.zoom)
    readonly property int indent: Math.round(picture / 2) + 10
    // The radius of the turn from a guide into an item.
    readonly property int bend: 8

    y: view.originY
    width: view.width

    QtObject {
        id: built
        // For each row: its depth, whether it's a folder, the row it's under.
        property var depths: []
        property var folders: []
        property var parents: []
        property var guides: []
        // The lines and the dots, one path for each colour of the style.
        property var paths: []
        property var dotPaths: []
        // Trace's route, from the top-level folder down to the cursor's row.
        property var route: []
        property string routePath: ""
        property string routeDotPath: ""
    }

    // Reads the rows in one call, unless the lines are off or folders don't
    // open in place here; a row is under the last row seen a level up.
    function walk() {
        const outline = style !== "off" && view.model.expandable ? view.model.outline() : null
        const depths = outline ? outline.depths : []
        const parents = [], last = []
        for (let row = 0; row < depths.length; ++row) {
            const depth = depths[row]
            parents.push(depth > 0 ? last[depth - 1] : -1)
            last[depth] = row
        }
        built.depths = depths
        built.folders = outline ? outline.folders : []
        built.parents = parents
        layout()
    }

    // The guides, in row order, from the rows read last. The x measures are
    // taken from a row's left edge, where FileRow puts its arrow, an item
    // 18 wide with the glyph centred on 9; an item's row starts one indent
    // further in, and its picture 22 after that.
    function layout() {
        const graph = style === "graph"
        const found = [], guideAt = []
        const paths = ["", "", "", ""], dotPaths = ["", "", "", ""]
        const across = x => LayoutMirroring.enabled ? width - x : x
        const depths = built.depths, folders = built.folders, parents = built.parents
        for (let row = 0; row < parents.length; ++row) {
            const parent = parents[row]
            if (parent < 0)
                continue
            const depth = depths[parent]
            const left = 4 + depth * indent
            const color = graph ? depth % 4 : 0
            let guide = guideAt[parent]
            if (!guide) {
                // 6 under the arrow's centre, which is the row's centre
                // unless the row is shorter than the arrow's 32.
                const y0 = parent * rowStep + Math.max(rowStep, 32) / 2 + 6
                // Under the arrow's centre, on half a pixel so a 1 px line
                // covers one column of pixels.
                guide = { row: parent, depth: depth, x: across(left + 9.5), y0: y0, y1: 0, curves: [] }
                guideAt[parent] = guide
                found.push(guide)
                // Graph marks where each guide starts, just under its top.
                if (graph)
                    dotPaths[color] += circle(guide.x, y0 + 2.5, 2)
            }
            const y = row * rowStep + rowStep / 2
            // A file's line stops 5 short of its picture. A folder's stops
            // 4 short of its arrow glyph, whose left edge is 6 in; in graph
            // it stops where the arrow item starts, under the folder's dot.
            let end = left + indent
            if (!folders[row])
                end += 17
            else if (!graph)
                end += 2
            const endX = across(end)
            guide.curves.push({ row: row, y: y, endX: endX })
            guide.y1 = y - bend
            paths[color] += curve(guide.x, y, endX)
            if (graph && folders[row])
                dotPaths[color] += circle(endX, y, 2.5)
        }
        for (const guide of found)
            paths[graph ? guide.depth % 4 : 0] += "M" + guide.x + " " + guide.y0 + "V" + guide.y1
        built.guides = found
        built.paths = paths
        built.dotPaths = dotPaths
        retrace()
    }

    // From a guide at x, a quarter circle from a bend above a child's centre
    // line to a bend along it, turning with the reading direction, then level
    // to its end.
    function curve(x, y, endX) {
        const mirror = LayoutMirroring.enabled
        return "M" + x + " " + (y - bend)
            + "A" + bend + " " + bend + " 0 0 " + (mirror ? 1 : 0) + " " + (x + (mirror ? -bend : bend)) + " " + y
            + "H" + endX
    }

    // A dot of radius r at x, y as a closed path: two half circles.
    function circle(x, y, r) {
        return "M" + (x - r) + " " + y + "a" + r + " " + r + " 0 1 0 " + 2 * r + " 0"
            + "a" + r + " " + r + " 0 1 0 " + -2 * r + " 0Z"
    }

    // Trace's route: the cursor's row and the folders it's in, each one's
    // guide drawn again down to the next one's curve, and a dot at its end.
    function retrace() {
        const chain = []
        for (let r = view.currentIndex; spec.route && r >= 0 && r < built.parents.length; r = built.parents[r])
            chain.unshift(r)
        let path = "", end = null
        for (let i = 1; i < chain.length; ++i) {
            const guide = built.guides.find(g => g.row === chain[i - 1])
            end = guide.curves.find(c => c.row === chain[i])
            path += "M" + guide.x + " " + guide.y0 + "V" + (end.y - bend) + curve(guide.x, end.y, end.endX)
        }
        built.route = end ? chain : []
        built.routePath = path
        built.routeDotPath = end ? circle(end.endX, end.y, 2.5) : ""
    }

    // Rows can change more than once in a turn (an opening removes rows a
    // re-read listed before inserting its own), so they're read once after.
    function reread() { Qt.callLater(walk) }
    // A new theme, zoom or direction lays the rows read last out again; so
    // does a new width, which moves the lines only when they're mirrored.
    function relayout() { Qt.callLater(layout) }

    // A new style reads the rows again, since off reads none.
    onStyleChanged: reread()
    onSpecChanged: relayout()
    onWidthChanged: if (LayoutMirroring.enabled) relayout()
    LayoutMirroring.onEnabledChanged: relayout()
    Component.onCompleted: reread()

    Connections {
        target: lines.view.model
        // Rows added, removed or replaced, or a change in whether folders
        // open in place here.
        function onRowsInserted() { lines.reread() }
        function onRowsRemoved() { lines.reread() }
        function onModelReset() { lines.reread() }
        function onExpandableChanged() { lines.reread() }
    }
    Connections {
        target: lines.view
        function onCurrentIndexChanged() { lines.retrace() }
        function onZoomChanged() { lines.relayout() }
    }

    // Strokes and dots, one path per colour, drawn opaque and given their
    // opacity as a whole layer: a curve leaves its guide along it, and
    // translucent strokes would be darker where they overlap.
    component Layer: Item {
        id: tile
        property real lineWidth: 1
        property var colors: []
        property var paths: []
        property var dotPaths: []
        // A style with one colour leaves paths 1 to 3 empty; they take it too.
        function color(i) {
            return colors[i % colors.length]
        }
        anchors.fill: parent
        // The software renderer loses a layer's contents above scale 1, so
        // there the strokes go without one, their joins slightly stronger.
        layer.enabled: visible && (GraphicsInfo.api !== GraphicsInfo.Software || Screen.devicePixelRatio === 1)
        Shape {
            // Back in the lines' own coordinates, where the paths are.
            y: -tile.parent.y
            preferredRendererType: Shape.CurveRenderer
            Stroke { path: tile.paths[0] || ""; strokeColor: tile.color(0); strokeWidth: tile.lineWidth }
            Stroke { path: tile.paths[1] || ""; strokeColor: tile.color(1); strokeWidth: tile.lineWidth }
            Stroke { path: tile.paths[2] || ""; strokeColor: tile.color(2); strokeWidth: tile.lineWidth }
            Stroke { path: tile.paths[3] || ""; strokeColor: tile.color(3); strokeWidth: tile.lineWidth }
            Fill { path: tile.dotPaths[0] || ""; fillColor: tile.color(0) }
            Fill { path: tile.dotPaths[1] || ""; fillColor: tile.color(1) }
            Fill { path: tile.dotPaths[2] || ""; fillColor: tile.color(2) }
            Fill { path: tile.dotPaths[3] || ""; fillColor: tile.color(3) }
        }
    }
    component Stroke: ShapePath {
        property alias path: svg.path
        fillColor: "transparent"
        capStyle: ShapePath.FlatCap
        joinStyle: ShapePath.RoundJoin
        PathSvg { id: svg }
    }
    component Fill: ShapePath {
        property alias path: svg.path
        strokeColor: "transparent"
        strokeWidth: -1
        PathSvg { id: svg }
    }

    // Only the part of the list in view is layered, so the textures stay
    // small however long the list; scrolling moves the paths, not rebuilds them.
    Item {
        y: lines.view.contentY - lines.y
        width: lines.width
        height: lines.view.height
        Layer {
            visible: built.guides.length > 0
            opacity: lines.spec.opacity
            lineWidth: lines.spec.width
            colors: lines.spec.colors
            paths: built.paths
            dotPaths: built.dotPaths
        }
        Layer {
            visible: built.route.length > 0
            opacity: lines.spec.route ? lines.spec.route.opacity : 0
            lineWidth: lines.spec.route ? lines.spec.route.width : 1
            colors: [lines.spec.route ? lines.spec.route.color : "transparent"]
            paths: [built.routePath]
            dotPaths: [built.routeDotPath]
        }
    }
}
