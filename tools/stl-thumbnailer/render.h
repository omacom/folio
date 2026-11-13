#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace StlThumbnail {
inline constexpr qint64 maxBytes = 128 * 1024 * 1024;
inline constexpr quint32 maxTriangles = 2'000'000;
inline constexpr int maxPixels = 1024;

// A bounded, two-sided orthographic picture. No file is opened by the renderer.
QImage render(const QByteArray &bytes, int pixels, QString *error = nullptr);
}
