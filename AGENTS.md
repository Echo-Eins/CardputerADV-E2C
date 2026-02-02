# AGENTS.md — Cardputer Remote Desktop Development Guide

Comprehensive reference for AI agents and developers working on the
**cardputer-remote** system: an encrypted remote desktop consisting of a
Rust server (PC) and an ESP32-S3 client (M5Stack Cardputer).

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [File Map](#2-file-map)
3. [Protocol v1 Specification](#3-protocol-v1-specification)
4. [Cryptographic Implementation](#4-cryptographic-implementation)
5. [Server Internals (Rust / Tokio)](#5-server-internals-rust--tokio)
6. [Client Internals (ESP32 / Arduino)](#6-client-internals-esp32--arduino)
7. [ESP32 Hardware Constraints](#7-esp32-hardware-constraints)
8. [Input System](#8-input-system)
9. [Key Generation & Deployment](#9-key-generation--deployment)
10. [Known Pitfalls & Historical Bugs](#10-known-pitfalls--historical-bugs)
11. [Build & Run](#11-build--run)
12. [Debug Checklist](#12-debug-checklist)

---

## 1. Architecture Overview

```
┌──────────────────────┐        TCP :19847        ┌──────────────────────────┐
│   Rust Server (PC)   │◄────────────────────────►│  ESP32-S3 Client         │
│                      │   AES-128-GCM encrypted  │  (M5Stack Cardputer)     │
│  - Screen capture    │                          │  - 240x135 ST7789 LCD    │
│  - JPEG compression  │   ScreenFrame ──────►    │  - 56-key matrix kbd     │
│  - enigo input sim   │   ◄────── KeyPress       │  - WiFi (lwIP TCP/IP)    │
│  - tokio async       │   ◄────── MouseMove      │  - mbedtls crypto        │
│  - mDNS discovery    │   Heartbeat ◄────►       │  - SD card for keys      │
└──────────────────────┘                          └──────────────────────────┘
```

**Data flow**: Server captures screen → JPEG-compresses to 240x135 →
encrypts with AES-128-GCM → sends over TCP. Client decrypts → decodes
JPEG → renders to display. Client captures keyboard/mouse input →
encrypts → sends to server → server simulates via enigo.

**Security model**: Mutual authentication via ECDSA. Session keys derived
from ECDH + HKDF. All post-handshake traffic is AES-128-GCM authenticated
and encrypted. Replay protection via monotonic nonce counters.

---

## 2. File Map

### Server — `cardputer-remote/`

| File | Purpose |
|------|---------|
| `Cargo.toml` | Dependencies: tokio, p256, aes-gcm, hkdf, scrap, enigo, mdns-sd, image, tracing |
| `config.toml` | Runtime configuration (port, keys, display settings, logging) |
| `src/lib.rs` | Crate root. Exports `VERSION`, `PROTOCOL_VERSION`, public modules |
| `src/main.rs` | Entry point. Argument parsing, logging init, App event loop with `tokio::select!` |
| `src/config/mod.rs` | TOML config loading & validation. `Config`, `ServerConfig`, `SecurityConfig`, `DisplayConfig` |
| `src/protocol/mod.rs` | Wire format. `PacketHeader`, `PacketType` enum (0x00-0xF0), `Packet`, payload structs. **Two header parsers**: `from_bytes()` (with version check, for handshake) and `from_bytes_encrypted()` (no version check, for session — AES-GCM provides auth) |
| `src/crypto/mod.rs` | `CryptoContext` — ECDH ephemeral key exchange, HKDF-SHA256 key derivation, AES-128-GCM encrypt/decrypt, ECDSA sign/verify, nonce management with replay protection |
| `src/network/mod.rs` | `DiscoveryService` (mDNS), `Server` (TCP listener + handshake), `Connection` (cancellation-safe encrypted send/receive with persistent state buffers) |
| `src/network/session.rs` | `Session` — main event loop (`tokio::select!`): receives client packets, sends screen frames, heartbeats. Handles all packet types. Sends `ErrorPacket` to client before disconnecting on errors |
| `src/capture/mod.rs` | `ScreenCapturer` — uses `scrap` to grab screen, `image` to resize to 240x135, JPEG compression. Runs in `spawn_blocking` (scrap is !Send). Delta detection via sampling hash |
| `src/input/mod.rs` | `InputController` — `enigo` wrapper. USB HID keycode → `enigo::Key` mapping. Mouse move/click, key press/release with modifier support. Logs warnings on enigo failures |
| `src/bin/keygen.rs` | CLI tool to generate ECDSA keypairs + discovery cookie. Outputs hex (for config.toml) and binary (for SD card `/rd_keys/`) |

### Client — `Evil-Cardputer-v1-5-0/`

| File | Purpose |
|------|---------|
| `remote_desktop.h` | All protocol constants, packet type enum, error codes, key file paths, session state enum, public function declarations |
| `remote_desktop.cpp` | **~1900 lines**. Complete client implementation: key loading, mDNS discovery, TCP connect, ECDH handshake with mbedtls, encrypted packet send/receive, JPEG decode & render, keyboard/mouse input processing, settings UI |
| `Evil-Cardputer-v1-5-0.ino` | Main Arduino sketch — menu system that calls `remoteDesktop()` |

### Client (ESP-IDF, alternate) — `cardputer-remote-client/`

| File | Purpose |
|------|---------|
| `src/main.c` | ESP-IDF-based client entry point |
| `src/remote_crypto.c` | Cryptographic operations via mbedtls |
| `src/remote_network.c` | Network communication |
| `src/remote_display.c` | Display rendering |
| `src/remote_input.c` | Input capture |
| `include/*.h` | Corresponding headers |
| `partitions.csv` | Flash partition table |

### Support Files

| Path | Purpose |
|------|---------|
| `SD-Card-File/` | SD card contents including configs, payloads, audio |
| `utilities/Bad_Usb_Lib/` | USB HID keyboard layouts (28 locales) |
| `cardputer-remote/src/bin/keygen.rs` | Key generation CLI |

---

## 3. Protocol v1 Specification

### Wire Format

Every packet follows the same structure:

```
┌───────────┬───────────┬──────────────┬──────────────┬──────────────┐
│ Version   │ Type      │ Length (BE)   │ Payload      │ Tag          │
│ 1 byte    │ 1 byte    │ 2 bytes       │ N bytes      │ 16 bytes     │
│ 0x01      │ 0x00-0xF0 │ uint16 BE     │ variable     │ AES-GCM tag  │
└───────────┴───────────┴──────────────┴──────────────┴──────────────┘
                         ◄── Length ────►
Header: 4 bytes                          Tag: 16 bytes (zero during handshake)
```

- `Length` = size of Payload only (does NOT include header or tag)
- For encrypted packets: Payload = `nonce(12) || ciphertext`
- For handshake packets: Payload = raw unencrypted data, tag = zeros
- `MAX_PAYLOAD_SIZE` = 65515 bytes

### Packet Types

| Code | Name | Direction | Payload |
|------|------|-----------|---------|
| `0x00` | DiscoveryRequest | C→S | `cookie(16)` |
| `0x01` | DiscoveryResponse | S→C | `cookie(16) \|\| device_name \|\| port(2 BE)` |
| `0x02` | HandshakeInit | C→S | `pubkey(65) \|\| nonce(32) \|\| signature(64)` = 161 bytes |
| `0x03` | HandshakeResponse | S→C | `pubkey(65) \|\| nonce(32) \|\| signature(64)` = 161 bytes |
| `0x04` | HandshakeComplete | C→S | Encrypted: `transcript_mac(32)` |
| `0x10` | SessionStart | S→C | Encrypted: empty |
| `0x11` | SessionEnd | C→S / S→C | Encrypted: empty |
| `0x12` | SessionTimeout | S→C | Encrypted: empty |
| `0x13` | Heartbeat | S→C / C→S | Encrypted: empty |
| `0x14` | HeartbeatAck | S→C / C→S | Encrypted: empty |
| `0x20` | ScreenFrame | S→C | Encrypted: `sequence(4 BE) \|\| timestamp(4 BE) \|\| jpeg_data` |
| `0x21` | ScreenDelta | S→C | Reserved |
| `0x22` | ScreenRequest | C→S | Encrypted: empty |
| `0x30` | MouseMove | C→S | Encrypted: `dx(i8) \|\| dy(i8)` |
| `0x31` | MouseClick | C→S | Encrypted: `button(u8) \|\| action(u8)` |
| `0x32` | KeyPress | C→S | Encrypted: `keycode(u8) \|\| modifiers(u8)` |
| `0x33` | KeyRelease | C→S | Encrypted: `keycode(u8) \|\| modifiers(u8)` |
| `0x34` | KeyType | C→S | Encrypted: UTF-8 string |
| `0x40` | ModeSwitch | C→S | Encrypted: JSON `{"mode": 0}` |
| `0x41` | ModeAck | S→C | Encrypted: JSON `{"mode": 0}` |
| `0xF0` | ErrorPacket | S→C | Encrypted: UTF-8 error message |

### Connection Lifecycle

```
Client                                          Server
  │                                                │
  │──── DiscoveryRequest (cookie) ────────────────►│
  │◄──── DiscoveryResponse (cookie, name, port) ───│
  │                                                │
  │──── HandshakeInit (ephPub, nonce, sig) ───────►│  Client signs: ephPub || nonce
  │◄──── HandshakeResponse (ephPub, nonce, sig) ───│  Server signs: ephPub || cNonce || sNonce
  │                                                │
  │  Both derive session keys via ECDH + HKDF      │
  │                                                │
  │──── HandshakeComplete (encrypted MAC) ────────►│  HMAC(cPub||sPub||cNonce||sNonce)
  │◄──── SessionStart (encrypted) ─────────────────│
  │                                                │
  │         ═══ Encrypted session ═══              │
  │◄──── ScreenFrame (JPEG) ───────────────────────│  ~10 FPS
  │──── KeyPress/MouseMove ───────────────────────►│
  │◄────────── Heartbeat ──────────────────────────│  Every 5s
  │──────────── HeartbeatAck ─────────────────────►│
  │                                                │
  │──── SessionEnd ───────────────────────────────►│
  │                                                │
```

### Version Check Policy

- **Handshake packets** (`receive_packet` in `Server`): Use `PacketHeader::from_bytes()` which validates `version == 0x01`. This catches protocol mismatches before keys are established.
- **Session packets** (`Connection::receive`): Use `PacketHeader::from_bytes_encrypted()` which **skips** version validation. Post-handshake, AES-GCM authentication replaces the version byte's role. This prevents false "Invalid protocol version" errors from stream alignment issues.

---

## 4. Cryptographic Implementation

### Key Exchange — ECDH on secp256r1

- **Curve**: NIST P-256 (secp256r1 / prime256v1)
- **Server**: `p256` crate with `EphemeralSecret`
- **Client**: mbedtls `mbedtls_ecdh_*` functions

**Critical**: Public keys are always sent as **uncompressed** 65-byte points
(0x04 prefix). ESP32 Arduino's mbedtls build lacks `MBEDTLS_ECP_POINT_COMPRESSION`,
so it **cannot parse compressed (33-byte) points**. The server accepts both
formats on receive but always sends uncompressed.

### Key Derivation — HKDF-SHA256 (RFC 5869)

```
shared_secret = ECDH(our_ephemeral, peer_ephemeral)
salt = SHA256(client_nonce || server_nonce)
info = "cardputer-remote-v1-session-keys"   // 31 bytes, ASCII
OKM  = HKDF-Expand(shared_secret, salt, info, 64 bytes)

client_to_server_key = OKM[0..16]    // AES-128 key
server_to_client_key = OKM[16..32]   // AES-128 key
hmac_key             = OKM[32..64]   // HMAC-SHA256 key
```

**Historical bug**: HKDF `info` was initially sent with the wrong length
(length byte mismatch between Rust and C). Fix: use a fixed-length ASCII
string without length prefix. Both sides must use the identical 31-byte
string `"cardputer-remote-v1-session-keys"`.

### Encryption — AES-128-GCM

- **Key size**: 128 bits (16 bytes)
- **Nonce**: 12 bytes (96 bits)
- **Tag**: 16 bytes (128 bits)
- **Directional keys**: Server encrypts with `server_to_client_key`, client encrypts with `client_to_server_key`

### Nonce Format

```
┌─────────────────────┬──────────────────────────┐
│ Counter (4 bytes BE)│ Random (8 bytes)          │
└─────────────────────┴──────────────────────────┘
       monotonic           fixed per session
```

- **Counter**: Starts at 0, increments by 1 per packet. Big-endian uint32.
- **Random**: Generated once per session (from the first 8 bytes of the handshake nonce). Fixed for the lifetime of the session.
- **Replay protection**: Receiver tracks `last_incoming_nonce`. Counter must be `>= last_incoming_nonce`. After accepting, sets `last_incoming_nonce = counter + 1`.

**Nonce overflow**: At `u32::MAX` (4,294,967,295 packets), the session must be renegotiated. At 10 FPS this is ~13.6 years.

### Transcript MAC (Handshake Verification)

After key derivation, the client computes:

```
transcript = client_ephemeral_pubkey || server_ephemeral_pubkey || client_nonce || server_nonce
mac = HMAC-SHA256(hmac_key, transcript)
```

This MAC is encrypted and sent as `HandshakeComplete`. The server recomputes
and verifies with constant-time comparison.

### Signature Format

- **Algorithm**: ECDSA with SHA-256 on secp256r1
- **Format**: Raw `r || s` (32 + 32 = 64 bytes). NOT DER-encoded.
- **HandshakeInit signed data**: `ephemeral_pubkey || nonce`
- **HandshakeResponse signed data**: `ephemeral_pubkey || client_nonce || server_nonce`

---

## 5. Server Internals (Rust / Tokio)

### Cancellation-Safe Receive (Critical)

The session event loop uses `tokio::select!` with multiple branches:

```rust
tokio::select! {
    result = self.connection.receive() => { /* handle packet */ }
    Some(frame) = frame_rx.recv() => { /* send frame */ }
    _ = heartbeat_interval.tick() => { /* send heartbeat */ }
    _ = timeout_interval.tick() => { /* check timeout */ }
}
```

When a branch wins (e.g., `frame_rx.recv()`), all other pending futures are
**dropped**. If `connection.receive()` was mid-read using `read_exact()`:

1. `read_exact` consumed N bytes from `BufReader`'s internal buffer into its destination
2. Future is dropped → destination (a local variable) is destroyed
3. Those N bytes are **permanently lost** from the TCP stream
4. Next `receive()` call reads from the wrong position → stream misalignment
5. Server sees garbage → "Invalid protocol version: 158" errors

**The fix**: `Connection` stores all receive state in persistent struct fields:

```rust
pub struct Connection {
    reader: BufReader<OwnedReadHalf>,
    writer: OwnedWriteHalf,
    crypto: CryptoContext,
    // Persistent state — survives cancellation
    recv_header: [u8; 4],
    recv_header_filled: usize,
    recv_packet_type: Option<PacketType>,
    recv_payload_len: usize,
    recv_payload: Vec<u8>,
    recv_payload_filled: usize,
    recv_tag: [u8; 16],
    recv_tag_filled: usize,
}
```

`receive()` uses `reader.read(&mut self.recv_header[filled..])` (NOT
`read_exact`). Each read writes directly into the persistent buffer. Progress
tracking (`filled` counters) is updated between `.await` points (cancellation
can only happen at `.await`). If cancelled, the next call resumes exactly
where it left off.

### BufReader

`BufReader::with_capacity(65536, read_half)` — 64KB internal buffer. This
batches small OS reads (reduces syscalls) without affecting cancellation
safety, because we use `read()` not `read_exact()`.

### Screen Capture

`scrap::Capturer` is `!Send`, so it runs inside `tokio::task::spawn_blocking`.
Frames are sent to the session loop via `mpsc::channel(2)` (bounded, backpressure).
Delta detection: A sampling hash over 256 evenly-spaced RGB bytes. If hash
matches previous frame, the frame is skipped.

### Logging

- Default level: `info` (set in `config.toml`)
- Key press/release and mouse click: `info!` level (visible by default)
- Mouse move: `debug!` level (too noisy at info)
- enigo failures: `warn!` level (previously silently discarded with `let _ =`)
- Use `-v` flag or `level = "debug"` in config for verbose output

---

## 6. Client Internals (ESP32 / Arduino)

### Session Structure

```cpp
struct RDSession {
    WiFiClient client;            // TCP connection
    RDSessionState state;         // DISCONNECTED → CONNECTING → HANDSHAKE → CONNECTED
    mbedtls_gcm_context gcmEncrypt;  // AES-128-GCM for outgoing
    mbedtls_gcm_context gcmDecrypt;  // AES-128-GCM for incoming
    uint32_t txCounter;           // Outgoing nonce counter
    uint32_t rxCounter;           // Expected incoming nonce counter
    uint8_t txNonceRandom[8];     // Our random nonce part
    uint8_t rxNonceRandom[8];     // Peer's random nonce part
    // ... key material, handshake state
};
```

### Packet Receive — `rdReceivePacketEx`

```
1. Poll available() >= HEADER_SIZE in a loop (with timeout parameter)
2. readBytes(header, 4) — consumes header from TCP stream
3. Validate version byte (0x01)
4. readBytes(payload, payloadLen) — streams as data arrives
5. readBytes(tag, 16)
```

**TCP receive buffer**: Set to **30KB** via `setsockopt(SO_RCVBUF, 30720)` after
`connect()`. This allows the OS to buffer 2-3 full screen frames (~8-10KB each)
without backpressure stalling the sender.

**No artificial timeout**: The previous `setTimeout(10000)` workaround caused
handshake failures and is removed. With a 30KB buffer, `readBytes()` naturally
streams data as it arrives. The default WiFiClient timeout (1 second) is
sufficient.

### Frame Rendering

```cpp
// Decrypt: nonce(12) || ciphertext → plaintext
// Parse: sequence(4) || timestamp(4) || jpeg_data
// Decode JPEG to RGB565 framebuffer
// Blit to M5.Display
```

Uses `TJpgDec` library for hardware-accelerated JPEG decoding on ESP32-S3.

### Settings UI

Scrollable vertical menu drawn with M5GFX. Scrollbar on the right side.
Navigation: UP/DOWN arrows scroll, ENTER selects, ESC returns. Menu items
include server IP, port, auto-connect toggle, FPS setting.

### Disconnect Reason Display

On disconnect, a red overlay at the bottom of the screen shows the reason
string for 3 seconds. If the server sends an `ErrorPacket` before
disconnecting, the client decrypts and displays the server's error message.

---

## 7. ESP32 Hardware Constraints

### Memory
- **Total RAM**: ~520KB (ESP32-S3)
- **PSRAM**: 8MB (QSPI, slower access)
- **Static buffers**: `rxBuffer[32768]`, `decBuffer[32768]` — allocated static, not on stack
- **lwIP TCP receive buffer**: Default ~5-6KB (`TCP_WND`). Overridden to 30KB via `setsockopt`

### Display
- **Resolution**: 240x135 pixels
- **Controller**: ST7789V2
- **Color**: RGB565 (16-bit)
- **Interface**: SPI
- **Library**: M5GFX / LovyanGFX

### Keyboard
- **Type**: 56-key matrix (not a standard USB HID keyboard)
- **Library**: M5Cardputer keyboard API (`M5Cardputer.Keyboard`)
- **FN key**: Hardware modifier for alternate key functions
- **Layout** (physical):
  ```
  `  1  2  3  4  5  6  7  8  9  0  BS
  Tab q  w  e  r  t  y  u  i  o  p  '
  FN   a  s  d  f  g  h  j  k  l  ENT
       z  x  c  v  b  n  m  ,  .  /
              [SPACE]        ←  ↑  →
                             ↓ (FN+/)
  ```

### Cryptography (mbedtls)
- **ECDH**: `mbedtls_ecdh_*` — secp256r1 only
- **AES-GCM**: `mbedtls_gcm_*` — hardware-accelerated on ESP32-S3
- **NO point compression**: `MBEDTLS_ECP_POINT_COMPRESSION` is not enabled in ESP32 Arduino's mbedtls. All public keys must be **uncompressed (65 bytes, 0x04 prefix)**.
- **ECDSA signature format**: mbedtls outputs DER-encoded signatures. Must convert to raw `r||s` (64 bytes) before sending to the Rust server which expects raw format.

### WiFi / TCP
- **WiFiClient**: Arduino WiFi library wrapping lwIP
- **Nagle's algorithm**: Disabled with `setNoDelay(true)` for low latency
- **SO_RCVBUF**: Must be set via raw `setsockopt` after `connect()` — no high-level API
- **fd() method**: Returns the lwIP socket file descriptor for raw socket operations
- **readBytes()**: Blocking read that returns when data is available or timeout expires. Streams data as TCP delivers it — no need to wait for all bytes to be buffered.
- **available()**: Returns bytes currently in the lwIP receive buffer. Does NOT trigger a TCP read. Using `while(available() < N)` for N > buffer size creates a **deadlock**.

---

## 8. Input System

### FN Mode Bindings (Client-Side)

The client handles FN-key mode switching locally. When FN is held:

| Key | Action |
|-----|--------|
| FN + Arrow Up | Mouse move up (sends `MouseMove {dx:0, dy:-1}`) |
| FN + Arrow Down | Mouse move down |
| FN + Arrow Left | Mouse move left |
| FN + Arrow Right | Mouse move right |
| FN + L | Left mouse click (sends `MouseClick {button:Left, action:Click}`) |
| FN + ' | Right mouse click (sends `MouseClick {button:Right, action:Click}`) |

Without FN, arrow keys and letter keys send `KeyPress`/`KeyRelease` packets
with USB HID keycodes.

### USB HID Keycode Mapping (Server-Side)

The server's `InputController::build_keymap()` maps HID codes to `enigo::Key`:

| Range | Keys | HID codes |
|-------|------|-----------|
| `0x04`-`0x1D` | a-z | Letters |
| `0x1E`-`0x26` | 1-9 | Numbers |
| `0x27` | 0 | |
| `0x28` | Enter | |
| `0x29` | Escape | |
| `0x2A` | Backspace | |
| `0x2B` | Tab | |
| `0x2C` | Space | |
| `0x2D`-`0x38` | - = [ ] \ ; ' ` , . / | Punctuation |
| `0x3A`-`0x45` | F1-F12 | Function keys |
| `0x4A`-`0x4E` | Home, PgUp, Del, End, PgDn | Navigation |
| `0x4F`-`0x52` | Right, Left, Down, Up | Arrow keys |

### Modifier Flags (byte `modifiers`)

| Bit | Modifier |
|-----|----------|
| `0x01` | Ctrl |
| `0x02` | Shift |
| `0x04` | Alt |
| `0x08` | GUI/Meta/Win |

### Key Packet Format

Sent as raw bytes (NOT JSON):

```
KeyPress:   [keycode: u8, modifiers: u8]   // 2 bytes
KeyRelease: [keycode: u8, modifiers: u8]   // 2 bytes
```

**Historical bug**: Early versions serialized key events as JSON. This was
changed to raw 2-byte format for efficiency and reliability.

---

## 9. Key Generation & Deployment

### Using the keygen tool

```bash
cd cardputer-remote
cargo run --bin keygen            # Generate all keys + cookie
cargo run --bin keygen -- --output /mnt/sdcard  # Write binary files to SD card
```

### Files on SD Card (`/rd_keys/`)

| File | Size | Contents |
|------|------|----------|
| `client.key` | 32 bytes | Cardputer ECDSA private key (raw) |
| `client.pub` | 65 bytes | Cardputer public key (uncompressed, 0x04 prefix) |
| `server.pub` | 65 bytes | Server public key (uncompressed, 0x04 prefix) |
| `cookie` | 16 bytes | Discovery cookie (must match server's `config.toml`) |

### Server config.toml

```toml
[security]
discovery_cookie = "a1b2c3d4..."       # 32 hex chars (16 bytes)
private_key = "0123456789abcdef..."     # 64 hex chars (32 bytes)
cardputer_public_key = "04deadbeef..."  # 130 hex chars (65 bytes, uncompressed)
```

The `cardputer_public_key` must be the hex-encoded contents of the client's
`client.pub` file. It can be 66 chars (compressed, 33 bytes) or 130 chars
(uncompressed, 65 bytes).

---

## 10. Known Pitfalls & Historical Bugs

### ESP32 mbedtls: No Compressed EC Points

**Symptom**: Handshake fails with `-0x4E80` error.
**Cause**: ESP32 Arduino's mbedtls lacks `MBEDTLS_ECP_POINT_COMPRESSION`.
Compressed 33-byte EC points cannot be parsed.
**Fix**: Always send uncompressed 65-byte public keys (0x04 prefix).
The server's `generate_ephemeral_keypair()` uses `to_encoded_point(false)`.

### HKDF Info String Mismatch

**Symptom**: Decryption fails immediately after handshake — wrong session keys.
**Cause**: HKDF `info` parameter was different lengths or content between
server and client.
**Fix**: Both sides use the exact ASCII string
`"cardputer-remote-v1-session-keys"` (31 bytes, no length prefix, no null terminator in the hashing).

### tokio::select! Cancellation → Stream Misalignment

**Symptom**: "Invalid protocol version: 158" after ~10-30 seconds of stable session.
**Cause**: `read_exact()` on `BufReader` is NOT cancellation-safe.
When `tokio::select!` drops the receive future mid-read, bytes consumed from
BufReader's internal buffer are lost with the dropped future's local variables.
**Fix**: Persistent receive buffers in `Connection` struct with manual
`read()` + progress tracking instead of `read_exact()`.
See [Section 5: Cancellation-Safe Receive](#cancellation-safe-receive-critical).

### ESP32 TCP Buffer Deadlock

**Symptom**: "Payload timeout: need 8126, have 7176" — client freezes.
**Cause**: `while(available() < totalBytes)` with `totalBytes > TCP_WND`.
The lwIP receive buffer defaults to ~5-6KB. If a frame is 8KB+, the condition
can never be satisfied: we wait for all bytes to be buffered, but the buffer
is too small to hold them all.
**Fix**: Use `readBytes()` which streams data as it arrives (no need to buffer
everything first). Also `setsockopt(SO_RCVBUF, 30720)` after connect.

### setTimeout(10000) Causing Handshake Failures

**Symptom**: Handshake works sometimes but fails in certain network conditions.
**Cause**: `WiFiClient.setTimeout(10000)` was set globally during the session
loop. This persisted into subsequent connection attempts, making handshake
operations wait too long before detecting failures.
**Fix**: Removed the global timeout override. Default WiFiClient timeout
(1 second) is sufficient with the 30KB receive buffer.

### HeartbeatAck Not Handled

**Symptom**: "Unknown packet type: 0x14" in client logs after reconnection.
**Cause**: `HeartbeatAck` (0x14) was missing from the client's packet type
switch statement.
**Fix**: Added `case RD_PKT_HEARTBEAT_ACK: break;` to the client's receive loop.

### Keyboard Input Not Working — enigo Key::Unicode Fails on Linux

**Symptom**: Key presses on Cardputer do nothing on the server. Server logs:
```
WARN Enigo key press failed for Unicode('n'): you tried to simulate invalid input:
     (key state could not be converted to u32)
```
**Cause**: `enigo.key(Key::Unicode(c), Direction::Press)` fails on Linux with
the xkbcommon backend. The backend cannot convert Unicode characters to
platform keycodes for discrete press/release simulation.
**Fix**: For `Key::Unicode` keys (letters, digits, punctuation), use
`enigo.text(&ch.to_string())` instead, which goes through a higher-level
text input path that works reliably. Shift is applied by mapping to the
shifted character using a US keyboard layout table. Named keys (`Key::Return`,
`Key::Tab`, `Key::UpArrow`, `Key::F1`, etc.) continue using `enigo.key()`
since they have direct platform keysym mappings.

**Other historical causes** (all fixed):
1. Key events logged at `debug!` level — invisible at default `info` level. Fixed: upgraded to `info!`.
2. `enigo` failures silently discarded with `let _ = ...`. Fixed: now logs `warn!`.
3. Client sending wrong keycode format — was JSON, now raw bytes `[keycode, modifier]`.

### Heartbeat Timing

**Symptom**: Immediate session timeout after connecting.
**Cause**: `lastHeartbeat` initialized to 0 instead of `millis()`. The first
timeout check saw `millis() - 0 > threshold` and immediately disconnected.
**Fix**: `uint32_t lastHeartbeat = millis();`

### Unencrypted Post-Handshake Packets

**Symptom**: Server rejected Heartbeat/KeyPress with decryption errors.
**Cause**: Client sent some post-handshake packets without encryption.
**Fix**: ALL packets after `HandshakeComplete` must be encrypted with
`rdSendEncrypted()`, including Heartbeat, HeartbeatAck, SessionEnd.

### Nonce Random Part Mismatch

**Symptom**: All decryption fails with "Invalid nonce".
**Cause**: The nonce random part validation checked against the wrong source
(peer's nonce random vs. our own).
**Fix**: `incoming_nonce_random` is set from the first 8 bytes of the
**peer's** handshake nonce. Outgoing nonce random is generated locally.

---

## 11. Build & Run

### Server (Rust)

```bash
cd cardputer-remote

# Check (no linking — avoids needing libxdo etc.)
cargo check

# Build (needs system libraries: libxdo-dev, libxcb-randr0-dev on Linux)
sudo apt install libxdo-dev libxcb-randr0-dev  # Debian/Ubuntu
cargo build --release

# Generate keys
cargo run --bin keygen

# Run server
cargo run -- -c config.toml -v
```

**System dependencies** (for linking):
- `libxdo` — enigo keyboard/mouse simulation (X11)
- `libxcb`, `libxcb-shm`, `libxcb-randr` — scrap screen capture (X11)

### Client (Arduino IDE)

1. Install M5Stack board support in Arduino IDE
2. Install libraries: `M5Cardputer`, `ArduinoJson`, `TJpg_Decoder`
3. Open `Evil-Cardputer-v1-5-0/Evil-Cardputer-v1-5-0.ino`
4. Select board: "M5Cardputer" or "ESP32-S3 Dev Module"
5. Set partition scheme: Huge APP (3MB No OTA)
6. Flash

### Deploying Keys

1. Run `cargo run --bin keygen -- --output /path/to/sdcard`
2. This creates `/path/to/sdcard/rd_keys/` with binary key files
3. Copy hex values from keygen output into server's `config.toml`
4. Insert SD card into Cardputer

---

## 12. Debug Checklist

### Connection Issues

- [ ] WiFi connected? Check `WiFi.status() == WL_CONNECTED`
- [ ] Server reachable? Ping the server IP from another device
- [ ] Correct port? Default is 19847, check both config.toml and client config
- [ ] Firewall? Open TCP port 19847 on the server
- [ ] Discovery cookie matches? Both sides must have the same 16-byte cookie

### Handshake Failures

- [ ] Keys generated together? Use `keygen --both` to ensure matching pairs
- [ ] Public keys correct? Server's `cardputer_public_key` must match client's `client.pub`
- [ ] Key format? Server accepts compressed (33) or uncompressed (65). Client REQUIRES uncompressed (65) from server.
- [ ] Check server log: "Client signature verified" → "Session keys derived" → "Transcript MAC verified"
- [ ] If "Expected HandshakeComplete" timeout: client may not be sending it. Check client serial output.

### Session Drops

- [ ] "Invalid protocol version: N" → Was cancellation-safe receive in place? Check `Connection` has persistent buffers
- [ ] "Payload timeout" on client → Check TCP buffer size (should be 30KB)
- [ ] Immediate timeout → Check `lastHeartbeat` initialized to `millis()`
- [ ] After ~4 billion packets → Nonce overflow (theoretical, 13.6 years at 10 FPS)

### Keyboard Not Working

- [ ] Run server with `-v` flag or `level = "debug"` in config.toml
- [ ] Check server log for "Key press: keycode=0x..." messages
- [ ] If no key messages: client not sending. Check client serial output.
- [ ] If key messages but no effect: check for "Enigo key press failed" warnings
- [ ] If "Unknown HID keycode": keycode not in server's keymap. Add it to `build_keymap()`
- [ ] On headless Linux: enigo requires X11 display. Set `DISPLAY=:0`

### Error Messages Reference

| Server Log | Meaning |
|------------|---------|
| `Invalid protocol version: N` | Stream misalignment or wrong client |
| `Decryption failed` | Wrong keys, nonce replay, or corrupted data |
| `Invalid packet type: 0xNN` | Unknown or unsupported packet type |
| `Connection closed` | TCP connection dropped (client disconnected or network issue) |
| `Receive error` | IO error during packet read |
| `Enigo key press failed` | OS-level input simulation failed |

| Client Serial | Meaning |
|---------------|---------|
| `[RD] Bad version: 0xNN` | Server sent wrong protocol version |
| `[RD] Payload incomplete` | TCP read timed out mid-packet |
| `[RD] Server error: ...` | Server sent ErrorPacket with message before disconnect |
| `[RD] Unknown packet type` | Server sent an unrecognized packet type |
| `[RD] Handshake failed` | Crypto error during key exchange |
| `[RD] GCM decrypt failed` | Wrong session keys or corrupted data |
