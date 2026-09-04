/*
 * web_reader.cpp - Streaming text-mode Web Reader for Cardputer ADV.
 *
 * Deliberately unsupported: JavaScript, CSS layout, images, video and forms.
 * HTTP redirects and chunked transfer are supported. Servers are asked for
 * identity encoding so that no full compressed document is retained in RAM.
 */

#include "web_reader.h"
#include "gui/gui.h"

#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <new>
#include <time.h>

#include "display_runtime.h"
#include "file_editor.h"
#include "flash_pager.h"
#include "input_compat.h"
#include "netcore.h"
#include "network_transport.h"
#include "runtime_memory.h"
#include "scroll_input.h"

extern String getUserInput(bool isPassword);
extern bool inMenu;

namespace WebReader {
namespace {

constexpr const char* CONFIG_PATH = "/evil/config/web_reader.json";
constexpr const char* HISTORY_PATH = "/evil/web/history.json";
constexpr const char* BOOKMARK_PATH = "/evil/web/bookmarks.json";
constexpr const char* SAVED_ROOT = "/evil/web/saved";
constexpr size_t MAX_LINKS = 64;
constexpr size_t MAX_HISTORY = 32;
constexpr size_t MAX_BOOKMARKS = 24;
constexpr size_t MAX_BACK_STACK = 12;

struct WebConfig {
    String home = "https://example.com/";
    String search =
        "https://lite.duckduckgo.com/lite/?q={query}";
    String userAgent = "CardputerADV-WebReader/1.0";
    bool tlsInsecure = true;
    String caCertPath;
    size_t maxPageBytes = 196608;
};

struct UrlParts {
    bool tls = true;
    String scheme;
    String host;
    uint16_t port = 443;
    String path;
};

struct WebLink {
    char text[80] = {};
    char href[224] = {};
};

struct LineSpan {
    uint32_t start;
    uint16_t length;
};

struct WebEntry {
    String title;
    String url;
};

struct Page {
    FlashPager::Object textStore;
    uint8_t* ramStore = nullptr;
    size_t length = 0;
    size_t capacity = 0;
    LineSpan* lines = nullptr;
    size_t lineCount = 0;
    size_t lineCapacity = 0;
    WebLink* links = nullptr;
    size_t linkCount = 0;
    size_t linkCapacity = 0;
    String title;
    String url;
    String contentType;
    int statusCode = 0;
    size_t wireBytes = 0;
    bool truncated = false;
    bool storageError = false;
    bool usingRam = false;
    String storageNotice;
    String allocationError;
    uint8_t writeBuffer[512] = {};
    size_t writeStart = 0;
    size_t writeLength = 0;
    uint8_t readCache[512] = {};
    size_t readCacheStart = 0;
    size_t readCacheLength = 0;

    bool allocateProfile(size_t lineSlots, size_t linkSlots, bool external,
                         size_t internalReserve = 48U * 1024U) {
        const size_t lineBytes = lineSlots * sizeof(LineSpan);
        const size_t linkBytes = linkSlots * sizeof(WebLink);
        LineSpan* newLines = nullptr;
        WebLink* newLinks = nullptr;

        if (external) {
            newLines = static_cast<LineSpan*>(
                RuntimeMemory::allocateExternal(lineBytes));
            newLinks = static_cast<WebLink*>(
                RuntimeMemory::allocateExternal(linkBytes, true));
        } else {
            newLines = static_cast<LineSpan*>(RuntimeMemory::allocateInternal(
                lineBytes, false, internalReserve + linkBytes));
            if (newLines) {
                newLinks = static_cast<WebLink*>(
                    RuntimeMemory::allocateInternal(linkBytes, true,
                                                    internalReserve));
            }
        }

        if (!newLines || !newLinks) {
            RuntimeMemory::release(newLines);
            RuntimeMemory::release(newLinks);
            return false;
        }
        lines = newLines;
        links = newLinks;
        lineCapacity = lineSlots;
        linkCapacity = linkSlots;
        clear();
        return true;
    }

    String pagerFailure(const char* stage) {
        const FlashPager::Status status = FlashPager::lastStatus();
        String reason(stage);
        reason += ": ";
        reason += FlashPager::statusText(status);
        if (status == FlashPager::Status::PartitionNotFound)
            reason += " (flash merged.bin at 0x0 once)";
        return reason;
    }

    bool allocateRamFallback(size_t requested, const String& pagerReason) {
        static constexpr size_t cacheSizes[] = {
            24U * 1024U, 16U * 1024U, 12U * 1024U,
            8U * 1024U, 4U * 1024U};
        const struct {
            size_t lineSlots;
            size_t linkSlots;
        } profiles[] = {{512, 8}, {256, 4}};

        usingRam = true;
        size_t previousSize = 0;
        for (const size_t candidate : cacheSizes) {
            size_t amount = requested < candidate ? requested : candidate;
            if (amount < 4096) amount = 4096;
            if (amount == previousSize) continue;
            previousSize = amount;

            ramStore = static_cast<uint8_t*>(RuntimeMemory::allocateInternal(
                amount, false, 28U * 1024U));
            if (!ramStore) continue;
            capacity = amount;

            for (const auto& profile : profiles) {
                if (allocateProfile(profile.lineSlots, profile.linkSlots,
                                    false, 20U * 1024U)) {
                    storageNotice = pagerReason + "; RAM page cache " +
                                    String(capacity / 1024U) + " KB";
                    allocationError = "";
                    clear();
                    Serial.printf("[WebReader] %s\n", storageNotice.c_str());
                    return !storageError;
                }
            }

            RuntimeMemory::release(ramStore);
            ramStore = nullptr;
            capacity = 0;
        }

        usingRam = false;
        allocationError = pagerReason + "; RAM fallback failed; " +
                          RuntimeMemory::describe();
        return false;
    }

    bool allocate(size_t requested) {
        release();
        if (requested < 4096) requested = 4096;
        if (!FlashPager::begin())
            return allocateRamFallback(requested,
                                       pagerFailure("pager initialization"));
        if (!textStore.open("web-reader", "current-page",
                            FlashPager::StorageClass::Cache, requested))
            return allocateRamFallback(requested,
                                       pagerFailure("page store open"));
        capacity = textStore.capacity();

        const struct {
            size_t lineSlots;
            size_t linkSlots;
        } profiles[] = {{1024, 16}, {768, 12}, {512, 8}, {256, 4}};
        for (const auto& profile : profiles) {
            if (allocateProfile(profile.lineSlots, profile.linkSlots,
                                false)) {
                return true;
            }
        }
        textStore.close(false);
        capacity = 0;
        allocationError = String("page index allocation failed; ") +
                          RuntimeMemory::describe();
        return false;
    }

    void release() {
        if (textStore.isOpen()) textStore.close(false);
        RuntimeMemory::release(ramStore);
        RuntimeMemory::release(lines);
        RuntimeMemory::release(links);
        ramStore = nullptr;
        lines = nullptr;
        links = nullptr;
        capacity = 0;
        lineCapacity = 0;
        linkCapacity = 0;
        usingRam = false;
        storageNotice = "";
        allocationError = "";
        clearMetadata();
    }

    void clearMetadata() {
        length = 0;
        lineCount = 0;
        linkCount = 0;
        title = "";
        url = "";
        contentType = "";
        statusCode = 0;
        wireBytes = 0;
        truncated = false;
        storageError = false;
        writeStart = 0;
        writeLength = 0;
        readCacheLength = 0;
    }

    void clear() {
        clearMetadata();
        if (usingRam) {
            storageError = ramStore == nullptr || capacity == 0;
            return;
        }
        if (textStore.isOpen() && textStore.inTransaction() &&
            !textStore.rollback())
            storageError = true;
        if (!storageError &&
            (!textStore.isOpen() || !textStore.beginTransaction() ||
             !textStore.truncate(0)))
            storageError = true;
        if (links) memset(links, 0, linkCapacity * sizeof(WebLink));
    }

    bool flushWrites() {
        if (storageError) return false;
        if (!writeLength) return true;
        if (usingRam) {
            if (!ramStore || writeStart > capacity ||
                writeLength > capacity - writeStart) {
                storageError = true;
                return false;
            }
            memcpy(ramStore + writeStart, writeBuffer, writeLength);
        } else if (textStore.write(writeStart, writeBuffer, writeLength) !=
                   writeLength) {
            storageError = true;
            return false;
        }
        writeStart += writeLength;
        writeLength = 0;
        readCacheLength = 0;
        return true;
    }

    void append(char c) {
        if (storageError || length >= capacity) {
            truncated = true;
            return;
        }
        if (!writeLength) writeStart = length;
        if (writeLength == sizeof(writeBuffer) && !flushWrites()) {
            truncated = true;
            return;
        }
        if (!writeLength) writeStart = length;
        writeBuffer[writeLength++] = static_cast<uint8_t>(c);
        ++length;
    }

    void append(const char* value) {
        if (!value) return;
        while (*value) append(*value++);
    }

    bool readAt(size_t offset, void* output, size_t amount) {
        if (!output || offset > length || amount > length - offset)
            return false;
        uint8_t* destination = static_cast<uint8_t*>(output);
        size_t completed = 0;
        while (completed < amount) {
            const size_t position = offset + completed;
            if (writeLength && position >= writeStart) {
                size_t part = writeStart + writeLength - position;
                if (part > amount - completed) part = amount - completed;
                memcpy(destination + completed,
                       writeBuffer + position - writeStart, part);
                completed += part;
                continue;
            }
            size_t part = amount - completed;
            if (writeLength && position < writeStart &&
                part > writeStart - position)
                part = writeStart - position;
            size_t read = part;
            if (usingRam) {
                if (!ramStore) return false;
                memcpy(destination + completed, ramStore + position, part);
            } else {
                read = textStore.read(position, destination + completed, part);
            }
            if (read != part) return false;
            completed += part;
        }
        return true;
    }

    char charAt(size_t offset) {
        if (offset >= length) return '\0';
        if (writeLength && offset >= writeStart &&
            offset < writeStart + writeLength)
            return static_cast<char>(writeBuffer[offset - writeStart]);
        if (offset < readCacheStart ||
            offset >= readCacheStart + readCacheLength) {
            readCacheStart = (offset / sizeof(readCache)) * sizeof(readCache);
            readCacheLength = length - readCacheStart;
            if (readCacheLength > sizeof(readCache))
                readCacheLength = sizeof(readCache);
            if (!readAt(readCacheStart, readCache, readCacheLength)) {
                storageError = true;
                readCacheLength = 0;
                return '\0';
            }
        }
        return static_cast<char>(readCache[offset - readCacheStart]);
    }

    char lastChar() { return length ? charAt(length - 1) : '\0'; }

    void shrink(size_t newLength) {
        if (newLength >= length) return;
        length = newLength;
        if (writeLength) {
            if (newLength <= writeStart) {
                writeStart = newLength;
                writeLength = 0;
            } else if (newLength < writeStart + writeLength) {
                writeLength = newLength - writeStart;
            }
        }
        readCacheLength = 0;
    }

    bool seal() {
        if (usingRam) {
            if (!flushWrites()) {
                storageError = true;
                return false;
            }
            readCacheLength = 0;
            return true;
        }
        if (!flushWrites() || !textStore.truncate(length) ||
            !textStore.commit()) {
            storageError = true;
            return false;
        }
        readCacheLength = 0;
        return true;
    }

    void buildLines(size_t width) {
        lineCount = 0;
        if (width < 4 || !seal()) return;
        size_t cursor = 0;
        while (cursor < length && lineCount < lineCapacity) {
            while (cursor < length && charAt(cursor) == '\r') ++cursor;
            if (cursor < length && charAt(cursor) == '\n') {
                lines[lineCount++] = {
                    static_cast<uint32_t>(cursor), static_cast<uint16_t>(0)};
                ++cursor;
                continue;
            }
            const size_t start = cursor;
            size_t end = cursor;
            size_t lastSpace = static_cast<size_t>(-1);
            while (end < length && charAt(end) != '\n' &&
                   end - start < width) {
                const char current = charAt(end);
                if (current == ' ' || current == '\t') lastSpace = end;
                ++end;
            }
            if (end < length && charAt(end) != '\n' &&
                end - start >= width && lastSpace != static_cast<size_t>(-1) &&
                lastSpace > start) {
                end = lastSpace;
            }
            size_t shownEnd = end;
            while (shownEnd > start &&
                   (charAt(shownEnd - 1) == ' ' ||
                    charAt(shownEnd - 1) == '\t'))
                --shownEnd;
            const size_t span = shownEnd - start;
            lines[lineCount++] = {
                static_cast<uint32_t>(start),
                static_cast<uint16_t>(span > 65535 ? 65535 : span)};
            cursor = end;
            if (cursor < length && charAt(cursor) == '\n') ++cursor;
            while (cursor < length &&
                   (charAt(cursor) == ' ' || charAt(cursor) == '\t'))
                ++cursor;
        }
        if (cursor < length) truncated = true;
    }
};

enum class UiEvent : uint8_t {
    NONE,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    ENTER,
    EXIT,
    GO,
    SEARCH,
    BACK,
    HOME,
    RELOAD,
    LINKS,
    BOOKMARK,
    SAVE,
};

struct InputState {
    bool states[13] = {};
    bool save = false;

    UiEvent poll() {
        M5.update();
        M5Cardputer.update();
        ScrollInput::poll();
        const ScrollEvent wheel = ScrollInput::getMenuEvent();
        const bool current[] = {
            M5Cardputer.Keyboard.isKeyPressed(';'),
            M5Cardputer.Keyboard.isKeyPressed('.'),
            M5Cardputer.Keyboard.isKeyPressed(','),
            M5Cardputer.Keyboard.isKeyPressed('/'),
            InputCompat::isEnterPressed(),
            InputCompat::isBackPressed(),
            M5Cardputer.Keyboard.isKeyPressed('g'),
            M5Cardputer.Keyboard.isKeyPressed('s'),
            M5Cardputer.Keyboard.isKeyPressed('b'),
            M5Cardputer.Keyboard.isKeyPressed('h'),
            M5Cardputer.Keyboard.isKeyPressed('r'),
            M5Cardputer.Keyboard.isKeyPressed('l'),
            M5Cardputer.Keyboard.isKeyPressed('m')};
        const bool currentSave = M5Cardputer.Keyboard.isKeyPressed('p');
        UiEvent result = UiEvent::NONE;
        if ((current[0] && !states[0]) || wheel == ScrollEvent::ScrollUp)
            result = UiEvent::UP;
        else if ((current[1] && !states[1]) ||
                 wheel == ScrollEvent::ScrollDown)
            result = UiEvent::DOWN;
        else if (current[2] && !states[2])
            result = UiEvent::LEFT;
        else if (current[3] && !states[3])
            result = UiEvent::RIGHT;
        else if (current[4] && !states[4])
            result = UiEvent::ENTER;
        else if (current[5] && !states[5])
            result = UiEvent::EXIT;
        else if (current[6] && !states[6])
            result = UiEvent::GO;
        else if (current[7] && !states[7])
            result = UiEvent::SEARCH;
        else if (current[8] && !states[8])
            result = UiEvent::BACK;
        else if (current[9] && !states[9])
            result = UiEvent::HOME;
        else if (current[10] && !states[10])
            result = UiEvent::RELOAD;
        else if (current[11] && !states[11])
            result = UiEvent::LINKS;
        else if (current[12] && !states[12])
            result = UiEvent::BOOKMARK;
        for (size_t i = 0; i < 13; ++i) states[i] = current[i];
        if (currentSave && !save) result = UiEvent::SAVE;
        save = currentSave;
        return result;
    }
};

struct WebState {
    Page page;
    WebConfig config;
    WebEntry* history = nullptr;
    size_t historyCount = 0;
    WebEntry* bookmarks = nullptr;
    size_t bookmarkCount = 0;
    String* backStack = nullptr;
    size_t backCount = 0;
};

WebState* g_state = nullptr;
#define g_page (g_state->page)
#define g_config (g_state->config)
#define g_history (g_state->history)
#define g_historyCount (g_state->historyCount)
#define g_bookmarks (g_state->bookmarks)
#define g_bookmarkCount (g_state->bookmarkCount)
#define g_backStack (g_state->backStack)
#define g_backCount (g_state->backCount)

lgfx::LGFX_Device& display() {
    return GUI::runtimeDisplay();
}

void drawTitle(const String& title, uint16_t color = TFT_CYAN) {
    lgfx::LGFX_Device& d = display();
    d.fillScreen(TFT_BLACK);
    d.setTextSize(1);
    d.setTextWrap(false);
    d.fillRect(0, 0, d.width(), 14, TFT_DARKGREY);
    d.setTextColor(color, TFT_DARKGREY);
    d.setCursor(3, 3);
    String shown = title;
    const int maxChars = (d.width() - 6) / 6;
    if (shown.length() > static_cast<size_t>(maxChars))
        shown = shown.substring(0, maxChars);
    d.print(shown);
}

void drawFooter(const String& text) {
    lgfx::LGFX_Device& d = display();
    const int y = d.height() - 12;
    d.fillRect(0, y, d.width(), 12, TFT_DARKGREY);
    d.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
    d.setCursor(3, y + 2);
    String shown = text;
    const int maxChars = (d.width() - 6) / 6;
    if (shown.length() > static_cast<size_t>(maxChars))
        shown = shown.substring(0, maxChars);
    d.print(shown);
}

void showStatus(const String& title, const String& message,
                uint16_t color = TFT_WHITE) {
    drawTitle(title);
    lgfx::LGFX_Device& d = display();
    d.setTextWrap(true);
    d.setTextColor(color, TFT_BLACK);
    d.setCursor(5, 24);
    d.print(message);
    d.setTextWrap(false);
}

void modal(const String& title, const String& message,
           uint16_t color = TFT_WHITE) {
    showStatus(title, message, color);
    drawFooter("Enter / FN+Del");
    InputState input;
    while (true) {
        UiEvent event = input.poll();
        if (event == UiEvent::ENTER || event == UiEvent::EXIT) break;
        delay(10);
    }
}

bool loadConfig(WebConfig& config, String& error) {
    String json;
    bool truncated = false;
    if (!NetworkTransport::readFile(CONFIG_PATH, json, 16384, error,
                                    &truncated))
        return false;
    if (truncated) {
        error = "web_reader.json is too large";
        return false;
    }
    JsonDocument doc;
    DeserializationError parseError = deserializeJson(doc, json);
    if (parseError) {
        error = String("web_reader.json: ") + parseError.c_str();
        return false;
    }
    config.home = doc["home"] | config.home;
    config.search = doc["search_template"] | config.search;
    config.userAgent = doc["user_agent"] | config.userAgent;
    config.tlsInsecure = doc["tls"]["insecure"] | true;
    config.caCertPath = doc["tls"]["ca_cert"] | "";
    long maxBytes = doc["max_page_bytes"] | 196608;
    if (maxBytes < 8192) maxBytes = 8192;
    if (maxBytes > 196608) maxBytes = 196608;
    config.maxPageBytes = static_cast<size_t>(maxBytes);
    error = "";
    return true;
}

bool saveConfig(const WebConfig& config, String& error) {
    JsonDocument doc;
    doc["version"] = 1;
    doc["home"] = config.home;
    doc["search_template"] = config.search;
    doc["user_agent"] = config.userAgent;
    doc["tls"]["insecure"] = config.tlsInsecure;
    doc["tls"]["ca_cert"] = config.caCertPath;
    doc["max_page_bytes"] = config.maxPageBytes;
    String json;
    serializeJsonPretty(doc, json);
    return NetworkTransport::writeFileAtomic(CONFIG_PATH, json, error);
}

bool loadEntries(const char* path, WebEntry* entries, size_t capacity,
                 size_t& count) {
    count = 0;
    String json;
    String error;
    bool truncated = false;
    if (!NetworkTransport::readFile(path, json, 32768, error, &truncated) ||
        truncated)
        return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    JsonArray array = doc["entries"].as<JsonArray>();
    for (JsonObject item : array) {
        if (count >= capacity) break;
        String url = item["url"] | "";
        if (url.length() == 0 || url.length() > 512) continue;
        entries[count].url = url;
        entries[count].title = item["title"] | url;
        ++count;
    }
    return true;
}

bool saveEntries(const char* path, const WebEntry* entries, size_t count,
                 String& error) {
    JsonDocument doc;
    JsonArray array = doc["entries"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
        JsonObject item = array.add<JsonObject>();
        item["title"] = entries[i].title;
        item["url"] = entries[i].url;
    }
    String json;
    serializeJsonPretty(doc, json);
    return NetworkTransport::writeFileAtomic(path, json, error);
}

String lowerCopy(String value) {
    value.toLowerCase();
    return value;
}

bool parseUrl(String url, UrlParts& output, String& error) {
    url.trim();
    if (url.length() == 0 || url.length() > 512) {
        error = "URL is empty or too long";
        return false;
    }
    if (url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0 ||
        url.indexOf(' ') >= 0) {
        error = "URL contains invalid characters";
        return false;
    }
    if (url.indexOf("://") < 0) url = "https://" + url;
    const int schemeEnd = url.indexOf("://");
    output.scheme = lowerCopy(url.substring(0, schemeEnd));
    if (output.scheme != "http" && output.scheme != "https") {
        error = "Only HTTP and HTTPS are supported";
        return false;
    }
    output.tls = output.scheme == "https";
    const int authorityStart = schemeEnd + 3;
    int pathStart = url.indexOf('/', authorityStart);
    const int queryStart = url.indexOf('?', authorityStart);
    if (pathStart < 0 || (queryStart >= 0 && queryStart < pathStart))
        pathStart = queryStart;
    if (pathStart < 0) pathStart = url.length();
    String authority = url.substring(authorityStart, pathStart);
    if (authority.indexOf('@') >= 0 || authority.startsWith("[")) {
        error = "Userinfo and IPv6 literal URLs are not supported";
        return false;
    }
    const int colon = authority.lastIndexOf(':');
    output.port = output.tls ? 443 : 80;
    if (colon > 0) {
        const long parsed = authority.substring(colon + 1).toInt();
        if (parsed < 1 || parsed > 65535) {
            error = "Invalid URL port";
            return false;
        }
        output.port = static_cast<uint16_t>(parsed);
        authority = authority.substring(0, colon);
    }
    if (authority.length() == 0) {
        error = "URL host is empty";
        return false;
    }
    output.host = authority;
    output.path =
        pathStart < static_cast<int>(url.length()) ? url.substring(pathStart)
                                                  : "/";
    const int fragment = output.path.indexOf('#');
    if (fragment >= 0) output.path = output.path.substring(0, fragment);
    if (output.path.length() == 0) output.path = "/";
    error = "";
    return true;
}

String authority(const UrlParts& parts) {
    String value = parts.scheme + "://" + parts.host;
    if ((parts.tls && parts.port != 443) ||
        (!parts.tls && parts.port != 80))
        value += ":" + String(parts.port);
    return value;
}

String normalizePath(const String& input) {
    String query;
    String path = input;
    const int queryAt = path.indexOf('?');
    if (queryAt >= 0) {
        query = path.substring(queryAt);
        path = path.substring(0, queryAt);
    }
    String segments[48];
    size_t count = 0;
    size_t cursor = 0;
    while (cursor <= path.length() && count < 48) {
        int slash = path.indexOf('/', cursor);
        if (slash < 0) slash = path.length();
        String segment = path.substring(cursor, slash);
        if (segment == "..") {
            if (count) --count;
        } else if (segment.length() && segment != ".") {
            segments[count++] = segment;
        }
        cursor = static_cast<size_t>(slash + 1);
        if (slash == static_cast<int>(path.length())) break;
    }
    String result = "/";
    for (size_t i = 0; i < count; ++i) {
        if (i) result += '/';
        result += segments[i];
    }
    if (path.endsWith("/") && !result.endsWith("/")) result += '/';
    return result + query;
}

String resolveUrl(const String& base, String reference) {
    reference.trim();
    reference.replace("&amp;", "&");
    if (reference.startsWith("http://") || reference.startsWith("https://"))
        return reference;
    UrlParts baseParts;
    String error;
    if (!parseUrl(base, baseParts, error)) return "";
    if (reference.startsWith("//"))
        return baseParts.scheme + ":" + reference;
    if (reference.startsWith("#")) return base;
    if (reference.startsWith("?")) {
        String path = baseParts.path;
        const int query = path.indexOf('?');
        if (query >= 0) path = path.substring(0, query);
        return authority(baseParts) + path + reference;
    }
    if (reference.startsWith("/"))
        return authority(baseParts) + normalizePath(reference);
    String directory = baseParts.path;
    const int query = directory.indexOf('?');
    if (query >= 0) directory = directory.substring(0, query);
    const int slash = directory.lastIndexOf('/');
    directory = slash >= 0 ? directory.substring(0, slash + 1) : "/";
    return authority(baseParts) + normalizePath(directory + reference);
}

String decodeEntity(const String& entity) {
    if (entity == "amp") return "&";
    if (entity == "lt") return "<";
    if (entity == "gt") return ">";
    if (entity == "quot") return "\"";
    if (entity == "apos") return "'";
    if (entity == "nbsp") return " ";
    if (entity.startsWith("#x") || entity.startsWith("#X")) {
        const long value = strtol(entity.c_str() + 2, nullptr, 16);
        if (value > 0 && value < 128) return String(static_cast<char>(value));
    } else if (entity.startsWith("#")) {
        const long value = strtol(entity.c_str() + 1, nullptr, 10);
        if (value > 0 && value < 128) return String(static_cast<char>(value));
    }
    return String("&") + entity + ";";
}

class HtmlParser {
public:
    explicit HtmlParser(Page& page) : _page(page) {}

    void feed(const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; ++i) feedChar(static_cast<char>(data[i]));
    }

    void finish() {
        flushEntity();
        _page.title.trim();
        if (_page.title.length() == 0) _page.title = "(untitled page)";
        while (_page.length &&
               (_page.lastChar() == ' ' || _page.lastChar() == '\n'))
            _page.shrink(_page.length - 1);
    }

private:
    Page& _page;
    bool _inTag = false;
    char _quote = 0;
    String _tag;
    bool _inEntity = false;
    String _entity;
    String _skipTag;
    bool _inTitle = false;
    bool _inPre = false;
    bool _linkActive = false;
    size_t _linkTextStart = 0;
    String _linkHref;
    bool _lastSpace = true;

    void appendText(char c) {
        if (_skipTag.length()) return;
        if (_inTitle) {
            if (_page.title.length() < 160) _page.title += c;
            return;
        }
        if (!_inPre && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (!_lastSpace && _page.length &&
                _page.lastChar() != '\n') {
                _page.append(' ');
                _lastSpace = true;
            }
            return;
        }
        _page.append(c);
        _lastSpace = false;
    }

    void newline() {
        if (_skipTag.length() || _inTitle) return;
        while (_page.length && _page.lastChar() == ' ')
            _page.shrink(_page.length - 1);
        if (_page.length && _page.lastChar() != '\n')
            _page.append('\n');
        _lastSpace = true;
    }

    void flushEntity() {
        if (!_inEntity) return;
        const String decoded = decodeEntity(_entity);
        for (size_t i = 0; i < decoded.length(); ++i) appendText(decoded[i]);
        _entity = "";
        _inEntity = false;
    }

    static String tagName(String tag) {
        tag.trim();
        if (tag.startsWith("/")) tag.remove(0, 1);
        int end = 0;
        while (end < static_cast<int>(tag.length()) &&
               !isspace(tag[end]) && tag[end] != '/')
            ++end;
        tag = tag.substring(0, end);
        tag.toLowerCase();
        return tag;
    }

    static String attribute(const String& tag, const String& requested) {
        String lower = lowerCopy(tag);
        String key = lowerCopy(requested);
        size_t cursor = 0;
        while (cursor < lower.length()) {
            int found = lower.indexOf(key, cursor);
            if (found < 0) return "";
            const bool leftOk =
                found == 0 || isspace(lower[found - 1]) || lower[found - 1] == '/';
            int after = found + key.length();
            while (after < static_cast<int>(lower.length()) &&
                   isspace(lower[after]))
                ++after;
            if (!leftOk || after >= static_cast<int>(lower.length()) ||
                lower[after] != '=') {
                cursor = static_cast<size_t>(found + key.length());
                continue;
            }
            ++after;
            while (after < static_cast<int>(tag.length()) && isspace(tag[after]))
                ++after;
            if (after >= static_cast<int>(tag.length())) return "";
            if (tag[after] == '"' || tag[after] == '\'') {
                const char quote = tag[after++];
                int end = tag.indexOf(quote, after);
                if (end < 0) end = tag.length();
                return tag.substring(after, end);
            }
            int end = after;
            while (end < static_cast<int>(tag.length()) &&
                   !isspace(tag[end]) && tag[end] != '>')
                ++end;
            return tag.substring(after, end);
        }
        return "";
    }

    void finishLink() {
        if (!_linkActive) return;
        if (_page.linkCount < _page.linkCapacity) {
            WebLink& link = _page.links[_page.linkCount];
            String text;
            if (_linkTextStart < _page.length) {
                char bounded[80] = {};
                size_t amount = _page.length - _linkTextStart;
                if (amount >= sizeof(bounded)) amount = sizeof(bounded) - 1;
                if (_page.readAt(_linkTextStart, bounded, amount))
                    text = bounded;
            }
            text.replace("\n", " ");
            text.trim();
            if (text.length() == 0) text = _linkHref;
            snprintf(link.text, sizeof(link.text), "%s", text.c_str());
            snprintf(link.href, sizeof(link.href), "%s", _linkHref.c_str());
            _page.append(" [");
            String number = String(_page.linkCount + 1);
            _page.append(number.c_str());
            _page.append("]");
            ++_page.linkCount;
        }
        _linkActive = false;
        _linkHref = "";
    }

    void processTag() {
        String raw = _tag;
        raw.trim();
        const bool closing = raw.startsWith("/");
        const String name = tagName(raw);
        if (_skipTag.length()) {
            if (closing && name == _skipTag) _skipTag = "";
            return;
        }
        if (!closing &&
            (name == "script" || name == "style" || name == "noscript" ||
             name == "svg")) {
            _skipTag = name;
            return;
        }
        if (name == "title") {
            _inTitle = !closing;
            return;
        }
        if (name == "pre") {
            newline();
            _inPre = !closing;
            return;
        }
        if (name == "a") {
            if (closing) {
                finishLink();
            } else {
                _linkHref = attribute(raw, "href");
                _linkActive = _linkHref.length() > 0 &&
                              !_linkHref.startsWith("javascript:") &&
                              !_linkHref.startsWith("mailto:");
                _linkTextStart = _page.length;
            }
            return;
        }
        if (name == "br" || name == "hr" || name == "p" || name == "div" ||
            name == "section" || name == "article" || name == "header" ||
            name == "footer" || name == "tr" || name == "h1" ||
            name == "h2" || name == "h3" || name == "h4" ||
            (closing && (name == "li" || name == "blockquote"))) {
            newline();
        }
        if (!closing && name == "li") {
            newline();
            _page.append("* ");
            _lastSpace = false;
        }
    }

    void feedChar(char c) {
        if (_inTag) {
            if (_quote) {
                if (c == _quote) _quote = 0;
                if (_tag.length() < 768) _tag += c;
                return;
            }
            if (c == '"' || c == '\'') {
                _quote = c;
                if (_tag.length() < 768) _tag += c;
                return;
            }
            if (c == '>') {
                _inTag = false;
                processTag();
                _tag = "";
                return;
            }
            if (_tag.length() < 768) _tag += c;
            return;
        }
        if (_inEntity) {
            if (c == ';') {
                flushEntity();
            } else if (_entity.length() < 12 &&
                       (isalnum(c) || c == '#' || c == 'x' || c == 'X')) {
                _entity += c;
            } else {
                flushEntity();
                appendText(c);
            }
            return;
        }
        if (c == '<') {
            flushEntity();
            _inTag = true;
            _tag = "";
        } else if (c == '&') {
            _inEntity = true;
            _entity = "";
        } else {
            appendText(c);
        }
    }
};

void appendPlain(Page& page, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        char c = static_cast<char>(data[i]);
        if (c != '\r') page.append(c);
    }
}

bool readHttpBody(NetworkTransport::Connection& connection, Page& page,
                  bool html, bool chunked, int64_t contentLength,
                  String& error) {
    HtmlParser parser(page);
    uint8_t buffer[1024];
    size_t accepted = 0;
    auto consume = [&](const uint8_t* data, size_t length) {
        size_t allowed = length;
        if (accepted + allowed > g_config.maxPageBytes)
            allowed = g_config.maxPageBytes - accepted;
        if (allowed) {
            if (html)
                parser.feed(data, allowed);
            else
                appendPlain(page, data, allowed);
            accepted += allowed;
        }
        page.wireBytes += length;
        if (allowed < length) page.truncated = true;
    };

    if (chunked) {
        while (accepted < g_config.maxPageBytes) {
            String line;
            if (!connection.readLine(line, 128, 15000, error)) return false;
            const int semicolon = line.indexOf(';');
            if (semicolon >= 0) line = line.substring(0, semicolon);
            line.trim();
            char* end = nullptr;
            const unsigned long chunk = strtoul(line.c_str(), &end, 16);
            if (!end || *end != '\0') {
                error = "Invalid HTTP chunk size";
                return false;
            }
            if (chunk == 0) {
                do {
                    if (!connection.readLine(line, 2048, 10000, error))
                        return false;
                } while (line.length());
                break;
            }
            size_t remaining = chunk;
            while (remaining) {
                size_t request =
                    remaining < sizeof(buffer) ? remaining : sizeof(buffer);
                if (!connection.readExact(buffer, request, 15000, error))
                    return false;
                consume(buffer, request);
                remaining -= request;
                if (accepted >= g_config.maxPageBytes) break;
            }
            if (accepted >= g_config.maxPageBytes) {
                page.truncated = true;
                break;
            }
            uint8_t crlf[2];
            if (!connection.readExact(crlf, sizeof(crlf), 5000, error))
                return false;
        }
    } else if (contentLength >= 0) {
        uint64_t remaining = static_cast<uint64_t>(contentLength);
        while (remaining && accepted < g_config.maxPageBytes) {
            size_t request = remaining < sizeof(buffer)
                                 ? static_cast<size_t>(remaining)
                                 : sizeof(buffer);
            if (!connection.readExact(buffer, request, 15000, error))
                return false;
            consume(buffer, request);
            remaining -= request;
        }
        if (remaining) page.truncated = true;
    } else {
        while (connection.connected() && accepted < g_config.maxPageBytes) {
            const int count =
                connection.readSome(buffer, sizeof(buffer), 7000, error);
            if (count < 0) return false;
            if (count == 0) break;
            consume(buffer, static_cast<size_t>(count));
            M5Cardputer.update();
            if (InputCompat::isBackPressed()) {
                error = "Request cancelled";
                return false;
            }
        }
        if (accepted >= g_config.maxPageBytes) page.truncated = true;
    }
    if (html)
        parser.finish();
    else if (page.title.length() == 0)
        page.title = "(text document)";
    return true;
}

bool fetchOnce(const String& url, Page& page, String& redirect,
               String& error) {
    UrlParts parts;
    if (!parseUrl(url, parts, error)) return false;
    page.clear();
    page.url = url;

    NetworkTransport::TlsOptions tls;
    tls.insecure = g_config.tlsInsecure;
    tls.caCertPath = g_config.caCertPath;
    NetworkTransport::Connection connection;
    showStatus("Web Reader", String("Connecting:\n") + parts.host);
    if (!connection.connect("web-reader", parts.host, parts.port, parts.tls,
                            tls, error))
        return false;

    String hostHeader = parts.host;
    if ((parts.tls && parts.port != 443) ||
        (!parts.tls && parts.port != 80))
        hostHeader += ":" + String(parts.port);
    String request;
    request.reserve(512 + parts.path.length());
    request += "GET " + parts.path + " HTTP/1.1\r\n";
    request += "Host: " + hostHeader + "\r\n";
    request += "User-Agent: " + g_config.userAgent + "\r\n";
    request += "Accept: text/html,text/plain,application/xhtml+xml,"
               "application/json;q=0.8,*/*;q=0.2\r\n";
    request += "Accept-Encoding: identity\r\n";
    request += "Connection: close\r\n\r\n";
    if (!connection.writeString(request, error)) return false;

    String line;
    if (!connection.readLine(line, 2048, 15000, error)) return false;
    if (!line.startsWith("HTTP/")) {
        error = "Invalid HTTP status line";
        return false;
    }
    const int firstSpace = line.indexOf(' ');
    page.statusCode =
        firstSpace >= 0 ? line.substring(firstSpace + 1).toInt() : 0;

    bool chunked = false;
    int64_t contentLength = -1;
    String contentEncoding;
    size_t headerBytes = line.length();
    while (true) {
        if (!connection.readLine(line, 4096, 15000, error)) return false;
        headerBytes += line.length();
        if (headerBytes > 32768) {
            error = "HTTP headers exceed 32 KB";
            return false;
        }
        if (line.length() == 0) break;
        const int colon = line.indexOf(':');
        if (colon <= 0) continue;
        String name = lowerCopy(line.substring(0, colon));
        String value = line.substring(colon + 1);
        value.trim();
        if (name == "location")
            redirect = resolveUrl(url, value);
        else if (name == "content-type")
            page.contentType = value;
        else if (name == "content-length")
            contentLength = strtoll(value.c_str(), nullptr, 10);
        else if (name == "transfer-encoding")
            chunked = lowerCopy(value).indexOf("chunked") >= 0;
        else if (name == "content-encoding")
            contentEncoding = lowerCopy(value);
    }

    if (page.statusCode >= 300 && page.statusCode < 400 &&
        redirect.length()) {
        connection.close(0);
        return true;
    }
    if (contentEncoding.length() && contentEncoding != "identity") {
        error = String("Unsupported Content-Encoding: ") + contentEncoding;
        return false;
    }

    String lowerType = lowerCopy(page.contentType);
    const bool html = lowerType.length() == 0 ||
                      lowerType.indexOf("text/html") >= 0 ||
                      lowerType.indexOf("application/xhtml") >= 0;
    if (!html && lowerType.indexOf("text/") < 0 &&
        lowerType.indexOf("json") < 0 && lowerType.indexOf("xml") < 0) {
        error = String("Unsupported Content-Type: ") + page.contentType;
        return false;
    }
    showStatus("Web Reader", "Receiving page...");
    const bool ok =
        readHttpBody(connection, page, html, chunked, contentLength, error);
    connection.close(ok ? 0 : -1);
    if (ok && page.statusCode >= 400) {
        page.title = "HTTP " + String(page.statusCode);
    }
    return ok;
}

bool fetchPage(String url, Page& page, String& error) {
    for (int redirectCount = 0; redirectCount <= 5; ++redirectCount) {
        String redirect;
        if (!fetchOnce(url, page, redirect, error)) return false;
        if (redirect.length() == 0) {
            page.url = url;
            if (page.title.length() == 0) page.title = page.url;
            const size_t chars =
                (display().width() > 12 ? display().width() - 8 : 228) / 6;
            page.buildLines(chars);
            return true;
        }
        url = redirect;
    }
    error = "Too many HTTP redirects";
    return false;
}

String urlEncode(const String& input) {
    static const char hex[] = "0123456789ABCDEF";
    String output;
    output.reserve(input.length() * 3 + 1);
    for (size_t i = 0; i < input.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(input[i]);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output += static_cast<char>(c);
        } else if (c == ' ') {
            output += '+';
        } else {
            output += '%';
            output += hex[c >> 4];
            output += hex[c & 0x0F];
        }
    }
    return output;
}

void addHistory(const Page& page) {
    if (page.url.length() == 0) return;
    size_t existing = MAX_HISTORY;
    for (size_t i = 0; i < g_historyCount; ++i) {
        if (g_history[i].url == page.url) {
            existing = i;
            break;
        }
    }
    WebEntry entry{page.title, page.url};
    if (existing < g_historyCount) {
        for (size_t i = existing; i > 0; --i) g_history[i] = g_history[i - 1];
        g_history[0] = entry;
    } else {
        size_t count =
            g_historyCount < MAX_HISTORY ? g_historyCount + 1 : MAX_HISTORY;
        for (size_t i = count - 1; i > 0; --i)
            g_history[i] = g_history[i - 1];
        g_history[0] = entry;
        g_historyCount = count;
    }
    String error;
    saveEntries(HISTORY_PATH, g_history, g_historyCount, error);
}

bool isBookmarked(const String& url, size_t* index = nullptr) {
    for (size_t i = 0; i < g_bookmarkCount; ++i) {
        if (g_bookmarks[i].url == url) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

void toggleBookmark(const Page& page) {
    size_t index = 0;
    if (isBookmarked(page.url, &index)) {
        for (size_t i = index + 1; i < g_bookmarkCount; ++i)
            g_bookmarks[i - 1] = g_bookmarks[i];
        --g_bookmarkCount;
    } else {
        if (g_bookmarkCount >= MAX_BOOKMARKS) {
            modal("Bookmarks", "Bookmark limit reached.", TFT_ORANGE);
            return;
        }
        g_bookmarks[g_bookmarkCount++] = {page.title, page.url};
    }
    String error;
    if (!saveEntries(BOOKMARK_PATH, g_bookmarks, g_bookmarkCount, error))
        modal("Bookmarks", error, TFT_RED);
}

void pushBack(const String& url) {
    if (url.length() == 0) return;
    if (g_backCount >= MAX_BACK_STACK) {
        for (size_t i = 1; i < g_backCount; ++i)
            g_backStack[i - 1] = g_backStack[i];
        --g_backCount;
    }
    g_backStack[g_backCount++] = url;
}

String popBack() {
    if (!g_backCount) return "";
    return g_backStack[--g_backCount];
}

void drawReader(size_t scroll, int selectedLink) {
    drawTitle(g_page.title);
    lgfx::LGFX_Device& d = display();
    const int top = 15;
    const int rows = (d.height() - top - 12) / 10;
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    char line[128];
    for (int row = 0; row < rows; ++row) {
        const size_t index = scroll + row;
        if (index >= g_page.lineCount) break;
        const LineSpan& span = g_page.lines[index];
        size_t count = span.length;
        if (count >= sizeof(line)) count = sizeof(line) - 1;
        if (!g_page.readAt(span.start, line, count)) count = 0;
        line[count] = '\0';
        d.setCursor(3, top + row * 10);
        d.print(line);
    }
    String footer = String(scroll + 1) + "/" + String(g_page.lineCount);
    if (g_page.truncated) footer += " TRUNC";
    if (selectedLink >= 0)
        footer += " Link " + String(selectedLink + 1);
    else
        footer += " l Links";
    drawFooter(footer);
}

int chooseEntry(const String& title, const WebEntry* entries, size_t count) {
    if (count == 0) {
        modal(title, "No entries.", TFT_ORANGE);
        return -1;
    }
    size_t selected = 0;
    InputState input;
    while (true) {
        drawTitle(title);
        lgfx::LGFX_Device& d = display();
        const int visible = (d.height() - 27) / 12;
        size_t start = selected >= static_cast<size_t>(visible)
                           ? selected - visible + 1
                           : 0;
        for (int row = 0; row < visible; ++row) {
            const size_t index = start + row;
            if (index >= count) break;
            const int y = 15 + row * 12;
            const bool active = index == selected;
            if (active) d.fillRect(0, y, d.width(), 12, TFT_NAVY);
            d.setTextColor(active ? TFT_WHITE : TFT_LIGHTGREY,
                           active ? TFT_NAVY : TFT_BLACK);
            d.setCursor(3, y + 2);
            String shown = entries[index].title;
            const int maxChars = (d.width() - 8) / 6;
            if (shown.length() > static_cast<size_t>(maxChars))
                shown = shown.substring(0, maxChars);
            d.print(shown);
        }
        drawFooter(";/. Move  Enter  FN+Del");
        const UiEvent event = input.poll();
        if (event == UiEvent::EXIT) return -1;
        if (event == UiEvent::UP && selected) --selected;
        if (event == UiEvent::DOWN && selected + 1 < count) ++selected;
        if (event == UiEvent::ENTER) return static_cast<int>(selected);
        delay(10);
    }
}

int chooseLink() {
    if (g_page.linkCount == 0) {
        modal("Links", "No links on this page.", TFT_ORANGE);
        return -1;
    }
    size_t selected = 0;
    InputState input;
    while (true) {
        drawTitle("Page links");
        lgfx::LGFX_Device& d = display();
        const size_t visible = (d.height() - 27) / 12;
        const size_t start = selected >= visible ? selected - visible + 1 : 0;
        for (size_t row = 0; row < visible && start + row < g_page.linkCount;
             ++row) {
            const size_t index = start + row;
            const int y = 15 + static_cast<int>(row) * 12;
            const bool active = index == selected;
            if (active) d.fillRect(0, y, d.width(), 12, TFT_NAVY);
            d.setTextColor(active ? TFT_WHITE : TFT_LIGHTGREY,
                           active ? TFT_NAVY : TFT_BLACK);
            d.setCursor(3, y + 2);
            d.print(index + 1);
            d.print(". ");
            String shown = g_page.links[index].text;
            const int maxChars = (d.width() - 24) / 6;
            if (shown.length() > static_cast<size_t>(maxChars))
                shown = shown.substring(0, maxChars);
            d.print(shown);
        }
        drawFooter(";/. Move  Enter  FN+Del");
        const UiEvent event = input.poll();
        if (event == UiEvent::EXIT) return -1;
        if (event == UiEvent::UP && selected) --selected;
        if (event == UiEvent::DOWN && selected + 1 < g_page.linkCount)
            ++selected;
        if (event == UiEvent::ENTER) return static_cast<int>(selected);
        delay(10);
    }
}

String safeFileName(String value) {
    value.trim();
    if (value.length() == 0) value = "page";
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (!isalnum(static_cast<uint8_t>(c)) && c != '-' && c != '_')
            value.setCharAt(i, '_');
    }
    if (value.length() > 32) value = value.substring(0, 32);
    return value;
}

void saveCurrentPage() {
    String error;
    if (!NetworkTransport::ensureDirectory(SAVED_ROOT, &error)) {
        modal("Save failed", error, TFT_RED);
        return;
    }
    time_t now = time(nullptr);
    String stamp = now >= 1700000000 ? String(static_cast<uint32_t>(now))
                                     : String(millis());
    String path = String(SAVED_ROOT) + "/" + safeFileName(g_page.title) + "_" +
                  stamp + ".txt";
    const String temporary = path + ".tmp";
    bool saved = false;
    {
        DisplayRuntime::ScopedSdDisplayRelease sdGuard;
        if (SD.exists(temporary.c_str())) SD.remove(temporary.c_str());
        File output = SD.open(temporary.c_str(), FILE_WRITE);
        if (!output) {
            error = "Cannot create temporary page file";
        } else {
            const size_t expected = g_page.title.length() + g_page.url.length() +
                                    g_page.length + 3;
            size_t written = output.print(g_page.title);
            written += output.print('\n');
            written += output.print(g_page.url);
            written += output.print("\n\n");
            uint8_t chunk[512];
            size_t copied = 0;
            while (copied < g_page.length) {
                size_t amount = g_page.length - copied;
                if (amount > sizeof(chunk)) amount = sizeof(chunk);
                if (!g_page.readAt(copied, chunk, amount)) break;
                const size_t part = output.write(chunk, amount);
                written += part;
                copied += part;
                if (part != amount) break;
            }
            output.flush();
            output.close();
            if (written != expected) {
                error = "Incomplete page write";
                SD.remove(temporary.c_str());
            } else {
                if (SD.exists(path.c_str())) SD.remove(path.c_str());
                saved = SD.rename(temporary.c_str(), path.c_str());
                if (!saved) error = "Cannot install saved page";
            }
        }
    }
    modal(saved ? "Page saved" : "Save failed", saved ? path : error,
          saved ? TFT_GREEN : TFT_RED);
}

String promptUrl(const String& label) {
    showStatus("Web Reader", label);
    drawFooter("Type URL, Enter");
    String value = getUserInput(false);
    value.trim();
    return value;
}

String makeSearchUrl(const String& query) {
    String url = g_config.search;
    url.replace("{query}", urlEncode(query));
    return url;
}

enum class ReaderAction : uint8_t { MENU, EXIT, OPEN, BACK, RELOAD };

ReaderAction readerLoop(String& requestedUrl) {
    size_t scroll = 0;
    int selectedLink = -1;
    InputState input;
    while (true) {
        drawReader(scroll, selectedLink);
        const size_t visible = (display().height() - 27) / 10;
        const UiEvent event = input.poll();
        if (event == UiEvent::EXIT) return ReaderAction::EXIT;
        if (event == UiEvent::UP && scroll) --scroll;
        if (event == UiEvent::DOWN && scroll + visible < g_page.lineCount)
            ++scroll;
        if (event == UiEvent::LEFT)
            scroll = scroll > visible ? scroll - visible : 0;
        if (event == UiEvent::RIGHT && g_page.lineCount &&
            scroll + visible < g_page.lineCount) {
            scroll += visible;
            if (scroll >= g_page.lineCount) scroll = g_page.lineCount - 1;
        }
        if (event == UiEvent::LINKS) {
            const int link = chooseLink();
            if (link >= 0) selectedLink = link;
        }
        if (event == UiEvent::ENTER) {
            if (selectedLink < 0) selectedLink = chooseLink();
            if (selectedLink >= 0 &&
                selectedLink < static_cast<int>(g_page.linkCount)) {
                requestedUrl =
                    resolveUrl(g_page.url, g_page.links[selectedLink].href);
                if (requestedUrl.length()) return ReaderAction::OPEN;
            }
        }
        if (event == UiEvent::GO) {
            requestedUrl = promptUrl("Open URL");
            if (requestedUrl.length()) return ReaderAction::OPEN;
        }
        if (event == UiEvent::SEARCH) {
            String query = promptUrl("Search query");
            if (query.length()) {
                requestedUrl = makeSearchUrl(query);
                return ReaderAction::OPEN;
            }
        }
        if (event == UiEvent::BACK) {
            requestedUrl = popBack();
            if (requestedUrl.length()) return ReaderAction::BACK;
            return ReaderAction::MENU;
        }
        if (event == UiEvent::HOME) {
            requestedUrl = g_config.home;
            return ReaderAction::OPEN;
        }
        if (event == UiEvent::RELOAD) {
            requestedUrl = g_page.url;
            return ReaderAction::RELOAD;
        }
        if (event == UiEvent::BOOKMARK) toggleBookmark(g_page);
        if (event == UiEvent::SAVE) saveCurrentPage();
        delay(10);
    }
}

int homeMenu() {
    const char* items[] = {"Home page", "Open URL", "Search web",
                           "Bookmarks", "History", "Settings JSON", "Back"};
    size_t selected = 0;
    InputState input;
    while (true) {
        drawTitle("Web Reader");
        lgfx::LGFX_Device& d = display();
        const int visible = (d.height() - 27) / 12;
        for (int row = 0; row < visible && row < 7; ++row) {
            const int y = 15 + row * 12;
            const bool active = static_cast<size_t>(row) == selected;
            if (active) d.fillRect(0, y, d.width(), 12, TFT_NAVY);
            d.setTextColor(active ? TFT_WHITE : TFT_LIGHTGREY,
                           active ? TFT_NAVY : TFT_BLACK);
            d.setCursor(3, y + 2);
            d.print(items[row]);
        }
        if (g_page.usingRam) {
            String footer = "RAM cache ";
            footer += String(g_page.capacity / 1024U);
            footer += "KB";
            if (g_config.tlsInsecure) footer += " TLS!";
            drawFooter(footer);
        } else {
            drawFooter(g_config.tlsInsecure ? "WARNING: insecure TLS"
                                            : "Verified TLS");
        }
        UiEvent event = input.poll();
        if (event == UiEvent::EXIT) return 6;
        if (event == UiEvent::UP && selected) --selected;
        if (event == UiEvent::DOWN && selected < 6) ++selected;
        if (event == UiEvent::ENTER) return static_cast<int>(selected);
        delay(10);
    }
}

}  // namespace

void run() {
    WebState state;
    g_state = &state;
    inMenu = false;
    NetCore::begin();
    String error;
    NetworkTransport::ensureDirectory("/evil/config", &error);
    NetworkTransport::ensureDirectory("/evil/web", &error);
    if (!loadConfig(g_config, error)) {
        g_config = WebConfig();
        saveConfig(g_config, error);
    }
    g_history = new (std::nothrow) WebEntry[MAX_HISTORY];
    g_bookmarks = new (std::nothrow) WebEntry[MAX_BOOKMARKS];
    g_backStack = new (std::nothrow) String[MAX_BACK_STACK];
    if (!g_history || !g_bookmarks || !g_backStack) {
        delete[] g_history;
        delete[] g_bookmarks;
        delete[] g_backStack;
        g_history = nullptr;
        g_bookmarks = nullptr;
        g_backStack = nullptr;
        modal("Web Reader", "Unable to allocate navigation state.", TFT_RED);
        g_state = nullptr;
        inMenu = true;
        return;
    }
    loadEntries(HISTORY_PATH, g_history, MAX_HISTORY, g_historyCount);
    loadEntries(BOOKMARK_PATH, g_bookmarks, MAX_BOOKMARKS, g_bookmarkCount);
    if (!g_page.allocate(g_config.maxPageBytes)) {
        const String reason = g_page.allocationError.length()
                                  ? g_page.allocationError
                                  : String("page allocation failed; ") +
                                        RuntimeMemory::describe();
        modal("Web Reader", reason,
              TFT_RED);
        delete[] g_history;
        delete[] g_bookmarks;
        delete[] g_backStack;
        g_history = nullptr;
        g_bookmarks = nullptr;
        g_backStack = nullptr;
        g_state = nullptr;
        inMenu = true;
        return;
    }

    while (InputCompat::isEnterPressed()) {
        M5Cardputer.update();
        delay(10);
    }

    bool running = true;
    String pendingUrl;
    bool pushCurrent = false;
    while (running) {
        if (pendingUrl.length() == 0) {
            const int choice = homeMenu();
            if (choice == 0)
                pendingUrl = g_config.home;
            else if (choice == 1)
                pendingUrl = promptUrl("Open URL");
            else if (choice == 2) {
                String query = promptUrl("Search query");
                if (query.length()) pendingUrl = makeSearchUrl(query);
            } else if (choice == 3) {
                const int index =
                    chooseEntry("Bookmarks", g_bookmarks, g_bookmarkCount);
                if (index >= 0) pendingUrl = g_bookmarks[index].url;
            } else if (choice == 4) {
                const int index =
                    chooseEntry("History", g_history, g_historyCount);
                if (index >= 0) pendingUrl = g_history[index].url;
            } else if (choice == 5) {
                textEditorOpen(CONFIG_PATH);
                WebConfig updated;
                if (loadConfig(updated, error)) {
                    g_config = updated;
                    g_page.release();
                    if (!g_page.allocate(g_config.maxPageBytes)) {
                        modal("Web Reader", g_page.allocationError.length()
                                                ? g_page.allocationError
                                                : "Buffer allocation failed.",
                              TFT_RED);
                        running = false;
                    }
                } else {
                    modal("web_reader.json", error, TFT_RED);
                }
            } else {
                running = false;
            }
            pushCurrent = false;
            if (!running) break;
            if (pendingUrl.length() == 0) continue;
        }

        String previousUrl = g_page.url;
        if (fetchPage(pendingUrl, g_page, error)) {
            if (pushCurrent && previousUrl.length() &&
                previousUrl != g_page.url)
                pushBack(previousUrl);
            addHistory(g_page);
            String next;
            const ReaderAction action = readerLoop(next);
            if (action == ReaderAction::EXIT) {
                running = false;
            } else if (action == ReaderAction::MENU) {
                pendingUrl = "";
            } else {
                pushCurrent = action == ReaderAction::OPEN;
                pendingUrl = next;
            }
        } else {
            modal("Web error", error, TFT_RED);
            pendingUrl = "";
            pushCurrent = false;
        }
        NetCore::poll();
    }

    g_page.release();
    delete[] g_history;
    delete[] g_bookmarks;
    delete[] g_backStack;
    g_history = nullptr;
    g_bookmarks = nullptr;
    g_backStack = nullptr;
    g_historyCount = 0;
    g_bookmarkCount = 0;
    g_backCount = 0;
    g_state = nullptr;
    inMenu = true;
}

}  // namespace WebReader
