// What a message type is allowed to expose.
//
// Two doors that used to stand open, and both of them the same shape: an
// affordance offered where it cannot mean anything, which is worse than no
// affordance at all — the user reaches for it, gets a result, and only much
// later finds out the device ignored it.
//
//   OFF has no channels. An Off message is not laid out at all: mapToDevice
//   skips it and findLayoutClashes returns nothing for it, so rows added there
//   go nowhere. The Received Channels tab is grayed exactly as a relay's is.
//   But GRAYED, not gone, and the rows are KEPT — turning a configured message
//   off and back on has to return it intact, which is what separates this from
//   the relay case, where the rows really are dropped.
//
//   A PICKER THAT ONLY READS cannot invent a channel. What a channel carries
//   has to be produced somewhere first — a receive message row, a calculation,
//   a constant — so a channel named at a site that merely reads it has nothing
//   writing it: a transmit row would send its default value for ever, and a
//   math input would read that same default. New… therefore belongs to
//   ChannelRole::Output alone, and on the Input side the button is not built.
//
//   The line runs between the ROLES, not between the dialogs, and this suite
//   checks it from both sides: a transmit comms row and pickInput() itself (the
//   static behind math, table axes, counter triggers, conditions and
//   integrators) have no New…, while the receive comms row and the CRC8
//   channel picker — both of which WRITE what they name — still do. Nothing
//   becomes unbuildable: Tools -> Channel Editor creates a channel from
//   anywhere, and so does every Output picker.
//
// Everything runs through the real dialogs and their real widgets. The pickers
// are MODAL, so those checks run from a timer inside the modal loop.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>

#include <cstdio>
#include <functional>

#include "../src/model/comms_types.h"
#include "../src/model/configuration.h"
#include "../src/ui/add_channel_dialog.h"
#include "../src/ui/section_editor_dialog.h"
#include "../src/ui/select_channel_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

#define REQUIRE(cond)                                                                \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
            return;                                                                  \
        }                                                                            \
    } while (0)

namespace {

using namespace ct;

void buildConfig(Configuration &config)
{
    config.clear();
    config.bus[0].enabled = true;
    config.bus[0].rateKbps = 500;
    for (const char *name : {"Speed", "Rpm"}) {
        Channel c;
        c.name = QString::fromLatin1(name);
        c.dataType = QStringLiteral("u16");
        c.baseResolution = 1.0;
        c.minValue = 0;
        c.maxValue = 1000;
        c.userDefined = true;
        config.catalog().addOrUpdateUserChannel(c);
    }
}

// A message with two channels in it, so "the rows survived" is a question with
// an answer.
CommsSection twoRowMessage(SectionDevice device)
{
    CommsSection s;
    s.name = QStringLiteral("Msg");
    s.device = device;
    s.baseAddress = 0x640;
    s.messageLengthBytes = 8;
    for (int i = 0; i < 2; ++i) {
        CommsChannelRow row;
        row.channelName = i == 0 ? QStringLiteral("Speed") : QStringLiteral("Rpm");
        row.startBit = i * 16;
        row.bitLength = 16;
        s.rows.append(row);
    }
    return s;
}

// The Message Type combo, picked out by its ITEMS rather than by position: the
// Channels tab has a "Message Type" group of its own (Single / Compound), so
// the label nearby is not enough to identify it.
QComboBox *deviceCombo(QWidget &w)
{
    for (QComboBox *combo : w.findChildren<QComboBox *>()) {
        for (int i = 0; i < combo->count(); ++i)
            if (combo->itemText(i) == QStringLiteral("Off")
                && combo->itemText(0) == QStringLiteral("Off"))
                return combo;
    }
    return nullptr;
}

// OK, through the dialog's own button box — accept() is private, and clicking
// OK is what a user does anyway.
// A warning box the dialog puts up waits for a click that never comes under
// the offscreen platform. While the watch runs, a modal box that comes up is
// closed within a moment and its text kept, so a refused section fails its
// check instead of hanging the run.
QString g_refusal;
QTimer *g_refusalWatch = nullptr;
void watchRefusals(bool on)
{
    if (!g_refusalWatch) {
        g_refusalWatch = new QTimer(qApp);
        g_refusalWatch->setInterval(200);
        QObject::connect(g_refusalWatch, &QTimer::timeout, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                std::puts(qPrintable(QStringLiteral("  refused: ") + box->text()));
                g_refusal += box->text() + QLatin1String(" | ");
                box->close();
            }
        });
    }
    g_refusal.clear();
    if (on)
        g_refusalWatch->start();
    else
        g_refusalWatch->stop();
}

bool clickOk(QDialog &d)
{
    auto *box = d.findChild<QDialogButtonBox *>();
    if (!box || !box->button(QDialogButtonBox::Ok))
        return false;
    box->button(QDialogButtonBox::Ok)->click();
    return true;
}

QPushButton *button(QWidget &w, const QString &text)
{
    for (QPushButton *b : w.findChildren<QPushButton *>())
        if (b->text() == text)
            return b;
    return nullptr;
}

const char *deviceName(SectionDevice d)
{
    switch (d) {
    case SectionDevice::Off: return "Off";
    case SectionDevice::ReceiveMessage: return "Receive Message";
    case SectionDevice::TransmitMessage: return "Transmit Message";
    case SectionDevice::TransmitCrc8: return "Transmit CRC8";
    case SectionDevice::MessageRelay: return "Message Relay";
    case SectionDevice::UdsRequest: return "UDS Request";
    }
    return "?";
}

// ---------------------------------------------------------------- the tab

void testTheChannelsTabFollowsTheMessageType()
{
    Configuration config;
    buildConfig(config);
    SectionEditorDialog dialog(&config, twoRowMessage(SectionDevice::ReceiveMessage), 0, {}, -1);
    auto *tabs = dialog.findChild<QTabWidget *>();
    QComboBox *combo = deviceCombo(dialog);
    REQUIRE(tabs != nullptr);
    REQUIRE(combo != nullptr);

    struct Case { SectionDevice device; bool tabLive; };
    // Off and Message Relay are the two that carry no channels, for different
    // reasons that come to the same thing: neither is laid out into a frame.
    const Case cases[] = {
        {SectionDevice::ReceiveMessage, true},
        {SectionDevice::Off, false},
        {SectionDevice::TransmitMessage, true},
        {SectionDevice::MessageRelay, false},
        {SectionDevice::TransmitCrc8, true},
        {SectionDevice::UdsRequest, true}, // its response's channels
        {SectionDevice::Off, false}, // and again, arriving from the other side
    };
    for (const Case &c : cases) {
        combo->setCurrentIndex(combo->findData(int(c.device)));
        const bool live = tabs->isTabEnabled(1);
        std::printf("  %-17s channels tab %s\n", deviceName(c.device),
                    live ? "live" : "grayed");
        CHECK(live == c.tabLive);
    }
}

void testOffOpensWithTheTabAlreadyGrayed()
{
    // Not only on a change of the combo: a message SAVED as Off must come back
    // that way, which is applyDeviceKindEnablement running from the
    // constructor rather than only from the signal.
    Configuration config;
    buildConfig(config);
    SectionEditorDialog dialog(&config, twoRowMessage(SectionDevice::Off), 0, {}, -1);
    auto *tabs = dialog.findChild<QTabWidget *>();
    REQUIRE(tabs != nullptr);
    CHECK(!tabs->isTabEnabled(1));
    // Grayed, not gone. The rows are still there and the tab has to keep
    // saying so — this is the difference from the CRC8 tab beside it, which
    // really is hidden for every other message type.
    CHECK(tabs->isTabVisible(1));
    CHECK(!tabs->isTabVisible(2));
}

void testOffKeepsItsChannels()
{
    // THE POINT OF GRAYING RATHER THAN CLEARING. Switch a configured receive
    // message off, save, and the rows are all still there — turning a message
    // off is a way to stop sending it for a while, not a way to throw its
    // definition away.
    Configuration config;
    buildConfig(config);
    SectionEditorDialog dialog(&config, twoRowMessage(SectionDevice::ReceiveMessage), 0, {}, -1);
    QComboBox *combo = deviceCombo(dialog);
    REQUIRE(combo != nullptr);
    combo->setCurrentIndex(combo->findData(int(SectionDevice::Off)));
    REQUIRE(clickOk(dialog));
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.section().device == SectionDevice::Off);
    CHECK(dialog.section().rows.size() == 2);
    if (dialog.section().rows.size() == 2) {
        CHECK(dialog.section().rows.at(0).channelName == QStringLiteral("Speed"));
        CHECK(dialog.section().rows.at(1).channelName == QStringLiteral("Rpm"));
    }
}

void testARelayStillDropsItsChannels()
{
    // The control, and the reason Off could not simply copy the relay's
    // handling: a relay is a whole-frame gateway rule, its rows are meaningless
    // rather than dormant, and syncParametersFromUi clears them so they cannot
    // reappear in the channel usage report as phantoms. Off must NOT do that,
    // and a test that only watched Off would not notice if the two were ever
    // merged into one branch.
    Configuration config;
    buildConfig(config);
    SectionEditorDialog dialog(&config, twoRowMessage(SectionDevice::ReceiveMessage), 0, {}, -1);
    QComboBox *combo = deviceCombo(dialog);
    REQUIRE(combo != nullptr);
    combo->setCurrentIndex(combo->findData(int(SectionDevice::MessageRelay)));
    // A relay refuses to close without somewhere to forward to. The forward
    // boxes live in the "Message Relay" group; the routing group beside it has
    // three checkboxes with the same labels, so the group is what tells them
    // apart.
    QGroupBox *relayGroup = nullptr;
    for (QGroupBox *g : dialog.findChildren<QGroupBox *>())
        if (g->title() == QStringLiteral("Message Relay"))
            relayGroup = g;
    REQUIRE(relayGroup != nullptr);
    bool ticked = false;
    for (QCheckBox *b : relayGroup->findChildren<QCheckBox *>()) {
        if (b->text().startsWith(QStringLiteral("CAN")) && b->isEnabled() && !ticked) {
            b->setChecked(true);
            ticked = true;
        }
    }
    REQUIRE(ticked);
    REQUIRE(clickOk(dialog));
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.section().rows.isEmpty());
}

// ---------------------------------------------------------------- the BRS box

// THE BIT RATE SWITCH beside CAN FD frame (Mitch, 2026-10-08): on Transmit
// Message and Transmit CRC8 only, live while CAN FD frame is ticked, where the
// target honours it; unticked and saved, the section says so, and ticked again
// after a trip through classic the choice comes back. On a target that cannot
// send FD without it, the box shows ticked and stays greyed.
QCheckBox *checkNamed(QWidget &w, const QString &text)
{
    for (QCheckBox *b : w.findChildren<QCheckBox *>())
        if (b->text() == text)
            return b;
    return nullptr;
}

void testBrsFollowsCanFd()
{
    Configuration config;
    buildConfig(config);
    config.bus[0].dataRateKbps = 2000;
    DeviceCapacity cap = DeviceCapacity::builtIn();
    cap.canFeatures |= CAPACITY_CAN_BRS_PER_MESSAGE;
    config.setCapacity(cap);
    CommsSection s = twoRowMessage(SectionDevice::TransmitMessage);
    s.fd = true;
    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    QComboBox *combo = deviceCombo(dialog);
    QCheckBox *fd = checkNamed(dialog, QStringLiteral("CAN FD frame"));
    QCheckBox *brs = checkNamed(dialog, QStringLiteral("BRS"));
    REQUIRE(combo && fd && brs);
    struct Case { SectionDevice device; bool shown; };
    const Case cases[] = {
        {SectionDevice::ReceiveMessage, false}, {SectionDevice::TransmitCrc8, true},
        {SectionDevice::MessageRelay, false},   {SectionDevice::Off, false},
        // The UDS request before the last Transmit Message, which the OK below
        // accepts as the frame this test is about.
        {SectionDevice::UdsRequest, true},       {SectionDevice::TransmitMessage, true},
    };
    for (const Case &c : cases) {
        combo->setCurrentIndex(combo->findData(int(c.device)));
        std::printf("  %-17s BRS %s\n", deviceName(c.device),
                    brs->isHidden() ? "hidden" : brs->isEnabled() ? "live" : "grayed");
        CHECK(brs->isHidden() == !c.shown);
    }
    CHECK(fd->isChecked() && brs->isEnabled() && brs->isChecked()); // on by default
    brs->setChecked(false);
    fd->setChecked(false); // classic: no switch to choose
    CHECK(!brs->isEnabled() && !brs->isChecked());
    fd->setChecked(true); // FD again: the choice comes back
    CHECK(brs->isEnabled() && !brs->isChecked());
    REQUIRE(clickOk(dialog));
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.section().fd && !dialog.section().brs);

    // A target that cannot: greyed, showing what the frame will do.
    Configuration older;
    buildConfig(older);
    older.bus[0].dataRateKbps = 2000;
    SectionEditorDialog oldDialog(&older, s, 0, {}, -1);
    QCheckBox *oldBrs = checkNamed(oldDialog, QStringLiteral("BRS"));
    REQUIRE(oldBrs);
    CHECK(!oldBrs->isHidden() && !oldBrs->isEnabled() && oldBrs->isChecked());
}

// ---------------------------------------------------------------- UDS Request

// The UDS Request group: shown for that message type alone, with the Base
// Address and Message Length relabelled; the Received Channels tab live; and
// OK keeps what was typed — the request's bytes parsed from hex, the response's
// identifier, the timeouts and the flow control.
void testUdsRequestControls()
{
    Configuration config;
    buildConfig(config);
    watchRefusals(true);
    DeviceCapacity cap = DeviceCapacity::builtIn();
    cap.canFeatures |= CAPACITY_CAN_UDS_REQUESTS;
    cap.tables.resize(int(DeviceTable::Uds) + 1); // the 1.x layout stops at the CRC8 rules
    cap.tables[int(DeviceTable::Uds)] = TableCapacity{32, int(sizeof(UdsConfig))};
    config.setCapacity(cap);
    CommsSection s = twoRowMessage(SectionDevice::UdsRequest);
    s.baseAddress = 0x7E0;
    s.udsResponseAddress = 0x7E8;
    s.udsRequest = QByteArray::fromHex("22F190");
    s.messageLengthBytes = 20;
    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    QComboBox *combo = deviceCombo(dialog);
    REQUIRE(combo != nullptr);
    std::puts("  UDS Request       editor built");
    QGroupBox *group = nullptr;
    for (QGroupBox *g : dialog.findChildren<QGroupBox *>())
        if (g->title() == QStringLiteral("UDS Request"))
            group = g;
    QLabel *lengthLabel = nullptr;
    for (QLabel *l : dialog.findChildren<QLabel *>())
        if (l->text() == QStringLiteral("Response Length :"))
            lengthLabel = l;
    REQUIRE(group != nullptr);
    CHECK(!group->isHidden() && lengthLabel != nullptr);
    auto *tabs = dialog.findChild<QTabWidget *>();
    REQUIRE(tabs != nullptr);
    CHECK(tabs->isTabEnabled(1) && tabs->tabText(1) == QStringLiteral("Received Channels"));
    for (SectionDevice d : {SectionDevice::ReceiveMessage, SectionDevice::TransmitMessage,
                            SectionDevice::MessageRelay, SectionDevice::TransmitCrc8}) {
        combo->setCurrentIndex(combo->findData(int(d)));
        CHECK(group->isHidden());
    }
    combo->setCurrentIndex(combo->findData(int(SectionDevice::UdsRequest)));
    CHECK(!group->isHidden());
    std::puts("  UDS Request       group follows the kind");
    // Typed: the bytes, the identifier, the length.
    QLineEdit *request = nullptr, *response = nullptr;
    for (QLineEdit *e : group->findChildren<QLineEdit *>()) {
        if (e->placeholderText() == QStringLiteral("22 F1 90"))
            request = e;
        else if (e->text().startsWith(QStringLiteral("0x7E8")))
            response = e;
    }
    REQUIRE(request != nullptr && response != nullptr);
    CHECK(request->text() == QStringLiteral("22 F1 90"));
    request->setText(QStringLiteral("2e f1 90 01 02"));
    response->setText(QStringLiteral("18DAF110"));
    std::puts("  UDS Request       accepting");
    REQUIRE(clickOk(dialog));
    watchRefusals(false);
    if (!g_refusal.isEmpty())
        std::puts(qPrintable(QStringLiteral("  refused: ") + g_refusal));
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.section().device == SectionDevice::UdsRequest
          && dialog.section().udsRequest == QByteArray::fromHex("2EF1900102")
          && dialog.section().udsResponseAddress == 0x18DAF110
          && dialog.section().messageLengthBytes == 20 && dialog.section().rows.size() == 2);
}

// ---------------------------------------------------------------- the picker

// Run `trigger`, which opens a modal picker and blocks until it closes; list
// the buttons that modal shows, then close it. isHidden() rather than
// isVisible(): the question is "did somebody call setVisible(false) on this
// button", and isHidden() answers it without depending on the modal being
// mapped under the offscreen platform.
//
// Takes a callable rather than the button to click, because the pickers this
// suite has to reach are opened three different ways: a Select… on a row
// dialog, a Select… on the CRC tab, and the static pickInput() itself, which
// is the one no widget in this test owns.
QStringList liveButtonsInModalWhile(const std::function<void()> &trigger, bool *found)
{
    QStringList texts;
    *found = false;
    QTimer timer;
    int ticks = 0;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal) {
            if (++ticks > 400)
                timer.stop(); // the picker never came up; the caller checks `found`
            return;
        }
        for (QPushButton *b : modal->findChildren<QPushButton *>())
            if (!b->isHidden())
                texts << b->text();
        *found = true;
        modal->close(); // Cancel: nothing is picked, nothing is written
        timer.stop();
    });
    timer.start(5);
    trigger();
    return texts;
}

QStringList pickerButtonsFor(Configuration &config, bool transmit, bool *found)
{
    CommsChannelRow row;
    row.channelName = QStringLiteral("Speed");
    row.startBit = 0;
    row.bitLength = 16;
    AddChannelDialog dialog(&config, row, SectionAlignment::Normal, 8, transmit);
    QPushButton *select = button(dialog, QStringLiteral("Select…"));
    if (!select) {
        *found = false;
        return {};
    }
    return liveButtonsInModalWhile([select]() { select->click(); }, found);
}

void testATransmitRowCannotInventAChannel()
{
    // The reported case, through the dialog a user actually opens. It gets the
    // answer from ChannelRole rather than from anything AddChannelDialog does,
    // but the whole chain is what was reported and the whole chain is what is
    // checked.
    Configuration config;
    buildConfig(config);
    bool found = false;
    const QStringList buttons = pickerButtonsFor(config, /*transmit=*/true, &found);
    REQUIRE(found);
    std::printf("  transmit row picker : %s\n", qPrintable(buttons.join(QStringLiteral(" "))));
    CHECK(!buttons.contains(QStringLiteral("New…")));
    // Everything else is still there. Removing New… must not have taken Edit…
    // with it: correcting a channel's unit or range from the picker is
    // unrelated to defining one that nothing writes.
    CHECK(buttons.contains(QStringLiteral("Edit…")));
    CHECK(buttons.contains(QStringLiteral("OK")));
    CHECK(buttons.contains(QStringLiteral("Cancel")));
}

void testAReceiveRowStillCan()
{
    // The receive row IS the writer, so a channel defined at its picker is
    // filled the moment the row is saved. Nothing to close here.
    Configuration config;
    buildConfig(config);
    bool found = false;
    const QStringList buttons = pickerButtonsFor(config, /*transmit=*/false, &found);
    REQUIRE(found);
    std::printf("  receive row picker  : %s\n", qPrintable(buttons.join(QStringLiteral(" "))));
    CHECK(buttons.contains(QStringLiteral("New…")));
}

void testTheCrcChannelPickerStillOffersNew()
{
    // The CRC8's channel is an OUTPUT — the device computes the checksum and
    // publishes it — so it is a channel that has to be brought into existence
    // somewhere, and the picker is the natural place. Taking New… from here
    // would leave a required field with no way to fill it on a fresh
    // configuration.
    Configuration config;
    buildConfig(config);
    SectionEditorDialog dialog(&config, twoRowMessage(SectionDevice::TransmitCrc8), 0, {}, -1);
    QPushButton *select = button(dialog, QStringLiteral("Select…"));
    REQUIRE(select != nullptr);
    bool found = false;
    const QStringList buttons =
        liveButtonsInModalWhile([select]() { select->click(); }, &found);
    REQUIRE(found);
    std::printf("  CRC8 channel picker : %s\n", qPrintable(buttons.join(QStringLiteral(" "))));
    CHECK(buttons.contains(QStringLiteral("New…")));
}

void testNoInputPickerCanInventAChannel()
{
    // pickInput() ITSELF, the static behind every remaining read site: math A/B
    // and C, a table axis, a counter or timer trigger, both sides of a
    // condition term, an integrator's input and its enable and reset triggers.
    // Driving the static rather than each of those dialogs is the point — it is
    // the one place the rule lives, so it is the one place worth pinning, and a
    // new read site inherits the answer by calling it.
    Configuration config;
    buildConfig(config);
    bool found = false;
    const QStringList buttons = liveButtonsInModalWhile(
        [&config]() { SelectChannelDialog::pickInput(&config, QString(), nullptr); }, &found);
    REQUIRE(found);
    std::printf("  pickInput (math etc): %s\n", qPrintable(buttons.join(QStringLiteral(" "))));
    CHECK(!buttons.contains(QStringLiteral("New…")));
    CHECK(buttons.contains(QStringLiteral("Edit…")));
}

void testTheRoleDecidesAndTheButtonIsNotBuilt()
{
    // The rule at its source: the ROLE decides, and on the read side the button
    // is never constructed rather than constructed and hidden. Direct
    // construction rather than a modal, because the question is only whether
    // the button exists.
    Configuration config;
    buildConfig(config);
    SelectChannelDialog input(&config, ChannelRole::Input);
    CHECK(button(input, QStringLiteral("New…")) == nullptr);
    CHECK(button(input, QStringLiteral("Edit…")) != nullptr);

    SelectChannelDialog output(&config, ChannelRole::Output);
    QPushButton *newButton = button(output, QStringLiteral("New…"));
    REQUIRE(newButton != nullptr);
    CHECK(!newButton->isHidden());
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    watchRefusals(true); // every refusal box of the run closed and printed, none waited on
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testTheChannelsTabFollowsTheMessageType();
    testOffOpensWithTheTabAlreadyGrayed();
    testOffKeepsItsChannels();
    testARelayStillDropsItsChannels();
    testBrsFollowsCanFd();
    testUdsRequestControls();
    testATransmitRowCannotInventAChannel();
    testAReceiveRowStillCan();
    testTheCrcChannelPickerStillOffersNew();
    testNoInputPickerCanInventAChannel();
    testTheRoleDecidesAndTheButtonIsNotBuilt();

    if (fails == 0)
        std::printf("test_message_type_gating: all checks passed\n");
    else
        std::printf("test_message_type_gating: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
