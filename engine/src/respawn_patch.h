#ifndef BB_RESPAWN_PATCH_H
#define BB_RESPAWN_PATCH_H

#include <stddef.h>
#include <string.h>

enum { BB_RESPAWN_UNSUPPORTED, BB_RESPAWN_APPLIED, BB_RESPAWN_ALREADY_APPLIED };

static inline int bb_patch_respawn_delay(unsigned char *image, size_t size) {
    static const unsigned char init[] = {
        0x41,0x80,0xbc,0x24,0xe4,0x00,0x00,0x00,0x00,
        0x74,0x0a,
        0xc5,0xfa,0x10,0x05,0x5c,0xf5,0xfe,0x02,
        0xeb,0x04,0xc5,0xf8,0x57,0xc0,
        0xc4,0xc1,0x7a,0x11,0x84,0x24,0x78,0x02,0x00,0x00,
        0x41,0xc7,0x84,0x24,0x7c,0x02,0x00,0x00,0x00,0x00,0x70,0x41
    };
    static const unsigned char ready[] = {
        0xc5,0xf8,0x2e,0xca,0x0f,0x87,0x53,0x06,0x00,0x00,
        0x45,0x84,0xff,0x0f,0x84,0x4a,0x06,0x00,0x00
    };
    static const unsigned char twelve[] = {0x00,0x00,0x40,0x41};
    static const unsigned char zero[] = {0xc5,0xf8,0x57,0xc0,0x0f,0x1f,0x40,0x00};
    const size_t init_at = 0x1938b59, instruction_at = 0x1938b64;
    const size_t twelve_at = 0x49280c8, ready_at = 0x193a581;
    unsigned char expected[sizeof(init)];
    if (!image || size < twelve_at + sizeof(twelve)) return BB_RESPAWN_UNSUPPORTED;
    if (memcmp(image + twelve_at, twelve, sizeof(twelve)) ||
        memcmp(image + ready_at, ready, sizeof(ready))) return BB_RESPAWN_UNSUPPORTED;
    if (!memcmp(image + init_at, init, sizeof(init))) {
        memcpy(image + instruction_at, zero, sizeof(zero));
        return BB_RESPAWN_APPLIED;
    }
    memcpy(expected, init, sizeof(init));
    memcpy(expected + (instruction_at - init_at), zero, sizeof(zero));
    return !memcmp(image + init_at, expected, sizeof(expected)) ?
        BB_RESPAWN_ALREADY_APPLIED : BB_RESPAWN_UNSUPPORTED;
}

#endif
