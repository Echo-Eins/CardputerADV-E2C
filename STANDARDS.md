# Cryptographic Standards Reference

This document describes the cryptographic standards and specifications implemented in the Cardputer Remote Desktop system.

---

## 1. Elliptic Curve Cryptography

### ECDSA (Digital Signatures)
- **Standard**: FIPS 186-4, ANSI X9.62
- **Curve**: secp256r1 (P-256, prime256v1)
- **Key Size**: 256-bit private key, 33-byte compressed public key
- **Hash**: SHA-256 (FIPS 180-4)
- **Signature Format**: r||s (64 bytes, each component 32 bytes big-endian)

### ECDH (Key Exchange)
- **Standard**: NIST SP 800-56A Rev. 3
- **Curve**: secp256r1 (P-256)
- **Shared Secret**: 32 bytes (x-coordinate of shared point)

### Curve Parameters (secp256r1)
```
p  = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
a  = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFC
b  = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
Gx = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
Gy = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5
n  = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
h  = 1
```

---

## 2. Key Derivation

### HKDF (HMAC-based Key Derivation Function)
- **Standard**: RFC 5869
- **Hash**: SHA-256
- **Salt**: SHA-256(client_nonce || server_nonce)
- **Info**: "cardputer-remote-v1-session-keys" (33 bytes)
- **Output**: 64 bytes

### Key Material Layout
```
Offset  Size  Purpose
0       16    AES key for client→server (c2s)
16      16    AES key for server→client (s2c)
32      32    HMAC key for transcript MAC
```

---

## 3. Authenticated Encryption

### AES-GCM
- **Standard**: NIST SP 800-38D
- **Key Size**: 128 bits
- **Nonce**: 96 bits (12 bytes)
- **Tag**: 128 bits (16 bytes)
- **AAD**: None used

### Nonce Structure
```
Byte    Content
0-3     Counter (big-endian, monotonically increasing)
4-11    Random (generated at session start)
```

### Packet Format
```
[Header: 4 bytes][Payload: variable][Tag: 16 bytes]

Header:
  Byte 0: Protocol version (0x01)
  Byte 1: Packet type
  Byte 2-3: Payload length (big-endian)

Encrypted Payload:
  [Nonce: 12 bytes][Ciphertext: variable]
```

---

## 4. Message Authentication

### HMAC-SHA256
- **Standard**: RFC 2104, FIPS 198-1
- **Key**: 32 bytes (from HKDF)
- **Output**: 32 bytes

### Transcript MAC Computation
```
MAC = HMAC-SHA256(hmac_key,
        client_ephemeral_pubkey ||
        server_ephemeral_pubkey ||
        client_nonce ||
        server_nonce)
```

---

## 5. Hash Functions

### SHA-256
- **Standard**: FIPS 180-4
- **Output**: 256 bits (32 bytes)
- **Used For**: ECDSA signing, HKDF, salt computation

---

## 6. Random Number Generation

### ESP32 (MbedTLS)
- **Implementation**: mbedtls_ctr_drbg (CTR_DRBG)
- **Standard**: NIST SP 800-90A
- **Entropy Source**: mbedtls_entropy (hardware RNG + system entropy)

### PC (Rust)
- **Implementation**: OsRng (ring/p256 crate)
- **Standard**: Uses OS CSPRNG (getrandom)

---

## 7. Protocol Handshake

### Message Sequence
```
1. Client → Server: HandshakeInit
   [ephemeral_pubkey:33][nonce:32][signature:64] = 129 bytes
   signature = ECDSA(client_private, SHA256(ephemeral_pubkey || nonce))

2. Server → Client: HandshakeResponse
   [ephemeral_pubkey:33][nonce:32][signature:64] = 129 bytes
   signature = ECDSA(server_private, SHA256(ephemeral_pubkey || client_nonce || server_nonce))

3. Client → Server: HandshakeComplete (encrypted)
   AES-GCM(transcript_mac)

4. Server → Client: SessionStart
   Empty payload
```

### Key Derivation Steps
```
1. shared_secret = ECDH(our_private, peer_ephemeral_public)
2. salt = SHA256(client_nonce || server_nonce)
3. prk = HMAC-SHA256(salt, shared_secret)
4. key_material = HKDF-Expand(prk, info, 64)
5. c2s_key = key_material[0:16]
6. s2c_key = key_material[16:32]
7. hmac_key = key_material[32:64]
```

---

## 8. Security Levels

| Primitive | Security Level |
|-----------|---------------|
| ECDSA P-256 | 128-bit |
| ECDH P-256 | 128-bit |
| AES-128-GCM | 128-bit |
| SHA-256 | 256-bit |
| HMAC-SHA256 | 256-bit |

**Overall System Security**: 128-bit (limited by smallest primitive)

---

## 9. Compliance References

| Standard | Description |
|----------|-------------|
| FIPS 186-4 | Digital Signature Standard (DSS) |
| FIPS 180-4 | Secure Hash Standard (SHS) |
| FIPS 198-1 | HMAC |
| NIST SP 800-38D | GCM Mode |
| NIST SP 800-56A | Key Establishment |
| NIST SP 800-90A | Random Number Generation |
| RFC 5869 | HKDF |
| RFC 2104 | HMAC |
| SEC 2 | Recommended Elliptic Curve Parameters |

---

## 10. Implementation Libraries

### ESP32 (C++)
- **MbedTLS** (bundled with ESP-IDF/Arduino)
  - mbedtls/ecdh.h - ECDH key exchange
  - mbedtls/ecdsa.h - ECDSA signatures
  - mbedtls/gcm.h - AES-GCM encryption
  - mbedtls/sha256.h - SHA-256 hash
  - mbedtls/md.h - HMAC
  - mbedtls/ctr_drbg.h - Random generation

### PC (Rust)
- **p256** crate - ECDSA/ECDH with P-256
- **aes-gcm** crate - AES-GCM encryption
- **sha2** crate - SHA-256
- **hkdf** crate - HKDF implementation
- **hmac** crate - HMAC
- **rand** crate - Random generation
