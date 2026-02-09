/*
 * Crypto Utilities for Evil-Cardputer
 *
 * This module provides cryptographic primitives:
 * - MD4 hash implementation
 * - MD5 hash implementation (lightweight)
 * - HMAC-MD5
 * - NTLM hash generation
 * - Base64 encoding/decoding
 * - Hex encoding/decoding utilities
 * - UTF-16LE conversion
 */

#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <vector>

// ============================================================================
// MD4 Implementation
// ============================================================================

typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
} MD4_CTX;

// Initialize MD4 context
void MD4_Init(MD4_CTX *ctx);

// Update MD4 with data
void MD4_Update(MD4_CTX *ctx, const uint8_t *input, size_t len);

// Finalize and get MD4 digest (16 bytes)
void MD4_Final(uint8_t digest[16], MD4_CTX *ctx);

// Internal functions (exposed for advanced use)
void MD4_Transform(uint32_t state[4], const uint8_t block[64]);
void MD4_Encode(uint8_t *output, const uint32_t *input, size_t len);

// ============================================================================
// MD5 Implementation (Ultra-lightweight for ESP32)
// ============================================================================

typedef struct {
    uint32_t state[4];   // A, B, C, D
    uint32_t count[2];   // nb bits (mod 2^64)
    uint8_t  buffer[64]; // bloc en cours
} MD5U_CTX;

// Initialize MD5 context
void md5u_init(MD5U_CTX *ctx);

// Update MD5 with data
void md5u_update(MD5U_CTX *ctx, const uint8_t *input, size_t len);

// Finalize and get MD5 digest (16 bytes)
void md5u_final(uint8_t digest[16], MD5U_CTX *ctx);

// Internal functions
void md5u_encode(uint8_t *out, const uint32_t *in, size_t len);
void md5u_decode(uint32_t *out, const uint8_t *in, size_t len);
void md5u_transform(uint32_t state[4], const uint8_t block[64]);

// ============================================================================
// HMAC-MD5 Functions
// ============================================================================

// Fast HMAC-MD5 using internal MD5 implementation
bool fastHMAC_MD5(const uint8_t *key, size_t keylen,
                  const uint8_t *msg, size_t msglen,
                  uint8_t out[16]);

// HMAC-MD5 using mbedtls (for verification/compatibility)
bool hmacMD5(const uint8_t *key, size_t keylen,
             const uint8_t *msg, size_t msglen,
             uint8_t out[16]);

// ============================================================================
// NTLM Hash
// ============================================================================

// Generate NTLM hash (MD4 of UTF-16LE password)
void ntlmHash(const char *password, uint8_t out[16]);

// ============================================================================
// Base64 Encoding/Decoding
// ============================================================================

// Encode binary data to base64 string
String base64Encode(const uint8_t *data, size_t len);

// Decode base64 string to binary data
bool base64Decode(const String& b64, std::vector<uint8_t>& out);

// ============================================================================
// Hex Encoding/Decoding
// ============================================================================

// Convert binary data to hex string
String toHex(const uint8_t* data, size_t len);

// Convert single hex character to value
uint8_t hexCharToValue(char c);

// Convert hex string to binary data
void hexToBytes(const String &hex, uint8_t *out, size_t len);

// ============================================================================
// String Utilities
// ============================================================================

// Convert string to uppercase
String toUpperCaseStr(const String &s);

// Convert ASCII string to UTF-16LE (allocates memory, caller must free)
void toUTF16LE(const String &s, uint8_t **buf, size_t *outLen);

// Debug: dump hex data to Serial
void dumpHex(const char *label, const uint8_t *buf, size_t len);

// ============================================================================
// Vector Helpers (for protocol building)
// ============================================================================

// Push little-endian 16-bit value to vector
void vecPushLE16(std::vector<uint8_t>& v, uint16_t x);

// Push little-endian 32-bit value to vector
void vecPushLE32(std::vector<uint8_t>& v, uint32_t x);

// Push single byte to vector
void vecPush8(std::vector<uint8_t>& v, uint8_t x);

// Push UTF-16LE string to vector
void vecPushUTF16LE(std::vector<uint8_t>& v, const char* ascii);

#endif // CRYPTO_UTILS_H
