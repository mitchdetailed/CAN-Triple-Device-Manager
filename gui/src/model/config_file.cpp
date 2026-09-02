#include "config_file.h"

#include <QFile>

#include "secure_file.h"

namespace ct {

const char *const kConfigPreambleMagic = "CAN Triple Device Manager configuration";

namespace {

// One place per key, so the writer and the reader cannot drift apart on a
// spelling. The trailing space is part of the key: "format " matches the line
// "format 2" and not a hypothetical "formatting".
const QByteArray kCrLf = QByteArrayLiteral("\r\n");
const QByteArray kKeyFormat = QByteArrayLiteral("format ");
const QByteArray kKeySchema = QByteArrayLiteral("schema ");
const QByteArray kKeyWrittenBy = QByteArrayLiteral("written-by ");
constexpr char kDosEof = '\x1a';

QByteArray buildPreamble(int schemaVersion, const QString &writtenBy)
{
    QByteArray text;
    text += kConfigPreambleMagic;
    text += kCrLf;
    text += kKeyFormat + QByteArray::number(kConfigFileFormatVersion) + kCrLf;
    text += kKeySchema + QByteArray::number(schemaVersion) + kCrLf;
    // Latin-1 and stripped of anything that would break the line structure. The
    // version comes from CT_APP_VERSION and has never been anything but digits
    // and dots; this is here so that if it ever is, the preamble stays parseable
    // rather than becoming a file nothing can open.
    QByteArray who = writtenBy.toLatin1();
    who.replace('\r', ' ').replace('\n', ' ').replace(kDosEof, ' ');
    text += kKeyWrittenBy + who + kCrLf;
    text += kDosEof;

    // Truncate before padding. A pathological writtenBy is the only way to get
    // here over-long, and a preamble that overran would push the body off its
    // fixed offset and produce a file this program could not read back.
    text.truncate(kConfigPreambleBytes);
    text.append(kConfigPreambleBytes - text.size(), '\0');
    return text;
}

// The value after `key` on its own line, or an empty QByteArray. Scans the
// preamble only, which is a fixed 128 bytes we already hold.
QByteArray valueFor(const QByteArray &preamble, const QByteArray &key)
{
    int from = 0;
    while (from < preamble.size()) {
        int end = preamble.indexOf(kCrLf, from);
        if (end < 0)
            end = preamble.size();
        const QByteArray line = preamble.mid(from, end - from);
        if (line.startsWith(key))
            return line.mid(key.size()).trimmed();
        if (end >= preamble.size())
            break;
        from = end + kCrLf.size();
    }
    return QByteArray();
}

bool parsePreamble(const QByteArray &preamble, ConfigFileInfo *out, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    if (preamble.size() < kConfigPreambleBytes
        || !preamble.startsWith(kConfigPreambleMagic)) {
        return fail(QStringLiteral("Not a CAN Triple configuration file"));
    }

    ConfigFileInfo info;
    bool ok = false;
    info.formatVersion = valueFor(preamble, kKeyFormat).toInt(&ok);
    if (!ok || info.formatVersion <= 0)
        return fail(QStringLiteral("This configuration file is damaged and cannot be opened."));
    // Checked before anything is decrypted, and the message names the remedy.
    // A newer FORMAT means the layout itself moved, so there is nothing this
    // build can usefully attempt.
    if (info.formatVersion > kConfigFileFormatVersion) {
        return fail(QStringLiteral("This file was saved by a newer version of "
                                   "CAN Triple Device Manager and can't be opened."));
    }
    // Absent or unparseable is tolerated rather than fatal: the sealed body
    // carries the version that decides anything, and refusing here would turn a
    // scratched label into an unopenable configuration.
    info.schemaVersion = valueFor(preamble, kKeySchema).toInt();
    info.writtenBy = QString::fromLatin1(valueFor(preamble, kKeyWrittenBy));

    if (out)
        *out = info;
    return true;
}

} // namespace

bool isBinaryConfigFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return f.read(qstrlen(kConfigPreambleMagic)) == QByteArray(kConfigPreambleMagic);
}

bool peekBinaryConfigFile(const QString &path, ConfigFileInfo *out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QByteArray preamble = f.read(kConfigPreambleBytes);
    f.close();
    return parsePreamble(preamble, out, error);
}

bool writeBinaryConfigFile(const QString &path, const QByteArray &plainBody, int schemaVersion,
                           const QString &writtenBy, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    // Standard mode, no password, no embedded key. A .ct3 has no password to
    // wrap under and confers no protected-comms authority — both of those are
    // what Save Secure Config is for, and quietly giving them to every plain
    // save would make the two formats the same thing wearing two names.
    SecureSaveOptions options;
    options.embeddedCommsKey = kNoAccessKey;

    QByteArray blob;
    if (!sealSecureBlob(plainBody, options, &blob, error))
        return false;

    // QFile IN PLACE, not QSaveFile, and the reasoning is inherited from the
    // JSON writer this replaced rather than re-derived — because the one
    // premise that changed does not change the conclusion.
    //
    // QSaveFile writes beside the target and renames over it, which cannot
    // replace a file another process holds open. On Windows that is not exotic:
    // OneDrive or Dropbox syncing the folder, a backup agent, an antivirus
    // scanner mid-read. This was tried here first, and the test suite failed
    // immediately on the most ordinary shape there is — a file opened, closed
    // and still owned by its QTemporaryFile — with commit() returning
    // "Access is denied." while a plain QFile wrote the same path without
    // complaint. A save that refuses because something is syncing the folder is
    // a bug users would hit constantly.
    //
    // What DID change is the cost of a torn write: a truncated format-1 .ct3
    // was invalid JSON that a person could still open in an editor and salvage
    // by hand, while a truncated format-2 .ct3 fails the payload's tag and is
    // refused whole. That is worse, and it is still the better trade, because
    // of what a torn file actually costs here. Both the old writer and this one
    // detect the failure — write() short or flush() false is returned, not
    // swallowed — so the document stays dirty and stays in memory. On a File >
    // Save the bytes being overwritten are an older copy of the very document
    // still open in front of the user, who can save it somewhere else. Nothing
    // that is not still in hand is lost.
    //
    // The whole file is one buffer and one write() for that reason: there is no
    // state where the preamble landed and the body did not.
    const QByteArray whole = buildPreamble(schemaVersion, writtenBy) + blob;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(f.errorString());
    // flush() rather than trusting write(): QFile buffers, and QFile::error()
    // straight after write() reads NoError for anything the OS has merely
    // queued — so on a full disk this would "succeed", the dirty flag would
    // clear, and the user would be left with a truncated file and no warning.
    if (f.write(whole) != whole.size() || !f.flush())
        return fail(f.errorString());
    return true;
}

bool readBinaryConfigFile(const QString &path, QByteArray *plainBody, ConfigFileInfo *info,
                          QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(f.errorString());
    const QByteArray raw = f.readAll();
    f.close();

    ConfigFileInfo parsed;
    if (!parsePreamble(raw.left(kConfigPreambleBytes), &parsed, error))
        return false;

    // openSecureBlob's failure messages are written for a .ct3s and talk about
    // passwords, which no .ct3 has ever had. Its verdict is what matters, not
    // its wording, so the wording is replaced: for a file that cannot require a
    // password there is exactly one explanation left for a tag that does not
    // verify, and naming it is more use than repeating the container's hedge.
    SecureFileInfo blobInfo;
    if (!openSecureBlob(raw.mid(kConfigPreambleBytes), plainBody, &blobInfo, nullptr))
        return fail(QStringLiteral("This configuration file is damaged and cannot be opened."));

    if (info)
        *info = parsed;
    return true;
}

} // namespace ct
