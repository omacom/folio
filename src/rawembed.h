#pragma once

#include <QImage>
#include <QString>

// Camera RAW thumbnails from the embedded JPEG, found in the TIFF directories of CR2 NEF NRW ARW SR2 DNG PEF.
namespace RawEmbed {

bool handles(const QString &mime);

// The largest valid embedded JPEG, upright and at most `pixels` on its longer side; null when the file holds none.
QImage thumbnail(const QString &path, int pixels);

} // namespace RawEmbed
