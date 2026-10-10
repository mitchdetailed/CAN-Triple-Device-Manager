// Tools → Options… (src/ui/options_dialog.*): the theme goes live as the combo
// changes and Cancel puts the one at open back; "Remember last connection"
// is off by default, is kept by OK and not by Cancel, and turning it off
// forgets the connection it kept. The settings live in an INI file in a
// scratch directory, so the test never touches this account's real ones.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

#include "../src/ui/options_dialog.h"
#include "../src/ui/theme.h"

using namespace ct;

static int fails = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir scratch;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    QCoreApplication::setOrganizationName(QStringLiteral("ct-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_options_dialog"));

    std::printf("the defaults: ask every time, nothing kept\n");
    CHECK(!rememberLastConnection());
    QString port;
    qint32 baud = 0;
    CHECK(!savedConnection(&port, &baud));

    std::printf("the theme list, with the current theme selected\n");
    applyTheme(Theme::WindowsDefault);
    saveTheme(Theme::WindowsDefault);
    {
        OptionsDialog dialog;
        auto *combo = dialog.findChild<QComboBox *>();
        auto *box = dialog.findChild<QCheckBox *>();
        CHECK(combo != nullptr && box != nullptr);
        if (combo && box) {
            const QList<ThemeInfo> themes = availableThemes();
            CHECK(combo->count() == themes.size());
            for (int i = 0; i < themes.size() && i < combo->count(); ++i)
                CHECK(combo->itemText(i) == themes[i].name);
            CHECK(themeFromKey(combo->currentData().toString()) == Theme::WindowsDefault);
            CHECK(!box->isChecked());

            // A choice applies at once; Cancel puts the opening theme back.
            int dark = -1;
            for (int i = 0; i < combo->count(); ++i)
                if (themeFromKey(combo->itemData(i).toString()) == Theme::Dark)
                    dark = i;
            CHECK(dark >= 0);
            int applied = 0;
            QObject::connect(&dialog, &OptionsDialog::themeApplied, [&applied] { ++applied; });
            combo->setCurrentIndex(dark);
            CHECK(currentTheme() == Theme::Dark && applied == 1);
            box->setChecked(true);
            dialog.reject();
            CHECK(currentTheme() == Theme::WindowsDefault && applied == 2);
            CHECK(!rememberLastConnection()); // Cancel keeps nothing
        }
    }

    std::printf("OK keeps the box; the theme chosen stays\n");
    {
        OptionsDialog dialog;
        auto *combo = dialog.findChild<QComboBox *>();
        auto *box = dialog.findChild<QCheckBox *>();
        CHECK(combo != nullptr && box != nullptr);
        if (combo && box) {
            int light = -1;
            for (int i = 0; i < combo->count(); ++i)
                if (themeFromKey(combo->itemData(i).toString()) == Theme::Light)
                    light = i;
            combo->setCurrentIndex(light);
            box->setChecked(true);
            dialog.accept();
            CHECK(currentTheme() == Theme::Light && rememberLastConnection());
        }
    }
    {
        OptionsDialog dialog;
        auto *box = dialog.findChild<QCheckBox *>();
        CHECK(box && box->isChecked()); // reopened, it shows what was kept
    }

    std::printf("the connection kept while remembering, forgotten when not\n");
    saveConnection(QStringLiteral("COM9"), 115200);
    CHECK(savedConnection(&port, &baud) && port == QStringLiteral("COM9") && baud == 115200);
    forgetConnection();
    CHECK(!savedConnection(&port, &baud));
    saveConnection(QStringLiteral("COM43"), 0);
    setRememberLastConnection(false);
    CHECK(!rememberLastConnection() && !savedConnection(&port, &baud));

    applyTheme(Theme::WindowsDefault);
    saveTheme(Theme::WindowsDefault);
    if (fails == 0)
        std::printf("test_options_dialog: all checks passed\n");
    else
        std::printf("test_options_dialog: %d check(s) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
