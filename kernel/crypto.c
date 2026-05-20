// kernel/crypto.c — Password hashing via PBKDF2-SHA256 (uses mbedTLS)
// Compiled with MBEDFLAGS so mbedTLS headers are available.
#include <stdint.h>
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"

int kernel_pbkdf2_sha256(
    const uint8_t* password, uint32_t plen,
    const uint8_t* salt,     uint32_t slen,
    uint32_t       iterations,
    uint8_t*       out,      uint32_t outlen)
{
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return -1; }
    int ret = mbedtls_pkcs5_pbkdf2_hmac(&ctx, password, plen, salt, slen,
                                         iterations, outlen, out);
    mbedtls_md_free(&ctx);
    return ret;
}
