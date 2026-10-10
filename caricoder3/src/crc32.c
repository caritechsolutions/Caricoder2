/*
 * caricoder3 - MPEG-2 CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF, no reflection)
 * Copyright (c) 2026 CariTech Solutions
 *
 * A section whose trailing CRC_32 is correct gives a CRC of zero when the
 * whole section, CRC included, is run through this.
 */
#include "cc3.h"

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
        crc_table[i] = c;
    }
    crc_ready = true;
}

/* Called once from main before any thread starts. */
__attribute__((constructor)) static void crc_ctor(void)
{
    crc_init();
}

uint32_t crc32_mpeg(const uint8_t *data, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;

    if (!crc_ready)
        crc_init();
    for (size_t i = 0; i < len; i++)
        c = (c << 8) ^ crc_table[((c >> 24) ^ data[i]) & 0xFF];
    return c;
}
