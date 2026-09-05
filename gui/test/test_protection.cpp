// Console self-test: the per-message protection TIER, as a model.
//
// 2.3.0 replaced two independent booleans ("Protect Communication", v19, and
// "Read-only", v20) with one ordered level. Everything here is host-side and
// deliberately so: the device enforces nothing about message protection any
// more, which is exactly why the host's rules are the only ones left to hold
// down. What test_firmware_link pins is the device's silence; what this pins is
// the meaning the application attaches to the tier and every route by which
// that meaning could quietly change.
//
// Five properties get most of the attention, because each of them was either a
// live bug or a one-line mistake away from being one:
//
//   1. The schema-13 migration, whose two traps point in OPPOSITE directions.
//      The legacy key literally spelled "hidden" is the ancestor of Protected,
//      not of the new Hidden; and v20's "readOnlyComms" CONCEALED, so it must
//      become Hidden rather than the new, visible, Read Only.
//   2. Not re-running that migration on a file that already carries a tier,
//      which would ratchet every Read Only section toward Hidden on each load.
//   3. applyBusSections, the single chokepoint that refuses an untick. A guard
//      on the checkbox would leave a dozen other writers into the document.
//   4. clearContent, so a Get replaces what the DEVICE knows without wiping
//      what the DOCUMENT knows. Before the split, one Get left every tier
//      lowerable with no challenge at all.
//   5. isChannelConcealed vs isChannelEditLocked, which used to be one
//      predicate and are now two because Read Only is visible AND locked.
//   6. proofsRequiredFor, the per-tier proof policy the two dialogs read instead
//      of switching on the tier themselves — including Protected's 2.3.1
//      strengthening to section password AND device round trip, and the keyless
//      upgrade path that keeps every Protected message written before it
//      openable by its own author.
//   7. revokeSectionAccess and the ORDER it must run in relative to
//      applyBusSections. Reversing those two refuses a legitimate edit.
//
// The last three are enforced in the DIALOGS and nowhere else, so the second
// half of this file drives the real widgets rather than the model:
//
//   8. Rule 1 — no marked tier leaves the section editor without a password of
//      its own — which lives entirely in SectionEditorDialog::accept(), and the
//      SEAM with the keyless rule that lets the two coexist.
//   9. Rule 2 — one path for every tier change, RAISING included — and rule 3's
//      re-conceal on editor close, driven through Communications Setup so the
//      ordering against applyBusSections is exercised rather than described.
//
// Exits 0 on success, 1 on first failure.
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>
#include <functional>

#include "../src/model/comms_types.h"
#include "../src/model/config_file.h"
#include "../src/model/configuration.h"
#include "../src/model/device_mapper.h"
#include "../src/protocol/access_state.h"
#include "../src/protocol/wire_structs.h"
#include "../src/ui/channel_editor_dialog.h"
#include "../src/ui/edit_channel_dialog.h"
#include "../src/ui/communications_dialog.h"
#include "../src/ui/section_editor_dialog.h"

static int failures = 0;

// The schema this build writes. kConfigSchemaVersion is file-static in
// configuration.cpp, so every direct CommsSection::fromJson call that is
// re-reading something this build just WROTE has to be handed this number:
// passing a stale one re-runs the pre-14 migration and ratchets a Read Only
// section into Hidden, which is property 2 above failing silently.
static constexpr int kCurrentSchemaVersion = 21;
// The last schema that spelled protection with the legacy boolean keys.
static constexpr int kLegacySchemaVersion = 13;

// Flushed on every failure, which matters more here than in the model-only
// tests: the second half of this file drives real event loops, so a wrong answer
// can present as a HANG rather than as an exit code — and a hang killed by a
// build timeout takes every buffered line with it, leaving no clue which check
// was the last to pass.
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                          \
            std::fflush(stdout);                                                                 \
            ++failures;                                                                          \
        }                                                                                        \
    } while (0)

using namespace ct;

// The body of a format-2 .ct3, for the checks that assert on what the writer
// PUT IN THE FILE rather than on what a reload produces. Reading the file as
// JSON is what these used to do and it no longer works: the body is sealed, and
// going through the container is the only honest way to look at it. A round
// trip through Configuration would prove something weaker — it cannot tell a
// key that was written from one the loader defaulted.
static QJsonObject configBodyOf(const QString &path)
{
    QByteArray plain;
    if (!ct::readBinaryConfigFile(path, &plain, nullptr, nullptr))
        return QJsonObject();
    return QJsonDocument::fromJson(plain).object();
}

// The document's own section, by name, on one bus. A grant now names a SECTION —
// bus, name and the messageKey it was proved against — rather than a bare string,
// so every test that used to hand over a name hands over the section the document
// actually holds. Looking it up here rather than at each call site is what stops a
// test proving something about a section the model was never asked about.
//
// A name that is not on that bus is a fixture mistake, and it is reported as a
// named failure rather than answered with a default: a silent "not found" would
// turn a grant that never happened into a test that quietly stopped testing.
static const CommsSection *sectionNamed(const Configuration &cfg, int bus, const QString &name)
{
    for (const CommsSection &s : cfg.bus[bus].sections)
        if (s.name.compare(name, Qt::CaseInsensitive) == 0)
            return &s;
    std::printf("FAIL %s: no section \"%s\" on bus %d\n", __FILE__, qPrintable(name), bus);
    std::fflush(stdout);
    ++failures;
    return nullptr;
}

static void grantByName(Configuration &cfg, int bus, const QString &name)
{
    if (const CommsSection *s = sectionNamed(cfg, bus, name))
        cfg.grantSectionAccess(bus, *s);
}

static bool grantedByName(const Configuration &cfg, int bus, const QString &name)
{
    const CommsSection *s = sectionNamed(cfg, bus, name);
    return s && cfg.sectionAccessGranted(bus, *s);
}

// A plain receive section carrying one channel row. Everything below needs one
// and none of them care about the protocol details.
// v16: A MARKED MESSAGE NAMES ONE OF THE DOCUMENT'S FOUR PASSWORDS, so a fixture
// has to put the password in a SLOT as well as on the section. Setting
// messageKey alone now builds an invalid document - the wire carries a slot
// number, a key belonging to no slot has nothing to travel as, and validation
// says so.
//
// Returns the derived key, so a test reads `const AccessKey k = registerPassword(
// cfg, "x")` and uses k exactly where it used deriveAccessKey("x") before.
// Idempotent: asking twice for the same password gives the same slot back.
static AccessKey registerPassword(Configuration &cfg, const char *password)
{
    const AccessKey k = deriveAccessKey(QString::fromUtf8(password));
    if (cfg.commsPasswordSlotFor(k) == 0) {
        const int slot = cfg.firstFreeCommsPasswordSlot();
        CHECK(slot != 0); // a fixture wanting a fifth password is a fixture bug
        if (slot != 0)
            cfg.setCommsPasswordSlot(slot, k);
    }
    return k;
}

static CommsSection makeSection(const QString &name, quint32 id, CommsProtection protection,
                                const QString &channel)
{
    CommsSection s;
    s.name = name;
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = id;
    s.messageLengthBytes = 8;
    s.protection = protection;
    if (!channel.isEmpty()) {
        CommsChannelRow r;
        r.channelName = channel;
        r.startBit = 0;
        r.bitLength = 16;
        s.rows.append(r);
    }
    return s;
}

// ---------------------------------------------------------------- the level

static void testTierModel()
{
    // ORDERED, and every consumer relies on it: "conceal" is >= Hidden and
    // "edit-locked" is != None. If the enum were ever renumbered so that these
    // comparisons stopped holding, a Hidden message would start displaying.
    CHECK(CommsProtection::None < CommsProtection::ReadOnly);
    CHECK(CommsProtection::ReadOnly < CommsProtection::Hidden);
    CHECK(CommsProtection::Hidden < CommsProtection::Protected);

    // The numeric value IS the wire level. configuration.cpp static_asserts this
    // against the MSGPROT_* block so the build stops rather than a Send quietly
    // downgrading; asserted here too because a static_assert proves the two
    // headers agree, and this proves the two CONVERSIONS do.
    CHECK(commsProtectionToWire(CommsProtection::None) == MSGPROT_NONE);
    CHECK(commsProtectionToWire(CommsProtection::ReadOnly) == MSGPROT_READONLY);
    CHECK(commsProtectionToWire(CommsProtection::Hidden) == MSGPROT_HIDDEN);
    CHECK(commsProtectionToWire(CommsProtection::Protected) == MSGPROT_PROTECTED);
    for (CommsProtection p : {CommsProtection::None, CommsProtection::ReadOnly,
                              CommsProtection::Hidden, CommsProtection::Protected})
        CHECK(commsProtectionFromWire(commsProtectionToWire(p)) == p);
    // The legacy patterns, decoded. These two are the whole reason the encoding
    // is this way round: 0x80 is what 2.2.x wrote for its concealing "Read-only"
    // and must NOT land on the new visible tier.
    CHECK(commsProtectionFromWire(MSGFLAG_ACTIVE | 0x80) == CommsProtection::Hidden);
    CHECK(commsProtectionFromWire(MSGFLAG_ACTIVE | 0xC0) == CommsProtection::Protected);
    // The engine-evaluated bits never reach the level.
    CHECK(commsProtectionFromWire(MSGFLAG_ACTIVE | MSGFLAG_TRANSMIT) == CommsProtection::None);

    // Tokens. None has NO spelling — it is written by omitting the key — and
    // anything unrecognised clamps to the strongest tier, because a plain .ct3
    // is unauthenticated JSON and a hand-edited value is routine rather than
    // theoretical.
    CHECK(commsProtectionToken(CommsProtection::None).isEmpty());
    CHECK(commsProtectionToken(CommsProtection::ReadOnly) == QLatin1String("readOnly"));
    CHECK(commsProtectionToken(CommsProtection::Hidden) == QLatin1String("hidden"));
    CHECK(commsProtectionToken(CommsProtection::Protected) == QLatin1String("protected"));
    for (CommsProtection p :
         {CommsProtection::ReadOnly, CommsProtection::Hidden, CommsProtection::Protected})
        CHECK(commsProtectionFromToken(commsProtectionToken(p)) == p);
    CHECK(commsProtectionFromToken(QStringLiteral("banana")) == CommsProtection::Protected);
    CHECK(commsProtectionFromToken(QString()) == CommsProtection::Protected);
    CHECK(commsProtectionFromToken(QStringLiteral("READONLY")) == CommsProtection::Protected);

    // ---- the two per-section predicates, and where they disagree ----
    // Read Only is the ONLY tier where they differ, and that difference is the
    // whole point of the tier: visible, and still not editable.
    const CommsSection none = makeSection(QStringLiteral("A"), 0x100, CommsProtection::None, {});
    const CommsSection ro = makeSection(QStringLiteral("B"), 0x200, CommsProtection::ReadOnly, {});
    const CommsSection hid = makeSection(QStringLiteral("C"), 0x300, CommsProtection::Hidden, {});
    const CommsSection prot =
        makeSection(QStringLiteral("D"), 0x400, CommsProtection::Protected, {});

    for (bool revealed : {false, true}) {
        CHECK(!none.isConcealed(revealed));
        CHECK(!ro.isConcealed(revealed)); // never, at either setting
    }
    CHECK(hid.isConcealed(false));
    CHECK(prot.isConcealed(false));
    CHECK(!hid.isConcealed(true));
    CHECK(!prot.isConcealed(true));

    // isEditLocked takes no `revealed` argument AT ALL, and that is deliberate
    // rather than an omission: the password buys viewing and the right to untick
    // the box, and unticking is what unlocks editing. A signature that could
    // express "revealed, therefore editable" would be the bug.
    CHECK(!none.isEditLocked());
    CHECK(ro.isEditLocked());
    CHECK(hid.isEditLocked());
    CHECK(prot.isEditLocked());

    // displayDetail: Read Only says everything, the other two say only which of
    // them they are. Concealed wording must tell Hidden and Protected apart —
    // they take different passwords to open, so a single word would send the
    // user to the wrong dialog.
    CHECK(ro.displayDetail(false).contains(QLatin1String("200")));
    CHECK(!hid.displayDetail(false).contains(QLatin1String("300")));
    CHECK(!prot.displayDetail(false).contains(QLatin1String("400")));
    CHECK(hid.displayDetail(false) != prot.displayDetail(false));
    CHECK(prot.displayDetail(true).contains(QLatin1String("400")));
}

// ------------------------------------------------------------------ the file

static void testJsonRoundTrip()
{
    // Every tier survives a schema-14 write/read as ITSELF.
    for (CommsProtection p : {CommsProtection::None, CommsProtection::ReadOnly,
                              CommsProtection::Hidden, CommsProtection::Protected}) {
        CommsSection s = makeSection(QStringLiteral("Feed"), 0x2A0, p, QStringLiteral("Value"));
        s.messageKey = AccessKey(0x1234ABCDu);
        const QJsonObject o = s.toJson();
        // None is spelled by OMITTING the key, so an ordinary message carries no
        // trace of the feature. Writing "protection": "none" would give a
        // hand-editor a value to change and a v13 build a reason to think it
        // understood the file.
        CHECK(o.contains(QStringLiteral("protection")) == (p != CommsProtection::None));
        // The legacy keys are read-only from here on. Writing either alongside
        // "protection" would leave two disagreeing sources of truth in one file.
        CHECK(!o.contains(QStringLiteral("protectedComms")));
        CHECK(!o.contains(QStringLiteral("readOnlyComms")));
        CHECK(!o.contains(QStringLiteral("hidden")));

        const CommsSection back = CommsSection::fromJson(o, kCurrentSchemaVersion);
        CHECK(back.protection == p);
        // The section's OWN password is a document secret and survives with it.
        // It did not retire with the device's per-message key — DECISIONS D2
        // keeps it, and it is what unlocks Read Only and Hidden — so dropping it
        // from the file would destroy every section password on the next save.
        CHECK(back.messageKey == s.messageKey);
    }

    // A hand-edited or truncated token clamps to Protected. Over-restricting
    // annoys; the other direction hands out a protocol.
    {
        QJsonObject o = makeSection(QStringLiteral("F"), 0x100, CommsProtection::ReadOnly, {})
                            .toJson();
        o[QStringLiteral("protection")] = QStringLiteral("read-only"); // plausible, and wrong
        CHECK(CommsSection::fromJson(o, kCurrentSchemaVersion).protection
              == CommsProtection::Protected);
        o[QStringLiteral("protection")] = QJsonValue(); // null, i.e. present but empty
        CHECK(CommsSection::fromJson(o, kCurrentSchemaVersion).protection
              == CommsProtection::Protected);
    }

    // An ABSENT key at v14 honestly means None, and never reaches the clamp.
    {
        QJsonObject o = makeSection(QStringLiteral("F"), 0x100, CommsProtection::None, {}).toJson();
        CHECK(!o.contains(QStringLiteral("protection")));
        CHECK(CommsSection::fromJson(o, kCurrentSchemaVersion).protection
              == CommsProtection::None);
    }

    // A whole document, through a real .ct3, with the file version it claims.
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        QString err;
        Configuration cfg;
        cfg.bus[0].enabled = true;
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Cal"), 0x111, CommsProtection::ReadOnly, {}));
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Hid"), 0x222, CommsProtection::Hidden, {}));
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Prot"), 0x333, CommsProtection::Protected, {}));
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Open"), 0x444, CommsProtection::None, {}));
        const QString path = dir.filePath(QStringLiteral("tiers.ct3"));
        CHECK(cfg.saveToFile(path, &err));

        const QJsonObject root = configBodyOf(path);
        // The bump is not cosmetic. A shipped v13 build hard-refuses a v14 file;
        // without the bump it would open one, find no key it recognised, load a
        // Hidden message as ordinary and print its CAN ID and every bit position
        // in four different views.
        CHECK(root[QStringLiteral("fileVersion")].toInt() == kCurrentSchemaVersion);

        Configuration back;
        CHECK(back.loadFromFile(path, &err));
        CHECK(back.bus[0].sections.size() == 4);
        if (back.bus[0].sections.size() == 4) {
            CHECK(back.bus[0].sections[0].protection == CommsProtection::ReadOnly);
            CHECK(back.bus[0].sections[1].protection == CommsProtection::Hidden);
            CHECK(back.bus[0].sections[2].protection == CommsProtection::Protected);
            CHECK(back.bus[0].sections[3].protection == CommsProtection::None);
        }
    }
}

static void testMigrationFromSchema13()
{
    // The five §5.1 rules, in order, each as its own fixture. Read the two
    // marked TRAP cases before changing anything here: they point in opposite
    // directions and each looks like a bug when read on its own.
    const auto legacy = [](const QString &key, bool value) {
        QJsonObject o;
        o[QStringLiteral("name")] = QStringLiteral("Legacy");
        o[QStringLiteral("device")] = QStringLiteral("receive");
        o[QStringLiteral("baseAddress")] = QStringLiteral("6A1");
        o[QStringLiteral("messageLength")] = 8;
        if (!key.isEmpty())
            o[key] = value;
        return o;
    };

    // 1. v19's "protectedComms" is an exact match for the new Protected.
    CHECK(CommsSection::fromJson(legacy(QStringLiteral("protectedComms"), true),
                                 kLegacySchemaVersion)
              .protection
          == CommsProtection::Protected);

    // 2. TRAP. The v7 key is literally spelled "hidden" and the new ladder has a
    //    Hidden in it. They are not the same thing: this key is the direct
    //    ancestor of protectedComms, so it maps to PROTECTED. Re-pointing it at
    //    CommsProtection::Hidden because the names match looks like a tidy-up in
    //    review and silently downgrades every schema-7 file on disk.
    CHECK(CommsSection::fromJson(legacy(QStringLiteral("hidden"), true), kLegacySchemaVersion)
              .protection
          == CommsProtection::Protected);

    // 3. TRAP, pointing the other way. v20's "Read-only" CONCEALED — v21's
    //    isConcealed() ORed both old flags — so a message marked this way in an
    //    existing file is one its author has never seen displayed. The new Read
    //    Only permits viewing, so mapping it there would print the CAN ID, frame
    //    layout, timing and every channel's bit position of every read-only
    //    message in every existing file, on first open, with nothing on screen
    //    to say it had happened. Over-restricting annoys; under-restricting
    //    leaks.
    CHECK(CommsSection::fromJson(legacy(QStringLiteral("readOnlyComms"), true),
                                 kLegacySchemaVersion)
              .protection
          == CommsProtection::Hidden);
    CHECK(CommsSection::fromJson(legacy(QStringLiteral("readOnlyComms"), true),
                                 kLegacySchemaVersion)
              .protection
          != CommsProtection::ReadOnly);

    // 4. Both set — the shape v21's `readOnlyComms = readOnly || protect` ladder
    //    actually produced — collapses to Protected with no residue.
    {
        QJsonObject o = legacy(QStringLiteral("protectedComms"), true);
        o[QStringLiteral("readOnlyComms")] = true;
        CHECK(CommsSection::fromJson(o, kLegacySchemaVersion).protection
              == CommsProtection::Protected);
    }
    // ...and protectedComms:false does NOT fall through to the v7 "hidden"
    // branch when the key is present and simply says no.
    {
        QJsonObject o = legacy(QStringLiteral("protectedComms"), false);
        o[QStringLiteral("hidden")] = true; // both present: the newer key wins
        CHECK(CommsSection::fromJson(o, kLegacySchemaVersion).protection
              == CommsProtection::None);
    }

    // 5. No protection keys at all means None. Nothing is invented for a file
    //    whose author never set anything.
    CHECK(CommsSection::fromJson(legacy(QString(), false), kLegacySchemaVersion).protection
          == CommsProtection::None);

    // The section's own password is parsed rather than discarded: it is still
    // live, and it is what unlocks Read Only and Hidden.
    {
        QJsonObject o = legacy(QStringLiteral("readOnlyComms"), true);
        o[QStringLiteral("messageKey")] = QString::number(0xDEADBEEFu);
        const CommsSection s = CommsSection::fromJson(o, kLegacySchemaVersion);
        CHECK(s.protection == CommsProtection::Hidden);
        CHECK(s.messageKey == AccessKey(0xDEADBEEFu));
    }

    // The same rules through a real file, so the fileVersion actually reaches
    // CommsSection::fromJson rather than being defaulted somewhere in between.
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        QString err;
        QJsonObject busObj;
        busObj[QStringLiteral("enabled")] = true;
        busObj[QStringLiteral("sections")] =
            QJsonArray{legacy(QStringLiteral("readOnlyComms"), true),
                       legacy(QStringLiteral("protectedComms"), true)};
        QJsonObject root;
        root[QStringLiteral("fileType")] = QStringLiteral("CANTripleConfig");
        root[QStringLiteral("fileVersion")] = kLegacySchemaVersion;
        root[QStringLiteral("buses")] = QJsonArray{busObj};
        const QString path = dir.filePath(QStringLiteral("v13.ct3"));
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(root).toJson());
        f.close();

        Configuration cfg;
        CHECK(cfg.loadFromFile(path, &err));
        CHECK(cfg.bus[0].sections.size() == 2);
        if (cfg.bus[0].sections.size() == 2) {
            CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Hidden);
            CHECK(cfg.bus[0].sections[1].protection == CommsProtection::Protected);
        }
    }
}

static void testNoReRatchetOnSchema14()
{
    // The migration must run ONLY when the file actually predates 14. Applied
    // unconditionally it would re-read a v14 file's absent legacy keys next to
    // its own "protection" and walk every Read Only section up to Hidden — once
    // per load, silently, and irreversibly after the next save.

    // At the section level: a v14 object carrying BOTH the new key and a stale
    // legacy one must honour the new key. (A file like this is what a
    // third-party tool or a partial hand-edit produces.)
    {
        QJsonObject o = makeSection(QStringLiteral("Cal"), 0x100, CommsProtection::ReadOnly, {})
                            .toJson();
        o[QStringLiteral("readOnlyComms")] = true; // the v20 spelling, left behind
        CHECK(CommsSection::fromJson(o, kCurrentSchemaVersion).protection
              == CommsProtection::ReadOnly);
    }

    // And through the file, twice round, because one cycle would not catch a
    // ratchet that needs a save to become permanent.
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString err;
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Cal"), 0x321, CommsProtection::ReadOnly, {}));

    QString path = dir.filePath(QStringLiteral("ratchet0.ct3"));
    CHECK(cfg.saveToFile(path, &err));
    for (int cycle = 0; cycle < 3; ++cycle) {
        // Each round loads the previous round's output and writes the next
        // round's input, so a one-step drift compounds instead of cancelling.
        Configuration back;
        CHECK(back.loadFromFile(path, &err));
        CHECK(back.bus[0].sections.size() == 1);
        if (back.bus[0].sections.size() != 1)
            return;
        // Not "<= ReadOnly" and not "!= None": the exact tier, every cycle. A
        // ratchet moves it one step, and only an exact comparison notices.
        CHECK(back.bus[0].sections[0].protection == CommsProtection::ReadOnly);
        path = dir.filePath(QStringLiteral("ratchet%1.ct3").arg(cycle + 1));
        CHECK(back.saveToFile(path, &err));
    }
}

// ------------------------------------------------------------ the untick rule

// THE PREDICATE THE SEND GATE HINGES ON. A configuration carrying Protect
// Communication markings goes only to a unit that proves the same Protected
// Comms key, so getting this wrong either blocks ordinary sends or lets a
// protected configuration onto a device that never held its password.
static void testHasProtectedComms()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CHECK(!cfg.hasProtectedComms()); // nothing configured at all

    // The lesser tiers are conventions of this application and the device
    // enforces nothing about them, so neither gates a send.
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Open"), 0x400, CommsProtection::None, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Cal"), 0x300, CommsProtection::ReadOnly, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Hid"), 0x200, CommsProtection::Hidden, {}));
    CHECK(!cfg.hasProtectedComms());

    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Prot"), 0x100, CommsProtection::Protected, {}));
    CHECK(cfg.hasProtectedComms());

    // SWITCHED OFF IS NOT SENT. mapToDevice skips an Off section outright, so a
    // marking on a message that is not travelling must not refuse the send --
    // otherwise a message someone disabled months ago quietly makes every send
    // demand a password for content the unit never receives.
    cfg.bus[0].sections.last().device = SectionDevice::Off;
    CHECK(!cfg.hasProtectedComms());
    cfg.bus[0].sections.last().device = SectionDevice::ReceiveMessage;
    CHECK(cfg.hasProtectedComms());

    // Any bus, not just the first.
    Configuration third;
    third.bus[2].enabled = true;
    third.bus[2].sections.append(
        makeSection(QStringLiteral("Prot3"), 0x101, CommsProtection::Protected, {}));
    CHECK(third.hasProtectedComms());
}

static void testApplyBusSectionsGuard()
{
    const QString pass = QStringLiteral("edit-protected-comms");

    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Prot"), 0x100, CommsProtection::Protected, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Hid"), 0x200, CommsProtection::Hidden, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Cal"), 0x300, CommsProtection::ReadOnly, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Open"), 0x400, CommsProtection::None, {}));
    // Read Only and Hidden are guarded by the SECTION's own password and by
    // nothing else, so they have to HAVE one for there to be anything to
    // enforce. A section with no messageKey is deliberately open at those two
    // tiers (see Configuration::maySectionLower), which is exactly the state a
    // Get used to force every section into.
    cfg.bus[0].sections[1].messageKey = deriveAccessKey(QStringLiteral("hid-own"));
    cfg.bus[0].sections[2].messageKey = deriveAccessKey(QStringLiteral("cal-own"));
    CHECK(cfg.setCommsPassword(pass));
    cfg.concealProtectedComms();
    CHECK(!cfg.commsRevealed());
    // Per section, and the answers differ by tier. There is no document-wide
    // mayLowerProtection() any more: it was commsRevealed(), true for every
    // document with no Protected Comms verifier, so on the documents that
    // use per-section passwords this chokepoint checked nothing at all.
    for (const CommsSection &s : cfg.bus[0].sections)
        CHECK(cfg.maySectionLower(s) == (s.protection == CommsProtection::None));

    // A lowering, refused, with the document left EXACTLY as it was. "Refused"
    // has to mean the whole call did nothing: a partial apply would drop the
    // other three sections' edits on the floor while the caller was still
    // showing a dialog.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::Hidden; // Protected -> Hidden
        next[3].baseAddress = 0x4FF;                  // an innocent edit riding along
        QString refusal;
        CHECK(!cfg.applyBusSections(0, next, &refusal));
        CHECK(refusal.contains(QLatin1String("Prot")));
        CHECK(!refusal.isEmpty());
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Protected);
        CHECK(cfg.bus[0].sections[3].baseAddress == 0x400);
    }

    // Every downward step is refused, at every tier — including the small ones.
    for (const auto &step : {std::make_pair(1, CommsProtection::ReadOnly), // Hidden   -> ReadOnly
                             std::make_pair(1, CommsProtection::None),     // Hidden   -> None
                             std::make_pair(2, CommsProtection::None)}) {  // ReadOnly -> None
        QList<CommsSection> next = cfg.bus[0].sections;
        next[step.first].protection = step.second;
        CHECK(!cfg.applyBusSections(0, next, nullptr));
    }

    // RAISING is always free. Nobody needs a password to protect something more.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[3].protection = CommsProtection::Protected; // None -> Protected
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[3].protection == CommsProtection::Protected);
        // ...and put it back for the rest of this function.
        next[3].protection = CommsProtection::None;
        CHECK(!cfg.applyBusSections(0, next, nullptr)); // which is now a LOWERING
        CHECK(cfg.bus[0].sections[3].protection == CommsProtection::Protected);
    }

    // REMOVAL is permitted at every tier. This is the rule DECISIONS.md repeats,
    // and the one the old Lua binding contradicted.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next.removeAt(0); // the Protected one
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections.size() == 3);
    }

    // A NEW name is an addition, not an untick, whatever tier it arrives at.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next.append(makeSection(QStringLiteral("Fresh"), 0x500, CommsProtection::None, {}));
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections.size() == 4);
    }

    // Matching is CASE-INSENSITIVE, like every other name comparison in the
    // document. Otherwise "hid" would be a new section and the untick would be
    // three keystrokes.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        for (CommsSection &s : next)
            if (s.name == QLatin1String("Hid")) {
                s.name = QStringLiteral("hid");
                s.protection = CommsProtection::None;
            }
        CHECK(!cfg.applyBusSections(0, next, nullptr));
    }

    // The per-section grant. Read Only and Hidden are unlocked by the SECTION's
    // own password and Protected by an Protected Comms proof against a live
    // device; whichever ran, the section editor records it here. The document
    // stays concealed throughout — that is the point, and it is what makes the
    // grant a grant rather than a second way of spelling "revealed".
    {
        grantByName(cfg, 0, QStringLiteral("Hid"));
        CHECK(grantedByName(cfg, 0, QStringLiteral("Hid")));
        CHECK(!cfg.commsRevealed()); // still concealed document-wide
        QList<CommsSection> next = cfg.bus[0].sections;
        for (CommsSection &s : next)
            if (s.name == QLatin1String("Hid"))
                s.protection = CommsProtection::None;
        CHECK(cfg.applyBusSections(0, next, nullptr));
        // ...and it grants exactly one section, not the document. "Cal" was
        // never challenged, so it is still guarded.
        QList<CommsSection> other = cfg.bus[0].sections;
        for (CommsSection &s : other)
            if (s.name == QLatin1String("Cal"))
                s.protection = CommsProtection::None;
        CHECK(!cfg.applyBusSections(0, other, nullptr));
    }

    // Concealing again drops every grant. A password proved for this session
    // must not outlive the session's reveal state, or "Conceal" would be a
    // display change pretending to be a security one.
    cfg.concealProtectedComms();
    CHECK(!grantedByName(cfg, 0, QStringLiteral("Hid")));

    // Revealing the document buys NOTHING for any section, at any tier. Hole D
    // was the Protected Comms password standing in for a section's own,
    // which the spec does not authorise — and the last remnant of it, the arm
    // that let commsRevealed() open a KEYLESS Protected section, went in 2.3.2.
    // That arm published every section a Get produces, because the wire carries
    // no key and commsRevealed() is true for any document with no password.
    CHECK(cfg.revealProtectedComms(pass));
    CHECK(cfg.commsRevealed());
    {
        // "Open" was raised to Protected earlier and is the Protected one now.
        // It is also KEYLESS, so nothing lowers it — not the document password,
        // not anything — and the refusal says that in those words rather than
        // sending the reader off for a password nobody holds.
        QList<CommsSection> next = cfg.bus[0].sections;
        for (CommsSection &s : next)
            if (s.name == QLatin1String("Open"))
                s.protection = CommsProtection::None;
        QString keyless;
        CHECK(!cfg.applyBusSections(0, next, &keyless));
        CHECK(keyless.contains(QLatin1String("without a Message Password")));
        for (const CommsSection &s : cfg.bus[0].sections)
            if (s.name == QLatin1String("Open"))
                CHECK(s.protection == CommsProtection::Protected);

        // THE REPAIR, and it is the one both refusals name: give it a first
        // password — free at every tier, or a section a Get produced could never
        // be fixed — prove that, and then untick.
        QList<CommsSection> keyed = cfg.bus[0].sections;
        for (CommsSection &s : keyed)
            if (s.name == QLatin1String("Open"))
                s.messageKey = registerPassword(cfg, "open-own");
        CHECK(cfg.applyBusSections(0, keyed, nullptr));
        grantByName(cfg, 0, QStringLiteral("Open"));
        QList<CommsSection> lowered = cfg.bus[0].sections;
        for (CommsSection &s : lowered)
            if (s.name == QLatin1String("Open"))
                s.protection = CommsProtection::None;
        CHECK(cfg.applyBusSections(0, lowered, nullptr));
        for (const CommsSection &s : cfg.bus[0].sections)
            if (s.name == QLatin1String("Open"))
                CHECK(s.protection == CommsProtection::None);

        // ...and "Cal" is STILL refused, holding its own password, with the
        // document's master password given. That is the whole of hole D.
        QList<CommsSection> cal = cfg.bus[0].sections;
        for (CommsSection &s : cal)
            if (s.name == QLatin1String("Cal"))
                s.protection = CommsProtection::None;
        QString refusal;
        CHECK(!cfg.applyBusSections(0, cal, &refusal));
        CHECK(refusal.contains(QLatin1String("Message Password")));

        // The section's own password, proved, is what opens it.
        grantByName(cfg, 0, QStringLiteral("Cal"));
        CHECK(cfg.applyBusSections(0, cal, nullptr));
    }

    // A bad bus index is refused rather than clamped, and says so.
    {
        QString refusal;
        CHECK(!cfg.applyBusSections(-1, cfg.bus[0].sections, &refusal));
        CHECK(!refusal.isEmpty());
        CHECK(!cfg.applyBusSections(3, cfg.bus[0].sections, nullptr));
    }
}

// CHANGING a section's password is exactly as privileged as LOWERING its tier,
// and this is the model half of that — the half that has to hold with no help
// from any dialog.
//
// The bypass it closes needed no password at all. Read Only never conceals, so
// its editor opens with no challenge; typing anything into Message Password
// replaced the key, and applyBusSections compared tiers and NOTHING ELSE, so the
// swap went through. The tier never moved, so rule 2's guard never fired — and
// the thing that guard protects was gone. The second trip unticks Read Only with
// the new owner's password. This is NOT the documented remove-and-retype escape:
// the message survives intact, with somebody else holding it.
static void testKeySwapIsAsPrivilegedAsAnUntick()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Cal"), 0x100, CommsProtection::ReadOnly, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Hid"), 0x200, CommsProtection::Hidden, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Open"), 0x300, CommsProtection::None, {}));
    cfg.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("cal-own"));
    cfg.bus[0].sections[1].messageKey = deriveAccessKey(QStringLiteral("hid-own"));
    // An UNMARKED section carrying a key. Legal, and deliberately so: the editor
    // does not destroy a password when the boxes clear.
    cfg.bus[0].sections[2].messageKey = deriveAccessKey(QStringLiteral("stale-own"));
    const AccessKey attacker = deriveAccessKey(QStringLiteral("attacker1234"));

    // THE BYPASS, as the probe drove it: the tier untouched, the key replaced.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].messageKey = attacker;
        QString refusal;
        CHECK(!cfg.applyBusSections(0, next, &refusal));
        CHECK(refusal.contains(QLatin1String("Cal")));
        CHECK(refusal.contains(QLatin1String("Message Password")));
        // Nothing was written.
        CHECK(cfg.bus[0].sections[0].messageKey == deriveAccessKey(QStringLiteral("cal-own")));
    }
    // Hidden too. Read Only is the one with no challenge on the way in, but the
    // rule is about the act, not about which tier makes it convenient.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[1].messageKey = attacker;
        CHECK(!cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[1].messageKey == deriveAccessKey(QStringLiteral("hid-own")));
    }
    // The section's own password, proved, is what permits it — the same grant
    // that permits the untick, because it is the same privilege.
    {
        grantByName(cfg, 0, QStringLiteral("Cal"));
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].messageKey = attacker;
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[0].messageKey == attacker);
        // ...and the grant died with the key it was proved against, so the NEXT
        // swap is refused again. That is the belt under this whole check: one
        // grant cannot be spent twice on two different secrets.
        QList<CommsSection> again = cfg.bus[0].sections;
        again[0].messageKey = deriveAccessKey(QStringLiteral("attacker-second"));
        CHECK(!cfg.applyBusSections(0, again, nullptr));
    }
    // An UNMARKED section's key guards nothing, so replacing it is free. Refusing
    // here would turn a stale password — which the editor deliberately keeps —
    // into a lock on a section nobody marked.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[2].messageKey = attacker;
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[2].messageKey == attacker);
    }
    // SETTING A FIRST password is free at every tier, and it has to be: every
    // section a Get produces arrives keyless, and giving one a password is the
    // action that FIXES that. A rule that refused it would refuse the repair.
    {
        Configuration fresh;
        fresh.bus[0].enabled = true;
        fresh.bus[0].sections.append(
            makeSection(QStringLiteral("Imported"), 0x400, CommsProtection::Hidden, {}));
        CHECK(fresh.bus[0].sections[0].messageKey == kNoAccessKey);
        QList<CommsSection> next = fresh.bus[0].sections;
        next[0].messageKey = deriveAccessKey(QStringLiteral("now-it-has-one"));
        QString refusal;
        CHECK(fresh.applyBusSections(0, next, &refusal));
        CHECK(refusal.isEmpty());
        CHECK(fresh.bus[0].sections[0].messageKey != kNoAccessKey);
    }
    // REMOVAL is still free at every tier, key or no key. The point of the rule
    // is that a message must not change owner behind its owner's back; deleting
    // it is not that, and the spec permits it three times over.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next.removeAt(1); // the Hidden one, never proved
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections.size() == 2);
    }
}

// Two sections may share a name, and the chokepoint's `before` lookup used to
// keep ONE entry per name — so with a guarded "Engine Data" and an open one, the
// open one answered for the guarded one and the untick went through. Solved the
// way device_mapper.cpp already solves it: per-key lists, consumed in document
// order.
static void testDuplicateNamesCannotAnswerForEachOther()
{
    const AccessKey first = deriveAccessKey(QStringLiteral("first-own"));
    const AccessKey second = deriveAccessKey(QStringLiteral("second-own"));

    // The pairing is POSITIONAL, so both arrangements are run: whichever entry a
    // single-slot map happened to keep, one of these two would pass on its own.
    for (int guardedAt : {0, 1}) {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Engine Data"), 0x100, CommsProtection::None, {}));
        // Different case, because the lookup is case-insensitive and these two
        // are the same name as far as every rule in this file is concerned.
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("engine data"), 0x200, CommsProtection::None, {}));
        cfg.bus[0].sections[guardedAt].protection = CommsProtection::Hidden;
        cfg.bus[0].sections[guardedAt].messageKey = first;

        QList<CommsSection> next = cfg.bus[0].sections;
        next[guardedAt].protection = CommsProtection::None;
        QString refusal;
        CHECK(!cfg.applyBusSections(0, next, &refusal));
        CHECK(!refusal.isEmpty());
        CHECK(cfg.bus[0].sections[guardedAt].protection == CommsProtection::Hidden);
    }

    // ...and it is not "any duplicate refuses everything" either. Two guarded
    // sections sharing a name, with DIFFERENT passwords: proving the second
    // authorises the second and leaves the first exactly where it was.
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Engine Data"), 0x100, CommsProtection::Hidden, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Engine Data"), 0x200, CommsProtection::Hidden, {}));
    cfg.bus[0].sections[0].messageKey = first;
    cfg.bus[0].sections[1].messageKey = second;
    // Granted from the SECTION, so it is the second one's key that is recorded —
    // which is the only thing that tells two same-named sections apart.
    cfg.grantSectionAccess(0, cfg.bus[0].sections[1]);
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::None; // the one nobody proved
        CHECK(!cfg.applyBusSections(0, next, nullptr));
    }
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[1].protection = CommsProtection::None; // the one that was proved
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Hidden);
        CHECK(cfg.bus[0].sections[1].protection == CommsProtection::None);
    }
}

// A PURE REORDER IS NOT A CHANGE, and pairing on the name alone said it was.
// Position is what document order pairs on, so moving one of two same-named
// sections past the other handed each of them the OTHER's prior record: the model
// compared "Secret" (key A) against the record for "Secret" (key B), saw a
// Message Password replaced, and refused. Reachable from the real dialog in two
// clicks — select the second of two same-named sections, Move Up, OK — and it
// refused an edit that changed nothing whatever.
//
// Identity — name, base address, device kind — is what a user means by "the same
// message", and it does not move when a row does.
static void testAReorderIsNotAChange()
{
    const AccessKey first = deriveAccessKey(QStringLiteral("first-own"));
    const AccessKey second = deriveAccessKey(QStringLiteral("second-own"));

    // Two same-named sections, different addresses, different passwords, neither
    // proved. Nothing but the order changes.
    {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Secret"), 0x100, CommsProtection::Hidden, {}));
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Secret"), 0x200, CommsProtection::Hidden, {}));
        cfg.bus[0].sections[0].messageKey = first;
        cfg.bus[0].sections[1].messageKey = second;

        QList<CommsSection> next = cfg.bus[0].sections;
        next.swapItemsAt(0, 1);
        QString refusal;
        CHECK(cfg.applyBusSections(0, next, &refusal));
        CHECK(refusal.isEmpty());
        CHECK(cfg.bus[0].sections[0].baseAddress == 0x200);
        CHECK(cfg.bus[0].sections[1].baseAddress == 0x100);
        CHECK(cfg.bus[0].sections[0].messageKey == second);
    }

    // The same reorder with DIFFERENT TIERS on the two, which is the arrangement
    // that made the refusal read as a lowering rather than a password swap.
    {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Secret"), 0x100, CommsProtection::Protected, {}));
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Secret"), 0x200, CommsProtection::ReadOnly, {}));
        cfg.bus[0].sections[0].messageKey = first;
        cfg.bus[0].sections[1].messageKey = second;

        QList<CommsSection> next = cfg.bus[0].sections;
        next.swapItemsAt(0, 1);
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::ReadOnly);
        CHECK(cfg.bus[0].sections[1].protection == CommsProtection::Protected);
    }

    // Identity is all three fields. A section that shares a name AND an address
    // with another but is a different KIND of thing is still its own message.
    {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Twin"), 0x100, CommsProtection::Hidden, {}));
        CommsSection tx = makeSection(QStringLiteral("Twin"), 0x100, CommsProtection::Hidden, {});
        tx.device = SectionDevice::TransmitMessage;
        cfg.bus[0].sections.append(tx);
        cfg.bus[0].sections[0].messageKey = first;
        cfg.bus[0].sections[1].messageKey = second;

        QList<CommsSection> next = cfg.bus[0].sections;
        next.swapItemsAt(0, 1);
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[0].isTransmit());
    }

    // ...and the guard is not softened by any of it. A section RENAMED or
    // RENUMBERED has no identity match, falls back to document order, and is
    // refused exactly as before — otherwise "edit the address while unticking"
    // would be the way past the whole rule.
    {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Secret"), 0x100, CommsProtection::Hidden, {}));
        cfg.bus[0].sections[0].messageKey = first;

        QList<CommsSection> renumbered = cfg.bus[0].sections;
        renumbered[0].baseAddress = 0x999;
        renumbered[0].protection = CommsProtection::None;
        CHECK(!cfg.applyBusSections(0, renumbered, nullptr));
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Hidden);
    }

    // Sections identical on ALL THREE fall back to document order, which is the
    // only pairing information that exists for them — they are indistinguishable
    // to anyone reading the list. The guard must still hold there.
    {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Same"), 0x100, CommsProtection::Hidden, {}));
        cfg.bus[0].sections.append(
            makeSection(QStringLiteral("Same"), 0x100, CommsProtection::None, {}));
        cfg.bus[0].sections[0].messageKey = first;

        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::None; // the guarded one, unticked
        CHECK(!cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Hidden);
    }
}

// RENAME AND UNTICK IN ONE COMMIT. The chokepoint paired a proposed section with
// a prior one by name — identity first, then the bare name — so a `next` section
// carrying a name the document had never seen paired with nothing at all and was
// read as an ADDITION. Rename "Secret" to "Public" and clear the marking in the
// same list and the whole rule stepped aside.
//
// It was justified by a premise that reads as if it covered this: "a concealed
// section cannot be renamed, because its editor will not open for it". READ ONLY
// IS A MARKED TIER THAT CONCEALS NOTHING and whose editor opens for anybody, so
// the premise was never true of one of the three tiers — and a caller of
// applyBusSections is under no obligation to have come through an editor at all,
// which is the point of having a chokepoint rather than a checkbox handler.
//
// Not reachable through the shipped UI today. It is pinned anyway because the
// contract of applyBusSections is about callers that do not exist yet: an undo
// stack, a Duplicate Section command, a scripting API.
// The same laundering, reached by DELETING the name instead of changing it. An
// unnamed prior used to be skipped by every pairing index, so an untick of one
// paired with nothing and read as an ADDITION: tier cleared, key gone, nothing
// proved, at all three tiers. Only a hand-edited file can produce an unnamed
// section — the editor auto-names an empty field and a Get names everything it
// rebuilds — which is exactly how latent the rename case was, and the same
// argument applies: this chokepoint refuses on its own, for callers that never
// came through an editor.
static void testAnUnnamedSectionCannotLaunderAnUntickEither()
{
    const CommsProtection tiers[] = {CommsProtection::ReadOnly, CommsProtection::Hidden,
                                     CommsProtection::Protected};
    const AccessKey own = deriveAccessKey(QStringLiteral("secret-own-password"));

    for (CommsProtection tier : tiers) {
        for (bool keyed : {true, false}) {
            Configuration cfg;
            cfg.bus[0].enabled = true;
            CommsSection s = makeSection(QStringLiteral("Secret"), 0x100, tier,
                                         QStringLiteral("Boost"));
            s.name.clear();
            if (keyed)
                s.messageKey = own;
            cfg.bus[0].sections.append(s);

            QList<CommsSection> next = cfg.bus[0].sections;
            next[0].protection = CommsProtection::None;

            QString refusal;
            CHECK(!cfg.applyBusSections(0, next, &refusal));
            CHECK(!refusal.isEmpty());
            CHECK(cfg.bus[0].sections.size() == 1);
            CHECK(cfg.bus[0].sections.first().protection == tier);
        }
    }

    // Not over-refused: REMOVING an unnamed marked section outright is still
    // free, because removal is permitted at every tier by spec.
    {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        CommsSection s = makeSection(QStringLiteral("Secret"), 0x100, CommsProtection::Hidden,
                                     QStringLiteral("Boost"));
        s.name.clear();
        s.messageKey = own;
        cfg.bus[0].sections.append(s);
        CHECK(cfg.applyBusSections(0, QList<CommsSection>(), nullptr));
        CHECK(cfg.bus[0].sections.isEmpty());
    }
}

static void testRenamingCannotLaunderAnUntick()
{
    struct Case {
        const char *label;
        CommsProtection tier;
    };
    const Case cases[] = {
        {"Read Only", CommsProtection::ReadOnly},
        {"Hidden", CommsProtection::Hidden},
        {"Protect Communication", CommsProtection::Protected},
    };
    const AccessKey own = deriveAccessKey(QStringLiteral("secret-own-password"));

    for (const Case &k : cases) {
        // Keyed AND keyless, because they are refused for different reasons and
        // only one of them was ever going to be noticed: a keyed section has a
        // password that could have authorised this and was not given, a keyless
        // one has nothing that could.
        for (bool keyed : {true, false}) {
            Configuration cfg;
            cfg.bus[0].enabled = true;
            CommsSection s = makeSection(QStringLiteral("Secret"), 0x100, k.tier,
                                         QStringLiteral("Boost"));
            if (keyed)
                s.messageKey = own;
            cfg.bus[0].sections.append(s);

            // Everything about the message is untouched except the two fields the
            // laundering needs: a new name, and no marking.
            QList<CommsSection> next = cfg.bus[0].sections;
            next[0].name = QStringLiteral("Public");
            next[0].protection = CommsProtection::None;

            QString refusal;
            CHECK(!cfg.applyBusSections(0, next, &refusal));
            CHECK(!refusal.isEmpty());
            // The document is untouched — the refusal is not a warning.
            CHECK(cfg.bus[0].sections.size() == 1);
            CHECK(cfg.bus[0].sections.first().protection == k.tier);
            CHECK(cfg.bus[0].sections.first().name == QLatin1String("Secret"));
        }
    }

    // ...and the legitimate version of the same edit still goes through. Someone
    // who has proved the section's password may rename it and unmark it in one
    // commit; the grant is answered against the PRIOR section, which is the name
    // and the key the challenge was met for.
    for (const Case &k : cases) {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        CommsSection s = makeSection(QStringLiteral("Secret"), 0x100, k.tier,
                                     QStringLiteral("Boost"));
        s.messageKey = own;
        cfg.bus[0].sections.append(s);
        cfg.grantSectionAccess(0, cfg.bus[0].sections.first());

        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].name = QStringLiteral("Public");
        next[0].protection = CommsProtection::None;
        QString refusal;
        CHECK(cfg.applyBusSections(0, next, &refusal));
        CHECK(refusal.isEmpty());
        CHECK(cfg.bus[0].sections.first().name == QLatin1String("Public"));
    }

    // REMOVAL IS STILL FREE, and this is what stops the body handle from being a
    // trap. Removing a marked message and adding a genuinely different one in the
    // same commit must still work: a different message is at a different address,
    // or carries different channels, and pairs with nothing.
    {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        CommsSection s = makeSection(QStringLiteral("Secret"), 0x100, CommsProtection::Hidden,
                                     QStringLiteral("Boost"));
        s.messageKey = own;
        cfg.bus[0].sections.append(s);

        QList<CommsSection> next;
        next.append(makeSection(QStringLiteral("Fresh"), 0x200, CommsProtection::None,
                                QStringLiteral("Torque")));
        QString refusal;
        CHECK(cfg.applyBusSections(0, next, &refusal));
        CHECK(refusal.isEmpty());
        CHECK(cfg.bus[0].sections.size() == 1);
        CHECK(cfg.bus[0].sections.first().name == QLatin1String("Fresh"));
    }
}

// A GRANT PROVED AGAINST NOTHING ANSWERS FOR NOTHING. sectionGrantProves matched
// on lowerName + provedKey with no bus, and EVERY keyless section carries the
// same kNoAccessKey — so where the section was keyless the key discriminated
// nothing and the lower-cased name, the one value the person being kept out
// chooses, decided on its own.
static void testKeylessGrantsProveNothing()
{
    // The probe, as it ran: two keyless Protected sections of the same name on
    // two buses, a document password set and NOT proved. Unlocking the one on
    // CAN 2 revealed the one on CAN 1 and made it lowerable.
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[1].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Engine Data"), 0x100, CommsProtection::Protected, {}));
    cfg.bus[1].sections.append(
        makeSection(QStringLiteral("Engine Data"), 0x100, CommsProtection::Protected, {}));
    CHECK(cfg.setCommsPassword(QStringLiteral("doc-password")));
    cfg.concealProtectedComms();
    CHECK(!cfg.commsRevealed());
    CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections[0]));

    cfg.grantSectionAccess(1, cfg.bus[1].sections[0]);
    CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections[0]));
    CHECK(!cfg.maySectionLower(cfg.bus[0].sections[0]));
    // ...and it answered for the section it was taken on no better. Nothing was
    // proved, so there is nothing for it to answer.
    CHECK(!cfg.isSectionRevealed(cfg.bus[1].sections[0]));
    CHECK(!cfg.sectionAccessGranted(1, cfg.bus[1].sections[0]));

    // SAME BUS too, which the old rule blocked only by accident: applyBusSections
    // is per-bus, so a same-bus impostor was refused for the wrong reason.
    Configuration same;
    same.bus[0].enabled = true;
    same.bus[0].sections.append(
        makeSection(QStringLiteral("Engine Data"), 0x100, CommsProtection::Hidden, {}));
    same.bus[0].sections.append(
        makeSection(QStringLiteral("Engine Data"), 0x200, CommsProtection::Hidden, {}));
    same.grantSectionAccess(0, same.bus[0].sections[1]);
    CHECK(!same.isSectionRevealed(same.bus[0].sections[0]));
    CHECK(!same.isSectionRevealed(same.bus[0].sections[1]));
    {
        QList<CommsSection> next = same.bus[0].sections;
        next[0].protection = CommsProtection::None;
        CHECK(!same.applyBusSections(0, next, nullptr));
    }

    // The BUS is matched wherever the caller has one, and applyBusSections has
    // one. Two sections that genuinely SHARE a password open each other through
    // the bus-less display predicate — that is inherent to using the key as the
    // discriminator and is stated in the header rather than pretended away — but
    // the chokepoint, which knows its bus, does not accept it.
    const AccessKey shared = deriveAccessKey(QStringLiteral("shared-password"));
    Configuration twins;
    twins.bus[0].enabled = true;
    twins.bus[1].enabled = true;
    twins.bus[0].sections.append(
        makeSection(QStringLiteral("Shared"), 0x100, CommsProtection::Hidden, {}));
    twins.bus[1].sections.append(
        makeSection(QStringLiteral("Shared"), 0x100, CommsProtection::Hidden, {}));
    twins.bus[0].sections[0].messageKey = shared;
    twins.bus[1].sections[0].messageKey = shared;
    twins.grantSectionAccess(1, twins.bus[1].sections[0]);
    CHECK(twins.isSectionRevealed(twins.bus[0].sections[0])); // bus-less: yes
    {
        QList<CommsSection> next = twins.bus[0].sections;
        next[0].protection = CommsProtection::None;
        CHECK(!twins.applyBusSections(0, next, nullptr)); // bus-checked: no
    }
    // ...and on the bus the grant was actually taken on, it applies.
    {
        QList<CommsSection> next = twins.bus[1].sections;
        next[0].protection = CommsProtection::None;
        CHECK(twins.applyBusSections(1, next, nullptr));
    }
}

// ------------------------------------------ the proof policy (rules 1 and 2)

// proofsRequiredFor is where "what does this tier demand" lives, and both
// dialogs read it rather than switching on the tier themselves. Pinned as a
// table, because the whole value of moving it here is that a change lands in one
// place — and a change that was meant to land in one place and did not is exactly
// what this notices.
static void testProofPolicy()
{
    CHECK(!Configuration::proofsRequiredFor(CommsProtection::None).sectionPassword);
    CHECK(!Configuration::proofsRequiredFor(CommsProtection::None).deviceProof);

    // Read Only and Hidden name no device anywhere, deliberately: they have to
    // stay usable with nothing plugged in.
    CHECK(Configuration::proofsRequiredFor(CommsProtection::ReadOnly).sectionPassword);
    CHECK(!Configuration::proofsRequiredFor(CommsProtection::ReadOnly).deviceProof);
    CHECK(Configuration::proofsRequiredFor(CommsProtection::Hidden).sectionPassword);
    CHECK(!Configuration::proofsRequiredFor(CommsProtection::Hidden).deviceProof);

    // BOTH. This is the 2.3.1 strengthening: Protected used to demand the device
    // round trip alone, which left it the only marked tier with no per-section
    // secret — one document password opened every Protected message in the file.
    CHECK(Configuration::proofsRequiredFor(CommsProtection::Protected).sectionPassword);
    CHECK(Configuration::proofsRequiredFor(CommsProtection::Protected).deviceProof);
}

// Rule 1's model half: the document's Protected Comms password is no longer
// sufficient on its own for a Protected section that carries a Message Password.
// It supplies the DEVICE half of that tier and says nothing about the section's.
static void testProtectedAlsoNeedsItsSectionPassword()
{
    const QString pass = QStringLiteral("edit-protected-comms");

    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Keyed"), 0x100, CommsProtection::Protected, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Legacy"), 0x200, CommsProtection::Protected, {}));
    cfg.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("keyed-own"));
    CHECK(cfg.setCommsPassword(pass)); // leaves the document revealed
    CHECK(cfg.commsRevealed());

    const CommsSection &keyed = cfg.bus[0].sections[0];
    const CommsSection &legacy = cfg.bus[0].sections[1];

    // The keyed one stays shut with the master password given. Before 2.3.1 both
    // of these were true and one document password read every Protected message.
    CHECK(!cfg.isSectionRevealed(keyed));
    CHECK(!cfg.maySectionLower(keyed));
    // ...and the untick is refused at the chokepoint, not merely in a dialog.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::Hidden;
        QString refusal;
        CHECK(!cfg.applyBusSections(0, next, &refusal));
        // The message has to name BOTH, or it sends the user off for one password
        // and leaves them stuck at the second.
        CHECK(refusal.contains(QLatin1String("Message Password")));
        CHECK(refusal.contains(QLatin1String("Protected Comms")));
    }

    // THE KEYLESS ARM, WHERE IT USED TO LIVE. "Legacy" is a Protected section
    // with no Message Password — the shape of every Protected message written
    // before 2.3.1, and of every section ANY Get produces, since the wire's field
    // is reserved[4] and carries no key. The tier used to fall back to the
    // document password for exactly this case and it was called an upgrade path.
    //
    // It behaved as a leak. commsRevealed() is TRUE for any document that has no
    // Protected Comms password, and a Get into a fresh window produces
    // precisely that document, so the fallback opened every retrieved Protected
    // message to everybody. These two lines asserted the opposite of what they
    // assert now, and that is why the suite stayed green through the user's bug.
    CHECK(cfg.commsRevealed()); // the document password is GIVEN...
    CHECK(!cfg.isSectionRevealed(legacy)); // ...and opens nothing
    CHECK(!cfg.maySectionLower(legacy));
    // Taking it away changes nothing either. Nothing opens this section, because
    // no password for it exists to be given.
    cfg.concealProtectedComms();
    CHECK(!cfg.isSectionRevealed(legacy));
    CHECK(!cfg.maySectionLower(legacy));
    // A grant cannot be taken for it, and that is the belt: kNoAccessKey is not a
    // secret, every keyless section in the document carries it, so a grant
    // recorded against it would be matched on the NAME alone — the one value the
    // person being kept out chooses.
    grantByName(cfg, 0, QStringLiteral("Legacy"));
    CHECK(!grantedByName(cfg, 0, QStringLiteral("Legacy")));
    CHECK(!cfg.isSectionRevealed(legacy));
    CHECK(!cfg.maySectionLower(legacy));

    // A grant is what opens the keyed one, and the grant stands for BOTH proofs
    // having been run — that contract lives on the two dialogs, which is why the
    // predicate here can stay this simple.
    grantByName(cfg, 0, QStringLiteral("Keyed"));
    CHECK(cfg.isSectionRevealed(keyed));
    CHECK(cfg.maySectionLower(keyed));
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::Hidden;
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Hidden);
    }
    // ...and it opened one section, not the tier. "Legacy" is still shut.
    CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections[1]));
}

// ------------------------------------------------------ the grant's lifetime

// Rule 3's model half. Communications Setup re-conceals a section whose editor
// has closed while the marking still hides it, and this is the operation it uses.
// The ORDER is the trap: a grant also authorises a lowering, so revoking before
// the change reaches applyBusSections refuses the very edit that was authorised —
// Protect Communication down to Hidden is exactly that shape.
static void testRevokeSectionAccess()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Prot"), 0x100, CommsProtection::Protected, {}));
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Hid"), 0x200, CommsProtection::Hidden, {}));
    cfg.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("prot-own"));
    cfg.bus[0].sections[1].messageKey = deriveAccessKey(QStringLiteral("hid-own"));

    grantByName(cfg, 0, QStringLiteral("Prot"));
    grantByName(cfg, 0, QStringLiteral("Hid"));
    CHECK(grantedByName(cfg, 0, QStringLiteral("Prot")));
    CHECK(grantedByName(cfg, 0, QStringLiteral("Hid")));

    // One section, and only that one. A revoke that reached its neighbours would
    // re-lock messages the user never touched.
    cfg.revokeSectionAccess(0, QStringLiteral("Prot"));
    CHECK(!grantedByName(cfg, 0, QStringLiteral("Prot")));
    CHECK(grantedByName(cfg, 0, QStringLiteral("Hid")));
    CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections[0]));
    CHECK(cfg.isSectionRevealed(cfg.bus[0].sections[1]));

    // Case-insensitive on the way out as well as in, or a grant taken under
    // "Engine Data" would survive a revoke of "engine data".
    cfg.revokeSectionAccess(0, QStringLiteral("hID"));
    CHECK(!grantedByName(cfg, 0, QStringLiteral("Hid")));
    // Revoking what was never granted is a no-op, not a fault: the dialog queues
    // a section's OLD and NEW name and only one of them can have been granted.
    cfg.revokeSectionAccess(0, QStringLiteral("Never Granted"));

    // THE ORDERING. Written first, revoked second: accepted. This is the sequence
    // CommunicationsDialog::accept() runs, and the reason its flushPendingRevokes()
    // call sits after the applyBusSections loop rather than before it.
    grantByName(cfg, 0, QStringLiteral("Prot"));
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::Hidden; // lowered, and STILL concealing
        CHECK(cfg.applyBusSections(0, next, nullptr));
        cfg.revokeSectionAccess(0, QStringLiteral("Prot"));
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Hidden);
        CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections[0])); // padlocked again
    }
    // Backwards, on the same edit, from the state it started in: refused. If this
    // ever passes, the ordering has stopped mattering and the comment explaining
    // it has become a lie.
    {
        cfg.bus[0].sections[0].protection = CommsProtection::Protected;
        grantByName(cfg, 0, QStringLiteral("Prot"));
        cfg.revokeSectionAccess(0, QStringLiteral("Prot"));
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::Hidden;
        CHECK(!cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Protected);
    }
}

// A GRANT NAMES A SECTION, NOT A STRING THE VIEWER PICKED. It used to be a bare
// lower-cased name, matched across every bus — and a name is a value the person
// being kept out gets to choose. Create a section on ANOTHER BUS with the same
// name and a password of your own, unlock that one, and the real one opened:
// applyBusSections is per-bus, the grant set was global, and same-bus was the
// only case accidentally blocked.
//
// The grant now records the bus AND the messageKey the challenge was answered
// against, and the key is what makes the whole thing hold: it is the half an
// impostor cannot reproduce, and it retires the grant automatically when the
// password it stood for is replaced.
static void testGrantsNameASectionNotAName()
{
    const AccessKey real = deriveAccessKey(QStringLiteral("the-real-password"));
    const AccessKey mine = deriveAccessKey(QStringLiteral("my-own-password"));

    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[2].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Secret"), 0x100, CommsProtection::Hidden, {}));
    cfg.bus[0].sections[0].messageKey = real;
    // The impostor: same name, another bus, a password the attacker chose.
    cfg.bus[2].sections.append(
        makeSection(QStringLiteral("Secret"), 0x100, CommsProtection::Hidden, {}));
    cfg.bus[2].sections[0].messageKey = mine;

    // Unlocking the one you own opens the one you own, and nothing else.
    cfg.grantSectionAccess(2, cfg.bus[2].sections[0]);
    CHECK(cfg.isSectionRevealed(cfg.bus[2].sections[0]));
    CHECK(cfg.maySectionLower(cfg.bus[2].sections[0]));
    CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections[0]));
    CHECK(!cfg.maySectionLower(cfg.bus[0].sections[0]));
    // ...and the chokepoint agrees, which is the line that mattered: this is the
    // step the probe reached, and it applied.
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::None;
        QString refusal;
        CHECK(!cfg.applyBusSections(0, next, &refusal));
        CHECK(!refusal.isEmpty());
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::Hidden);
    }
    // The exact, bus-checked question answers per bus, so a caller that passes
    // the wrong one is told no rather than quietly told yes.
    CHECK(cfg.sectionAccessGranted(2, cfg.bus[2].sections[0]));
    CHECK(!cfg.sectionAccessGranted(0, cfg.bus[2].sections[0]));
    CHECK(!cfg.sectionAccessGranted(0, cfg.bus[0].sections[0]));

    // Proving the real one opens the real one.
    cfg.grantSectionAccess(0, cfg.bus[0].sections[0]);
    CHECK(cfg.isSectionRevealed(cfg.bus[0].sections[0]));
    {
        QList<CommsSection> next = cfg.bus[0].sections;
        next[0].protection = CommsProtection::None;
        CHECK(cfg.applyBusSections(0, next, nullptr));
        CHECK(cfg.bus[0].sections[0].protection == CommsProtection::None);
    }

    // THE BELT. A grant dies with the key it was proved against, whoever
    // replaces it and by whatever route — so even a writer that skipped the
    // chokepoint cannot hand a section to a new owner and leave the old owner's
    // session standing on it.
    Configuration keyed;
    keyed.bus[0].enabled = true;
    keyed.bus[0].sections.append(
        makeSection(QStringLiteral("Secret"), 0x100, CommsProtection::Hidden, {}));
    keyed.bus[0].sections[0].messageKey = real;
    keyed.grantSectionAccess(0, keyed.bus[0].sections[0]);
    CHECK(keyed.isSectionRevealed(keyed.bus[0].sections[0]));
    keyed.bus[0].sections[0].messageKey = mine; // written behind the chokepoint's back
    CHECK(!keyed.isSectionRevealed(keyed.bus[0].sections[0]));
    CHECK(!keyed.maySectionLower(keyed.bus[0].sections[0]));
    CHECK(!keyed.sectionAccessGranted(0, keyed.bus[0].sections[0]));

    // Revoking reaches one bus's grant and leaves the other's alone, for the
    // same reason granting does.
    Configuration two;
    two.bus[0].enabled = true;
    two.bus[1].enabled = true;
    two.bus[0].sections.append(
        makeSection(QStringLiteral("Shared"), 0x100, CommsProtection::Hidden, {}));
    two.bus[1].sections.append(
        makeSection(QStringLiteral("Shared"), 0x100, CommsProtection::Hidden, {}));
    two.bus[0].sections[0].messageKey = real;
    two.bus[1].sections[0].messageKey = mine;
    two.grantSectionAccess(0, two.bus[0].sections[0]);
    two.grantSectionAccess(1, two.bus[1].sections[0]);
    two.revokeSectionAccess(0, QStringLiteral("shared")); // case-insensitive, as ever
    CHECK(!two.sectionAccessGranted(0, two.bus[0].sections[0]));
    CHECK(two.sectionAccessGranted(1, two.bus[1].sections[0]));
}

static void testLiveViewCannotAuthoriseALowering()
{
    // copyContentTo makes the Live View's own Configuration. It is a
    // Configuration, so maySectionLower() can be asked of it — and a copy
    // that omitted the verifier set would answer commsRevealed() == true and
    // wave through any downgrade placed on it. That is why copying
    // m_accessVerifiers and m_commsRevealed is a prerequisite of the untick rule
    // rather than a tidy-up.
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Prot"), 0x100, CommsProtection::Protected, {}));
    // Keyed, because a KEYLESS marked section is concealed from everybody and
    // lowerable by nobody — so it would answer this test's questions the right
    // way for the wrong reason, and would go on answering them if copyContentTo
    // stopped copying anything at all.
    cfg.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("prot-own"));
    CHECK(cfg.setCommsPassword(QStringLiteral("live-view-secret")));
    cfg.concealProtectedComms();

    Configuration view;
    cfg.copyContentTo(view);
    CHECK(view.hasCommsPassword());
    CHECK(!view.commsRevealed());
    CHECK(view.bus[0].sections.size() == 1);
    if (view.bus[0].sections.size() == 1)
        CHECK(!view.maySectionLower(view.bus[0].sections[0]));
    CHECK(view.anySectionConcealed());
    QList<CommsSection> next = view.bus[0].sections;
    CHECK(next.size() == 1);
    if (next.size() == 1) {
        next[0].protection = CommsProtection::None;
        CHECK(!view.applyBusSections(0, next, nullptr));
    }

    // The open state travels too, in the other direction: a copy taken while the
    // section is unlocked must not re-conceal it, or the Live View would blank
    // out the very channels the operator opened the document to watch.
    //
    // It is the GRANT that carries this, not commsRevealed(). The document-wide
    // flag reveals nothing at any tier now, so a copy that carried only that
    // would answer this question wrongly in the direction that hides work.
    CHECK(cfg.revealProtectedComms(QStringLiteral("live-view-secret")));
    cfg.grantSectionAccess(0, cfg.bus[0].sections[0]);
    CHECK(!cfg.anySectionConcealed());
    Configuration open;
    cfg.copyContentTo(open);
    CHECK(open.commsRevealed());
    CHECK(!open.anySectionConcealed());
    CHECK(open.bus[0].sections.size() == 1);
    if (open.bus[0].sections.size() == 1)
        CHECK(open.maySectionLower(open.bus[0].sections[0]));
}

// ------------------------------------------------------------------- the Get

static void testGetPreservesDocumentState()
{
    // mapFromDevice calls clearContent(), not clear(). Before that split a Get
    // wiped the access verifiers, after which hasCommsPassword() was false,
    // commsRevealed() was therefore true, and every tier in the document could
    // be lowered with no challenge whatever — from the one operation an operator
    // performs routinely and without thinking of it as a change to the document.
    const QString pass = QStringLiteral("survives-a-get");

    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Prot"), 0x100, CommsProtection::Protected,
                    QStringLiteral("Secret Value")));
    // Keyed. A grant is only ever recorded against a real secret — kNoAccessKey
    // is shared by every keyless section, so a grant taken against it would be
    // decided by the name alone — so a keyless fixture here would prove that a
    // Get preserves grants by having none to lose.
    cfg.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("prot-own"));
    cfg.setConfigTitle(QStringLiteral("Before The Get"));
    CHECK(cfg.setCommsPassword(pass));
    grantByName(cfg, 0, QStringLiteral("Prot"));
    cfg.concealProtectedComms();
    CHECK(cfg.hasCommsPassword());
    CHECK(!cfg.commsRevealed());
    // Conceal clears the grants, so re-grant one to prove a Get does not.
    grantByName(cfg, 0, QStringLiteral("Prot"));

    // A Get of a device holding one protected message.
    Configuration source;
    source.bus[0].enabled = true;
    source.bus[0].sections.append(
        makeSection(QStringLiteral("Prot"), 0x100, CommsProtection::Protected, {}));
    const MappingResult mr = mapToDevice(source);
    CHECK(mr.ok());
    mapFromDevice(mr.tables, cfg);

    // The CONTENT is replaced...
    CHECK(cfg.configTitle().isEmpty());
    // ...and everything the DOCUMENT knew survives.
    CHECK(cfg.hasCommsPassword());
    CHECK(cfg.accessVerifiers().isSet(AccessFunction::EditProtectedComms));
    CHECK(!cfg.commsRevealed());
    // A section this session has proved nothing about is still guarded: the
    // verifier came back, so commsRevealed() is false and Protected still needs
    // it. (The grant checked below is what makes "Prot" itself lowerable.)
    CHECK(!cfg.maySectionLower(
        makeSection(QStringLiteral("Untouched"), 0x999, CommsProtection::Protected, {})));
    CHECK(grantedByName(cfg, 0, QStringLiteral("Prot")));
    // The right password still opens it afterwards, which is the proof that the
    // verifier itself came through rather than merely the "has one" flag.
    CHECK(cfg.revealProtectedComms(pass));
    CHECK(cfg.commsRevealed());
}

// A Get must not destroy the two per-section facts the wire does not carry: the
// section's own password and the name the user gave it. It used to destroy both.
// clearContent() empties every BusConfig, mapFromDevice rebuilds the sections
// from the device tables, and the record's key field is `reserved[4]` — so every
// messageKey came back kNoAccessKey, which means "no password on this section",
// which means the comms dialog opened a Hidden section with no prompt and the
// untick went through. One routine Get walked past every per-section password in
// the document.
// THE BUG AS REPORTED: set up a protected message, Send, restart the Device
// Manager, Get. The message came back marked and with NO password - concealed,
// un-editable and un-releasable, because releasing a marking is authorised by
// the password guarding it and the only copy had stayed in a .ct3. The wire had
// nowhere to carry one after the v20 per-message key was retired in 2.3.0.
//
// Store v16 carries a three-bit SLOT instead: the four passwords are
// document-wide and ride once in the config header, and each marked message
// names one. This is that exact sequence - map to the device, then read back
// into a document that has never seen any of it.
static void testGetIntoAFreshDocumentRestoresSectionKeys()
{
    const AccessKey protKey = deriveAccessKey(QStringLiteral("first-password"));
    const AccessKey hidKey = deriveAccessKey(QStringLiteral("second-password"));

    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel c;
    c.name = QStringLiteral("Boost");
    c.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(c);
    // The two passwords exist as DOCUMENT slots first; a marked message names
    // one rather than inventing its own.
    cfg.setCommsPasswordSlot(1, protKey);
    cfg.setCommsPasswordSlot(3, hidKey); // not slot 2: the number must travel
    CHECK(cfg.commsPasswordSlotFor(hidKey) == 3);

    CommsSection prot = makeSection(QStringLiteral("Driveline"), 0x640,
                                    CommsProtection::Protected, QStringLiteral("Boost"));
    prot.alignment = SectionAlignment::WordSwap;
    prot.messageKey = protKey;
    cfg.bus[0].sections.append(prot);
    CommsSection hid = makeSection(QStringLiteral("Turbo"), 0x641,
                                   CommsProtection::Hidden, {});
    hid.messageKey = hidKey;
    cfg.bus[0].sections.append(hid);
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Open"), 0x642, CommsProtection::None, {}));

    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    if (!mr.ok())
        return;
    // The slot NUMBER is what travels, not the key.
    CHECK(mr.tables.messages.size() == 3);
    if (mr.tables.messages.size() == 3) {
        const auto slotOf = [&](quint32 id) {
            for (const CanMessageConfig &m : mr.tables.messages)
                if (m.can_id == id)
                    return int(m.password_slot);
            return -1;
        };
        CHECK(slotOf(0x640) == 1);
        CHECK(slotOf(0x641) == 3);
        CHECK(slotOf(0x642) == 0); // unmarked: nothing to point at
    }
    CHECK(mr.tables.messagePasswords.key[0] == quint32(protKey));
    CHECK(mr.tables.messagePasswords.key[2] == quint32(hidKey));
    CHECK(mr.tables.messagePasswords.key[1] == 0); // an empty slot stays empty

    // The Get, into a document that knows nothing - a restarted Device Manager.
    Configuration fresh;
    mapFromDevice(mr.tables, fresh);
    CHECK(fresh.bus[0].sections.size() == 3);
    if (fresh.bus[0].sections.size() != 3)
        return;

    // The four came home...
    CHECK(fresh.commsPassword(1) == protKey);
    CHECK(fresh.commsPassword(3) == hidKey);
    CHECK(fresh.commsPassword(2) == kNoAccessKey);
    CHECK(fresh.commsPasswordsInUse() == 2);

    const auto keyOf = [&](quint32 id) {
        for (const CommsSection &s : fresh.bus[0].sections)
            if (s.baseAddress == id)
                return s.messageKey;
        return kNoAccessKey;
    };
    const auto tierOf = [&](quint32 id) {
        for (const CommsSection &s : fresh.bus[0].sections)
            if (s.baseAddress == id)
                return s.protection;
        return CommsProtection::None;
    };
    // ...the markings survive, as they always did...
    CHECK(tierOf(0x640) == CommsProtection::Protected);
    CHECK(tierOf(0x641) == CommsProtection::Hidden);
    // ...and now so do the passwords, rebuilt from the slot each message names.
    CHECK(keyOf(0x640) == protKey);
    CHECK(keyOf(0x641) == hidKey);
    CHECK(keyOf(0x642) == kNoAccessKey); // nothing invented for an unmarked one

    // Which is the point: the section can be proved and released, where before a
    // Get left it permanently stuck.
    for (const CommsSection &s : fresh.bus[0].sections)
        if (s.baseAddress == 0x641) {
            fresh.grantSectionAccess(0, s);
            CHECK(fresh.maySectionLower(fresh.bus[0].sections[1]));
        }

    // THE DOCUMENT WINS WHERE IT HAS AN ANSWER. A password changed here but not
    // yet sent must not be overwritten by the unit's older copy.
    Configuration newer;
    newer.bus[0].enabled = true;
    newer.catalog().addOrUpdateUserChannel(c);
    const AccessKey changedKey = deriveAccessKey(QStringLiteral("changed-since-the-send"));
    newer.setCommsPasswordSlot(1, changedKey);
    CommsSection changed = prot;
    changed.messageKey = changedKey;
    newer.bus[0].sections.append(changed);
    mapFromDevice(mr.tables, newer);
    CHECK(newer.commsPassword(1) == changedKey);
    bool found = false;
    for (const CommsSection &s : newer.bus[0].sections)
        if (s.baseAddress == 0x640) {
            found = true;
            CHECK(s.messageKey == changedKey);
            CHECK(s.messageKey != protKey);
        }
    CHECK(found);
}

static void testGetPreservesSectionKeysAndNames()
{
    const AccessKey hidKey = deriveAccessKey(QStringLiteral("hidden-own-password"));

    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel c;
    c.name = QStringLiteral("Boost");
    c.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(c);
    CommsSection hid = makeSection(QStringLiteral("Turbo pressure"), 0x640,
                                   CommsProtection::Hidden, QStringLiteral("Boost"));
    hid.alignment = SectionAlignment::WordSwap; // so the 16-bit row fits (see below)
    hid.messageKey = hidKey;
    // v16: a marked message NAMES one of the document's four passwords. The key
    // alone is no longer enough - with no slot, mapToDevice writes 0 and the Get
    // has nothing to resolve it against.
    cfg.setCommsPasswordSlot(1, hidKey);
    cfg.bus[0].sections.append(hid);
    CHECK(cfg.anySectionConcealed());

    // The device image the Get reads back. Built from this same document, which
    // is the realistic case: Send, then Get.
    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    if (!mr.ok())
        return;
    mapFromDevice(mr.tables, cfg);

    CHECK(cfg.bus[0].sections.size() == 1);
    if (cfg.bus[0].sections.size() != 1)
        return;
    const CommsSection &back = cfg.bus[0].sections[0];
    // The user's own name, not the regenerated "Receive 0x640" — which would
    // also have printed the withheld CAN ID straight into the sections list.
    CHECK(back.name == QLatin1String("Turbo pressure"));
    CHECK(back.protection == CommsProtection::Hidden);
    CHECK(back.messageKey == hidKey);
    CHECK(back.messageKey != kNoAccessKey);
    // ...so it is still concealed and still guarded afterwards.
    CHECK(!cfg.isSectionRevealed(back));
    CHECK(!cfg.maySectionLower(back));
    CHECK(cfg.anySectionConcealed());

    // A section the document has NEVER seen has no password to restore — nothing
    // has one to give it — and must not be named after the ID it is withholding.
    //
    // THIS IS THE USER'S BUG, and until 2.3.2 the last line of it asserted the
    // opposite: `CHECK(fresh.isSectionRevealed(...))`, under a comment reading
    // "keyless means open, deliberately: the alternative is a section nobody can
    // ever open again". That is why the suite stayed green while a Hidden
    // message, sent to a device and read back in a restarted application, showed
    // a padlock and the word "hidden" over a row whose channels were listed and
    // whose editor opened on a double-click with nothing asked for.
    //
    // The alternative it was afraid of is the correct behaviour. A section nobody
    // can open is what a marked section with no password IS: no password for it
    // exists anywhere, so there is nothing that could open it honestly, and
    // showing the protocol to someone who cannot produce one is the exact failure
    // the tier is for. It is not a brick either — see the removal check below.
    Configuration fresh;
    mapFromDevice(mr.tables, fresh);
    CHECK(fresh.bus[0].sections.size() == 1);
    if (fresh.bus[0].sections.size() != 1)
        return;
    CHECK(!fresh.bus[0].sections[0].name.contains(QLatin1String("640"),
                                                  Qt::CaseInsensitive));
    CHECK(fresh.bus[0].sections[0].name == QLatin1String("Hidden message 1"));
    // STORE v16: THE TIER SURVIVES AND SO DOES THE KEY. This assertion used to
    // read the other way — "the key does not, the wire has nowhere to carry
    // one" — and the paragraph above it argued that a keyless concealed section
    // was the honest outcome. It was honest and it was unusable: a Get into a
    // fresh document produced a message nobody could edit, release or open,
    // because the only copy of its password had stayed in a .ct3.
    //
    // The wire carries one now, in its own sparse table. See
    // testGetIntoAFreshDocumentRestoresSectionKeys for the round trip itself.
    CHECK(fresh.bus[0].sections[0].protection == CommsProtection::Hidden);
    CHECK(fresh.bus[0].sections[0].messageKey == hidKey);
    // Still CONCEALED until the password is actually given — holding the key is
    // not the same as having proved it, and nothing here has proved anything.
    CHECK(!fresh.isSectionRevealed(fresh.bus[0].sections[0]));
    CHECK(fresh.bus[0].sections[0].isConcealed(
        fresh.isSectionRevealed(fresh.bus[0].sections[0])));
    CHECK(fresh.anySectionConcealed());
    CHECK(fresh.isChannelConcealed(QStringLiteral("Boost")));

    // ...but it is now OPENABLE by whoever knows the password, which is what the
    // table was added for. Proving it grants access and the section lowers in the
    // ordinary way. Removal stays free at every tier, as it always was.
    {
        fresh.grantSectionAccess(0, fresh.bus[0].sections[0]);
        CHECK(fresh.isSectionRevealed(fresh.bus[0].sections[0]));
        CHECK(fresh.maySectionLower(fresh.bus[0].sections[0]));
        QList<CommsSection> gone;
        CHECK(fresh.applyBusSections(0, gone, nullptr));
        CHECK(fresh.bus[0].sections.isEmpty());
    }
}

// THE USER'S SCENARIO, END TO END: build a configuration, mark a message, send
// it, RESTART the application, Get it back. A fresh Configuration has never held
// the section, so mapFromDevice has no snapshot of its own to re-apply and the
// section arrives exactly as the wire describes it.
//
// WHAT ARRIVES CHANGED AT STORE v16, and this test changed with it. It used to
// assert that the marking came home KEYLESS - concealed, un-editable and
// un-releasable - and argued that was the honest outcome, because the wire had
// nowhere to carry a password after the v20 per-message key was retired.
//
// It was honest and it was unusable: the message could not be opened, edited or
// released by anybody, and the only copy of its password had stayed in a .ct3
// the user might not have. The wire carries a three-bit SLOT now, naming one of
// the document's four passwords, and the four keys ride in the config header.
//
// So the padlock is still real - concealment does not weaken here, and every
// assertion about what is WITHHELD stands unchanged. What is new is that the
// message comes back with a password, which is what makes it manageable.
static void testGetIntoAFreshDocumentConceals()
{
    for (const auto &tier : {CommsProtection::Hidden, CommsProtection::Protected}) {
        Configuration built;
        built.bus[0].enabled = true;
        Channel c;
        c.name = QStringLiteral("Boost");
        c.userDefined = true;
        built.catalog().addOrUpdateUserChannel(c);
        CommsSection s = makeSection(QStringLiteral("Turbo pressure"), 0x640, tier,
                                     QStringLiteral("Boost"));
        s.alignment = SectionAlignment::WordSwap; // so the 16-bit row fits
        // KEYED, exactly as the user had it. registerPassword puts the password
        // in one of the document's four slots as well as on the section, which
        // is what a marking means now: the wire carries the SLOT, and a key
        // belonging to no slot has nothing to travel as.
        s.messageKey = registerPassword(built, "the-message-password");
        built.bus[0].sections.append(s);
        built.grantSectionAccess(0, built.bus[0].sections[0]);
        CHECK(built.isSectionRevealed(built.bus[0].sections[0]));

        const MappingResult mr = mapToDevice(built);
        CHECK(mr.ok());
        if (!mr.ok())
            return;

        // The restart: a Configuration that has never seen this message.
        Configuration afterRestart;
        mapFromDevice(mr.tables, afterRestart);
        CHECK(afterRestart.bus[0].sections.size() == 1);
        if (afterRestart.bus[0].sections.size() != 1)
            return;
        const CommsSection &back = afterRestart.bus[0].sections[0];
        CHECK(back.protection == tier); // the lock the user saw is real...
        // ...and the password came home with it, rebuilt from the slot the
        // message names. This assertion read == kNoAccessKey before v16.
        CHECK(back.messageKey == built.bus[0].sections[0].messageKey);
        CHECK(afterRestart.commsPasswordSlotFor(back.messageKey) != 0);

        // CONCEALMENT IS UNCHANGED. Holding the key is not the same as having
        // PROVED it, and nothing here has proved anything - so the message is
        // still withheld exactly as it was, and every assertion below is the one
        // that was here before.
        CHECK(!afterRestart.isSectionRevealed(back));
        CHECK(back.isConcealed(afterRestart.isSectionRevealed(back)));
        // "But the Channels are Listed" - not any more, at either tier.
        CHECK(afterRestart.isChannelConcealed(QStringLiteral("Boost")));
        CHECK(afterRestart.concealedChannelNames().contains(QStringLiteral("Boost")));

        // WHAT IS NEW is that the door now opens for whoever knows the password.
        // Before v16 a grant could not be manufactured because the section held
        // no key to match one against; now proving the password grants access and
        // the marking can be released, which is the entire point of carrying it.
        CHECK(afterRestart.maySectionLower(back) == false); // not yet proved
        afterRestart.grantSectionAccess(0, back);
        CHECK(afterRestart.sectionAccessGranted(0, back));
        CHECK(afterRestart.isSectionRevealed(afterRestart.bus[0].sections[0]));
        CHECK(afterRestart.maySectionLower(afterRestart.bus[0].sections[0]));
    }

    // A RELAY, which carries no channels and so exercises the other half of
    // mapFromDevice's rebuild. A relay forwards whole frames, so what a marking
    // withholds on one is the match address and the mask — the gateway rule
    // itself — and it must be withheld on the same terms.
    Configuration built;
    built.bus[0].enabled = true;
    built.bus[1].enabled = true;
    CommsSection relay;
    relay.name = QStringLiteral("Gateway rule");
    relay.device = SectionDevice::MessageRelay;
    relay.baseAddress = 0x300;
    relay.relayBitmask = 0x7F0;
    relay.routeBusMask = 0x2; // to CAN 2, never this section's own bus
    relay.protection = CommsProtection::Hidden;
    relay.messageKey = registerPassword(built, "relay-own");
    built.bus[0].sections.append(relay);

    const MappingResult mr = mapToDevice(built);
    CHECK(mr.ok());
    if (!mr.ok())
        return;
    Configuration afterRestart;
    mapFromDevice(mr.tables, afterRestart);
    CHECK(afterRestart.bus[0].sections.size() == 1);
    if (afterRestart.bus[0].sections.size() != 1)
        return;
    const CommsSection &back = afterRestart.bus[0].sections[0];
    CHECK(back.isRelay());
    CHECK(back.protection == CommsProtection::Hidden);
    CHECK(back.messageKey == kNoAccessKey);
    CHECK(!afterRestart.isSectionRevealed(back));
    CHECK(!afterRestart.maySectionLower(back));
    CHECK(afterRestart.anySectionConcealed());
}

// The save paths, after the reversal above. This is the one place where failing
// closed would have cost more than it bought, and the resolution is deliberately
// asymmetric: what a file writer must not do is LAUNDER a secret it cannot read,
// and a keyless section has no secret to launder.
static void testSavingAfterAGet()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString err;

    // ---- keyless concealed sections SAVE ----
    // Otherwise "back up this unit before I change anything" is impossible: every
    // section a Get produces is keyless and concealed, so the broad test would
    // refuse every configuration read off a device. It would protect nothing
    // either — no password for these sections exists, the file gains no reader it
    // did not already have, and whoever is saving can read the same bytes off the
    // unit with any serial tool.
    {
        Configuration got;
        got.bus[0].enabled = true;
        got.bus[0].sections.append(
            makeSection(QStringLiteral("Retrieved"), 0x640, CommsProtection::Hidden, {}));
        got.bus[0].sections.append(
            makeSection(QStringLiteral("Retrieved Protected"), 0x650,
                        CommsProtection::Protected, {}));
        CHECK(got.anySectionConcealed());        // it IS concealed...
        CHECK(!got.anyKeyedSectionConcealed());  // ...and nothing could open it
        const QString path = dir.filePath(QStringLiteral("backup.ct3"));
        CHECK(got.saveToFile(path, &err));
        CHECK(err.isEmpty());
        CHECK(QFile::exists(path));

        SecureSaveOptions opts;
        CHECK(got.saveSecureToFile(dir.filePath(QStringLiteral("backup.ct3s")), opts, &err));
    }

    // ---- a KEYED concealed section SAVES, both writers, and stays shut ------
    // This was the laundering case the refusal was written for, and the reason
    // it stopped being one is the format: writing the message out no longer
    // writes its CAN ID and bit layout anywhere legible. Both writers seal, the
    // tier and the key go with the section, and what the writer could not read
    // before the save it cannot read after it.
    {
        Configuration keyed;
        keyed.bus[0].enabled = true;
        keyed.bus[0].sections.append(
            makeSection(QStringLiteral("Guarded"), 0x640, CommsProtection::Hidden, {}));
        keyed.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("guarded-own"));
        CHECK(keyed.anyKeyedSectionConcealed());

        const QString shut = dir.filePath(QStringLiteral("shut.ct3"));
        err.clear();
        CHECK(keyed.saveToFile(shut, &err));
        CHECK(QFile::exists(shut));
        SecureSaveOptions opts;
        const QString shutSecure = dir.filePath(QStringLiteral("shut.ct3s"));
        CHECK(keyed.saveSecureToFile(shutSecure, opts, &err));

        // Neither file is legible, and neither opens the message.
        for (const QString &p : {shut, shutSecure}) {
            QFile f(p);
            CHECK(f.open(QIODevice::ReadOnly));
            const QByteArray raw = f.readAll();
            f.close();
            CHECK(!raw.contains("Guarded"));

            Configuration back;
            QString why;
            CHECK(back.loadFromFile(p, &why));
            CHECK(back.anyKeyedSectionConcealed());
            CHECK(back.bus[0].sections.size() == 1);
            CHECK(back.bus[0].sections.first().protection == CommsProtection::Hidden);
            CHECK(back.bus[0].sections.first().messageKey
                  == deriveAccessKey(QStringLiteral("guarded-own")));
        }

        // The section's own password is still what opens it, as it always was.
        keyed.grantSectionAccess(0, keyed.bus[0].sections[0]);
        CHECK(!keyed.anyKeyedSectionConcealed());
    }

    // ---- one of each, and both travel ----
    // A mixed document is the realistic one — a Get into the window the
    // configuration was built in restores the keys it knew and leaves the rest
    // keyless. Both kinds save, and the keyed one is still the one that answers
    // anyKeyedSectionConcealed().
    {
        Configuration mixed;
        mixed.bus[0].enabled = true;
        mixed.bus[0].sections.append(
            makeSection(QStringLiteral("Retrieved"), 0x640, CommsProtection::Hidden, {}));
        mixed.bus[0].sections.append(
            makeSection(QStringLiteral("Guarded"), 0x650, CommsProtection::Hidden, {}));
        mixed.bus[0].sections[1].messageKey = deriveAccessKey(QStringLiteral("guarded-own"));
        CHECK(mixed.anyKeyedSectionConcealed());
        const QString mixedPath = dir.filePath(QStringLiteral("mixed.ct3"));
        CHECK(mixed.saveToFile(mixedPath, &err));
        Configuration back;
        CHECK(back.loadFromFile(mixedPath, &err));
        CHECK(back.bus[0].sections.size() == 2);
        CHECK(back.anyKeyedSectionConcealed());
    }
}

// RENUMBERING A MESSAGE MUST NOT DISCARD ITS PASSWORD. mapFromDevice snapshots
// each section's key against its identity ON THE WIRE — bus, CAN ID, direction —
// so editing a base address and then doing a Get left the rebuilt section
// matching no snapshot at all: the key went silently over the side, and the
// section came back keyless, marked, and therefore open to everybody. That is a
// plain bug for anyone renumbering a message before quite apart from what it
// costs the marking.
//
// THE SECTIONS ARE NAMED THE WAY A USER NAMES THEM, and that is the whole point
// of this fixture rather than a detail of it. It used to call them "Hidden
// message 1" and "Hidden message 2" — the names mapFromDevice GENERATES for a
// concealing section it does not recognise — so the fallback it was written to
// pin was matching the mapper's own output against itself, and passed while the
// case it stands for was broken. Type a real name into it and the section came
// back as "Hidden message 1" with no key: unopenable, un-unmarkable, and no
// longer even recognisable, out of an edit nobody thinks of as destructive.
static void testGetKeepsThePasswordWhenTheIdMoves()
{
    const AccessKey movedKey = deriveAccessKey(QStringLiteral("moved-own-password"));
    const AccessKey stayedKey = deriveAccessKey(QStringLiteral("stayed-own-password"));

    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel c;
    c.name = QStringLiteral("Boost");
    c.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(c);
    Channel c2;
    c2.name = QStringLiteral("Torque");
    c2.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(c2);

    CommsSection moved = makeSection(QStringLiteral("Turbo pressure"), 0x640,
                                     CommsProtection::Hidden, QStringLiteral("Boost"));
    moved.alignment = SectionAlignment::WordSwap; // so the 16-bit row fits
    moved.messageKey = movedKey;
    CommsSection stayed = makeSection(QStringLiteral("Oil temperature"), 0x650,
                                      CommsProtection::Hidden, QStringLiteral("Torque"));
    stayed.alignment = SectionAlignment::WordSwap;
    stayed.messageKey = stayedKey;
    cfg.bus[0].sections.append(moved);
    cfg.bus[0].sections.append(stayed);

    // The device image, taken while the document and the hardware still agree.
    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    if (!mr.ok())
        return;

    // The edit that used to cost a password: one message renumbered in the
    // DOCUMENT, with the device still holding the address it was sent at.
    cfg.bus[0].sections[0].baseAddress = 0x123;
    mapFromDevice(mr.tables, cfg);

    CHECK(cfg.bus[0].sections.size() == 2);
    if (cfg.bus[0].sections.size() != 2)
        return;
    // THE USER'S OWN NAME IS STILL ON IT. Asking for it by that name is half the
    // assertion: a run that renamed it to "Hidden message 1" fails here, at the
    // named lookup, rather than several lines later on a key comparison.
    const CommsSection *back = sectionNamed(cfg, 0, QStringLiteral("Turbo pressure"));
    const CommsSection *same = sectionNamed(cfg, 0, QStringLiteral("Oil temperature"));
    if (!back || !same)
        return;
    // The renumbered one is matched by PAYLOAD — the channels it carries — because
    // neither its wire identity nor either name means anything to the other side
    // once the address has moved and the name was never the generated one.
    CHECK(back->messageKey == movedKey);
    CHECK(back->protection == CommsProtection::Hidden);
    CHECK(!cfg.isSectionRevealed(*back));
    // Still concealed, and still not lowerable by someone who has proved nothing —
    // but no longer LOCKED, which is the difference this test exists for. Its own
    // password opens it again, and the section is the document's to unmark.
    CHECK(!cfg.maySectionLower(*back));
    cfg.grantSectionAccess(0, *back);
    CHECK(cfg.maySectionLower(*sectionNamed(cfg, 0, QStringLiteral("Turbo pressure"))));
    cfg.revokeSectionAccess(0, QStringLiteral("Turbo pressure"));
    // The one that did not move is still matched by IDENTITY, and it kept its
    // OWN key rather than the other one's — the fallbacks must not be able to
    // re-hand a record the identity pass already consumed.
    CHECK(same->messageKey == stayedKey);
    CHECK(!cfg.isSectionRevealed(*same));
    // Both are still concealing, so the document still refuses to save in clear.
    CHECK(cfg.anySectionConcealed());

    // A section this document has NEVER seen still comes back keyless, concealed
    // and honestly labelled. The fallbacks match records the document holds; they
    // do not invent a password for a message that arrived from somebody else's
    // hardware. This is the bug the user reported, checked from the other end.
    Configuration fresh;
    mapFromDevice(mr.tables, fresh);
    CHECK(fresh.bus[0].sections.size() == 2);
    for (const CommsSection &s : fresh.bus[0].sections) {
        CHECK(s.messageKey == kNoAccessKey);
        CHECK(s.protection == CommsProtection::Hidden);
        CHECK(!fresh.isSectionRevealed(s));
        CHECK(!fresh.maySectionLower(s));
        // Neutrally named, so the sections list cannot print the CAN ID the tier
        // exists to withhold.
        CHECK(s.name.startsWith(QLatin1String("Hidden message ")));
    }
}

// The two WEAKER handles the same second pass falls back on, each isolated by a
// fixture the other cannot answer. Both are relays, because a relay carries no
// channels at all — so the payload handle is empty for it and cannot quietly
// stand in for the handle each half is actually about.
//
//   Read Only relay   renumbered, still under the name the last Get gave it. The
//                     section's OWN name is the match, which is only possible
//                     because the name is now looked up BEFORE anything is
//                     assigned. The old order overwrote it first — but only at
//                     the concealing tiers, so this half would have passed then
//                     as well; it is here to hold the ordering down from the side
//                     where nothing is renamed at all.
//   Hidden relay      renumbered, under the name mapFromDevice generates for a
//                     concealing section. Both sides converge on "Hidden message
//                     1", which is the ONE case the old neutral-name-first
//                     ordering got right, and it must keep working: a relay has
//                     no payload, so this is the only fallback left for it.
static void testGetKeepsThePasswordForAGeneratedName()
{
    const AccessKey visibleKey = deriveAccessKey(QStringLiteral("relay-visible"));
    const AccessKey concealedKey = deriveAccessKey(QStringLiteral("relay-concealed"));

    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[1].enabled = true;
    const auto relay = [](const QString &name, quint32 id, CommsProtection tier) {
        CommsSection s;
        s.name = name;
        s.device = SectionDevice::MessageRelay;
        s.baseAddress = id;
        s.protection = tier;
        s.relayBitmask = 0x7FF;
        s.routeBusMask = (1 << 1); // forward to CAN 2; the source bus is excluded
        return s;
    };
    CommsSection visible = relay(QStringLiteral("Relay 0x300"), 0x300, CommsProtection::ReadOnly);
    visible.messageKey = visibleKey;
    CommsSection concealed =
        relay(QStringLiteral("Hidden message 1"), 0x301, CommsProtection::Hidden);
    concealed.messageKey = concealedKey;
    cfg.bus[0].sections.append(visible);
    cfg.bus[0].sections.append(concealed);

    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    if (!mr.ok())
        return;

    // Both renumbered in the DOCUMENT, with the device still holding what it was
    // sent. Neither has a wire identity left to match.
    cfg.bus[0].sections[0].baseAddress = 0x333;
    cfg.bus[0].sections[1].baseAddress = 0x334;
    mapFromDevice(mr.tables, cfg);

    const CommsSection *v = sectionNamed(cfg, 0, QStringLiteral("Relay 0x300"));
    const CommsSection *h = sectionNamed(cfg, 0, QStringLiteral("Hidden message 1"));
    if (!v || !h)
        return;
    CHECK(v->messageKey == visibleKey);
    CHECK(v->protection == CommsProtection::ReadOnly);
    CHECK(h->messageKey == concealedKey);
    CHECK(h->protection == CommsProtection::Hidden);
    // Each got its OWN key. Two relays with no payload between them is exactly
    // the shape in which a fallback that guessed would guess wrong.
    CHECK(v->messageKey != h->messageKey);
}

static void testWireRoundTripPreservesTier()
{
    // mapToDevice -> mapFromDevice for receive, transmit AND relay. The relay
    // half closes a real bug: before 2.3.0 the mapper's relay branch had no
    // protection code at all, so a marked relay concealed in the GUI and reached
    // the device bare — after which a Get read it back as an ordinary rule and
    // the next Send stored it that way.
    struct Case {
        const char *name;
        SectionDevice device;
        quint32 id;
        CommsProtection tier;
    };
    const Case cases[] = {
        {"Rx None", SectionDevice::ReceiveMessage, 0x100, CommsProtection::None},
        {"Rx ReadOnly", SectionDevice::ReceiveMessage, 0x101, CommsProtection::ReadOnly},
        {"Rx Hidden", SectionDevice::ReceiveMessage, 0x102, CommsProtection::Hidden},
        {"Rx Protected", SectionDevice::ReceiveMessage, 0x103, CommsProtection::Protected},
        {"Tx ReadOnly", SectionDevice::TransmitMessage, 0x200, CommsProtection::ReadOnly},
        {"Tx Hidden", SectionDevice::TransmitMessage, 0x201, CommsProtection::Hidden},
        {"Tx Protected", SectionDevice::TransmitMessage, 0x202, CommsProtection::Protected},
        {"Relay ReadOnly", SectionDevice::MessageRelay, 0x300, CommsProtection::ReadOnly},
        {"Relay Hidden", SectionDevice::MessageRelay, 0x301, CommsProtection::Hidden},
        {"Relay Protected", SectionDevice::MessageRelay, 0x302, CommsProtection::Protected},
    };

    Configuration cfg;
    cfg.bus[0].enabled = true;
    for (const Case &k : cases) {
        // A channel of its own per message. Sharing one would make every
        // receive section write the same destination, which mapToDevice rejects
        // before it ever reaches the flags this test is about.
        const QString channel =
            k.device == SectionDevice::MessageRelay
                ? QString()
                : QStringLiteral("Value %1").arg(k.id, 0, 16);
        if (!channel.isEmpty()) {
            Channel c;
            c.name = channel;
            c.userDefined = true;
            cfg.catalog().addOrUpdateUserChannel(c);
        }
        CommsSection s = makeSection(QString::fromUtf8(k.name), k.id, k.tier, channel);
        s.device = k.device;
        // Intel byte order, so makeSection's start-bit-0/16-bit row fits an
        // 8-byte frame. Under the default (Motorola) a DBC start bit of 0 is the
        // signal's MSB and a 16-bit field runs off the front of the message,
        // which mapToDevice rejects — before it reaches any of the flags this
        // test is actually about.
        s.alignment = SectionAlignment::WordSwap;
        if (k.device == SectionDevice::MessageRelay) {
            s.relayBitmask = 0x7FF;
            s.routeBusMask = (1 << 1); // forward to CAN 2; source CAN 1 excluded
        }
        cfg.bus[0].sections.append(s);
    }

    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    CHECK(mr.tables.messages.size() == 7);
    CHECK(mr.tables.relays.size() == 3);

    // Outbound: the level is in the flags, and the retired key field is zero in
    // every record. The device scrubs `reserved` on the way in as well, but a
    // host that filled it would still be putting private data on a wire other
    // hosts read. One byte rather than four since store v10 claimed the other
    // three for Triggered transmit; none of these cases is triggered, so their
    // trigger fields read as the unset sentinel.
    for (const CanMessageConfig &m : mr.tables.messages) {
        bool matched = false;
        for (const Case &k : cases) {
            if (k.device == SectionDevice::MessageRelay || m.can_id != k.id)
                continue;
            matched = true;
            CHECK((m.flags & MSGPROT_MASK) == commsProtectionToWire(k.tier));
        }
        CHECK(matched);
        // No password slot: these sections carry markings but no key that is one
        // of the document's four, so there is nothing for the wire to point at.
        CHECK(m.password_slot == 0);
        CHECK(m.tx_trigger_flags == 0);
        CHECK(m.tx_trigger_cond == TX_TRIGGER_COND_NONE);
    }
    for (const RelayConfig &rl : mr.tables.relays) {
        bool matched = false;
        for (const Case &k : cases) {
            if (k.device != SectionDevice::MessageRelay || rl.address != k.id)
                continue;
            matched = true;
            CHECK((rl.flags & MSGPROT_MASK) == commsProtectionToWire(k.tier));
        }
        CHECK(matched);
        // The engine's own relay bits are untouched by the level, and vice
        // versa: they live in bits 0..2 and the level in bits 6..7.
        CHECK(rl.flags & RELAYFLAG_ACTIVE);
    }

    // Inbound. Names are rebuilt from the CAN id, so the tier is matched back by
    // address rather than by name.
    Configuration back;
    mapFromDevice(mr.tables, back);
    int seen = 0;
    for (const CommsSection &s : back.bus[0].sections)
        for (const Case &k : cases) {
            if (s.baseAddress != k.id)
                continue;
            if ((s.isRelay()) != (k.device == SectionDevice::MessageRelay))
                continue;
            ++seen;
            CHECK(s.protection == k.tier);
        }
    CHECK(seen == int(sizeof(cases) / sizeof(cases[0])));
}

// ----------------------------------------------------- the two channel rules

static void testChannelPredicateSplit()
{
    // One predicate became two because Read Only is VISIBLE and still LOCKED.
    // Driving a control from the concealment predicate would leave a Read Only
    // message's channels editable, and changing a channel's base resolution
    // silently changes what that message decodes to.
    const QString pass = QStringLiteral("channel-split-secret");

    Configuration cfg;
    cfg.bus[0].enabled = true;
    for (const char *n : {"Cal Value", "Hidden Value", "Open Value"}) {
        Channel c;
        c.name = QString::fromUtf8(n);
        c.userDefined = true;
        cfg.catalog().addOrUpdateUserChannel(c);
    }
    cfg.bus[0].sections.append(makeSection(QStringLiteral("Cal"), 0x100,
                                           CommsProtection::ReadOnly,
                                           QStringLiteral("Cal Value")));
    cfg.bus[0].sections.append(makeSection(QStringLiteral("Hid"), 0x200, CommsProtection::Hidden,
                                           QStringLiteral("Hidden Value")));
    cfg.bus[0].sections.append(makeSection(QStringLiteral("Open"), 0x300, CommsProtection::None,
                                           QStringLiteral("Open Value")));
    // Hidden answers to the SECTION's own password, so it needs one for there to
    // be anything to conceal. Without it the tier is open to everybody by
    // design, and this whole test would be measuring the wrong thing.
    cfg.bus[0].sections[1].messageKey = deriveAccessKey(QStringLiteral("hid-own"));
    CHECK(cfg.setCommsPassword(pass));
    cfg.concealProtectedComms();

    // ---- concealed ----
    // Read Only: the values show, the controls do not open. This is the pair
    // that had no coverage anywhere before 2.3.0.
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Cal Value")));
    CHECK(cfg.isChannelEditLocked(QStringLiteral("Cal Value")));
    // Hidden: both.
    CHECK(cfg.isChannelConcealed(QStringLiteral("Hidden Value")));
    CHECK(cfg.isChannelEditLocked(QStringLiteral("Hidden Value")));
    // Unmarked: neither. The point is to protect the protocol, not the outputs.
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Open Value")));
    CHECK(!cfg.isChannelEditLocked(QStringLiteral("Open Value")));
    // A name nothing carries is not protected by accident.
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Nothing Uses This")));
    CHECK(!cfg.isChannelEditLocked(QStringLiteral("Nothing Uses This")));
    // Case-insensitive, like the grids that look these up.
    CHECK(cfg.isChannelEditLocked(QStringLiteral("cal value")));
    CHECK(cfg.isChannelConcealed(QStringLiteral("hidden value")));
    // An empty name asks nothing and must not be answered "protected" — several
    // callers pass one for a blank grid row.
    CHECK(!cfg.isChannelConcealed(QString()));
    CHECK(!cfg.isChannelEditLocked(QString()));

    CHECK(cfg.concealedChannelNames() == QStringList{QStringLiteral("Hidden Value")});
    {
        const QStringList locked = cfg.editLockedChannelNames();
        CHECK(locked.size() == 2);
        CHECK(locked.contains(QStringLiteral("Cal Value")));
        CHECK(locked.contains(QStringLiteral("Hidden Value")));
        CHECK(!locked.contains(QStringLiteral("Open Value")));
    }

    // ---- revealed ----
    // The document's Protected Comms password reveals PROTECTED sections
    // and nothing else. It is not a master key over Hidden, so "Hidden Value" is
    // still withheld with it given — this is hole A, which made the Hidden tier
    // inert on precisely the documents whose sections carry their own passwords.
    CHECK(cfg.revealProtectedComms(pass));
    CHECK(cfg.commsRevealed());
    CHECK(cfg.isChannelConcealed(QStringLiteral("Hidden Value")));
    CHECK(cfg.concealedChannelNames() == QStringList{QStringLiteral("Hidden Value")});

    // The SECTION's own password is what lifts it, and it lifts CONCEALMENT and
    // nothing else. The edit lock stays until the tier itself is lowered, because
    // a password buys viewing and the right to untick — not editing. This
    // asymmetry is the single most likely thing to be "fixed" by someone who has
    // not read DECISIONS.md.
    grantByName(cfg, 0, QStringLiteral("Hid"));
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Hidden Value")));
    CHECK(cfg.concealedChannelNames().isEmpty());
    CHECK(cfg.isChannelEditLocked(QStringLiteral("Hidden Value")));
    CHECK(cfg.isChannelEditLocked(QStringLiteral("Cal Value")));
    CHECK(cfg.editLockedChannelNames().size() == 2);

    // A document with only Read Only sections conceals nothing from anybody, so
    // it saves as a plain .ct3 freely. Before the split that was keyed off the
    // document-wide flag and a read-only-only document could not be saved.
    {
        Configuration ro;
        ro.bus[0].enabled = true;
        ro.bus[0].sections.append(
            makeSection(QStringLiteral("Cal"), 0x100, CommsProtection::ReadOnly, {}));
        CHECK(ro.setCommsPassword(QStringLiteral("read-only-only")));
        ro.concealProtectedComms();
        CHECK(!ro.commsRevealed());
        CHECK(!ro.anySectionConcealed());
        QTemporaryDir dir;
        CHECK(dir.isValid());
        QString err;
        CHECK(ro.saveToFile(dir.filePath(QStringLiteral("readonly.ct3")), &err));
    }
}

// A grant is session state that says "somebody typed THIS section's password",
// and it is matched by name. File > New clears it through clear(); File > Open
// loads over the LIVE document without one, so grants used to survive into a
// different file and unlock any section that happened to share a name. Two
// unrelated configurations both having an "Engine Data" is the normal case, not
// a contrived one — which is what made this worth a test of its own rather than
// a line in another.
static void testGrantsDoNotSurviveAnOpen()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("other.ct3"));

    // The file that gets opened: one Hidden section guarded by a password this
    // session has never been told.
    {
        Configuration other;
        other.bus[0].enabled = true;
        CommsSection s = makeSection(QStringLiteral("Engine Data"), 0x640,
                                     CommsProtection::Hidden, QStringLiteral("RPM"));
        s.messageKey = registerPassword(other, "other-document-password");
        other.bus[0].sections.append(s);
        // The author of the file has of course unlocked their own section — and
        // without this the save is refused, which is the plain-.ct3 guard doing
        // its job rather than a fault in the fixture.
        other.grantSectionAccess(0, s);
        QString err;
        CHECK(other.saveToFile(path, &err));
    }

    // The live document: same section NAME, a different password, and a grant
    // for it — the state a user is in after legitimately opening their own.
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection mine = makeSection(QStringLiteral("Engine Data"), 0x200,
                                    CommsProtection::Hidden, QStringLiteral("RPM"));
    mine.messageKey = registerPassword(cfg, "my-own-password");
    cfg.bus[0].sections.append(mine);
    grantByName(cfg, 0, QStringLiteral("Engine Data"));
    CHECK(grantedByName(cfg, 0, QStringLiteral("Engine Data")));

    QString err;
    CHECK(cfg.loadFromFile(path, &err));

    // The incoming section must be concealed and un-lowerable: nobody has proved
    // anything about it. Before the fix all three of these were the wrong way
    // round and the section printed its CAN ID in full.
    CHECK(!grantedByName(cfg, 0, QStringLiteral("Engine Data")));
    CHECK(cfg.bus[0].sections.size() == 1);
    const CommsSection &loaded = cfg.bus[0].sections.first();
    CHECK(loaded.protection == CommsProtection::Hidden);
    CHECK(!cfg.isSectionRevealed(loaded));
    CHECK(!cfg.maySectionLower(loaded));
    CHECK(cfg.anySectionConcealed());
    // The section's own password came through the load intact, so what was
    // cleared is the grant and not the ability to open the section at all. This
    // is the check the dialogs perform before calling grantSectionAccess.
    CHECK(loaded.messageKey == deriveAccessKey(QStringLiteral("other-document-password")));
    cfg.grantSectionAccess(0, loaded);
    CHECK(cfg.isSectionRevealed(loaded));
    CHECK(cfg.maySectionLower(loaded));
}

// ===========================================================================
//                      the dialogs, driven for real
// ===========================================================================
//
// Rules 1 and 2 are enforced in SectionEditorDialog and rule 3's re-conceal in
// CommunicationsDialog, and a model test cannot reach either. That is by
// design rather than by accident: Configuration deliberately does not know
// WHICH challenges were run, only that they were (see the contract on
// grantSectionAccess), so "the editor refuses to close over an empty password
// field" is not a statement about the model at all. Everything below therefore
// goes through the real widgets and their real OK paths — a box is ticked, a
// password is typed, the button is pressed.

// Every challenge in those two dialogs is a MODAL dialog with an event loop of
// its own, so the click that raises one does not return until something has
// answered it. This is the thing that answers.
//
// A REPEATING timer, not a single shot, because the loops nest: ticking Protect
// Communication raises a password prompt, then a device refusal, then an
// advisory, one inside the next. A shot armed before the click answers the
// first and leaves the second blocking for ever, which shows up as a hung run
// rather than as a failure.
class ModalPilot : public QObject
{
public:
    ModalPilot()
    {
        m_timer.setInterval(1);
        QObject::connect(&m_timer, &QTimer::timeout, this, &ModalPilot::pump);
        m_timer.start();
    }

    // Answers handed to successive password prompts, in order. An EXHAUSTED
    // script CANCELS rather than repeating the last answer, and that matters:
    // both prompts loop until the password is right or the user gives up, so a
    // pilot that kept retyping a wrong one would hang the run instead of
    // failing it. Cancelling is also how "the user did not know it" is spelled.
    QStringList passwords;
    // Drives a section editor that some other dialog has exec()'d. Called once
    // per editor; whatever it does may raise more modals of its own, which the
    // re-entrant pump answers from `passwords` exactly as it would any other.
    std::function<void(SectionEditorDialog &)> onEditor;

    int prompts = 0;  // password challenges that actually RAN
    int messages = 0; // refusals and advisories
    int editors = 0;  // section editors that reached exec()
    QString transcript;

    bool said(const char *needle) const
    {
        return transcript.contains(QLatin1String(needle), Qt::CaseInsensitive);
    }
    void reset()
    {
        prompts = 0;
        messages = 0;
        editors = 0;
        transcript.clear();
    }

private:
    void pump()
    {
        QWidget *top = QApplication::activeModalWidget();
        if (!top)
            return;
        if (auto *input = qobject_cast<QInputDialog *>(top)) {
            ++prompts;
            if (passwords.isEmpty()) {
                input->reject();
                return;
            }
            input->setTextValue(passwords.takeFirst());
            input->accept();
            return;
        }
        if (auto *box = qobject_cast<QMessageBox *>(top)) {
            ++messages;
            transcript += box->text() + QLatin1Char('\n');
            // Clicked rather than accept()ed: QMessageBox reimplements done()
            // around its button ROLES, and the button is what a user has.
            if (QAbstractButton *b = box->defaultButton())
                b->click();
            else if (QAbstractButton *b = box->button(QMessageBox::Ok))
                b->click();
            else
                box->accept();
            return;
        }
        if (auto *editor = qobject_cast<SectionEditorDialog *>(top)) {
            // Once per editor. The callback's own modals re-enter this function,
            // and without the latch each of those re-entries would find the same
            // editor still on the stack and start the callback again.
            if (m_drivenEditor.data() == editor) {
                // AN EDITOR NOBODY IS DRIVING IS A HANG, AND A HANG IS THE ONE
                // FAILURE SHAPE THIS SUITE CANNOT REPORT. exec() does not return
                // until something closes the dialog, so a run that opens an editor
                // no test scripted — or whose script pressed OK and had it refused
                // — sat here for ever, and a build-timeout kill takes every
                // buffered FAIL line with it. That is exactly what happened under
                // a mutation that reopened keyless marked sections: eighteen
                // failures printed, then silence.
                //
                // Reached only once the script has RUN AND RETURNED (or there was
                // none), so an editor legitimately sitting under its own nested
                // password prompt is untouched — during that, the prompt is the
                // active modal and this branch is not the one being taken.
                if (!m_scriptDone)
                    return;
                std::printf("FAIL %s: a section editor was left open with nothing to close it\n",
                            __FILE__);
                std::fflush(stdout);
                ++failures;
                editor->reject();
                return;
            }
            m_drivenEditor = editor;
            m_scriptDone = false;
            ++editors;
            if (!onEditor) {
                // No script at all. Every test that expects an editor supplies
                // one, so this is a test that expected NO editor and got one —
                // counted above, closed below on the next tick, and asserted on by
                // whoever cared.
                m_scriptDone = true;
                return;
            }
            // QUEUED, and it has to be. Qt will not deliver a timer's timeout
            // while that same timer's handler is still on the stack (the event
            // dispatcher's inTimerEvent guard), so anything the callback does
            // that spins an event loop of its own would never be answered from
            // here — and unticking a marking spins one, for the password prompt.
            // That failure is a HANG rather than a failed check, which is the
            // worst shape a test can take. Queueing lets this handler return
            // first, so the callback runs from the editor's own loop with the
            // timer free to fire again underneath it.
            QPointer<SectionEditorDialog> target(editor);
            QMetaObject::invokeMethod(
                this,
                [this, target] {
                    if (target && onEditor)
                        onEditor(*target);
                    // Set AFTER the callback returns, never before: the callback
                    // spins nested loops, and arming the force-close while it is
                    // still running would shut the editor out from under it.
                    m_scriptDone = true;
                },
                Qt::QueuedConnection);
        }
    }

    QTimer m_timer;
    QPointer<QWidget> m_drivenEditor;
    bool m_scriptDone = false;
};

// The widget lookups below never return null. A widget that cannot be found is
// reported as one named failure and answered with a detached stand-in, so the
// run carries on and prints every OTHER check: a renamed control that segfaulted
// here would take the rest of this file's output with it and say nothing about
// what it was looking for.
//
// The stand-in is built on first use — inside main(), where QApplication already
// exists — and then deliberately LEAKED. A function-local static QWidget would be
// destroyed after main() returns, which is after QApplication is gone, and
// destroying a widget then crashes: the failure path would take the exit code
// with it and report a segfault instead of the named check that failed.
template <typename W>
static W *orStandIn(W *found, const char *what)
{
    if (found)
        return found;
    std::printf("FAIL %s: no \"%s\" in this dialog\n", __FILE__, what);
    std::fflush(stdout);
    ++failures;
    static W *standIn = new W;
    return standIn;
}

// The three tier boxes, by the label a user reads. Found by text rather than by
// position because the ladder's ORDER is the thing several of these tests are
// about, and an index would quietly follow a reorder instead of noticing one.
//
// fromUtf8, never QLatin1String: these are translator-facing strings and the
// controls in these dialogs carry real typographic characters — an ellipsis in
// "Edit…", arrows in the move buttons — which a Latin-1 comparison silently
// fails to match rather than refusing to compile.
static QCheckBox *tierBox(QDialog &d, const char *label)
{
    for (QCheckBox *b : d.findChildren<QCheckBox *>())
        if (b->text().remove(QLatin1Char('&')) == QString::fromUtf8(label))
            return b;
    return orStandIn<QCheckBox>(nullptr, label);
}

// The Message Password field. The Parameters tab holds five line edits and
// their order is layout detail; this one is found by the property that DEFINES
// it — it hides what is typed into it — which cannot silently become true of
// the name or the base address.
static QLineEdit *passwordField(QDialog &d)
{
    for (QLineEdit *e : d.findChildren<QLineEdit *>())
        if (e->echoMode() == QLineEdit::Password)
            return e;
    return orStandIn<QLineEdit>(nullptr, "Message Password field");
}

// The Clear button beside the Message Password field. Matched on its label, so
// fromUtf8 for the same reason tierBox needs it: the text carries a real
// ellipsis, not three dots.
static QPushButton *clearPasswordButton(QDialog &d)
{
    for (QPushButton *b : d.findChildren<QPushButton *>())
        if (b->text().remove(QLatin1Char('&')) == QString::fromUtf8("Clear…"))
            return b;
    return orStandIn<QPushButton>(nullptr, "Clear button");
}

static void clickStandard(QDialog &d, QDialogButtonBox::StandardButton which)
{
    auto *box = d.findChild<QDialogButtonBox *>();
    CHECK(box != nullptr);
    if (!box)
        return;
    QPushButton *b = box->button(which);
    CHECK(b != nullptr);
    if (b)
        b->click();
}

// A section editor primed the way Communications Setup primes one, plus the
// bookkeeping every test below repeats: a live "did OK actually work?" flag.
// accepted() is emitted by QDialog::accept(), which SectionEditorDialog::accept()
// reaches only after every refusal above it has been passed, so watching it asks
// exactly the question rule 1 is about.
struct EditorHarness {
    EditorHarness(Configuration *cfg, const CommsSection &s, ProtectedCommsProver prover = {})
        : dialog(cfg, s, 0, ConfigPatch{}, 0, nullptr, std::move(prover))
    {
        QObject::connect(&dialog, &QDialog::accepted, &dialog, [this] { accepted = true; });
    }
    SectionEditorDialog dialog;
    bool accepted = false;
};

// ------------------------------------------------------------------ rule 1

// RULE 1, all three tiers: A MARKING WITH NO PASSWORD CANNOT BE CREATED.
//
// This used to be a rule about what may LEAVE the dialog - a marked section with
// an empty password field was refused at OK. Store v16 moved the enforcement
// earlier and made it stricter: a marking names one of the document's four
// message passwords, so the editor asks at the TICK and accepts only an answer
// that matches one of them. The invalid state is no longer created and then
// refused; it cannot be reached.
//
// Protect Communication is included deliberately - its device round trip is an
// ADDITION to the section password, not a replacement for it - and this test
// isolates that by handing it a prover that says yes, so the device half is
// already given and the section password is the only thing still missing.
static void testEditorRefusesEveryMarkedTierWithNoPassword()
{
    struct Case {
        const char *label;
        CommsProtection tier;
    };
    const Case cases[] = {
        {"Read Only", CommsProtection::ReadOnly},
        {"Hidden", CommsProtection::Hidden},
        {"Protect Communication", CommsProtection::Protected},
    };

    for (const Case &k : cases) {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        // Named, and the name does not contain the CAN ID: ticking Hidden or
        // Protected on a section whose name discloses its address raises a
        // SECOND advisory, and this test counts the password one.
        const CommsSection s =
            makeSection(QStringLiteral("Calibration"), 0x640, CommsProtection::None, {});
        cfg.bus[0].sections.append(s);

        // NO PASSWORDS AT ALL IS ITS OWN ANSWER, and not a password prompt. A
        // configuration with none has nothing a marking could name, so sending
        // the user to a prompt they cannot possibly satisfy would be worse than
        // telling them where the passwords are set.
        {
            ModalPilot pilot;
            EditorHarness h(&cfg, s, [] { return true; });
            QCheckBox *box = tierBox(h.dialog, k.label);
            box->setChecked(true);
            CHECK(pilot.prompts == 0); // not asked
            CHECK(pilot.said("Passwords tab"));
            CHECK(!box->isChecked()); // and the tick was given back
        }

        // From here the document HAS one, so the prompt is the right answer.
        const AccessKey slotKey = registerPassword(cfg, "section-own-password");

        // CANCELLING COSTS THE MARKING. A box that stayed ticked after the user
        // backed out would be a marked section with nothing guarding it,
        // created by pressing Cancel.
        {
            ModalPilot pilot; // no passwords queued: the prompt is rejected
            EditorHarness h(&cfg, s, [] { return true; });
            QCheckBox *box = tierBox(h.dialog, k.label);
            box->setChecked(true);
            CHECK(pilot.prompts == 1); // asked at the tick, not deferred to OK
            CHECK(!box->isChecked());  // and the tick was given back
        }

        // A PASSWORD THAT IS NOT ONE OF THE FOUR IS REFUSED AND RE-ASKED, so a
        // mistyped one does not cost the marking either. Two answers are queued:
        // a wrong one, then the real one.
        {
            ModalPilot pilot;
            pilot.passwords << QStringLiteral("not-one-of-the-four")
                            << QStringLiteral("section-own-password");
            EditorHarness h(&cfg, s, [] { return true; });
            QCheckBox *box = tierBox(h.dialog, k.label);
            box->setChecked(true);
            CHECK(pilot.prompts == 2); // asked again rather than given up on
            CHECK(box->isChecked());
        }

        // Supplied, and the marking takes.
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("section-own-password");
        EditorHarness h(&cfg, s, [] { return true; });
        QCheckBox *box = tierBox(h.dialog, k.label);
        box->setChecked(true);
        CHECK(pilot.prompts == 1);
        CHECK(box->isChecked());

        // ONE BOX, NOT THREE. The tiers used to nest on screen, so Protected
        // showed Read Only and Hidden ticked as well.
        for (const Case &other : cases)
            if (other.tier != k.tier)
                CHECK(!tierBox(h.dialog, other.label)->isChecked());

        pilot.reset();
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(pilot.messages == 0);
        CHECK(h.dialog.section().protection == k.tier);
        // The key is the SLOT's, not one invented for this section.
        CHECK(h.dialog.section().messageKey == slotKey);
        CHECK(cfg.commsPasswordSlotFor(h.dialog.section().messageKey) != 0);
    }
}

// THE SEAM, and it is the one place in this feature where two rules that look
// contradictory both have to hold. A marked section with NO password is CONCEALED
// and not lowerable (Configuration::isSectionRevealed, maySectionLower): nothing
// exists that could open it, so nothing is shown to anybody. What 2.3.1 adds on
// top is that you can no longer LEAVE THE EDITOR having created or kept one, so
// the only documents this state can reach the user in are the ones that predate
// the rule and the ones a Get produces — the wire carries reserved[4] and no key.
//
// This heading used to say the opposite: that such a section is OPEN, "because
// that is what stops every file written before 2.3.1 turning into a brick nobody
// can open". That was the reasoning 2.3.2 reversed — it is what shipped the
// user's bug, since every section a Get returns is exactly this section — and it
// sat directly above a function asserting the reverse. Nothing is bricked: the
// message can always be removed, and at Read Only it can be repaired outright
// (the last block below drives that end to end).
//
// Read either half alone and the other looks like a bug, which is exactly why
// both are pinned here, in one function, rather than in two that could be
// "reconciled" by someone tidying up.
static void testKeylessMarkedSectionIsConcealedAndCannotBeKept()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].sections.append(
        makeSection(QStringLiteral("Imported"), 0x640, CommsProtection::Hidden, {}));
    const CommsSection keyless = cfg.bus[0].sections.first();
    CHECK(keyless.messageKey == kNoAccessKey);

    // Half one, and it is the reverse of what this function asserted until
    // 2.3.2: CONCEALED, not lowerable, and the document reports it as withheld.
    // It used to read "still visible, still lowerable ... because nothing is
    // being concealed from anybody", which is the reasoning that shipped the
    // user's bug — every section a Get produces is exactly this section.
    CHECK(!cfg.isSectionRevealed(keyless));
    CHECK(!cfg.maySectionLower(keyless));
    CHECK(cfg.anySectionConcealed());
    // ...but NOT keyed-concealed, which is what keeps the file writers usable.
    CHECK(!cfg.anyKeyedSectionConcealed());

    // Half two: the editor will not close over it. Opened and OK'd with nothing
    // touched at all — the migrated-file case, and the commonest one.
    {
        ModalPilot pilot;
        EditorHarness h(&cfg, keyless);
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(!h.accepted);
        CHECK(pilot.said("no Message Password"));
        // Nothing was asked for on the way IN. There is no password on this
        // section, so there was nothing to prove — that is the keyless rule, and
        // a prompt here would mean the editor had started demanding one.
        CHECK(pilot.prompts == 0);
    }

    // Cancel still works from that state, and it has to: someone who opens a
    // keyless marked section only to look at it must be able to leave. This is
    // why rule 1 lives in accept() and in no toggled() handler.
    {
        ModalPilot pilot;
        EditorHarness h(&cfg, keyless);
        clickStandard(h.dialog, QDialogButtonBox::Cancel);
        CHECK(!h.accepted);
        CHECK(pilot.messages == 0);
    }

    // THE REPAIR IS OFFERED AT THE UNTICK, because there is nowhere else it
    // could be: the Message Password field is gone, and a keyless marking
    // cannot pay for its own release. Unticking asks "give it a Message
    // Password now?" - Yes runs the same prompt a new marking uses, which
    // accepts only one of the document's four. The box stays ticked either
    // way; the marking is released on the NEXT visit, with the password.
    {
        registerPassword(cfg, "now-it-has-one"); // the prompt matches the four
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("now-it-has-one");
        EditorHarness h(&cfg, keyless);
        tierBox(h.dialog, "Hidden")->setChecked(false);
        CHECK(pilot.prompts == 1);                       // the offer was taken
        CHECK(tierBox(h.dialog, "Hidden")->isChecked()); // marking NOT released
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::Hidden);
        CHECK(h.dialog.section().messageKey
              == deriveAccessKey(QStringLiteral("now-it-has-one")));
    }

    // The refusal used to name a SECOND way out — "or untick the box" — and that
    // way out is closed. An untick is authorised by the section's own password,
    // and Configuration::maySectionLower fails closed when there is none, so a
    // dialog that let the box come off would only be arranging for the model to
    // refuse the whole commit later, in another window, with unrelated edits
    // riding on the same OK. The dialog refuses it here, where it can explain.
    {
        ModalPilot pilot;
        EditorHarness h(&cfg, keyless);
        tierBox(h.dialog, "Hidden")->setChecked(false);
        // The box goes straight back: nothing moved. Checked from BOTH ends now
        // that the boxes are exclusive — "Hidden is still ticked" alone would not
        // rule out a second box having appeared beside it.
        CHECK(tierBox(h.dialog, "Hidden")->isChecked());
        CHECK(!tierBox(h.dialog, "Read Only")->isChecked());
        CHECK(!tierBox(h.dialog, "Protect Communication")->isChecked());
        CHECK(pilot.said("no Message Password"));
        // The question box (Yes clicked by the pilot) led to the password prompt,
        // and this pilot has nothing queued, so the prompt was cancelled. Declining
        // the offered repair leaves everything exactly as it was.
        CHECK(pilot.prompts == 1); // offered, declined
        clickStandard(h.dialog, QDialogButtonBox::Cancel);
        CHECK(!h.accepted);
    }

    // RAISING TO ANOTHER MARKING IS NOT FREE, because a message wears one
    // marking and swapping it means releasing the current one first. The refusal
    // is the same one every marked section gets, and the door it names - untick
    // the ticked box - genuinely opens for a keyless section too: unticking is
    // where the repair is offered, so the path is refusal, untick, take the
    // offer, OK, reopen, release, re-mark. Longer than a keyed section's, but
    // every step is offered on screen rather than discovered.
    {
        ModalPilot pilot;
        EditorHarness h(&cfg, keyless, [] { return true; });
        tierBox(h.dialog, "Protect Communication")->setChecked(true);
        CHECK(!tierBox(h.dialog, "Protect Communication")->isChecked());
        CHECK(tierBox(h.dialog, "Hidden")->isChecked()); // nothing moved
        CHECK(pilot.prompts == 0); // the guard refuses before any password talk
        CHECK(pilot.said("release Current Message Protection"));
        clickStandard(h.dialog, QDialogButtonBox::Cancel);
        CHECK(!h.accepted);
    }

    // THE REPAIR, end to end and through the real dialog: a first password is
    // free, and the untick then has something to be authorised by on the next
    // visit. Two visits, deliberately — applyBusSections asks about the section
    // AS IT STANDS IN THE DOCUMENT, and while the editor is open that is still
    // the keyless one.
    {
        Configuration doc;
        doc.bus[0].enabled = true;
        doc.bus[0].sections.append(
            makeSection(QStringLiteral("Imported"), 0x640, CommsProtection::Hidden, {}));

        // The password the repair will offer has to be one of the document's
        // four - the prompt matches, it does not invent.
        registerPassword(doc, "adopted-by-me");

        // One pilot at a time, and each in its own scope. Two alive together both
        // answer the same modal, and the one with no script CANCELS it - which
        // would make the second visit below fail as though the ladder had refused
        // a password it was never given.
        //
        // FIRST VISIT: the untick triggers the repair offer (a question box the
        // pilot answers Yes), the prompt takes the password, and the marking
        // deliberately does NOT come off - the model can only be asked about the
        // section as the DOCUMENT holds it, which is still the keyless one.
        {
            ModalPilot pilot;
            pilot.passwords << QStringLiteral("adopted-by-me");
            EditorHarness h(&doc, doc.bus[0].sections.first());
            tierBox(h.dialog, "Hidden")->setChecked(false);
            CHECK(pilot.prompts == 1);
            CHECK(tierBox(h.dialog, "Hidden")->isChecked()); // still marked
            clickStandard(h.dialog, QDialogButtonBox::Ok);
            CHECK(h.accepted);
            QList<CommsSection> next = doc.bus[0].sections;
            next[0] = h.dialog.section();
            QString refusal;
            CHECK(doc.applyBusSections(0, next, &refusal)); // a FIRST key is an addition
            CHECK(refusal.isEmpty());
            CHECK(doc.bus[0].sections[0].messageKey
                  == deriveAccessKey(QStringLiteral("adopted-by-me")));
        }

        // Second visit: the password now exists, so the ladder asks for it and
        // the untick goes through.
        {
            ModalPilot pilot;
            pilot.passwords << QStringLiteral("adopted-by-me");
            EditorHarness h(&doc, doc.bus[0].sections.first());
            tierBox(h.dialog, "Hidden")->setChecked(false);
            CHECK(pilot.prompts == 1);
            CHECK(!tierBox(h.dialog, "Hidden")->isChecked());
            tierBox(h.dialog, "Read Only")->setChecked(false);
            clickStandard(h.dialog, QDialogButtonBox::Ok);
            CHECK(h.accepted);
            CHECK(h.dialog.section().protection == CommsProtection::None);
        }
    }
}

// ------------------------------------------------------------------ rule 2

// RULE 2, RAISING — the case the user names, and the one that used to be free.
// Read Only's whole promise is "this needs my password to change", and before
// 2.3.1 anyone holding the file could walk it up to Hidden or Protected without
// being asked for anything.
static void testEditorRaiseNeedsTheOldPasswordThenANewOne()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection s = makeSection(QStringLiteral("Calibration"), 0x640,
                                 CommsProtection::ReadOnly, {});
    s.messageKey = registerPassword(cfg, "read-only-password");
    cfg.bus[0].sections.append(s);
    // A MARKING IS NOT MOVED SIDEWAYS. Ticking a second box over one that is
    // already set is refused outright, and refused BEFORE any password is
    // asked for: there is nothing to prove yet, because nothing is being given
    // up. The refusal names the way through - release, then re-mark.
    {
        ModalPilot pilot;
        EditorHarness h(&cfg, s);
        tierBox(h.dialog, "Hidden")->setChecked(true);
        CHECK(pilot.prompts == 0);   // not asked for a password it cannot use
        CHECK(pilot.said("release Current Message Protection"));
        CHECK(!tierBox(h.dialog, "Hidden")->isChecked());
        CHECK(tierBox(h.dialog, "Read Only")->isChecked()); // nothing moved
    }

    // RELEASING IS WHAT COSTS THE OLD PASSWORD, and not knowing it stops the
    // move dead. The prompt RAN and the marking did not go - both halves
    // matter, because a gate that never asks and a gate that asks and ignores
    // the answer fail differently.
    {
        ModalPilot pilot; // no answers: the user gives up at the prompt
        EditorHarness h(&cfg, s);
        tierBox(h.dialog, "Read Only")->setChecked(false);
        CHECK(pilot.prompts == 1);
        CHECK(tierBox(h.dialog, "Read Only")->isChecked()); // put back
    }

    // The whole journey: release with the right password (after a wrong try),
    // then mark it Hidden with a DIFFERENT one of the four. The old key does
    // NOT come along - that is the property this test has always been about,
    // and it is structural now: the new marking's prompt asks afresh, and
    // authoriseTierChange clears the pending key on a tier change.
    registerPassword(cfg, "hidden-password"); // the second of the four
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("not-the-one")
                        << QStringLiteral("read-only-password")
                        << QStringLiteral("hidden-password");
        EditorHarness h(&cfg, s);

        tierBox(h.dialog, "Read Only")->setChecked(false); // wrong, then right
        CHECK(pilot.prompts == 2);
        CHECK(!tierBox(h.dialog, "Read Only")->isChecked()); // unmarked now

        tierBox(h.dialog, "Hidden")->setChecked(true);   // asks for the new one
        CHECK(pilot.prompts == 3);
        CHECK(tierBox(h.dialog, "Hidden")->isChecked());
        CHECK(!tierBox(h.dialog, "Read Only")->isChecked()); // one box, not two

        pilot.reset();
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(pilot.messages == 0);
        CHECK(h.dialog.section().protection == CommsProtection::Hidden);
        CHECK(h.dialog.section().messageKey == deriveAccessKey(QStringLiteral("hidden-password")));
        CHECK(h.dialog.section().messageKey
              != deriveAccessKey(QStringLiteral("read-only-password")));
    }

    // Unticking and re-ticking the SAME box costs nothing more and leaves the
    // stored key alone: the proof is about leaving the marking the section
    // arrived on, and it has been given. Without this the only way out of a
    // mistaken untick would be Cancel, which throws away every other edit too.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("read-only-password");
        EditorHarness h(&cfg, s);
        tierBox(h.dialog, "Read Only")->setChecked(false); // give it up
        CHECK(pilot.prompts == 1);
        tierBox(h.dialog, "Read Only")->setChecked(true);  // and take it back
        CHECK(pilot.prompts == 1);                         // not interrogated twice
        pilot.reset();
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(pilot.messages == 0);
        CHECK(h.dialog.section().protection == CommsProtection::ReadOnly);
        CHECK(h.dialog.section().messageKey == s.messageKey); // untouched
    }
}

// RULE 2, LOWERING — the behaviour that already shipped, pinned here so that
// unifying the two directions onto one path cannot quietly drop it.
// TICKING PROTECT COMMUNICATION ALWAYS ASKS FOR THE PASSWORD, and a password
// chosen for another marking never carries onto it. Found by hand: choose a
// password for Read Only, untick it (which is free for a marking taken this
// visit), tick Protect Communication - and the pending key slid across without
// a prompt, so the strongest marking took a password nobody stated for it.
static void testProtectedAlwaysAsksAndNothingCarries()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    const CommsSection s =
        makeSection(QStringLiteral("Calibration"), 0x640, CommsProtection::None, {});
    cfg.bus[0].sections.append(s);
    registerPassword(cfg, "first-password");
    const AccessKey secondKey = registerPassword(cfg, "second-password");

    // The carry-over: RO chosen, released, Protected ticked. The prompt MUST
    // run again - two prompts total for the RO tick and the Protected tick -
    // and the key applied is the one stated for Protected.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("first-password")
                        << QStringLiteral("second-password");
        EditorHarness h(&cfg, s, [] { return true; });
        tierBox(h.dialog, "Read Only")->setChecked(true);
        CHECK(pilot.prompts == 1);
        tierBox(h.dialog, "Read Only")->setChecked(false); // free: taken this visit
        CHECK(pilot.prompts == 1);
        tierBox(h.dialog, "Protect Communication")->setChecked(true);
        CHECK(pilot.prompts == 2); // asked afresh, no silent carry
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::Protected);
        CHECK(h.dialog.section().messageKey == secondKey);
        CHECK(h.dialog.section().messageKey
              != deriveAccessKey(QStringLiteral("first-password")));
    }

    // THE PATH THE USER ACTUALLY HIT: a section that ARRIVED Protected,
    // unticked (proving its password and the device), then re-ticked. For the
    // lesser tiers that change of mind is free and reuses the stored key; for
    // Protect Communication the tick states the password every time. Without
    // this, re-ticking restored the strongest marking with no popup at all.
    {
        Configuration cfg2;
        cfg2.bus[0].enabled = true;
        CommsSection p = makeSection(QStringLiteral("Driveline"), 0x641,
                                     CommsProtection::Protected, {});
        p.messageKey = registerPassword(cfg2, "protected-password");
        cfg2.bus[0].sections.append(p);

        ModalPilot pilot;
        pilot.passwords << QStringLiteral("protected-password")   // the untick
                        << QStringLiteral("protected-password");  // the re-tick
        EditorHarness h(&cfg2, p, [] { return true; });
        tierBox(h.dialog, "Protect Communication")->setChecked(false);
        CHECK(pilot.prompts == 1);
        tierBox(h.dialog, "Protect Communication")->setChecked(true);
        CHECK(pilot.prompts == 2); // asked again - Protected always asks
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::Protected);
    }

    // And with the prompt CANCELLED at the Protected tick, nothing of the
    // released password survives to be applied: the section leaves unmarked
    // and keyless.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("first-password");
        EditorHarness h(&cfg, s, [] { return true; });
        tierBox(h.dialog, "Read Only")->setChecked(true);
        tierBox(h.dialog, "Read Only")->setChecked(false);
        tierBox(h.dialog, "Protect Communication")->setChecked(true); // no answer left
        CHECK(!tierBox(h.dialog, "Protect Communication")->isChecked());
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::None);
        CHECK(h.dialog.section().messageKey == kNoAccessKey);
    }
}

// A RELEASED PLACEHOLDER NAME REGENERATES. A Get into a fresh document names a
// concealing section "Hidden message 1" so the list does not spell out the CAN
// ID the marking withholds. Unhide it, and that name is no longer protecting
// anything - it is just wrong, and it stayed. It now falls back to the ordinary
// generated name the moment the marking stops concealing.
static void testReleasedPlaceholderNameRegenerates()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection s = makeSection(QStringLiteral("Hidden message 1"), 0x640,
                                 CommsProtection::Hidden, {});
    s.messageKey = registerPassword(cfg, "hidden-password");
    cfg.bus[0].sections.append(s);

    // Released: the placeholder becomes the generated name, ID included - the
    // marking that made the ID sensitive is gone.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("hidden-password");
        EditorHarness h(&cfg, s);
        tierBox(h.dialog, "Hidden")->setChecked(false);
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::None);
        CHECK(h.dialog.section().name == QStringLiteral("Receive 0x640"));
    }

    // Still marked: the placeholder stays - it is the concealment working.
    {
        ModalPilot pilot;
        EditorHarness h(&cfg, s);
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().name == QStringLiteral("Hidden message 1"));
    }

    // A REAL name is never touched by a release. "Driveline" is not a
    // placeholder, and renaming a user's message would be worse than the bug.
    {
        Configuration cfg2;
        cfg2.bus[0].enabled = true;
        CommsSection mine = makeSection(QStringLiteral("Driveline"), 0x641,
                                        CommsProtection::Hidden, {});
        mine.messageKey = registerPassword(cfg2, "hidden-password");
        cfg2.bus[0].sections.append(mine);
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("hidden-password");
        EditorHarness h(&cfg2, mine);
        tierBox(h.dialog, "Hidden")->setChecked(false);
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().name == QStringLiteral("Driveline"));
    }
}

static void testEditorLowerStillNeedsTheSectionPassword()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection s =
        makeSection(QStringLiteral("Turbo pressure"), 0x300, CommsProtection::Hidden, {});
    s.messageKey = registerPassword(cfg, "hidden-password");
    cfg.bus[0].sections.append(s);

    {
        ModalPilot pilot; // the user gives up
        EditorHarness h(&cfg, s);
        tierBox(h.dialog, "Hidden")->setChecked(false);
        CHECK(pilot.prompts == 1);
        CHECK(tierBox(h.dialog, "Hidden")->isChecked()); // put back
    }
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("hidden-password");
        EditorHarness h(&cfg, s);
        tierBox(h.dialog, "Hidden")->setChecked(false);
        CHECK(pilot.prompts == 1);
        // AN UNTICK LANDS UNMARKED, not on a lesser marking. The boxes used to
        // nest, so giving up Hidden left the message Read Only and still needing
        // a password chosen for THAT marking; now the marking is simply gone and
        // there is nothing left for a password to guard.
        CHECK(!tierBox(h.dialog, "Hidden")->isChecked());
        CHECK(!tierBox(h.dialog, "Read Only")->isChecked());
        CHECK(!tierBox(h.dialog, "Protect Communication")->isChecked());
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::None);
        // The stored key is deliberately NOT destroyed by an untick — see
        // syncParametersFromUi. On an unmarked section it guards nothing, which
        // is harmless and recoverable; Clear… is what removes it outright.
        CHECK(h.dialog.section().messageKey == s.messageKey);
    }

    registerPassword(cfg, "read-only-password"); // the second of the four

    // AND THE KEY STILL DOES NOT CARRY ONTO A NEW MARKING. That was the other
    // half of this test, and it survives the change — it is just reached by
    // taking a fresh marking rather than by landing on a leftover one.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("hidden-password")
                        << QStringLiteral("read-only-password");
        EditorHarness h(&cfg, s);
        tierBox(h.dialog, "Hidden")->setChecked(false);   // costs the old one
        tierBox(h.dialog, "Read Only")->setChecked(true); // asks for a new one
        CHECK(pilot.prompts == 2);
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::ReadOnly);
        CHECK(h.dialog.section().messageKey
              == deriveAccessKey(QStringLiteral("read-only-password")));
        CHECK(h.dialog.section().messageKey != s.messageKey);
    }
}

// RULE 2, THE OTHER WAY OFF THE SECTION - and the way is now CLOSED. This test
// used to pin that typing a NEW password over an existing one proved the old
// one first, because the Message Password field was a free-handover hole until
// 2.3.1 guarded it.
//
// Store v16 removed the field outright: a message names one of the document's
// four passwords, chosen through a prompt, and there is no per-section
// replacement surface left to guard. Changing which password guards a message
// is release-then-re-mark, each step costing its own password - the same
// no-free-handover property, now structural. The slot itself is guarded the
// same way at the Passwords tab: an in-use slot can be neither cleared nor
// replaced while anything names it.
static void testEditorReplacingAPasswordProvesTheOldOne()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection s = makeSection(QStringLiteral("Calibration"), 0x640,
                                 CommsProtection::ReadOnly, {});
    s.messageKey = registerPassword(cfg, "read-only-password");
    cfg.bus[0].sections.append(s);
    const AccessKey newKey = registerPassword(cfg, "a-new-password");

    // THE HOLE IS GONE WITH THE SURFACE: the editor holds no password field at
    // all, so there is nothing an attacker could type into. Asserted directly,
    // because this absence is the load-bearing part.
    {
        ModalPilot pilot;
        EditorHarness h(&cfg, s);
        int passwordEdits = 0;
        for (QLineEdit *e : h.dialog.findChildren<QLineEdit *>())
            if (e->echoMode() == QLineEdit::Password)
                ++passwordEdits;
        CHECK(passwordEdits == 0);
        // Nothing asked on the way in either - Read Only conceals nothing.
        CHECK(pilot.prompts == 0);
        clickStandard(h.dialog, QDialogButtonBox::Cancel);
    }

    // The owner changing their message's password still works: release with the
    // old one, re-mark with the new one. Two prompts, two different secrets, no
    // step free.
    // TWO VISITS, because within one the stored key still counts for the
    // marking the section arrived on: untick-and-retick the SAME box is the
    // free change-of-mind path and deliberately keeps the old key. Releasing,
    // committing, and re-marking is what actually changes hands.
    CommsSection released;
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("read-only-password");
        EditorHarness h(&cfg, s);
        tierBox(h.dialog, "Read Only")->setChecked(false); // proves the old
        CHECK(pilot.prompts == 1);
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        released = h.dialog.section();
        CHECK(released.protection == CommsProtection::None);
    }
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("a-new-password");
        EditorHarness h(&cfg, released);
        tierBox(h.dialog, "Read Only")->setChecked(true); // chooses the new
        CHECK(pilot.prompts == 1);
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::ReadOnly);
        CHECK(h.dialog.section().messageKey == newKey);
        CHECK(h.dialog.section().messageKey
              != deriveAccessKey(QStringLiteral("read-only-password")));
    }
}

// RULE 2, PROTECTED: both halves, and NEITHER of them alone. The section
// password is checked here against messageKey; the Protected Comms password
// is checked BY A CONNECTED DEVICE, which is the only thing that makes this tier
// stronger than Hidden and is why there is no offline fallback for it.
static void testEditorProtectedNeedsBothHalves()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection open = makeSection(QStringLiteral("Calibration"), 0x640,
                                    CommsProtection::None, {});
    CommsSection prot = makeSection(QStringLiteral("Driveline"), 0x641,
                                    CommsProtection::Protected, {});
    prot.messageKey = registerPassword(cfg, "protected-password");
    cfg.bus[0].sections.append(open);
    cfg.bus[0].sections.append(prot);

    // ---- on the way UP, with no device to ask ----
    // PERMITTED. The tick used to demand the Protected Comms password confirmed
    // by a connected unit, which made the tier impossible to set up on the
    // bench where configurations are written. The device check MOVED to the
    // Send gate - a configuration carrying Protect Communication goes only to
    // a unit proving the same password - so ticking costs the Message Password
    // alone, and offline is not a special case.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("protected-password");
        EditorHarness h(&cfg, open); // no prover: this window has no device path
        tierBox(h.dialog, "Protect Communication")->setChecked(true);
        CHECK(pilot.prompts == 1);
        CHECK(tierBox(h.dialog, "Protect Communication")->isChecked());
        clickStandard(h.dialog, QDialogButtonBox::Cancel);
    }
    // A refusing device is equally irrelevant at the tick - it is not asked.
    {
        ModalPilot pilot;
        EditorHarness h(&cfg, open, [] { return false; });
        tierBox(h.dialog, "Protect Communication")->setChecked(true);
        CHECK(!tierBox(h.dialog, "Protect Communication")->isChecked());
    }
    // A DEVICE THAT AGREES IS STILL NOT THE SECTION PASSWORD. The refusal has
    // moved earlier rather than gone: the marking is asked to name its own
    // password at the tick, so a user who has the device and not the password
    // never gets a Protected box to stick at all. Before 2.3.1 this dialog would
    // have closed on the device's word alone.
    {
        ModalPilot pilot; // device says yes; no password offered at the prompt
        EditorHarness h(&cfg, open, [] { return true; });
        tierBox(h.dialog, "Protect Communication")->setChecked(true);
        CHECK(pilot.prompts == 1);
        CHECK(!tierBox(h.dialog, "Protect Communication")->isChecked());
    }
    // With BOTH halves it takes, and the dialog closes.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("protected-password");
        EditorHarness h(&cfg, open, [] { return true; });
        tierBox(h.dialog, "Protect Communication")->setChecked(true);
        CHECK(tierBox(h.dialog, "Protect Communication")->isChecked());
        pilot.reset();
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::Protected);
        CHECK(h.dialog.section().messageKey
              == deriveAccessKey(QStringLiteral("protected-password")));
    }

    // ---- on the way DOWN ----
    // The section password is asked FIRST, so a user who cannot give it never
    // reaches the device at all. Counted, because "the device was asked anyway"
    // is a leak of a different kind: it spends a round trip proving something on
    // behalf of someone who has already failed the local half.
    {
        int proverCalls = 0;
        ModalPilot pilot; // the user gives up at the section password
        EditorHarness h(&cfg, prot, [&proverCalls] {
            ++proverCalls;
            return true;
        });
        tierBox(h.dialog, "Protect Communication")->setChecked(false);
        CHECK(pilot.prompts == 1);
        CHECK(proverCalls == 0);
        CHECK(tierBox(h.dialog, "Protect Communication")->isChecked());
        // ...and NOTHING was banked. A grant stands for the WHOLE of a tier's
        // challenge, so a dialog that gave up half way must not report one.
        CHECK(!h.dialog.protectionUnlocked());
    }
    // Releasing costs the section password and nothing more - the device is
    // not consulted at the boundary any longer, so a refusing prover changes
    // nothing here. What it must still NOT do is mint the document grant:
    // protectionUnlocked() reports a device that actually confirmed, and no
    // device confirmed anything in this window.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("protected-password");
        EditorHarness h(&cfg, prot, [] { return false; });
        tierBox(h.dialog, "Protect Communication")->setChecked(false);
        CHECK(pilot.prompts == 1);
        CHECK(!tierBox(h.dialog, "Protect Communication")->isChecked());
        CHECK(!h.dialog.protectionUnlocked());
        clickStandard(h.dialog, QDialogButtonBox::Cancel);
    }
    // Both halves given: the marking comes off, and only then does the dialog
    // report the section unlocked. It lands UNMARKED rather than on Hidden — the
    // boxes no longer nest — so there is nothing left needing a password.
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("protected-password");
        EditorHarness h(&cfg, prot, [] { return true; });
        tierBox(h.dialog, "Protect Communication")->setChecked(false);
        CHECK(!tierBox(h.dialog, "Protect Communication")->isChecked());
        CHECK(!tierBox(h.dialog, "Hidden")->isChecked());
        CHECK(!tierBox(h.dialog, "Read Only")->isChecked());
        // protectionUnlocked() is no longer set here: the editor does not ask
        // the device at the boundary, so the grant that authorises the commit
        // comes from Communications Setup UNLOCKING the concealed section on
        // the way in - the door the hardware still stands at.
        CHECK(!h.dialog.protectionUnlocked());
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::None);
    }

    // ...and taking Hidden instead is the second step, with its own password.
    // Protected's key does not become Hidden's just because the boxes are
    // adjacent, which is what the old one-untick descent could look like.
    registerPassword(cfg, "hidden-password");
    {
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("protected-password")
                        << QStringLiteral("hidden-password");
        EditorHarness h(&cfg, prot, [] { return true; });
        tierBox(h.dialog, "Protect Communication")->setChecked(false);
        tierBox(h.dialog, "Hidden")->setChecked(true);
        CHECK(pilot.prompts == 2);
        clickStandard(h.dialog, QDialogButtonBox::Ok);
        CHECK(h.accepted);
        CHECK(h.dialog.section().protection == CommsProtection::Hidden);
        CHECK(h.dialog.section().messageKey
              == deriveAccessKey(QStringLiteral("hidden-password")));
    }
}

// REMOVAL is permitted at every tier with no password whatever — the rule
// DECISIONS.md repeats and the one most likely to be "tightened" by someone who
// has just finished reading rules 1 and 2. It is deliberate: a viewer who cannot
// SEE a message cannot retype it, so allowing removal destroys rather than
// reveals, and refusing it would leave a configuration nobody could edit.
static void testRemovalNeedsNothingAtAnyTier()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    for (CommsProtection tier : {CommsProtection::ReadOnly, CommsProtection::Hidden,
                                 CommsProtection::Protected}) {
        CommsSection s = makeSection(QStringLiteral("Msg %1").arg(int(tier)),
                                     0x100 + quint32(tier), tier, {});
        s.messageKey = deriveAccessKey(QStringLiteral("own-password-%1").arg(int(tier)));
        cfg.bus[0].sections.append(s);
    }
    CHECK(cfg.bus[0].sections.size() == 3);
    // No grants, no document password, no device: nothing has been proved about
    // any of these, and all three go.
    while (!cfg.bus[0].sections.isEmpty()) {
        QList<CommsSection> next = cfg.bus[0].sections;
        next.removeLast();
        QString refusal;
        CHECK(cfg.applyBusSections(0, next, &refusal));
        CHECK(refusal.isEmpty());
    }
    CHECK(cfg.bus[0].sections.isEmpty());
}

// ------------------------------------------------------------------ rule 3

// The widgets belonging to one bus, reached through that bus's TAB PAGE. There
// are three of every one of these and findChildren() does not hand them back in
// creation order, so scoping to the page is what says which bus is meant.
static QWidget *busPage(CommunicationsDialog &d, int bus)
{
    auto *tabs = d.findChild<QTabWidget *>();
    return tabs ? tabs->widget(bus) : nullptr;
}

static QPushButton *busButton(CommunicationsDialog &d, const char *text, int bus = 0)
{
    if (QWidget *page = busPage(d, bus))
        for (QPushButton *b : page->findChildren<QPushButton *>())
            if (b->text().remove(QLatin1Char('&')) == QString::fromUtf8(text))
                return b;
    return orStandIn<QPushButton>(nullptr, text);
}

// What the channel pane beside the sections list is showing. This is the exact
// surface the user's rule 3 names — "do not show the channels available in the
// Main Communications Setup afterwards" — so it is what gets asserted, rather
// than a private flag standing in for it.
static QString channelPaneText(CommunicationsDialog &d, int bus = 0)
{
    QWidget *page = busPage(d, bus);
    auto *list = page ? page->findChild<QListWidget *>() : nullptr;
    if (!list)
        return QStringLiteral("(no pane found)");
    QStringList out;
    for (int i = 0; i < list->count(); ++i)
        out << list->item(i)->text();
    return out.join(QLatin1Char('\n'));
}

// The rows of one bus's sections list, as the user reads them.
static QStringList sectionRows(CommunicationsDialog &d, int bus = 0)
{
    QWidget *page = busPage(d, bus);
    auto *tree = page ? page->findChild<QTreeWidget *>() : nullptr;
    if (!tree)
        return {QStringLiteral("(no sections list found)")};
    QStringList out;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        out << tree->topLevelItem(i)->text(0) + QLatin1Char('\t')
                   + tree->topLevelItem(i)->text(1);
    return out;
}

// WHAT THE USER ACTUALLY SAW, driven through the real Communications Setup: a
// keyless marked section — i.e. every section a Get produces — padlocked in the
// list, and its Edit button opening the whole message with nothing asked for.
//
// The two sites disagreed. The rule-3 queue re-concealed on the TIER while the
// unlock skipped the challenge whenever messageKey was kNoAccessKey, so the row
// came back locked and the door beside it stayed open. Both now ask the same
// question — "does this section conceal?" — and the answer for a keyless marking
// is yes, so the unlock explains instead of opening.
static void testKeylessSectionCannotBeOpenedFromCommsSetup()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel c;
    c.name = QStringLiteral("RPM");
    c.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(c);
    CommsSection s = makeSection(QStringLiteral("Engine Data"), 0x640, CommsProtection::Hidden,
                                 QStringLiteral("RPM"));
    s.alignment = SectionAlignment::WordSwap;
    cfg.bus[0].sections.append(s); // KEYLESS, as a Get leaves it
    CHECK(cfg.bus[0].sections.first().messageKey == kNoAccessKey);

    ModalPilot pilot;
    // Deliberately EMPTY. Any prompt at all is a failure here: there is no
    // password to type, and an exhausted script cancels rather than hanging.
    CommunicationsDialog d(&cfg);

    // The row is padlocked AND says why, so nobody goes looking for a password
    // that was never written down.
    const QStringList rows = sectionRows(d);
    CHECK(rows.size() == 1);
    if (rows.size() == 1) {
        CHECK(rows.first().contains(QString::fromUtf8("🔒")));
        CHECK(rows.first().contains(QString::fromUtf8("no password")));
    }
    CHECK(channelPaneText(d).contains(QLatin1String("Channel information locked")));

    // Edit stays LIVE — the button that could plausibly explain must not be the
    // one that does nothing — and pressing it explains rather than opening.
    CHECK(busButton(d, "Edit…")->isEnabled());
    busButton(d, "Edit…")->click();
    CHECK(pilot.prompts == 0);
    CHECK(pilot.messages == 1);
    CHECK(pilot.said("without a Message Password"));
    CHECK(pilot.said("removed"));
    // No editor was opened, nothing was granted, and the pane is still locked.
    CHECK(!grantedByName(cfg, 0, QStringLiteral("Engine Data")));
    CHECK(channelPaneText(d).contains(QLatin1String("Channel information locked")));

    clickStandard(d, QDialogButtonBox::Cancel);
    CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections.first()));
    // No editor was opened at all, said as its own assertion rather than inferred
    // from the absence of a grant.
    CHECK(pilot.editors == 0);
}

// WHAT THE PRODUCT MAY HONESTLY ADVISE ABOUT A KEYLESS MARKED SECTION, PER TIER —
// driven through the real Communications Setup, because three surfaces used to
// give two different answers and only one of them was checked.
//
// The advice everywhere was "give it a Message Password first and untick the
// marking afterwards". Measured here:
//
//   Read Only   TRUE. The tier conceals nothing, so Edit… opens with nothing
//               asked for, a first password is free, and the untick goes through
//               on the second visit.
//   Hidden      FALSE. Edit… explains and opens nothing. A Message Password is
//               typed into the editor, and the editor is what is being refused.
//   Protected   FALSE, the same way and for the same reason.
//
// Nothing is bricked — removal is free at every tier — but the wording sent two
// thirds of its readers looking for a route the application refuses. This pins the
// behaviour so the wording has something to be checked against.
static void testKeylessRepairIsPossibleOnlyAtReadOnly()
{
    const auto build = [](Configuration &cfg, CommsProtection tier) {
        cfg.bus[0].enabled = true;
        Channel c;
        c.name = QStringLiteral("RPM");
        c.userDefined = true;
        cfg.catalog().addOrUpdateUserChannel(c);
        CommsSection s = makeSection(QStringLiteral("Engine Data"), 0x640, tier,
                                     QStringLiteral("RPM"));
        s.alignment = SectionAlignment::WordSwap;
        cfg.bus[0].sections.append(s); // KEYLESS, as a Get leaves it
    };

    // ---- the two tiers that conceal: no editor, no repair ----
    for (CommsProtection tier : {CommsProtection::Hidden, CommsProtection::Protected}) {
        Configuration cfg;
        build(cfg, tier);
        ModalPilot pilot;
        // A password IS offered, so a run that opened a prompt would consume it
        // and pass a naive check. prompts == 0 is what says nothing was asked.
        pilot.passwords << QStringLiteral("would-be-adopted");
        pilot.onEditor = [](SectionEditorDialog &e) {
            passwordField(e)->setText(QStringLiteral("would-be-adopted"));
            clickStandard(e, QDialogButtonBox::Ok);
        };

        CommunicationsDialog d(&cfg);
        busButton(d, "Edit…")->click();
        CHECK(pilot.editors == 0); // the repair route does not exist here
        CHECK(pilot.prompts == 0);
        CHECK(pilot.messages == 1);
        // The refusal says the thing the help used to deny: not merely that it
        // cannot be opened, but that it cannot be given a password either.
        CHECK(pilot.said("cannot be given one"));
        CHECK(pilot.said("REMOVED"));
        clickStandard(d, QDialogButtonBox::Ok);
        CHECK(cfg.bus[0].sections.first().messageKey == kNoAccessKey);
        CHECK(cfg.bus[0].sections.first().protection == tier);
    }

    // ---- Read Only: the two-visit repair, end to end ----
    {
        Configuration cfg;
        build(cfg, CommsProtection::ReadOnly);

        // Visit one: the editor opens with nothing asked for - Read Only
        // conceals nothing - and the untick OFFERS the repair: a question box,
        // then the password prompt, which accepts one of the document's four.
        registerPassword(cfg, "adopted-by-me");
        {
            ModalPilot pilot;
            pilot.passwords << QStringLiteral("adopted-by-me");
            pilot.onEditor = [](SectionEditorDialog &e) {
                tierBox(e, "Read Only")->setChecked(false); // triggers the offer
                clickStandard(e, QDialogButtonBox::Ok);
            };
            CommunicationsDialog d(&cfg);
            busButton(d, "Edit…")->click();
            CHECK(pilot.editors == 1);
            CHECK(pilot.prompts == 1); // the offered password prompt
            clickStandard(d, QDialogButtonBox::Ok);
        }
        CHECK(cfg.bus[0].sections.first().messageKey
              == deriveAccessKey(QStringLiteral("adopted-by-me")));
        CHECK(cfg.bus[0].sections.first().protection == CommsProtection::ReadOnly);

        // Visit two: the password now exists, so the untick asks for it and goes
        // through. A separate dialog, because Communications Setup snapshots the
        // buses when it is built.
        {
            ModalPilot pilot;
            pilot.passwords << QStringLiteral("adopted-by-me");
            pilot.onEditor = [](SectionEditorDialog &e) {
                tierBox(e, "Read Only")->setChecked(false);
                clickStandard(e, QDialogButtonBox::Ok);
            };
            CommunicationsDialog d(&cfg);
            busButton(d, "Edit…")->click();
            CHECK(pilot.editors == 1);
            CHECK(pilot.prompts == 1);
            clickStandard(d, QDialogButtonBox::Ok);
        }
        CHECK(cfg.bus[0].sections.first().protection == CommsProtection::None);
    }
}

// RULE 3, through both real dialogs. Opening a concealed message with its
// password reveals it; closing the editor with the marking still concealing puts
// it straight back. The grant itself outlives the editor and is dropped when
// Communications Setup finally commits — see the ordering test below for why it
// cannot be dropped any sooner.
static void testCommunicationsSetupReConcealsOnEditorClose()
{
    const auto build = [](Configuration &cfg) {
        cfg.clear();
        cfg.bus[0].enabled = true;
        Channel c;
        c.name = QStringLiteral("RPM");
        c.userDefined = true;
        cfg.catalog().addOrUpdateUserChannel(c);
        CommsSection s = makeSection(QStringLiteral("Engine Data"), 0x640,
                                     CommsProtection::Hidden, QStringLiteral("RPM"));
        s.alignment = SectionAlignment::WordSwap;
        s.messageKey = registerPassword(cfg, "engine-password");
        cfg.bus[0].sections.append(s);
    };

    // ---- Cancel out of the editor ----
    {
        Configuration cfg;
        build(cfg);
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("engine-password");
        pilot.onEditor = [](SectionEditorDialog &e) {
            clickStandard(e, QDialogButtonBox::Cancel);
        };

        CommunicationsDialog d(&cfg);
        CHECK(channelPaneText(d).contains(QLatin1String("Channel information locked")));
        // Live for a concealed message, and that is deliberate: pressing it is
        // what runs the challenge. It used to be greyed out with a tooltip
        // naming a menu, which made the one control that could plausibly ask for
        // the password the one control that refused to do anything.
        CHECK(busButton(d, "Edit…")->isEnabled());
        busButton(d, "Edit…")->click();
        CHECK(pilot.prompts == 1); // the section's own password, asked once

        // The editor has closed on a section that STILL conceals, so the pane is
        // locked again immediately — this is the user's sentence, tested.
        CHECK(channelPaneText(d).contains(QLatin1String("Channel information locked")));
        // The grant is still on the DOCUMENT at this point, deliberately: it is
        // what authorises a lowering applyBusSections has not seen yet. Rule 3's
        // re-conceal is immediate; the grant drops when the write lands or is
        // abandoned.
        CHECK(grantedByName(cfg, 0, QStringLiteral("Engine Data")));

        clickStandard(d, QDialogButtonBox::Cancel);
        CHECK(!grantedByName(cfg, 0, QStringLiteral("Engine Data")));
        CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections.first()));
    }

    // ---- OK out of the editor, nothing changed ----
    // Same answer. The user wrote "after closing or editing", and a message that
    // re-locks only when you cancel would teach people to press OK.
    {
        Configuration cfg;
        build(cfg);
        ModalPilot pilot;
        pilot.passwords << QStringLiteral("engine-password");
        pilot.onEditor = [](SectionEditorDialog &e) {
            // The stored key still guards the tier the section arrived on, so
            // rule 1 is satisfied with the field left empty and OK closes.
            clickStandard(e, QDialogButtonBox::Ok);
        };

        CommunicationsDialog d(&cfg);
        busButton(d, "Edit…")->click();
        CHECK(channelPaneText(d).contains(QLatin1String("Channel information locked")));
        clickStandard(d, QDialogButtonBox::Ok);
        CHECK(!grantedByName(cfg, 0, QStringLiteral("Engine Data")));
        CHECK(cfg.bus[0].sections.first().protection == CommsProtection::Hidden);
        CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections.first()));
    }

    // ---- lowered to a tier that conceals NOTHING ----
    // No re-conceal, and no revoke: there is nothing left to hide, and dropping
    // the grant would only re-lock a message the user has just unmarked.
    {
        Configuration cfg;
        build(cfg);
        ModalPilot pilot;
        // One for the unlock on the way in, one for giving up Hidden inside the
        // editor. The same password, asked by two different dialogs, because
        // neither remembers what the other proved.
        pilot.passwords << QStringLiteral("engine-password")
                        << QStringLiteral("engine-password");
        pilot.onEditor = [](SectionEditorDialog &e) {
            tierBox(e, "Hidden")->setChecked(false);
            tierBox(e, "Read Only")->setChecked(false);
            clickStandard(e, QDialogButtonBox::Ok);
        };

        CommunicationsDialog d(&cfg);
        busButton(d, "Edit…")->click();
        CHECK(pilot.prompts == 2);
        CHECK(!channelPaneText(d).contains(QLatin1String("Channel information locked")));
        CHECK(channelPaneText(d).contains(QLatin1String("RPM")));
        clickStandard(d, QDialogButtonBox::Ok);
        CHECK(cfg.bus[0].sections.first().protection == CommsProtection::None);
        CHECK(grantedByName(cfg, 0, QStringLiteral("Engine Data")));
    }
}

// THE ORDERING, end to end. Protect Communication lowered to Hidden is the exact
// shape rule 3 can break: the resulting tier still conceals, so the revoke is
// queued — and the grant that was queued for revocation is also the only thing
// authorising the lowering that Communications Setup has not written yet.
//
// Flush before the applyBusSections loop instead of after it and this test fails
// at the last two lines: the document keeps Protect Communication, and the user
// is told they may not make the change they were just asked for two passwords to
// make.
static void testProtectedDownToHiddenSurvivesTheRevoke()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel c;
    c.name = QStringLiteral("Torque");
    c.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(c);
    CommsSection s = makeSection(QStringLiteral("Driveline"), 0x641,
                                 CommsProtection::Protected, QStringLiteral("Torque"));
    s.alignment = SectionAlignment::WordSwap;
    s.messageKey = registerPassword(cfg, "driveline-password");
    cfg.bus[0].sections.append(s);
    // The password Hidden will be guarded by has to be one of the four BEFORE
    // the editor asks for it - the prompt matches, it does not invent.
    registerPassword(cfg, "hidden-password");

    ModalPilot pilot;
    // Three times: Communications Setup asks to OPEN it, the editor asks again
    // to give up Protect Communication, and taking Hidden afterwards asks for
    // the password that will guard THAT. The device is asked twice, for the two
    // crossings of the Protected boundary.
    pilot.passwords << QStringLiteral("driveline-password")
                    << QStringLiteral("driveline-password")
                    << QStringLiteral("hidden-password");
    int proverCalls = 0;
    pilot.onEditor = [](SectionEditorDialog &e) {
        // Giving up Protect Communication now lands UNMARKED rather than on
        // Hidden: one marking at a time, and this one has been given up.
        tierBox(e, "Protect Communication")->setChecked(false);
        CHECK(!tierBox(e, "Hidden")->isChecked());
        // Taking Hidden is a second, separate decision, and it asks for its own
        // password — which is how "the key that guarded Protect Communication
        // does not carry onto Hidden" is now enforced, at the tick rather than
        // at the door.
        tierBox(e, "Hidden")->setChecked(true);
        clickStandard(e, QDialogButtonBox::Ok);
    };

    CommunicationsDialog d(&cfg, nullptr, [&proverCalls] {
        ++proverCalls;
        return true;
    });
    CHECK(channelPaneText(d).contains(QLatin1String("Channel information locked")));
    busButton(d, "Edit…")->click();
    CHECK(pilot.prompts == 3);
    // ONE device confirmation now, not two: Communications Setup proves it to
    // OPEN the concealed section, and the boundary no longer asks again - the
    // second door the hardware stood at moved to the Send gate.
    CHECK(proverCalls == 1);
    // Lowered, and STILL concealing — so rule 3 re-locks the pane at once.
    CHECK(channelPaneText(d).contains(QLatin1String("Channel information locked")));
    CHECK(grantedByName(cfg, 0, QStringLiteral("Driveline")));

    clickStandard(d, QDialogButtonBox::Ok);
    // The write went through, THEN the grant was dropped.
    CHECK(cfg.bus[0].sections.first().protection == CommsProtection::Hidden);
    CHECK(cfg.bus[0].sections.first().messageKey
          == deriveAccessKey(QStringLiteral("hidden-password")));
    CHECK(!grantedByName(cfg, 0, QStringLiteral("Driveline")));
    CHECK(!cfg.isSectionRevealed(cfg.bus[0].sections.first()));
}


// THE SOURCE COLUMN AND THE ONE CATEGORY THAT IS NEVER UNUSED.
//
// sourceFor() walks the comms sections, then every calculation, and anything it
// does not find it calls "unused". Device Channels are found by none of those —
// Device On Time, the per-bus diagnostics (Rx Count, Tx Count, Rx/Tx Errors,
// Error Frames, Bus Load, the three bus-state flags) and the MCU health block
// are produced by the firmware itself — so the Channel Editor labelled the one
// group of channels that is ALWAYS being produced as the one thing they never
// are. They read "Internal" now.
//
// Checked against ChannelCatalog::deviceChannels() rather than against a list
// of names typed out here: the point is that the whole category answers, and a
// hand-written list would quietly stop covering whichever channel got added
// after it.
void testDeviceChannelsReportAnInternalSource()
{
    Configuration cfg;
    cfg.clear();
    cfg.bus[0].enabled = true;

    // A user channel nothing produces, to hold the other half of the behaviour
    // still: "unused" was never wrong for these and must not have moved.
    Channel orphan;
    orphan.name = QStringLiteral("Orphan Pressure");
    orphan.unit = QStringLiteral("kPa");
    orphan.quantity = QStringLiteral("Pressure");
    orphan.dataType = QStringLiteral("u16");
    cfg.catalog().addOrUpdateUserChannel(orphan);

    ChannelEditorDialog d(&cfg);
    QTreeWidget *tree = d.findChild<QTreeWidget *>();
    CHECK(tree != nullptr);
    if (!tree)
        return;

    int sourceCol = -1;
    for (int c = 0; c < tree->columnCount(); ++c)
        if (tree->headerItem()->text(c) == QStringLiteral("Source"))
            sourceCol = c;
    CHECK(sourceCol >= 0);
    if (sourceCol < 0)
        return;

    const auto sourceOf = [&](const QString &name) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == name)
                return tree->topLevelItem(i)->text(sourceCol);
        return QStringLiteral("<no such row>");
    };

    const QList<Channel> device = ChannelCatalog::deviceChannels();
    CHECK(!device.isEmpty());
    for (const Channel &dc : device)
        CHECK(sourceOf(dc.name) == QStringLiteral("Internal"));

    CHECK(sourceOf(QStringLiteral("Orphan Pressure")) == QStringLiteral("unused"));
}

// THE PROTECT COMMUNICATION SEND GATE, branch by branch and in order.
//
// This is the only thing standing between a configuration sealed under one
// Protected Comms password and a unit that does not hold it - the device
// enforces nothing about message protection itself. Until the decision was
// lifted out of MainWindow::onSendConfiguration it could not be tested at all:
// every branch lived inside a slot needing a window, a device and a user. What
// is pinned here is the part that would ship a bypass if it broke - WHICH
// answer comes back for each state, and in what ORDER the states are judged.
static void testProtectedCommsSendGate()
{
    using namespace ct::device_session;
    using V = ProtectedSendVerdict;

    const AccessKey docKey = deriveAccessKey(QStringLiteral("document-comms-pw"));

    AccessState none;                 // pre-v19 firmware: no access passwords at all
    AccessState bare;                 // v19+, but no Protected Comms password set
    bare.supported = true;
    AccessState armed;                // v19+, Protected Comms set
    armed.supported = true;
    armed.set[int(AccessFunction::EditProtectedComms)] = true;

    // The happy path, and the ONLY verdict that lets a send proceed.
    CHECK(protectedSendVerdict(true, armed, docKey, /*proved=*/true, /*wrongKey=*/false)
          == V::Allowed);

    // A unit that could not be asked is never reported as disagreeing: "I could
    // not tell" must not become "it is wrong". Checked FIRST, so it wins even
    // when the state passed alongside it would have refused for another reason.
    CHECK(protectedSendVerdict(false, armed, docKey, true, false) == V::NoDeviceAnswer);
    CHECK(protectedSendVerdict(false, bare, kNoAccessKey, false, true) == V::NoDeviceAnswer);

    // No password on the unit - including firmware too old to have one - is not
    // the same password.
    CHECK(protectedSendVerdict(true, bare, docKey, true, false) == V::DeviceHasNoPassword);
    CHECK(protectedSendVerdict(true, none, docKey, true, false) == V::DeviceHasNoPassword);
    // And that answer outranks the document's own missing key, so a user is
    // told about the end they can fix on the bench in front of them.
    CHECK(protectedSendVerdict(true, bare, kNoAccessKey, false, false)
          == V::DeviceHasNoPassword);

    // A document with nothing to match with is named as such rather than sent
    // to a prove that would fail for a second reason.
    CHECK(protectedSendVerdict(true, armed, kNoAccessKey, false, false)
          == V::DocumentHasNoKey);

    // The case the rule exists for, kept distinct from a failed round trip:
    // wrongKey says the unit answered and disagreed.
    CHECK(protectedSendVerdict(true, armed, docKey, false, /*wrongKey=*/true) == V::Mismatch);
    CHECK(protectedSendVerdict(true, armed, docKey, false, /*wrongKey=*/false)
          == V::ProofFailed);

    // Every refusal is a refusal: nothing but Allowed may let a send through.
    const V refusals[] = { V::NoDeviceAnswer, V::DeviceHasNoPassword, V::DocumentHasNoKey,
                           V::Mismatch, V::ProofFailed };
    for (V v : refusals)
        CHECK(v != V::Allowed);

    // The gate only runs at all for a configuration that carries Protect
    // Communication messages - hasProtectedComms() is the switch, and an Off
    // section is not sent, so its marking does not hold up a send.
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection prot;
    prot.name = QStringLiteral("Sealed");
    prot.device = SectionDevice::ReceiveMessage;
    prot.baseAddress = 0x660;
    prot.messageLengthBytes = 8;
    prot.protection = CommsProtection::Protected;
    prot.messageKey = docKey;
    cfg.bus[0].sections.append(prot);
    CHECK(cfg.hasProtectedComms());
    cfg.bus[0].sections[0].device = SectionDevice::Off;
    CHECK(!cfg.hasProtectedComms());
}

// A NEW DOCUMENT KEEPS NONE OF THE LAST ONE'S KEYS, the four Message
// Passwords included. They were the one secret clear() forgot: every other one
// was wiped, so File > New after opening a protected configuration carried its
// derived keys into the blank document - where a Save would persist them, a
// Send would program them onto a device, and a Get would keep them in
// preference to the device's own (mapFromDevice fills only empty slots).
static void testNewDocumentKeepsNoPasswords()
{
    Configuration cfg;
    cfg.setCommsPasswordSlot(1, deriveAccessKey(QStringLiteral("first-document-pw")));
    cfg.setCommsPasswordSlot(3, deriveAccessKey(QStringLiteral("third-document-pw")));
    cfg.setCommsPassword(QStringLiteral("protected-comms-pw"));
    CHECK(cfg.commsPasswordsInUse() == 2);

    cfg.clear(); // File > New, on the same object the window holds

    for (int slot = 1; slot <= Configuration::kCommsPasswordSlots; ++slot)
        CHECK(cfg.commsPassword(slot) == kNoAccessKey);
    CHECK(cfg.commsPasswordsInUse() == 0);
    // So a marking made in the new document takes slot 1, rather than finding
    // it occupied by a password the user was never shown and cannot produce.
    CHECK(cfg.firstFreeCommsPasswordSlot() == 1);
    // And the document-wide Protected Comms key goes with them, as it always did.
    CHECK(cfg.commsKey() == kNoAccessKey);
}

// THE EDIT CHANNEL DIALOG'S OWN REFUSALS.
//
// A channel's Range Minimum/Maximum became editable in 1.1.0, and the device
// clamps every reading to whatever they say - applying the minimum first and
// the maximum second, with no inverted-pair guard, so min >= max pins every
// reading to the maximum. validateConfiguration catches that at Send and is
// tested; this is the front line that stops it being typed in the first place,
// and it had nothing.
static void testEditChannelDialogRefusals()
{
    Configuration cfg;

    // A helper that fills the dialog, presses OK, and says whether it closed.
    // The pilot answers the refusal box OK raises - without it the click never
    // returns and the run hangs instead of failing.
    const auto attempt = [&](const QString &name, double lo, double hi,
                             const Channel &initial, bool isNew) {
        ModalPilot pilot;
        EditChannelDialog dlg(&cfg, initial, isNew, nullptr);
        auto *nameEdit = dlg.findChild<QLineEdit *>(QStringLiteral("channelName"));
        auto *minSpin = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("rangeMinimum"));
        auto *maxSpin = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("rangeMaximum"));
        CHECK(nameEdit && minSpin && maxSpin);
        if (!nameEdit || !minSpin || !maxSpin)
            return false;
        nameEdit->setText(name);
        minSpin->setValue(lo);
        maxSpin->setValue(hi);
        auto *box = dlg.findChild<QDialogButtonBox *>();
        CHECK(box);
        if (!box)
            return false;
        QAbstractButton *ok = box->button(QDialogButtonBox::Ok);
        CHECK(ok);
        if (!ok)
            return false;
        ok->click();
        // A dialog that refused is still open; one that accepted is not.
        return !dlg.isVisible() && dlg.result() == QDialog::Accepted;
    };

    Channel base;
    base.name = QStringLiteral("Coolant Temp");
    base.dataType = QStringLiteral("s16");
    base.decimalPlaces = 1;
    base.minValue = -40;
    base.maxValue = 215;

    // An ordinary narrowed range is accepted - the feature works.
    CHECK(attempt(QStringLiteral("Coolant Temp"), -40.0, 150.0, base, true));

    // INVERTED is refused: the device would pin every reading to the maximum.
    CHECK(!attempt(QStringLiteral("Coolant Temp"), 150.0, -40.0, base, true));
    // EQUAL is refused too - a range of one value is the same defect with a
    // narrower gap, and the check is >= for that reason.
    CHECK(!attempt(QStringLiteral("Coolant Temp"), 100.0, 100.0, base, true));

    // And the name rules the same OK press enforces.
    CHECK(!attempt(QString(), -40.0, 150.0, base, true)); // empty
    // An over-long name never reaches the refusal: the field caps typing at
    // MAX_CHANNEL_NAME_BYTES, so 200 characters arrive as the limit and are
    // accepted. That is the better guard of the two - validate()'s length check
    // is the backstop for a multi-byte name whose CHARACTER count fits while
    // its UTF-8 byte count does not.
    {
        ModalPilot pilot;
        EditChannelDialog dlg(&cfg, base, true, nullptr);
        auto *nameEdit = dlg.findChild<QLineEdit *>(QStringLiteral("channelName"));
        CHECK(nameEdit);
        if (nameEdit) {
            nameEdit->setText(QString(200, QLatin1Char('x')));
            CHECK(nameEdit->text().size() == MAX_CHANNEL_NAME_BYTES);
        }
    }

    // A duplicate of a channel the document already has is refused as well.
    Channel existing;
    existing.name = QStringLiteral("Taken");
    existing.dataType = QStringLiteral("u8");
    existing.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(existing);
    CHECK(!attempt(QStringLiteral("Taken"), 0.0, 100.0, base, true));
}

int main(int argc, char **argv)
{
    // Offscreen: half of this file drives real widgets, and a test binary that
    // needs a desktop is a test binary that does not run on a build machine.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testTierModel();
    testJsonRoundTrip();
    testMigrationFromSchema13();
    testNoReRatchetOnSchema14();
    testHasProtectedComms();
    testProtectedCommsSendGate();
    testNewDocumentKeepsNoPasswords();
    testEditChannelDialogRefusals();
    testApplyBusSectionsGuard();
    testKeySwapIsAsPrivilegedAsAnUntick();
    testDuplicateNamesCannotAnswerForEachOther();
    testAReorderIsNotAChange();
    testAnUnnamedSectionCannotLaunderAnUntickEither();
    testRenamingCannotLaunderAnUntick();
    testKeylessGrantsProveNothing();
    testProofPolicy();
    testProtectedAlsoNeedsItsSectionPassword();
    testRevokeSectionAccess();
    testGrantsNameASectionNotAName();
    testLiveViewCannotAuthoriseALowering();
    testGetPreservesDocumentState();
    testGetIntoAFreshDocumentRestoresSectionKeys();
    testGetPreservesSectionKeysAndNames();
    testGetIntoAFreshDocumentConceals();
    testSavingAfterAGet();
    testGetKeepsThePasswordWhenTheIdMoves();
    testGetKeepsThePasswordForAGeneratedName();
    testGrantsDoNotSurviveAnOpen();
    testWireRoundTripPreservesTier();
    testChannelPredicateSplit();

    // The dialogs. Rules 1, 2 and 3 are enforced here and nowhere else.
    testEditorRefusesEveryMarkedTierWithNoPassword();
    testKeylessMarkedSectionIsConcealedAndCannotBeKept();
    testEditorRaiseNeedsTheOldPasswordThenANewOne();
    testProtectedAlwaysAsksAndNothingCarries();
    testReleasedPlaceholderNameRegenerates();
    testEditorLowerStillNeedsTheSectionPassword();
    testEditorReplacingAPasswordProvesTheOldOne();
    testEditorProtectedNeedsBothHalves();
    testRemovalNeedsNothingAtAnyTier();
    testKeylessSectionCannotBeOpenedFromCommsSetup();
    testKeylessRepairIsPossibleOnlyAtReadOnly();
    testCommunicationsSetupReConcealsOnEditorClose();
    testProtectedDownToHiddenSurvivesTheRevoke();
    testDeviceChannelsReportAnInternalSource();

    if (failures == 0)
        std::printf("ALL PROTECTION TIER TESTS PASSED\n");
    else
        std::printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
