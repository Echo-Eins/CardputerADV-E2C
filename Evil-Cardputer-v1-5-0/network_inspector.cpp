/*
 * network_inspector.cpp
 *
 * Architecture:
 *   WiFi task callback -> bounded SPSC PSRAM ring
 *   UI task           -> parser/history/stats
 *   PCAP writer       -> 64 KB batches guarded by SD/display arbitration
 *
 * The callback performs no allocation, parsing, logging, rendering, or I/O.
 */

#include "network_inspector.h"

#include "display_runtime.h"
#include "flash_pager.h"
#include "gui/gui.h"
#include "input_compat.h"
#include "netcore.h"
#include "runtime_memory.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <stdarg.h>
#include <time.h>

extern bool inMenu;

namespace NetworkInspector {
namespace {

using LB = GUI::LegacyBridge;

constexpr const char* CAPTURE_OWNER = "network-inspector";
constexpr const char* CAPTURE_DIRECTORY = "/evil/captures";
constexpr size_t SNAPLEN = 512;
constexpr size_t CAPTURE_CAPACITY_PSRAM = 256;
constexpr size_t HISTORY_CAPACITY_PSRAM = 192;
constexpr size_t PCAP_BATCH_PSRAM = 64U * 1024U;
constexpr size_t PCAP_BATCH_FALLBACK = 4U * 1024U;
constexpr size_t PCAP_BATCH_MINIMUM = 2U * 1024U;
constexpr size_t INSPECTOR_MEMORY_RESERVE = 36U * 1024U;
constexpr uint32_t PCAP_FLUSH_INTERVAL_MS = 1000;
constexpr uint32_t UI_REFRESH_MS = 125;
constexpr size_t PROCESS_BUDGET = 48;
constexpr size_t RAW_PREVIEW_SIZE = 48;

enum class ViewMode : uint8_t { LIVE = 0, DETAILS, STATS, RADIO };

enum class DisplayFilter : uint8_t {
    ALL = 0,
    MANAGEMENT,
    DATA,
    TCP,
    UDP,
    DNS,
    ARP,
    ICMP,
    PROTECTED,
    COUNT,
};

struct CaptureSlot {
    uint64_t monotonicUs;
    uint16_t capturedLength;
    uint16_t originalLength;
    int8_t rssi;
    uint8_t channel;
    uint8_t antenna;
    uint8_t rate;
    uint8_t mcs;
    uint8_t signalMode;
    uint8_t rxState;
    uint8_t packetType;
    uint8_t data[SNAPLEN];
};

struct PacketRecord {
    uint32_t number;
    uint64_t monotonicUs;
    uint16_t capturedLength;
    uint16_t originalLength;
    uint16_t frameControl;
    uint16_t wifiSequence;
    uint16_t etherType;
    uint16_t sourcePort;
    uint16_t destinationPort;
    uint16_t tcpWindow;
    uint16_t dnsType;
    uint32_t tcpSequence;
    uint32_t tcpAcknowledgment;
    int8_t rssi;
    uint8_t channel;
    uint8_t antenna;
    uint8_t rate;
    uint8_t mcs;
    uint8_t signalMode;
    uint8_t rxState;
    uint8_t wifiType;
    uint8_t wifiSubtype;
    uint8_t ipVersion;
    uint8_t ipProtocol;
    uint8_t ttl;
    uint8_t tcpFlags;
    uint8_t icmpType;
    uint8_t icmpCode;
    bool protectedFrame;
    bool retry;
    bool moreFragments;
    bool toDs;
    bool fromDs;
    char protocol[12];
    char source[40];
    char destination[40];
    char bssid[20];
    char info[96];
    char dnsName[64];
    char ssid[33];
    uint8_t rawPreviewLength;
    uint8_t rawPreview[RAW_PREVIEW_SIZE];
};

struct InspectorStats {
    uint64_t totalPackets;
    uint64_t totalBytes;
    uint64_t managementFrames;
    uint64_t dataFrames;
    uint64_t controlFrames;
    uint64_t protectedFrames;
    uint64_t retryFrames;
    uint64_t tcpPackets;
    uint64_t udpPackets;
    uint64_t dnsPackets;
    uint64_t arpPackets;
    uint64_t icmpPackets;
    int64_t rssiTotal;
    uint32_t rssiSamples;
    uint32_t rssiStrong;
    uint32_t rssiGood;
    uint32_t rssiWeak;
    uint32_t rssiPoor;
    uint32_t channelPackets[15];
    uint32_t packetsPerSecond;
    uint32_t ppsAccumulator;
    uint32_t ppsUpdatedAtMs;
};

struct __attribute__((packed)) PcapGlobalHeader {
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    int32_t timezone;
    uint32_t timestampAccuracy;
    uint32_t snaplen;
    uint32_t network;
};

struct __attribute__((packed)) PcapRecordHeader {
    uint32_t seconds;
    uint32_t microseconds;
    uint32_t includedLength;
    uint32_t originalLength;
};

struct __attribute__((packed)) RadiotapHeader {
    uint8_t version;
    uint8_t pad;
    uint16_t length;
    uint32_t present;
    uint8_t flags;
    uint8_t alignmentPad;
    uint16_t channelFrequency;
    uint16_t channelFlags;
    int8_t signalDbm;
    uint8_t antenna;
};

CaptureSlot* g_captureSlots = nullptr;
size_t g_captureCapacity = 0;
volatile uint32_t g_captureWrite = 0;
volatile uint32_t g_captureRead = 0;
volatile uint32_t g_captureDropped = 0;

PacketRecord* g_history = nullptr;
size_t g_historyCapacity = 0;
size_t g_historyWrite = 0;
size_t g_historyCount = 0;
uint32_t g_nextPacketNumber = 1;

InspectorStats* g_statsStorage = nullptr;
#define g_stats (*g_statsStorage)
ViewMode g_view = ViewMode::LIVE;
DisplayFilter g_filter = DisplayFilter::ALL;
bool g_followLatest = true;
int g_selectedVisible = -1;
int g_detailScroll = 0;
uint8_t g_currentChannel = 0;
bool g_screenDirty = true;
String g_notice;
uint32_t g_noticeUntilMs = 0;

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

uint16_t readBe16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) |
           static_cast<uint16_t>(data[1]);
}

uint32_t readBe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void writeText(char* destination, size_t capacity, const char* format, ...) {
    if (!destination || capacity == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(destination, capacity, format, args);
    va_end(args);
    destination[capacity - 1] = '\0';
}

void setProtocol(PacketRecord& record, const char* protocol) {
    writeText(record.protocol, sizeof(record.protocol), "%s",
              protocol ? protocol : "?");
}

void formatMac(const uint8_t* mac, char* output, size_t capacity) {
    if (!mac) {
        writeText(output, capacity, "-");
        return;
    }
    writeText(output, capacity, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
              mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void formatIpv4(const uint8_t* address, char* output, size_t capacity) {
    writeText(output, capacity, "%u.%u.%u.%u", address[0], address[1],
              address[2], address[3]);
}

void formatIpv6(const uint8_t* address, char* output, size_t capacity) {
    writeText(output, capacity,
              "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
              "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
              address[0], address[1], address[2], address[3], address[4],
              address[5], address[6], address[7], address[8], address[9],
              address[10], address[11], address[12], address[13], address[14],
              address[15]);
}

const char* managementSubtypeName(uint8_t subtype) {
    switch (subtype) {
        case 0: return "Assoc Request";
        case 1: return "Assoc Response";
        case 2: return "Reassoc Request";
        case 3: return "Reassoc Response";
        case 4: return "Probe Request";
        case 5: return "Probe Response";
        case 8: return "Beacon";
        case 9: return "ATIM";
        case 10: return "Disassociation";
        case 11: return "Authentication";
        case 12: return "Deauthentication";
        case 13: return "Action";
        default: return "Management";
    }
}

const char* controlSubtypeName(uint8_t subtype) {
    switch (subtype) {
        case 7: return "Control Wrapper";
        case 8: return "Block Ack Request";
        case 9: return "Block Ack";
        case 10: return "PS-Poll";
        case 11: return "RTS";
        case 12: return "CTS";
        case 13: return "ACK";
        case 14: return "CF-End";
        case 15: return "CF-End+ACK";
        default: return "Control";
    }
}

const char* dnsTypeName(uint16_t type) {
    switch (type) {
        case 1: return "A";
        case 2: return "NS";
        case 5: return "CNAME";
        case 12: return "PTR";
        case 15: return "MX";
        case 16: return "TXT";
        case 28: return "AAAA";
        case 33: return "SRV";
        case 255: return "ANY";
        default: return "TYPE";
    }
}

const char* dhcpTypeName(uint8_t type) {
    switch (type) {
        case 1: return "Discover";
        case 2: return "Offer";
        case 3: return "Request";
        case 4: return "Decline";
        case 5: return "ACK";
        case 6: return "NAK";
        case 7: return "Release";
        case 8: return "Inform";
        default: return "Message";
    }
}

void formatTcpFlags(uint8_t flags, char* output, size_t capacity) {
    size_t used = 0;
    const struct {
        uint8_t mask;
        char letter;
    } names[] = {{0x02, 'S'}, {0x10, 'A'}, {0x01, 'F'}, {0x04, 'R'},
                 {0x08, 'P'}, {0x20, 'U'}, {0x40, 'E'}, {0x80, 'C'}};
    for (const auto& name : names) {
        if ((flags & name.mask) != 0 && used + 1 < capacity) {
            output[used++] = name.letter;
        }
    }
    if (used == 0 && capacity > 1) output[used++] = '-';
    if (capacity > 0) output[used < capacity ? used : capacity - 1] = '\0';
}

uint16_t channelFrequency(uint8_t channel) {
    if (channel == 14) return 2484;
    if (channel >= 1 && channel <= 13) {
        return static_cast<uint16_t>(2407 + channel * 5);
    }
    return 0;
}

class PcapWriter {
public:
    bool start(uint8_t channel) {
        if (_active) return true;
        _lastError = "";
        _path = "";
        _used = 0;
        _packets = 0;
        _bytes = 0;

        if (!_batch && !allocateBatch()) {
            _lastError = "No memory for PCAP batch";
            return false;
        }

        DisplayRuntime::ScopedSdDisplayRelease sdGuard;
        if (!SD.exists("/evil") && !SD.mkdir("/evil")) {
            _lastError = "Cannot create /evil";
            return false;
        }
        if (!SD.exists(CAPTURE_DIRECTORY) &&
            !SD.mkdir(CAPTURE_DIRECTORY)) {
            _lastError = "Cannot create capture directory";
            return false;
        }

        _path = makeUniquePath(channel);
        _file = SD.open(_path.c_str(), FILE_WRITE);
        if (!_file) {
            _lastError = "Cannot open PCAP file";
            _path = "";
            return false;
        }

        PcapGlobalHeader header = {};
        header.magic = 0xa1b2c3d4;
        header.major = 2;
        header.minor = 4;
        header.timezone = 0;
        header.timestampAccuracy = 0;
        header.snaplen = static_cast<uint32_t>(SNAPLEN +
                                                sizeof(RadiotapHeader));
        header.network = 127;  // LINKTYPE_IEEE802_11_RADIOTAP

        const size_t written =
            _file.write(reinterpret_cast<const uint8_t*>(&header),
                        sizeof(header));
        _file.flush();
        if (written != sizeof(header)) {
            _file.close();
            SD.remove(_path.c_str());
            _lastError = "Cannot write PCAP header";
            _path = "";
            return false;
        }

        _bytes = sizeof(header);
        _lastFlushMs = millis();
        _active = true;
        return true;
    }

    bool append(const CaptureSlot& slot) {
        if (!_active || !_file || !_batch) return false;

        RadiotapHeader radio = {};
        radio.version = 0;
        radio.length = sizeof(RadiotapHeader);
        radio.present = 0x0000082A;  // flags, channel, signal, antenna
        const bool completeFrame =
            slot.capturedLength == slot.originalLength &&
            slot.originalLength >= 4;
        radio.flags = completeFrame ? 0x10 : 0x00;  // FCS at end
        if (completeFrame && slot.rxState != 0) radio.flags |= 0x40;
        radio.channelFrequency = channelFrequency(slot.channel);
        radio.channelFlags = 0x0080;  // 2 GHz
        radio.signalDbm = slot.rssi;
        radio.antenna = slot.antenna;

        const uint64_t timestamp =
            NetCore::unixMicrosForMonotonic(slot.monotonicUs);
        PcapRecordHeader record = {};
        record.seconds = static_cast<uint32_t>(timestamp / 1000000ULL);
        record.microseconds = static_cast<uint32_t>(timestamp % 1000000ULL);
        record.includedLength =
            sizeof(RadiotapHeader) + slot.capturedLength;
        record.originalLength =
            sizeof(RadiotapHeader) + slot.originalLength;

        const size_t required =
            sizeof(record) + sizeof(radio) + slot.capturedLength;
        if (required > _batchCapacity) {
            _lastError = "PCAP record exceeds batch buffer";
            _active = false;
            return false;
        }
        if (_used + required > _batchCapacity && !flush()) return false;

        copyToBatch(&record, sizeof(record));
        copyToBatch(&radio, sizeof(radio));
        copyToBatch(slot.data, slot.capturedLength);
        ++_packets;
        _bytes += required;

        if (_used >= (_batchCapacity * 3U) / 4U) return flush();
        return true;
    }

    bool periodicFlush() {
        if (!_active || _used == 0) return true;
        if (static_cast<uint32_t>(millis() - _lastFlushMs) <
            PCAP_FLUSH_INTERVAL_MS) {
            return true;
        }
        return flush();
    }

    bool flush() {
        if (!_file || _used == 0) {
            _lastFlushMs = millis();
            return true;
        }

        DisplayRuntime::ScopedSdDisplayRelease sdGuard;
        const size_t written = _file.write(_batch, _used);
        _file.flush();
        if (written != _used) {
            _lastError = "Incomplete PCAP SD write";
            _active = false;
            return false;
        }
        _used = 0;
        _lastFlushMs = millis();
        return true;
    }

    void stop() {
        if (_file) {
            if (_active) flush();
            DisplayRuntime::ScopedSdDisplayRelease sdGuard;
            _file.flush();
            _file.close();
        }
        _active = false;
        _used = 0;
    }

    void shutdown() {
        stop();
        if (_batch) {
            RuntimeMemory::release(_batch);
            _batch = nullptr;
        }
        _batchCapacity = 0;
    }

    bool active() const { return _active; }
    uint64_t packets() const { return _packets; }
    uint64_t bytes() const { return _bytes; }
    const String& path() const { return _path; }
    const String& error() const { return _lastError; }

private:
    bool allocateBatch() {
        _batch = static_cast<uint8_t*>(
            RuntimeMemory::allocateExternal(PCAP_BATCH_PSRAM));
        if (_batch) {
            _batchCapacity = PCAP_BATCH_PSRAM;
            return true;
        }
        _batch = static_cast<uint8_t*>(RuntimeMemory::allocateInternal(
            PCAP_BATCH_FALLBACK, false, INSPECTOR_MEMORY_RESERVE));
        if (_batch) {
            _batchCapacity = PCAP_BATCH_FALLBACK;
            return true;
        }
        _batch = static_cast<uint8_t*>(RuntimeMemory::allocateInternal(
            PCAP_BATCH_MINIMUM, false, INSPECTOR_MEMORY_RESERVE));
        if (_batch) {
            _batchCapacity = PCAP_BATCH_MINIMUM;
            return true;
        }
        return false;
    }

    String makeUniquePath(uint8_t channel) {
        char stem[96] = {};
        const time_t now = time(nullptr);
        if (NetCore::timeSynchronized() && now > 1700000000) {
            tm utc = {};
            gmtime_r(&now, &utc);
            snprintf(stem, sizeof(stem),
                     "%s/NET_%04d%02d%02d_%02d%02d%02dZ_CH%02u",
                     CAPTURE_DIRECTORY, utc.tm_year + 1900, utc.tm_mon + 1,
                     utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                     channel);
        } else {
            snprintf(stem, sizeof(stem), "%s/NET_UP%010lu_CH%02u",
                     CAPTURE_DIRECTORY, static_cast<unsigned long>(millis()),
                     channel);
        }

        String candidate = String(stem) + ".pcap";
        for (uint16_t suffix = 1; SD.exists(candidate.c_str()); ++suffix) {
            candidate = String(stem) + "_" + String(suffix) + ".pcap";
        }
        return candidate;
    }

    void copyToBatch(const void* data, size_t length) {
        memcpy(_batch + _used, data, length);
        _used += length;
    }

    File _file;
    uint8_t* _batch = nullptr;
    size_t _batchCapacity = 0;
    size_t _used = 0;
    bool _active = false;
    uint32_t _lastFlushMs = 0;
    uint64_t _packets = 0;
    uint64_t _bytes = 0;
    String _path;
    String _lastError;
};

PcapWriter g_pcap;

void IRAM_ATTR captureCallback(void* buffer,
                               wifi_promiscuous_pkt_type_t packetType) {
    if (!g_captureSlots || g_captureCapacity < 2 || !buffer ||
        packetType == WIFI_PKT_MISC) {
        return;
    }

    const wifi_promiscuous_pkt_t* packet =
        static_cast<const wifi_promiscuous_pkt_t*>(buffer);
    const uint16_t originalLength = packet->rx_ctrl.sig_len;
    if (originalLength < 2) return;

    const uint32_t write =
        __atomic_load_n(&g_captureWrite, __ATOMIC_RELAXED);
    const uint32_t read =
        __atomic_load_n(&g_captureRead, __ATOMIC_ACQUIRE);
    const uint32_t next =
        static_cast<uint32_t>((write + 1) % g_captureCapacity);
    if (next == read) {
        __atomic_fetch_add(&g_captureDropped, 1, __ATOMIC_RELAXED);
        return;
    }

    CaptureSlot& slot = g_captureSlots[write];
    slot.monotonicUs = static_cast<uint64_t>(esp_timer_get_time());
    slot.originalLength = originalLength;
    slot.capturedLength =
        originalLength < SNAPLEN ? originalLength : SNAPLEN;
    slot.rssi = packet->rx_ctrl.rssi;
    slot.channel = packet->rx_ctrl.channel;
    slot.antenna = packet->rx_ctrl.ant;
    slot.rate = packet->rx_ctrl.rate;
    slot.mcs = packet->rx_ctrl.mcs;
    slot.signalMode = packet->rx_ctrl.sig_mode;
    slot.rxState = packet->rx_ctrl.rx_state;
    slot.packetType = static_cast<uint8_t>(packetType);
    memcpy(slot.data, packet->payload, slot.capturedLength);

    __atomic_store_n(&g_captureWrite, next, __ATOMIC_RELEASE);
}

bool readDnsName(const uint8_t* data, size_t length, size_t offset,
                 char* output, size_t outputCapacity, size_t* endOffset) {
    if (!data || !output || outputCapacity == 0 || offset >= length) {
        return false;
    }

    size_t cursor = offset;
    size_t nextOffset = offset;
    size_t written = 0;
    bool jumped = false;
    uint8_t jumps = 0;
    output[0] = '\0';

    while (cursor < length && jumps < 16) {
        const uint8_t labelLength = data[cursor];
        if (labelLength == 0) {
            if (!jumped) nextOffset = cursor + 1;
            if (written == 0 && outputCapacity > 1) output[written++] = '.';
            output[written] = '\0';
            if (endOffset) *endOffset = nextOffset;
            return true;
        }

        if ((labelLength & 0xC0) == 0xC0) {
            if (cursor + 1 >= length) return false;
            const uint16_t pointer =
                static_cast<uint16_t>(((labelLength & 0x3F) << 8) |
                                      data[cursor + 1]);
            if (pointer >= length) return false;
            if (!jumped) nextOffset = cursor + 2;
            cursor = pointer;
            jumped = true;
            ++jumps;
            continue;
        }

        if ((labelLength & 0xC0) != 0 || labelLength > 63 ||
            cursor + 1 + labelLength > length) {
            return false;
        }
        ++cursor;
        if (written > 0 && written + 1 < outputCapacity) {
            output[written++] = '.';
        }
        for (uint8_t i = 0;
             i < labelLength && written + 1 < outputCapacity; ++i) {
            const uint8_t ch = data[cursor + i];
            output[written++] =
                ch >= 0x20 && ch <= 0x7E ? static_cast<char>(ch) : '?';
        }
        cursor += labelLength;
        if (!jumped) nextOffset = cursor;
    }
    output[written < outputCapacity ? written : outputCapacity - 1] = '\0';
    return false;
}

bool parseDns(PacketRecord& record, const uint8_t* data, size_t length,
              bool mdns) {
    if (!data || length < 12) return false;
    const uint16_t flags = readBe16(data + 2);
    const uint16_t questions = readBe16(data + 4);
    const bool response = (flags & 0x8000) != 0;

    size_t afterName = 12;
    bool hasName = false;
    if (questions > 0) {
        hasName = readDnsName(data, length, 12, record.dnsName,
                              sizeof(record.dnsName), &afterName);
        if (hasName && afterName + 4 <= length) {
            record.dnsType = readBe16(data + afterName);
        }
    }

    setProtocol(record, mdns ? "mDNS" : "DNS");
    if (hasName) {
        writeText(record.info, sizeof(record.info), "%s %s %s",
                  response ? "Response" : "Query",
                  dnsTypeName(record.dnsType), record.dnsName);
    } else {
        writeText(record.info, sizeof(record.info), "%s id=0x%04X",
                  response ? "DNS response" : "DNS query", readBe16(data));
    }
    return true;
}

bool parseDhcp(PacketRecord& record, const uint8_t* data, size_t length) {
    if (!data || length < 240 || data[236] != 0x63 || data[237] != 0x82 ||
        data[238] != 0x53 || data[239] != 0x63) {
        return false;
    }

    uint8_t messageType = 0;
    size_t offset = 240;
    while (offset < length) {
        const uint8_t option = data[offset++];
        if (option == 0) continue;
        if (option == 255) break;
        if (offset >= length) break;
        const uint8_t optionLength = data[offset++];
        if (offset + optionLength > length) break;
        if (option == 53 && optionLength >= 1) {
            messageType = data[offset];
            break;
        }
        offset += optionLength;
    }

    setProtocol(record, "DHCP");
    writeText(record.info, sizeof(record.info), "DHCP %s xid=%08lX",
              dhcpTypeName(messageType),
              static_cast<unsigned long>(readBe32(data + 4)));
    return true;
}

void detectTcpApplication(PacketRecord& record, const uint8_t* payload,
                          size_t length) {
    if (!payload || length == 0) return;

    if ((record.sourcePort == 22 || record.destinationPort == 22) &&
        length >= 4 && memcmp(payload, "SSH-", 4) == 0) {
        setProtocol(record, "SSH");
        writeText(record.info, sizeof(record.info), "SSH identification");
        return;
    }

    if (length >= 5 && payload[0] == 0x16 && payload[1] == 0x03) {
        setProtocol(record, "TLS");
        const char* handshake =
            length >= 6 && payload[5] == 0x01 ? "ClientHello" :
            length >= 6 && payload[5] == 0x02 ? "ServerHello" : "Handshake";
        writeText(record.info, sizeof(record.info), "TLS 3.%u %s",
                  payload[2], handshake);
        return;
    }

    const char* methods[] = {"GET ", "POST ", "PUT ", "HEAD ",
                             "DELETE ", "PATCH ", "OPTIONS ", "HTTP/"};
    for (const char* method : methods) {
        const size_t methodLength = strlen(method);
        if (length >= methodLength &&
            memcmp(payload, method, methodLength) == 0) {
            setProtocol(record, "HTTP");
            char token[16] = {};
            size_t i = 0;
            while (i + 1 < sizeof(token) && i < length &&
                   payload[i] > 0x20 && payload[i] < 0x7F) {
                token[i] = static_cast<char>(payload[i]);
                ++i;
            }
            writeText(record.info, sizeof(record.info), "HTTP %s", token);
            return;
        }
    }
}

void parseTransport(PacketRecord& record, const uint8_t* data, size_t length,
                    uint8_t protocol, bool ipv6) {
    record.ipProtocol = protocol;
    if (protocol == 6) {
        if (length < 20) return;
        record.sourcePort = readBe16(data);
        record.destinationPort = readBe16(data + 2);
        record.tcpSequence = readBe32(data + 4);
        record.tcpAcknowledgment = readBe32(data + 8);
        const size_t headerLength =
            static_cast<size_t>((data[12] >> 4) * 4);
        record.tcpFlags = data[13];
        record.tcpWindow = readBe16(data + 14);
        setProtocol(record, "TCP");

        char flags[12] = {};
        formatTcpFlags(record.tcpFlags, flags, sizeof(flags));
        writeText(record.info, sizeof(record.info), "%u > %u [%s] Win=%u",
                  record.sourcePort, record.destinationPort, flags,
                  record.tcpWindow);

        if (headerLength < 20 || headerLength > length) return;
        const uint8_t* payload = data + headerLength;
        size_t payloadLength = length - headerLength;
        if ((record.sourcePort == 53 || record.destinationPort == 53) &&
            payloadLength >= 2) {
            const uint16_t dnsLength = readBe16(payload);
            if (dnsLength <= payloadLength - 2) {
                parseDns(record, payload + 2, dnsLength, false);
                return;
            }
        }
        detectTcpApplication(record, payload, payloadLength);
        return;
    }

    if (protocol == 17) {
        if (length < 8) return;
        record.sourcePort = readBe16(data);
        record.destinationPort = readBe16(data + 2);
        const uint8_t* payload = data + 8;
        const size_t payloadLength = length - 8;
        setProtocol(record, "UDP");
        writeText(record.info, sizeof(record.info), "%u > %u Len=%u",
                  record.sourcePort, record.destinationPort,
                  static_cast<unsigned>(payloadLength));

        if (record.sourcePort == 53 || record.destinationPort == 53) {
            parseDns(record, payload, payloadLength, false);
        } else if (record.sourcePort == 5353 ||
                   record.destinationPort == 5353) {
            parseDns(record, payload, payloadLength, true);
        } else if (record.sourcePort == 67 || record.sourcePort == 68 ||
                   record.destinationPort == 67 ||
                   record.destinationPort == 68) {
            parseDhcp(record, payload, payloadLength);
        } else if (record.sourcePort == 443 ||
                   record.destinationPort == 443) {
            setProtocol(record, "QUIC");
            writeText(record.info, sizeof(record.info), "QUIC %s header",
                      payloadLength > 0 && (payload[0] & 0x80) ? "long"
                                                               : "short");
        } else if (record.sourcePort == 1900 ||
                   record.destinationPort == 1900) {
            setProtocol(record, "SSDP");
            writeText(record.info, sizeof(record.info), "SSDP message");
        } else if (record.sourcePort == 123 ||
                   record.destinationPort == 123) {
            setProtocol(record, "NTP");
            writeText(record.info, sizeof(record.info), "NTP message");
        }
        return;
    }

    if (protocol == 1 || protocol == 58) {
        if (length < 4) return;
        record.icmpType = data[0];
        record.icmpCode = data[1];
        setProtocol(record, protocol == 58 ? "ICMPv6" : "ICMP");
        writeText(record.info, sizeof(record.info), "%s type=%u code=%u",
                  protocol == 58 ? "ICMPv6" : "ICMP", record.icmpType,
                  record.icmpCode);
        return;
    }

    if (protocol == 50) {
        setProtocol(record, "ESP");
        writeText(record.info, sizeof(record.info), "IPsec ESP");
        return;
    }

    writeText(record.info, sizeof(record.info), "%s next-header=%u",
              ipv6 ? "IPv6" : "IPv4", protocol);
}

void parseIpv4(PacketRecord& record, const uint8_t* data, size_t length) {
    if (!data || length < 20 || (data[0] >> 4) != 4) return;
    const size_t headerLength = static_cast<size_t>((data[0] & 0x0F) * 4);
    if (headerLength < 20 || headerLength > length) return;

    const uint16_t totalLength = readBe16(data + 2);
    const size_t packetLength =
        totalLength >= headerLength && totalLength < length ? totalLength
                                                            : length;
    record.ipVersion = 4;
    record.ttl = data[8];
    record.ipProtocol = data[9];
    formatIpv4(data + 12, record.source, sizeof(record.source));
    formatIpv4(data + 16, record.destination, sizeof(record.destination));
    setProtocol(record, "IPv4");

    const uint16_t fragment = readBe16(data + 6);
    const uint16_t fragmentOffset = fragment & 0x1FFF;
    if (fragmentOffset != 0) {
        writeText(record.info, sizeof(record.info),
                  "IPv4 fragment offset=%u", fragmentOffset * 8U);
        return;
    }

    parseTransport(record, data + headerLength, packetLength - headerLength,
                   data[9], false);
}

void parseIpv6(PacketRecord& record, const uint8_t* data, size_t length) {
    if (!data || length < 40 || (data[0] >> 4) != 6) return;
    record.ipVersion = 6;
    record.ttl = data[7];
    formatIpv6(data + 8, record.source, sizeof(record.source));
    formatIpv6(data + 24, record.destination, sizeof(record.destination));
    setProtocol(record, "IPv6");

    uint8_t nextHeader = data[6];
    size_t offset = 40;
    for (uint8_t depth = 0; depth < 6 && offset < length; ++depth) {
        if (nextHeader == 0 || nextHeader == 43 || nextHeader == 60) {
            if (offset + 2 > length) return;
            const uint8_t following = data[offset];
            const size_t extensionLength =
                static_cast<size_t>(data[offset + 1] + 1) * 8;
            if (extensionLength < 8 || offset + extensionLength > length) {
                return;
            }
            nextHeader = following;
            offset += extensionLength;
            continue;
        }
        if (nextHeader == 44) {
            if (offset + 8 > length) return;
            const uint8_t following = data[offset];
            const uint16_t fragmentOffset =
                readBe16(data + offset + 2) & 0xFFF8;
            nextHeader = following;
            offset += 8;
            if (fragmentOffset != 0) {
                writeText(record.info, sizeof(record.info),
                          "IPv6 fragment offset=%u", fragmentOffset);
                return;
            }
            continue;
        }
        if (nextHeader == 51) {
            if (offset + 2 > length) return;
            const uint8_t following = data[offset];
            const size_t extensionLength =
                static_cast<size_t>(data[offset + 1] + 2) * 4;
            if (extensionLength < 8 || offset + extensionLength > length) {
                return;
            }
            nextHeader = following;
            offset += extensionLength;
            continue;
        }
        break;
    }
    if (offset <= length) {
        parseTransport(record, data + offset, length - offset, nextHeader,
                       true);
    }
}

void parseArp(PacketRecord& record, const uint8_t* data, size_t length) {
    if (!data || length < 28 || readBe16(data) != 1 ||
        readBe16(data + 2) != 0x0800 || data[4] != 6 || data[5] != 4) {
        return;
    }

    const uint16_t operation = readBe16(data + 6);
    char senderIp[16] = {};
    char targetIp[16] = {};
    char senderMac[20] = {};
    formatIpv4(data + 14, senderIp, sizeof(senderIp));
    formatIpv4(data + 24, targetIp, sizeof(targetIp));
    formatMac(data + 8, senderMac, sizeof(senderMac));
    writeText(record.source, sizeof(record.source), "%s", senderIp);
    writeText(record.destination, sizeof(record.destination), "%s", targetIp);
    setProtocol(record, "ARP");
    if (operation == 1) {
        writeText(record.info, sizeof(record.info), "Who has %s? Tell %s",
                  targetIp, senderIp);
    } else if (operation == 2) {
        writeText(record.info, sizeof(record.info), "%s is at %s", senderIp,
                  senderMac);
    } else {
        writeText(record.info, sizeof(record.info), "ARP operation=%u",
                  operation);
    }
}

void extractSsid(const uint8_t* frame, size_t length, size_t tagsOffset,
                 char* output, size_t capacity) {
    if (!output || capacity == 0) return;
    output[0] = '\0';
    size_t offset = tagsOffset;
    while (offset + 2 <= length) {
        const uint8_t id = frame[offset++];
        const uint8_t fieldLength = frame[offset++];
        if (offset + fieldLength > length) break;
        if (id == 0) {
            const size_t copyLength =
                fieldLength < capacity - 1 ? fieldLength : capacity - 1;
            for (size_t i = 0; i < copyLength; ++i) {
                const uint8_t ch = frame[offset + i];
                output[i] =
                    ch >= 0x20 && ch <= 0x7E ? static_cast<char>(ch) : '?';
            }
            output[copyLength] = '\0';
            return;
        }
        offset += fieldLength;
    }
}

void parseManagement(PacketRecord& record, const uint8_t* frame,
                     size_t length) {
    setProtocol(record, "MGMT");
    if (length < 24) {
        writeText(record.info, sizeof(record.info), "Short management frame");
        return;
    }

    formatMac(frame + 10, record.source, sizeof(record.source));
    formatMac(frame + 4, record.destination, sizeof(record.destination));
    formatMac(frame + 16, record.bssid, sizeof(record.bssid));

    size_t tagsOffset = 0;
    if (record.wifiSubtype == 8 || record.wifiSubtype == 5) {
        tagsOffset = 36;
    } else if (record.wifiSubtype == 4) {
        tagsOffset = 24;
    } else if (record.wifiSubtype == 0) {
        tagsOffset = 28;
    } else if (record.wifiSubtype == 2) {
        tagsOffset = 34;
    }
    if (tagsOffset > 0 && tagsOffset < length) {
        extractSsid(frame, length, tagsOffset, record.ssid,
                    sizeof(record.ssid));
    }

    const char* subtype = managementSubtypeName(record.wifiSubtype);
    if ((record.wifiSubtype == 8 || record.wifiSubtype == 5 ||
         record.wifiSubtype == 4) &&
        tagsOffset > 0) {
        writeText(record.info, sizeof(record.info), "%s SSID=%s", subtype,
                  record.ssid[0] ? record.ssid : "<hidden>");
    } else if ((record.wifiSubtype == 10 || record.wifiSubtype == 12) &&
               length >= 26) {
        writeText(record.info, sizeof(record.info), "%s reason=%u", subtype,
                  readLe16(frame + 24));
    } else if (record.wifiSubtype == 11 && length >= 30) {
        writeText(record.info, sizeof(record.info),
                  "Authentication alg=%u seq=%u status=%u",
                  readLe16(frame + 24), readLe16(frame + 26),
                  readLe16(frame + 28));
    } else if (record.wifiSubtype == 13 && length >= 25) {
        writeText(record.info, sizeof(record.info), "Action category=%u",
                  frame[24]);
    } else {
        writeText(record.info, sizeof(record.info), "%s", subtype);
    }
}

void parseControl(PacketRecord& record, const uint8_t* frame,
                  size_t length) {
    setProtocol(record, "CTRL");
    if (length >= 10) {
        formatMac(frame + 4, record.destination, sizeof(record.destination));
    }
    if (length >= 16) {
        formatMac(frame + 10, record.source, sizeof(record.source));
    }
    writeText(record.info, sizeof(record.info), "%s",
              controlSubtypeName(record.wifiSubtype));
}

void parseData(PacketRecord& record, const uint8_t* frame, size_t length) {
    setProtocol(record, "DATA");
    if (length < 24) {
        writeText(record.info, sizeof(record.info), "Short data frame");
        return;
    }

    const uint8_t* address1 = frame + 4;
    const uint8_t* address2 = frame + 10;
    const uint8_t* address3 = frame + 16;
    size_t baseHeaderLength = 24;

    if (!record.toDs && !record.fromDs) {
        formatMac(address2, record.source, sizeof(record.source));
        formatMac(address1, record.destination, sizeof(record.destination));
        formatMac(address3, record.bssid, sizeof(record.bssid));
    } else if (record.toDs && !record.fromDs) {
        formatMac(address2, record.source, sizeof(record.source));
        formatMac(address3, record.destination, sizeof(record.destination));
        formatMac(address1, record.bssid, sizeof(record.bssid));
    } else if (!record.toDs && record.fromDs) {
        formatMac(address3, record.source, sizeof(record.source));
        formatMac(address1, record.destination, sizeof(record.destination));
        formatMac(address2, record.bssid, sizeof(record.bssid));
    } else {
        if (length < 30) return;
        formatMac(frame + 24, record.source, sizeof(record.source));
        formatMac(address3, record.destination, sizeof(record.destination));
        writeText(record.bssid, sizeof(record.bssid), "-");
        baseHeaderLength = 30;
    }

    const bool qos = (record.wifiSubtype & 0x08) != 0;
    bool amsdu = false;
    size_t headerLength = baseHeaderLength;
    if (qos) {
        if (headerLength + 2 > length) return;
        const uint16_t qosControl = readLe16(frame + headerLength);
        amsdu = (qosControl & 0x0080) != 0;
        headerLength += 2;
    }
    if ((record.frameControl & 0x8000) != 0 && qos) {
        if (headerLength + 4 > length) return;
        headerLength += 4;
    }

    if (record.protectedFrame) {
        setProtocol(record, "802.11");
        writeText(record.info, sizeof(record.info), "Protected data frame");
        return;
    }
    if (amsdu) {
        writeText(record.info, sizeof(record.info), "QoS A-MSDU");
        return;
    }
    if (headerLength + 8 > length) {
        writeText(record.info, sizeof(record.info), "Data, no LLC payload");
        return;
    }

    const uint8_t* llc = frame + headerLength;
    size_t payloadLength = length - headerLength;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) {
        writeText(record.info, sizeof(record.info), "Data payload");
        return;
    }

    uint16_t etherType = readBe16(llc + 6);
    const uint8_t* payload = llc + 8;
    payloadLength -= 8;
    if (etherType == 0x8100 && payloadLength >= 4) {
        etherType = readBe16(payload + 2);
        payload += 4;
        payloadLength -= 4;
    }
    record.etherType = etherType;

    switch (etherType) {
        case 0x0800:
            parseIpv4(record, payload, payloadLength);
            break;
        case 0x0806:
            parseArp(record, payload, payloadLength);
            break;
        case 0x86DD:
            parseIpv6(record, payload, payloadLength);
            break;
        case 0x888E:
            setProtocol(record, "EAPOL");
            writeText(record.info, sizeof(record.info), "802.1X EAPOL");
            break;
        default:
            writeText(record.info, sizeof(record.info), "EtherType 0x%04X",
                      etherType);
            break;
    }
}

void parseCapture(const CaptureSlot& slot, PacketRecord& record) {
    record = {};
    record.number = g_nextPacketNumber++;
    record.monotonicUs = slot.monotonicUs;
    record.capturedLength = slot.capturedLength;
    record.originalLength = slot.originalLength;
    record.rssi = slot.rssi;
    record.channel = slot.channel;
    record.antenna = slot.antenna;
    record.rate = slot.rate;
    record.mcs = slot.mcs;
    record.signalMode = slot.signalMode;
    record.rxState = slot.rxState;
    writeText(record.source, sizeof(record.source), "-");
    writeText(record.destination, sizeof(record.destination), "-");
    writeText(record.bssid, sizeof(record.bssid), "-");
    setProtocol(record, "802.11");

    record.rawPreviewLength =
        slot.capturedLength < RAW_PREVIEW_SIZE ? slot.capturedLength
                                              : RAW_PREVIEW_SIZE;
    memcpy(record.rawPreview, slot.data, record.rawPreviewLength);

    if (slot.capturedLength < 2) {
        writeText(record.info, sizeof(record.info), "Short 802.11 frame");
        return;
    }

    record.frameControl = readLe16(slot.data);
    record.wifiType = (record.frameControl >> 2) & 0x03;
    record.wifiSubtype = (record.frameControl >> 4) & 0x0F;
    record.toDs = (record.frameControl & 0x0100) != 0;
    record.fromDs = (record.frameControl & 0x0200) != 0;
    record.moreFragments = (record.frameControl & 0x0400) != 0;
    record.retry = (record.frameControl & 0x0800) != 0;
    record.protectedFrame = (record.frameControl & 0x4000) != 0;
    if (slot.capturedLength >= 24) {
        record.wifiSequence = readLe16(slot.data + 22) >> 4;
    }

    switch (record.wifiType) {
        case 0:
            parseManagement(record, slot.data, slot.capturedLength);
            break;
        case 1:
            parseControl(record, slot.data, slot.capturedLength);
            break;
        case 2:
            parseData(record, slot.data, slot.capturedLength);
            break;
        default:
            writeText(record.info, sizeof(record.info), "802.11 extension");
            break;
    }
}

void accountRecord(const PacketRecord& record) {
    ++g_stats.totalPackets;
    g_stats.totalBytes += record.originalLength;
    ++g_stats.ppsAccumulator;

    if (record.wifiType == 0) ++g_stats.managementFrames;
    if (record.wifiType == 1) ++g_stats.controlFrames;
    if (record.wifiType == 2) ++g_stats.dataFrames;
    if (record.protectedFrame) ++g_stats.protectedFrames;
    if (record.retry) ++g_stats.retryFrames;

    if (record.channel <= 14) ++g_stats.channelPackets[record.channel];
    g_stats.rssiTotal += record.rssi;
    ++g_stats.rssiSamples;
    if (record.rssi >= -50) {
        ++g_stats.rssiStrong;
    } else if (record.rssi >= -65) {
        ++g_stats.rssiGood;
    } else if (record.rssi >= -80) {
        ++g_stats.rssiWeak;
    } else {
        ++g_stats.rssiPoor;
    }

    if (strcmp(record.protocol, "TCP") == 0 ||
        strcmp(record.protocol, "TLS") == 0 ||
        strcmp(record.protocol, "HTTP") == 0 ||
        strcmp(record.protocol, "SSH") == 0) {
        ++g_stats.tcpPackets;
    }
    if (strcmp(record.protocol, "UDP") == 0 ||
        strcmp(record.protocol, "QUIC") == 0 ||
        strcmp(record.protocol, "SSDP") == 0 ||
        strcmp(record.protocol, "NTP") == 0 ||
        strcmp(record.protocol, "DHCP") == 0) {
        ++g_stats.udpPackets;
    }
    if (strcmp(record.protocol, "DNS") == 0 ||
        strcmp(record.protocol, "mDNS") == 0) {
        ++g_stats.dnsPackets;
    }
    if (strcmp(record.protocol, "ARP") == 0) ++g_stats.arpPackets;
    if (strcmp(record.protocol, "ICMP") == 0 ||
        strcmp(record.protocol, "ICMPv6") == 0) {
        ++g_stats.icmpPackets;
    }
}

void updatePps() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - g_stats.ppsUpdatedAtMs;
    if (elapsed < 1000) return;
    g_stats.packetsPerSecond =
        elapsed > 0
            ? static_cast<uint32_t>(
                  (static_cast<uint64_t>(g_stats.ppsAccumulator) * 1000ULL) /
                  elapsed)
            : 0;
    g_stats.ppsAccumulator = 0;
    g_stats.ppsUpdatedAtMs = now;
}

void addHistory(const PacketRecord& record) {
    if (!g_history || g_historyCapacity == 0) return;
    g_history[g_historyWrite] = record;
    g_historyWrite = (g_historyWrite + 1) % g_historyCapacity;
    if (g_historyCount < g_historyCapacity) ++g_historyCount;
}

bool matchesFilter(const PacketRecord& record) {
    switch (g_filter) {
        case DisplayFilter::ALL: return true;
        case DisplayFilter::MANAGEMENT: return record.wifiType == 0;
        case DisplayFilter::DATA: return record.wifiType == 2;
        case DisplayFilter::TCP:
            return strcmp(record.protocol, "TCP") == 0 ||
                   strcmp(record.protocol, "TLS") == 0 ||
                   strcmp(record.protocol, "HTTP") == 0 ||
                   strcmp(record.protocol, "SSH") == 0;
        case DisplayFilter::UDP:
            return strcmp(record.protocol, "UDP") == 0 ||
                   strcmp(record.protocol, "QUIC") == 0 ||
                   strcmp(record.protocol, "SSDP") == 0 ||
                   strcmp(record.protocol, "NTP") == 0 ||
                   strcmp(record.protocol, "DHCP") == 0;
        case DisplayFilter::DNS:
            return strcmp(record.protocol, "DNS") == 0 ||
                   strcmp(record.protocol, "mDNS") == 0;
        case DisplayFilter::ARP:
            return strcmp(record.protocol, "ARP") == 0;
        case DisplayFilter::ICMP:
            return strcmp(record.protocol, "ICMP") == 0 ||
                   strcmp(record.protocol, "ICMPv6") == 0;
        case DisplayFilter::PROTECTED: return record.protectedFrame;
        default: return true;
    }
}

size_t collectVisible(uint16_t* indices, size_t capacity) {
    if (!indices || !g_history || capacity == 0) return 0;
    const size_t oldest =
        (g_historyWrite + g_historyCapacity - g_historyCount) %
        g_historyCapacity;
    size_t visible = 0;
    for (size_t i = 0; i < g_historyCount && visible < capacity; ++i) {
        const size_t physical = (oldest + i) % g_historyCapacity;
        if (matchesFilter(g_history[physical])) {
            indices[visible++] = static_cast<uint16_t>(physical);
        }
    }
    return visible;
}

PacketRecord* selectedRecord() {
    uint16_t indices[HISTORY_CAPACITY_PSRAM];
    const size_t count = collectVisible(indices, HISTORY_CAPACITY_PSRAM);
    if (count == 0) {
        g_selectedVisible = -1;
        return nullptr;
    }
    if (g_followLatest || g_selectedVisible < 0) {
        g_selectedVisible = static_cast<int>(count) - 1;
    }
    if (g_selectedVisible >= static_cast<int>(count)) {
        g_selectedVisible = static_cast<int>(count) - 1;
    }
    return &g_history[indices[g_selectedVisible]];
}

size_t processCaptured(size_t budget) {
    size_t processed = 0;
    while (processed < budget) {
        const uint32_t read =
            __atomic_load_n(&g_captureRead, __ATOMIC_RELAXED);
        const uint32_t write =
            __atomic_load_n(&g_captureWrite, __ATOMIC_ACQUIRE);
        if (read == write) break;

        const CaptureSlot slot = g_captureSlots[read];
        const uint32_t next =
            static_cast<uint32_t>((read + 1) % g_captureCapacity);
        __atomic_store_n(&g_captureRead, next, __ATOMIC_RELEASE);

        if (g_pcap.active()) g_pcap.append(slot);

        PacketRecord record;
        parseCapture(slot, record);
        accountRecord(record);
        addHistory(record);
        ++processed;
    }
    if (processed > 0) g_screenDirty = true;
    return processed;
}

void resetHistoryAndStats() {
    g_historyWrite = 0;
    g_historyCount = 0;
    g_nextPacketNumber = 1;
    g_stats = {};
    g_stats.ppsUpdatedAtMs = millis();
    g_selectedVisible = -1;
    g_detailScroll = 0;
    g_followLatest = true;
    g_screenDirty = true;
}

bool tryBufferProfile(size_t captureCapacity, size_t historyCapacity,
                      bool external) {
    const size_t captureBytes = captureCapacity * sizeof(CaptureSlot);
    const size_t historyBytes = historyCapacity * sizeof(PacketRecord);

    if (external) {
        g_captureSlots = static_cast<CaptureSlot*>(
            RuntimeMemory::allocateExternal(captureBytes, true));
        g_history = static_cast<PacketRecord*>(
            RuntimeMemory::allocateExternal(historyBytes, true));
    } else {
        g_captureSlots = static_cast<CaptureSlot*>(
            RuntimeMemory::allocateInternal(
                captureBytes, true, INSPECTOR_MEMORY_RESERVE + historyBytes));
        if (g_captureSlots) {
            g_history = static_cast<PacketRecord*>(
                RuntimeMemory::allocateInternal(historyBytes, true,
                                                INSPECTOR_MEMORY_RESERVE));
        }
    }

    if (!g_captureSlots || !g_history) {
        RuntimeMemory::release(g_captureSlots);
        RuntimeMemory::release(g_history);
        g_captureSlots = nullptr;
        g_history = nullptr;
        return false;
    }
    g_captureCapacity = captureCapacity;
    g_historyCapacity = historyCapacity;
    return true;
}

bool allocateBuffers(String& error) {
    if (RuntimeMemory::externalAvailable() &&
        tryBufferProfile(CAPTURE_CAPACITY_PSRAM, HISTORY_CAPACITY_PSRAM,
                         true)) {
        // Full profile.
    } else {
        const struct {
            size_t capture;
            size_t history;
        } profiles[] = {{32, 24}, {24, 16}, {16, 12}, {8, 8}};
        bool allocated = false;
        for (const auto& profile : profiles) {
            if (tryBufferProfile(profile.capture, profile.history, false)) {
                allocated = true;
                break;
            }
        }
        if (!allocated) {
            error = String("Capture buffers unavailable; ") +
                    RuntimeMemory::describe();
            return false;
        }
    }
    __atomic_store_n(&g_captureWrite, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_captureRead, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_captureDropped, 0, __ATOMIC_RELAXED);
    resetHistoryAndStats();
    return true;
}

void releaseBuffers() {
    RuntimeMemory::release(g_captureSlots);
    RuntimeMemory::release(g_history);
    g_captureSlots = nullptr;
    g_history = nullptr;
    g_captureCapacity = 0;
    g_historyCapacity = 0;
    g_captureWrite = 0;
    g_captureRead = 0;
}

const char* filterName() {
    switch (g_filter) {
        case DisplayFilter::ALL: return "ALL";
        case DisplayFilter::MANAGEMENT: return "MGMT";
        case DisplayFilter::DATA: return "DATA";
        case DisplayFilter::TCP: return "TCP";
        case DisplayFilter::UDP: return "UDP";
        case DisplayFilter::DNS: return "DNS";
        case DisplayFilter::ARP: return "ARP";
        case DisplayFilter::ICMP: return "ICMP";
        case DisplayFilter::PROTECTED: return "CRYPT";
        default: return "ALL";
    }
}

const char* viewName() {
    switch (g_view) {
        case ViewMode::LIVE: return "LIVE";
        case ViewMode::DETAILS: return "DETAIL";
        case ViewMode::STATS: return "STATS";
        case ViewMode::RADIO: return "RADIO";
        default: return "NET";
    }
}

void setNotice(const String& notice, uint32_t durationMs = 1600) {
    g_notice = notice;
    g_noticeUntilMs = millis() + durationMs;
    g_screenDirty = true;
}

String compactEndpoint(const char* endpoint, uint16_t port) {
    String value(endpoint ? endpoint : "-");
    if (value.indexOf('.') >= 0) {
        const int lastDot = value.lastIndexOf('.');
        const int previousDot =
            lastDot > 0 ? value.lastIndexOf('.', lastDot - 1) : -1;
        if (previousDot >= 0) value = value.substring(previousDot + 1);
    } else if (value.length() > 9) {
        value = value.substring(value.length() - 9);
    }
    if (port != 0) value += ":" + String(port);
    if (value.length() > 14) value = value.substring(value.length() - 14);
    return value;
}

String formatBytes(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL) {
        return String(static_cast<float>(bytes) / (1024.0f * 1024.0f), 1) +
               " MB";
    }
    if (bytes >= 1024ULL) {
        return String(static_cast<float>(bytes) / 1024.0f, 1) + " KB";
    }
    return String(static_cast<uint32_t>(bytes)) + " B";
}

void drawChrome(const char* footer) {
    LB::fillScreen(TFT_BLACK);
    LB::setTextFont(1);
    LB::setTextSize(1);
    LB::fillRect(0, 0, LB::width(), 13, TFT_DARKGREY);

    String header = String("NET ") + viewName() + " CH" +
                    String(g_currentChannel) + " " + filterName() + " " +
                    String(g_stats.packetsPerSecond) + "p/s D" +
                    String(__atomic_load_n(&g_captureDropped,
                                           __ATOMIC_RELAXED));
    LB::setTextColor(g_pcap.active() ? TFT_GREEN : TFT_WHITE);
    LB::setCursor(2, 2);
    LB::print(header);

    LB::fillRect(0, LB::height() - 10, LB::width(), 10, TFT_DARKGREY);
    LB::setTextColor(TFT_CYAN);
    LB::setCursor(2, LB::height() - 9);
    if (g_notice.length() > 0 &&
        static_cast<int32_t>(g_noticeUntilMs - millis()) > 0) {
        LB::print(g_notice);
    } else {
        LB::print(footer);
    }
}

void drawLive() {
    drawChrome("R rec F filter Tab views Enter detail");
    uint16_t indices[HISTORY_CAPACITY_PSRAM];
    const size_t count = collectVisible(indices, HISTORY_CAPACITY_PSRAM);
    if (count == 0) {
        LB::setTextColor(TFT_YELLOW);
        LB::setCursor(5, 28);
        LB::println("Waiting for 802.11 frames...");
        LB::setTextColor(TFT_DARKGREY);
        LB::setCursor(5, 42);
        LB::println("Connected STA stays on its AP channel.");
        return;
    }

    if (g_followLatest || g_selectedVisible < 0) {
        g_selectedVisible = static_cast<int>(count) - 1;
    }
    if (g_selectedVisible >= static_cast<int>(count)) {
        g_selectedVisible = static_cast<int>(count) - 1;
    }

    const int infoY = LB::height() - 21;
    int rows = (infoY - 14) / 10;
    if (rows < 1) rows = 1;
    int top = g_selectedVisible - rows + 1;
    if (top < 0) top = 0;
    if (top + rows > static_cast<int>(count)) {
        top = static_cast<int>(count) - rows;
        if (top < 0) top = 0;
    }

    for (int row = 0; row < rows && top + row < static_cast<int>(count);
         ++row) {
        const int visibleIndex = top + row;
        const PacketRecord& record = g_history[indices[visibleIndex]];
        const int y = 14 + row * 10;
        const bool selected = visibleIndex == g_selectedVisible;
        if (selected) {
            LB::fillRect(0, y - 1, LB::width(), 10, TFT_NAVY);
            LB::setTextColor(TFT_GREEN);
        } else {
            LB::setTextColor(record.protectedFrame ? TFT_DARKGREY
                                                   : TFT_WHITE);
        }

        char line[196] = {};
        if (LB::width() >= 400) {
            snprintf(line, sizeof(line),
                     "%c%06lu %-8s %-39s > %-39s %4u  %s",
                     selected ? '>' : ' ',
                     static_cast<unsigned long>(record.number),
                     record.protocol, record.source, record.destination,
                     record.originalLength, record.info);
        } else {
            const String source =
                compactEndpoint(record.source, record.sourcePort);
            const String destination = compactEndpoint(
                record.destination, record.destinationPort);
            snprintf(line, sizeof(line), "%c%05lu %-6s %s>%s",
                     selected ? '>' : ' ',
                     static_cast<unsigned long>(record.number),
                     record.protocol, source.c_str(), destination.c_str());
        }
        LB::setCursor(1, y);
        LB::print(line);
    }

    const PacketRecord& selected = g_history[indices[g_selectedVisible]];
    LB::fillRect(0, infoY, LB::width(), 10, TFT_BLACK);
    LB::setTextColor(TFT_YELLOW);
    LB::setCursor(2, infoY + 1);
    LB::print(selected.info);
}

int addWrappedLine(String* lines, int count, int capacity,
                   const String& prefix, const String& value) {
    if (count >= capacity) return count;
    const int columns = LB::width() >= 400 ? 76 : 38;
    String combined = prefix + value;
    for (int offset = 0;
         offset < static_cast<int>(combined.length()) && count < capacity;
         offset += columns) {
        lines[count++] = combined.substring(offset, offset + columns);
    }
    if (combined.length() == 0 && count < capacity) lines[count++] = "";
    return count;
}

int buildDetailLines(const PacketRecord& record, String* lines,
                     int capacity) {
    int count = 0;
    auto add = [&](const String& value) {
        if (count < capacity) lines[count++] = value;
    };

    add("#" + String(record.number) + "  " + record.protocol +
        "  t=" +
        String(static_cast<uint32_t>(record.monotonicUs / 1000ULL)) + "ms");
    count = addWrappedLine(lines, count, capacity, "Source: ",
                           record.source);
    count = addWrappedLine(lines, count, capacity, "Destination: ",
                           record.destination);
    count = addWrappedLine(lines, count, capacity, "BSSID: ",
                           record.bssid);
    add("Radio: RSSI " + String(static_cast<int>(record.rssi)) + " dBm CH" +
        String(record.channel) + " ant " + String(record.antenna));
    add("PHY: mode " + String(record.signalMode) + " rate " +
        String(record.rate) + " MCS " + String(record.mcs));
    add("Length: cap " + String(record.capturedLength) + " / wire " +
        String(record.originalLength));
    add("802.11: type " + String(record.wifiType) + " subtype " +
        String(record.wifiSubtype) + " seq " +
        String(record.wifiSequence));
    add("WiFi flags:" + String(record.toDs ? " ToDS" : "") +
        String(record.fromDs ? " FromDS" : "") +
        String(record.retry ? " Retry" : "") +
        String(record.moreFragments ? " MoreFrag" : "") +
        String(record.protectedFrame ? " Protected" : ""));
    if (record.etherType != 0) {
        char etherType[24];
        snprintf(etherType, sizeof(etherType), "EtherType: 0x%04X",
                 record.etherType);
        add(etherType);
    }
    if (record.ipVersion != 0) {
        add("IP: v" + String(record.ipVersion) + " TTL/Hop " +
            String(record.ttl) + " protocol " +
            String(record.ipProtocol));
    }
    if (record.sourcePort != 0 || record.destinationPort != 0) {
        add("Ports: " + String(record.sourcePort) + " -> " +
            String(record.destinationPort));
    }
    if (record.ipProtocol == 6) {
        char flags[12] = {};
        formatTcpFlags(record.tcpFlags, flags, sizeof(flags));
        add("TCP: flags " + String(flags) + " window " +
            String(record.tcpWindow));
        add("TCP Seq: " + String(record.tcpSequence));
        add("TCP Ack: " + String(record.tcpAcknowledgment));
    }
    if (record.icmpType != 0 || record.icmpCode != 0) {
        add("ICMP: type " + String(record.icmpType) + " code " +
            String(record.icmpCode));
    }
    if (record.dnsName[0]) {
        count = addWrappedLine(lines, count, capacity, "DNS: ",
                               String(dnsTypeName(record.dnsType)) + " " +
                                   record.dnsName);
    }
    if (record.ssid[0]) {
        count = addWrappedLine(lines, count, capacity, "SSID: ",
                               record.ssid);
    }
    count = addWrappedLine(lines, count, capacity, "Info: ",
                           record.info);

    char raw[RAW_PREVIEW_SIZE * 3 + 16] = "Raw: ";
    size_t used = strlen(raw);
    const size_t preview =
        record.rawPreviewLength < 16 ? record.rawPreviewLength : 16;
    for (size_t i = 0; i < preview && used + 4 < sizeof(raw); ++i) {
        used += snprintf(raw + used, sizeof(raw) - used, "%02X ",
                         record.rawPreview[i]);
    }
    add(raw);
    return count;
}

void drawDetails() {
    drawChrome(";/. packet ,// scroll Enter back");
    PacketRecord* record = selectedRecord();
    if (!record) {
        LB::setTextColor(TFT_YELLOW);
        LB::setCursor(5, 28);
        LB::print("No packet selected");
        return;
    }

    String lines[40];
    const int lineCount = buildDetailLines(*record, lines, 40);
    const int visibleLines = (LB::height() - 24) / 10;
    int maxScroll = lineCount - visibleLines;
    if (maxScroll < 0) maxScroll = 0;
    if (g_detailScroll > maxScroll) g_detailScroll = maxScroll;
    if (g_detailScroll < 0) g_detailScroll = 0;

    LB::setTextColor(TFT_WHITE);
    for (int i = 0; i < visibleLines &&
                    g_detailScroll + i < lineCount; ++i) {
        LB::setCursor(2, 14 + i * 10);
        LB::print(lines[g_detailScroll + i]);
    }
}

void drawStats() {
    drawChrome("Tab next R rec C clear Del exit");
    const uint32_t dropped =
        __atomic_load_n(&g_captureDropped, __ATOMIC_RELAXED);
    String lines[16];
    int count = 0;
    lines[count++] = "Packets: " + String(static_cast<uint32_t>(
                                      g_stats.totalPackets)) +
                     "  dropped: " + String(dropped);
    lines[count++] = "Traffic: " + formatBytes(g_stats.totalBytes) +
                     "  rate: " + String(g_stats.packetsPerSecond) + " p/s";
    lines[count++] = "802.11 MGMT " +
                     String(static_cast<uint32_t>(g_stats.managementFrames)) +
                     " DATA " +
                     String(static_cast<uint32_t>(g_stats.dataFrames)) +
                     " CTRL " +
                     String(static_cast<uint32_t>(g_stats.controlFrames));
    lines[count++] = "TCP " +
                     String(static_cast<uint32_t>(g_stats.tcpPackets)) +
                     " UDP " +
                     String(static_cast<uint32_t>(g_stats.udpPackets)) +
                     " DNS " +
                     String(static_cast<uint32_t>(g_stats.dnsPackets));
    lines[count++] = "ARP " +
                     String(static_cast<uint32_t>(g_stats.arpPackets)) +
                     " ICMP " +
                     String(static_cast<uint32_t>(g_stats.icmpPackets));
    lines[count++] = "Protected " +
                     String(static_cast<uint32_t>(g_stats.protectedFrames)) +
                     " Retry " +
                     String(static_cast<uint32_t>(g_stats.retryFrames));
    lines[count++] = "History: " + String(g_historyCount) + "/" +
                     String(g_historyCapacity);
    lines[count++] = String("PCAP: ") +
                     (g_pcap.active() ? "recording" : "stopped");
    lines[count++] = "PCAP packets: " +
                     String(static_cast<uint32_t>(g_pcap.packets())) +
                     "  " + formatBytes(g_pcap.bytes());
    if (g_pcap.path().length()) {
        String name = g_pcap.path();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        lines[count++] = "File: " + name;
    }
    if (g_pcap.error().length()) {
        lines[count++] = "SD error: " + g_pcap.error();
    }

    LB::setTextColor(TFT_WHITE);
    const int visibleLines = (LB::height() - 24) / 10;
    for (int i = 0; i < count && i < visibleLines; ++i) {
        LB::setCursor(2, 14 + i * 10);
        LB::print(lines[i]);
    }
}

void drawRadio() {
    drawChrome("Tab next ,// channel when offline");
    const int averageRssi =
        g_stats.rssiSamples
            ? static_cast<int>(g_stats.rssiTotal /
                               static_cast<int64_t>(g_stats.rssiSamples))
            : 0;
    String lines[20];
    int count = 0;
    lines[count++] = "Channel: " + String(g_currentChannel) +
                     (WiFi.status() == WL_CONNECTED ? " (STA locked)"
                                                   : " (manual)");
    lines[count++] = "RSSI average: " + String(averageRssi) + " dBm";
    lines[count++] = "RSSI >=-50: " + String(g_stats.rssiStrong);
    lines[count++] = "RSSI -51..-65: " + String(g_stats.rssiGood);
    lines[count++] = "RSSI -66..-80: " + String(g_stats.rssiWeak);
    lines[count++] = "RSSI <-80: " + String(g_stats.rssiPoor);
    lines[count++] = "Protected: " +
                     String(static_cast<uint32_t>(g_stats.protectedFrames));
    lines[count++] = "Retries: " +
                     String(static_cast<uint32_t>(g_stats.retryFrames));
    for (uint8_t channel = 1; channel <= 14; ++channel) {
        if (g_stats.channelPackets[channel] != 0) {
            lines[count++] = "CH" + String(channel) + ": " +
                             String(g_stats.channelPackets[channel]);
        }
    }

    LB::setTextColor(TFT_WHITE);
    const int visibleLines = (LB::height() - 24) / 10;
    for (int i = 0; i < count && i < visibleLines; ++i) {
        LB::setCursor(2, 14 + i * 10);
        LB::print(lines[i]);
    }
}

void drawScreen() {
    switch (g_view) {
        case ViewMode::LIVE: drawLive(); break;
        case ViewMode::DETAILS: drawDetails(); break;
        case ViewMode::STATS: drawStats(); break;
        case ViewMode::RADIO: drawRadio(); break;
    }
    LB::display();
    g_screenDirty = false;
}

void moveSelection(int direction) {
    uint16_t indices[HISTORY_CAPACITY_PSRAM];
    const size_t count = collectVisible(indices, HISTORY_CAPACITY_PSRAM);
    if (count == 0) return;
    if (g_followLatest || g_selectedVisible < 0) {
        g_selectedVisible = static_cast<int>(count) - 1;
    }
    g_followLatest = false;
    g_selectedVisible += direction;
    if (g_selectedVisible < 0) g_selectedVisible = 0;
    if (g_selectedVisible >= static_cast<int>(count)) {
        g_selectedVisible = static_cast<int>(count) - 1;
    }
    g_detailScroll = 0;
    g_screenDirty = true;
}

void cycleFilter() {
    const uint8_t next =
        (static_cast<uint8_t>(g_filter) + 1) %
        static_cast<uint8_t>(DisplayFilter::COUNT);
    g_filter = static_cast<DisplayFilter>(next);
    g_followLatest = true;
    g_selectedVisible = -1;
    g_detailScroll = 0;
    setNotice(String("Filter: ") + filterName());
}

void cycleView() {
    if (g_view == ViewMode::DETAILS) {
        g_view = ViewMode::LIVE;
    } else if (g_view == ViewMode::LIVE) {
        g_view = ViewMode::STATS;
    } else if (g_view == ViewMode::STATS) {
        g_view = ViewMode::RADIO;
    } else {
        g_view = ViewMode::LIVE;
    }
    g_screenDirty = true;
}

void toggleRecording() {
    if (g_pcap.active()) {
        g_pcap.stop();
        setNotice("PCAP stopped");
        return;
    }
    if (g_pcap.start(g_currentChannel)) {
        setNotice("PCAP recording");
    } else {
        setNotice(String("PCAP: ") + g_pcap.error(), 2500);
    }
}

void changeChannel(int direction) {
    if (WiFi.status() == WL_CONNECTED) {
        setNotice("Channel locked by connected STA");
        return;
    }
    int channel = static_cast<int>(g_currentChannel) + direction;
    if (channel < 1) channel = 13;
    if (channel > 13) channel = 1;
    const esp_err_t rc =
        esp_wifi_set_channel(static_cast<uint8_t>(channel),
                             WIFI_SECOND_CHAN_NONE);
    if (rc == ESP_OK) {
        g_currentChannel = static_cast<uint8_t>(channel);
        setNotice("Channel " + String(channel));
    } else {
        setNotice(String("Channel error: ") + esp_err_to_name(rc));
    }
}

void showFatal(const String& message) {
    LB::fillScreen(TFT_BLACK);
    LB::setTextSize(1);
    LB::setTextColor(TFT_RED);
    LB::setCursor(5, 12);
    LB::println("Network Inspector");
    LB::setTextColor(TFT_WHITE);
    LB::setCursor(5, 30);
    LB::println(message);
    LB::display();
    delay(2200);
}

}  // namespace

void run() {
    // Main-flash erase/program stalls both S3 cores. Keep background pager GC
    // outside promiscuous capture so it cannot create deterministic packet
    // loss bursts. The live capture ring itself always remains in SRAM.
    FlashPager::CriticalGuard pagerGuard;
    InspectorStats runtimeStats = {};
    g_statsStorage = &runtimeStats;
    inMenu = false;
    g_view = ViewMode::LIVE;
    g_filter = DisplayFilter::ALL;
    g_followLatest = true;
    g_selectedVisible = -1;
    g_detailScroll = 0;
    g_notice = "";
    g_noticeUntilMs = 0;
    g_screenDirty = true;

    String error;
    if (!allocateBuffers(error)) {
        showFatal(error);
        g_statsStorage = nullptr;
        inMenu = true;
        return;
    }

    NetCore::begin();
    NetCore::poll();
    if (WiFi.getMode() == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
        delay(100);
    }

    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&g_currentChannel, &secondary) != ESP_OK ||
        g_currentChannel == 0) {
        g_currentChannel = 1;
        esp_wifi_set_channel(g_currentChannel, WIFI_SECOND_CHAN_NONE);
    }

    const uint32_t filterMask =
        WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
        WIFI_PROMIS_FILTER_MASK_CTRL;
    if (!NetCore::acquirePromiscuous(CAPTURE_OWNER, captureCallback,
                                     filterMask, &error)) {
        showFatal(error);
        releaseBuffers();
        g_statsStorage = nullptr;
        inMenu = true;
        return;
    }

    NetCore::ensureTimeSync();
    if (!g_pcap.start(g_currentChannel)) {
        setNotice(String("Live only: ") + g_pcap.error(), 2500);
    } else {
        setNotice("PCAP recording");
    }

    LB::setTextSize(1);
    uint32_t lastDrawMs = 0;
    uint32_t lastInputMs = 0;
    bool running = true;

    while (running) {
        M5.update();
        M5Cardputer.update();
        NetCore::poll();

        processCaptured(PROCESS_BUDGET);
        updatePps();
        if (!g_pcap.periodicFlush()) {
            setNotice(String("PCAP stopped: ") + g_pcap.error(), 2500);
        }

        const uint32_t now = millis();
        if (g_notice.length() > 0 &&
            static_cast<int32_t>(g_noticeUntilMs - now) <= 0) {
            g_notice = "";
            g_screenDirty = true;
        }
        if (g_screenDirty &&
            static_cast<uint32_t>(now - lastDrawMs) >= UI_REFRESH_MS) {
            drawScreen();
            lastDrawMs = now;
        }

        if (InputCompat::isBackPressed()) {
            running = false;
            delay(120);
            continue;
        }

        if (static_cast<uint32_t>(now - lastInputMs) >= 120) {
            bool handled = false;
            if (M5Cardputer.Keyboard.isKeyPressed('\t')) {
                cycleView();
                handled = true;
            } else if (M5Cardputer.Keyboard.isKeyPressed('r')) {
                toggleRecording();
                handled = true;
            } else if (M5Cardputer.Keyboard.isKeyPressed('f')) {
                cycleFilter();
                handled = true;
            } else if (M5Cardputer.Keyboard.isKeyPressed('c')) {
                resetHistoryAndStats();
                setNotice("History cleared");
                handled = true;
            } else if (M5Cardputer.Keyboard.isKeyPressed(' ')) {
                g_followLatest = !g_followLatest;
                setNotice(g_followLatest ? "Following latest"
                                         : "Selection paused");
                handled = true;
            } else if (M5Cardputer.Keyboard.isKeyPressed(';')) {
                moveSelection(-1);
                handled = true;
            } else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
                moveSelection(1);
                handled = true;
            } else if (M5Cardputer.Keyboard.isKeyPressed(',')) {
                if (g_view == ViewMode::DETAILS) {
                    if (g_detailScroll > 0) --g_detailScroll;
                    g_screenDirty = true;
                } else {
                    changeChannel(-1);
                }
                handled = true;
            } else if (M5Cardputer.Keyboard.isKeyPressed('/')) {
                if (g_view == ViewMode::DETAILS) {
                    ++g_detailScroll;
                    g_screenDirty = true;
                } else {
                    changeChannel(1);
                }
                handled = true;
            } else if (InputCompat::isEnterPressed()) {
                if (g_view == ViewMode::LIVE && selectedRecord()) {
                    g_followLatest = false;
                    g_view = ViewMode::DETAILS;
                    g_detailScroll = 0;
                } else {
                    g_view = ViewMode::LIVE;
                }
                g_screenDirty = true;
                handled = true;
            }
            if (handled) lastInputMs = now;
        }
        delay(5);
    }

    NetCore::releasePromiscuous(CAPTURE_OWNER);
    while (processCaptured(g_captureCapacity) > 0) {
        delay(0);
    }
    g_pcap.stop();
    if (g_pcap.path().length()) {
        Serial.println(String("[NetworkInspector] PCAP: ") + g_pcap.path());
    }
    g_pcap.shutdown();
    releaseBuffers();
    g_statsStorage = nullptr;
    LB::setTextSize(1.5);
    inMenu = true;
}

}  // namespace NetworkInspector
