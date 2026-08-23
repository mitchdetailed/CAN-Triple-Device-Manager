// The .ct3 container — what File > Save and File > Save As write.
//
// FORMAT 2 IS BINARY. Format 1 was indented JSON: every CAN ID, bit layout,
// scaling factor and channel name legible in Notepad, findable by a text search
// of a whole drive, and quotable into an email by anyone who opened it. Nothing
// about that was a security control and it is not being described as one now —
// see "what this is not" below — but a configuration is somebody's protocol
// work, and having it readable by accident, by any program that opens files, is
// a property nobody asked for and several people were surprised by.
//
// The file is a readable PREAMBLE followed by the sealed container from
// secure_file.h — the same bytes a .ct3s carries and the same sealing the .ct3t
// templates already go through. There is one implementation of that sealing and
// this file does not contain a second one.
//
// ---------------------------------------------------------------------------
// LAYOUT
//
//   Bytes 0..127   the preamble: US-ASCII, CRLF-terminated lines, fixed size.
//   Bytes 128..    the sealed container, byte-for-byte what sealSecureBlob()
//                  produced — its own 64-byte header first, then the carrier.
//
// The preamble reads, in this order and with exactly these keys:
//
//     CAN Triple Device Manager configuration<CR><LF>
//     format 2<CR><LF>
//     schema 19<CR><LF>
//     written-by 1.3.0<CR><LF>
//     <0x1A>
//     ...NUL padding out to 128 bytes...
//
// Fixed at 128 bytes so the body starts at a constant offset and the reader
// never has to scan for where the text stops — a scan is a place to get a
// length wrong, and this one would be scanning attacker-supplied bytes.
//
// The 0x1A is DOS end-of-file. `type config.ct3` at a command prompt therefore
// prints the four lines and stops, instead of spraying the terminal with
// ciphertext. That is a courtesy, not a guarantee: PowerShell's Get-Content
// ignores it, and nothing depends on the behaviour either way.
//
// ---------------------------------------------------------------------------
// THE PREAMBLE IS A LABEL. THE BODY IS THE TRUTH.
//
// Every value in the preamble is cleartext and unauthenticated, so anybody with
// a hex editor can change `schema 19` to say anything at all. None of it is
// therefore allowed to be the authority on anything:
//
//   - The SEALED BODY carries its own fileType and fileVersion, exactly as the
//     JSON wrapper used to and exactly as a .ct3s body already does, and those
//     are what Configuration::loadFromFile believes. They are covered by the
//     payload's tag, so a flipped bit in them is caught rather than obeyed.
//   - The preamble's `schema` is written so a human, a support ticket or a
//     script can see what a file holds without this program and without the
//     key. It is also checked on read, but only to REFUSE: a file whose
//     preamble claims a schema or format from the future is rejected with a
//     sentence about upgrading rather than opened and misread. Refusing on a
//     lie costs the liar nothing they did not already have.
//   - `written-by` is for people. It is never read back for a decision, the
//     same rule the JSON wrapper's writtenBy followed, because a parser that
//     branches on an application version is deciding structure from a number
//     that does not describe structure.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS NOT
//
// It is not protection, and the difference matters enough to spell out where
// somebody will read it before repeating a claim to a customer.
//
// The key that decrypts a .ct3 travels inside the .ct3, exactly as it does for
// a standard .ct3s. This defeats Notepad, a text search, a grep for "0x640",
// and any tool that does not implement this format. It does NOT defeat someone
// who reads this source or disassembles the application — the key is in the
// file and a determined reader will find it. Anyone who needs more than that
// wants Save Secure Config with a password, which is the only thing here that
// cannot be opened without a secret the file does not contain.
//
// A .ct3 also confers no concealment. Hidden and Protected messages are fully
// readable and editable in the app once the file is open TO SOMEONE WHO HOLDS
// THEIR PASSWORDS — the bytes being opaque changes nothing about that. What it
// did change is who may WRITE such a file: saveToFile used to refuse a document
// holding messages this session could not read, because the JSON it emitted
// would have spelled them out. Sealing the body removed that leak, so the
// refusal went with it and the marking is enforced at the editor instead.
//
// ---------------------------------------------------------------------------
// READING FORMAT 1
//
// Every .ct3 written before this release is JSON, and every one of them still
// opens: Configuration::loadFromFile routes on what the file actually starts
// with, never on the extension. A format-1 file loaded and saved comes back as
// format 2, which is the only migration there is and it needs no user action.
// There is deliberately no way to write format 1 again — an escape hatch back
// to legible JSON would be the leak this change exists to close, sitting behind
// a menu item.
#pragma once

#include <QByteArray>
#include <QString>

namespace ct {

// The preamble is exactly this long, and the sealed container begins straight
// after it.
constexpr int kConfigPreambleBytes = 128;

// 1 was indented JSON. Bump only for a change to the LAYOUT above — the shape
// of the configuration inside the body is what the schema version tracks, and
// conflating the two is how a file ends up claiming a version of the wrong
// thing.
constexpr int kConfigFileFormatVersion = 2;

// The first line, and the whole of the detection test. Long and legible on
// purpose: the first thing anyone does with an unknown file is look at the
// front of it, and this answers them.
extern const char *const kConfigPreambleMagic;

// What the preamble says about itself. Advisory — see above.
struct ConfigFileInfo
{
    int formatVersion = 0;
    int schemaVersion = 0;
    QString writtenBy;
};

// True when `path` opens and begins with kConfigPreambleMagic. Cheap, and it is
// how Open tells a format-2 .ct3 from a format-1 one and from a .ct3s.
bool isBinaryConfigFile(const QString &path);

// The preamble only, without touching the container. Enough to answer "what is
// this and can this build open it" before the open document is disturbed.
bool peekBinaryConfigFile(const QString &path, ConfigFileInfo *out, QString *error = nullptr);

// Seal `plainBody` — the compact JSON body, carrying its own fileType and
// fileVersion — behind a preamble stamped with `schemaVersion` and `writtenBy`.
bool writeBinaryConfigFile(const QString &path, const QByteArray &plainBody, int schemaVersion,
                           const QString &writtenBy, QString *error = nullptr);

// Recover the body. Fails whole on a damaged or tampered file: the payload's
// tag is checked before a byte is handed back, so there is no half-read.
bool readBinaryConfigFile(const QString &path, QByteArray *plainBody, ConfigFileInfo *info,
                          QString *error = nullptr);

} // namespace ct
