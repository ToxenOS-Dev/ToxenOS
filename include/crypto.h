#pragma once
#include <stdint.h>

int kernel_pbkdf2_sha256(
    const uint8_t* password, uint32_t plen,
    const uint8_t* salt,     uint32_t slen,
    uint32_t       iterations,
    uint8_t*       out,      uint32_t outlen);
