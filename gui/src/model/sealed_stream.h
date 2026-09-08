// The sealed install stream: what a .ct3s carries instead of a configuration.
//
// A package used to hold the configuration itself, encrypted under a key that
// travelled inside the file. Send Secure Configuration decrypted it on the
// host, mapped it to device records and streamed them in the clear — so the
// plaintext existed in the customer's process, and anyone holding the package
// and this application's (public) source could read it. This is the other
// half of the fix, the device's half being firmware/include/seal.h: the
// Builder maps the configuration to device records at build time and seals
// each command frame under keys derived from the fleet key. The host never
// holds the plaintext again; it relays sealed frames to a device that derives
// the same keys from its licence store and decrypts inside.
//
// The construction is the firmware's own seal.c, compiled into this
// application through ct_device_core, so the Builder's sealer and the
// device's opener cannot drift.
//
// STREAM LAYOUT — all multi-byte integers little-endian:
//     u8[4]  magic        "CT3I"
//     u8     version      kSealedStreamVersion (1)
//     u8[16] salt         the per-package salt the device derives its keys from
//     u32    frameCount
//     frames, each:
//       u8   flags        kFrameFlashTimeout: the inner command erases or
//                         commits flash and needs the long timeout
//       u16  length
//       u8[] sealed       u32 index || ciphertext || tag, exactly as seal.c
//                         produces it; the ciphertext decrypts to
//                         inner_cmd || inner_payload
//
// The inner commands are never named in the clear; the flag byte is the one
// piece of per-frame metadata the relay needs, and it says nothing about what
// the frame contains beyond "this one takes a while".
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace ct {

constexpr quint8 kSealedStreamVersion = 1;
constexpr quint8 kFrameFlashTimeout = 0x01;

// One command frame before sealing: what ConfigTransfer plans for a Send.
struct PlannedFrame
{
    quint8 cmd = 0;
    QByteArray payload;
    QString stage;          // the relay's progress text ("Sending messages (3/9)")
    bool flashTimeout = false;
};

// One frame after sealing, as parsed back off a stream.
struct SealedFrame
{
    QByteArray bytes; // u32 index || ciphertext || tag
    QString stage;    // reconstructed as "Installing (n/N)"; the stream carries no names
    bool flashTimeout = false;
};

// Seal `frames` under `fleetKey` (the derived Firmware Key, kLicenseKeyBytes)
// with a fresh random salt. Returns the stream, or an empty array with `error`
// set when a frame would not fit the wire.
QByteArray buildSealedStream(const QByteArray &fleetKey, const QList<PlannedFrame> &frames,
                             QString *error);

// The reverse: the salt and the frames out of a stream. Validates lengths
// only — the tags are the device's to check.
bool parseSealedStream(const QByteArray &stream, QByteArray *salt, QList<SealedFrame> *frames,
                       QString *error);

// The largest inner payload a sealed frame can carry and still fit the wire
// budget an ordinary frame has: MAX_TX_PAYLOAD less the seal overhead and the
// inner command byte. The planner chunks against this.
int sealedInnerPayloadBudget();

} // namespace ct
