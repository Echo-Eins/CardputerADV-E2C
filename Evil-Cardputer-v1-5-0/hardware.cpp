/*
 * Hardware Abstraction Layer Implementation
 *
 * Implements hardware control for Evil-Cardputer
 */

#include "hardware.h"
#include <AudioOutput.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <stdarg.h>
#include "gui/gui.h"
#include "display_runtime.h"

using LB = GUI::LegacyBridge;

// ============================================================================
// Board Detection - Static Storage
// ============================================================================

static BoardType _detectedBoard = BoardType::UNKNOWN;

// ============================================================================
// AudioOutputM5Speaker Class (for MP3 playback)
// ============================================================================

class AudioOutputM5Speaker : public AudioOutput {
public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t virtual_sound_channel = 0) {
        _m5sound = m5sound;
        _virtual_ch = virtual_sound_channel;
    }

    virtual ~AudioOutputM5Speaker(void) {}

    virtual bool begin(void) override {
        return true;
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (_tri_buffer_index < tri_buf_size) {
            _tri_buffer[_tri_index][_tri_buffer_index] = sample[0];
            _tri_buffer[_tri_index][_tri_buffer_index + 1] = sample[1];
            _tri_buffer_index += 2;
            return true;
        }
        flush();
        return false;
    }

    virtual void flush(void) override {
        if (_tri_buffer_index) {
            _m5sound->playRaw(_tri_buffer[_tri_index], _tri_buffer_index, hertz, true, 1, _virtual_ch);
            _tri_index = _tri_index < 2 ? _tri_index + 1 : 0;
            _tri_buffer_index = 0;
        }
    }

    virtual bool stop(void) override {
        flush();
        _m5sound->stop(_virtual_ch);
        return true;
    }

protected:
    m5::Speaker_Class* _m5sound;
    uint8_t _virtual_ch;
    static constexpr size_t tri_buf_size = 128;
    int16_t _tri_buffer[3][tri_buf_size];
    size_t _tri_buffer_index = 0;
    size_t _tri_index = 0;
};

// ============================================================================
// Audio - Global Storage (for legacy compatibility)
// ============================================================================

static AudioFileSourceSD file;  // Legacy name
static AudioOutputM5Speaker out(&M5.Speaker);  // Legacy name
AudioGeneratorMP3 mp3;  // Global for legacy direct access
static AudioFileSourceID3* id3 = nullptr;  // Legacy name

// ============================================================================
// Board Detection Implementation
// ============================================================================

void hwDetectBoard() {
    if (M5.getBoard() == m5::board_t::board_M5CardputerADV) {
        _detectedBoard = BoardType::CARDPUTER_ADV;
        Serial.println("Detected: Cardputer-ADV");
    } else if (M5.getBoard() == m5::board_t::board_M5Cardputer) {
        _detectedBoard = BoardType::CARDPUTER;
        Serial.println("Detected: Cardputer");
    } else {
        _detectedBoard = BoardType::UNKNOWN;
        Serial.println("Unknown board type");
    }
}

BoardType hwGetBoardType() {
    return _detectedBoard;
}

const char* hwGetBoardName() {
    switch (_detectedBoard) {
        case BoardType::CARDPUTER:     return "Cardputer";
        case BoardType::CARDPUTER_ADV: return "Cardputer-ADV";
        default:                       return "Unknown";
    }
}

bool hwIsCardputerADV() {
    return _detectedBoard == BoardType::CARDPUTER_ADV;
}

// ============================================================================
// Display Implementation
// ============================================================================

DisplayConfig HardwareDisplay::_config = {
    .backend = DisplayBackend::M5_UNIFIED,
    .width = 240,
    .height = 135,
    .rotation = 1,
    .doubleBuffer = false
};
bool HardwareDisplay::_initialized = false;

void HardwareDisplay::init() {
    if (_initialized) return;

    _config.width = GUI::runtimeDisplayWidth() > 0 ? GUI::runtimeDisplayWidth() : LB::width();
    _config.height = GUI::runtimeDisplayHeight() > 0 ? GUI::runtimeDisplayHeight() : LB::height();
    _config.rotation = LB::getRotation();
    _initialized = true;
}

DisplayConfig HardwareDisplay::getConfig() {
    const DisplayProfile* profile = DisplayRuntime::getAppliedProfile();
    if (profile) {
        switch (profile->driver) {
            case DisplayDriver::TFT_ESPI_ILI9488:
                _config.backend = DisplayBackend::TFT_ESPI_ILI9488;
                break;
            case DisplayDriver::LGFX_ILI9488:
                _config.backend = DisplayBackend::LGFX_ILI9488;
                break;
            case DisplayDriver::M5_BUILTIN:
            default:
                _config.backend = DisplayBackend::M5_UNIFIED;
                break;
        }
    } else {
        _config.backend = DisplayBackend::M5_UNIFIED;
    }
    _config.width = GUI::runtimeDisplayWidth() > 0 ? GUI::runtimeDisplayWidth() : LB::width();
    _config.height = GUI::runtimeDisplayHeight() > 0 ? GUI::runtimeDisplayHeight() : LB::height();
    _config.rotation = LB::getRotation();
    return _config;
}

// ---------------------------------------------------------------------------
// HardwareDisplay drawing methods — delegated to GUI::LegacyBridge
// ---------------------------------------------------------------------------

void HardwareDisplay::clear()                              { LB::clear(); }
void HardwareDisplay::fillScreen(uint16_t c)               { LB::fillScreen(c); }
void HardwareDisplay::display()                            { LB::display(); }
void HardwareDisplay::setCursor(int16_t x, int16_t y)     { LB::setCursor(x, y); }
void HardwareDisplay::setTextColor(uint16_t c)             { LB::setTextColor(c); }
void HardwareDisplay::setTextColor(uint16_t fg, uint16_t bg) { LB::setTextColor(fg, bg); }
void HardwareDisplay::setTextSize(float s)                 { LB::setTextSize(s); }
void HardwareDisplay::setTextFont(uint8_t f)               { LB::setTextFont(f); }
void HardwareDisplay::print(const char* t)                 { LB::print(t); }
void HardwareDisplay::print(const String& t)               { LB::print(t); }
void HardwareDisplay::println(const char* t)               { LB::println(t); }
void HardwareDisplay::println(const String& t)             { LB::println(t); }

void HardwareDisplay::printf(const char* format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    LB::print(buf);
}

int  HardwareDisplay::textWidth(const char* t)             { return LB::textWidth(t); }
int  HardwareDisplay::textWidth(const String& t)           { return LB::textWidth(t); }

void HardwareDisplay::drawPixel(int16_t x, int16_t y, uint16_t c) { LB::drawPixel(x, y, c); }
void HardwareDisplay::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) { LB::drawLine(x0, y0, x1, y1, c); }
void HardwareDisplay::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { LB::drawRect(x, y, w, h, c); }
void HardwareDisplay::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { LB::fillRect(x, y, w, h, c); }
void HardwareDisplay::drawCircle(int16_t x, int16_t y, int16_t r, uint16_t c) { LB::drawCircle(x, y, r, c); }
void HardwareDisplay::fillCircle(int16_t x, int16_t y, int16_t r, uint16_t c) { LB::fillCircle(x, y, r, c); }

void HardwareDisplay::drawJpgFile(fs::FS& fs, const char* path, int16_t x, int16_t y) {
    // Delegate to bridge-level file decode to avoid transient heap buffers.
    LB::drawJpgFile(fs, path, x, y);
}

void HardwareDisplay::drawImage(const char* filepath) {
    LB::drawJpgFile(SD, filepath, 0, 0);
}

uint8_t HardwareDisplay::getBrightness()                   { return LB::getBrightness(); }
void    HardwareDisplay::setBrightness(uint8_t b)          { LB::setBrightness(b); }
int16_t HardwareDisplay::width()                           { return LB::width(); }
int16_t HardwareDisplay::height()                          { return LB::height(); }

void HardwareDisplay::setRotation(uint8_t rotation) {
    LB::setRotation(rotation);
    _config.rotation = rotation;
}

uint8_t HardwareDisplay::getRotation()                     { return LB::getRotation(); }

// ============================================================================
// LED Implementation
// ============================================================================

// Global pixels object for legacy compatibility
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(HW_LED_COUNT, HW_LED_PIN, NEO_GRB);

bool HardwareLED::_enabled = true;
bool HardwareLED::_initialized = false;

void HardwareLED::init() {
    if (_initialized) return;
    pixels.begin();
    _initialized = true;
}

void HardwareLED::setColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!_enabled) return;
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

void HardwareLED::setColor(uint32_t color) {
    if (!_enabled) return;
    pixels.setPixelColor(0, color);
    pixels.show();
}

void HardwareLED::setColorRange(int startPixel, int endPixel, uint32_t color) {
    if (!_enabled) return;
    for (int i = startPixel; i <= endPixel; i++) {
        pixels.setPixelColor(i, color);
    }
    pixels.show();
    delay(30);
}

void HardwareLED::off() {
    pixels.setPixelColor(0, 0);
    pixels.show();
}

void HardwareLED::blink(uint8_t r, uint8_t g, uint8_t b, int delayMs) {
    if (!_enabled) return;
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
    delay(delayMs);
    pixels.setPixelColor(0, 0);
    pixels.show();
}

bool HardwareLED::isEnabled() {
    return _enabled;
}

void HardwareLED::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        off();
    }
}

void HardwareLED::show() {
    pixels.show();
}

Adafruit_NeoPixel& HardwareLED::getPixels() {
    return pixels;
}

// ============================================================================
// Audio Implementation
// ============================================================================

bool HardwareAudio::_enabled = true;
bool HardwareAudio::_initialized = false;

void HardwareAudio::init() {
    if (_initialized) return;
    _initialized = true;
}

void HardwareAudio::play(const char* filepath) {
    if (!_enabled) return;

    // Stop any current playback
    stop();

    // Open file and create ID3 wrapper
    file.open(filepath);
    id3 = new AudioFileSourceID3(&file);

    // Start playback
    mp3.begin(id3, &out);
}

void HardwareAudio::stop() {
    if (id3 == nullptr) return;

    out.stop();
    mp3.stop();
    id3->close();
    file.close();
    delete id3;
    id3 = nullptr;
}

bool HardwareAudio::isPlaying() {
    return mp3.isRunning();
}

bool HardwareAudio::loop() {
    if (!mp3.isRunning()) return false;
    return mp3.loop();
}

uint8_t HardwareAudio::getVolume() {
    return M5Cardputer.Speaker.getVolume();
}

void HardwareAudio::setVolume(uint8_t volume) {
    M5Cardputer.Speaker.setVolume(volume);
}

bool HardwareAudio::isEnabled() {
    return _enabled;
}

void HardwareAudio::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        stop();
    }
}

// ============================================================================
// Power/Battery Implementation
// ============================================================================

int HardwarePower::getBatteryLevel() {
    // Use M5Unified power API only (ADC oneshot path) to avoid
    // ESP-IDF v5 conflict with deprecated ADC legacy driver.
    int percent = M5.Power.getBatteryLevel();
    if (percent >= 0 && percent <= 100) {
        return percent;
    }

    // Fallback: estimate from battery voltage if percentage is unavailable.
    const int mv = M5.Power.getBatteryVoltage();
    if (mv > 0) {
        percent = static_cast<int>((mv - 3350) * 100.0f / (4150.0f - 3350.0f));
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        return percent;
    }

    return -1;
}

String HardwarePower::getBatteryLevelString() {
    int level = getBatteryLevel();
    if (level < 0) {
        return String("error");
    }
    return String(level);
}

int HardwarePower::getBatteryCurrent() {
    return M5.Power.getBatteryCurrent();
}

float HardwarePower::getTemperature() {
    float temp;
    M5.Imu.getTemp(&temp);
    return temp;
}

String HardwarePower::getTemperatureString() {
    return String((int)round(getTemperature()));
}

int HardwarePower::getFreeHeapKB() {
    return esp_get_free_heap_size() / 1024;
}

String HardwarePower::getFreeHeapString() {
    return String(getFreeHeapKB());
}

float HardwarePower::getStackWatermarkKB() {
    UBaseType_t stackWords = uxTaskGetStackHighWaterMark(NULL);
    return stackWords * 4 / 1024.0f;
}

String HardwarePower::getStackWatermarkString() {
    return String(getStackWatermarkKB());
}

// ============================================================================
// GPS Implementation
// ============================================================================

int HardwareGPS::_rxPin = -1;
int HardwareGPS::_txPin = -1;
int HardwareGPS::_pinsMode = -1;
bool HardwareGPS::_initialized = false;

static HardwareSerial _gpsSerial(2);

void HardwareGPS::init(int baudrate) {
    if (_initialized) return;

    // Determine pins based on board type and mode
    if (_pinsMode == -1) {
        // Auto mode - detect from board
        if (hwGetBoardType() == BoardType::CARDPUTER_ADV) {
            _rxPin = 15;
            _txPin = 13;
            // Enable ADV GPS power
            pinMode(5, OUTPUT);
            digitalWrite(5, HIGH);
        } else if (hwGetBoardType() == BoardType::CARDPUTER) {
            _rxPin = 1;
            _txPin = -1;
        } else {
            _rxPin = -1;
            _txPin = -1;
        }
    } else if (_pinsMode == 0) {
        _rxPin = 1;
        _txPin = 2;
    } else if (_pinsMode == 1) {
        _rxPin = 15;
        _txPin = 13;
    }

    _gpsSerial.begin(baudrate, SERIAL_8N1, _rxPin, _txPin);
    _initialized = true;
}

int HardwareGPS::getRxPin() {
    return _rxPin;
}

int HardwareGPS::getTxPin() {
    return _txPin;
}

void HardwareGPS::setPinsMode(int mode) {
    _pinsMode = mode;
}

int HardwareGPS::getPinsMode() {
    return _pinsMode;
}

HardwareSerial& HardwareGPS::serial() {
    return _gpsSerial;
}

// ============================================================================
// Hardware Module Initialization
// ============================================================================

void hwInit() {
    // Initialize all subsystems
    HardwareDisplay::init();
    HardwareLED::init();
    HardwareAudio::init();
    // GPS is initialized separately with baudrate
}

// ============================================================================
// Legacy Compatibility - Global Variables
// ============================================================================

bool ledOn = true;
bool soundOn = true;

// ============================================================================
// Legacy Compatibility - Functions
// ============================================================================

void setColorRange(int startPixel, int endPixel, uint32_t color) {
    HardwareLED::setColorRange(startPixel, endPixel, color);
}

void play(const char* fname) {
    if (!soundOn) return;
    HardwareAudio::play(fname);
}

void stop(void) {
    HardwareAudio::stop();
}

String getBatteryLevel() {
    return HardwarePower::getBatteryLevelString();
}

String getTemperature() {
    return HardwarePower::getTemperatureString();
}

String getStack() {
    return HardwarePower::getStackWatermarkString();
}

String getRamUsage() {
    return HardwarePower::getFreeHeapString();
}

void drawImage(const char* filepath) {
    HardwareDisplay::drawImage(filepath);
}
