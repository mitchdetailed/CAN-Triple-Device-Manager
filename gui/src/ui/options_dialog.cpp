#include "options_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

namespace ct {

namespace {
const QLatin1String kRememberKey("connection/rememberLast");
const QLatin1String kPortKey("connection/lastPort");
const QLatin1String kBaudKey("connection/lastBaud");
} // namespace

bool rememberLastConnection()
{
    return QSettings().value(kRememberKey, false).toBool();
}

void setRememberLastConnection(bool remember)
{
    QSettings settings;
    settings.setValue(kRememberKey, remember);
    // Off means "ask every time", so nothing is kept to go back to: a port
    // remembered under an earlier yes must not resurface when the box is
    // ticked again months later.
    if (!remember) {
        settings.remove(kPortKey);
        settings.remove(kBaudKey);
    }
}

bool savedConnection(QString *port, qint32 *baud)
{
    QSettings settings;
    const QString kept = settings.value(kPortKey).toString();
    if (kept.isEmpty())
        return false;
    if (port)
        *port = kept;
    if (baud)
        *baud = settings.value(kBaudKey, 0).toInt();
    return true;
}

void saveConnection(const QString &port, qint32 baud)
{
    QSettings settings;
    settings.setValue(kPortKey, port);
    settings.setValue(kBaudKey, baud);
}

void forgetConnection()
{
    QSettings settings;
    settings.remove(kPortKey);
    settings.remove(kBaudKey);
}

OptionsDialog::OptionsDialog(QWidget *parent)
    : QDialog(parent)
    , m_themeAtOpen(currentTheme())
{
    setWindowTitle(tr("Options"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    // The theme: applied as the combo changes, so the choice can be seen
    // before OK; see theme.h for what each one is.
    m_themeCombo = new QComboBox(this);
    for (const ThemeInfo &info : availableThemes()) {
        m_themeCombo->addItem(info.name, themeKey(info.theme));
        if (info.theme == m_themeAtOpen)
            m_themeCombo->setCurrentIndex(m_themeCombo->count() - 1);
    }
    m_themeCombo->setToolTip(tr("The colours the whole program is drawn in. A choice applies at "
                                "once, to every window, and is kept for this Windows account."));
    form->addRow(tr("Theme:"), m_themeCombo);
    connect(m_themeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0)
            return;
        const Theme theme = themeFromKey(m_themeCombo->itemData(index).toString());
        applyTheme(theme);
        saveTheme(theme);
        emit themeApplied();
    });

    m_rememberCheck = new QCheckBox(tr("Remember last connection"), this);
    m_rememberCheck->setChecked(rememberLastConnection());
    m_rememberCheck->setToolTip(tr("Ticked, Online > Connect goes straight back to the port it "
                                   "last connected to, and asks only when that port is gone. "
                                   "Unticked, it asks which port to use every time."));
    form->addRow(QString(), m_rememberCheck);
    layout->addLayout(form);

    auto *note = new QLabel(
        tr("These settings belong to the program, not to the configuration."), this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &OptionsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &OptionsDialog::reject);
    layout->addWidget(buttons);
}

void OptionsDialog::accept()
{
    setRememberLastConnection(m_rememberCheck->isChecked());
    QDialog::accept();
}

void OptionsDialog::reject()
{
    // The theme went live as the combo changed; Cancel means the one the
    // dialog opened with.
    if (currentTheme() != m_themeAtOpen) {
        applyTheme(m_themeAtOpen);
        saveTheme(m_themeAtOpen);
        emit themeApplied();
    }
    QDialog::reject();
}

} // namespace ct
