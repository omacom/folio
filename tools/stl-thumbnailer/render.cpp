// Camera fitting, depth-buffer rasterization and stepped OKLab shading adapted
// from Flea (MIT), cde415617459de2cb452bee14b7699e82a4bdd9b.
// Copyright (c) 2026 GM. See LICENSE in this directory.
#include "render.h"

#include <QByteArrayView>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

namespace {
using Point = std::array<double, 3>;
using Triangle = std::array<Point, 3>;
using Visit = std::function<bool(const Triangle &)>;
constexpr double maxCoordinate = 1e9;
// Bounds work even for a file made of millions of full-frame overlapping faces.
constexpr quint64 maxSamples = 128'000'000;

double dot(const Point &a, const Point &b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
Point subtract(const Point &a, const Point &b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Point cross(const Point &a, const Point &b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
Point unit(Point p) {
    const double length = std::hypot(p[0], p[1], p[2]);
    if (length == 0) return {};
    for (auto &coordinate : p) coordinate /= length;
    return p;
}
bool coordinate(double value) { return std::isfinite(value) && std::abs(value) <= maxCoordinate; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

class Tokens {
public:
    explicit Tokens(QByteArrayView bytes) : m_bytes(bytes) {}
    QByteArrayView next() {
        while (m_at < m_bytes.size() && space(m_bytes[m_at])) ++m_at;
        const qsizetype start = m_at;
        while (m_at < m_bytes.size() && !space(m_bytes[m_at])) {
            if (++m_at - start > 128) { good = false; return {}; }
        }
        return m_bytes.sliced(start, m_at-start);
    }
    bool expect(QByteArrayView word) { return next() == word && good; }
    // Names after solid/endsolid may contain spaces; they are not geometry.
    bool name() {
        const qsizetype start = m_at;
        while (m_at < m_bytes.size() && m_bytes[m_at] != '\n')
            if (++m_at - start > 1024) return good = false;
        return true;
    }
    bool number(double &value) {
        auto word = next();
        if (!word.empty() && word.front() == '+') word = word.sliced(1);
        if (word.empty()) return false;
        const auto read = std::from_chars(word.data(), word.data()+word.size(), value);
        return good && read.ec == std::errc() && read.ptr == word.data()+word.size() && coordinate(value);
    }
    bool good = true;
private:
    QByteArrayView m_bytes;
    qsizetype m_at = 0;
};

// Scan twice instead of allocating a vertex/face graph: first fit the camera,
// then render. Both scans see the same bounded in-memory snapshot of the file.
bool scan(const QByteArray &bytes, const Visit &visit) {
    if (bytes.size() >= 84) {
        const quint32 count = qFromLittleEndian<quint32>(bytes.constData()+80);
        if (84 + quint64(count)*50 == quint64(bytes.size())) {
            if (count == 0 || count > StlThumbnail::maxTriangles) return false;
            // Color extensions have incompatible conventions; leave them to an installed renderer.
            if (bytes.first(80).contains("COLOR=")) return false;
            for (quint32 i = 0; i < count; ++i) {
                Triangle triangle;
                const char *face = bytes.constData()+84+quint64(i)*50;
                if (qFromLittleEndian<quint16>(face+48) != 0) return false;
                for (int corner = 0; corner < 3; ++corner) {
                    for (int axis = 0; axis < 3; ++axis) {
                        const quint32 bits = qFromLittleEndian<quint32>(face+12+corner*12+axis*4);
                        float value;
                        std::memcpy(&value, &bits, sizeof value);
                        if (!coordinate(value)) return false;
                        triangle[corner][axis] = value;
                    }
                }
                if (!visit(triangle)) return false;
            }
            return true;
        }
    }
    Tokens words(bytes);
    quint32 count = 0;
    auto token = words.next();
    while (!token.empty()) {
        if (token != "solid" || !words.name()) return false;
        bool closed = false;
        while (!(token = words.next()).empty()) {
            if (token == "endsolid") { closed = words.name(); break; }
            if (token != "facet" || !words.expect("normal")) return false;
            double ignored;
            for (int axis = 0; axis < 3; ++axis) if (!words.number(ignored)) return false;
            if (!words.expect("outer") || !words.expect("loop")) return false;
            Triangle triangle;
            for (auto &point : triangle) {
                if (!words.expect("vertex")) return false;
                for (auto &value : point) {
                    if (!words.number(value)) return false;
                    // STL's binary coordinates are float32; keep ASCII equivalent.
                    value = float(value);
                }
            }
            if (!words.expect("endloop") || !words.expect("endfacet")
                || ++count > StlThumbnail::maxTriangles || !visit(triangle)) return false;
        }
        if (!closed) return false;
        token = words.next();
    }
    return words.good && count > 0;
}

std::array<QRgb, 11> tones() {
    const Point lit{.65, .68, .73};
    Point linear = lit;
    for (auto &c : linear) c = c <= .04045 ? c/12.92 : std::pow((c+.055)/1.055, 2.4);
    const double l = std::cbrt(dot(linear, {.41222146, .53633255, .051445995}));
    const double m = std::cbrt(dot(linear, {.2119035, .6806995, .10739696}));
    const double s = std::cbrt(dot(linear, {.08830246, .28171885, .6299787}));
    const double light = std::max(0., .21045426*l + .7936178*m - .004072047*s - .30);
    const double a = 1.9779985*l - 2.4285922*m + .4505937*s;
    const double b = .025904037*l + .78277177*m - .80867577*s;
    const auto cube = [](double x) { return x*x*x; };
    const Point lab{cube(light+.39633778*a+.21580376*b),
                    cube(light-.105561346*a-.06385417*b), cube(light-.08948418*a-1.2914855*b)};
    Point shade{dot(lab, {4.0767417, -3.3077116, .23096994}),
                dot(lab, {-1.268438, 2.6097574, -.34131938}),
                dot(lab, {-.0041960863, -.7034186, 1.7076147})};
    for (auto &c : shade) {
        c = std::clamp(c, 0., 1.);
        c = c <= .0031308 ? c*12.92 : 1.055*std::pow(c, 1./2.4)-.055;
    }
    std::array<QRgb, 11> result;
    for (int k = 0; k <= 10; ++k) {
        Point value;
        for (int i = 0; i < 3; ++i) value[i] = shade[i]+(lit[i]-shade[i])*k/10.;
        result[k] = qRgb(qRound(value[0]*255), qRound(value[1]*255), qRound(value[2]*255));
    }
    return result;
}
}

QImage StlThumbnail::render(const QByteArray &bytes, int pixels, QString *error) try {
    const auto fail = [&](const char *message) { if (error) *error = QString::fromLatin1(message); return QImage(); };
    if (bytes.isEmpty() || bytes.size() > maxBytes || pixels < 1 || pixels > maxPixels)
        return fail("STL input or image size exceeds its bound");
    const Point right = unit({1, 1, 0}), toward = unit({1, -1, 1}), up = cross(toward, right);
    const Point light = unit({-.55*right[0]+.65*up[0]+.5*toward[0],
                              -.55*right[1]+.65*up[1]+.5*toward[1],
                              -.55*right[2]+.65*up[2]+.5*toward[2]});
    Point lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
    const auto project = [&](const Point &p) { return Point{dot(p, right), -dot(p, up), dot(p, toward)}; };
    if (!scan(bytes, [&](const Triangle &triangle) {
        for (const auto &p : triangle) {
            const auto q = project(p);
            for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], q[i]); hi[i] = std::max(hi[i], q[i]); }
        }
        return true;
    })) return fail("Invalid or oversized STL geometry");
    const double span = std::max(hi[0]-lo[0], hi[1]-lo[1]);
    if (!(span > 0) || !std::isfinite(span)) return fail("STL has no visible extent");
    const int width = pixels*2;
    const double scale = .92*width/span;
    const Point centre{(lo[0]+hi[0])/2, (lo[1]+hi[1])/2, (lo[2]+hi[2])/2};
    const double depthSpan = std::max(span, hi[2]-lo[2]);
    const auto screen = [&](const Point &p) {
        const auto q = project(p);
        return std::array<float, 3>{float((q[0]-centre[0])*scale+width/2.),
                                    float((q[1]-centre[1])*scale+width/2.), float((q[2]-centre[2])/depthSpan)};
    };
    std::vector<float> depth(size_t(width)*width, -std::numeric_limits<float>::infinity());
    std::vector<QRgb> colour(depth.size(), 0);
    const auto ink = tones();
    quint64 work = 0;
    bool visible = false;
    if (!scan(bytes, [&](const Triangle &triangle) {
        const auto a = screen(triangle[0]), b = screen(triangle[1]), c = screen(triangle[2]);
        const float area = (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        if (!std::isfinite(area) || std::abs(area) < 1e-8f) return true;
        const auto bound = [width](float x) { return int(std::clamp(x, 0.f, float(width))); };
        const int x0 = bound(std::floor(std::min({a[0], b[0], c[0]})));
        const int x1 = bound(std::ceil(std::max({a[0], b[0], c[0]})));
        const int y0 = bound(std::floor(std::min({a[1], b[1], c[1]})));
        const int y1 = bound(std::ceil(std::max({a[1], b[1], c[1]})));
        work += quint64(x1-x0)*quint64(y1-y0);
        if (work > maxSamples) return false;
        auto normal = unit(cross(subtract(triangle[1], triangle[0]), subtract(triangle[2], triangle[0])));
        if (dot(normal, normal) == 0) return true;
        if (dot(normal, toward) < 0) for (auto &v : normal) v = -v;
        const int shade = qRound(std::clamp((dot(normal, light)+.3)/1.25, 0., 1.)*10);
        const float inverse = 1/area;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float px = x+.5f, py = y+.5f;
                const float w0 = ((c[0]-b[0])*(py-b[1])-(c[1]-b[1])*(px-b[0]))*inverse;
                const float w1 = ((a[0]-c[0])*(py-c[1])-(a[1]-c[1])*(px-c[0]))*inverse;
                const float w2 = 1-w0-w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = w0*a[2]+w1*b[2]+w2*c[2];
                const size_t at = size_t(y)*width+x;
                if (z > depth[at]) { depth[at] = z; colour[at] = ink[shade]; visible = true; }
            }
        }
        return true;
    })) return fail("STL exceeds the raster work bound");
    if (!visible) return fail("STL produced no visible pixels");
    QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return fail("Cannot allocate STL image");
    // Average premultiplied samples, preserving transparent silhouette edges.
    for (int y = 0; y < pixels; ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < pixels; ++x) {
            const size_t at = size_t(y*2)*width+x*2;
            int r=0, g=0, b=0, a=0;
            for (size_t i : {at, at+1, at+width, at+width+1}) {
                r += qRed(colour[i]); g += qGreen(colour[i]); b += qBlue(colour[i]); a += qAlpha(colour[i]);
            }
            row[x] = qRgba((r+2)/4, (g+2)/4, (b+2)/4, (a+2)/4);
        }
    }
    return image;
} catch (const std::bad_alloc &) {
    if (error) *error = QStringLiteral("Cannot allocate STL buffers");
    return {};
}
