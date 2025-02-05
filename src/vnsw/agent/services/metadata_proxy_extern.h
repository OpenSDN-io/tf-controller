#ifndef METADATA_PROXY_EXTERN
#define METADATA_PROXY_EXTERN

#include <isc/result.h>

typedef void isc_md_type_t;
#define ISC_MD_MD5    isc__crypto_md5
#define ISC_MD_SHA1   isc__crypto_sha1
#define ISC_MD_SHA224 isc__crypto_sha224
#define ISC_MD_SHA256 isc__crypto_sha256
#define ISC_MD_SHA384 isc__crypto_sha384
#define ISC_MD_SHA512 isc__crypto_sha512

#define ISC_MD5_DIGESTLENGTH    isc_md_type_get_size(ISC_MD_MD5)
#define ISC_MD5_BLOCK_LENGTH    isc_md_type_get_block_size(ISC_MD_MD5)
#define ISC_SHA1_DIGESTLENGTH   isc_md_type_get_size(ISC_MD_SHA1)
#define ISC_SHA1_BLOCK_LENGTH   isc_md_type_get_block_size(ISC_MD_SHA1)
#define ISC_SHA224_DIGESTLENGTH isc_md_type_get_size(ISC_MD_SHA224)
#define ISC_SHA224_BLOCK_LENGTH isc_md_type_get_block_size(ISC_MD_SHA224)
#define ISC_SHA256_DIGESTLENGTH isc_md_type_get_size(ISC_MD_SHA256)
#define ISC_SHA256_BLOCK_LENGTH isc_md_type_get_block_size(ISC_MD_SHA256)
#define ISC_SHA384_DIGESTLENGTH isc_md_type_get_size(ISC_MD_SHA384)
#define ISC_SHA384_BLOCK_LENGTH isc_md_type_get_block_size(ISC_MD_SHA384)
#define ISC_SHA512_DIGESTLENGTH isc_md_type_get_size(ISC_MD_SHA512)
#define ISC_SHA512_BLOCK_LENGTH isc_md_type_get_block_size(ISC_MD_SHA512)

#define ISC_MAX_MD_SIZE    64U  /* EVP_MAX_MD_SIZE */
#define ISC_MAX_BLOCK_SIZE 128U /* ISC_SHA512_BLOCK_LENGTH */

#include <openssl/evp.h>

extern const EVP_MD *isc__crypto_md5;
extern const EVP_MD *isc__crypto_sha1;
extern const EVP_MD *isc__crypto_sha224;
extern const EVP_MD *isc__crypto_sha256;
extern const EVP_MD *isc__crypto_sha384;
extern const EVP_MD *isc__crypto_sha512;


extern isc_result_t
isc_hmac(const isc_md_type_t *type, const void *key, const size_t keylen,
            const unsigned char *buf, const size_t len, unsigned char *digest,
            unsigned int *digestlen);

#endif