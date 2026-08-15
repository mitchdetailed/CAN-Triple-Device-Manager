/*
 * fw_image.h — the firmware-update contract.
 *
 * THIS FILE IS COMPILED INTO ALL THREE COMPONENTS: the bootloader, the
 * application firmware, and the Qt configurator. It is the single place the
 * flash map, the image header and the boot control block are defined, because
 * every one of those is a thing two independently-built binaries must agree on
 * byte-for-byte. A mirrored copy that drifted would not fail a build; it would
 * brick a device in the field, which is the one failure this whole subsystem
 * exists to prevent.
 *
 * It deliberately includes NOTHING but <stdint.h>. The bootloader must stay
 * small and must not drag in the config store, the HAL or the engine, so the
 * map below is written as raw addresses. The cross-checks that tie those
 * addresses back to the application's own constants (FLASH_BANK2_ADDR,
 * FLASH_STORE_CAPACITY, the preserve ring) live in the application's
 * user_code.c, which is the one translation unit that sees both worlds.
 *
 * ---------------------------------------------------------------------------
 * THE FLASH MAP  (STM32G473CB, DBANK=1, 2 KB pages, 512 KB die)
 * ---------------------------------------------------------------------------
 *
 *   BANK 1  0x08000000-0x0803FFFF          executes here
 *     0x08000000   16 KB   bootloader          pages   0-7
 *     0x08004000  120 KB   APPLICATION SLOT    pages   8-67
 *     0x08022000  120 KB   reserved (unused)   pages  68-127
 *
 *   BANK 2  0x08040000-0x0807FFFF          written while bank 1 executes
 *     0x08040000  128 KB   config store        pages   0-63
 *     0x08060000    4 KB   preserve ring       pages  64-65
 *     0x08061000  120 KB   STAGING SLOT        pages  66-125
 *     0x0807F000    4 KB   boot control block  pages 126-127
 *
 * THIS IS MAP v2 (bootloader version 2). Map v1 had a 96 KB config store, the
 * preserve ring at 0x08058000 and 152 KB app/staging slots. The 32 KB moved
 * from the app slot into the config store to make room for the script table
 * (docs/SCRIPTING-PLAN.md) while the fleet consisted of one bench board —
 * the same change after units ship is a recall. The application still used
 * only 54 KB of the old 152, so 120 keeps 2.2x headroom.
 *
 * The two maps are mutually incompatible in one specific way: a map-v1
 * APPLICATION stages updates at 0x08059000, which map v2 assigns to the
 * config store. That is why FW_BOOTLOADER_VERSION is 2 and every image built
 * at this map sets min_bootloader_version = 2 — a v1 bootloader refuses the
 * image cleanly (BL_TOO_OLD) instead of installing something whose staging
 * writes would land inside the configuration. Crossing the map boundary is an
 * SWD operation (`build.ps1 provision`), by design. The reverse hazard — OTA
 * DOWNGRADING a v2-map unit to a v1-map image — cannot be caught by the
 * version check (old images carry min_bootloader_version = 1, which a v2
 * bootloader satisfies); it is documented in FIRMWARE-UPDATE.md instead, and
 * pre-v2 release artefacts are quarantined rather than left lying around.
 *
 * Why staging lives in bank 2 and not in spare bank 1: READ-WHILE-WRITE. On
 * this part a program or erase stalls the CPU only for accesses to the SAME
 * bank. With the image landing in bank 2 while code runs from bank 1, a
 * firmware download does not stall the core at all — CAN keeps being serviced
 * and the 7.37 Mbaud receive ring keeps draining. Staging in bank 1 would
 * reintroduce exactly the stall that used to corrupt frames mid-transfer.
 *
 * Why the app slot and the staging slot are the SAME size: an application that
 * fits in flash but not in staging is one that can never be shipped as an
 * update. Making the two equal turns that mistake into a link error (the app
 * linker script's LENGTH is FW_APP_MAX_SIZE) instead of a surprise at release
 * time. The 120 KB above the app slot is deliberately left fallow; spending it
 * means growing staging too, and there is nothing left in bank 2 to grow into.
 *
 * ---------------------------------------------------------------------------
 * THE UPDATE SEQUENCE, and why it cannot brick a unit
 * ---------------------------------------------------------------------------
 *
 *   1. The RUNNING APPLICATION receives the image over the normal serial
 *      protocol and programs it into the staging slot. The app slot is not
 *      touched, so a download that is corrupt, truncated, or abandoned costs
 *      nothing — the device is still running the firmware it booted.
 *   2. The application verifies the staged CRC32 and only then arms the boot
 *      control block, and reboots.
 *   3. The BOOTLOADER re-verifies the staged image from scratch. It does not
 *      trust the application's word for it: the point of checking again is
 *      that the app may have died between step 1 and step 2.
 *   4. Only after that does it erase the app slot and copy. Staging is never
 *      erased during the copy, so if power fails halfway the source is still
 *      intact and the whole operation simply repeats on the next boot.
 *
 * The commit is therefore IDEMPOTENT, which is what makes it power-safe. There
 * is no "half-updated" state to detect and no rollback path to get wrong: the
 * bootloader either has a valid app or has a valid staged image it can lay
 * down again, and it keeps trying until one of those is true.
 */
#ifndef FW_IMAGE_H
#define FW_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
#  define FW_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
extern "C" {
#else
#  define FW_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* --------------------------------------------------------------------------
 * Flash map
 * -------------------------------------------------------------------------- */

#define FW_FLASH_BASE        0x08000000u
#define FW_FLASH_END         0x08080000u   /* 512 KB die */
#define FW_BANK2_BASE        0x08040000u
#define FW_PAGE_SIZE              0x800u   /* 2 KB, dual-bank */

#define FW_BOOTLOADER_BASE   0x08000000u
#define FW_BOOTLOADER_SIZE       0x4000u   /*  16 KB */

#define FW_APP_BASE          0x08004000u
#define FW_APP_MAX_SIZE         0x1E000u   /* 120 KB — equals staging, see above */

#define FW_STAGING_BASE      0x08061000u
#define FW_STAGING_SIZE         0x1E000u

#define FW_BCB_BASE          0x0807F000u
#define FW_BCB_SIZE              0x1000u   /* 2 pages, ping-ponged */

/* The map has to close exactly. Each of these has caught a real class of
 * mistake in layouts like this one: a gap between the bootloader and the app
 * leaves the vector table at an address the jump does not use; an app slot
 * that crosses 0x08040000 forfeits read-while-write and gets erased by its own
 * config save; a staging slot smaller than the app slot silently truncates the
 * image it is supposed to hold. */
FW_STATIC_ASSERT(FW_BOOTLOADER_BASE + FW_BOOTLOADER_SIZE == FW_APP_BASE,
                 "bootloader must end exactly where the app slot begins");
FW_STATIC_ASSERT(FW_APP_BASE + FW_APP_MAX_SIZE <= FW_BANK2_BASE,
                 "app slot must stay inside bank 1 or read-while-write is lost");
FW_STATIC_ASSERT(FW_STAGING_SIZE == FW_APP_MAX_SIZE,
                 "staging must be able to hold any image the app slot can");
FW_STATIC_ASSERT(FW_STAGING_BASE >= FW_BANK2_BASE,
                 "staging must be in bank 2 so downloads do not stall the core");
FW_STATIC_ASSERT(FW_STAGING_BASE + FW_STAGING_SIZE == FW_BCB_BASE,
                 "boot control block must sit immediately above staging");
FW_STATIC_ASSERT(FW_BCB_BASE + FW_BCB_SIZE == FW_FLASH_END,
                 "boot control block must end at the top of flash");
FW_STATIC_ASSERT(FW_BOOTLOADER_SIZE % FW_PAGE_SIZE == 0 &&
                 FW_APP_MAX_SIZE % FW_PAGE_SIZE == 0 &&
                 FW_STAGING_SIZE % FW_PAGE_SIZE == 0 &&
                 FW_BCB_SIZE % FW_PAGE_SIZE == 0,
                 "every region must be a whole number of erase pages");
FW_STATIC_ASSERT(FW_BCB_SIZE / FW_PAGE_SIZE == 2,
                 "the boot control block ping-pongs between exactly two pages");

/* --------------------------------------------------------------------------
 * Reading flash by absolute address
 * -------------------------------------------------------------------------- */

/* On the device, a flash address IS a pointer — bank 2 is memory-mapped and
 * this macro compiles to nothing.
 *
 * Host builds are the reason it exists. The configurator's firmware-link test
 * compiles the real serial_proto.c, fw_update.c and bcb.c so both sides of the
 * wire are checked against each other, and those modules read the staging slot
 * and the boot control block by absolute address. Nothing is mapped at
 * 0x08059000 in a Windows process, so without a translation point the test
 * would fault the moment it touched the update path — and the update path is
 * exactly the one worth testing off-hardware, because its failure mode is a
 * device that will not boot.
 *
 * WRITES need no equivalent: they already go through an injected driver, so a
 * host test hands over a RAM-backed one and nothing else changes. */
#if defined(__arm__)
#  define FW_FLASH_PTR(addr) ((const void *)(uintptr_t)(addr))
#else
/* Supplied by the host test harness: maps a flash address onto its RAM model
 * of bank 2. */
const void *fw_flash_ptr(uint32_t addr);
#  define FW_FLASH_PTR(addr) fw_flash_ptr(addr)
#endif

/* --------------------------------------------------------------------------
 * Image header
 * -------------------------------------------------------------------------- */

/* The header is embedded IN the image at a fixed offset rather than bolted on
 * the front, because offset 0 belongs to the vector table — the reset vector
 * has to be the first thing at FW_APP_BASE or the part will not start. 0x200
 * clears the largest vector table this family has (16 system exceptions + 102
 * device interrupts = 472 bytes), with room to spare if ST adds more. */
#define FW_IMAGE_HEADER_OFFSET   0x200u

#define FW_IMAGE_MAGIC       0x57465443u   /* 'CTFW' little-endian */
#define FW_IMAGE_HDR_VERSION        1u

/* Which product the image is for. The bootloader refuses anything else, so a
 * file for a different board cannot be flashed into this one by mistake. */
#define FW_PRODUCT_CAN_TRIPLE  0x4333u     /* 'C3' */

/* STM32G4 programs flash 64 bits at a time, so an image is only writable if it
 * is a whole number of doublewords. The packaging tool pads with 0xFF (the
 * erased value) to reach this. */
#define FW_IMAGE_ALIGN              8u

typedef struct {
    uint32_t magic;                /* FW_IMAGE_MAGIC */
    uint16_t header_version;       /* FW_IMAGE_HDR_VERSION */
    uint16_t product_id;           /* FW_PRODUCT_CAN_TRIPLE */

    /* Total bytes of the image, header included — the exact number the
     * bootloader copies and the exact number the CRC covers. */
    uint32_t image_size;

    /* CRC32 (IEEE, reflected, init/final 0xFFFFFFFF) over the whole image
     * EXCEPT these four bytes: [0, crc_off) then [crc_off + 4, image_size).
     * Skipping rather than zeroing keeps the packaging tool and the two
     * firmware readers from having to agree on a "zero it first" step that is
     * easy to implement in three subtly different ways. */
    uint32_t image_crc32;

    uint16_t fw_version_major;
    uint16_t fw_version_minor;
    uint16_t fw_version_patch;

    /* The FLASH_STORE_VERSION this image was built against. The configurator
     * compares it with the running one and can tell the user, BEFORE the
     * update, that the stored configuration will not survive it — which is the
     * difference between a warned re-Send and a device that silently comes
     * back empty. */
    uint16_t flash_store_version;

    /* Refuse to install on a bootloader too old to understand the image. Lets
     * a future format change fail cleanly instead of half-working. */
    uint32_t min_bootloader_version;

    uint32_t build_id;             /* short git hash, 0 if unknown */
    char     build_desc[32];       /* human label, NUL-padded, may be empty */
} FwImageHeader;

FW_STATIC_ASSERT(sizeof(FwImageHeader) == 64, "FwImageHeader must be 64 bytes");
FW_STATIC_ASSERT(FW_IMAGE_HEADER_OFFSET + sizeof(FwImageHeader) < 0x400u,
                 "header must not run past the space reserved for it");

/* Byte offset of image_crc32 within the whole image — the span both CRC
 * implementations skip. Derived, never written out by hand. */
#define FW_IMAGE_CRC_OFFSET \
    (FW_IMAGE_HEADER_OFFSET + (uint32_t)__builtin_offsetof(FwImageHeader, image_crc32))

/* The smallest thing that could possibly be a valid image. */
#define FW_IMAGE_MIN_SIZE (FW_IMAGE_HEADER_OFFSET + (uint32_t)sizeof(FwImageHeader))

/* --------------------------------------------------------------------------
 * Boot control block
 * -------------------------------------------------------------------------- */

#define FW_BCB_MAGIC         0x42435443u   /* 'CTCB' little-endian */
#define FW_BCB_RECORD_SIZE          32u
#define FW_BCB_SLOTS_PER_PAGE  (FW_PAGE_SIZE / FW_BCB_RECORD_SIZE)   /* 64 */

/* How many times the bootloader will attempt a commit before giving up and
 * booting whatever is already in the app slot. Without this, an image that
 * passes CRC but crashes the copy would reboot-loop forever. */
#define FW_MAX_COMMIT_ATTEMPTS      3u

enum {
    FW_STATE_IDLE    = 0,   /* nothing staged that wants installing */
    FW_STATE_PENDING = 1,   /* staging holds an image; install it on next boot */
};

/* Outcome of the last commit the bootloader attempted, reported back to the
 * configurator so a failed update says WHY rather than just not happening. */
enum {
    FW_RESULT_NONE            = 0,
    FW_RESULT_OK              = 1,
    FW_RESULT_BAD_MAGIC       = 2,
    FW_RESULT_WRONG_PRODUCT   = 3,
    FW_RESULT_BAD_SIZE        = 4,
    FW_RESULT_BAD_CRC         = 5,
    FW_RESULT_BL_TOO_OLD      = 6,
    FW_RESULT_ERASE_FAILED    = 7,
    FW_RESULT_PROGRAM_FAILED  = 8,
    FW_RESULT_VERIFY_FAILED   = 9,
    FW_RESULT_GAVE_UP         = 10,  /* hit FW_MAX_COMMIT_ATTEMPTS */
};

/* Appended, never overwritten in place: flash cannot rewrite a word without an
 * erase, and an erase is the one moment the state would be absent. Records are
 * written into whichever of the two pages is live; when it fills, the other is
 * erased, the next record written there, and only then is the old page erased.
 * State is therefore readable at every instant, including mid-swap. The record
 * with the highest seq whose own crc32 checks out is the current one. */
typedef struct {
    uint32_t magic;          /* FW_BCB_MAGIC */
    uint32_t seq;            /* higher wins; wraps are not a concern at 1/update */
    uint8_t  state;          /* FW_STATE_* */
    uint8_t  attempts;       /* commits tried for the CURRENT staged image */
    uint8_t  last_result;    /* FW_RESULT_* */
    uint8_t  reserved0;
    uint32_t staged_size;    /* mirrors the staged header, so the bootloader can */
    uint32_t staged_crc32;   /*   sanity-check before trusting the staged bytes */
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t crc32;          /* over the preceding 28 bytes */
} BootControlRecord;

FW_STATIC_ASSERT(sizeof(BootControlRecord) == FW_BCB_RECORD_SIZE,
                 "BootControlRecord must be exactly one slot");
FW_STATIC_ASSERT(FW_BCB_RECORD_SIZE % FW_IMAGE_ALIGN == 0,
                 "a record must be a whole number of programmable doublewords");

/* --------------------------------------------------------------------------
 * CRC32 (shared by bootloader, application and packaging tool)
 * -------------------------------------------------------------------------- */

/* Bitwise rather than table-driven: 152 KB costs about 70 ms at 170 MHz, which
 * is invisible next to the ~1.1 s the erase alone takes, and it keeps a table
 * out of a bootloader whose whole budget is 16 KB. */
uint32_t fw_crc32(const void *data, uint32_t len);

/* Same value, computed across several calls. Start with FW_CRC32_INIT, feed
 * each span in order, finish with fw_crc32_final(). The bootloader needs this
 * because the image CRC skips four bytes in the middle. */
#define FW_CRC32_INIT 0xFFFFFFFFu
uint32_t fw_crc32_update(uint32_t crc, const void *data, uint32_t len);
static inline uint32_t fw_crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

/* --------------------------------------------------------------------------
 * Shared validation
 * -------------------------------------------------------------------------- */

/* Validate an image sitting in flash (or RAM) at `base`, `avail` bytes of room.
 * Returns FW_RESULT_OK or the specific reason it was rejected. Used by the
 * bootloader on the staged image before committing, by the bootloader on the
 * app slot before jumping, and by the application on the staged image before
 * arming the boot control block — one implementation, so all three agree on
 * what "valid" means. */
uint8_t fw_image_validate(const void *base, uint32_t avail, uint32_t bl_version);

/* The header inside an image, or NULL if `avail` cannot even hold one. Does
 * not validate; use fw_image_validate for that. */
static inline const FwImageHeader *fw_image_header(const void *base, uint32_t avail)
{
    if (avail < FW_IMAGE_MIN_SIZE) {
        return (const FwImageHeader *)0;
    }
    return (const FwImageHeader *)((const uint8_t *)base + FW_IMAGE_HEADER_OFFSET);
}

/* This bootloader's own version, reported to the configurator and compared
 * against an image's min_bootloader_version.
 *
 * 2 marks the flash-map change (128 KB config store, staging at 0x08061000).
 * A version-1 bootloader installs from the OLD staging address, so an image
 * built at this map must never be accepted by one — min_bootloader_version in
 * fw_header.c is the matching half of this constant, and the pair is exactly
 * the mechanism min_bootloader_version was designed for. Bump BOTH together,
 * and only when an image genuinely cannot run under the older bootloader. */
#define FW_BOOTLOADER_VERSION 2u

/* --------------------------------------------------------------------------
 * Bootloader version stamp
 * -------------------------------------------------------------------------- */

/* The bootloader publishes its version at a FIXED address so the application
 * can read it back and report the truth.
 *
 * The tempting shortcut is for the application to report FW_BOOTLOADER_VERSION
 * out of its own build — but the two images are flashed independently and are
 * updated on completely different schedules (the app over the wire, the
 * bootloader only ever over SWD). An app built today running on a bootloader
 * flashed a year ago would confidently report the wrong number, and the one
 * decision that number feeds is whether an image is safe to install. So the
 * app reads it out of the bootloader instead, and reports 0 when the magic is
 * absent — which is also the honest answer on a unit that has no bootloader at
 * all, i.e. one flashed with a pre-fork image. */
#define FW_BL_INFO_OFFSET 0x200u
#define FW_BL_INFO_MAGIC  0x4C425443u   /* 'CTBL' little-endian */

typedef struct {
    uint32_t magic;      /* FW_BL_INFO_MAGIC */
    uint32_t version;    /* FW_BOOTLOADER_VERSION as built */
    uint32_t app_base;   /* where this bootloader jumps; a cross-check for the app */
    uint32_t reserved;
} BootloaderInfo;

FW_STATIC_ASSERT(sizeof(BootloaderInfo) == 16, "BootloaderInfo must be 16 bytes");

/* Read the stamp out of the running device's bootloader region. Returns the
 * version, or 0 if no recognisable bootloader is present.
 *
 * A zero here is load-bearing, not just informational: staging an update on a
 * unit with no bootloader would write an image nothing will ever install,
 * while telling the user the update succeeded. fw_update_begin() refuses on 0
 * for exactly that reason. */
#if defined(__arm__)
static inline uint32_t fw_bootloader_version(void)
{
    const BootloaderInfo *info =
        (const BootloaderInfo *)(FW_BOOTLOADER_BASE + FW_BL_INFO_OFFSET);
    return (info->magic == FW_BL_INFO_MAGIC) ? info->version : 0u;
}
#else
/* Host builds (the configurator's firmware-link test) compile this same code
 * but have nothing mapped at 0x08000200, so dereferencing it would fault. The
 * tests exercise the update state machine, not the stamp, so report a current
 * bootloader and let them get on with it. */
static inline uint32_t fw_bootloader_version(void)
{
    return FW_BOOTLOADER_VERSION;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* FW_IMAGE_H */
