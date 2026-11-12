#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

// The Omarchy palette, followed live. A file manager draws far more surface
// than a one-tool app, so it reads the whole colors.toml rather than only the
// accent, and the Material style flips with the theme's light or dark mode.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QColor accentText READ accentText NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor darkBackground READ darkBackground NOTIFY changed)
    Q_PROPERTY(QColor lighterBackground READ lighterBackground NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor brightForeground READ brightForeground NOTIFY changed)
    Q_PROPERTY(QColor secondaryForeground READ secondaryForeground NOTIFY changed)
    Q_PROPERTY(QColor red READ red NOTIFY changed)
    Q_PROPERTY(QColor yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QColor blue READ blue NOTIFY changed)
    Q_PROPERTY(QColor magenta READ magenta NOTIFY changed)
    Q_PROPERTY(QColor cyan READ cyan NOTIFY changed)
    Q_PROPERTY(QColor orange READ orange NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(qreal glass READ glass NOTIFY changed)
    Q_PROPERTY(qreal currentTint READ currentTint CONSTANT)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QColor accent() const { return color("accent"); }
    QColor accentForeground() const;
    // The accent where it is text: lifted with the other text while glassy.
    QColor accentText() const { return ink("accent"); }
    QColor background() const { return color("background"); }
    QColor darkBackground() const { return color("dark_background"); }
    QColor lighterBackground() const;
    QColor foreground() const { return ink("foreground"); }
    QColor brightForeground() const { return ink("bright_foreground"); }
    QColor secondaryForeground() const;
    QColor red() const { return ink("red"); }
    QColor yellow() const { return color("yellow"); }
    QColor blue() const { return color("blue"); }
    QColor magenta() const { return color("magenta"); }
    QColor cyan() const { return color("cyan"); }
    QColor orange() const { return color("orange"); }
    bool light() const { return m_light; }
    int radius() const { return m_radius; }
    // How opaque the window's grounds are: 1, or less while Omarchy's windows are glassy.
    qreal glass() const { return m_glass; }
    // The accent tint under the current row, which accent text is lifted to read on too.
    static constexpr qreal currentRowTint = 0.18;
    qreal currentTint() const { return currentRowTint; }

signals:
    void changed();

private:
    QColor color(const QString &key) const;
    QColor ink(const QString &key) const { return m_inks.value(key, color(key)); }
    void reload();
    void refreshRounding();
    void followTransparency();

    QString m_directory;
    QHash<QString, QColor> m_colors;
    bool m_light = false;
    QString m_toggles;
    qreal m_glass = 1;
    QHash<QString, QColor> m_inks;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
    bool m_transparent = false;
    QSet<QString> m_tagged;
    QFutureWatcher<QSet<QString>> m_tagging;
    bool m_tagBusy = false, m_tagAgain = false;
};
