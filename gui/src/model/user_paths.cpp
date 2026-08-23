#include "user_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace ct {

namespace {

// The folder names, together, so the layout can be read at a glance and a
// rename is one edit rather than a search. These are what the user sees in
// Explorer, so they are named for their CONTENTS: the window is called
// Communications Setup, but what it writes are communications templates.
const QLatin1String kProduct("CAN Triple Device Manager");
const QLatin1String kConfigurations("Configurations");
const QLatin1String kCommsTemplates("Communications Templates");
const QLatin1String kFirmwareImages("Firmware");
const QLatin1String kFirmwareBackups("Firmware Update Backups");
const QLatin1String kDeviceScripts("Scripts");

} // namespace

QString programRoot()
{
    return QCoreApplication::applicationDirPath();
}

QString configurationsDirectory()
{
    return programRoot() + QLatin1Char('/') + kConfigurations;
}

QString commsTemplatesDirectory()
{
    return programRoot() + QLatin1Char('/') + kCommsTemplates;
}

QString firmwareImagesDirectory()
{
    return programRoot() + QLatin1Char('/') + kFirmwareImages;
}

QString userFilesRoot()
{
    // DocumentsLocation, not AppDataLocation: both of these are files the user
    // may need to find by hand months later, and AppData is where files go to
    // be forgotten.
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
           + QLatin1Char('/') + kProduct;
}

QString firmwareBackupsDirectory()
{
    return userFilesRoot() + QLatin1Char('/') + kFirmwareBackups;
}

QString deviceScriptsDirectory()
{
    return userFilesRoot() + QLatin1Char('/') + kDeviceScripts;
}

bool ensureWritableDirectory(const QString &dir, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    if (!QDir().mkpath(dir)) {
        return fail(QStringLiteral("This folder could not be created:\n\n%1")
                        .arg(QDir::toNativeSeparators(dir)));
    }

    // THE PROBE. mkpath returns true for a directory that already exists, so on
    // an installed copy it says nothing at all about whether this account may
    // write there — and under Program Files it ordinarily may not. Writing a
    // real file is the only way to ask a question the ACL is too subtle to
    // answer by reading: inherited denies, a policy that stripped the ACE, a
    // folder created by an older Setup that never granted anything.
    //
    // The name starts with a dot and ends in .tmp so that if a crash ever
    // stranded one it reads as leftovers rather than as somebody's template.
    QFile probe(dir + QLatin1String("/.ct-writable.tmp"));
    if (!probe.open(QIODevice::WriteOnly)) {
        const QString native = QDir::toNativeSeparators(dir);
        // Two different problems wearing one error code, and they need
        // different advice. Under Program Files this is the expected state for
        // a copy whose installer did not grant the folder — pointing at a
        // reinstall is the repair. Anywhere else it is an ordinary permission
        // or read-only-media problem and guessing would be noise.
        if (dir.contains(QLatin1String("Program Files"), Qt::CaseInsensitive)) {
            return fail(
                QStringLiteral(
                    "This folder is inside the installed program and Windows will not let CAN "
                    "Triple Device Manager write to it:\n\n%1\n\n"
                    "The installer is what grants permission on this folder, so a copy installed "
                    "by an older Setup — or one whose permissions a system policy has since "
                    "removed — cannot save here. Re-running the current installer repairs it. "
                    "Until then, save somewhere under Documents instead.")
                    .arg(native));
        }
        return fail(QStringLiteral("Windows will not let this program write to:\n\n%1\n\n%2")
                        .arg(native, probe.errorString()));
    }
    probe.close();
    probe.remove();
    return true;
}

} // namespace ct
