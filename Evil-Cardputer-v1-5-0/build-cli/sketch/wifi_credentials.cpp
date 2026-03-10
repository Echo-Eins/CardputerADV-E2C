#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\wifi_credentials.cpp"
#include "wifi_credentials.h"
#include <SD.h>
#include <ArduinoJson.h>

// Global storage for saved networks
static std::vector<SavedNetwork> savedNetworks;
static bool credentialsLoaded = false;

void wifiCredentialsInit() {
    wifiCredentialsLoad();
}

bool wifiCredentialsLoad() {
    savedNetworks.clear();

    if (!SD.exists(WIFI_CREDENTIALS_PATH)) {
        Serial.println(F("[WiFiCred] No credentials file found"));
        credentialsLoaded = true;
        return true;  // Not an error, just empty
    }

    File file = SD.open(WIFI_CREDENTIALS_PATH, FILE_READ);
    if (!file) {
        Serial.println(F("[WiFiCred] Failed to open credentials file"));
        return false;
    }

    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.print(F("[WiFiCred] JSON parse error: "));
        Serial.println(error.c_str());
        return false;
    }

    // Load networks array
    JsonArray networks = doc["networks"].as<JsonArray>();
    for (JsonObject net : networks) {
        SavedNetwork sn;
        sn.ssid = net["ssid"].as<String>();
        sn.password = net["password"].as<String>();
        sn.lastUsed = net["lastUsed"] | 0UL;
        sn.autoConnect = net["autoConnect"] | false;

        if (sn.ssid.length() > 0) {
            savedNetworks.push_back(sn);
        }
    }

    Serial.printf("[WiFiCred] Loaded %d saved networks\n", savedNetworks.size());
    credentialsLoaded = true;
    return true;
}

bool wifiCredentialsSave() {
    // Ensure directory exists
    if (!SD.exists("/evil")) {
        SD.mkdir("/evil");
    }

    // Build JSON document
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    for (const SavedNetwork& sn : savedNetworks) {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"] = sn.ssid;
        net["password"] = sn.password;
        net["lastUsed"] = sn.lastUsed;
        net["autoConnect"] = sn.autoConnect;
    }

    // Write to file
    File file = SD.open(WIFI_CREDENTIALS_PATH, FILE_WRITE);
    if (!file) {
        Serial.println(F("[WiFiCred] Failed to open file for writing"));
        return false;
    }

    size_t written = serializeJsonPretty(doc, file);
    file.close();

    if (written == 0) {
        Serial.println(F("[WiFiCred] Failed to write JSON"));
        return false;
    }

    Serial.printf("[WiFiCred] Saved %d networks to SD\n", savedNetworks.size());
    return true;
}

bool wifiCredentialsHas(const String& ssid) {
    if (!credentialsLoaded) wifiCredentialsLoad();

    for (const SavedNetwork& sn : savedNetworks) {
        if (sn.ssid == ssid) {
            return true;
        }
    }
    return false;
}

String wifiCredentialsGetPassword(const String& ssid) {
    if (!credentialsLoaded) wifiCredentialsLoad();

    for (const SavedNetwork& sn : savedNetworks) {
        if (sn.ssid == ssid) {
            return sn.password;
        }
    }
    return "";
}

SavedNetwork* wifiCredentialsGet(const String& ssid) {
    if (!credentialsLoaded) wifiCredentialsLoad();

    for (SavedNetwork& sn : savedNetworks) {
        if (sn.ssid == ssid) {
            return &sn;
        }
    }
    return nullptr;
}

void wifiCredentialsSaveNetwork(const String& ssid, const String& password, bool autoConnect) {
    if (!credentialsLoaded) wifiCredentialsLoad();

    // Check if network already exists
    for (SavedNetwork& sn : savedNetworks) {
        if (sn.ssid == ssid) {
            // Update existing
            sn.password = password;
            sn.lastUsed = millis() / 1000;  // Simplified timestamp
            sn.autoConnect = autoConnect;
            wifiCredentialsSave();
            Serial.printf("[WiFiCred] Updated network: %s\n", ssid.c_str());
            return;
        }
    }

    // Add new network
    SavedNetwork sn;
    sn.ssid = ssid;
    sn.password = password;
    sn.lastUsed = millis() / 1000;
    sn.autoConnect = autoConnect;
    savedNetworks.push_back(sn);

    wifiCredentialsSave();
    Serial.printf("[WiFiCred] Added new network: %s\n", ssid.c_str());
}

void wifiCredentialsUpdateLastUsed(const String& ssid) {
    SavedNetwork* sn = wifiCredentialsGet(ssid);
    if (sn) {
        sn->lastUsed = millis() / 1000;
        wifiCredentialsSave();
    }
}

void wifiCredentialsSetAutoConnect(const String& ssid, bool autoConnect) {
    SavedNetwork* sn = wifiCredentialsGet(ssid);
    if (sn) {
        sn->autoConnect = autoConnect;
        wifiCredentialsSave();
        Serial.printf("[WiFiCred] Set autoConnect=%d for %s\n", autoConnect, ssid.c_str());
    }
}

bool wifiCredentialsForget(const String& ssid) {
    if (!credentialsLoaded) wifiCredentialsLoad();

    for (auto it = savedNetworks.begin(); it != savedNetworks.end(); ++it) {
        if (it->ssid == ssid) {
            savedNetworks.erase(it);
            wifiCredentialsSave();
            Serial.printf("[WiFiCred] Forgot network: %s\n", ssid.c_str());
            return true;
        }
    }
    return false;
}

const std::vector<SavedNetwork>& wifiCredentialsGetAll() {
    if (!credentialsLoaded) wifiCredentialsLoad();
    return savedNetworks;
}

std::vector<SavedNetwork*> wifiCredentialsGetAutoConnectNetworks(const std::vector<String>& visibleSSIDs) {
    if (!credentialsLoaded) wifiCredentialsLoad();

    std::vector<SavedNetwork*> result;

    for (SavedNetwork& sn : savedNetworks) {
        if (!sn.autoConnect) continue;

        // Check if this network is visible
        for (const String& visible : visibleSSIDs) {
            if (visible == sn.ssid) {
                result.push_back(&sn);
                break;
            }
        }
    }

    // Sort by lastUsed (most recent first)
    std::sort(result.begin(), result.end(), [](SavedNetwork* a, SavedNetwork* b) {
        return a->lastUsed > b->lastUsed;
    });

    return result;
}

size_t wifiCredentialsCount() {
    if (!credentialsLoaded) wifiCredentialsLoad();
    return savedNetworks.size();
}
