/*
 * fleet_identity.h — who this unit is, fixed at build time.
 *
 * THIS FILE DEFINES WHAT A BADGED UNIT IS; THE VALUES FOR A GIVEN UNIT GO IN
 * identity.local.ini (see below). Nothing writes these values at runtime and no
 * host command can change them: the identity is part of the binary, so it cannot
 * be lost to a flash erase, cannot be re-badged over the serial port, and cannot
 * drift out of step with a configuration that was sent later.
 *
 * ---------------------------------------------------------------------------
 * Setting it
 *
 * Everything below has a default, so a build that sets nothing still compiles —
 * it just reports itself unprovisioned, which is honest, rather than silently
 * claiming to be somebody. The values do NOT live in this file and no longer
 * live in platformio.ini either: they go in identity.local.ini in the project
 * root — a gitignored KEY=VALUE file, copied from identity.local.ini.example —
 * which scripts/build_flags.py reads before every build and turns into the
 * matching -DCT_* defines:
 *
 *     CT_VENDOR_ID=Acme
 *     CT_MODEL_ID=CAN Triple TD
 *     CT_SERIAL_NUMBER=0x00000123
 *     CT_FLEET_KEY=0xDEADBEEF
 *
 * Those four are a PLACEHOLDER, not anybody's fleet: 0xDEADBEEF is not a real
 * derived key and never was. A real one comes from the CAN Triple Device
 * Manager (Online > Fleet Identity…), and a real one belongs only in your own
 * gitignored file — never in a published example.
 *
 * The vendor and model are plain unquoted text, spaces and all. The old sharp
 * edge — the '"..."' a -D string needed in order to survive both the shell and
 * SCons — is the script's problem now, and there is nothing left to get wrong
 * by hand. Each string is still capped at 16 bytes, and a longer one is still a
 * build error naming the key rather than a silent truncation to something
 * nothing will ever match. No file at all means an unprovisioned build, which
 * is the honest default for a fresh clone; a file that IS present but malformed
 * is a hard build error naming the offending line, never a half-badged unit.
 *
 * ---------------------------------------------------------------------------
 * The one field that is per-UNIT rather than per-BUILD
 *
 * CT_SERIAL_NUMBER. Everything else is the same across a product line; a serial
 * number is not, so a build carries exactly one unit's serial and each board
 * needs its own edit-and-upload. That is the deliberate cost of "set it once in
 * identity.local.ini and forget it": there is no provisioning step, no tool, and
 * nothing that can be reflashed away — in exchange for not being able to flash
 * fifty boards from one binary.
 *
 * If that trade stops paying, the STM32's 96-bit unique ID is already readable
 * (CMD_GET_DEVICE_ID) and needs no provisioning at all. It is a different
 * thing, though: silicon chooses it, not you. The two coexist — the UID is the
 * chip's identity and the serial is yours.
 *
 * ---------------------------------------------------------------------------
 * What the fleet key is, and what it is not
 *
 * CT_FLEET_KEY is a shared secret held by every unit of the same vendor and
 * model. The host challenges a device to prove it holds the key, which is what
 * turns "this device CLAIMS to be an Acme CAN Triple" into a claim worth acting
 * on, and stops a look-alike collecting an update built for somebody else.
 *
 * Being exact, because a shared secret invites more trust than it earns:
 *   * It is in the binary. Anyone who can read the flash — or the .bin file you
 *     ship — has it. The backstop is STM32G4 readout protection, a programming
 *     decision rather than a code one.
 *   * It is shared, so ONE compromised unit compromises the attestation for
 *     every unit built with the same key. There is no revocation short of a new
 *     key and a reflash of the fleet.
 *   * It authenticates the FLEET, never the individual unit. Serial-number
 *     matching narrows which unit an update installs on; it is not a second
 *     secret and a device's serial can be read by anyone who asks.
 *
 * ---------------------------------------------------------------------------
 * There is deliberately no "series" field
 *
 * An earlier revision carried a CT_SERIES_ID alongside the model, meaning
 * "which configuration line within this product". It was removed as redundant:
 * CT_MODEL_ID is a sixteen-character string set in the same file at the same
 * moment, so anything a series number could distinguish — "CAN Triple TD" from
 * "CAN Triple END" — a model name distinguishes better, and reads properly in
 * a dialog instead of being a hex value somebody has to look up.
 *
 * The consequence, stated so it is a choice rather than a surprise: two
 * configuration lines that share an identical CT_MODEL_ID can no longer be told
 * apart. Give them distinct model names.
 */
#ifndef FLEET_IDENTITY_H
#define FLEET_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-width and NUL-PADDED, not NUL-terminated: all 16 bytes are usable
 * characters. A reader must treat them as a counted field and stop at the first
 * NUL or the sixteenth byte, whichever comes first. Chosen over "15 chars plus a
 * terminator" because these are compared, not printed with %s, and an exact
 * 16-byte compare is the operation the uploader actually performs. */
#define FLEET_VENDOR_ID_LEN 16u
#define FLEET_MODEL_ID_LEN  16u
#define FLEET_KEY_LEN       4u

/* Retired flag names, caught rather than ignored.
 *
 * An unrecognised -D is not an error: the compiler takes the definition, nothing
 * reads it, and the build succeeds. identity.local.ini is no longer a way in —
 * scripts/build_flags.py refuses a key it does not recognise, by name, before
 * anything compiles — but a stale -DCT_SERIES_KEY arriving from anywhere else
 * (a hand-added build_flags line, a CI wrapper, an IDE's own defines) still
 * produces a unit whose CT_FLEET_KEY fell back to the default of ZERO — a board
 * that looks provisioned, builds without a murmur, and can prove nothing to
 * anybody. That happened once during the rename, on a board that had already
 * been flashed.
 *
 * These two lines turn that into a build failure that names the new flag. */
#ifdef CT_SERIES_KEY
#error "CT_SERIES_KEY was renamed: use -DCT_FLEET_KEY instead (same 4-byte value)."
#endif
#ifdef CT_SERIES_ID
#error "CT_SERIES_ID was removed — a config line is distinguished by CT_MODEL_ID now."
#endif

/* Defaults. Empty strings and zeroes read as "unprovisioned", which the host
 * treats as "nothing to check against" rather than as a match — a blank unit
 * must never satisfy a policy that names a vendor. */
#ifndef CT_VENDOR_ID
#define CT_VENDOR_ID ""
#endif
#ifndef CT_MODEL_ID
#define CT_MODEL_ID ""
#endif
#ifndef CT_SERIAL_NUMBER
#define CT_SERIAL_NUMBER 0u
#endif
#ifndef CT_FLAGS
#define CT_FLAGS 0u
#endif
#ifndef CT_FLEET_KEY
#define CT_FLEET_KEY 0u
#endif

/* The build-time identity, in the byte order the wire uses. Deliberately NOT a
 * packed wire struct: it is assembled into one by the serial layer, so a
 * compiler's idea of alignment here can never silently change the protocol. */
typedef struct {
    char vendor_id[FLEET_VENDOR_ID_LEN];
    char model_id[FLEET_MODEL_ID_LEN];
    uint32_t serial_number;
    uint16_t flags;
    uint8_t fleet_key[FLEET_KEY_LEN];
} FleetIdentity;

/* The one instance, built from the macros above. Points at .rodata and lives as
 * long as the firmware does. */
const FleetIdentity *fleet_identity(void);

/* True when vendor or model says something. An unprovisioned unit answers false
 * and the host stops checking there.
 *
 * serial_number is deliberately NOT part of this. A serial identifies a unit;
 * it says nothing about which fleet the unit belongs to, so a board built with
 * only CT_SERIAL_NUMBER set is still unprovisioned as far as any match is
 * concerned. ct::FleetIdentity::isSet() on the host applies the same rule, and
 * the two must not drift — a device and a host disagreeing about what
 * "unprovisioned" means is a mismatch that shows up as an update mysteriously
 * refused. */
bool fleet_identity_is_set(void);

/* True when CT_FLEET_KEY is non-zero, i.e. the device can answer a challenge. */
bool fleet_key_is_set(void);

#ifdef __cplusplus
}
#endif

#endif /* FLEET_IDENTITY_H */
