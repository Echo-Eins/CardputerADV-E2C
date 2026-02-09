/*
 * Crypto Utilities Implementation
 *
 * Cryptographic primitives for Evil-Cardputer
 */

#include "crypto_utils.h"
#include <string.h>
#include <stdlib.h>
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

// ============================================================================
// MD4 Implementation
// ============================================================================

// MD4 macros
#define ROL(x,n) ((uint32_t)((uint32_t)(x) << (n)) | (uint32_t)((uint32_t)(x) >> (32-(n))))
#define MD4_F1(x,y,z) (((x)&(y)) | ((~x)&(z)))
#define MD4_G(x,y,z) (((x)&(y)) | ((x)&(z)) | ((y)&(z)))
#define MD4_H(x,y,z) ((x) ^ (y) ^ (z))

void MD4_Encode(uint8_t *output, const uint32_t *input, size_t len) {
    for (size_t i = 0, j = 0; j < len; i++, j += 4) {
        output[j]   = (uint8_t)( input[i]        & 0xff);
        output[j+1] = (uint8_t)((input[i] >> 8)  & 0xff);
        output[j+2] = (uint8_t)((input[i] >> 16) & 0xff);
        output[j+3] = (uint8_t)((input[i] >> 24) & 0xff);
    }
}

void MD4_Transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], X[16];

    for (int i = 0, j = 0; j < 64; i++, j += 4)
        X[i] = (uint32_t)block[j] | ((uint32_t)block[j+1]<<8) |
               ((uint32_t)block[j+2]<<16) | ((uint32_t)block[j+3]<<24);

    #define ROUND1(a,b,c,d,k,s) a = ROL(a + MD4_F1(b,c,d) + X[k], s)
    #define ROUND2(a,b,c,d,k,s) a = ROL(a + MD4_G(b,c,d) + X[k] + 0x5a827999, s)
    #define ROUND3(a,b,c,d,k,s) a = ROL(a + MD4_H(b,c,d) + X[k] + 0x6ed9eba1, s)

    ROUND1(a,b,c,d, 0, 3);  ROUND1(d,a,b,c, 1, 7);  ROUND1(c,d,a,b, 2,11);  ROUND1(b,c,d,a, 3,19);
    ROUND1(a,b,c,d, 4, 3);  ROUND1(d,a,b,c, 5, 7);  ROUND1(c,d,a,b, 6,11);  ROUND1(b,c,d,a, 7,19);
    ROUND1(a,b,c,d, 8, 3);  ROUND1(d,a,b,c, 9, 7);  ROUND1(c,d,a,b,10,11);  ROUND1(b,c,d,a,11,19);
    ROUND1(a,b,c,d,12, 3);  ROUND1(d,a,b,c,13, 7);  ROUND1(c,d,a,b,14,11);  ROUND1(b,c,d,a,15,19);

    ROUND2(a,b,c,d, 0, 3);  ROUND2(d,a,b,c, 4, 5);  ROUND2(c,d,a,b, 8, 9);  ROUND2(b,c,d,a,12,13);
    ROUND2(a,b,c,d, 1, 3);  ROUND2(d,a,b,c, 5, 5);  ROUND2(c,d,a,b, 9, 9);  ROUND2(b,c,d,a,13,13);
    ROUND2(a,b,c,d, 2, 3);  ROUND2(d,a,b,c, 6, 5);  ROUND2(c,d,a,b,10, 9);  ROUND2(b,c,d,a,14,13);
    ROUND2(a,b,c,d, 3, 3);  ROUND2(d,a,b,c, 7, 5);  ROUND2(c,d,a,b,11, 9);  ROUND2(b,c,d,a,15,13);

    ROUND3(a,b,c,d, 0, 3);  ROUND3(d,a,b,c, 8, 9);  ROUND3(c,d,a,b, 4,11);  ROUND3(b,c,d,a,12,15);
    ROUND3(a,b,c,d, 2, 3);  ROUND3(d,a,b,c,10, 9);  ROUND3(c,d,a,b, 6,11);  ROUND3(b,c,d,a,14,15);
    ROUND3(a,b,c,d, 1, 3);  ROUND3(d,a,b,c, 9, 9);  ROUND3(c,d,a,b, 5,11);  ROUND3(b,c,d,a,13,15);
    ROUND3(a,b,c,d, 3, 3);  ROUND3(d,a,b,c,11, 9);  ROUND3(c,d,a,b, 7,11);  ROUND3(b,c,d,a,15,15);

    #undef ROUND1
    #undef ROUND2
    #undef ROUND3

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

void MD4_Init(MD4_CTX *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

void MD4_Update(MD4_CTX *ctx, const uint8_t *input, size_t len) {
    size_t i, idx, partLen;
    idx = (ctx->count[0] >> 3) & 0x3F;
    if ((ctx->count[0] += ((uint32_t)len << 3)) < ((uint32_t)len << 3))
        ctx->count[1]++;
    ctx->count[1] += ((uint32_t)len >> 29);
    partLen = 64 - idx;
    if (len >= partLen) {
        memcpy(&ctx->buffer[idx], input, partLen);
        MD4_Transform(ctx->state, ctx->buffer);
        for (i = partLen; i + 63 < len; i += 64)
            MD4_Transform(ctx->state, &input[i]);
        idx = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[idx], &input[i], len - i);
}

void MD4_Final(uint8_t digest[16], MD4_CTX *ctx) {
    uint8_t bits[8];
    MD4_Encode(bits, ctx->count, 8);
    size_t idx = (ctx->count[0] >> 3) & 0x3f;
    size_t padLen = (idx < 56) ? (56 - idx) : (120 - idx);
    static uint8_t PADDING[64] = { 0x80 };
    MD4_Update(ctx, PADDING, padLen);
    MD4_Update(ctx, bits, 8);
    MD4_Encode(digest, ctx->state, 16);
}

// ============================================================================
// NTLM Hash (MD4 of UTF-16LE password)
// ============================================================================

void ntlmHash(const char *password, uint8_t out[16]) {
    size_t len = strlen(password);
    size_t ulen = len * 2;
    uint8_t* buf = (uint8_t*)malloc(ulen);
    if (!buf) return;

    for (size_t i = 0; i < len; i++) {
        buf[2*i]   = (uint8_t)password[i];
        buf[2*i+1] = 0x00;
    }

    MD4_CTX ctx;
    MD4_Init(&ctx);
    MD4_Update(&ctx, buf, ulen);
    MD4_Final(out, &ctx);

    free(buf);
}

// ============================================================================
// MD5 Implementation (Ultra-lightweight for ESP32)
// ============================================================================

inline uint32_t ROTL32(uint32_t x, uint8_t n) {
    return (x << n) | (x >> (32 - n));
}

#define MD5_Ff(x,y,z) ((x & y) | (~x & z))
#define MD5_Gg(x,y,z) ((x & z) | (y & ~z))
#define MD5_Hh(x,y,z) (x ^ y ^ z)
#define MD5_Ii(x,y,z) (y ^ (x | ~z))

#define MD5_FF(a,b,c,d,x,s,ac) { a += MD5_Ff(b,c,d) + (x) + (uint32_t)(ac); a = ROTL32(a, s); a += b; }
#define MD5_GG(a,b,c,d,x,s,ac) { a += MD5_Gg(b,c,d) + (x) + (uint32_t)(ac); a = ROTL32(a, s); a += b; }
#define MD5_HH(a,b,c,d,x,s,ac) { a += MD5_Hh(b,c,d) + (x) + (uint32_t)(ac); a = ROTL32(a, s); a += b; }
#define MD5_II(a,b,c,d,x,s,ac) { a += MD5_Ii(b,c,d) + (x) + (uint32_t)(ac); a = ROTL32(a, s); a += b; }

void md5u_init(MD5U_CTX *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
}

void md5u_encode(uint8_t *out, const uint32_t *in, size_t len) {
    for (size_t i = 0, j = 0; j < len; i++, j += 4) {
        out[j]   = (uint8_t)( in[i]        & 0xFF);
        out[j+1] = (uint8_t)((in[i] >>  8) & 0xFF);
        out[j+2] = (uint8_t)((in[i] >> 16) & 0xFF);
        out[j+3] = (uint8_t)((in[i] >> 24) & 0xFF);
    }
}

void md5u_decode(uint32_t *out, const uint8_t *in, size_t len) {
    for (size_t i = 0, j = 0; j < len; i++, j += 4) {
        out[i] =  (uint32_t)in[j]
                | ((uint32_t)in[j+1] << 8)
                | ((uint32_t)in[j+2] << 16)
                | ((uint32_t)in[j+3] << 24);
    }
}

IRAM_ATTR void md5u_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    md5u_decode(x, block, 64);

    // Round 1
    MD5_FF(a,b,c,d, x[ 0],  7, 0xd76aa478); MD5_FF(d,a,b,c, x[ 1], 12, 0xe8c7b756);
    MD5_FF(c,d,a,b, x[ 2], 17, 0x242070db); MD5_FF(b,c,d,a, x[ 3], 22, 0xc1bdceee);
    MD5_FF(a,b,c,d, x[ 4],  7, 0xf57c0faf); MD5_FF(d,a,b,c, x[ 5], 12, 0x4787c62a);
    MD5_FF(c,d,a,b, x[ 6], 17, 0xa8304613); MD5_FF(b,c,d,a, x[ 7], 22, 0xfd469501);
    MD5_FF(a,b,c,d, x[ 8],  7, 0x698098d8); MD5_FF(d,a,b,c, x[ 9], 12, 0x8b44f7af);
    MD5_FF(c,d,a,b, x[10], 17, 0xffff5bb1); MD5_FF(b,c,d,a, x[11], 22, 0x895cd7be);
    MD5_FF(a,b,c,d, x[12],  7, 0x6b901122); MD5_FF(d,a,b,c, x[13], 12, 0xfd987193);
    MD5_FF(c,d,a,b, x[14], 17, 0xa679438e); MD5_FF(b,c,d,a, x[15], 22, 0x49b40821);

    // Round 2
    MD5_GG(a,b,c,d, x[ 1],  5, 0xf61e2562); MD5_GG(d,a,b,c, x[ 6],  9, 0xc040b340);
    MD5_GG(c,d,a,b, x[11], 14, 0x265e5a51); MD5_GG(b,c,d,a, x[ 0], 20, 0xe9b6c7aa);
    MD5_GG(a,b,c,d, x[ 5],  5, 0xd62f105d); MD5_GG(d,a,b,c, x[10],  9, 0x02441453);
    MD5_GG(c,d,a,b, x[15], 14, 0xd8a1e681); MD5_GG(b,c,d,a, x[ 4], 20, 0xe7d3fbc8);
    MD5_GG(a,b,c,d, x[ 9],  5, 0x21e1cde6); MD5_GG(d,a,b,c, x[14],  9, 0xc33707d6);
    MD5_GG(c,d,a,b, x[ 3], 14, 0xf4d50d87); MD5_GG(b,c,d,a, x[ 8], 20, 0x455a14ed);
    MD5_GG(a,b,c,d, x[13],  5, 0xa9e3e905); MD5_GG(d,a,b,c, x[ 2],  9, 0xfcefa3f8);
    MD5_GG(c,d,a,b, x[ 7], 14, 0x676f02d9); MD5_GG(b,c,d,a, x[12], 20, 0x8d2a4c8a);

    // Round 3
    MD5_HH(a,b,c,d, x[ 5],  4, 0xfffa3942); MD5_HH(d,a,b,c, x[ 8], 11, 0x8771f681);
    MD5_HH(c,d,a,b, x[11], 16, 0x6d9d6122); MD5_HH(b,c,d,a, x[14], 23, 0xfde5380c);
    MD5_HH(a,b,c,d, x[ 1],  4, 0xa4beea44); MD5_HH(d,a,b,c, x[ 4], 11, 0x4bdecfa9);
    MD5_HH(c,d,a,b, x[ 7], 16, 0xf6bb4b60); MD5_HH(b,c,d,a, x[10], 23, 0xbebfbc70);
    MD5_HH(a,b,c,d, x[13],  4, 0x289b7ec6); MD5_HH(d,a,b,c, x[ 0], 11, 0xeaa127fa);
    MD5_HH(c,d,a,b, x[ 3], 16, 0xd4ef3085); MD5_HH(b,c,d,a, x[ 6], 23, 0x04881d05);
    MD5_HH(a,b,c,d, x[ 9],  4, 0xd9d4d039); MD5_HH(d,a,b,c, x[12], 11, 0xe6db99e5);
    MD5_HH(c,d,a,b, x[15], 16, 0x1fa27cf8); MD5_HH(b,c,d,a, x[ 2], 23, 0xc4ac5665);

    // Round 4
    MD5_II(a,b,c,d, x[ 0],  6, 0xf4292244); MD5_II(d,a,b,c, x[ 7], 10, 0x432aff97);
    MD5_II(c,d,a,b, x[14], 15, 0xab9423a7); MD5_II(b,c,d,a, x[ 5], 21, 0xfc93a039);
    MD5_II(a,b,c,d, x[12],  6, 0x655b59c3); MD5_II(d,a,b,c, x[ 3], 10, 0x8f0ccc92);
    MD5_II(c,d,a,b, x[10], 15, 0xffeff47d); MD5_II(b,c,d,a, x[ 1], 21, 0x85845dd1);
    MD5_II(a,b,c,d, x[ 8],  6, 0x6fa87e4f); MD5_II(d,a,b,c, x[15], 10, 0xfe2ce6e0);
    MD5_II(c,d,a,b, x[ 6], 15, 0xa3014314); MD5_II(b,c,d,a, x[13], 21, 0x4e0811a1);
    MD5_II(a,b,c,d, x[ 4],  6, 0xf7537e82); MD5_II(d,a,b,c, x[11], 10, 0xbd3af235);
    MD5_II(c,d,a,b, x[ 2], 15, 0x2ad7d2bb); MD5_II(b,c,d,a, x[ 9], 21, 0xeb86d391);

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

void md5u_update(MD5U_CTX *ctx, const uint8_t *input, size_t len) {
    uint32_t i = 0, idx = (ctx->count[0] >> 3) & 0x3F;
    ctx->count[0] += (uint32_t)len << 3;
    if (ctx->count[0] < ((uint32_t)len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)len >> 29;

    uint32_t partLen = 64 - idx;
    if (len >= partLen) {
        memcpy(&ctx->buffer[idx], input, partLen);
        md5u_transform(ctx->state, ctx->buffer);
        for (i = partLen; i + 63 < len; i += 64)
            md5u_transform(ctx->state, &input[i]);
        idx = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[idx], &input[i], len - i);
}

void md5u_final(uint8_t digest[16], MD5U_CTX *ctx) {
    static const uint8_t PADDING[64] = { 0x80 };
    uint8_t bits[8];
    md5u_encode(bits, ctx->count, 8);

    uint32_t idx = (ctx->count[0] >> 3) & 0x3F;
    uint32_t padLen = (idx < 56) ? (56 - idx) : (120 - idx);
    md5u_update(ctx, PADDING, padLen);
    md5u_update(ctx, bits, 8);
    md5u_encode(digest, ctx->state, 16);
}

// ============================================================================
// HMAC-MD5 Functions
// ============================================================================

bool fastHMAC_MD5(const uint8_t *key, size_t keylen,
                  const uint8_t *msg, size_t msglen,
                  uint8_t out[16]) {
    uint8_t k_ipad[64], k_opad[64];
    uint8_t khash[16];

    // 1) If key > 64, key = MD5(key)
    const uint8_t* k = key;
    size_t klen = keylen;
    if (keylen > 64) {
        MD5U_CTX ck;
        md5u_init(&ck);
        md5u_update(&ck, key, keylen);
        md5u_final(khash, &ck);
        k = khash;
        klen = 16;
    }

    // 2) Prepare ipad/opad (64 bytes fixed)
    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5c, 64);
    for (size_t i = 0; i < klen; ++i) {
        k_ipad[i] ^= k[i];
        k_opad[i] ^= k[i];
    }

    // 3) inner = MD5( (k^ipad) || msg )
    uint8_t inner[16];
    MD5U_CTX c;
    md5u_init(&c);
    md5u_update(&c, k_ipad, 64);
    md5u_update(&c, msg, msglen);
    md5u_final(inner, &c);

    // 4) outer = MD5( (k^opad) || inner )
    md5u_init(&c);
    md5u_update(&c, k_opad, 64);
    md5u_update(&c, inner, 16);
    md5u_final(out, &c);

    return true;
}

bool hmacMD5(const uint8_t *key, size_t keylen,
             const uint8_t *msg, size_t msglen,
             uint8_t out[16]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info) return false;
    int r = mbedtls_md_hmac(info, key, keylen, msg, msglen, out);
    return (r == 0);
}

// ============================================================================
// Base64 Encoding/Decoding
// ============================================================================

String base64Encode(const uint8_t *data, size_t len) {
    size_t out_len = 0, out_size = 4 * ((len + 2) / 3) + 1;
    unsigned char *out_buf = (unsigned char*)malloc(out_size);
    if (!out_buf) return "";

    int ret = mbedtls_base64_encode(out_buf, out_size, &out_len, data, len);
    if (ret != 0) {
        Serial.printf("[-] Base64 encode error: %d\n", ret);
        free(out_buf);
        return "";
    }

    out_buf[out_len] = '\0';
    String s((char*)out_buf);
    free(out_buf);
    return s;
}

bool base64Decode(const String& b64, std::vector<uint8_t>& out) {
    size_t need = (b64.length() * 3) / 4 + 4;
    out.resize(need);
    size_t outLen = 0;

    int ret = mbedtls_base64_decode(out.data(), out.size(), &outLen,
                                    (const unsigned char*)b64.c_str(), b64.length());
    if (ret != 0 || outLen < 12) return false;
    out.resize(outLen);
    return true;
}

// ============================================================================
// Hex Encoding/Decoding
// ============================================================================

String toHex(const uint8_t* data, size_t len) {
    String s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        char buf[3];
        sprintf(buf, "%02x", data[i]);
        s += buf;
    }
    return s;
}

uint8_t hexCharToValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;  // to lowercase
    return 10 + (c - 'a');
}

void hexToBytes(const String &hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        out[i] = (hexCharToValue(hex[2*i]) << 4) | hexCharToValue(hex[2*i+1]);
    }
}

// ============================================================================
// String Utilities
// ============================================================================

String toUpperCaseStr(const String &s) {
    String out = s;
    out.toUpperCase();
    return out;
}

void toUTF16LE(const String &s, uint8_t **buf, size_t *outLen) {
    *outLen = s.length() * 2;
    *buf = (uint8_t*)malloc(*outLen);
    if (!*buf) {
        *outLen = 0;
        return;
    }
    for (size_t i = 0; i < s.length(); ++i) {
        (*buf)[2*i] = (uint8_t)s[i];
        (*buf)[2*i+1] = 0x00;
    }
}

void dumpHex(const char *label, const uint8_t *buf, size_t len) {
    Serial.print(label);
    for (size_t i = 0; i < len; i++) {
        if (buf[i] < 0x10) Serial.print("0");
        Serial.print(buf[i], HEX);
    }
    Serial.println();
}

// ============================================================================
// Vector Helpers
// ============================================================================

void vecPushLE16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
}

void vecPushLE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 24));
}

void vecPush8(std::vector<uint8_t>& v, uint8_t x) {
    v.push_back(x);
}

void vecPushUTF16LE(std::vector<uint8_t>& v, const char* ascii) {
    while (*ascii) {
        vecPush8(v, (uint8_t)*ascii);
        vecPush8(v, 0x00);
        ++ascii;
    }
}
