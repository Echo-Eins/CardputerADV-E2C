# Cardputer Remote - System Analysis

## Overview

**cardputer-remote** is a secure remote desktop system consisting of two components:
- **Server** (Rust, ~1500 LOC) - runs on a PC, captures screen, receives input
- **Client** (C/ESP-IDF, ~3660 LOC) - runs on M5Stack Cardputer / Cardputer ADV, displays screen, sends input

The system allows a Cardputer to act as a wireless remote desktop viewer and input device for a PC over encrypted TCP.

---

## Architecture

```
┌─────────────────────────┐         TCP/19847          ┌────────────────────────────┐
│   Cardputer (Client)    │◄──────────────────────────►│      PC (Server/Rust)      │
│   ESP32-S3 / ESP-IDF    │   ECDH + AES-128-GCM      │                            │
│                         │                            │  Screen Capture (scrap)    │
│  mDNS Discovery ────────┼──► DiscoveryRequest ──────►│  Input Sim (enigo)         │
│  ECDH Handshake ────────┼──► HandshakeInit ─────────►│  mDNS Service             │
│  JPEG Decode (HW) ◄─────┼──── ScreenFrame ◄─────────│  JPEG Compress (image)     │
│  Keyboard Input ────────┼──► KeyPress/MouseMove ────►│  Keyboard/Mouse mapping    │
│  Display (ST7789V2) ◄───┼──── Encrypted frames ◄────│  Frame delta detection     │
└─────────────────────────┘                            └────────────────────────────┘
```

---

## Protocol (v1)

### Packet Format
```
[1B version][1B type][2B length (BE)][payload][16B AES-GCM tag]
Header = 4 bytes, Tag = 16 bytes
Max payload = 65515 bytes
```

### Packet Types

| Code | Type | Direction | Description |
|------|------|-----------|-------------|
| 0x00 | DiscoveryRequest | C→S | Client discovers server (cookie validation) |
| 0x01 | DiscoveryResponse | S→C | Server responds with name + port |
| 0x02 | HandshakeInit | C→S | ECDH ephemeral pubkey + nonce + ECDSA signature |
| 0x03 | HandshakeResponse | S→C | Server ephemeral pubkey + nonce + ECDSA signature |
| 0x04 | HandshakeComplete | C→S | Encrypted transcript MAC (HMAC-SHA256) |
| 0x10 | SessionStart | S→C | Session established |
| 0x11 | SessionEnd | Both | Session termination |
| 0x12 | SessionTimeout | S→C | Idle timeout |
| 0x13 | Heartbeat | Both | Keep-alive ping |
| 0x14 | HeartbeatAck | Both | Keep-alive pong |
| 0x20 | ScreenFrame | S→C | Full JPEG frame (seq + timestamp + jpeg) |
| 0x21 | ScreenDelta | S→C | Delta frame (defined but unused) |
| 0x22 | ScreenRequest | C→S | Force screen refresh |
| 0x30 | MouseMove | C→S | Relative mouse dx/dy (i8, i8) |
| 0x31 | MouseClick | C→S | Button + action (JSON) |
| 0x32 | KeyPress | C→S | USB HID keycode + modifiers (JSON) |
| 0x33 | KeyRelease | C→S | USB HID keycode + modifiers (JSON) |
| 0x34 | KeyType | C→S | UTF-8 string to type |
| 0x40 | ModeSwitch | C→S | Switch Mouse/Keyboard mode |
| 0x41 | ModeAck | S→C | Confirm mode switch |
| 0xF0 | ErrorPacket | Both | Error code + message |

### Connection Flow
```
Client                              Server
  │                                    │
  ├─── DiscoveryRequest (cookie) ─────►│  (optional)
  │◄── DiscoveryResponse ─────────────┤
  │                                    │
  ├─── HandshakeInit ─────────────────►│  pubkey(33) + nonce(32) + sig(64) = 129B
  │    [ephem_pubkey||nonce signed     │  server verifies client ECDSA signature
  │     by client static key]          │
  │                                    │
  │◄── HandshakeResponse ─────────────┤  pubkey(33) + nonce(32) + sig(64) = 129B
  │    [ephem_pubkey||client_nonce||   │  client verifies server ECDSA signature
  │     server_nonce signed by         │
  │     server static key]             │
  │                                    │
  │  ── Both derive session keys ──    │  ECDH shared secret → HKDF-SHA256
  │    salt = SHA256(client_nonce ||   │  → c2s_key(16) + s2c_key(16) + hmac_key(32)
  │           server_nonce)            │
  │    info = "cardputer-remote-v1-    │
  │           session-keys"            │
  │                                    │
  ├─── HandshakeComplete (encrypted)──►│  HMAC-SHA256(transcript) encrypted w/ AES-GCM
  │    transcript = client_pub ||      │
  │    server_pub || client_nonce ||   │
  │    server_nonce                    │
  │                                    │
  │◄── SessionStart ──────────────────┤
  │                                    │
  │◄── ScreenFrame (encrypted) ───────┤  Continuous JPEG frames
  ├─── Input events (encrypted) ──────►│  Mouse/Keyboard commands
  │◄──► Heartbeat/HeartbeatAck ───────►│  Every 5 seconds
  │                                    │
```

---

## Cryptography

### Algorithms
| Purpose | Algorithm | Key Size |
|---------|-----------|----------|
| Key Exchange | ECDH (secp256r1/P-256) | 256-bit |
| Authentication | ECDSA (secp256r1) | 256-bit |
| Encryption | AES-128-GCM | 128-bit |
| Key Derivation | HKDF-SHA256 (RFC 5869) | 256-bit |
| Transcript MAC | HMAC-SHA256 | 256-bit |

### Nonce Structure (12 bytes)
```
[4B counter (BE)][8B random]
```
- Counter starts at 0, increments monotonically (replay protection)
- Random part derived from handshake nonce (first 8 bytes of peer_nonce)
- Counter overflow (u32::MAX) forces session renegotiation

### Key Derivation
```
shared_secret = ECDH(our_ephemeral, peer_ephemeral)
salt = SHA256(client_nonce || server_nonce)
info = "cardputer-remote-v1-session-keys"
OKM = HKDF-SHA256(salt, shared_secret, info, 64 bytes)
  → client_to_server_key = OKM[0..16]   (AES-128)
  → server_to_client_key = OKM[16..32]  (AES-128)
  → hmac_key             = OKM[32..64]  (HMAC-SHA256)
```

### Security Properties
- **Forward secrecy**: ephemeral ECDH keys per session
- **Mutual authentication**: both sides sign with static ECDSA keys
- **Replay protection**: monotonic nonce counter
- **Authenticated encryption**: AES-GCM (AEAD)
- **Constant-time comparison**: for crypto values (constant_time_eq)

---

## Server (Rust) - Detailed

### Module Structure
```
src/
├── main.rs           - CLI, logging, App event loop (tokio::select!)
├── lib.rs            - Module exports, VERSION constant
├── protocol/mod.rs   - Wire format: PacketType, PacketHeader, Packet, data structs
├── network/
│   ├── mod.rs        - DiscoveryService, Server (TCP listener), Connection (encrypted I/O)
│   └── session.rs    - Session lifecycle: frame capture, input dispatch, heartbeat
├── crypto/mod.rs     - CryptoContext: ECDH, ECDSA, AES-GCM, HKDF, nonce management
├── config/mod.rs     - TOML config: ServerConfig, SecurityConfig, DisplayConfig
├── input/mod.rs      - InputController: enigo-based mouse/keyboard simulation
├── capture/mod.rs    - ScreenCapturer: scrap-based capture, BGRA→RGB→JPEG pipeline
└── bin/keygen.rs     - Key generation utility
```

### Screen Capture Pipeline
1. `scrap::Capturer` captures primary display (BGRA format)
2. BGRA → RGB conversion (with optional region cropping)
3. Frame hash comparison (256-sample fast hash, skip unchanged frames)
4. Resize to target dimensions (240x135 default, matching Cardputer LCD)
5. JPEG compression at configurable quality (default 70%)
6. Encrypted with AES-128-GCM and sent as `ScreenFrame` packet

### Input Processing
- **Mouse mode**: Arrow keys → mouse movement (dx/dy * speed), Enter → left click
- **Keyboard mode**: USB HID keycodes mapped to enigo Keys (letters, numbers, F1-F12, navigation)
- **Modifiers**: Ctrl(0x01), Shift(0x02), Alt(0x04), GUI/Meta(0x08)
- **Mode toggle**: ModeSwitch packet toggles between Mouse/Keyboard modes

### Config (config.toml)
```toml
[server]
port = 19847
session_timeout_secs = 0  # 0 = never
max_fps = 10
jpeg_quality = 70

[security]
discovery_cookie = "<32 hex chars>"
private_key = "<64 hex chars>"
cardputer_public_key = "<66 hex chars>"

[network]
mdns_service_name = "cardputer-remote"
device_name = "PC"
bind_address = "0.0.0.0"

[display]
target_width = 240
target_height = 135
capture_region = [x, y, w, h]  # optional
```

### Dependencies
- **tokio** - async runtime (full features)
- **p256** - ECDH/ECDSA on secp256r1
- **aes-gcm** - AES-128-GCM AEAD
- **scrap** - cross-platform screen capture
- **enigo** - cross-platform input simulation
- **image** - JPEG encoding/resizing
- **mdns-sd** - mDNS service discovery
- **serde/toml** - config serialization

---

## Client (ESP32-S3 / ESP-IDF) - Detailed

### Module Structure
```
src/
├── main.c              - App entry, FreeRTOS tasks, event loop, state machine
├── remote_network.c    - WiFi STA, mDNS browse, TCP connect, packet I/O
├── remote_crypto.c     - ECDH (mbedtls), AES-GCM (mbedtls), ECDSA, HKDF
├── remote_protocol.c   - Packet framing: build/parse headers, serialize input events
├── remote_display.c    - LCD init (ST7789V2), JPEG decode (ESP32-S3 HW), DMA blit
└── remote_input.c      - 56-key matrix scan (4x14), keymap, modifier tracking

include/
├── remote_config.h     - Constants: display size, WiFi creds, keys, timeouts
├── remote_network.h    - Network state struct, function prototypes
├── remote_crypto.h     - Crypto context struct, function prototypes
├── remote_protocol.h   - Packet structs, PacketType enum
├── remote_display.h    - Display init/draw prototypes
└── remote_input.h      - Input event structs, keymap definitions
```

### Display Pipeline (remote_display.c)
1. SPI bus init for ST7789V2 LCD (240x135, 1.14")
2. Incoming `ScreenFrame` → extract JPEG data
3. **Hardware JPEG decode** via ESP32-S3 JPEG peripheral (not software)
4. DMA transfer decoded pixels to LCD framebuffer
5. Direct SPI push to ST7789V2 controller
6. Status bar overlay: connection indicator, mode indicator, FPS counter

### Keyboard Input (remote_input.c)
- 56-key matrix (4 rows x 14 columns)
- GPIO matrix scan with debounce
- USB HID keycode generation
- Modifier tracking (Fn, Shift, Ctrl combinations)
- Special combos: Fn+Arrow for mouse mode, Fn+Enter for left click
- Key repeat with configurable delay

### Network (remote_network.c)
- WiFi STA mode connection
- mDNS browse for `_cardputerremote._tcp`
- TCP socket connection to discovered server
- Non-blocking packet I/O with FreeRTOS event groups
- Reconnection logic with backoff

### Crypto (remote_crypto.c)
- Uses **mbedtls** (ESP-IDF built-in) for all crypto
- ECDH on secp256r1 via `mbedtls_ecdh_*`
- AES-128-GCM via `mbedtls_gcm_*`
- ECDSA signing/verification via `mbedtls_ecdsa_*`
- HKDF-SHA256 via `mbedtls_hkdf`
- Keys stored in NVS (Non-Volatile Storage) partition

### FreeRTOS Architecture
```
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│   Network Task   │  │   Display Task   │  │   Input Task     │
│   (TCP recv)     │  │   (JPEG decode   │  │   (Key matrix    │
│   Priority: 5    │  │    + LCD update)  │  │    scan)         │
│                  │  │   Priority: 4    │  │   Priority: 3    │
└────────┬─────────┘  └────────▲─────────┘  └────────┬─────────┘
         │                     │                      │
         ▼                     │                      ▼
    ┌─────────────────────────────────────────────────────┐
    │              Event Groups / Queues                   │
    │   frame_queue, input_queue, state_event_group        │
    └─────────────────────────────────────────────────────┘
```

---

## Hardware Context: M5Stack Cardputer ADV

### Specs (vs Original Cardputer)

| Feature | Original Cardputer | Cardputer ADV |
|---------|-------------------|---------------|
| Core Module | M5StampS3 (ESP32-S3FN8) | StampS3A (ESP32-S3FN8) |
| CPU | Xtensa dual-core LX7 @240MHz | Xtensa dual-core LX7 @240MHz |
| Memory | 512KB SRAM, 8MB Flash, 8MB PSRAM | 512KB SRAM, 8MB Flash, 8MB PSRAM |
| Display | 1.14" IPS LCD, 240x135, ST7789V2 | 1.14" IPS LCD, 240x135, ST7789V2 |
| Keyboard | 56-key (4x14), 260gf | 56-key (4x14), **160gf** |
| Audio | SPM1423 mic + NS4150B amp | **ES8311 codec** + mic + amp + **3.5mm jack** |
| IMU | None | **BMI270 6-axis** |
| Battery | 120mAh + 1400mAh base | **1750mAh integrated** |
| WiFi | 2.4GHz | 2.4GHz (**improved antenna**) |
| BLE | 5.0 | 5.0 |
| IR | Yes | Yes |
| SD Card | microSD | microSD |
| Expansion | HY2.0-4P Grove | HY2.0-4P Grove + **14-pin EXT header** |
| GPS Pins | RX=1, TX=-1 | **RX=15, TX=13** |
| Size | 84x54x16mm | 84x54x**19.6mm** |
| Weight | ~65g | **81g** |
| Price | $29.90 (EOL) | $29.99 |

### Board Detection in Firmware
```cpp
if (M5.getBoard() == m5::board_t::board_M5CardputerADV) {
    gpsRxPin = 15;   // ADV GPS pins
    gpsTxPin = 13;
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);
} else if (M5.getBoard() == m5::board_t::board_M5Cardputer) {
    gpsRxPin = 1;    // Original Cardputer
    gpsTxPin = -1;
}
```

### Display API (M5Unified library)
```cpp
M5.begin();
M5.Lcd.setRotation(1);         // Landscape mode
M5.Display.setTextSize(1.5);
M5.Display.setTextColor(color);
M5.Display.setTextFont(1);
M5.Display.fillRect(x, y, w, h, color);
M5.Display.drawJpgFile(SD, path);
M5.Display.clear();
M5.Display.display();          // Flush framebuffer

// Canvas (offscreen sprite for flicker-free drawing)
M5Canvas canvas(&M5.Display);
canvas.createSprite(width, height);
canvas.fillRect(...);
canvas.pushSprite(x, y);

// Keyboard
M5Cardputer.update();
M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER);
```

### Key Libraries
- **M5Unified** - unified M5Stack device abstraction
- **M5Cardputer.h** - Cardputer-specific keyboard/peripheral support
- **M5GFX** / **LovyanGFX** - display driver (ST7789V2, SPI)
- **ESP-IDF** - FreeRTOS, WiFi, BLE, mbedtls, JPEG HW decoder

---

## Key Observations

### Display Compatibility
Both the original Cardputer and Cardputer ADV use the **same display** (1.14" IPS, 240x135, ST7789V2). The cardputer-remote server already targets 240x135 by default. No display-related code changes needed for ADV compatibility.

### Input Compatibility
Both devices use the **same 56-key 4x14 matrix**. The ADV has lighter key actuation (160gf vs 260gf) but the electrical interface is identical. The cardputer-remote-client input code works on both.

### New ADV Hardware Not Yet Utilized
- **BMI270 IMU** - could enable gyroscope-based mouse control
- **ES8311 Audio codec** - could enable audio streaming
- **3.5mm audio jack** - audio output capability
- **14-pin EXT header** - additional peripheral expansion
- **GPS (pins 15/13)** - location-aware features

### ESP-IDF vs Arduino
- The **cardputer-remote-client** uses pure ESP-IDF (C, FreeRTOS, mbedtls)
- The **Evil-Cardputer firmware** uses Arduino framework (M5Unified, M5Cardputer.h)
- These are different build systems and cannot share code directly without adaptation
- The remote client's ESP-IDF approach gives lower-level control and better performance for real-time JPEG decoding + DMA display

### Crypto Implementation Parity
Both client (mbedtls) and server (p256/aes-gcm crates) implement the same protocol:
- secp256r1 ECDH key exchange
- ECDSA mutual authentication
- AES-128-GCM with counter+random nonces
- HKDF-SHA256 key derivation
- HMAC-SHA256 transcript verification
