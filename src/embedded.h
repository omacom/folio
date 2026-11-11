#pragma once

#include <QImage>
#include <QString>

// Thumbnails from the picture a file keeps inside its own zip: EPUB, comic book, OpenDocument, Office Open XML, 3MF, FreeCAD, Krita and OpenRaster.
namespace Embedded {

bool handles(const QString &mime, const QString &path);

// The kept picture, upright and at most `pixels` on its longer side; null when the file holds none Folio can read.
QImage thumbnail(const QString &path, const QString &mime, int pixels);

} // namespace Embedded
