// Turns a .ct3 into a sealed .ct3s package — the work behind the Secure
// Configuration Builder's Build button, kept out of the dialog so the dialog
// can be tested without linking the mapper, the script compiler and Lua.
//
// What a package contains after this (see secure_file.h for the container and
// sealed_stream.h for the stream):
//   - the policy: the licence and hardware matches, the version stamp, WHICH
//     passwords the install sets (never their keys), and a precomputed
//     licence proof the relay can check a unit against without holding the
//     fleet key;
//   - the install stream: every command frame of a Send, mapped here and
//     sealed under keys derived from the fleet key, access-password writes
//     included;
//   - optionally, an editable copy of the configuration, wrapped under a
//     package password and nothing else.
// The fleet passphrase is used here to derive the key, and goes no further:
// neither it nor the derived key is written anywhere.
#pragma once

#include <QString>
#include <QStringList>

#include "secure_file.h"

namespace ct {

struct PackageBuildRequest
{
    QString sourcePath;      // the .ct3 to package; loaded from disk, never from memory
    QString outputPath;      // the .ct3s to write
    QString fleetPassphrase; // the Firmware Key passphrase the target fleet proves
    // The policy as the Builder collected it: matches, version stamp, and the
    // password updates WITH their derived keys. The keys are moved into the
    // sealed stream and withheld from the policy that is written.
    SecurePackagePolicy policy;
    // Empty with includeEditable = false is an install-only package.
    QString openPassword;
    bool includeEditable = false;
};

struct PackageBuildResult
{
    bool ok = false;
    QString error;
    QStringList summary; // one sentence per line, for the dialog to show
    int frameCount = 0;
};

PackageBuildResult buildSecurePackage(const PackageBuildRequest &request);

} // namespace ct
