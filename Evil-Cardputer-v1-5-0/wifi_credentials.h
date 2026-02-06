#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#include <Arduino.h>
#include <vector>

// Path to credentials file on SD card
#define WIFI_CREDENTIALS_PATH "/evil/wifi_credentials.json"

// Structure for a saved WiFi network
struct SavedNetwork {
    String ssid;
    String password;
    unsigned long lastUsed;    // Unix timestamp
    bool autoConnect;
};

// Initialize the credentials system (call in setup)
void wifiCredentialsInit();

// Load all saved networks from SD card
bool wifiCredentialsLoad();

// Save all networks to SD card
bool wifiCredentialsSave();

// Check if a network has saved credentials
bool wifiCredentialsHas(const String& ssid);

// Get password for a network (returns empty string if not found)
String wifiCredentialsGetPassword(const String& ssid);

// Get full network info (returns nullptr if not found)
SavedNetwork* wifiCredentialsGet(const String& ssid);

// Save or update network credentials
void wifiCredentialsSaveNetwork(const String& ssid, const String& password, bool autoConnect = false);

// Update last used timestamp
void wifiCredentialsUpdateLastUsed(const String& ssid);

// Set autoConnect flag for a network
void wifiCredentialsSetAutoConnect(const String& ssid, bool autoConnect);

// Remove a network from saved list
bool wifiCredentialsForget(const String& ssid);

// Get list of all saved networks
const std::vector<SavedNetwork>& wifiCredentialsGetAll();

// Get networks with autoConnect enabled that match visible SSIDs
std::vector<SavedNetwork*> wifiCredentialsGetAutoConnectNetworks(const std::vector<String>& visibleSSIDs);

// Get count of saved networks
size_t wifiCredentialsCount();

#endif // WIFI_CREDENTIALS_H
