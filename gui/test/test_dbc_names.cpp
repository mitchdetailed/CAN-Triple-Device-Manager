// What a DBC signal is imported as: underscores become spaces.
//
// DBC signal names are C identifiers, so an author who wants "Engine Speed" has
// to write "Engine_Speed". The underscore is the format's limitation and not
// the name, and the channel catalogue has no such rule — "Coolant Temp" is how
// every hand-made channel in the app is spelled.
//
// THE PART THAT IS EASY TO GET WRONG is where the substitution happens. The
// file refers to its OWN signals by the underscored name: a SIG_VALTYPE_ line
// naming Boost_Pressure is how a float signal is marked as float, and it is
// matched against DbcSignal::name. Rewrite the name in the parser and every
// float signal in the file silently stops being a float. So the parser keeps
// the file's spelling and the substitution belongs at the one point where a
// signal becomes a CHANNEL — the import dialog's name column, which the user
// can see and edit before anything is created.
//
// Both halves are pinned here, together, because either alone would look fine.

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>

#include <cmath>
#include <cstdio>

#include "../src/model/channel_catalog.h"
#include "../src/model/frame_layout.h"
#include "../src/model/configuration.h"
#include "../src/model/dbc_import.h"
#include "../src/ui/import_dbc_dialog.h"

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

const char *const kDbc = R"DBC(VERSION "unit-test"

BO_ 1600 EngineData: 8 ECU
 SG_ Engine_Speed : 0|16@1+ (1,0) [0|20000] "rpm" Dash
 SG_ Coolant_Temp_Sensor : 16|16@1- (0.1,-40) [-40|215] "degC" Dash
 SG_ Boost_Pressure : 32|32@1+ (1,0) [0|500] "kPa" Dash

SIG_VALTYPE_ 1600 Boost_Pressure : 1;
)DBC";

// ------------------------------------------------------------ the transform

void testUnderscoresBecomeSpaces()
{
    CHECK(channelNameFromDbcSignal(QStringLiteral("Engine_Speed"))
          == QStringLiteral("Engine Speed"));
    CHECK(channelNameFromDbcSignal(QStringLiteral("Coolant_Temp_Sensor"))
          == QStringLiteral("Coolant Temp Sensor"));
    // Every one of them, not just the first.
    CHECK(channelNameFromDbcSignal(QStringLiteral("A_B_C_D_E"))
          == QStringLiteral("A B C D E"));
}

void testANameWithoutUnderscoresIsUntouched()
{
    CHECK(channelNameFromDbcSignal(QStringLiteral("EngineSpeed"))
          == QStringLiteral("EngineSpeed"));
    // Digits and case are not this function's business.
    CHECK(channelNameFromDbcSignal(QStringLiteral("Bank1O2")) == QStringLiteral("Bank1O2"));
}

void testTheEndsAndTheDoublesAreTidied()
{
    // A straight character swap would leave these, and each is a name a user
    // would then have to fix by hand in the import dialog.
    CHECK(channelNameFromDbcSignal(QStringLiteral("Engine__Speed"))
          == QStringLiteral("Engine Speed"));
    CHECK(channelNameFromDbcSignal(QStringLiteral("_Rpm")) == QStringLiteral("Rpm"));
    CHECK(channelNameFromDbcSignal(QStringLiteral("Rpm_")) == QStringLiteral("Rpm"));
    CHECK(channelNameFromDbcSignal(QStringLiteral("__Rpm__")) == QStringLiteral("Rpm"));
}

void testANameOfNothingButUnderscoresComesBackEmpty()
{
    // Empty is the right answer rather than a string of spaces: the importer
    // already turns an empty base into "Signal", so this lands in a path that
    // exists instead of creating a channel named " ".
    CHECK(channelNameFromDbcSignal(QStringLiteral("___")).isEmpty());
    CHECK(channelNameFromDbcSignal(QString()).isEmpty());
}

// -------------------------------------------------- the file's own spelling

void testTheParserKeepsTheUnderscoredName()
{
    // THE PAIRING THAT MATTERS. If the substitution were done in the parser,
    // DbcSignal::name would read "Boost Pressure" and the SIG_VALTYPE_ line —
    // which spells it "Boost_Pressure" — would match nothing, leaving the
    // signal an integer. The float would be decoded as raw bits.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    REQUIRE(file.messages.size() == 1);
    const DbcMessage &msg = file.messages.first();
    REQUIRE(msg.signalList.size() == 3);

    CHECK(msg.signalList[0].name == QStringLiteral("Engine_Speed"));
    CHECK(msg.signalList[1].name == QStringLiteral("Coolant_Temp_Sensor"));
    CHECK(msg.signalList[2].name == QStringLiteral("Boost_Pressure"));

    // Resolved, which is only possible because the name still matches the file.
    CHECK(msg.signalList[2].valueType == 1);
    std::printf("  SIG_VALTYPE_ resolved              : %s\n",
                msg.signalList[2].valueType == 1 ? "yes" : "NO");
}

// ------------------------------------------------------------- end to end

// Check every signal in the tree and import, returning the dialog's sections.
QList<CommsSection> importAll(Configuration &config, const DbcFile &file, QStringList *shownNames,
                              const QString &editFirstTo = QString(),
                              const QString &pickUnitOnFirst = QString())
{
    ImportDbcDialog dialog(&config, file, QStringLiteral("unit-test.dbc"), 0, {});
    auto *tree = dialog.findChild<QTreeWidget *>();
    if (!tree)
        return {};
    for (int m = 0; m < tree->topLevelItemCount(); ++m) {
        QTreeWidgetItem *msg = tree->topLevelItem(m);
        for (int s = 0; s < msg->childCount(); ++s) {
            QTreeWidgetItem *sig = msg->child(s);
            if (shownNames)
                shownNames->append(sig->text(0));
            if (s == 0 && !editFirstTo.isEmpty())
                sig->setText(0, editFirstTo);
            if (s == 0 && !pickUnitOnFirst.isEmpty())
                sig->setText(3, pickUnitOnFirst);
            sig->setCheckState(0, Qt::Checked);
        }
    }
    // OK through the dialog's own button box: accept() is protected, and
    // clicking OK is the path a user takes anyway. It is enabled only once
    // something is checked, which the loop above has just done.
    auto *box = dialog.findChild<QDialogButtonBox *>();
    if (!box || !box->button(QDialogButtonBox::Ok))
        return {};
    // accept() puts up a notes box when anything was renamed, clipped or left
    // without a unit, and that box is MODAL - it would hold this test open for
    // ever. Dismissed from a timer. m_sections is assigned before the box goes
    // up, so closing it does not lose the answer.
    QTimer notes;
    int ticks = 0;
    QObject::connect(&notes, &QTimer::timeout, [&notes, &ticks]() {
        if (QWidget *modal = QApplication::activeModalWidget()) {
            modal->close();
            notes.stop();
        } else if (++ticks > 200) {
            notes.stop(); // no box: the ordinary case
        }
    });
    notes.start(5);
    box->button(QDialogButtonBox::Ok)->click();
    notes.stop();
    return dialog.importedSections();
}

void testTheImportDialogShowsTheSpacedName()
{
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    Configuration config;
    config.clear();

    QStringList shown;
    const QList<CommsSection> sections = importAll(config, file, &shown);
    std::printf("  names offered by the dialog        : %s\n",
                qPrintable(shown.join(QStringLiteral(" | "))));
    CHECK(shown == QStringList({QStringLiteral("Engine Speed"),
                                QStringLiteral("Coolant Temp Sensor"),
                                QStringLiteral("Boost Pressure")}));

    // And the channels really are created under those names — the column is the
    // identity, not a preview of one.
    REQUIRE(sections.size() == 1);
    QStringList rowNames;
    for (const CommsChannelRow &r : sections.first().rows)
        rowNames << r.channelName;
    std::printf("  channels the import created        : %s\n",
                qPrintable(rowNames.join(QStringLiteral(" | "))));
    CHECK(rowNames == shown);
    for (const QString &n : rowNames)
        CHECK(config.catalog().findByName(n).isValid());
    // Nothing keeps the underscored spelling.
    for (const QString &n : rowNames)
        CHECK(!n.contains(QLatin1Char('_')));
}

void testTheUsersOwnEditStillWins()
{
    // The column is editable, and the substitution is a default rather than a
    // rule imposed on the name. Someone who wants the underscore back can have
    // it.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    Configuration config;
    config.clear();

    const QList<CommsSection> sections =
        importAll(config, file, nullptr, QStringLiteral("Engine_Speed"));
    REQUIRE(sections.size() == 1);
    REQUIRE(!sections.first().rows.isEmpty());
    std::printf("  after editing the name back        : %s\n",
                qPrintable(sections.first().rows.first().channelName));
    CHECK(sections.first().rows.first().channelName == QStringLiteral("Engine_Speed"));
}


// ------------------------------------------------------------------ units

// A .dbc writes its unit as free text, and every tool spells it differently.
// "degC", "Deg C" and "Celsius" are one unit, and none of them is how this
// application spells it: the catalogue offers "C". Importing the file's string
// verbatim gave channels a unit no list contains - it matched no other channel,
// could not be picked from any combo, and had to be retyped by hand afterwards.
const char *const kUnitDbc = R"DBC(VERSION "unit-test"

BO_ 1600 EngineData: 8 ECU
 SG_ Coolant : 0|16@1+ (0.1,-40) [-40|215] "degC" Dash
 SG_ Road_Speed : 16|16@1+ (1,0) [0|400] "kph" Dash
 SG_ Engine_Speed : 32|16@1+ (1,0) [0|20000] "1/min" Dash
 SG_ Strange : 48|8@1+ (1,0) [0|255] "Nm/deg" Dash
)DBC";

void testAKnownUnitBecomesTheCatalogueSpelling()
{
    // Each of these is a unit the app has, spelled the way a DBC spells it.
    CHECK(dbcUnitFor(QStringLiteral("degC")).unit == QStringLiteral("C"));
    CHECK(dbcUnitFor(QStringLiteral("degC")).quantity == QStringLiteral("Temperature"));
    CHECK(dbcUnitFor(QStringLiteral("kph")).unit == QStringLiteral("km/h"));
    CHECK(dbcUnitFor(QStringLiteral("1/min")).unit == QStringLiteral("rpm"));
    CHECK(dbcUnitFor(QStringLiteral("mph")).unit == QStringLiteral("mile/h"));
    // degF is Temperature too, but it is NOT C - a quantity-only mapping with a
    // default would have made it one, which is the trap this table avoids.
    CHECK(dbcUnitFor(QStringLiteral("degF")).unit == QStringLiteral("F"));
    CHECK(dbcUnitFor(QStringLiteral("K")).unit == QStringLiteral("K"));
    // Case and whitespace are the file's business, not ours.
    CHECK(dbcUnitFor(QStringLiteral("  DEGC ")).unit == QStringLiteral("C"));
    // All of them recognised, so nothing is flagged.
    CHECK(dbcUnitFor(QStringLiteral("degC")).recognised);
}

void testEveryMappedUnitIsOneTheCatalogueOffers()
{
    // THE PROPERTY THAT MATTERS, over the whole table rather than a sample: an
    // import must never produce a (quantity, unit) pair the Edit Custom Channel
    // combos cannot show, because that channel then cannot be edited back to
    // anything without retyping. Adding a row to the table with a typo in the
    // unit is exactly the mistake this catches.
    const char *const spellings[] = {
        "degC", "C", "\u00b0C", "Celsius", "degF", "F", "K", "Kelvin",
        "kPa", "Pa", "MPa", "bar", "mbar", "psi", "hPa", "inHg", "mmHg", "atm",
        "km/h", "kph", "kmh", "mph", "mi/h", "m/s", "knots", "kn",
        "rpm", "1/min", "rev/min", "min-1",
        "V", "mV", "kV", "volt", "volts", "A", "mA", "amp", "amps",
        "deg", "degree", "degrees", "rad", "radians",
        "Nm", "N.m", "ftlb", "lbft", "W", "kW", "hp", "ps", "watt",
        "%", "percent", "ratio", "s", "sec", "secs", "ms", "us", "min", "h", "hr", "hour",
        "kg", "g", "mg", "t", "N", "lb", "lbs", "oz", "newton",
        "l", "ml", "cc", "cm3", "gal", "gallon",
        "l/h", "l/min", "l/s", "cc/min", "cc/s",
        "g/s", "kg/h", "kg/s", "g/min", "lb/h",
        "ohm", "ohms", "kohm", "lambda", "afr", "a/f",
        "g/s/s", "m/s2", "m/s^2",
    };
    int checked = 0;
    for (const char *s : spellings) {
        const DbcUnit u = dbcUnitFor(QString::fromUtf8(s));
        REQUIRE(u.recognised);
        const QStringList allowed = ChannelCatalog::unitsForQuantity(u.quantity);
        if (!allowed.contains(u.unit))
            std::printf("       \"%s\" -> %s / \"%s\" which %s does not offer\n", s,
                        qPrintable(u.quantity), qPrintable(u.unit), qPrintable(u.quantity));
        CHECK(allowed.contains(u.unit));
        CHECK(ChannelCatalog::quantities().contains(u.quantity));
        ++checked;
    }
    std::printf("  spellings checked against the catalogue : %d\n", checked);
}

void testAnUnplaceableUnitIsFlaggedRatherThanGuessed()
{
    const DbcUnit u = dbcUnitFor(QStringLiteral("Nm/deg"));
    std::printf("  \"Nm/deg\" -> %s / \"%s\", recognised=%d\n",
                qPrintable(u.quantity), qPrintable(u.unit), int(u.recognised));
    CHECK(!u.recognised);
    CHECK(u.quantity == QStringLiteral("Unitless"));
    CHECK(u.unit.isEmpty());

    // AN EMPTY UNIT IS NOT AN UNRECOGNISED ONE. A DBC signal with "" really is
    // unitless, and flagging every one of those would bury the handful that
    // need a decision under the many that do not.
    const DbcUnit none = dbcUnitFor(QString());
    CHECK(none.recognised);
    CHECK(none.quantity == QStringLiteral("Unitless"));
}

void testTheImportPanelOffersTheCatalogueUnit()
{
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kUnitDbc), &warnings);
    Configuration config;
    config.clear();
    ImportDbcDialog dialog(&config, file, QStringLiteral("unit-test.dbc"), 0, {});
    auto *tree = dialog.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    REQUIRE(tree->topLevelItemCount() == 1);
    QTreeWidgetItem *msg = tree->topLevelItem(0);
    REQUIRE(msg->childCount() == 4);

    QStringList shown;
    for (int i = 0; i < msg->childCount(); ++i)
        shown << msg->child(i)->text(1) + QStringLiteral("/") + msg->child(i)->text(3);
    std::printf("  type/unit offered            : %s\n",
                qPrintable(shown.join(QStringLiteral("  "))));

    CHECK(msg->child(0)->text(1) == QStringLiteral("Temperature"));
    CHECK(msg->child(0)->text(3) == QStringLiteral("C"));       // not "degC"
    CHECK(msg->child(1)->text(3) == QStringLiteral("km/h"));    // not "kph"
    CHECK(msg->child(2)->text(3) == QStringLiteral("rpm"));     // not "1/min"
    // The one nothing matched is marked for the user rather than guessed.
    CHECK(msg->child(3)->text(3) == QStringLiteral("(pick one)"));
    CHECK(msg->child(3)->toolTip(3).contains(QStringLiteral("Nm/deg")));
}

void testTheImportedChannelCarriesTheChosenUnit()
{
    // The Unit column used to be shown and then DISCARDED - the channel took
    // the DBC's raw string no matter what the column said. It is read back now.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kUnitDbc), &warnings);
    Configuration config;
    config.clear();
    const QList<CommsSection> sections = importAll(config, file, nullptr);
    REQUIRE(!sections.isEmpty());

    const Channel coolant = config.catalog().findByName(QStringLiteral("Coolant"));
    REQUIRE(coolant.isValid());
    std::printf("  imported Coolant             : %s / \"%s\"\n",
                qPrintable(coolant.quantity), qPrintable(coolant.unit));
    CHECK(coolant.unit == QStringLiteral("C"));
    CHECK(coolant.quantity == QStringLiteral("Temperature"));
    CHECK(ChannelCatalog::unitsForQuantity(coolant.quantity).contains(coolant.unit));

    // AND THE COLUMN IS WHAT DECIDES. Picking a different unit in the panel
    // has to reach the channel: the column used to be shown and discarded, and
    // a canonical default alone would look identical here.
    Configuration picked;
    picked.clear();
    importAll(picked, file, nullptr, QString(), QStringLiteral("F"));
    const Channel asF = picked.catalog().findByName(QStringLiteral("Coolant"));
    REQUIRE(asF.isValid());
    std::printf("  Coolant with F picked        : %s / \"%s\"\n",
                qPrintable(asF.quantity), qPrintable(asF.unit));
    CHECK(asF.unit == QStringLiteral("F"));

    // The unplaceable one imports UNITLESS rather than carrying the placeholder
    // text as though it were a unit.
    const Channel strange = config.catalog().findByName(QStringLiteral("Strange"));
    REQUIRE(strange.isValid());
    std::printf("  imported Strange             : %s / \"%s\"\n",
                qPrintable(strange.quantity), qPrintable(strange.unit));
    CHECK(strange.unit.isEmpty());
}


// -------------------------------------------------------------- multiplexing

// THE MULTIPLEXOR IS NOT A CHANNEL.
//
// A multiplexed DBC message imports as a COMPOUND section: each multiplexor
// value becomes an identifier, and the identifier's selector is the multiplexor
// signal's own bits. Importing that signal as a channel row as well put a
// channel exactly on top of the selector - and the device writes the selector
// into the frame AFTER the channels, so the channel does not share those bits,
// it is replaced by them. The section editor refuses to save it.
//
// Which is what made this a bad shape rather than merely a wasted row: the
// import succeeded, and the complaint arrived later, from the editor, about a
// row the user never chose to put anywhere.
const char *const kMuxDbc = R"DBC(VERSION "unit-test"

BO_ 512 MuxMsg: 8 ECU
 SG_ Selector M : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ Common_Value : 48|16@1+ (1,0) [0|65535] "" Vector__XXX
 SG_ Val_A m0 : 8|16@1+ (1,0) [0|65535] "" Vector__XXX
 SG_ Val_B m1 : 8|16@1+ (1,0) [0|65535] "" Vector__XXX
)DBC";

void testTheMultiplexorIsNotOfferedAsAChannel()
{
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kMuxDbc), &warnings);
    Configuration config;
    config.clear();
    ImportDbcDialog dialog(&config, file, QStringLiteral("mux.dbc"), 0, {});
    auto *tree = dialog.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    REQUIRE(tree->topLevelItemCount() == 1);
    QTreeWidgetItem *msg = tree->topLevelItem(0);
    REQUIRE(msg->childCount() == 4);

    // Still SHOWN - the row is how a reader sees which signal picks the variant
    // - but with no checkbox, and the Details column says what becomes of it.
    QTreeWidgetItem *selector = nullptr;
    for (int i = 0; i < msg->childCount(); ++i)
        if (msg->child(i)->text(0) == QStringLiteral("Selector"))
            selector = msg->child(i);
    REQUIRE(selector != nullptr);
    std::printf("  multiplexor row              : checkable=%d  %s\n",
                int(bool(selector->flags() & Qt::ItemIsUserCheckable)),
                qPrintable(selector->text(2)));
    CHECK(!(selector->flags() & Qt::ItemIsUserCheckable));
    CHECK(selector->text(2).contains(QStringLiteral("identifier")));

    // Every other signal is offered as usual.
    for (int i = 0; i < msg->childCount(); ++i) {
        if (msg->child(i) == selector)
            continue;
        CHECK(msg->child(i)->flags() & Qt::ItemIsUserCheckable);
    }
}

void testTheImportedCompoundSectionHasNoBlockingClash()
{
    // THE SYMPTOM, end to end. Import everything the panel offers, then ask
    // frame_layout the same question the section editor asks when OK is
    // pressed. It used to answer "the identifier writes its selector over
    // channel Selector".
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kMuxDbc), &warnings);
    Configuration config;
    config.clear();
    const QList<CommsSection> sections = importAll(config, file, nullptr);
    REQUIRE(sections.size() == 1);
    const CommsSection &s = sections.first();
    REQUIRE(s.compound);
    REQUIRE(s.identifiers.size() == 2);

    QStringList blocking;
    for (const LayoutClash &clash : findLayoutClashes(s))
        if (clash.blocking)
            blocking << clash.message();
    std::printf("  blocking clashes on import   : %d%s\n", int(blocking.size()),
                blocking.isEmpty() ? "" : qPrintable(QStringLiteral("  ") + blocking.first()));
    CHECK(blocking.isEmpty());

    // And no channel called Selector was created at all.
    CHECK(!config.catalog().findByName(QStringLiteral("Selector")).isValid());
    for (const CompoundIdentifier &ident : s.identifiers)
        for (const CommsChannelRow &row : ident.rows)
            CHECK(row.channelName != QStringLiteral("Selector"));

    // The signals that ARE channels came through: the common one replicated
    // into each variant, and one muxed signal per identifier.
    CHECK(config.catalog().findByName(QStringLiteral("Common Value")).isValid());
    CHECK(config.catalog().findByName(QStringLiteral("Val A")).isValid());
    CHECK(config.catalog().findByName(QStringLiteral("Val B")).isValid());
}

void testTheMessageRowStillReadsFullyChecked()
{
    // A consequence of taking the checkbox away, and one that would have been
    // its own small annoyance: the parent tristate counted ALL children, so a
    // multiplexed message could never reach Checked - every signal it offers
    // ticked and the header still showing partial, for ever.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kMuxDbc), &warnings);
    Configuration config;
    config.clear();
    ImportDbcDialog dialog(&config, file, QStringLiteral("mux.dbc"), 0, {});
    auto *tree = dialog.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    QTreeWidgetItem *msg = tree->topLevelItem(0);
    REQUIRE(msg != nullptr);

    // Tick everything the panel actually offers, exactly as a user would.
    for (int i = 0; i < msg->childCount(); ++i)
        if (msg->child(i)->flags() & Qt::ItemIsUserCheckable)
            msg->child(i)->setCheckState(0, Qt::Checked);
    std::printf("  message row after ticking all: %s\n",
                msg->checkState(0) == Qt::Checked      ? "Checked"
                : msg->checkState(0) == Qt::PartiallyChecked ? "PartiallyChecked"
                                                             : "Unchecked");
    CHECK(msg->checkState(0) == Qt::Checked);
}

void testSelectAllLeavesTheMultiplexorAlone()
{
    // Select All walks every child and sets its state, and setCheckState writes
    // the role whether the item is user-checkable or not - so without a guard
    // the button puts a ticked checkbox back on the one row that must not have
    // one.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kMuxDbc), &warnings);
    Configuration config;
    config.clear();
    ImportDbcDialog dialog(&config, file, QStringLiteral("mux.dbc"), 0, {});
    auto *tree = dialog.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    QPushButton *selectAll = nullptr;
    for (QPushButton *b : dialog.findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Select All"))
            selectAll = b;
    REQUIRE(selectAll != nullptr);
    selectAll->click();

    QTreeWidgetItem *msg = tree->topLevelItem(0);
    REQUIRE(msg != nullptr);
    int ticked = 0;
    QTreeWidgetItem *selector = nullptr;
    for (int i = 0; i < msg->childCount(); ++i) {
        if (msg->child(i)->text(0) == QStringLiteral("Selector"))
            selector = msg->child(i);
        else if (msg->child(i)->checkState(0) == Qt::Checked)
            ++ticked;
    }
    REQUIRE(selector != nullptr);
    std::printf("  after Select All             : %d signals ticked, multiplexor %s\n",
                ticked,
                selector->checkState(0) == Qt::Checked ? "TICKED" : "left alone");
    CHECK(ticked == 3);
    CHECK(selector->checkState(0) != Qt::Checked);
}

void testAMessageWithNoMuxedSignalsPickedStaysPlain()
{
    // The other branch, and the reason the multiplexor is held back rather than
    // dropped outright: with no multiplexed signal selected there is no
    // identifier, nothing writes over those bits, and the multiplexor is an
    // ordinary field the user may well want. This DBC has no multiplexing at
    // all, which is that case in its simplest form - the signal is just a
    // signal.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    Configuration config;
    config.clear();
    const QList<CommsSection> sections = importAll(config, file, nullptr);
    REQUIRE(sections.size() == 1);
    CHECK(!sections.first().compound);
    CHECK(sections.first().rows.size() == 3);
}

} // namespace

// ------------------------------------------------------------- the details

void testAFloatSignalIsLabelledIEEE754InTheDetails()
{
    // Reported as a bug: the Details column read the SG_ line's sign flag and
    // never looked at the SIG_VALTYPE_ marker, so a float signal was labelled
    // "unsigned" (its line reads @1+) while the import made it an IEEE754 row.
    // kDbc marks Boost_Pressure as a float; the other two are integers.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    Configuration config;
    config.clear();
    ImportDbcDialog dialog(&config, file, QStringLiteral("unit-test.dbc"), 0, {});
    auto *tree = dialog.findChild<QTreeWidget *>();
    REQUIRE(tree && tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 3);
    const QString speed = tree->topLevelItem(0)->child(0)->text(2);
    const QString coolant = tree->topLevelItem(0)->child(1)->text(2);
    const QString boost = tree->topLevelItem(0)->child(2)->text(2);
    std::printf("  Boost Pressure details             : %s\n", qPrintable(boost));
    CHECK(boost.contains(QStringLiteral("IEEE754")));
    CHECK(!boost.contains(QStringLiteral("unsigned")));
    CHECK(!boost.contains(QStringLiteral(" signed")));
    // The integer signals still say what they are.
    CHECK(speed.contains(QStringLiteral("unsigned")));
    CHECK(coolant.contains(QStringLiteral(" signed")));
    CHECK(!speed.contains(QStringLiteral("IEEE754")));
    // And in transmit mode the column reads the same.
    auto *mode = dialog.findChild<QComboBox *>(QStringLiteral("importAs"));
    REQUIRE(mode);
    mode->setCurrentIndex(1);
    CHECK(tree->topLevelItem(0)->child(2)->text(2).contains(QStringLiteral("IEEE754")));
}

// ---------------------------------------------------------------- transmit

// THE OTHER DIRECTION. A .dbc written from the far side of the wire \u2014 what a
// cluster, a dash or a gearbox controller expects to RECEIVE \u2014 is, from this
// device's side, a list of messages to send. The same tree imports it as
// transmit sections, and three things are different: no channel is created (a
// row SENDS an existing channel, named in the Send Channel column and
// prefilled by name); the file's offset changes sign, because this
// application's transmit row ADDS its Offset where a DBC subtracts; and the
// file's cycle time becomes the section's period.

Channel userChannel(const QString &name)
{
    Channel c;
    c.name = name;
    c.dataType = QStringLiteral("u16");
    c.category = QStringLiteral("User Channels");
    c.userDefined = true;
    return c;
}

// Import with the dialog in transmit mode: every signal ticked, an optional
// Send Channel typed into one row (the picker writes the same cell), and the
// column as first shown returned for inspection.
QList<CommsSection> importTransmit(Configuration &config, const DbcFile &file,
                                   QStringList *sendColumn = nullptr, int linkRow = -1,
                                   const QString &linkTo = QString(),
                                   bool *importEnabled = nullptr)
{
    ImportDbcDialog dialog(&config, file, QStringLiteral("unit-test.dbc"), 0, {});
    auto *mode = dialog.findChild<QComboBox *>(QStringLiteral("importAs"));
    auto *tree = dialog.findChild<QTreeWidget *>();
    if (!mode || !tree)
        return {};
    mode->setCurrentIndex(1); // Transmit Messages
    int n = 0;
    for (int m = 0; m < tree->topLevelItemCount(); ++m) {
        QTreeWidgetItem *msg = tree->topLevelItem(m);
        for (int s = 0; s < msg->childCount(); ++s, ++n) {
            QTreeWidgetItem *sig = msg->child(s);
            if (sendColumn)
                sendColumn->append(sig->text(1));
            if (n == linkRow && !linkTo.isEmpty())
                sig->setText(1, linkTo);
            if (sig->flags() & Qt::ItemIsUserCheckable)
                sig->setCheckState(0, Qt::Checked);
        }
    }
    auto *box = dialog.findChild<QDialogButtonBox *>();
    if (!box || !box->button(QDialogButtonBox::Ok))
        return {};
    if (importEnabled)
        *importEnabled = box->button(QDialogButtonBox::Ok)->isEnabled();
    if (!box->button(QDialogButtonBox::Ok)->isEnabled())
        return {};
    // The notes box is modal; see importAll.
    QTimer notes;
    int ticks = 0;
    QObject::connect(&notes, &QTimer::timeout, [&notes, &ticks]() {
        if (QWidget *modal = QApplication::activeModalWidget()) {
            modal->close();
            notes.stop();
        } else if (++ticks > 200) {
            notes.stop();
        }
    });
    notes.start(5);
    box->button(QDialogButtonBox::Ok)->click();
    notes.stop();
    return dialog.importedSections();
}

void testATransmitImportSendsExistingChannels()
{
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    Configuration config;
    config.clear();
    // Two of the three signals already have a channel. The file's case differs
    // from the catalogue's on one of them, and the catalogue's spelling wins.
    config.catalog().addOrUpdateUserChannel(userChannel(QStringLiteral("Engine Speed")));
    config.catalog().addOrUpdateUserChannel(userChannel(QStringLiteral("coolant temp sensor")));
    const int before = config.catalog().userChannels().size();

    QStringList sendColumn;
    const QList<CommsSection> sections = importTransmit(config, file, &sendColumn);
    std::printf("  Send Channel column as shown       : %s\n",
                qPrintable(sendColumn.join(QStringLiteral(" | "))));
    CHECK(sendColumn == QStringList({QStringLiteral("Engine Speed"),
                                     QStringLiteral("coolant temp sensor"),
                                     QStringLiteral("(pick one)")}));

    REQUIRE(sections.size() == 1);
    const CommsSection &s = sections.first();
    CHECK(s.device == SectionDevice::TransmitMessage);
    CHECK(s.isTransmit());
    CHECK(s.cyclic);
    CHECK(s.name == QStringLiteral("EngineData"));
    CHECK(s.baseAddress == 1600u);
    CHECK(s.messageLengthBytes == 8);
    // The dialog's rate, since this file states no cycle time.
    CHECK(s.transmitRateHz == 50);
    CHECK(s.transmitPeriodMs == 0);
    // Two rows: the third signal was ticked but had nothing to send.
    REQUIRE(s.rows.size() == 2);
    std::printf("  rows                               : %s, %s\n",
                qPrintable(s.rows[0].channelName), qPrintable(s.rows[1].channelName));
    CHECK(s.rows[0].channelName == QStringLiteral("Engine Speed"));
    CHECK(s.rows[1].channelName == QStringLiteral("coolant temp sensor"));
    CHECK(s.rows[0].startBit == 0 && s.rows[0].bitLength == 16);
    CHECK(s.rows[1].startBit == 16 && s.rows[1].bitLength == 16);
    CHECK(s.rows[1].dbcType == int(DbcType::Signed));
    // And nothing was created: not the matched ones again, not the skipped one.
    CHECK(config.catalog().userChannels().size() == before);
    CHECK(!config.catalog().findByName(QStringLiteral("Boost Pressure")).isValid());
}

void testTheTransmitOffsetChangesSign()
{
    // The rule on its own first.
    DbcSignal sig;
    sig.factor = 0.1;
    sig.offset = -40.0;
    const CommsChannelRow rx = rowFromDbcSignal(sig, QStringLiteral("Coolant"));
    const CommsChannelRow tx = transmitRowFromDbcSignal(sig, QStringLiteral("Coolant"));
    std::printf("  (0.1,-40): receive Offset %g, transmit Offset %g\n", rx.dbcOffset, tx.dbcOffset);
    CHECK(rx.dbcOffset == -40.0);
    CHECK(tx.dbcOffset == 40.0);
    CHECK(tx.dbcFactor == 0.1);
    // The transmit arithmetic as comms_types.h states it, raw = (physical +
    // Offset) / resolution: 25 degrees goes out as the count a DBC receiver
    // turns back into 25.
    CHECK(qRound((25.0 + tx.dbcOffset) / tx.dbcFactor) == 650);
    CHECK(650 * sig.factor + sig.offset == 25.0);
    // A positive offset \u2014 an absolute pressure carried as gauge, say \u2014 goes
    // negative.
    sig.factor = 4.0;
    sig.offset = 101.3;
    CHECK(transmitRowFromDbcSignal(sig, QStringLiteral("x")).dbcOffset == -101.3);
    // Zero stays zero, and not negative zero.
    sig.offset = 0.0;
    CHECK(transmitRowFromDbcSignal(sig, QStringLiteral("x")).dbcOffset == 0.0);
    CHECK(!std::signbit(transmitRowFromDbcSignal(sig, QStringLiteral("x")).dbcOffset));

    // Through the dialog: the row the import makes carries the negated offset.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    Configuration config;
    config.clear();
    config.catalog().addOrUpdateUserChannel(userChannel(QStringLiteral("Coolant Temp Sensor")));
    const QList<CommsSection> sections = importTransmit(config, file);
    REQUIRE(sections.size() == 1);
    REQUIRE(sections.first().rows.size() == 1);
    const CommsChannelRow &row = sections.first().rows.first();
    CHECK(row.channelName == QStringLiteral("Coolant Temp Sensor"));
    CHECK(row.dbcOffset == 40.0);
    CHECK(row.dbcFactor == 0.1);

    // And the receive import of the same file keeps the file's sign.
    Configuration rxConfig;
    rxConfig.clear();
    const QList<CommsSection> received = importAll(rxConfig, file, nullptr);
    REQUIRE(received.size() == 1);
    REQUIRE(received.first().rows.size() == 3);
    CHECK(received.first().rows[1].dbcOffset == -40.0);
}

const char *const kCycleDbc = R"DBC(VERSION "unit-test"

BO_ 1600 Fast: 8 ECU
 SG_ Speed_A : 0|16@1+ (1,0) [0|20000] "rpm" Dash

BO_ 1601 Plain: 8 ECU
 SG_ Speed_B : 0|16@1+ (1,0) [0|20000] "rpm" Dash

BO_ 1602 TooFast: 8 ECU
 SG_ Speed_C : 0|16@1+ (1,0) [0|20000] "rpm" Dash

BO_ 1603 Slow: 8 ECU
 SG_ Speed_D : 0|16@1+ (1,0) [0|20000] "rpm" Dash

BA_DEF_ BO_  "GenMsgCycleTime" INT 0 65535;
BA_DEF_DEF_  "GenMsgCycleTime" 100;
BA_ "GenMsgCycleTime" BO_ 1600 20;
BA_ "GenMsgCycleTime" BO_ 1602 2;
BA_ "GenMsgCycleTime" BO_ 1603 2000;
)DBC";

void testTheCycleTimeBecomesThePeriod()
{
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kCycleDbc), &warnings);
    REQUIRE(file.messages.size() == 4);
    // Parsed: the message's own line, else the file's default.
    std::printf("  cycle times parsed                 : %d %d %d %d ms\n",
                file.messages[0].cycleTimeMs, file.messages[1].cycleTimeMs,
                file.messages[2].cycleTimeMs, file.messages[3].cycleTimeMs);
    CHECK(file.messages[0].cycleTimeMs == 20);
    CHECK(file.messages[1].cycleTimeMs == 100);
    CHECK(file.messages[2].cycleTimeMs == 2);
    CHECK(file.messages[3].cycleTimeMs == 2000);

    // The rule on its own.
    int rate = 50, period = 0;
    transmitTimingFromCycleTime(0, &rate, &period);
    CHECK(rate == 50 && period == 0); // not stated: untouched
    transmitTimingFromCycleTime(20, &rate, &period);
    CHECK(rate == 50 && period == 20);
    transmitTimingFromCycleTime(100, &rate, &period);
    CHECK(rate == 10 && period == 100);
    transmitTimingFromCycleTime(33, &rate, &period);
    CHECK(rate == 30 && period == 33); // the period exact, the rate the nearest hertz
    transmitTimingFromCycleTime(2, &rate, &period);
    CHECK(rate == 200 && period == 5); // the device's floor
    transmitTimingFromCycleTime(2000, &rate, &period);
    CHECK(rate == 1 && period == 2000); // slower than 1 Hz: the period stands

    // Through the dialog.
    Configuration config;
    config.clear();
    for (const char *n : {"Speed A", "Speed B", "Speed C", "Speed D"})
        config.catalog().addOrUpdateUserChannel(userChannel(QString::fromLatin1(n)));
    const QList<CommsSection> sections = importTransmit(config, file);
    REQUIRE(sections.size() == 4);
    for (const CommsSection &s : sections)
        std::printf("  %-8s rate %3d Hz, period %4d ms\n", qPrintable(s.name), s.transmitRateHz,
                    s.transmitPeriodMs);
    CHECK(sections[0].transmitRateHz == 50 && sections[0].transmitPeriodMs == 20);
    CHECK(sections[1].transmitRateHz == 10 && sections[1].transmitPeriodMs == 100);
    CHECK(sections[2].transmitRateHz == 200 && sections[2].transmitPeriodMs == 5);
    CHECK(sections[3].transmitRateHz == 1 && sections[3].transmitPeriodMs == 2000);
    for (const CommsSection &s : sections)
        CHECK(s.cyclic && s.isTransmit());

    // A receive import reads none of it.
    Configuration rxConfig;
    rxConfig.clear();
    const QList<CommsSection> received = importAll(rxConfig, file, nullptr);
    REQUIRE(received.size() == 4);
    for (const CommsSection &s : received) {
        CHECK(s.isReceive());
        CHECK(s.transmitPeriodMs == 0);
    }
}

void testSwitchingModesKeepsTheTicksAndTheChoices()
{
    // The tree is rebuilt on every switch. What the user did to it \u2014 the
    // ticks, and a channel chosen for sending \u2014 has to come back.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    Configuration config;
    config.clear();
    config.catalog().addOrUpdateUserChannel(userChannel(QStringLiteral("Water Temp")));

    ImportDbcDialog dialog(&config, file, QStringLiteral("unit-test.dbc"), 0, {});
    auto *mode = dialog.findChild<QComboBox *>(QStringLiteral("importAs"));
    auto *tree = dialog.findChild<QTreeWidget *>();
    REQUIRE(mode && tree);
    CHECK(!dialog.transmitMode());
    CHECK(tree->headerItem()->text(1) == QStringLiteral("Channel Type"));
    REQUIRE(tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 3);
    tree->topLevelItem(0)->child(0)->setCheckState(0, Qt::Checked);
    tree->topLevelItem(0)->child(2)->setCheckState(0, Qt::Checked);
    CHECK(tree->topLevelItem(0)->checkState(0) == Qt::PartiallyChecked);

    mode->setCurrentIndex(1);
    CHECK(dialog.transmitMode());
    CHECK(tree->headerItem()->text(1) == QStringLiteral("Send Channel"));
    QTreeWidgetItem *msg = tree->topLevelItem(0);
    REQUIRE(msg && msg->childCount() == 3);
    CHECK(msg->child(0)->checkState(0) == Qt::Checked);
    CHECK(msg->child(1)->checkState(0) == Qt::Unchecked);
    CHECK(msg->child(2)->checkState(0) == Qt::Checked);
    CHECK(msg->checkState(0) == Qt::PartiallyChecked);
    // Nothing matches by name, so every cell asks.
    for (int i = 0; i < 3; ++i)
        CHECK(msg->child(i)->text(1) == QStringLiteral("(pick one)"));
    // Nothing is edited inline in transmit mode; the one choice is picked.
    CHECK(!(msg->child(0)->flags() & Qt::ItemIsEditable));
    msg->child(1)->setText(1, QStringLiteral("Water Temp")); // what the picker writes

    // Back to receive: the ticks survive and the receive columns are back.
    mode->setCurrentIndex(0);
    msg = tree->topLevelItem(0);
    REQUIRE(msg && msg->childCount() == 3);
    CHECK(tree->headerItem()->text(1) == QStringLiteral("Channel Type"));
    CHECK(msg->child(0)->checkState(0) == Qt::Checked);
    CHECK(msg->child(1)->checkState(0) == Qt::Unchecked);
    CHECK(msg->child(2)->checkState(0) == Qt::Checked);
    CHECK(msg->child(1)->text(1) == QStringLiteral("Temperature"));
    CHECK(msg->child(0)->flags() & Qt::ItemIsEditable);

    // And to transmit again: the choice made earlier is still there.
    mode->setCurrentIndex(1);
    msg = tree->topLevelItem(0);
    REQUIRE(msg && msg->childCount() == 3);
    std::printf("  after a round trip, row 1 sends    : %s\n", qPrintable(msg->child(1)->text(1)));
    CHECK(msg->child(1)->text(1) == QStringLiteral("Water Temp"));
    CHECK(msg->child(0)->text(1) == QStringLiteral("(pick one)"));
    CHECK(msg->child(0)->checkState(0) == Qt::Checked);
}

void testNothingToSendDisablesImport()
{
    // Every signal ticked and no channel anywhere to send: the count says so
    // and Import stays off \u2014 a section of nothing is not worth a dialog.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kDbc), &warnings);
    Configuration config;
    config.clear();
    bool enabled = true;
    const QList<CommsSection> none =
        importTransmit(config, file, nullptr, -1, QString(), &enabled);
    CHECK(!enabled);
    CHECK(none.isEmpty());

    // One choice made: that row imports, the other two are skipped with a
    // note, and the placeholder never becomes a channel name.
    Configuration one;
    one.clear();
    one.catalog().addOrUpdateUserChannel(userChannel(QStringLiteral("Boost Target")));
    const QList<CommsSection> picked =
        importTransmit(one, file, nullptr, 2, QStringLiteral("Boost Target"), &enabled);
    CHECK(enabled);
    REQUIRE(picked.size() == 1);
    REQUIRE(picked.first().rows.size() == 1);
    const CommsChannelRow &row = picked.first().rows.first();
    CHECK(row.channelName == QStringLiteral("Boost Target"));
    // An IEEE754 signal is a 32-bit float row in either direction.
    CHECK(row.dbcType == int(DbcType::IEEE754));
    CHECK(row.bitLength == 32);
    CHECK(row.channelName != QStringLiteral("(pick one)"));
}

void testAMultiplexedMessageTransmitsAsCompound()
{
    // Same shape as the receive import: each multiplexor value an identifier
    // the device writes into the frame, the multiplexor itself never a row \u2014
    // even when a channel of its name exists to tempt the match.
    QStringList warnings;
    const DbcFile file = parseDbc(QString::fromLatin1(kMuxDbc), &warnings);
    Configuration config;
    config.clear();
    for (const char *n : {"Common Value", "Val A", "Val B", "Selector"})
        config.catalog().addOrUpdateUserChannel(userChannel(QString::fromLatin1(n)));
    const QList<CommsSection> sections = importTransmit(config, file);
    REQUIRE(sections.size() == 1);
    const CommsSection &s = sections.first();
    CHECK(s.isTransmit());
    CHECK(s.compound);
    REQUIRE(s.identifiers.size() == 2);
    CHECK(s.identifiers[0].id == 0u && s.identifiers[1].id == 1u);
    for (const CompoundIdentifier &ident : s.identifiers) {
        REQUIRE(ident.rows.size() == 2);
        CHECK(ident.rows[0].channelName == QStringLiteral("Common Value"));
        for (const CommsChannelRow &r : ident.rows)
            CHECK(r.channelName != QStringLiteral("Selector"));
    }
    CHECK(s.identifiers[0].rows[1].channelName == QStringLiteral("Val A"));
    CHECK(s.identifiers[1].rows[1].channelName == QStringLiteral("Val B"));
    CHECK(s.rows.isEmpty());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testUnderscoresBecomeSpaces();
    testANameWithoutUnderscoresIsUntouched();
    testTheEndsAndTheDoublesAreTidied();
    testANameOfNothingButUnderscoresComesBackEmpty();
    testTheParserKeepsTheUnderscoredName();
    testTheImportDialogShowsTheSpacedName();
    testTheUsersOwnEditStillWins();
    testAKnownUnitBecomesTheCatalogueSpelling();
    testEveryMappedUnitIsOneTheCatalogueOffers();
    testAnUnplaceableUnitIsFlaggedRatherThanGuessed();
    testTheImportPanelOffersTheCatalogueUnit();
    testTheImportedChannelCarriesTheChosenUnit();
    testTheMultiplexorIsNotOfferedAsAChannel();
    testTheImportedCompoundSectionHasNoBlockingClash();
    testTheMessageRowStillReadsFullyChecked();
    testSelectAllLeavesTheMultiplexorAlone();
    testAMessageWithNoMuxedSignalsPickedStaysPlain();
    testAFloatSignalIsLabelledIEEE754InTheDetails();
    testATransmitImportSendsExistingChannels();
    testTheTransmitOffsetChangesSign();
    testTheCycleTimeBecomesThePeriod();
    testSwitchingModesKeepsTheTicksAndTheChoices();
    testNothingToSendDisablesImport();
    testAMultiplexedMessageTransmitsAsCompound();

    if (fails == 0)
        std::printf("test_dbc_names: all checks passed\n");
    else
        std::printf("test_dbc_names: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
