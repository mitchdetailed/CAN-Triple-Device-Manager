// The document: everything saved in a .ct3 file.
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

#include "access_keys.h"
#include "channel_catalog.h"
#include "comms_types.h"
#include "config_lock.h"
#include "secure_file.h"

namespace ct {

class Configuration;

// The .ct3 SCHEMA number this build writes and is the newest it can read. Not
// the application version and not the .ct3s container's formatVersion: it
// describes the shape of the document's JSON, and it is the only number
// CommsSection::fromJson consults to decide which migrations a file needs.
//
// Exposed as a function because the constant itself lives beside the loader in
// configuration.cpp, where every other user of it is, and a second definition
// in this header is a second thing to forget to bump. Communications templates
// need it: they carry sections spelled at this schema, and reading one back
// means telling fromJson which spelling it is looking at.
int configSchemaVersion();

// Every grid dialog edits a working copy of one slice of the document and
// writes it back only on OK, so mid-session the document is stale in BOTH
// directions: it lacks rows just added and still carries rows just deleted or
// re-pointed. A ConfigPatch re-states the caller's slice — "my math rows are
// really these" — on a scratch copy, so anything that has to answer "what
// generates this channel?" can answer it against live state instead of the
// last thing written back. See Configuration::buildLiveView.
using ConfigPatch = std::function<void(Configuration &)>;

// "Is this session authorised, RIGHT NOW, to lower a Protect Communication
// marking?" — which by spec means a CONNECTED DEVICE confirming the Edit
// Protected Comms password. That device round trip is the only thing making
// Protected stronger than Hidden, so it must never decay into a local check.
//
// A callable rather than a DeviceLink* on the dialogs, for two reasons. It keeps
// every fact about the hardware — is anything connected, does it even have the
// password set, has it already been proved this session — in the one place that
// owns the link (MainWindow), instead of teaching two dialogs the protocol. And
// it keeps the serial layer out of headers that GUI-only test targets compile,
// which is not a theoretical tidiness: the alternative made three of them
// require Qt::SerialPort to build a message-list widget.
//
// The callable reports its own failures to the user. An EMPTY one is a valid and
// expected state — an offline tool, a test harness, a document opened with no
// device path at all — and means "no device, so no": the caller must refuse and
// say why, never treat it as permission.
using ProtectedCommsProver = std::function<bool()>;

class Configuration : public QObject
{
    Q_OBJECT
public:
    explicit Configuration(QObject *parent = nullptr);

    // Document state
    QString filePath() const { return m_filePath; }
    bool isDirty() const { return m_dirty; }
    void setDirty(bool dirty = true);
    QString displayName() const; // file base name or "Untitled"

    // The configuration title stored on the device (<=32 bytes). Explicitly set
    // in the Send dialog; effectiveTitle() falls back to the saved-file base
    // name, or an EMPTY string when the document is untitled (so the Send dialog
    // forces the user to enter one) — deliberately NOT displayName()'s
    // "Untitled" placeholder.
    QString configTitle() const { return m_configTitle; }
    void setConfigTitle(const QString &title);
    QString effectiveTitle() const;

    void clear(); // new empty document
    // The CONTENT only — buses, calculation rows, catalog, comments, script,
    // title, file path — leaving the access passwords, the reveal grant, the
    // fleet identity and the device lock exactly as they were. This is what a
    // Get uses (device_mapper's mapFromDevice), because a Get replaces what the
    // hardware knows and must not touch what the DOCUMENT knows. Calling
    // clear() there, as it used to, wiped the access verifiers — after which
    // hasCommsPassword() was false, commsRevealed() was true, and every
    // protection tier in the document could be lowered with no challenge at all.
    void clearContent();

    // -------------------------------------------------------- access passwords
    // Only ONE of the three access passwords has a document-side meaning at all:
    // Edit Protected Comms. Its reach is now NARROWER than a reader would guess
    // and this comment used to overstate it, contradicting the per-tier table at
    // proofsRequiredFor() a hundred lines below.
    //
    // What the DOCUMENT's copy of it does: it gates setCommsPassword() (you
    // cannot replace a password you cannot prove), and concealProtectedComms()
    // drops every per-section grant with it. What it does NOT do is reveal or
    // unlock any section. Every marked tier — Read Only, Hidden and Protected
    // alike — is opened by the SECTION's own password (CommsSection::messageKey)
    // and nothing else; Protected additionally wants this password confirmed by
    // a CONNECTED DEVICE, which is a fact about hardware rather than about this
    // verifier. See isSectionRevealed() and maySectionLower(), which is where
    // that is implemented and where it stays implemented.
    //
    // Send and Get are device gates — they say what a unit will do for you, not
    // what a file will show you — so the document does not carry them.
    //
    // What the file stores is a salted VERIFIER, never the 4-byte key: enough to
    // check a typed password offline, useless for opening hardware. See
    // access_keys.h for why those are two different derivations.
    const AccessVerifierSet &accessVerifiers() const { return m_accessVerifiers; }
    bool hasCommsPassword() const;

    // The DOCUMENT-WIDE Edit Protected Comms state, and nothing more. True when
    // no such password is set — with nothing to prove there is nothing to
    // withhold — or when it has been given this session.
    //
    // NOT a suppression gate, and no longer a suppression gate for any tier:
    // isSectionRevealed() and maySectionLower() do not consult this at all. It
    // is true for every document that has no Edit Protected Comms password —
    // which is the normal shape when the sections carry their own — so every
    // `if (commsRevealed()) return false;` shortcut in front of a per-section
    // walk revealed every Hidden section in that document, and the keyless arm
    // it survived in for Protected did the same for every section a Get
    // produced. Both are gone; the per-section grant is the only answer.
    bool commsRevealed() const { return !hasCommsPassword() || m_commsRevealed; }
    // Try to reveal. Returns false on the wrong password, leaving state alone.
    bool revealProtectedComms(const QString &password);
    void concealProtectedComms(); // forget it again, and every per-section grant

    // ------------------------------------------- per-section session grants
    // What a tier demands before this session may see a section or move it off
    // that tier. THE policy, in one place, next to the two predicates that read
    // it and reachable by the two dialogs that have to RUN the challenges — so
    // "what counts as proved" is decided once instead of being re-derived in a
    // checkbox handler, an unlock button and a model predicate that can then
    // drift apart.
    //
    // Both flags are demands on the VIEWER, not descriptions of the document,
    // and this table is the same whether or not the section HAS a password. A
    // section with no messageKey has no section password to prove and therefore
    // cannot meet a demand for one: it is concealed and un-lowerable rather than
    // excused. See the fail-closed rule at isSectionRevealed.
    struct SectionProofs {
        bool sectionPassword; // this section's own Message Password (messageKey)
        bool deviceProof;     // Edit Protected Comms, confirmed by a CONNECTED device
    };
    // None       nothing.
    // ReadOnly   the section's own password.
    // Hidden     the section's own password.
    // Protected  the section's own password AND the device round trip. BOTH, as
    //            of 2.3.1: rule 1 of the user's spec gives every marked tier a
    //            password of its own, and rule 2 asks for "a new password for the
    //            new attribute" when a section moves onto one. Protected used to
    //            answer to Edit Protected Comms alone, which made it the one tier
    //            with no per-section secret at all.
    static SectionProofs proofsRequiredFor(CommsProtection tier);

    // A section whose OWN challenge has been met this session — EVERY part of it.
    // A caller must run all of proofsRequiredFor(tier) before calling in here,
    // because that is what keeps the predicates below simple: they ask "granted?"
    // and never re-litigate which passwords that stood for.
    //
    // The section editor and Communications Setup perform whichever challenges
    // apply and call in here. What this class records is WHICH SECTION was opened
    // and WHICH SECRET was given for it: the bus index, the lower-cased name and
    // the messageKey the challenge was answered against. It records neither which
    // of the two proofs ran nor what was typed — that contract stays on the two
    // dialogs, which is why the header used to say a grant records only "that
    // they were satisfied". It records more than that now, and it has to.
    //
    // Both halves of the identity are load-bearing, and each closes a hole that
    // was live:
    //
    //   THE BUS, because a bare name is a value the ATTACKER chooses. Adding a
    //   section on another bus under the same name, with a password of your own,
    //   and unlocking that used to open the real one: applyBusSections is
    //   per-bus and the grant set was global, so same-bus was the only case
    //   accidentally blocked.
    //
    //   THE KEY, because a grant must not outlive the secret it stood for. A
    //   grant answers only for a section whose messageKey is still the one the
    //   challenge was answered against, so REPLACING that key retires it
    //   automatically. That is the belt under applyBusSections' refusal of an
    //   unproved password change: a future writer that forgets the chokepoint
    //   can still hand a section to a new owner, but it cannot leave the old
    //   owner's session standing on the section it no longer holds the key to.
    //
    // A KEYLESS SECTION IS NOT GRANTABLE, and nothing here records one. This is
    // the half the header used to get wrong by omission: kNoAccessKey is not a
    // secret, every keyless section in the document shares it, so a grant taken
    // against it left the lower-cased NAME deciding on its own — exactly the
    // hole the key was introduced to close, reopened for the one kind of section
    // a Get produces by the hundred. Unlocking a keyless "Engine Data" on CAN 2
    // revealed a keyless "Engine Data" on CAN 1 and made it lowerable. So a
    // keyless section records no grant, and any grant standing for that (bus,
    // name) is dropped rather than left to answer for a section that no longer
    // holds the secret it was taken against.
    //
    // What that costs, stated plainly rather than papered over: a marked section
    // with no password can no longer be opened at all — see isSectionRevealed.
    // Nothing could open it honestly, because no password for it exists.
    //
    // A grant does two things at once, and deliberately not one without the
    // other. The section is REVEALED to this viewer, and its tier may be
    // LOWERED. A password entered to look at something is not by itself a demand
    // to lower it permanently — but the person who has just proved they may look
    // is exactly the person entitled to untick the box, and making them prove it
    // twice would only teach them to stop reading the prompt.
    //
    // Session state. Never written to a file, never sent to a device. Cleared by
    // Conceal Protected Comms and by File > New; it SURVIVES a Get for the same
    // reason the verifiers do — a Get replaces what the hardware knows, not what
    // this session has proved. A Get that leaves a section carrying a DIFFERENT
    // key from the one that was proved drops the grant with it, which is the key
    // rule above working rather than an exception to this one.
    //
    // An unnamed section is not grantable: the name is the handle, and a section
    // with no name has nothing for a grant to be about.
    void grantSectionAccess(int busIndex, const CommsSection &section);
    // The EXACT question: this bus, this name, and this section's key still the
    // one the challenge was answered against. The section is passed rather than
    // its name because the key is half the question and passing them separately
    // is how a caller ends up asking about one section with another's secret. A
    // negative bus answers false rather than meaning "any bus": this name
    // promises a bus-checked answer, and a caller that does not know which bus
    // it means is asking sectionGrantProves()'s question, not this one.
    bool sectionAccessGranted(int busIndex, const CommsSection &section) const;
    // Forget one section's grant, leaving every other section's alone. The
    // counterpart Communications Setup needs for rule 3: a section whose editor
    // has just closed while it STILL CONCEALS goes straight back to concealed,
    // rather than staying open for the rest of the session because it was opened
    // once. Same bus, same lower-cased name matching as grantSectionAccess — and
    // by NAME alone, deliberately: rule 3 queues the name a section had before
    // the editor opened as well as the one it has after, and by then the key may
    // have been replaced too, so a revoke that also had to match the key would
    // silently leave the grant it was asked to drop.
    //
    // Ordering is the trap, and it is not a hypothetical: the grant is also what
    // authorises a LOWERING at applyBusSections, so revoking before the edit has
    // been written to the document makes the model refuse the very change the
    // user was just authorised to make. See CommunicationsDialog's pending-revoke
    // set for how the two are sequenced.
    void revokeSectionAccess(int busIndex, const QString &sectionName);

    // The `revealed` argument EVERY suppression site passes to
    // CommsSection::isConcealed(). Answered per tier, because which password
    // applies is a property of the tier:
    //
    //   None / ReadOnly   always revealed. ReadOnly never conceals — that IS
    //                     the tier, and it is the whole difference from Hidden.
    //   Hidden            a grant for this section: its own password, given this
    //                     session.
    //   Protected         a grant — which by proofsRequiredFor() means this
    //                     section's own password AND a live device confirming
    //                     Edit Protected Comms.
    //
    // IT FAILS CLOSED, and that is the 2.3.2 reversal. A marked section with NO
    // messageKey is CONCEALED, where it used to be open to everybody. There is
    // no password in existence for such a section, so there is nothing that
    // could ever open it — and the old rule read that as "therefore show it to
    // anyone", which is precisely the failure the tier exists to prevent.
    //
    // It is also the user's bug, in one line. The wire has no room for a key
    // (the record's field is `reserved[4]`, written and read as zero), so EVERY
    // section a Get produces is keyless: mark a message Hidden, send it, restart,
    // Get it back, and the sections list showed a padlock and the word "hidden"
    // over a row whose channels were listed and whose editor opened on a
    // double-click with nothing asked for. Fail-closed makes the padlock true.
    //
    // What it deliberately costs: a configuration retrieved from a device cannot
    // have its marked messages read in this application, by anybody, ever. That
    // is the product's stated purpose — the customer installs updates and does
    // not read them — and the person who built the configuration still holds the
    // original .ct3, which holds the passwords. A retrieved section can still be
    // REMOVED, and the configuration re-sent, so nothing is bricked and no unit
    // is stranded. saveToFile()/saveSecureToFile() have a carve-out for exactly
    // this state; see anyKeyedSectionConcealed().
    //
    // `busIndex` is passed by every caller that HAS one, and answering per bus
    // is the point rather than a tidy-up: see sectionGrantProves(). -1 means the
    // caller was handed a bare section value and genuinely does not know.
    //
    // Emphatically NOT commsRevealed(), at any tier now. Passing the
    // document-wide flag was hole A: commsRevealed() is true whenever a document
    // carries no Edit Protected Comms verifier, so on precisely the documents
    // whose sections carry their own passwords, every Hidden section printed its
    // CAN ID and bit layout.
    bool isSectionRevealed(const CommsSection &section, int busIndex = -1) const;

    // Set, change or clear the document's Edit Protected Comms password; an
    // empty password clears it. Refuses while comms are still concealed —
    // otherwise "set a new password" would be the way past not knowing the old.
    // The secure-save options move with it, so a document that came from a
    // .ct3s is re-sealed under the password it now verifies rather than the one
    // it used to; see the coupling explained at the definition.
    bool setCommsPassword(const QString &password);

    // The 4-byte key for this session, when it is known: derived from a typed
    // password, or carried inside a .ct3s. Needed to satisfy a DEVICE's
    // protected-comms gate on the operator's behalf — which is what lets a
    // customer deploy a locked configuration without holding the password.
    AccessKey commsKey() const { return m_commsKey; }
    void setCommsKey(AccessKey key) { m_commsKey = key; }

    // -------------------------------------------------------- fleet identity
    // Which fleet this configuration is FOR, checked against the device's own
    // identity before an upload is allowed, and the policy saying how strictly.
    // fleetKey is a secret and is written only into a .ct3s; a plain .ct3
    // carries the identity without it.
    //
    // Note the asymmetry with the device side: a DEVICE's identity is compiled
    // into its firmware, while a CONFIGURATION's is edited here. That is not an
    // inconsistency — one is a statement about a piece of hardware, the other a
    // statement about which hardware a file is allowed to reach.
    const FleetIdentity &fleetIdentity() const { return m_fleetIdentity; }
    void setFleetIdentity(const FleetIdentity &id);
    const UploadPolicy &uploadPolicy() const { return m_uploadPolicy; }
    void setUploadPolicy(const UploadPolicy &policy);

    // ----------------------------------------------------------- device lock
    // The ONE CAN Triple this configuration may be sent to, as the UID hex
    // string Device Status shows. Empty means not locked, which is the default
    // and what every configuration written before this existed loads as.
    //
    // Distinct from the fleet identity above, which says which FAMILY of
    // hardware a file suits and is checked by the uploader. This is a single
    // serial number: "this file belongs to that unit and no other". Both can be
    // set; they answer different questions.
    //
    // Enforced by this application before a Send, not by the device — the
    // device already has its own binding, which stops it RUNNING a configuration
    // stamped for another unit. This one stops the send happening at all, which
    // is what keeps a technician from writing the wrong car's calibration into a
    // car and finding out when it will not start.
    QString lockedDeviceUid() const { return m_lockedDeviceUid; }
    // The key that authorises changing or clearing the lock, so the person who
    // set it is the one who can move it. kNoAccessKey means anyone may.
    AccessKey deviceLockKey() const { return m_deviceLockKey; }
    void setDeviceLock(const QString &uid, AccessKey key);
    bool isLockedToDevice() const { return !m_lockedDeviceUid.isEmpty(); }
    // Whether this configuration may be sent to the unit with `uid`. An unlocked
    // configuration goes anywhere; a locked one matches case-insensitively,
    // because the UID is hex and its case is a display choice, not part of it.
    bool mayBeSentTo(const QString &uid) const;

    // --------------------------------------------------------- files
    // What a file is, before committing to opening it — so the password prompt
    // happens while the current document is still untouched.
    struct FilePeek {
        bool secure = false;           // a .ct3s rather than a .ct3
        bool requiresPassword = false; // .ct3s that will not open without one
        bool commsProtected = false;   // carries an Edit Protected Comms verifier
    };
    static bool peekFile(const QString &path, FilePeek *out, QString *error = nullptr);

    // Reads either format; the magic decides, not the extension. `password` is
    // used for a .ct3s that requires one and to reveal protected comms on load
    // when it happens to be the right one; it is never required for a plain
    // .ct3, which always opens.
    bool loadFromFile(const QString &path, QString *error = nullptr,
                      const QString &password = QString());
    // Plain .ct3 — indented JSON, everything legible. Refuses while a KEYED
    // section is concealed, for the reason saveSecureToFile does and then some:
    // "everything legible" includes the protected messages, so a session that may
    // not read them must not be the one that writes them out in the clear. See
    // anyKeyedSectionConcealed() for why a keyless concealed section is not that
    // case and does not refuse.
    bool saveToFile(const QString &path, QString *error = nullptr);
    // Binary .ct3s — see secure_file.h. Refuses while a KEYED section is
    // concealed: the body has to be assembled in full to be sealed, and a session
    // that cannot see the protected messages must not be the one that rewrites
    // the file carrying them.
    bool saveSecureToFile(const QString &path, const SecureSaveOptions &options,
                          QString *error = nullptr);

    // True when the document came from (or was last written as) a .ct3s. Save
    // follows the format the file already has rather than silently downgrading
    // a secure configuration to plain JSON.
    bool isSecureFile() const { return m_secureFile; }
    // The options the current .ct3s was saved with, so a plain Save can repeat
    // them without re-asking.
    const SecureSaveOptions &secureOptions() const { return m_secureOptions; }

    // Content — 3 buses (index 0..2 = CAN1..CAN3)
    BusConfig bus[3];
    QList<MathRow> mathRows;
    QList<ConditionRow> conditionRows;
    QList<CounterRow> counterRows;
    QList<TimerRow> timerRows;
    QList<IntegratorRow> integratorRows; // v16: Calculations > Integrators
    QList<ConstantRow> constantRows;
    QList<Table2x16Row> table2x16Rows;   // v12: Calculations > Tables
    QList<Table8x8Row> table8x8Rows;     // the 4x4's replacement (schema 12)
    QString comments;

    // ---- The device script -------------------------------------------------
    //
    // A document describes its script in exactly ONE of two ways, never both:
    //
    //   SOURCE     — Lua the user wrote. Compiled to bytecode at Send time.
    //   BYTECODE   — a compiled image read back off a device by a Get, with no
    //                source in existence for it. Sent back verbatim.
    //
    // The pair is what makes this worth a setter instead of two public fields.
    // A document holding a source AND a stale retained image would send
    // whichever the code happened to test first, and the wrong answer writes a
    // script the user has not seen into a vehicle — worse than the honest
    // deletion that happened before any of this existed. So the invariant is
    //
    //     m_scriptBytecode is non-empty ONLY while m_scriptSource is blank
    //
    // and it is enforced in setScript(), the one writer both setters funnel
    // through, rather than re-argued at each call site. Nothing outside this
    // class can express the stale pair.
    //
    // Neither setter touches the dirty flag: clearContent() and loadBody() write
    // through here too, and both finish by clearing it. Callers that are a user
    // EDIT (the script editor) call setDirty() themselves, as they always did.
    const QString &scriptSource() const { return m_scriptSource; }
    // Blank-or-whitespace is "no script", and this is the ONE definition of it —
    // the compiler asks the same question of the same function, so the two
    // cannot drift into a state where a whitespace-only source drops a retained
    // image and then compiles to nothing, silently stripping the device.
    bool hasScriptSource() const { return !m_scriptSource.trimmed().isEmpty(); }
    // Setting a source SUPERSEDES any retained image, including when the new
    // source is empty: once the document describes its script as text, a
    // compiled image with no source behind it is no longer what this document
    // means, whatever the text says.
    void setScriptSource(const QString &source) { setScript(source, QByteArray()); }

    // The compiled image retained from a device Get: ScriptHeader + bytecode
    // exactly as script_vm.h lays it out, trimmed to code_bytes (no chunk
    // padding), so re-chunking it reproduces the bytes byte for byte. Empty in
    // every document that has not been read off a unit.
    const QByteArray &scriptBytecode() const { return m_scriptBytecode; }
    // Refused, per the invariant, when a source is present — a retained image
    // never overrides written Lua. Callers that must know whether it stuck ask
    // scriptBytecode() afterwards; mapFromDevice does exactly that.
    void setScriptBytecode(const QByteArray &image) { setScript(m_scriptSource, image); }

    ChannelCatalog &catalog() { return m_catalog; }
    const ChannelCatalog &catalog() const { return m_catalog; }

    // Updates every reference to a renamed channel (comms rows, diagnostic
    // channels, math inputs/destinations, condition inputs/targets).
    // Returns the number of references updated, and always emits
    // channelRenamed afterwards — see the signal for who needs it and why.
    int renameChannelReferences(const QString &oldName, const QString &newName);

    // Force every active User Condition's output channel to Boolean, and return
    // how many catalogue entries it had to change.
    //
    // A condition writes 1 or 0 and has never written anything else — the engine
    // assigns the literals — but the channel it writes into was an ordinary user
    // channel that could be declared float, s32, anything, or (very commonly, in
    // files written before channels had types at all) nothing. Declaring it
    // boolean is therefore not a restriction on what conditions can do; it is
    // the document finally saying what was already true.
    //
    // It rewrites minValue, maxValue and decimalPlaces along with dataType,
    // because the four together are one statement. Setting dataType alone would
    // leave a channel the Channel Editor immediately flags as a type that cannot
    // hold its own range, and would still send -1e9..1e9 as the device's clamp
    // for a value that is only ever 0 or 1.
    //
    // Idempotent and safe to call as often as convenient — load, Get, and every
    // close of the User Conditions editor all do.
    int forceConditionOutputsBoolean();

    // Is any section on any bus concealed from THIS viewer? The honest question
    // for anything that DESCRIBES the document's state — the status line's
    // "concealed" / "revealed", and the tests. Not the same as
    // hasCommsPassword() && !commsRevealed(): a document holding only Read Only
    // sections conceals nothing from anyone, password or no password.
    bool anySectionConcealed() const;

    // The narrower question, and the one the two FILE WRITERS ask: is any
    // concealed section one that a password could open?
    //
    // The difference exists because isSectionRevealed() now fails closed, so a
    // Get produces a document full of concealed sections and the plain
    // anySectionConcealed() test would have made "back up this unit" impossible
    // — a worse bug than the one that reversal fixes, and one with no security
    // behind it. A KEYLESS concealed section holds nothing the file could
    // launder: no password for it exists, so writing it out gives the file no
    // reader it did not already have, and the person doing the save could read
    // the same bytes off the unit with any serial tool.
    //
    // A KEYED concealed section is the laundering case the refusal was written
    // for and is still refused: the session cannot read it, a real password for
    // it is in existence, and re-emitting its CAN ID and bit layout in the clear
    // would hand the protocol to somebody who could not open it.
    bool anyKeyedSectionConcealed() const;

    // Channels carried by a marked message. Their definition belongs to that
    // message: change a data type, base resolution or decimal count and the
    // message starts decoding to different numbers with nothing on screen to
    // explain it.
    //
    // TWO predicates, because the single old isChannelProtected() answered two
    // questions that have come apart. Read Only messages are VISIBLE and still
    // NOT EDITABLE, so the set of channels whose values are withheld is no
    // longer the set whose controls are disabled. Pick by what the caller does:
    //   isChannelConcealed   — withhold a VALUE (bit position, factor, range)
    //   isChannelEditLocked  — disable a CONTROL
    // Concealment is lifted by the password; the edit lock is not, because the
    // password buys the right to UNTICK the tier and unticking is what unlocks
    // editing.
    bool isChannelConcealed(const QString &channelName) const;
    bool isChannelEditLocked(const QString &channelName) const;
    // The same two sets, for a dialog that has to grey out a whole list at once.
    QStringList concealedChannelNames() const;
    QStringList editLockedChannelNames() const;

    // ---------------------------------------------------- the untick rule
    // May THIS section be handed to somebody else in this session — its tier
    // LOWERED, or the Message Password that guards it REPLACED? Raising is always
    // free, setting a FIRST password is always free, and removing a section is
    // always allowed, at every tier.
    //
    // One predicate for both acts because they are one privilege. Read Only is
    // the case that proves it: the tier never moves when a password is swapped,
    // so a lowering-only rule left the tier standing over a message its author no
    // longer had the key to.
    //
    // Per section, because the authorisation is per section:
    //
    //   None                always.
    //   ReadOnly / Hidden   a grant for this section: its own password.
    //   Protected           a grant, which now stands for BOTH of that tier's
    //                       proofs — this section's own password and a live
    //                       device confirming Edit Protected Comms. The device
    //                       round trip is what makes the tier stronger than
    //                       Hidden, and the section editor is where it is
    //                       demanded; this predicate is the model's backstop,
    //                       not a substitute for it.
    //
    // A KEYLESS MARKED SECTION MAY NOT BE LOWERED, at any of the three tiers,
    // and that is the same reversal isSectionRevealed just made: an untick is
    // authorised by a password, and there is none. Read Only is included even
    // though it conceals nothing, because what is being given away is the
    // marking rather than the protocol, and the tier that promises "this needs
    // my password to change" cannot answer "there is no password, so help
    // yourself".
    //
    // The repair for a section in that state is to give it a password — setting
    // a FIRST one is free, and applyBusSections() agrees — and then to untick it
    // on a second visit with that password. Removing it is free too, at every
    // tier. What is not available is unticking it with nothing.
    //
    // RAISING a tier, and moving BETWEEN two marked tiers without going down, is
    // not this predicate's business and never reaches applyBusSections' refusal.
    // Rule 2's "prove the current password before changing the attribute" is
    // enforced in the section editor, which is the only place a tier can be moved
    // at all; the model's job is the two changes that give protection AWAY.
    //
    // There deliberately is no document-wide mayLowerProtection() any more. It
    // was commsRevealed(), which is true for every document with no Edit
    // Protected Comms password — so on those documents applyBusSections()
    // performed no check at all and the entire untick rule rested on one
    // QCheckBox handler. That is the widget-level-guard-only shape the chokepoint
    // exists to forbid.
    //
    // `busIndex` as at isSectionRevealed: supplied by every caller that has one,
    // and applyBusSections — the caller that matters, because it is the one that
    // authorises a hand-over — always does.
    bool maySectionLower(const CommsSection &section, int busIndex = -1) const;

    // Replace one bus's sections. THE single chokepoint for the untick rule —
    // every writer that can replace sections must come through here rather than
    // assigning bus[i].sections directly. Refuses, leaving the bus untouched and
    // filling `refusal` with a message fit to show the user, when a section that
    // is still present by NAME comes back with a lower tier OR WITH A DIFFERENT
    // messageKey that maySectionLower() does not authorise FOR THAT SECTION.
    // Sections sharing a name are paired on their IDENTITY — name, base address
    // and device kind — and only then in document order, so one cannot answer for
    // another and a pure REORDER is not read as two of them trading passwords.
    // See the rationale at the definition for why it matches by name, why
    // identity has to come first, and why a widget-level guard is not enough.
    bool applyBusSections(int busIndex, const QList<CommsSection> &next,
                          QString *refusal = nullptr);

    // Names of channels already used by a comms row somewhere ("allocated").
    QStringList allocatedChannelNames() const;
    // All channels generated by receive rows or math destinations.
    QStringList generatedChannelNames() const;

    // Everything that decides which channels exist and what generates them.
    // Document state — file path, dirty flag, title — is not content and is
    // deliberately left alone, so a scratch copy never looks like a document.
    void copyContentTo(Configuration &target) const;
    // This document's content with `patch` applied, left in `target`. The one
    // way to get a live view of a half-edited document; see ConfigPatch.
    void buildLiveView(Configuration &target, const ConfigPatch &patch) const;

signals:
    void dirtyChanged(bool dirty);
    void documentReset(); // after clear/load
    // A channel rename has just been applied to this document's stored
    // references (renameChannelReferences). The signal exists for the one
    // holder of references that walk cannot reach: a grid dialog's PRIVATE
    // working copy. Every grid dialog edits a copy of its slice and assigns it
    // back wholesale on OK — and the channel picker's Edit… commits a rename
    // to the document IMMEDIATELY (that is what makes the new name selectable),
    // while those dialogs are still open under it. A copy snapshotted before
    // the rename still carries the old name, so its OK would write that name
    // straight back over the rows the walk just fixed — and by then the old
    // catalog entry is gone, so the reference does not merely revert, it
    // dangles. Each open grid dialog listens and runs the SAME walk
    // (renameChannelRefs, comms_types.h) over its own rows. Emitted even when
    // zero stored references changed, because the only reference to the old
    // name may live in a working copy this document has never seen.
    void channelRenamed(const QString &oldName, const QString &newName);

private:
    // Reads the content keys out of `body` (the root object, or the decrypted
    // payload when the file is read-protected — the two carry identical keys).
    // `fileVersion` is the schema the FILE was written at, which for a pre-v8
    // read-protected .ct3 is knowable only from the legible wrapper; the comms
    // sections need it to decide whether the pre-14 protection keys migrate.
    // Protected's verdict, shared by isSectionRevealed and maySectionLower so the
    // two cannot answer it differently. See the definition for what its keyless
    // arm used to be and why it had to go.
    bool protectedSectionProved(const CommsSection &section, int busIndex = -1) const;

    // "Is there a grant that proves THIS section?" — with the bus matched when
    // the caller knows it, and NOT matched when the caller genuinely cannot.
    //
    // Some callers cannot be given one. isSectionRevealed() and
    // maySectionLower() are asked about sections that are not in this document
    // at all — a live view's copy, the incoming candidate in applyBusSections, a
    // row a report is walking — so a bus index would have to be supplied by
    // every one of a dozen display call sites, and a display site that supplied
    // the wrong one would leak rather than fail. Those pass -1. Everything that
    // is already walking bus[i], and applyBusSections above all, passes the real
    // index, because a narrower question is free there and free is the right
    // price for it.
    //
    // What -1 rests on is the KEY: this matches a grant on the lower-cased name
    // AND on the messageKey it was proved against, so the cross-bus impostor of
    // the hole this closes — same name, attacker's own password — does not
    // match, and neither does a section whose password has since been replaced.
    //
    // That argument needs kNoAccessKey excluded to hold at all, and the header
    // used to claim it whole without saying so. kNoAccessKey is what EVERY
    // keyless section carries, so it discriminates nothing, and with the key
    // contributing nothing the match fell back to the name — the attacker's
    // chosen value. A keyless section therefore matches NO grant here, ever.
    //
    // What survives, stated rather than pretended away: two sections that
    // genuinely SHARE a password open each other across buses. That is inherent
    // to using the key as the discriminator, it needs the real secret to reach,
    // and sectionAccessGranted() — the exact, bus-checked question the dialogs
    // ask — does not have it.
    bool sectionGrantProves(const CommsSection &section, int busIndex = -1) const;

    void loadBody(const QJsonObject &body, int fileVersion);
    // The content keys, without the file/lock wrapper. Canonicalised by Qt's
    // key sorting, which is what makes the write-protection MAC reproducible.
    QJsonObject buildBody() const;

    // THE one door for the script pair — see the block beside scriptSource().
    // Every route into a document (the editor, a file load, a Get, a copy, a
    // clear) reaches the two fields only through here, so the "source and a
    // stale image at once" state has no way to come into existence and no
    // caller has to remember the rule.
    void setScript(const QString &source, const QByteArray &retainedBytecode);

    QString m_filePath;
    QString m_configTitle;
    bool m_dirty = false;
    ChannelCatalog m_catalog;

    // The script pair. Private precisely so the invariant above can be a fact
    // about the type rather than a convention: these were a single public
    // QString until a Get learned to keep a device's compiled image, and a
    // public pair is a pair anybody can put out of step.
    QString m_scriptSource;
    QByteArray m_scriptBytecode;

    // Access state. m_commsKey is live key material and stays out of
    // copyContentTo, so a scratch live view can never carry the key.
    // m_accessVerifiers and m_commsRevealed ARE copied, and must be: a live view
    // is a Configuration, so maySectionLower() can be asked of it, and a copy
    // that omitted the verifier set would answer commsRevealed() == true and
    // authorise a Protected downgrade the document it came from would refuse.
    AccessVerifierSet m_accessVerifiers;
    FleetIdentity m_fleetIdentity;
    UploadPolicy m_uploadPolicy;
    AccessKey m_commsKey = kNoAccessKey;
    QString m_lockedDeviceUid;                  // empty = not locked to a device
    AccessKey m_deviceLockKey = kNoAccessKey;   // authorises changing the lock
    bool m_commsRevealed = false;
    // One section whose own challenge has been met this session: which section,
    // and which secret was given for it. See grantSectionAccess for why the bus
    // and the key are both part of the record and what each of them closes.
    //
    // A LIST rather than a hash because the lookup runs both ways — the exact
    // (bus, name) one and the key-matched scan sectionGrantProves() needs — and
    // a session holds a handful of these, not a table.
    struct SectionGrant {
        int busIndex = -1;
        QString lowerName; // LOWER-CASED, matching every other name comparison here
        AccessKey provedKey = kNoAccessKey; // the section's messageKey when it was proved
    };
    // Copied by copyContentTo alongside m_commsRevealed and for the identical
    // reason: a live view is a Configuration, so it can be asked these questions,
    // and a copy that answered them differently from its source would authorise
    // what the source refuses.
    QList<SectionGrant> m_sectionGrants;

    bool m_secureFile = false;
    SecureSaveOptions m_secureOptions;
};

} // namespace ct
