// Tools → Options…: the settings that belong to the program rather than to a
// document (Mitch, 2026-10-09) — the theme, which used to be a submenu of its
// own, and whether Online → Connect goes back to the last connection without
// asking. Both are kept per Windows account in QSettings, like the recent
// files and the theme always were.
#pragma once

#include <QDialog>
#include <QString>

#include "theme.h"

class QCheckBox;
class QComboBox;

namespace ct {

// Whether Online → Connect reuses the last successful connection (port and
// baud) without the Connection Settings dialog. Off by default: ask every time.
bool rememberLastConnection();
void setRememberLastConnection(bool remember);

// The last successful connection, kept only while rememberLastConnection() is
// on (turning it off forgets it). False when none is kept.
bool savedConnection(QString *port, qint32 *baud);
void saveConnection(const QString &port, qint32 baud);
void forgetConnection();

class OptionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit OptionsDialog(QWidget *parent = nullptr);

signals:
    // The theme was applied — live, as the combo changes, and again when
    // Cancel puts the old one back — so a window that draws colours into its
    // text can redraw.
    void themeApplied();

public slots:
    void accept() override;
    void reject() override;

private:
    QComboBox *m_themeCombo = nullptr;
    QCheckBox *m_rememberCheck = nullptr;
    Theme m_themeAtOpen;
};

} // namespace ct
