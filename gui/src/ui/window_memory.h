// Windows remember their size between runs.
//
// Every window and dialog in the program opened at a size chosen when it was
// written, and a user who had made the Monitor Channels grid tall enough to
// read did it again the next time, and the time after. This remembers the
// size each window was last closed at, under its own key in QSettings, and
// gives it back the next time that window is made.
//
// SIZE ONLY for dialogs, deliberately. Qt places a dialog over its parent, and
// a remembered POSITION would put it wherever it last was — on a monitor that
// is no longer plugged in, or over the wrong half of the desktop — whereas a
// remembered size lands over the parent as before, only the right size. The
// main window is the exception: it has no parent to sit over, so it keeps its
// whole geometry (place, size, maximised) through QWidget's save/restore pair,
// which itself moves a window back onto a screen when the one it was on has
// gone.
//
// WHERE: a file beside the program, {app}\Settings\windows.ini, when main()
// names one — so the sizes are the MACHINE's. Every account on a shared bench
// opens the same windows at the same sizes, and the file can be read, copied
// or deleted like the libraries beside it. Where that folder cannot be written
// (an older Setup that never granted it) main() names no file and the sizes go
// to this account's registry instead, silently: window sizes are a convenience
// and not worth a message box at every launch.
//
// OFF UNTIL main() TURNS IT ON. The tests build these dialogs by the hundred
// and the --screenshots helper shows and hides every one, and none of that
// should write to the settings of whichever machine it runs on. Header-only so
// the dialogs need nothing new linked, which is what lets a call sit in every
// constructor without every test target changing.
#pragma once

#include <QByteArray>
#include <QEvent>
#include <QObject>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QWidget>

#include <memory>
#include <utility>

namespace ct {

namespace window_memory_detail {

inline bool &enabledFlag()
{
    static bool enabled = false;
    return enabled;
}

inline QString &fileFlag()
{
    static QString file;
    return file;
}

inline QString keyFor(const QString &name, const char *what)
{
    return QStringLiteral("windows/%1/%2").arg(name, QLatin1String(what));
}

// The settings store: the named .ini when main() gave one, the application's
// default (the registry, on Windows) otherwise. QSettings writes an .ini
// through a temporary file and a rename, so a crash mid-write leaves the old
// file rather than half of a new one.
inline std::unique_ptr<QSettings> openSettings()
{
    if (fileFlag().isEmpty())
        return std::make_unique<QSettings>();
    return std::make_unique<QSettings>(fileFlag(), QSettings::IniFormat);
}

// Saves when the window goes away. Hide as well as Close: a dialog ended by
// OK, Cancel or Escape is hidden without a close event, and Hide covers all
// three as well as the X.
class Saver : public QObject
{
public:
    enum Mode { Size, Geometry };

    Saver(QWidget *widget, QString name, Mode mode)
        : QObject(widget), m_widget(widget), m_name(std::move(name)), m_mode(mode)
    {
        widget->installEventFilter(this);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_widget
            && (event->type() == QEvent::Hide || event->type() == QEvent::Close)) {
            const std::unique_ptr<QSettings> settings = openSettings();
            if (m_mode == Geometry)
                settings->setValue(keyFor(m_name, "geometry"), m_widget->saveGeometry());
            else
                settings->setValue(keyFor(m_name, "size"), m_widget->size());
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *m_widget;
    QString m_name;
    Mode m_mode;
};

} // namespace window_memory_detail

inline void setWindowMemoryEnabled(bool enabled)
{
    window_memory_detail::enabledFlag() = enabled;
}

inline bool windowMemoryEnabled()
{
    return window_memory_detail::enabledFlag();
}

// Names the .ini the sizes live in (see WHERE above). Empty = the
// application's default settings store.
inline void setWindowMemoryFile(const QString &path)
{
    window_memory_detail::fileFlag() = path;
}

inline QString windowMemoryFile()
{
    return window_memory_detail::fileFlag();
}

// For a dialog. Call once, after the widget's own default resize(): the size
// last saved under `name` replaces the default, bounded to the screen so a
// size remembered from a larger monitor still fits, and from then on the
// window's size is saved whenever it is hidden or closed. A saved size
// smaller than a usable window is ignored, in case a stray value ever lands
// in the settings.
inline void rememberWindowSize(QWidget *widget, const QString &name)
{
    using namespace window_memory_detail;
    if (!enabledFlag())
        return;
    QSize saved = openSettings()->value(keyFor(name, "size")).toSize();
    if (saved.isValid() && saved.width() >= 200 && saved.height() >= 150) {
        if (const QScreen *screen = widget->screen())
            saved = saved.boundedTo(screen->availableSize());
        widget->resize(saved);
    }
    new Saver(widget, name, Saver::Size);
}

// For a top-level window: the whole geometry. True when a saved geometry was
// restored, so the caller can skip its own first-run placement.
inline bool rememberWindowGeometry(QWidget *widget, const QString &name)
{
    using namespace window_memory_detail;
    if (!enabledFlag())
        return false;
    const QByteArray saved = openSettings()->value(keyFor(name, "geometry")).toByteArray();
    const bool restored = !saved.isEmpty() && widget->restoreGeometry(saved);
    new Saver(widget, name, Saver::Geometry);
    return restored;
}

} // namespace ct
