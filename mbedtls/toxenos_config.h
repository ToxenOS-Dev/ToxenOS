#ifndef TOXENOS_MBEDTLS_CONFIG_H
#define TOXENOS_MBEDTLS_CONFIG_H

#include "../mbedtls/toxenos_platform_fwd.h"

/* System */
#define MBEDTLS_HAVE_ASM
#undef  MBEDTLS_HAVE_TIME
#undef  MBEDTLS_HAVE_TIME_DATE
#undef  MBEDTLS_NET_C

/* Platform — use our calloc/free, but let platform.c handle printf */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO  toxenos_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO    toxenos_free
// Use our own snprintf/printf via macros
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO  toxenos_snprintf
#define MBEDTLS_PLATFORM_PRINTF_MACRO    toxenos_printf

/* Entropy */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_HMAC_DRBG_C

/* Crypto */
#define MBEDTLS_PKCS5_C         // PBKDF2 password hashing
#define MBEDTLS_AES_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR
#define MBEDTLS_CIPHER_PADDING_PKCS7

/* PSA - minimal, no USE_PSA_CRYPTO to avoid PSA path in TLS */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_CLIENT

/* Key exchange */
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* X.509 */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
// Using mbedTLS default cipher suite list (all enabled ciphers)
#define MBEDTLS_SSL_IN_CONTENT_LEN   16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096

/* Disable unused */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_VERSION_FEATURES
#undef MBEDTLS_PKCS7_C

#endif
