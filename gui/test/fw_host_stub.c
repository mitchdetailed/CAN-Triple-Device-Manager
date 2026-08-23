#include "fw_host_stub.h"

#include <string.h>

#include "flash_store.h"   /* FLASH_STORE_VERSION */
#include "fw_image.h"

/* The modelled region: staging slot + boot control block, i.e. everything the
 * update path reads or writes. The config store and preserve ring below are
 * deliberately NOT modelled — nothing in this subsystem may touch them, and
 * leaving them unmapped means a test that reaches into them crashes loudly
 * rather than passing quietly. */
#define HOST_BASE FW_STAGING_BASE
#define HOST_SIZE (FW_FLASH_END - FW_STAGING_BASE)

static uint8_t g_flash[HOST_SIZE];
static int g_fail_after = -1;
static int g_written;

void fw_host_reset(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    g_fail_after = -1;
    g_written = 0;
}

uint8_t *fw_host_at(uint32_t addr, uint32_t len)
{
    if (addr < HOST_BASE || (uint64_t)addr + len > (uint64_t)HOST_BASE + HOST_SIZE) {
        return NULL;
    }
    return &g_flash[addr - HOST_BASE];
}

const void *fw_flash_ptr(uint32_t addr)
{
    uint8_t *p = fw_host_at(addr, 1);
    /* A read outside the modelled region is a bug in the code under test, not
     * something to paper over with a plausible-looking pointer. */
    return p ? p : NULL;
}

void fw_host_fail_programs_after(int n)
{
    g_fail_after = n;
}

int fw_host_doublewords_written(void)
{
    return g_written;
}

bool fw_host_erase(uint32_t addr, uint32_t len)
{
    if ((addr % FW_PAGE_SIZE) != 0u || (len % FW_PAGE_SIZE) != 0u || len == 0u) {
        return false;
    }
    uint8_t *p = fw_host_at(addr, len);
    if (!p) {
        return false;
    }
    memset(p, 0xFF, len);
    return true;
}

bool fw_host_program(uint32_t addr, const void *src, uint32_t len)
{
    if ((addr % FW_IMAGE_ALIGN) != 0u || (len % FW_IMAGE_ALIGN) != 0u) {
        return false;
    }
    uint8_t *dst = fw_host_at(addr, len);
    if (!dst) {
        return false;
    }

    const uint8_t *from = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i += FW_IMAGE_ALIGN) {
        uint64_t want;
        uint64_t have;
        memcpy(&want, from + i, sizeof(want));
        memcpy(&have, dst + i, sizeof(have));

        if (have == want) {
            continue;   /* the driver's own skip; costs no write */
        }
        /* Real NOR: a doubleword that has already been programmed to something
         * else cannot be programmed again without an erase. Modelling this is
         * the point of the stub — it is what turns "the driver re-sent a chunk"
         * from a silent pass into a failure. */
        if (have != 0xFFFFFFFFFFFFFFFFull) {
            return false;
        }
        if (g_fail_after >= 0 && g_written >= g_fail_after) {
            return false;
        }
        memcpy(dst + i, &want, sizeof(want));
        ++g_written;
    }
    return true;
}

/* The running image's header. On the device this comes from fw_header.c and is
 * placed by the linker at FW_APP_BASE + 0x200; here it just has to exist and
 * carry believable values, because fw_update_status() reports it. */
const FwImageHeader g_fw_header = {
    .magic          = FW_IMAGE_MAGIC,
    .header_version = FW_IMAGE_HDR_VERSION,
    .product_id     = FW_PRODUCT_CAN_TRIPLE,
    .image_size     = 0u,
    .image_crc32    = 0u,
    .fw_version_major = 2,
    .fw_version_minor = 0,
    .fw_version_patch = 0,
    .flash_store_version = FLASH_STORE_VERSION,
    .min_bootloader_version = 1u,
    .build_id   = 0u,
    .build_desc = "host-test",
};
