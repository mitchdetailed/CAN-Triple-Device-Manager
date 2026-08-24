// Multi-command transfer procedures: Send Configuration (clear + chunked
// writes + optional read-back verify) and Get Configuration (chunked reads).
#pragma once

#include <QObject>

#include "../model/device_mapper.h"
#include "device_link.h"
#include "device_session.h"

namespace ct {

class ConfigTransfer : public QObject
{
    Q_OBJECT
public:
    // Sends tables (and, when provided, per-bus CONTROL_CAN setups) to the
    // device. When saveToFlash is set, a final SAVE_TO_FLASH step persists the
    // just-sent (and verified) configuration in one operation. Deletes itself
    // when finished.
    //
    // configVersion is the configuration's own revision number — the caller
    // passes Configuration::fleetIdentity().configVersion — and rides that same
    // SAVE_TO_FLASH as its payload, so the version the device reports can never
    // describe tables other than the ones it was committed with. It is sent even
    // when zero: an unversioned configuration has to CLEAR whatever version was
    // there before, or the unit would go on claiming a revision it is no longer
    // running and the uploader's "is this newer?" test would refuse the very
    // package that fixes it. Ignored when saveToFlash is false, because then
    // nothing is being committed for it to belong to.
    //
    // deviceAccess is whatever the caller already learned from
    // CMD_READ_ACCESS_KEYS about the unit in front of it, and it is here for one
    // reason. Read-back verification is a run of CMD_READ_* commands, and the
    // firmware gates every one of those on the Get Configuration password (see
    // serial_proto.c, tableForRead). A Send proves ACCESS_FN_SEND and nothing
    // else, deliberately: the whole point of splitting the two passwords is that
    // an operator can be trusted to deploy a configuration without being able to
    // read one back off a customer's unit, so a Send must never stop to ask for
    // the second one. On a device that guards Get, then, every verify step is a
    // round trip that can only come back ERR_LOCKED, and telling send() about it
    // means none of them are issued. Leaving it default (unsupported) is safe —
    // the verify phase is built as before and the ERR_LOCKED is dealt with when
    // it arrives, which it must be anyway for the device this side never asked
    // about. It sits after `parent` rather than beside the other transfer
    // options because every existing call passes `this` positionally in that
    // slot, and a pointer slotted into a bool-shaped gap converts silently.
    static ConfigTransfer *send(DeviceLink *link, const DeviceTables &tables, bool verify,
                                const QVector<ControlCanPayload> &busSetups = {},
                                bool saveToFlash = false, quint16 configVersion = 0,
                                const QString &configName = {}, bool resetAfter = false,
                                QObject *parent = nullptr,
                                const device_session::AccessState &deviceAccess = {});
    // Reads all four tables from the device. Deletes itself when finished.
    static ConfigTransfer *get(DeviceLink *link, QObject *parent = nullptr);

    // Test seam: build the real plan — a full Send with verify (true tables,
    // one record each) or a Get — without a link, and hand back the read-
    // classification guard's verdict (empty = healthy). Exists so the
    // classification test fails the moment a read step and DeviceLink's lists
    // disagree, instead of the disagreement shipping and being found on
    // hardware — which has now happened for six read commands.
    static QString planClassificationFaultForTest(bool getPlan);

    void cancel();

    // Configuration name read back from the device (get() only, valid after
    // tablesReady). Empty if the firmware is too old to support it.
    QString deviceConfigName() const { return m_deviceConfigName; }

    // What the three buses were running (get() only, valid after tablesReady).
    // EMPTY when the firmware could not tell us — the caller must then fall back
    // to assuming bring-up rates and say so, which is what a Get did before
    // CMD_READ_CAN_SETUP existed. Empty is therefore "unknown", never "off".
    QVector<ControlCanPayload> deviceBusSetup() const { return m_deviceBusSetup; }

    // Optional steps the device NACKed (e.g. v2-only commands on v1
    // firmware), plus the read-back verification when it had to be abandoned.
    // Read from the finished() handler.
    QStringList skippedStages() const { return m_skippedStages; }

    // True if the final Save-to-Flash step was skipped because the firmware is
    // too old to support it (the config was sent but not persisted).
    bool flashSaveWasSkipped() const { return m_flashSaveSkipped; }

    // True if the configuration was sent and committed but never read back,
    // because the device guards CMD_READ_* behind a Get Configuration password
    // this connection has not proved. The transfer still reports success — the
    // writes were all ACKed — so a caller that tells the user its send was
    // "verified" must consult this and say "sent" instead, or it is claiming a
    // check that did not happen.
    bool verifyWasSkipped() const { return m_verifySkipped; }

    // True if any step was skipped because its reply was LOST (a timeout that
    // outlived every retry), as distinct from the device declining it with a
    // NACK. The distinction is the whole point: a NACK on an optional read
    // means "this firmware genuinely does not have that table", which is a
    // truthful partial result; a lost reply means "I do not know what that
    // table held" — and because a Get maps an absent reply to an EMPTY table,
    // that silently reads as "the table is empty" in the result. Harmless when
    // a Get is only being displayed; NOT harmless when the result is about to
    // be written as a backup and the device then erased. A caller taking such a
    // backup MUST refuse it if this is true. See config_transfer.cpp's optional
    // branch.
    bool anyReplyLost() const { return m_replyLost; }

    // True if the transfer FAILED and the device's reason was ERR_LOCKED — it
    // declined the step for want of a password this session has not proved.
    //
    // Worth distinguishing from every other failure because it is the one the
    // user can do something about. On a Get it means the Get Configuration
    // password (ACCESS_FN_GET) has not been proved - nothing more. The device's
    // old per-message read gate, which refused a chunk carrying protected
    // records until Protected Comms was proved, was removed in 2.3.0 (see the
    // read-path note in serial_proto.c); do not re-introduce a Protected Comms
    // prompt on a locked Get, which would send the user to the wrong password.
    bool failedLocked() const { return m_failedLocked; }

signals:
    void progress(int done, int total, const QString &stage);
    void finished(bool ok, const QString &error);
    void tablesReady(const ct::DeviceTables &tables); // get() only, before finished()

private:
    explicit ConfigTransfer(DeviceLink *link, QObject *parent);

    struct Step {
        quint8 cmd = 0;
        QByteArray payload;
        QString stage;
        int timeoutMs = DeviceLink::kDefaultTimeoutMs;
        // For read steps: where to append response items.
        // Index into the DeviceTables read-back switch in runNext(), in the same
        // order as the firmware's EngineTable enum. -1 = not a table read.
        int table = -1;
        bool optional = false; // NACK tolerated (e.g. TX table on v1 firmware)
        bool skipIfUnsupported = false; // tolerate only ERR_INVALID_CMD, not real errors
        bool captureName = false; // read step: response is the 32-byte config name
        bool captureBusSetup = false; // read step: response is ControlCanPayload[3]
        bool captureDeviceChannels = false; // read step: response is DeviceChannelsConfig
        bool captureMsgPasswords = false;   // read step: response is MessagePasswordRecord
        QByteArray expectedEcho; // verify steps: expected response payload
        // Marks the read-back phase. expectedEcho would nearly do, but this is
        // also what runNext() scans forward over to abandon the REST of the
        // phase after one locked read, and "skip until the flag stops" wants to
        // be a flag rather than an inference from a payload being non-empty.
        bool isVerify = false;
    };

    void buildSendSteps(const DeviceTables &tables, bool verify,
                        const QVector<ControlCanPayload> &busSetups, bool saveToFlash,
                        quint16 configVersion, const QString &configName, bool resetAfter);
    void buildGetSteps();
    // Runs over the freshly built plan; a non-empty m_buildFault stops the
    // transfer before its first request, with the fix in the message. See the
    // definition for the six-entry history this guard answers to.
    void validateReadClassification();
    void runNext();

    DeviceLink *m_link;
    QList<Step> m_steps;
    QString m_buildFault; // non-empty: the plan failed classification and must not run
    int m_index = 0;
    bool m_cancelled = false;
    bool m_isGet = false;
    bool m_flashSaveSkipped = false;
    bool m_verifySkipped = false;
    bool m_failedLocked = false;
    bool m_replyLost = false;
    QStringList m_skippedStages;
    DeviceTables m_readTables;
    QString m_deviceConfigName;
    QVector<ControlCanPayload> m_deviceBusSetup;
};

} // namespace ct
