#include "package_builder.h"

#include <QFileInfo>
#include <QRandomGenerator>

#include "../protocol/config_transfer.h"
#include "../protocol/device_session.h"
#include "../protocol/wire_structs.h"
#include "../scripting/script_compiler.h"
#include "access_keys.h"
#include "configuration.h"
#include "sealed_stream.h"

namespace ct {

namespace {

QVector<ControlCanPayload> busSetupsFor(const Configuration &config)
{
    QVector<ControlCanPayload> setups;
    for (int i = 0; i < 3; ++i) {
        ControlCanPayload setup{};
        setup.bus_idx = quint8(i + 1);
        setup.mode = config.bus[i].enabled ? 1 : 0;
        setup.baud_rate = busRateHz(config.bus[i].rateKbps);
        setup.data_baud_rate = busRateHz(config.bus[i].dataRateKbps);
        setup.termination = config.bus[i].termination ? 1 : 0;
        setups.append(setup);
    }
    return setups;
}

// The access-password writes a package applies, as frames. Ahead of the
// configuration in the stream, for the reason main_window gives for the plain
// path: the keys live in the config store's write-once header and have to be
// in RAM before the SAVE that commits it. A ticked row with kNoAccessKey is a
// clear, exactly as the Builder's checkbox convention says.
QList<PlannedFrame> passwordFrames(const SecurePackagePolicy &policy)
{
    QList<PlannedFrame> out;
    const auto add = [&](bool wanted, AccessKey key, AccessFunction fn, int slot,
                         const QString &label) {
        if (!wanted)
            return;
        PlannedFrame f;
        f.cmd = CMD_WRITE_ACCESS_KEYS;
        f.payload = device_session::accessKeyWritePayload(fn, key, key == kNoAccessKey, slot);
        f.stage = QStringLiteral("Setting %1 password").arg(label);
        out.append(f);
    };
    add(policy.setSend, policy.sendKey, AccessFunction::SendConfiguration, 1,
        QStringLiteral("Send Configuration"));
    add(policy.setGet, policy.getKey, AccessFunction::GetConfiguration, 1,
        QStringLiteral("Get Configuration"));
    for (int i = 0; i < 4; ++i)
        add(policy.setCommsSlot[i], policy.commsSlotKey[i], AccessFunction::EditProtectedComms,
            i + 1, QStringLiteral("Protected Comms slot %1").arg(i + 1));
    return out;
}

QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(b.data()),
                                         reinterpret_cast<quint32 *>(b.data() + b.size()));
    return b;
}

} // namespace

PackageBuildResult buildSecurePackage(const PackageBuildRequest &request)
{
    PackageBuildResult result;
    const auto fail = [&](const QString &why) {
        result.ok = false;
        result.error = why;
        return result;
    };

    // Loaded from disk every time, never packaged from the editor: an unsaved
    // edit must not reach a customer's device without existing in a file.
    Configuration source;
    QString error;
    if (!source.loadFromFile(request.sourcePath, &error))
        return fail(error.isEmpty() ? QStringLiteral("That configuration could not be opened.")
                                    : error);

    // Mapped here, at build time, which is what lets the customer's Manager
    // never map it at all. A configuration that will not map is refused now,
    // with the count, rather than producing a package that fails on a bench.
    const MappingResult mapped = mapWithScript(source);
    if (!mapped.ok())
        return fail(QStringLiteral("This configuration cannot be programmed onto a device: the "
                                   "mapping reports %1 error(s). Open it and use Check "
                                   "Channels to see them.")
                        .arg(mapped.errors.size()));

    const QByteArray fleetKey = deriveLicenseKey(request.fleetPassphrase);
    if (fleetKey.size() != kLicenseKeyBytes)
        return fail(QStringLiteral("The Firmware Key could not be derived."));

    QList<PlannedFrame> frames = passwordFrames(request.policy);
    frames += ConfigTransfer::planInstallFrames(mapped.tables, busSetupsFor(source),
                                                request.policy.configVersion,
                                                source.effectiveTitle(),
                                                sealedInnerPayloadBudget());
    const QByteArray stream = buildSealedStream(fleetKey, frames, &error);
    if (stream.isEmpty())
        return fail(error);

    // The policy that is WRITTEN: the same demands, the password keys withheld
    // (they are in the stream now), the fleet key replaced by a proof the relay
    // can check a unit against — a nonce this Builder chose and the answer the
    // device must give under the PROVE label. Anyone can verify that answer;
    // only the key can produce it.
    SecurePackagePolicy written = request.policy;
    written.key.clear();
    written.keysWithheld = true;
    written.keyProofNonce = randomBytes(kAccessChallengeBytes);
    written.keyProofMac = licenseProveExpected(fleetKey, written.keyProofNonce);
    if (!written.isValid())
        return fail(QStringLiteral("The package policy could not be sealed."));

    SecureSaveOptions options = source.secureOptions();
    options.policy = written;
    options.installStream = stream;
    options.includeEditable = request.includeEditable;
    options.openPassword = request.includeEditable ? request.openPassword : QString();
    options.embeddedCommsKey = source.commsKey();
    if (!source.saveSecureToFile(request.outputPath, options, &error))
        return fail(error.isEmpty() ? QStringLiteral("The package could not be written.") : error);

    result.ok = true;
    result.frameCount = frames.size();
    result.summary << QStringLiteral("Package written to %1.")
                          .arg(QFileInfo(request.outputPath).fileName());
    QStringList matched;
    if (!written.matchManufacturer.isEmpty())
        matched << QStringLiteral("manufacturer");
    if (!written.matchModel.isEmpty())
        matched << QStringLiteral("model");
    if (!written.matchVersion.isEmpty())
        matched << QStringLiteral("version");
    if (!written.matchMcuId.isEmpty())
        matched << QStringLiteral("MCU ID");
    if (written.matchSerialSet)
        matched << QStringLiteral("HW serial");
    matched << QStringLiteral("Firmware Key");
    result.summary << QStringLiteral("It installs only on devices matching: %1.")
                          .arg(matched.join(QStringLiteral(", ")));
    result.summary << QStringLiteral("Its %1 install frames are sealed under that key and are "
                                     "decrypted by the device, never by the Manager.")
                          .arg(frames.size());
    if (written.changesPasswords())
        result.summary << QStringLiteral("It also sets device passwords as it installs.");
    if (written.configVersion != 0)
        result.summary << QStringLiteral("It stamps configuration version %1 on the unit.")
                              .arg(written.configVersion);
    result.summary << (request.includeEditable
                           ? QStringLiteral("It carries an editable copy that opens only with "
                                            "the package password.")
                           : QStringLiteral("It is install-only: it cannot be opened in the "
                                            "Manager at all."));
    return result;
}

} // namespace ct
