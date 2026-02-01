# Cardputer Remote Client

Secure remote desktop client for M5Stack Cardputer (ESP32-S3).

## Features

- ECDH key exchange (secp256r1)
- AES-128-GCM encryption
- Hardware JPEG decoder (ESP32-S3)
- DMA-based LCD rendering
- mDNS service discovery

## Requirements

- **ESP-IDF v5.0+** (recommended: v5.1 or v5.2)
- M5Stack Cardputer or Cardputer-Adv
- USB-C cable

## Build Instructions

### Option 1: ESP-IDF (Recommended)

#### 1. Install ESP-IDF

```bash
# Linux/macOS
mkdir -p ~/esp
cd ~/esp
git clone -b v5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
source export.sh

# Windows - use ESP-IDF Tools Installer from:
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/windows-setup.html
```

#### 2. Build the Project

```bash
cd cardputer-remote-client

# Set target to ESP32-S3
idf.py set-target esp32s3

# Build
idf.py build
```

#### 3. Flash to Cardputer

**Enter Download Mode:**
1. Set the switch on top of Cardputer to **OFF**
2. Hold down the **G0** button
3. Connect USB-C cable to computer
4. Release G0 button

```bash
# Flash (replace /dev/ttyACM0 with your port)
idf.py -p /dev/ttyACM0 flash

# Monitor serial output
idf.py -p /dev/ttyACM0 monitor

# Or both at once
idf.py -p /dev/ttyACM0 flash monitor
```

**Windows ports:** COM3, COM4, etc.
**Linux ports:** /dev/ttyACM0, /dev/ttyUSB0
**macOS ports:** /dev/cu.usbmodem*

### Option 2: Arduino IDE

#### 1. Install Arduino IDE

Download from: https://www.arduino.cc/en/software

#### 2. Add ESP32 Board Support

1. Open Arduino IDE
2. Go to **File > Preferences**
3. Add to "Additional Board Manager URLs":
   ```
   https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json
   ```
4. Go to **Tools > Board > Boards Manager**
5. Search for "M5Stack" and install

#### 3. Install Required Libraries

Via Library Manager (**Tools > Manage Libraries**):
- M5Cardputer
- M5Unified
- M5GFX

#### 4. Board Configuration

- **Board:** M5Stack Arduino > STAMP-S3
- **USB CDC On Boot:** Enabled
- **Flash Size:** 8MB
- **Partition Scheme:** No OTA (Large APP) or Huge APP

#### 5. Convert Project to Arduino

The current project is ESP-IDF based. For Arduino, you need to:
1. Rename `src/main.c` to `cardputer_remote.ino`
2. Move all `.c` files to the sketch folder
3. Rename `.c` to `.cpp` if needed
4. Add Arduino-specific includes

### Option 3: PlatformIO

#### platformio.ini

```ini
[env:m5stack-cardputer]
platform = espressif32@6.7.0
board = esp32-s3-devkitc-1
framework = espidf
upload_speed = 1500000

build_flags =
    -DCONFIG_IDF_TARGET_ESP32S3=1

monitor_speed = 115200
```

```bash
# Build
pio run

# Upload
pio run -t upload

# Monitor
pio device monitor
```

## Troubleshooting

### "Failed to connect" error
- Ensure Cardputer is in download mode (switch OFF + G0 button)
- Try different USB cable (data cable, not charge-only)
- Check if port is correct

### Build errors about missing components
```bash
# Update ESP-IDF components
idf.py reconfigure
```

### Out of memory during build
- Close other applications
- Use `idf.py build -j1` for single-threaded build

## Project Structure

```
cardputer-remote-client/
├── CMakeLists.txt          # ESP-IDF project file
├── sdkconfig.defaults      # Default configuration
├── partitions.csv          # Partition table
├── main/
│   ├── CMakeLists.txt      # Main component
│   └── idf_component.yml   # Component dependencies
├── src/
│   ├── main.c              # Entry point
│   ├── remote_crypto.c     # ECDH/AES-GCM
│   ├── remote_protocol.c   # Packet handling
│   ├── remote_network.c    # WiFi/TCP/mDNS
│   ├── remote_display.c    # LCD/JPEG
│   └── remote_input.c      # Keyboard
└── include/
    └── *.h                 # Headers
```

## Usage

After flashing:
1. Set switch back to **ON**
2. Press reset button or reconnect USB
3. Cardputer will search for server via mDNS
4. Connect to same WiFi network as server

## Server

The companion server (Rust) is in `../cardputer-remote/`

```bash
cd ../cardputer-remote
cargo run --release
```
