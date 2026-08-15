#include <QApplication>
#include <QCommandLineParser>
#include <QCursor>
#include <QDir>
#include <QIcon>
#include <QScreen>
#include <QStyle>
#include <QTimer>

#include "model/configuration.h"
#include "ui/add_channel_dialog.h"
#include "ui/communications_dialog.h"
#include "ui/conditions_dialog.h"
#include "ui/edit_channel_dialog.h"
#include "ui/main_window.h"
#include "ui/wheel_guard.h"
#include "ui/section_editor_dialog.h"
#include "ui/select_channel_dialog.h"

// The version has exactly one source: the project() call in CMakeLists.txt,
// which passes ${PROJECT_VERSION} down as -DCT_APP_VERSION. Nothing here (or
// in the installer, or in the About box) may hardcode a second copy. The
// fallback below only exists so an IDE or hand-rolled compile without the
// define still builds; it is deliberately not a plausible release number, so
// a build that lost the define is obvious rather than quietly mislabelled.
#ifndef CT_APP_VERSION
#define CT_APP_VERSION "0.0.0-dev"
#endif

namespace {

// Dev helper (--screenshots <dir>): renders the main dialogs with sample data
// and saves PNGs, for checking layout against the MoTeC reference.
void saveScreenshots(const QString &dir)
{
    QDir().mkpath(dir);
    ct::Configuration config;

    // Channels are all user-created now — seed the two the sample rows use.
    ct::Channel rpmCh;
    rpmCh.name = QStringLiteral("Engine RPM");
    rpmCh.unit = QStringLiteral("rpm");
    rpmCh.quantity = QStringLiteral("Rotational Speed");
    rpmCh.dataType = QStringLiteral("u16");
    rpmCh.minValue = 0;
    rpmCh.maxValue = 20000;
    rpmCh.userDefined = true;
    config.catalog().addOrUpdateUserChannel(rpmCh);
    ct::Channel tempCh;
    tempCh.name = QStringLiteral("Engine Temperature");
    tempCh.unit = QStringLiteral("C");
    tempCh.quantity = QStringLiteral("Temperature");
    tempCh.dataType = QStringLiteral("s16");
    tempCh.baseResolution = 0.1;
    tempCh.decimalPlaces = 1;
    tempCh.minValue = -50;
    tempCh.maxValue = 250;
    tempCh.userDefined = true;
    config.catalog().addOrUpdateUserChannel(tempCh);
    // In the catalogue but written by nothing — shows the input picker's
    // ungenerated case and the output picker's clean case.
    ct::Channel orphanCh;
    orphanCh.name = QStringLiteral("Oil Pressure");
    orphanCh.unit = QStringLiteral("kPa");
    orphanCh.quantity = QStringLiteral("Pressure and Stress");
    orphanCh.dataType = QStringLiteral("u16");
    orphanCh.minValue = 0;
    orphanCh.maxValue = 1000;
    orphanCh.userDefined = true;
    config.catalog().addOrUpdateUserChannel(orphanCh);

    ct::CommsSection section;
    section.name = QStringLiteral("Receive 0x640");
    section.device = ct::SectionDevice::ReceiveMessage;
    section.alignment = ct::SectionAlignment::WordSwap; // Intel start bits
    section.baseAddress = 0x640;
    ct::CommsChannelRow row;
    row.channelName = QStringLiteral("Engine RPM");
    row.startBit = 0;
    row.bitLength = 16;
    row.dbcFactor = 1.0;
    section.rows.append(row);
    ct::CommsChannelRow row2;
    row2.channelName = QStringLiteral("Engine Temperature");
    row2.startBit = 16;
    row2.bitLength = 16;
    row2.dbcType = int(ct::DbcType::Signed);
    row2.dbcFactor = 0.1;
    row2.dbcOffset = -40.0;
    section.rows.append(row2);
    config.bus[0].sections.append(section);

    ct::ConditionRow cond;
    cond.terms[0].aChannel = QStringLiteral("Engine Temperature");
    cond.terms[0].op = 4; // >
    cond.terms[0].bConst = 100;
    cond.outputChannel = QStringLiteral("Engine Hot");
    config.conditionRows.append(cond);

    auto grab = [&dir](QWidget *widget, const QString &name) {
        widget->show();
        QApplication::processEvents();
        widget->grab().save(QStringLiteral("%1/%2.png").arg(dir, name));
        widget->hide();
    };

    ct::MainWindow window;
    window.resize(1100, 700);
    grab(&window, QStringLiteral("main_window"));

    ct::CommunicationsDialog comms(&config);
    grab(&comms, QStringLiteral("communications_setup"));

    // Section 0 of bus 0 — the copy appended above. The document is
    // authoritative here, so no live patch.
    ct::SectionEditorDialog editor(&config, section, 0, {}, /*sectionIndex=*/0);
    grab(&editor, QStringLiteral("section_editor"));

    ct::AddChannelDialog add(&config, row2, section.alignment, 8, false);
    grab(&add, QStringLiteral("add_comms_channel"));

    // Both sides of the picker — they enforce opposite rules, so both are worth
    // eyeballing against the reference.
    ct::SelectChannelDialog selectOut(&config, ct::ChannelRole::Output);
    grab(&selectOut, QStringLiteral("select_channel_output"));

    ct::SelectChannelDialog selectIn(&config, ct::ChannelRole::Input);
    grab(&selectIn, QStringLiteral("select_channel_input"));

    ct::ConditionsDialog conditions(&config);
    grab(&conditions, QStringLiteral("conditions"));

    ct::EditChannelDialog edit(&config,
                               config.catalog().findByName(QStringLiteral("Engine Temperature")),
                               false);
    grab(&edit, QStringLiteral("edit_custom_channel"));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // The wheel must never edit a numeric field just because the pointer is
    // over one — see wheel_guard.h. Installed on the application so it covers
    // every dialog, including ones added later and the table cell editors.
    app.installEventFilter(new ct::WheelGuard(&app));
    // These two decide where the program keeps its own state, and nothing else:
    // QSettings lands in HKCU\Software\<organization>\<application>, and the
    // help engine's writable copy of the manual in
    // %APPDATA%\<organization>\<application>\help. Changing either MOVES that
    // state rather than migrating it, so they are not free to edit once a
    // release is in the field.
    QCoreApplication::setOrganizationName(QStringLiteral("Minton Performance"));
    QCoreApplication::setApplicationName(QStringLiteral("CAN Triple Device Manager"));
    QCoreApplication::setApplicationVersion(QStringLiteral(CT_APP_VERSION));

    // Every window and dialog inherits this one, which is why it is set on the
    // application rather than on the main window. All seven embedded sizes go
    // in: a title bar, the taskbar button and Alt-Tab each ask for a different
    // one, and QIcon then picks real artwork instead of rescaling a single
    // bitmap. 24 earns its place because that is the small-icon metric at 150%
    // display scaling. (The icon Explorer draws on the .exe is a different
    // thing entirely — that comes from the PE resource built by
    // resources/app.rc.)
    QIcon appIcon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        appIcon.addFile(QStringLiteral(":/icons/icon_%1.png").arg(size));
    QApplication::setWindowIcon(appIcon);

    QCommandLineParser parser;
    const QCommandLineOption screenshotOption(QStringLiteral("screenshots"),
                                              QStringLiteral("Save dialog screenshots and exit"),
                                              QStringLiteral("dir"));
    parser.addOption(screenshotOption);
    parser.process(app);

    if (parser.isSet(screenshotOption)) {
        saveScreenshots(parser.value(screenshotOption));
        return 0;
    }

    ct::MainWindow window;
    window.resize(1100, 700);
    // Center on the screen under the cursor (the monitor the app was launched
    // from), falling back to the primary screen. Qt's default placement can
    // otherwise land the window off the visible area on multi-monitor setups —
    // it appears in the taskbar but nowhere on screen.
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen)
        window.setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter,
                                               window.size(), screen->availableGeometry()));
    window.show();
    return app.exec();
}
