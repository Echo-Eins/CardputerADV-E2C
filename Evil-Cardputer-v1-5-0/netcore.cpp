/*
 * netcore.cpp - Shared, bounded network services.
 */

#include "netcore.h"
#include "runtime_memory.h"

#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <new>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

namespace NetCore {
namespace {

constexpr size_t DNS_CACHE_SIZE = 8;
constexpr size_t PROMISCUOUS_OWNER_SIZE = 24;
constexpr time_t MIN_VALID_UNIX_TIME = 1700000000;

struct DnsEntry {
    bool valid;
    uint32_t expiresAtMs;
    IPAddress address;
    char host[64];
};

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_initialized = false;
NetworkStatus* g_status = nullptr;
uint32_t g_lastPollMs = 0;

DnsEntry* g_dnsCache = nullptr;
uint8_t g_dnsReplaceIndex = 0;

Event* g_events = nullptr;
uint8_t g_eventRead = 0;
uint8_t g_eventWrite = 0;

ConnectionInfo* g_connections = nullptr;
uint32_t g_nextConnectionId = 1;

char g_promiscuousOwner[PROMISCUOUS_OWNER_SIZE] = {};
bool g_promiscuousReservation = false;

bool g_timeSyncRequested = false;
bool g_epochAnchorValid = false;
int64_t g_epochOffsetUs = 0;

template <typename T>
T* allocateRuntimeArray(
    size_t count,
    size_t reserve = RuntimeMemory::DEFAULT_INTERNAL_RESERVE) {
    void* memory = RuntimeMemory::allocatePreferred(sizeof(T) * count, false,
                                                     reserve);
    if (!memory) return nullptr;
    T* result = static_cast<T*>(memory);
    for (size_t i = 0; i < count; ++i) new (&result[i]) T();
    return result;
}

template <typename T>
void releaseRuntimeArray(T*& values, size_t count) {
    if (!values) return;
    for (size_t i = 0; i < count; ++i) values[i].~T();
    RuntimeMemory::release(values);
    values = nullptr;
}

bool ensureDnsStorage() {
    if (g_dnsCache) return true;
    DnsEntry* candidate = allocateRuntimeArray<DnsEntry>(DNS_CACHE_SIZE);
    if (!candidate) return false;

    portENTER_CRITICAL(&g_lock);
    if (!g_dnsCache) {
        g_dnsCache = candidate;
        candidate = nullptr;
    }
    portEXIT_CRITICAL(&g_lock);
    releaseRuntimeArray(candidate, DNS_CACHE_SIZE);
    return g_dnsCache != nullptr;
}

bool ensureEventStorage() {
    if (g_events) return true;
    Event* candidate = allocateRuntimeArray<Event>(EVENT_QUEUE_SIZE);
    if (!candidate) return false;

    portENTER_CRITICAL(&g_lock);
    if (!g_events) {
        g_events = candidate;
        g_eventRead = 0;
        g_eventWrite = 0;
        candidate = nullptr;
    }
    portEXIT_CRITICAL(&g_lock);
    releaseRuntimeArray(candidate, EVENT_QUEUE_SIZE);
    return g_events != nullptr;
}

bool ensureConnectionStorage() {
    if (g_connections) return true;
    ConnectionInfo* candidate =
        allocateRuntimeArray<ConnectionInfo>(MAX_CONNECTIONS);
    if (!candidate) return false;

    portENTER_CRITICAL(&g_lock);
    if (!g_connections) {
        g_connections = candidate;
        candidate = nullptr;
    }
    portEXIT_CRITICAL(&g_lock);
    releaseRuntimeArray(candidate, MAX_CONNECTIONS);
    return g_connections != nullptr;
}

void setError(String* error, const char* message) {
    if (error) *error = message ? message : "";
}

void emitEvent(EventType type, int32_t code, const char* detail) {
    if (!g_events) return;
    Event event = {};
    event.type = type;
    event.timestampMs = millis();
    event.code = code;
    snprintf(event.detail, sizeof(event.detail), "%s", detail ? detail : "");

    portENTER_CRITICAL(&g_lock);
    const uint8_t next =
        static_cast<uint8_t>((g_eventWrite + 1) % EVENT_QUEUE_SIZE);
    if (next == g_eventRead) {
        g_eventRead =
            static_cast<uint8_t>((g_eventRead + 1) % EVENT_QUEUE_SIZE);
    }
    g_events[g_eventWrite] = event;
    g_eventWrite = next;
    portEXIT_CRITICAL(&g_lock);
}

bool sameIp(const IPAddress& a, const IPAddress& b) {
    return static_cast<uint32_t>(a) == static_cast<uint32_t>(b);
}

void updateEpochAnchor() {
    timeval now = {};
    gettimeofday(&now, nullptr);
    if (now.tv_sec < MIN_VALID_UNIX_TIME) return;

    const int64_t epochUs =
        static_cast<int64_t>(now.tv_sec) * 1000000LL + now.tv_usec;
    const int64_t monotonicUs = esp_timer_get_time();
    bool firstSync = false;

    portENTER_CRITICAL(&g_lock);
    firstSync = !g_epochAnchorValid;
    g_epochOffsetUs = epochUs - monotonicUs;
    g_epochAnchorValid = true;
    if (g_status) g_status->timeSynchronized = true;
    portEXIT_CRITICAL(&g_lock);

    if (firstSync) {
        emitEvent(EventType::TIME_SYNCHRONIZED, 0, "SNTP time available");
    }
}

void refreshStatus() {
    NetworkStatus next = {};
    next.initialized = g_initialized;
    next.wifiStatus = WiFi.status();
    next.connected = next.wifiStatus == WL_CONNECTED;
    next.mode = WiFi.getMode();
    next.rssi = next.connected ? static_cast<int8_t>(WiFi.RSSI()) : 0;
    next.localIp = WiFi.localIP();
    next.gateway = WiFi.gatewayIP();
    next.subnet = WiFi.subnetMask();
    next.dns = WiFi.dnsIP();
    next.updatedAtMs = millis();

    String ssid = next.connected ? WiFi.SSID() : String();
    snprintf(next.ssid, sizeof(next.ssid), "%s", ssid.c_str());

    uint8_t primary = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &secondary) == ESP_OK) {
        next.primaryChannel = primary;
        next.secondaryChannel = secondary;
    }

    portENTER_CRITICAL(&g_lock);
    next.timeSynchronized = g_epochAnchorValid;
    const NetworkStatus previous = g_status ? *g_status : NetworkStatus();
    if (g_status) *g_status = next;
    if (!next.connected && !g_epochAnchorValid) g_timeSyncRequested = false;
    portEXIT_CRITICAL(&g_lock);

    if (!previous.connected && next.connected) {
        emitEvent(EventType::WIFI_CONNECTED, 0, next.ssid);
    } else if (previous.connected && !next.connected) {
        emitEvent(EventType::WIFI_DISCONNECTED,
                  static_cast<int32_t>(next.wifiStatus), "WiFi disconnected");
    } else if (next.connected && !sameIp(previous.localIp, next.localIp)) {
        const String ip = next.localIp.toString();
        emitEvent(EventType::IP_CHANGED, 0, ip.c_str());
    }
}

ConnectionInfo* findConnectionLocked(uint32_t id) {
    if (id == 0 || !g_connections) return nullptr;
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (g_connections[i].used && g_connections[i].id == id) {
            return &g_connections[i];
        }
    }
    return nullptr;
}

void clearPromiscuousReservation() {
    portENTER_CRITICAL(&g_lock);
    g_promiscuousReservation = false;
    g_promiscuousOwner[0] = '\0';
    portEXIT_CRITICAL(&g_lock);
}

}  // namespace

bool begin() {
    portENTER_CRITICAL(&g_lock);
    if (g_initialized) {
        portEXIT_CRITICAL(&g_lock);
        return true;
    }
    portEXIT_CRITICAL(&g_lock);

    NetworkStatus* networkStatus =
        allocateRuntimeArray<NetworkStatus>(1, 24U * 1024U);
    if (!networkStatus) return false;

    portENTER_CRITICAL(&g_lock);
    if (g_initialized) {
        portEXIT_CRITICAL(&g_lock);
        releaseRuntimeArray(networkStatus, 1);
        return true;
    }
    g_status = networkStatus;
    *g_status = {};
    g_status->initialized = true;
    g_initialized = true;
    portEXIT_CRITICAL(&g_lock);

    refreshStatus();
    updateEpochAnchor();
    return true;
}

void poll() {
    if (!g_initialized) begin();
    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - g_lastPollMs) < 500) return;
    g_lastPollMs = now;

    refreshStatus();
    updateEpochAnchor();

    const NetworkStatus snapshot = status();
    if (snapshot.connected && !snapshot.timeSynchronized) {
        ensureTimeSync();
    }
}

NetworkStatus status() {
    if (!g_initialized && !begin()) return NetworkStatus();
    NetworkStatus result = {};
    portENTER_CRITICAL(&g_lock);
    if (g_status) result = *g_status;
    portEXIT_CRITICAL(&g_lock);
    return result;
}

bool resolveHost(const char* host, IPAddress& result, uint32_t cacheTtlMs,
                 String* error) {
    if (!host || host[0] == '\0') {
        setError(error, "Empty hostname");
        return false;
    }
    if (!g_initialized && !begin()) {
        setError(error, "NetCore allocation failed");
        return false;
    }

    IPAddress numeric;
    if (numeric.fromString(host)) {
        result = numeric;
        if (error) *error = "";
        return true;
    }

    const bool cacheAvailable = ensureDnsStorage();
    const uint32_t now = millis();
    if (cacheAvailable) {
        portENTER_CRITICAL(&g_lock);
        for (size_t i = 0; i < DNS_CACHE_SIZE; ++i) {
            const DnsEntry& entry = g_dnsCache[i];
            if (entry.valid && strcasecmp(entry.host, host) == 0 &&
                static_cast<int32_t>(entry.expiresAtMs - now) > 0) {
                result = entry.address;
                portEXIT_CRITICAL(&g_lock);
                if (error) *error = "";
                return true;
            }
        }
        portEXIT_CRITICAL(&g_lock);
    }

    if (WiFi.status() != WL_CONNECTED) {
        setError(error, "WiFi is not connected");
        emitEvent(EventType::DNS_FAILED, WL_DISCONNECTED, host);
        return false;
    }

    IPAddress resolved;
    if (WiFi.hostByName(host, resolved) != 1) {
        setError(error, "DNS resolution failed");
        emitEvent(EventType::DNS_FAILED, -1, host);
        return false;
    }

    if (cacheAvailable) {
        portENTER_CRITICAL(&g_lock);
        DnsEntry& entry = g_dnsCache[g_dnsReplaceIndex];
        entry.valid = true;
        entry.address = resolved;
        entry.expiresAtMs = now + cacheTtlMs;
        snprintf(entry.host, sizeof(entry.host), "%s", host);
        g_dnsReplaceIndex =
            static_cast<uint8_t>((g_dnsReplaceIndex + 1) % DNS_CACHE_SIZE);
        portEXIT_CRITICAL(&g_lock);
    }

    result = resolved;
    const String detail = String(host) + "=" + resolved.toString();
    emitEvent(EventType::DNS_RESOLVED, 0, detail.c_str());
    if (error) *error = "";
    return true;
}

void clearDnsCache() {
    if (!g_dnsCache) return;
    portENTER_CRITICAL(&g_lock);
    for (size_t i = 0; i < DNS_CACHE_SIZE; ++i) {
        g_dnsCache[i].valid = false;
    }
    g_dnsReplaceIndex = 0;
    portEXIT_CRITICAL(&g_lock);
}

bool ensureTimeSync() {
    if (!g_initialized && !begin()) return false;
    updateEpochAnchor();
    if (timeSynchronized()) return true;
    if (WiFi.status() != WL_CONNECTED) return false;

    bool request = false;
    portENTER_CRITICAL(&g_lock);
    if (!g_timeSyncRequested) {
        g_timeSyncRequested = true;
        request = true;
    }
    portEXIT_CRITICAL(&g_lock);

    if (request) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    }
    return false;
}

bool timeSynchronized() {
    bool valid;
    portENTER_CRITICAL(&g_lock);
    valid = g_epochAnchorValid;
    portEXIT_CRITICAL(&g_lock);
    return valid;
}

uint64_t monotonicMicros() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

uint64_t unixMicrosForMonotonic(uint64_t monotonicUs) {
    bool valid;
    int64_t offset;
    portENTER_CRITICAL(&g_lock);
    valid = g_epochAnchorValid;
    offset = g_epochOffsetUs;
    portEXIT_CRITICAL(&g_lock);

    if (!valid) return monotonicUs;
    const int64_t epoch = static_cast<int64_t>(monotonicUs) + offset;
    return epoch > 0 ? static_cast<uint64_t>(epoch) : monotonicUs;
}

bool acquirePromiscuous(const char* owner, wifi_promiscuous_cb_t callback,
                        uint32_t filterMask, String* error) {
    if (!g_initialized && !begin()) {
        setError(error, "NetCore allocation failed");
        return false;
    }
    if (!owner || owner[0] == '\0' || !callback) {
        setError(error, "Invalid promiscuous owner or callback");
        return false;
    }

    portENTER_CRITICAL(&g_lock);
    if (g_promiscuousReservation) {
        char current[PROMISCUOUS_OWNER_SIZE];
        snprintf(current, sizeof(current), "%s", g_promiscuousOwner);
        portEXIT_CRITICAL(&g_lock);
        if (error) *error = String("Promiscuous mode owned by ") + current;
        return false;
    }
    g_promiscuousReservation = true;
    snprintf(g_promiscuousOwner, sizeof(g_promiscuousOwner), "%s", owner);
    portEXIT_CRITICAL(&g_lock);

    bool alreadyEnabled = false;
    esp_err_t rc = esp_wifi_get_promiscuous(&alreadyEnabled);
    if (rc != ESP_OK) {
        clearPromiscuousReservation();
        if (error) *error = String("WiFi not ready: ") + esp_err_to_name(rc);
        return false;
    }
    if (alreadyEnabled) {
        clearPromiscuousReservation();
        setError(error, "Promiscuous mode already active outside NetCore");
        return false;
    }

    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = filterMask;

    rc = esp_wifi_set_promiscuous_filter(&filter);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous_rx_cb(callback);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous(true);
    if (rc != ESP_OK) {
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        clearPromiscuousReservation();
        if (error) *error = String("Promiscuous start failed: ") +
                            esp_err_to_name(rc);
        return false;
    }

    emitEvent(EventType::PROMISCUOUS_ACQUIRED, 0, owner);
    if (error) *error = "";
    return true;
}

void releasePromiscuous(const char* owner) {
    if (!owner) return;

    bool matches = false;
    portENTER_CRITICAL(&g_lock);
    matches = g_promiscuousReservation &&
              strncmp(g_promiscuousOwner, owner,
                      sizeof(g_promiscuousOwner)) == 0;
    portEXIT_CRITICAL(&g_lock);
    if (!matches) return;

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    clearPromiscuousReservation();
    emitEvent(EventType::PROMISCUOUS_RELEASED, 0, owner);
}

bool isPromiscuousOwnedBy(const char* owner) {
    if (!owner) return false;
    bool matches;
    portENTER_CRITICAL(&g_lock);
    matches = g_promiscuousReservation &&
              strncmp(g_promiscuousOwner, owner,
                      sizeof(g_promiscuousOwner)) == 0;
    portEXIT_CRITICAL(&g_lock);
    return matches;
}

String promiscuousOwner() {
    char owner[PROMISCUOUS_OWNER_SIZE] = {};
    portENTER_CRITICAL(&g_lock);
    if (g_promiscuousReservation) {
        snprintf(owner, sizeof(owner), "%s", g_promiscuousOwner);
    }
    portEXIT_CRITICAL(&g_lock);
    return String(owner);
}

uint32_t registerConnection(const char* owner, const char* host, uint16_t port,
                            bool tls) {
    if (!g_initialized && !begin()) return 0;
    if (!ensureConnectionStorage()) return 0;
    ConnectionInfo created = {};
    bool success = false;

    portENTER_CRITICAL(&g_lock);
    ConnectionInfo* slot = nullptr;
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (!g_connections[i].used) {
            slot = &g_connections[i];
            break;
        }
    }
    if (!slot) {
        for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
            if (g_connections[i].state == ConnectionState::CLOSED ||
                g_connections[i].state == ConnectionState::FAILED) {
                slot = &g_connections[i];
                break;
            }
        }
    }

    if (slot) {
        *slot = {};
        slot->used = true;
        slot->id = g_nextConnectionId++;
        if (g_nextConnectionId == 0) g_nextConnectionId = 1;
        slot->state = ConnectionState::RESOLVING;
        slot->tls = tls;
        slot->port = port;
        slot->openedAtMs = millis();
        slot->lastActivityMs = slot->openedAtMs;
        snprintf(slot->owner, sizeof(slot->owner), "%s",
                 owner ? owner : "unknown");
        snprintf(slot->host, sizeof(slot->host), "%s", host ? host : "");
        created = *slot;
        success = true;
    }
    portEXIT_CRITICAL(&g_lock);

    if (!success) return 0;
    emitEvent(EventType::CONNECTION_OPENED,
              static_cast<int32_t>(created.id), created.host);
    return created.id;
}

void setConnectionState(uint32_t id, ConnectionState state) {
    portENTER_CRITICAL(&g_lock);
    ConnectionInfo* connection = findConnectionLocked(id);
    if (connection) {
        connection->state = state;
        connection->lastActivityMs = millis();
    }
    portEXIT_CRITICAL(&g_lock);
}

void addConnectionTraffic(uint32_t id, size_t sent, size_t received) {
    portENTER_CRITICAL(&g_lock);
    ConnectionInfo* connection = findConnectionLocked(id);
    if (connection) {
        connection->bytesSent += sent;
        connection->bytesReceived += received;
        connection->lastActivityMs = millis();
    }
    portEXIT_CRITICAL(&g_lock);
}

void closeConnection(uint32_t id, int32_t resultCode) {
    char detail[64] = {};
    bool found = false;
    portENTER_CRITICAL(&g_lock);
    ConnectionInfo* connection = findConnectionLocked(id);
    if (connection) {
        connection->state =
            resultCode == 0 ? ConnectionState::CLOSED
                            : ConnectionState::FAILED;
        connection->lastActivityMs = millis();
        snprintf(detail, sizeof(detail), "%s", connection->host);
        found = true;
    }
    portEXIT_CRITICAL(&g_lock);
    if (found) {
        emitEvent(EventType::CONNECTION_CLOSED, resultCode, detail);
    }
}

size_t snapshotConnections(ConnectionInfo* output, size_t capacity) {
    if (!output || capacity == 0 || !g_connections) return 0;
    size_t count = 0;
    portENTER_CRITICAL(&g_lock);
    for (size_t i = 0; i < MAX_CONNECTIONS && count < capacity; ++i) {
        if (g_connections[i].used) output[count++] = g_connections[i];
    }
    portEXIT_CRITICAL(&g_lock);
    return count;
}

bool nextEvent(Event& output) {
    if (!g_events && !ensureEventStorage()) return false;
    bool available = false;
    portENTER_CRITICAL(&g_lock);
    if (g_eventRead != g_eventWrite) {
        output = g_events[g_eventRead];
        g_eventRead =
            static_cast<uint8_t>((g_eventRead + 1) % EVENT_QUEUE_SIZE);
        available = true;
    }
    portEXIT_CRITICAL(&g_lock);
    return available;
}

void clearEvents() {
    if (!g_events) return;
    portENTER_CRITICAL(&g_lock);
    g_eventRead = g_eventWrite;
    portEXIT_CRITICAL(&g_lock);
}

bool isPrivateIpv4(const IPAddress& address) {
    const uint8_t a = address[0];
    const uint8_t b = address[1];
    return a == 10 || (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168) ||
           (a == 100 && b >= 64 && b <= 127) || a == 127 ||
           (a == 169 && b == 254);
}

}  // namespace NetCore
