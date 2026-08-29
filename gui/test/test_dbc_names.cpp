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
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTreeWidget>

#include <cstdio>

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
                              const QString &editFirstTo = QString())
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
            sig->setCheckState(0, Qt::Checked);
        }
    }
    // OK through the dialog's own button box: accept() is protected, and
    // clicking OK is the path a user takes anyway. It is enabled only once
    // something is checked, which the loop above has just done.
    auto *box = dialog.findChild<QDialogButtonBox *>();
    if (!box || !box->button(QDialogButtonBox::Ok))
        return {};
    box->button(QDialogButtonBox::Ok)->click();
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

} // namespace

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

    if (fails == 0)
        std::printf("test_dbc_names: all checks passed\n");
    else
        std::printf("test_dbc_names: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
