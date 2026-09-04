/*
 * netcore.h - Shared network services for Cardputer applications.
 *
 * NetCore centralizes lightweight network state, DNS caching, time anchoring,
 * connection telemetry, and exclusive ownership of ESP32 promiscuous mode.
 * It deliberately does not own WiFi credentials or force a WiFi mode.
 */

#ifndef NETCORE_H
#define NETCORE_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace NetCore {

constexpr size_t MAX_CONNECTIONS = 12;
constexpr size_t EVENT_QUEUE_SIZE = 32;

enum class EventType : uint8_t {
    WIFI_CONNECTED = 0,
    WIFI_DISCONNECTED,
    IP_CHANGED,
    DNS_RESOLVED,
    DNS_FAILED,
    CONNECTION_OPENED,
    CONNECTION_CLOSED,
    PROMISCUOUS_ACQUIRED,
    PROMISCUOUS_RELEASED,
    TIME_SYNCHRONIZED,
};

enum class ConnectionState : uint8_t {
    RESOLVING = 0,
    CONNECTING,
    CONNECTED,
    CLOSING,
    CLOSED,
    FAILED,
};

struct NetworkStatus {
    bool initialized;
    bool connected;
    bool timeSynchronized;
    wl_status_t wifiStatus;
    wifi_mode_t mode;
    int8_t rssi;
    uint8_t primaryChannel;
    wifi_second_chan_t secondaryChannel;
    IPAddress localIp;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns;
    char ssid[33];
    uint32_t updatedAtMs;
};

struct Event {
    EventType type;
    uint32_t timestampMs;
    int32_t code;
    char detail[64];
};

struct ConnectionInfo {
    bool used;
    uint32_t id;
    ConnectionState state;
    bool tls;
    uint16_t port;
    uint32_t openedAtMs;
    uint32_t lastActivityMs;
    uint64_t bytesSent;
    uint64_t bytesReceived;
    char owner[20];
    char host[64];
};

bool begin();
void poll();
NetworkStatus status();

bool resolveHost(const char* host, IPAddress& result,
                 uint32_t cacheTtlMs = 60000, String* error = nullptr);
void clearDnsCache();

bool ensureTimeSync();
bool timeSynchronized();
uint64_t monotonicMicros();
uint64_t unixMicrosForMonotonic(uint64_t monotonicUs);

bool acquirePromiscuous(const char* owner, wifi_promiscuous_cb_t callback,
                        uint32_t filterMask, String* error = nullptr);
void releasePromiscuous(const char* owner);
bool isPromiscuousOwnedBy(const char* owner);
String promiscuousOwner();

uint32_t registerConnection(const char* owner, const char* host, uint16_t port,
                            bool tls);
void setConnectionState(uint32_t id, ConnectionState state);
void addConnectionTraffic(uint32_t id, size_t sent, size_t received);
void closeConnection(uint32_t id, int32_t resultCode = 0);
size_t snapshotConnections(ConnectionInfo* output, size_t capacity);

bool nextEvent(Event& output);
void clearEvents();

bool isPrivateIpv4(const IPAddress& address);

}  // namespace NetCore

#endif  // NETCORE_H
