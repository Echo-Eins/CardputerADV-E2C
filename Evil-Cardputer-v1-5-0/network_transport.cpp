/*
 * network_transport.cpp - Common network transport built on NetCore.
 */

#include "network_transport.h"

#include <SD.h>

#include "display_runtime.h"
#include "netcore.h"

namespace NetworkTransport {
namespace {

void setError(String* output, const String& value) {
    if (output) *output = value;
}

bool makeDirectoriesUnlocked(const String& path, String& error) {
    if (path.length() == 0 || path[0] != '/') {
        error = "Path must be absolute";
        return false;
    }

    String current;
    for (size_t i = 1; i <= path.length(); ++i) {
        if (i != path.length() && path[i] != '/') continue;
        current = path.substring(0, i);
        if (current.length() == 0 || SD.exists(current.c_str())) continue;
        if (!SD.mkdir(current.c_str())) {
            error = String("Cannot create ") + current;
            return false;
        }
    }
    return true;
}

String parentDirectory(const String& path) {
    const int slash = path.lastIndexOf('/');
    if (slash <= 0) return "/";
    return path.substring(0, slash);
}

}  // namespace

Connection::Connection()
    : _client(nullptr), _connectionId(0) {}

Connection::~Connection() {
    close(-1);
}

bool Connection::connect(const char* owner, const String& host, uint16_t port,
                         bool tls, const TlsOptions& tlsOptions, String& error,
                         uint32_t timeoutMs) {
    close(0);
    error = "";

    NetCore::poll();
    if (WiFi.status() != WL_CONNECTED) {
        error = "WiFi is not connected";
        return false;
    }
    if (host.length() == 0 || port == 0) {
        error = "Invalid host or port";
        return false;
    }

    _connectionId =
        NetCore::registerConnection(owner ? owner : "network", host.c_str(),
                                    port, tls);
    // Telemetry is optional. A low-memory connection table must never block
    // the underlying TCP/TLS connection.

    IPAddress resolved;
    NetCore::setConnectionState(_connectionId,
                                NetCore::ConnectionState::RESOLVING);
    if (!NetCore::resolveHost(host.c_str(), resolved, 60000, &error)) {
        close(-2);
        return false;
    }

    if (tls) {
        _caCertificate = "";
        if (tlsOptions.insecure) {
            _secure.setInsecure();
        } else {
            if (tlsOptions.caCertPath.length() == 0) {
                error = "Verified TLS requires a CA certificate path";
                close(-3);
                return false;
            }
            bool truncated = false;
            if (!readFile(tlsOptions.caCertPath, _caCertificate, 24576, error,
                          &truncated)) {
                close(-4);
                return false;
            }
            if (truncated || _caCertificate.indexOf("BEGIN CERTIFICATE") < 0) {
                error = "Invalid or oversized CA certificate";
                close(-5);
                return false;
            }

            NetCore::ensureTimeSync();
            const uint32_t started = millis();
            while (!NetCore::timeSynchronized() &&
                   static_cast<uint32_t>(millis() - started) < 5000) {
                NetCore::poll();
                delay(20);
            }
            if (!NetCore::timeSynchronized()) {
                error = "System time is not synchronized for TLS";
                close(-6);
                return false;
            }
            _secure.setCACert(_caCertificate.c_str());
        }
        _secure.setTimeout(timeoutMs);
        _secure.setNoDelay(true);
        _client = &_secure;
    } else {
        _plain.setTimeout(timeoutMs);
        _plain.setNoDelay(true);
        _client = &_plain;
    }

    NetCore::setConnectionState(_connectionId,
                                NetCore::ConnectionState::CONNECTING);
    const bool ok = tls ? _secure.connect(host.c_str(), port)
                        : _plain.connect(host.c_str(), port);
    if (!ok) {
        error = String(tls ? "TLS" : "TCP") + " connection failed";
        close(-7);
        return false;
    }

    NetCore::setConnectionState(_connectionId,
                                NetCore::ConnectionState::CONNECTED);
    return true;
}

void Connection::close(int32_t resultCode) {
    if (_client) {
        _client->stop();
        _client = nullptr;
    }
    if (_connectionId != 0) {
        NetCore::closeConnection(_connectionId, resultCode);
        _connectionId = 0;
    }
    _caCertificate = "";
}

bool Connection::writeAll(const uint8_t* data, size_t length, String& error,
                          uint32_t timeoutMs) {
    if (!_client || (!data && length != 0)) {
        error = "Connection is not open";
        return false;
    }

    size_t offset = 0;
    uint32_t progressAt = millis();
    while (offset < length) {
        if (!_client->connected()) {
            error = "Connection closed while sending";
            return false;
        }
        const size_t written = _client->write(data + offset, length - offset);
        if (written > 0) {
            offset += written;
            progressAt = millis();
            NetCore::addConnectionTraffic(_connectionId, written, 0);
            continue;
        }
        if (static_cast<uint32_t>(millis() - progressAt) >= timeoutMs) {
            error = "Send timeout";
            return false;
        }
        NetCore::poll();
        delay(1);
    }
    return true;
}

bool Connection::writeString(const String& value, String& error,
                             uint32_t timeoutMs) {
    return writeAll(reinterpret_cast<const uint8_t*>(value.c_str()),
                    value.length(), error, timeoutMs);
}

bool Connection::writeLine(const String& value, String& error,
                           uint32_t timeoutMs) {
    if (!writeString(value, error, timeoutMs)) return false;
    static const uint8_t crlf[] = {'\r', '\n'};
    return writeAll(crlf, sizeof(crlf), error, timeoutMs);
}

int Connection::readSome(uint8_t* output, size_t capacity,
                         uint32_t timeoutMs, String& error) {
    if (!_client || !output || capacity == 0) {
        error = "Invalid read";
        return -1;
    }

    const uint32_t started = millis();
    while (_client->available() <= 0) {
        if (!_client->connected()) return 0;
        if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
            error = "Receive timeout";
            return -1;
        }
        NetCore::poll();
        delay(1);
    }

    int ready = _client->available();
    size_t requested = ready > 0 ? static_cast<size_t>(ready) : 1;
    if (requested > capacity) requested = capacity;
    const int received = _client->read(output, requested);
    if (received > 0) {
        NetCore::addConnectionTraffic(_connectionId, 0,
                                      static_cast<size_t>(received));
    }
    return received;
}

bool Connection::readExact(uint8_t* output, size_t length,
                           uint32_t timeoutMs, String& error) {
    size_t offset = 0;
    uint32_t progressAt = millis();
    while (offset < length) {
        const uint32_t elapsed = static_cast<uint32_t>(millis() - progressAt);
        if (elapsed >= timeoutMs) {
            error = "Receive timeout";
            return false;
        }
        const int received =
            readSome(output + offset, length - offset, timeoutMs - elapsed,
                     error);
        if (received < 0) return false;
        if (received == 0) {
            error = "Connection closed during receive";
            return false;
        }
        offset += static_cast<size_t>(received);
        progressAt = millis();
    }
    return true;
}

bool Connection::readExactString(size_t wireLength, String& output,
                                 size_t storeLimit, uint32_t timeoutMs,
                                 String& error, bool* truncated) {
    output = "";
    if (truncated) *truncated = wireLength > storeLimit;
    const size_t reserveLength =
        wireLength < storeLimit ? wireLength : storeLimit;
    if (!output.reserve(reserveLength + 1)) {
        error = "Not enough memory for response";
        return false;
    }

    uint8_t buffer[512];
    size_t remaining = wireLength;
    size_t stored = 0;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (!readExact(buffer, chunk, timeoutMs, error)) return false;
        const size_t room = stored < storeLimit ? storeLimit - stored : 0;
        const size_t keep = chunk < room ? chunk : room;
        for (size_t i = 0; i < keep; ++i) {
            output += static_cast<char>(buffer[i]);
        }
        stored += keep;
        remaining -= chunk;
    }
    return true;
}

bool Connection::readLine(String& output, size_t maxLength,
                          uint32_t timeoutMs, String& error) {
    output = "";
    if (!output.reserve(maxLength < 256 ? maxLength : 256)) {
        error = "Not enough memory for line";
        return false;
    }

    bool overflow = false;
    const uint32_t started = millis();
    while (true) {
        const uint32_t elapsed = static_cast<uint32_t>(millis() - started);
        if (elapsed >= timeoutMs) {
            error = "Line receive timeout";
            return false;
        }
        const int value = readByte(timeoutMs - elapsed, error);
        if (value < 0) {
            if (output.length() > 0 && error.length() == 0) break;
            return false;
        }
        if (value == '\n') break;
        if (value == '\r') continue;
        if (output.length() < maxLength) {
            output += static_cast<char>(value);
        } else {
            overflow = true;
        }
    }
    if (overflow) {
        error = "Protocol line exceeds limit";
        return false;
    }
    return true;
}

int Connection::readByte(uint32_t timeoutMs, String& error) {
    uint8_t value = 0;
    const int received = readSome(&value, 1, timeoutMs, error);
    if (received == 1) return value;
    if (received == 0) error = "";
    return -1;
}

bool Connection::connected() {
    return _client && (_client->connected() || _client->available() > 0);
}

int Connection::available() {
    return _client ? _client->available() : 0;
}

bool ensureDirectory(const String& path, String* error) {
    String localError;
    DisplayRuntime::ScopedSdDisplayRelease sdGuard;
    const bool ok = makeDirectoriesUnlocked(path, localError);
    setError(error, localError);
    return ok;
}

bool readFile(const String& path, String& output, size_t maxBytes,
              String& error, bool* truncated) {
    output = "";
    if (truncated) *truncated = false;
    DisplayRuntime::ScopedSdDisplayRelease sdGuard;
    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) {
        error = String("Cannot open ") + path;
        return false;
    }

    const size_t size = static_cast<size_t>(file.size());
    const size_t reserveLength = size < maxBytes ? size : maxBytes;
    if (!output.reserve(reserveLength + 1)) {
        file.close();
        error = "Not enough memory to read file";
        return false;
    }

    uint8_t buffer[512];
    size_t stored = 0;
    while (file.available() && stored < maxBytes) {
        size_t request = maxBytes - stored;
        if (request > sizeof(buffer)) request = sizeof(buffer);
        const int count = file.read(buffer, request);
        if (count <= 0) break;
        for (int i = 0; i < count; ++i) output += static_cast<char>(buffer[i]);
        stored += static_cast<size_t>(count);
    }
    if (truncated) *truncated = file.available() || size > maxBytes;
    file.close();
    error = "";
    return true;
}

bool writeFileAtomic(const String& path, const uint8_t* data, size_t length,
                     String& error) {
    if (!data && length != 0) {
        error = "Invalid file data";
        return false;
    }

    DisplayRuntime::ScopedSdDisplayRelease sdGuard;
    if (!makeDirectoriesUnlocked(parentDirectory(path), error)) return false;

    const String temporary = path + ".tmp";
    const String backup = path + ".bak";
    SD.remove(temporary.c_str());
    File file = SD.open(temporary.c_str(), FILE_WRITE);
    if (!file) {
        error = String("Cannot create ") + temporary;
        return false;
    }

    size_t written = 0;
    while (written < length) {
        const size_t count = file.write(data + written, length - written);
        if (count == 0) break;
        written += count;
    }
    file.flush();
    file.close();
    if (written != length) {
        SD.remove(temporary.c_str());
        error = "Incomplete SD write";
        return false;
    }

    SD.remove(backup.c_str());
    const bool hadOriginal = SD.exists(path.c_str());
    if (hadOriginal && !SD.rename(path.c_str(), backup.c_str())) {
        SD.remove(temporary.c_str());
        error = "Cannot create configuration backup";
        return false;
    }
    if (!SD.rename(temporary.c_str(), path.c_str())) {
        if (hadOriginal) SD.rename(backup.c_str(), path.c_str());
        SD.remove(temporary.c_str());
        error = "Cannot commit SD file";
        return false;
    }
    SD.remove(backup.c_str());
    error = "";
    return true;
}

bool writeFileAtomic(const String& path, const String& data, String& error) {
    return writeFileAtomic(
        path, reinterpret_cast<const uint8_t*>(data.c_str()), data.length(),
        error);
}

bool removeFile(const String& path) {
    DisplayRuntime::ScopedSdDisplayRelease sdGuard;
    return !SD.exists(path.c_str()) || SD.remove(path.c_str());
}

String base64Encode(const uint8_t* data, size_t length) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String output;
    output.reserve(((length + 2) / 3) * 4 + 1);
    for (size_t i = 0; i < length; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < length ? data[i + 1] : 0;
        const uint32_t c = i + 2 < length ? data[i + 2] : 0;
        const uint32_t value = (a << 16) | (b << 8) | c;
        output += table[(value >> 18) & 0x3F];
        output += table[(value >> 12) & 0x3F];
        output += i + 1 < length ? table[(value >> 6) & 0x3F] : '=';
        output += i + 2 < length ? table[value & 0x3F] : '=';
    }
    return output;
}

}  // namespace NetworkTransport
