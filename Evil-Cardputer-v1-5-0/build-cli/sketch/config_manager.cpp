#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\config_manager.cpp"
/**
 * @file config_manager.cpp
 * @brief Implementation of ConfigManager class
 */

#include "config_manager.h"

// Static member initialization
bool ConfigManager::_initialized = false;

// ============================================================================
// Initialization
// ============================================================================

bool ConfigManager::init() {
    if (_initialized) {
        return true;
    }

    // Ensure config folder exists
    if (!ensureConfigFolder()) {
        Serial.println(F("[ConfigManager] Failed to create config folder"));
        return false;
    }

    _initialized = true;
    Serial.println(F("[ConfigManager] Initialized"));
    return true;
}

bool ConfigManager::isInitialized() {
    return _initialized;
}

bool ConfigManager::ensureConfigFolder() {
    if (!SD.exists(CONFIG_FOLDER_PATH)) {
        if (!SD.mkdir(CONFIG_FOLDER_PATH)) {
            // Try creating parent folder first
            if (!SD.exists("/evil")) {
                SD.mkdir("/evil");
            }
            if (!SD.mkdir(CONFIG_FOLDER_PATH)) {
                return false;
            }
        }
    }
    return true;
}

// ============================================================================
// Internal Helpers
// ============================================================================

String ConfigManager::readConfigFile() {
    String content = "";

    if (!SD.exists(CONFIG_FILE_PATH)) {
        return content;
    }

    File configFile = SD.open(CONFIG_FILE_PATH, FILE_READ);
    if (!configFile) {
        Serial.println(F("[ConfigManager] Error opening config file for reading"));
        return content;
    }

    while (configFile.available()) {
        content += configFile.readStringUntil('\n') + '\n';
    }
    configFile.close();

    return content;
}

bool ConfigManager::writeConfigFile(const String& content) {
    ensureConfigFolder();

    File configFile = SD.open(CONFIG_FILE_PATH, FILE_WRITE);
    if (!configFile) {
        Serial.println(F("[ConfigManager] Error opening config file for writing"));
        return false;
    }

    configFile.print(content);
    configFile.close();
    return true;
}

int ConfigManager::findKeyPosition(const String& content, const String& key) {
    String searchKey = key + "=";
    int pos = content.indexOf(searchKey);

    // Make sure it's at start of line or start of content
    while (pos > 0) {
        char prevChar = content.charAt(pos - 1);
        if (prevChar == '\n') {
            break;  // Found at start of line
        }
        // Search for next occurrence
        pos = content.indexOf(searchKey, pos + 1);
    }

    return pos;
}

// ============================================================================
// Save Operations
// ============================================================================

bool ConfigManager::saveInt(const String& key, int value) {
    return saveString(key, String(value));
}

bool ConfigManager::saveBool(const String& key, bool value) {
    return saveInt(key, value ? 1 : 0);
}

bool ConfigManager::saveString(const String& key, const String& value) {
    ensureConfigFolder();

    String content = readConfigFile();
    String newLine = key + "=" + value;

    int startPos = findKeyPosition(content, key);
    if (startPos != -1) {
        // Key exists, replace the line
        int endPos = content.indexOf('\n', startPos);
        if (endPos == -1) {
            endPos = content.length();
        }
        String oldLine = content.substring(startPos, endPos);
        content.replace(oldLine, newLine);
    } else {
        // Key doesn't exist, append
        if (content.length() > 0 && !content.endsWith("\n")) {
            content += "\n";
        }
        content += newLine + "\n";
    }

    if (writeConfigFile(content)) {
        Serial.println("[ConfigManager] Saved: " + key);
        return true;
    }
    return false;
}

// ============================================================================
// Load Operations
// ============================================================================

int ConfigManager::loadInt(const String& key, int defaultValue) {
    String strValue = loadString(key, "");
    if (strValue.length() == 0) {
        return defaultValue;
    }
    return strValue.toInt();
}

bool ConfigManager::loadBool(const String& key, bool defaultValue) {
    String strValue = loadString(key, "");
    if (strValue.length() == 0) {
        return defaultValue;
    }
    strValue.toLowerCase();
    return (strValue == "1" || strValue == "true" || strValue == "yes" || strValue == "on");
}

String ConfigManager::loadString(const String& key, const String& defaultValue) {
    if (!SD.exists(CONFIG_FILE_PATH)) {
        return defaultValue;
    }

    File configFile = SD.open(CONFIG_FILE_PATH, FILE_READ);
    if (!configFile) {
        return defaultValue;
    }

    String searchKey = key + "=";
    String result = defaultValue;

    while (configFile.available()) {
        String line = configFile.readStringUntil('\n');
        line.trim();

        if (line.startsWith(searchKey)) {
            result = line.substring(searchKey.length());
            result.trim();
            break;
        }
    }

    configFile.close();
    return result;
}

bool ConfigManager::hasKey(const String& key) {
    String content = readConfigFile();
    return findKeyPosition(content, key) != -1;
}

// ============================================================================
// Utility Operations
// ============================================================================

bool ConfigManager::removeKey(const String& key) {
    String content = readConfigFile();

    int startPos = findKeyPosition(content, key);
    if (startPos == -1) {
        return false;  // Key not found
    }

    int endPos = content.indexOf('\n', startPos);
    if (endPos == -1) {
        endPos = content.length();
    } else {
        endPos++;  // Include the newline
    }

    content = content.substring(0, startPos) + content.substring(endPos);
    return writeConfigFile(content);
}

const char* ConfigManager::getConfigPath() {
    return CONFIG_FILE_PATH;
}

const char* ConfigManager::getConfigFolder() {
    return CONFIG_FOLDER_PATH;
}

void ConfigManager::reload() {
    // Currently no caching, so nothing to do
    // Future: could implement memory cache and invalidate here
}

// ============================================================================
// Legacy Compatibility Functions
// ============================================================================

// This is moved from the .ino file - saves integer config parameter
void saveConfigParameter(String key, int value) {
    ConfigManager::saveInt(key, value);
}

// String save functions - these have special handling for specific keys
void savePortalFileConfig(const String& pathIn) {
    String portalPath = pathIn;
    if (!portalPath.startsWith("/evil/sites/")) {
        portalPath = "/evil/sites/" + portalPath;
    }

    if (!SD.exists(portalPath)) {
        Serial.println("[ConfigManager] savePortalFileConfig: portal not found -> " + portalPath);
        return;
    }

    ConfigManager::saveString("portal_file", portalPath);
    Serial.println("[ConfigManager] portal_file saved: " + portalPath);
}

void saveClonedSSIDConfig(const String& ssid) {
    if (ssid.length() == 0) {
        Serial.println(F("[ConfigManager] saveClonedSSIDConfig: empty SSID -> abort"));
        return;
    }

    ConfigManager::saveString("cloned_ssid", ssid);
    Serial.println("[ConfigManager] cloned_ssid saved: " + ssid);
}

void savePasswordConfig(const String& pass) {
    ConfigManager::saveString("portal_password", pass);
    Serial.println(F("[ConfigManager] portal_password saved"));
}
