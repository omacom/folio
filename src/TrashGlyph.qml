import QtQuick
import QtQuick.Shapes

// The trash's icon in its two states: the plain bin when it's empty, and,
// when something's in it, the bin with its lid ajar and a small tied bag at
// its bottom right. Nothing moves; the icon just changes when `hasItems` does.
//
// Empty is the Glyph's own "trash", so it's the same icon to the pixel. The
// bag is solid, with a ring of `gap` round it: the color of what's behind the
// icon, so it's cut clear of the bin instead of running into it.
Item {
    id: bin

    property bool hasItems: false
    property color color: theme.foreground
    // What the icon sits on, for the ring round the bag.
    property color gap: theme.background
    property real size: 18
    property real stroke: 1.6

    implicitWidth: size
    implicitHeight: size

    Glyph {
        anchors.fill: parent
        visible: !bin.hasItems
        name: "trash"
        color: bin.color
        size: bin.size
        stroke: bin.stroke
    }

    Shape {
        anchors.fill: parent
        visible: bin.hasItems
        preferredRendererType: Shape.CurveRenderer

        // The bin: its body as drawn when empty, and its lid, which is the
        // closed one turned 12° about its left end, so it hangs open from
        // that corner.
        ShapePath {
            strokeColor: bin.color
            fillColor: "transparent"
            strokeWidth: bin.stroke
            capStyle: theme.radius > 0 ? ShapePath.RoundCap : ShapePath.SquareCap
            joinStyle: theme.radius > 0 ? ShapePath.RoundJoin : ShapePath.MiterJoin
            scale: Qt.size(bin.size / 24, bin.size / 24)
            PathSvg { path: "M6 7l1 13h10l1-13M4 7L19.65 3.67M8.89 5.96L8.27 3.03L14.14 1.78L14.76 4.71" }
        }

        // The gap: the bag's outline, grown by 1.5 pixels all round (a stroke
        // is centred on the line, so 3 wide), painted over the bin.
        ShapePath {
            strokeColor: bin.gap
            fillColor: bin.gap
            strokeWidth: 3
            joinStyle: ShapePath.RoundJoin
            scale: Qt.size(bin.size / 24, bin.size / 24)
            PathSvg { path: bag.path }
        }

        // The bag: a bulging sack, its neck tied in a knot with two ears.
        ShapePath {
            id: bag
            readonly property string path: "M16.16 13.93L14.6 10.81L17.98 12.5L21.36 10.81L19.8 13.93C22.4 15.1 23.7 17.83 22.79 20.69C22.27 22.25 21.1 22.77 19.8 22.77H16.16C14.86 22.77 13.69 22.25 13.17 20.69C12.26 17.83 13.56 15.1 16.16 13.93Z"
            strokeColor: "transparent"
            fillColor: bin.color
            scale: Qt.size(bin.size / 24, bin.size / 24)
            PathSvg { path: bag.path }
        }
    }
}
