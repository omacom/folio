#pragma once

#include <QImage>
#include <QString>

// Thumbnails from a file's own bytes: the picture an EPUB, comic book, OpenDocument, Office Open XML, 3MF, FreeCAD, Krita or OpenRaster file keeps in its zip, or a font's own letters.
namespace Embedded {

bool handles(const QString &mime, const QString &path);

// The kept picture, upright and at most `pixels` on its longer side, or the font's letters on a square of `pixels`; null when there is none.
QImage thumbnail(const QString &path, const QString &mime, int pixels);

} // namespace Embedded
