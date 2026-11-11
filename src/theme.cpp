// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {

// Used when Omarchy isn't installed: a neutral dark surface with Omarchy's
// fallback yellow accent, so the app still reads well on its own.
const QHash<QString, QColor> &fallbackColors() {
    static const QHash<QString, QColor> colors{
        {"accent", QColor("#FFD60A")},
        {"background", QColor("#16161a")},
        {"dark_background", QColor("#111114")},
        {"lighter_background", QColor("#232329")},
        {"foreground", QColor("#c8c8d0")},
        {"bright_foreground", QColor("#ececf1")},
        {"red", QColor("#f7768e")},
        {"yellow", QColor("#e0af68")},
        {"blue", QColor("#7aa2f7")},
        {"magenta", QColor("#bb9af7")},
        {"cyan", QColor("#7dcfff")},
        {"orange", QColor("#ff9e64")},
    };
    return colors;
}

double luminance(const QColor &c) {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}

QColor mix(const QColor &a, const QColor &b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

struct Palette {
    QHash<QString, QColor> colors;
    bool light = false;
};

Palette readPalette(const QString &path) {
    Palette palette;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return palette;
    const QString text = QString::fromUtf8(file.readAll());
    static const QRegularExpression colorLine(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                              QRegularExpression::MultilineOption);
    for (auto it = colorLine.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        palette.colors.insert(match.captured(1), QColor(match.captured(2)));
    }
    static const QRegularExpression modeLine(R"re(^\s*mode\s*=\s*["']light["'])re",
                                             QRegularExpression::MultilineOption);
    palette.light = modeLine.match(text).hasMatch();
    return palette;
}

} // namespace

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        if (std::exchange(m_roundingPending, false))
            QMetaObject::invokeMethod(this, &Theme::refreshRounding, Qt::QueuedConnection);
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    if (!m_hyprctl.isEmpty()) {
        // Hyprland names each config reload; a value set any other way is read when Folio is back in use.
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive)
                return;
            // Hyprland hangs up on a reader that fell behind, so a watch that ended is taken up again here.
            if (m_hyprlandEvents < 0)
                watchHyprland();
            refreshRounding();
        });
        // Subscribed before the first ask, so a reload during that ask isn't missed.
        watchHyprland();
    }
    reload();
}

Theme::~Theme() {
    closeHyprlandEvents();
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty())
        return;
    // An answer already on its way may predate the change, so ask once more after it.
    m_roundingPending = m_roundingProcess.state() != QProcess::NotRunning;
    if (m_roundingPending)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

// Hyprland's event socket, read as a plain Unix socket since Qt Network isn't linked.
void Theme::watchHyprland() {
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR"), instance = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (runtime.isEmpty() || instance.isEmpty())
        return;
    const QByteArray path = runtime + "/hypr/" + instance + "/.socket2.sock";
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= qsizetype(sizeof address.sun_path))
        return;
    std::memcpy(address.sun_path, path.constData(), path.size());
    m_hyprlandEvents = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_hyprlandEvents < 0)
        return;
    if (::connect(m_hyprlandEvents, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0) {
        closeHyprlandEvents();
        return;
    }
    m_hyprlandNotifier = new QSocketNotifier(m_hyprlandEvents, QSocketNotifier::Read, this);
    connect(m_hyprlandNotifier, &QSocketNotifier::activated, this, &Theme::readHyprlandEvents);
}

void Theme::readHyprlandEvents() {
    constexpr qsizetype longestLine = 64 * 1024; // window titles arrive here too; no line we want is this long
    char chunk[4096];
    ssize_t got;
    while ((got = ::read(m_hyprlandEvents, chunk, sizeof chunk)) > 0 || (got < 0 && errno == EINTR))
        if (got > 0)
            m_hyprlandLine.append(chunk, got);
    const bool closed = got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);

    // Sample input: "activewindow>>kitty,~\nconfigreloaded>>\n"; only a whole configreloaded line counts.
    bool reloaded = false;
    for (qsizetype end; (end = m_hyprlandLine.indexOf('\n')) >= 0; m_hyprlandLine.remove(0, end + 1))
        reloaded |= QByteArrayView(m_hyprlandLine).first(end) == "configreloaded>>";
    if (m_hyprlandLine.size() > longestLine)
        m_hyprlandLine.clear();
    if (reloaded)
        refreshRounding();
    // Hyprland is gone, and the corners keep their last rounding.
    if (closed)
        closeHyprlandEvents();
}

void Theme::closeHyprlandEvents() {
    if (m_hyprlandNotifier) {
        m_hyprlandNotifier->setEnabled(false);
        m_hyprlandNotifier->deleteLater();
        m_hyprlandNotifier = nullptr;
    }
    if (m_hyprlandEvents >= 0)
        ::close(m_hyprlandEvents);
    m_hyprlandEvents = -1;
    m_hyprlandLine.clear();
}

QColor Theme::color(const QString &key) const {
    if (const auto it = m_colors.constFind(key); it != m_colors.constEnd())
        return *it;
    // Older themes may lack the derived backgrounds; fall back to the base.
    if (key.endsWith("background") && m_colors.contains("background"))
        return m_colors.value("background");
    if (key.endsWith("foreground") && m_colors.contains("foreground"))
        return m_colors.value("foreground");
    return fallbackColors().value(key, QColor("#888888"));
}

// Folder cards, tiles and panels are drawn in the lighter background. Some
// themes set it to the background itself (or leave it out), which would make
// them invisible, so then it's the background nudged toward the foreground.
QColor Theme::lighterBackground() const {
    const QColor lighter = color("lighter_background");
    const QColor bg = background();
    const double gap = qAbs(luminance(lighter) - luminance(bg));
    if (gap > 0.004)
        return lighter;
    return mix(bg, foreground(), 0.08);
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QColor Theme::accentForeground() const {
    return luminance(accent()) > .179 ? QColor("black") : QColor("white");
}

// Themes' own dim foregrounds are often too faint for small text, so derive a
// secondary color by mixing the foreground toward the background, keeping
// roughly a 4.5:1 contrast for metadata and labels.
QColor Theme::secondaryForeground() const {
    return mix(background(), foreground(), 0.68);
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const Palette palette = readPalette(m_directory + "/theme/colors.toml");
    if (palette.colors != m_colors || palette.light != m_light) {
        m_colors = palette.colors;
        m_light = palette.light;
        emit changed();
    }
}
