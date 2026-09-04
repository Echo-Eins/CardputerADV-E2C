/*
 * network_transport.h - Bounded TCP/TLS and SD helpers shared by network apps.
 */

#ifndef NETWORK_TRANSPORT_H
#define NETWORK_TRANSPORT_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

namespace NetworkTransport {

struct TlsOptions {
    bool insecure = false;
    String caCertPath;
};

class Connection {
public:
    Connection();
    ~Connection();

    bool connect(const char* owner, const String& host, uint16_t port,
                 bool tls, const TlsOptions& tlsOptions, String& error,
                 uint32_t timeoutMs = 12000);
    void close(int32_t resultCode = 0);

    bool writeAll(const uint8_t* data, size_t length, String& error,
                  uint32_t timeoutMs = 12000);
    bool writeString(const String& value, String& error,
                     uint32_t timeoutMs = 12000);
    bool writeLine(const String& value, String& error,
                   uint32_t timeoutMs = 12000);

    int readSome(uint8_t* output, size_t capacity, uint32_t timeoutMs,
                 String& error);
    bool readExact(uint8_t* output, size_t length, uint32_t timeoutMs,
                   String& error);
    bool readExactString(size_t wireLength, String& output, size_t storeLimit,
                         uint32_t timeoutMs, String& error,
                         bool* truncated = nullptr);
    bool readLine(String& output, size_t maxLength, uint32_t timeoutMs,
                  String& error);
    int readByte(uint32_t timeoutMs, String& error);

    bool connected();
    int available();
    uint32_t telemetryId() const { return _connectionId; }

private:
    WiFiClient _plain;
    WiFiClientSecure _secure;
    Client* _client;
    String _caCertificate;
    uint32_t _connectionId;
};

bool ensureDirectory(const String& path, String* error = nullptr);
bool readFile(const String& path, String& output, size_t maxBytes,
              String& error, bool* truncated = nullptr);
bool writeFileAtomic(const String& path, const uint8_t* data, size_t length,
                     String& error);
bool writeFileAtomic(const String& path, const String& data, String& error);
bool removeFile(const String& path);

String base64Encode(const uint8_t* data, size_t length);
inline String base64Encode(const String& data) {
    return base64Encode(reinterpret_cast<const uint8_t*>(data.c_str()),
                        data.length());
}

}  // namespace NetworkTransport

#endif
