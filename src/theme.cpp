// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
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

double contrast(const QColor &a, const QColor &b) {
    const double x = luminance(a) + .05, y = luminance(b) + .05;
    return x > y ? x / y : y / x;
}

// Omarchy's glass blur brightness: the brightest grey the blur leaves behind a window.
constexpr double blurBrightness = .75;
// The lowest ground alpha glass tries, 0.75 in 8 bits.
constexpr int lowestAlpha = 192;
// WCAG AA contrast for body text.
constexpr double readable = 4.5;
// How far secondary text stays from the foreground, so it still reads quieter.
constexpr double quieterContrast = 1.25;
constexpr int hyprctlWaitMs = 1000;

// The worst contrast of `ink` on each ground at `alpha`, over the blur's brightest and darkest backdrop.
double worstContrast(const QColor &ink, const QList<QColor> &grounds, double alpha, const QColor &tint = {},
                     double tintAlpha = 0) {
    double worst = 21;
    for (const QColor &ground : grounds)
        for (const QColor &backdrop :
             {QColor::fromRgbF(blurBrightness, blurBrightness, blurBrightness), QColor(Qt::black)})
            worst = std::min(worst, contrast(ink, QColor(mix(mix(backdrop, ground, alpha), tint, tintAlpha).rgb())));
    return worst;
}

// The least mix of `ink` toward `toward` that holds, tested in the 8-bit color the screen draws.
template <typename Holds> QColor lift(const QColor &ink, const QColor &toward, Holds holds) {
    if (holds(ink))
        return ink;
    const auto at = [&](double t) { return QColor(mix(ink, toward, t).rgb()); };
    double lo = 0, hi = 1;
    for (int i = 0; i < 20; ++i) {
        const double mid = (lo + hi) / 2;
        (holds(at(mid)) ? hi : lo) = mid;
    }
    return at(hi);
}

struct Glass {
    double alpha = 1;
    QHash<QString, QColor> inks;
};

// The lowest ground alpha at which every ink, the accent included, lifts to read; with none the window stays opaque.
Glass glassFor(QHash<QString, QColor> inks, const QList<QColor> &grounds, const QColor &accent) {
    const QColor toward = luminance(inks["foreground"]) > luminance(grounds.first()) ? Qt::white : Qt::black;
    const auto quieter = [&](const QColor &quiet, const QColor &loud) {
        return contrast(quiet, loud) >= quieterContrast
               && (toward == Qt::white) == (luminance(quiet) < luminance(loud));
    };
    for (int a = lowestAlpha; a < 255; ++a) {
        const double alpha = a / 255.;
        // Every ink reads on the bare grounds and on the current row's accent tint over them.
        const auto reads = [&](const QColor &c) {
            return worstContrast(c, grounds, alpha) >= readable
                   && worstContrast(c, grounds, alpha, accent, Theme::currentRowTint) >= readable;
        };
        Glass glass{alpha, {}};
        for (auto it = inks.cbegin(); it != inks.cend(); ++it)
            glass.inks.insert(it.key(), lift(it.value(), toward, reads));
        const QColor quiet = glass.inks["secondary_foreground"];
        glass.inks["foreground"] = lift(glass.inks["foreground"], toward,
                                        [&](const QColor &c) { return reads(c) && quieter(quiet, c); });
        const QColor accentInk = lift(accent, toward, reads);
        if (std::all_of(glass.inks.cbegin(), glass.inks.cend(), reads) && quieter(quiet, glass.inks["foreground"])
            && reads(accentInk)) {
            glass.inks["accent"] = accentInk;
            return glass;
        }
    }
    return {};
}

// Tags Folio's solid windows "transparent" as Omarchy's key does, and untags only those it tagged.
// hyprctl -j clients: [{"address": "0x55d0c3a1", "pid": 4242, ...}]; getprop: 1 or 0.985; dispatch: ok.
QSet<QString> tagWindows(const QString &hyprctl, bool transparent, const QSet<QString> &tagged) {
    const auto run = [&](const QStringList &args) {
        QProcess process;
        process.start(hyprctl, args);
        if (!process.waitForFinished(hyprctlWaitMs)) {
            process.kill();
            process.waitForFinished();
        }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
                   ? process.readAllStandardOutput().trimmed() : QByteArray();
    };
    // Hyprland's Lua dispatcher first, and the classic one on older builds.
    const auto tag = [&](const QString &window, const QString &change) {
        return run({"dispatch", QString(R"(hl.dsp.window.tag({ window = "%1", tag = "%2" }))").arg(window, change)}) == "ok"
               || run({"dispatch", "tagwindow", change, window}) == "ok";
    };
    const QJsonDocument clients = QJsonDocument::fromJson(run({"-j", "clients"}));
    if (!clients.isArray())
        return tagged;
    static const QRegularExpression valid("^0x[0-9a-f]+$");
    QSet<QString> next;
    for (const auto &value : clients.array()) {
        const QJsonObject client = value.toObject();
        const QString address = client.value("address").toString();
        if (client.value("pid").toInteger() != QCoreApplication::applicationPid() || !valid.match(address).hasMatch())
            continue;
        const QString window = "address:" + address;
        if (tagged.contains(address)) {
            if (transparent || !tag(window, "-transparent"))
                next.insert(address);
        } else if (transparent && run({"getprop", window, "opacity"}).toDouble() == 1
                   && run({"getprop", window, "opacity_inactive"}).toDouble() == 1 && tag(window, "+transparent")) {
            next.insert(address);
        }
    }
    return next;
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
    // Omarchy's Window Transparency choice sits beside the current theme.
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + "/.local/state";
    m_toggles = directory.isEmpty() ? state + "/omarchy/toggles/hypr" : QDir::cleanPath(m_directory + "/../toggles/hypr");
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
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    connect(&m_tagging, &QFutureWatcher<QSet<QString>>::finished, this, [this] {
        m_tagged = m_tagging.result();
        m_tagBusy = false;
        if (std::exchange(m_tagAgain, false))
            followTransparency();
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty()) {
        m_roundingPoll.start();
        // The rounding only needs following while Folio is in use, and it's
        // read again the moment Folio is back.
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                refreshRounding();
                m_roundingPoll.start();
            } else {
                m_roundingPoll.stop();
            }
        });
        // A new window is in Hyprland's client list by the time it takes the keyboard.
        connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow *window) {
            if (window)
                followTransparency();
        });
    }
    reload();
}

Theme::~Theme() {
    m_tagging.waitForFinished();
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::followTransparency() {
    if (m_hyprctl.isEmpty())
        return;
    if (m_tagBusy) {
        m_tagAgain = true;
        return;
    }
    m_tagBusy = true;
    m_tagging.setFuture(QtConcurrent::run(tagWindows, m_hyprctl, m_transparent, m_tagged));
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
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
    return mix(bg, color("foreground"), 0.08);
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QColor Theme::accentForeground() const {
    return luminance(accent()) > .179 ? QColor("black") : QColor("white");
}

// Themes' own dim foregrounds are often too faint for small text, so derive a
// secondary color by mixing the foreground toward the background, keeping
// roughly a 4.5:1 contrast for metadata and labels.
QColor Theme::secondaryForeground() const {
    return m_inks.value("secondary_foreground", mix(background(), color("foreground"), 0.68));
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
    // Omarchy makes the toggles directory the first time one is set.
    QString toggles = m_toggles;
    while (!QFileInfo::exists(toggles) && toggles != "/")
        toggles = QFileInfo(toggles).absolutePath();
    candidates << toggles << m_toggles + "/window-glassy.lua" << m_toggles + "/window-opaque.lua";
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const Palette palette = readPalette(m_directory + "/theme/colors.toml");
    const bool paletteChanged = palette.colors != m_colors || palette.light != m_light;
    m_colors = palette.colors;
    m_light = palette.light;

    // Omarchy's own reading of its two files: opaque wins over glassy, and neither means transparent.
    const bool opaque = QFileInfo::exists(m_toggles + "/window-opaque.lua");
    const bool glassy = !opaque && QFileInfo::exists(m_toggles + "/window-glassy.lua");
    m_transparent = !opaque && !glassy;
    followTransparency();
    Glass glass;
    if (glassy)
        glass = glassFor({{"foreground", color("foreground")}, {"bright_foreground", color("bright_foreground")},
                          {"secondary_foreground", mix(background(), color("foreground"), 0.68)}, {"red", color("red")}},
                         {background(), darkBackground()}, color("accent"));
    if (paletteChanged || glass.alpha != m_glass || glass.inks != m_inks) {
        m_glass = glass.alpha;
        m_inks = glass.inks;
        emit changed();
    }
}
