// Channel labels carry the unit; channel IDENTITY stays the bare name.
//
// Showing "Coolant Temp °C" wherever a channel appears is a display change, but
// it is a display change made in dialogs whose read-only "Input :" / "Output :"
// boxes were the only record of which channel a row referred to — every one of
// them ends in `row.aChannel = m_aEdit->text()`. Decorating those boxes without
// separating the two jobs does not change what the user sees; it rewrites the
// configuration to name a channel that does not exist, and it does so silently:
// the row still looks right, the document still saves, and the value simply
// stops arriving on the device.
//
// So the property under test is not "the unit is shown" — it is:
//
//     OPENING A ROW AND PRESSING OK MUST NOT CHANGE WHICH CHANNEL IT NAMES.
//
// That is the exact path the corruption takes, and it is driven here for real:
// the manager dialog is opened on a row that references a unit-bearing channel,
// its Change… button opens the row editor, a timer presses OK on that editor,
// then OK on the manager — and the document is compared against what it held
// before. A dialog that decorated its identity field fails this immediately.
//
// Runs offscreen.

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QComboBox>
#include <QTreeWidget>

#include <cstdio>

#include "../src/protocol/wire_structs.h"
#include "../src/model/channel.h"
#include "../src/model/channel_catalog.h"
#include "../src/model/configuration.h"
#include "../src/model/dbc_import.h" // storageTypeHoldsRange
#include "../src/ui/channel_field.h"
#include "../src/ui/conditions_dialog.h"
#include "../src/ui/counters_dialog.h"
#include "../src/ui/integrators_dialog.h"
#include "../src/ui/math_dialog.h"
#include "../src/ui/timers_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

using namespace ct;

const char *kChan = "Coolant Temp";
const char *kUnit = "\xC2\xB0" "C"; // °C

void seedChannels(Configuration &config)
{
    const struct { const char *name; const char *unit; } kChans[] = {
        {kChan, kUnit},
        {"Engine RPM", "rpm"},
        {"Fan Request", ""}, // unitless — must come back undecorated
    };
    for (const auto &c : kChans) {
        Channel ch;
        ch.name = QString::fromUtf8(c.name);
        ch.unit = QString::fromUtf8(c.unit);
        ch.dataType = QStringLiteral("float");
        ch.minValue = -1000;
        ch.maxValue = 10000;
        ch.userDefined = true;
        config.catalog().addOrUpdateUserChannel(ch);
    }
}

// ---------------------------------------------------------------- the helpers
void testLabelHelpers()
{
    Configuration config;
    config.clear();
    seedChannels(config);

    CHECK(channelLabel(QStringLiteral("Coolant Temp"), QString::fromUtf8(kUnit))
          == QString::fromUtf8("Coolant Temp \xC2\xB0" "C"));
    // A unitless channel gets no trailing space — the thing that would show up
    // as a stray gap in every list in the program.
    CHECK(channelLabel(QStringLiteral("Fan Request"), QString()) == QStringLiteral("Fan Request"));

    const ChannelCatalog &cat = config.catalog();
    CHECK(cat.labelFor(QString::fromUtf8(kChan)) == QString::fromUtf8("Coolant Temp \xC2\xB0" "C"));
    CHECK(cat.labelFor(QStringLiteral("Fan Request")) == QStringLiteral("Fan Request"));
    // A reference to a channel that no longer exists still reads as the name it
    // is looking for, rather than going blank and hiding which one is broken.
    CHECK(cat.labelFor(QStringLiteral("Deleted Thing")) == QStringLiteral("Deleted Thing"));
    CHECK(cat.labelFor(QString()).isEmpty());
}

// A channel field shows the decorated label but answers with the bare name.
void testChannelFieldSeparatesDisplayFromIdentity()
{
    Configuration config;
    config.clear();
    seedChannels(config);

    QLineEdit edit;
    setChannelField(&edit, QString::fromUtf8(kChan), config.catalog());
    CHECK(edit.text() == QString::fromUtf8("Coolant Temp \xC2\xB0" "C")); // seen
    CHECK(channelField(&edit) == QString::fromUtf8(kChan));               // meant

    // Cleared field: no name, and no leftover from the last one.
    setChannelField(&edit, QString(), config.catalog());
    CHECK(edit.text().isEmpty());
    CHECK(channelField(&edit).isEmpty());

    // A plain line edit that was never set through the helper still answers
    // with its text, so a half-converted call site degrades to the old
    // behaviour rather than to an empty channel name.
    QLineEdit untouched;
    untouched.setText(QStringLiteral("Engine RPM"));
    CHECK(channelField(&untouched) == QStringLiteral("Engine RPM"));
}

// ------------------------------------------------- open a row, press OK, twice
// A row editor is opened with exec(), which runs its own event loop and blocks
// until something closes it — so the OK has to come from a timer running inside
// that loop. A repeating one rather than a single shot, because accepting an
// editor can raise a SECOND modal on top of it (a validation warning), and a
// one-shot would dismiss the warning and leave the editor open forever.
//
// The attempt counter is the reason this cannot hang the build: a dialog that
// refuses to close is rejected and the test fails on its assertions, instead of
// the whole sweep stopping on a window nobody can see.
int g_modalAttempts = 0;
// The shared closer presses OK on whatever modal appears, which is exactly what
// most tests want and exactly wrong for one that needs to LOOK at an editor
// before it closes. That test suspends the closer rather than racing it on
// timer intervals, which would pass or fail by milliseconds.
bool g_suspendModalCloser = false;

void installModalCloser()
{
    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, qApp, []() {
        if (g_suspendModalCloser)
            return;
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        // Monotonic, never reset: an editor that refuses to accept raises a
        // validation box on every attempt, and there is a moment between that
        // box closing and the editor becoming modal again where no modal is
        // found at all. Resetting on that gap makes the cap unreachable and the
        // test spins forever instead of failing.
        if (++g_modalAttempts > 200) {
            dlg->reject(); // give up rather than spin
            return;
        }
        if (auto *box = dlg->findChild<QDialogButtonBox *>()) {
            if (QPushButton *ok = box->button(QDialogButtonBox::Ok)) {
                ok->click();
                return;
            }
        }
        for (QPushButton *b : dlg->findChildren<QPushButton *>()) {
            const QString t = b->text().remove(QLatin1Char('&'));
            if (t == QStringLiteral("OK") || t == QStringLiteral("Yes")) {
                b->click();
                return;
            }
        }
        dlg->accept();
    });
    timer->start(5);
}

QPushButton *button(QDialog &d, const QString &text)
{
    for (QPushButton *b : d.findChildren<QPushButton *>())
        if (b->text().remove(QLatin1Char('&')) == text)
            return b;
    return nullptr;
}

// Select row 0 of the manager's list, open it with Change…, OK the editor, then
// OK the manager. Returns false if the dialog does not have the expected shape.
bool openFirstRowAndAccept(QDialog &d)
{
    auto *tree = d.findChild<QTreeWidget *>();
    if (!tree || tree->topLevelItemCount() == 0)
        return false;
    tree->setCurrentItem(tree->topLevelItem(0));

    QPushButton *change = button(d, QStringLiteral("Change…"));
    if (!change || !change->isEnabled())
        return false;
    change->click(); // blocks in exec() until installModalCloser()'s timer OKs it
    QApplication::processEvents();

    QPushButton *ok = nullptr;
    if (auto *box = d.findChild<QDialogButtonBox *>())
        ok = box->button(QDialogButtonBox::Ok);
    if (!ok)
        ok = button(d, QStringLiteral("OK"));
    if (!ok)
        return false;
    ok->click();
    return true;
}

void testMathKeepsBareNames()
{
    Configuration config;
    config.clear();
    seedChannels(config);
    MathRow row;
    row.aIsChannel = true;
    row.aChannel = QString::fromUtf8(kChan);
    row.destChannel = QStringLiteral("Engine RPM");
    config.mathRows.append(row);

    MathDialog d(&config);
    if (!openFirstRowAndAccept(d)) {
        std::printf("FAIL  math: could not drive the dialog\n");
        ++fails;
        return;
    }
    CHECK(config.mathRows.size() == 1);
    if (config.mathRows.isEmpty())
        return;
    CHECK(config.mathRows[0].aChannel == QString::fromUtf8(kChan));
    CHECK(config.mathRows[0].destChannel == QStringLiteral("Engine RPM"));
}

void testCountersKeepBareNames()
{
    Configuration config;
    config.clear();
    seedChannels(config);
    CounterRow row;
    row.outputChannel = QStringLiteral("Engine RPM");
    row.upChannel = QString::fromUtf8(kChan);
    config.counterRows.append(row);

    CountersDialog d(&config);
    if (!openFirstRowAndAccept(d)) {
        std::printf("FAIL  counters: could not drive the dialog\n");
        ++fails;
        return;
    }
    CHECK(config.counterRows.size() == 1);
    if (config.counterRows.isEmpty())
        return;
    CHECK(config.counterRows[0].upChannel == QString::fromUtf8(kChan));
    CHECK(config.counterRows[0].outputChannel == QStringLiteral("Engine RPM"));
}

void testIntegratorsKeepBareNames()
{
    Configuration config;
    config.clear();
    seedChannels(config);
    IntegratorRow row;
    row.outputChannel = QStringLiteral("Engine RPM");
    row.inputIsChannel = true;
    row.inputChannel = QString::fromUtf8(kChan);
    config.integratorRows.append(row);

    IntegratorsDialog d(&config);
    if (!openFirstRowAndAccept(d)) {
        std::printf("FAIL  integrators: could not drive the dialog\n");
        ++fails;
        return;
    }
    CHECK(config.integratorRows.size() == 1);
    if (config.integratorRows.isEmpty())
        return;
    CHECK(config.integratorRows[0].inputChannel == QString::fromUtf8(kChan));
    CHECK(config.integratorRows[0].outputChannel == QStringLiteral("Engine RPM"));
}

void testTimersKeepBareNames()
{
    Configuration config;
    config.clear();
    seedChannels(config);
    TimerRow row;
    row.outputChannel = QStringLiteral("Engine RPM");
    row.startChannel = QString::fromUtf8(kChan);
    config.timerRows.append(row);

    TimersDialog d(&config);
    if (!openFirstRowAndAccept(d)) {
        std::printf("FAIL  timers: could not drive the dialog\n");
        ++fails;
        return;
    }
    CHECK(config.timerRows.size() == 1);
    if (config.timerRows.isEmpty())
        return;
    CHECK(config.timerRows[0].startChannel == QString::fromUtf8(kChan));
    CHECK(config.timerRows[0].outputChannel == QStringLiteral("Engine RPM"));
}

void testConditionsKeepBareNames()
{
    Configuration config;
    config.clear();
    seedChannels(config);
    ConditionRow row;
    row.outputChannel = QStringLiteral("Engine RPM");
    // ConditionRow already comes with one term per expression. Appending a
    // second would leave the first one empty, which the editor rightly refuses
    // to accept — and BOTH expressions have to be filled, because the default
    // mode is Set/Reset and a latch with no Reset never clears.
    row.setTerms[0].aChannel = QString::fromUtf8(kChan);
    row.resetTerms[0].aChannel = QString::fromUtf8(kChan);
    config.conditionRows.append(row);

    ConditionsDialog d(&config);
    if (!openFirstRowAndAccept(d)) {
        std::printf("FAIL  conditions: could not drive the dialog\n");
        ++fails;
        return;
    }
    CHECK(config.conditionRows.size() == 1);
    if (config.conditionRows.isEmpty() || config.conditionRows[0].setTerms.isEmpty())
        return;
    CHECK(config.conditionRows[0].setTerms[0].aChannel == QString::fromUtf8(kChan));
    // The Reset half goes through the same channel fields, so it is the same
    // claim and worth asserting rather than assuming.
    CHECK(config.conditionRows[0].resetTerms[0].aChannel == QString::fromUtf8(kChan));
    CHECK(config.conditionRows[0].outputChannel == QStringLiteral("Engine RPM"));
}

// ------------------- the message operators, from a user's setup outward
// A configuration with ONE receive message and ONE transmit message, which is
// the smallest thing anyone builds first, and then: can a User Condition
// actually be pointed at the received one?
//
// This exists because the answer was reported as no. It drives the real row
// editor rather than calling populateMessages directly, because the failure
// being chased is "the option is not there", and a unit test of the list
// builder cannot see an option that the editor never shows.
void testConditionMessagePickerOffersSections()
{
    Configuration config;
    config.clear();
    seedChannels(config);

    CommsSection rx;
    rx.name = QStringLiteral("Engine Data");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    config.bus[0].sections.append(rx);

    CommsSection tx;
    tx.name = QStringLiteral("Status Out");
    tx.device = SectionDevice::TransmitMessage;
    tx.baseAddress = 0x641;
    tx.messageLengthBytes = 8;
    tx.transmitRateHz = 10;
    config.bus[0].sections.append(tx);

    ConditionRow row;
    row.outputChannel = QStringLiteral("Engine RPM");
    row.setTerms[0].aChannel = QString::fromUtf8(kChan);
    row.resetTerms[0].aChannel = QString::fromUtf8(kChan);
    config.conditionRows.append(row);

    ConditionsDialog d(&config);
    auto *tree = d.findChild<QTreeWidget *>();
    CHECK(tree && tree->topLevelItemCount() == 1);
    if (!tree || tree->topLevelItemCount() == 0)
        return;
    tree->setCurrentItem(tree->topLevelItem(0));

    // Everything worth knowing is collected from inside the modal editor, since
    // that is the only place these widgets exist.
    struct Found {
        bool sawEditor = false;
        bool opOffersReceived = false;
        bool opOffersTransmitted = false;
        QStringList messagesForReceived;
        QStringList messagesForTransmitted;
    } found;

    g_suspendModalCloser = true;
    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, qApp, [&found, timer]() {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        timer->stop();
        found.sawEditor = true;

        // The KIND combo — the first control of a comparison, and the one a
        // user actually reaches for. Found by what it offers rather than by
        // position, so a layout change does not silently stop testing this.
        QComboBox *op = nullptr;
        for (QComboBox *c : dlg->findChildren<QComboBox *>())
            if (c->findData(int(COND_OP_MSG_RX)) >= 0 && c->findData(int(COND_OP_MSG_TX)) >= 0) {
                op = c;
                break;
            }
        if (op) {
            const int rx = op->findData(int(COND_OP_MSG_RX));
            const int tx = op->findData(int(COND_OP_MSG_TX));
            found.opOffersReceived = rx >= 0;
            found.opOffersTransmitted = tx >= 0;

            // Selecting a message operator must swap input A for the message
            // list. Whatever combo becomes visible and is not the operator one
            // is that list.
            const auto visibleMessages = [&]() {
                QStringList out;
                for (QComboBox *c : dlg->findChildren<QComboBox *>()) {
                    if (c == op || !c->isVisibleTo(dlg))
                        continue;
                    for (int i = 0; i < c->count(); ++i)
                        if (c->itemData(i).canConvert<QVariantList>())
                            out << c->itemText(i);
                }
                return out;
            };
            if (rx >= 0) {
                op->setCurrentIndex(rx);
                found.messagesForReceived = visibleMessages();
            }
            if (tx >= 0) {
                op->setCurrentIndex(tx);
                found.messagesForTransmitted = visibleMessages();
            }
        }
        dlg->reject(); // nothing here should be committed
    });
    timer->start(1);

    QPushButton *change = button(d, QStringLiteral("Change…"));
    CHECK(change != nullptr);
    if (change)
        change->click(); // blocks until the timer above rejects it
    QApplication::processEvents();
    timer->stop();
    g_suspendModalCloser = false;

    CHECK(found.sawEditor);
    // The two operators are on the list at all — the thing a user looks for.
    CHECK(found.opOffersReceived);
    CHECK(found.opOffersTransmitted);
    // And the receive message is offered under "was received", spelled the way
    // the Communications list spells it.
    CHECK(found.messagesForReceived.contains(
        QStringLiteral("CAN 1 · Section 1 · Rx · Engine Data")));
    // The transmit message is NOT, because it cannot be received.
    CHECK(!found.messagesForReceived.contains(
        QStringLiteral("CAN 1 · Section 2 · Tx · Status Out")));
    // And the other way round under "was transmitted".
    CHECK(found.messagesForTransmitted.contains(
        QStringLiteral("CAN 1 · Section 2 · Tx · Status Out")));
    CHECK(!found.messagesForTransmitted.contains(
        QStringLiteral("CAN 1 · Section 1 · Rx · Engine Data")));
}

// ------------------------- a rename must reach an OPEN dialog's working copy
// The channel picker's Edit… commits a rename to the document immediately —
// catalog entry replaced, every stored reference rewritten — and it is
// reachable from INSIDE a grid dialog, whose working copy was snapshotted
// before the rename and is assigned back wholesale on OK. Without the
// Configuration::channelRenamed listener, that OK writes the old name straight
// back over the document, where it now names a channel the catalog no longer
// holds. This drives the exact sequence at the counters grid: dialog open,
// rename committed underneath it, dialog OK'd.
void testRenameReachesOpenWorkingCopy()
{
    Configuration config;
    config.clear();
    seedChannels(config);
    CounterRow producer;
    producer.outputChannel = QString::fromUtf8(kChan);
    CounterRow consumer;
    consumer.outputChannel = QStringLiteral("Engine RPM");
    consumer.upChannel = QString::fromUtf8(kChan);
    config.counterRows << producer << consumer;

    CountersDialog d(&config); // snapshots its working copy of counterRows

    // The rename, exactly as EditChannelDialog::createOrEdit commits it.
    Channel renamed = config.catalog().findByName(QString::fromUtf8(kChan));
    CHECK(renamed.isValid());
    renamed.name = QStringLiteral("Water Temp");
    config.catalog().removeUserChannel(QString::fromUtf8(kChan));
    config.catalog().addOrUpdateUserChannel(renamed);
    CHECK(config.renameChannelReferences(QString::fromUtf8(kChan),
                                         QStringLiteral("Water Temp")) == 2);

    // OK the grid dialog. Its working copy must carry the rename onward, not
    // resurrect the old name.
    auto *box = d.findChild<QDialogButtonBox *>();
    QPushButton *ok = box ? box->button(QDialogButtonBox::Ok) : nullptr;
    CHECK(ok != nullptr);
    if (!ok)
        return;
    ok->click();
    CHECK(config.counterRows.size() == 2);
    if (config.counterRows.size() != 2)
        return;
    CHECK(config.counterRows[0].outputChannel == QStringLiteral("Water Temp"));
    CHECK(config.counterRows[1].upChannel == QStringLiteral("Water Temp"));
}

// ------------------------------- and the row editors' live channel FIELDS too
// A row editor reads EVERY channel field back at OK, never just the one that
// was picked, so a field still holding the renamed-away name would write it
// back into a row the working-copy fix above just carried forward.
// renameOpenChannelFields() re-points them wherever they live.
void testRenameRepointsOpenChannelFields()
{
    Configuration config;
    config.clear();
    seedChannels(config);

    QLineEdit field; // parentless: a top-level widget, like a bare row editor
    setChannelField(&field, QString::fromUtf8(kChan), config.catalog());
    QLineEdit other;
    setChannelField(&other, QStringLiteral("Engine RPM"), config.catalog());

    Channel renamed = config.catalog().findByName(QString::fromUtf8(kChan));
    renamed.name = QStringLiteral("Water Temp");
    config.catalog().removeUserChannel(QString::fromUtf8(kChan));
    config.catalog().addOrUpdateUserChannel(renamed);

    CHECK(renameOpenChannelFields(QString::fromUtf8(kChan), QStringLiteral("Water Temp"),
                                  config.catalog())
          == 1);
    CHECK(channelField(&field) == QStringLiteral("Water Temp"));
    // The visible label follows, unit and all.
    CHECK(field.text() == QString::fromUtf8("Water Temp \xC2\xB0" "C"));
    // A field naming a different channel is left alone.
    CHECK(channelField(&other) == QStringLiteral("Engine RPM"));
}

// Every device channel must be internally consistent: the storage type it
// declares has to actually hold the range it declares, at the precision it
// declares. Nothing else checks this. A device channel is never typed through
// the Channel Editor — the dialog opens it read-only — so the validation that
// catches a bad user channel never runs over these, and the mapper stamps the
// declared type onto the value slot without asking whether it fits.
//
// Bus Load is why this test exists. It is a percentage carried to one decimal
// place, and a scaled-integer channel stores range * 10^decimals: 0…100 at 1 dp
// is 0…1000 raw counts, which a u8 cannot hold. Declared as u8 it would have
// clipped every reading above 25.5 % — a number that looks entirely plausible
// on a busy bus, on a channel already documented as an estimate.
static void testDeviceChannelsAreSelfConsistent()
{
    const QList<Channel> devices = ChannelCatalog::deviceChannels();
    CHECK(!devices.isEmpty());
    for (const Channel &c : devices) {
        CHECK(!c.name.isEmpty());
        CHECK(!c.dataType.isEmpty());
        CHECK(c.deviceChannelId >= 0);
        CHECK(!c.userDefined);
        CHECK(c.maxValue > c.minValue);
        // The property that matters.
        CHECK(storageTypeHoldsRange(c.dataType, c.minValue, c.maxValue, c.decimalPlaces));
        // And the unit has to be one the quantity actually offers, or the
        // Channel Editor would show a blank unit box for a built-in channel.
        CHECK(ChannelCatalog::unitsForQuantity(c.quantity).contains(c.unit));
    }

    // Ids are unique and each one round-trips back to its channel — the mapper
    // indexes the wire struct by these, so a duplicate would have two channels
    // fighting over one destination slot.
    QSet<int> ids;
    for (const Channel &c : devices) {
        CHECK(!ids.contains(c.deviceChannelId));
        ids.insert(c.deviceChannelId);
        CHECK(ChannelCatalog::deviceChannelById(c.deviceChannelId).name == c.name);
    }
    // An id this build has no channel for comes back invalid rather than
    // matching something by accident.
    CHECK(!ChannelCatalog::deviceChannelById(-1).isValid());
    CHECK(!ChannelCatalog::deviceChannelById(9999).isValid());

    // Every one of them answers isDeviceChannel(), which is what makes the
    // editor open them read-only and stops Lua creating one.
    for (const Channel &c : devices)
        CHECK(ChannelCatalog::isDeviceChannel(c.name));
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    installModalCloser();

    testLabelHelpers();
    testChannelFieldSeparatesDisplayFromIdentity();
    testMathKeepsBareNames();
    testCountersKeepBareNames();
    testIntegratorsKeepBareNames();
    testTimersKeepBareNames();
    testConditionsKeepBareNames();
    testConditionMessagePickerOffersSections();
    testRenameReachesOpenWorkingCopy();
    testRenameRepointsOpenChannelFields();
    testDeviceChannelsAreSelfConsistent();

    if (fails == 0) {
        std::printf("test_channel_labels: all checks passed\n");
        return 0;
    }
    std::printf("test_channel_labels: %d check(s) failed\n", fails);
    return 1;
}
