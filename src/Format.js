.pragma library
.import QtQml as QtQml

// Sizes, dates, counts and thumbnail URLs as the views show them, in one
// copy that every tile and row shares. A library can't see context
// properties, so a binding that shows a date reads translator.language
// itself to follow a language switch; whatever goes through qsTr here is
// refreshed with the rest of the strings.

// Each unit is whole with its number, so a language can place and space it
// its own way ("1,5 Mo"). Nothing for a size not read yet.
function size(bytes) {
    if (bytes === undefined)
        return ""
    const units = [qsTr("%1 B"), qsTr("%1 KB"), qsTr("%1 MB"), qsTr("%1 GB"), qsTr("%1 TB")]
    let i = 0
    let value = bytes
    while (value >= 1000 && i < units.length - 1) { value /= 1000; ++i }
    return units[i].arg(value.toLocaleString(Qt.locale(), "f", i === 0 ? 0 : 1))
}

// How much room is left on a drive, as its row in the sidebar says it.
function free(bytes) {
    return qsTr("%1 free").arg(size(bytes))
}

// When it changed: the time for today, the date before that. A plugin's
// items have no date.
function when(date) {
    if (!date || isNaN(date.getTime()))
        return ""
    const locale = Qt.locale()
    const now = new Date()
    const today = date.getFullYear() === now.getFullYear() && date.getMonth() === now.getMonth() && date.getDate() === now.getDate()
    return today ? date.toLocaleTimeString(locale, QtQml.Locale.ShortFormat) : date.toLocaleDateString(locale, QtQml.Locale.ShortFormat)
}

// A folder's contents; nothing until they've been counted.
function count(n) {
    return n === 0 ? qsTr("No items") : n > 0 ? qsTr("%Ln items", "", n) : ""
}

// How many match a search of `here` and below; `best` is how many of them are rows when another order cut the list, or -1; `most` is how many names were read of a tree that holds more, or -1.
function matches(n, here, reading, best, most) {
    if (reading)
        return qsTr("%Ln matches so far, still reading %1 and below", "", n).arg(here)
    // The counts go in first: a folder's name holding a marker is then never filled in again.
    if (most >= 0 && best >= 0)
        return qsTr("%Ln matches in %3 and below, the best %1 shown, only the nearest %2 items read", "", n).arg(best.toLocaleString(Qt.locale(), "f", 0)).arg(most.toLocaleString(Qt.locale(), "f", 0)).arg(here)
    if (most >= 0)
        return qsTr("%Ln matches in %2 and below, only the nearest %1 items read", "", n).arg(most.toLocaleString(Qt.locale(), "f", 0)).arg(here)
    if (best >= 0)
        return qsTr("%Ln matches in %2 and below, the best %1 shown", "", n).arg(best.toLocaleString(Qt.locale(), "f", 0)).arg(here)
    return qsTr("%Ln matches in %1 and below", "", n).arg(here)
}

// What an item is and how big, under its name in the details and the
// preview; for a plugin's item, its subtitle.
function summary(item, type) {
    if (!item || !item.path)
        return ""
    if (item.kind === "item")
        return item.extension
    if (item.isDir)
        return item.itemCount === 0 ? qsTr("Folder · No items") : item.itemCount > 0 ? qsTr("Folder · %Ln items", "", item.itemCount) : qsTr("Folder")
    return item.size === undefined ? (type || item.mime) : qsTr("%1 · %2").arg(type || item.mime).arg(size(item.size))
}

// The file's modified time is part of the URL: Qt keeps the images it has
// shown by their URL, so a picture saved over would otherwise come back
// with its old pixels.
function thumb(path, mime, px, modified) {
    // A time not read yet (undefined) gives no URL, or the picture would be asked for twice; null is a caller with no time.
    if (modified === undefined)
        return ""
    const changed = modified ? modified.getTime() : 0
    return "image://thumb/" + px + "/" + changed + "/" + encodeURIComponent(mime) + "/" + encodeURIComponent(path)
}

// A text file's first lines on a page in the theme's colors.
function page(path, px, modified, paper, ink) {
    // As for a thumbnail: a time not read yet gives no URL, or the page would be read and drawn twice.
    if (modified === undefined)
        return ""
    const changed = modified ? modified.getTime() : 0
    const hex = (color) => String(color).slice(1)
    return "image://thumb/page/" + px + "/" + changed + "/" + hex(paper) + "/" + hex(ink) + "/" + encodeURIComponent(path)
}

// A key combination as people read it: "Ctrl + C", not "Ctrl+C".
function keys(text) {
    return text.replace(/(\S)\+(?=\S)/g, "$1 + ")
}
