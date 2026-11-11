#pragma once

#include <QImage>
#include <QString>

// Camera RAW thumbnails from the embedded JPEG, found in the TIFF directories of CR2 NEF NRW ARW SR2 DNG PEF.
namespace RawEmbed {

bool handles(const QString &mime);

// No camera's preview holds more pixels than this; a frame header that claims more would cost a vast decode for nothing.
inline constexpr quint64 kMaxPixels = 250000000;

// The largest valid embedded JPEG, upright and at most `pixels` on its longer side; null when the file holds none.
QImage thumbnail(const QString &path, int pixels);

} // namespace RawEmbed
