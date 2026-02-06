/**
 * @file config_manager.h
 * @brief Centralized configuration management for Evil-Cardputer
 *
 * Provides unified interface for saving/loading configuration parameters
 * to/from SD card. Uses key-value format compatible with existing config.txt.
 *
 * Usage:
 *   ConfigManager::init();
 *   ConfigManager::saveInt("brightness", 128);
 *   int val = ConfigManager::loadInt("brightness", 255);
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <SD.h>

// Configuration paths
#define CONFIG_FOLDER_PATH "/evil/config"
#define CONFIG_FILE_PATH   "/evil/config/config.txt"

/**
 * @class ConfigManager
 * @brief Singleton configuration manager for persistent settings
 *
 * Handles all read/write operations to config.txt on SD card.
 * Thread-safe for single-writer scenarios.
 */
class ConfigManager {
public:
    /**
     * @brief Initialize the configuration manager
     * @return true if SD card is accessible and folder exists/created
     */
    static bool init();

    /**
     * @brief Check if config manager is initialized
     * @return true if init() was successful
     */
    static bool isInitialized();

    // =========================================================================
    // Save operations (write to SD)
    // =========================================================================

    /**
     * @brief Save an integer value
     * @param key Configuration key name
     * @param value Integer value to save
     * @return true on success
     */
    static bool saveInt(const String& key, int value);

    /**
     * @brief Save a string value
     * @param key Configuration key name
     * @param value String value to save
     * @return true on success
     */
    static bool saveString(const String& key, const String& value);

    /**
     * @brief Save a boolean value (stored as 0 or 1)
     * @param key Configuration key name
     * @param value Boolean value to save
     * @return true on success
     */
    static bool saveBool(const String& key, bool value);

    // =========================================================================
    // Load operations (read from SD)
    // =========================================================================

    /**
     * @brief Load an integer value
     * @param key Configuration key name
     * @param defaultValue Value to return if key not found
     * @return Loaded value or default
     */
    static int loadInt(const String& key, int defaultValue = 0);

    /**
     * @brief Load a string value
     * @param key Configuration key name
     * @param defaultValue Value to return if key not found
     * @return Loaded value or default
     */
    static String loadString(const String& key, const String& defaultValue = "");

    /**
     * @brief Load a boolean value
     * @param key Configuration key name
     * @param defaultValue Value to return if key not found
     * @return Loaded value or default
     */
    static bool loadBool(const String& key, bool defaultValue = false);

    /**
     * @brief Check if a key exists in config
     * @param key Configuration key name
     * @return true if key exists
     */
    static bool hasKey(const String& key);

    // =========================================================================
    // Utility operations
    // =========================================================================

    /**
     * @brief Remove a key from config
     * @param key Configuration key name
     * @return true on success
     */
    static bool removeKey(const String& key);

    /**
     * @brief Get the config file path
     * @return Path to config.txt
     */
    static const char* getConfigPath();

    /**
     * @brief Get the config folder path
     * @return Path to config folder
     */
    static const char* getConfigFolder();

    /**
     * @brief Reload config from file (clears cache if any)
     */
    static void reload();

private:
    static bool _initialized;

    // Internal helper to read entire config file content
    static String readConfigFile();

    // Internal helper to write entire config file content
    static bool writeConfigFile(const String& content);

    // Internal helper to find key in content
    static int findKeyPosition(const String& content, const String& key);

    // Ensure config folder exists
    static bool ensureConfigFolder();
};

// ============================================================================
// Backward compatibility aliases (for gradual migration)
// ============================================================================

/**
 * @brief Legacy function - saves integer config parameter
 * @deprecated Use ConfigManager::saveInt() instead
 */
void saveConfigParameter(String key, int value);

/**
 * @brief Legacy function - restores config parameter and applies to global
 * @deprecated Use ConfigManager::loadInt/loadString() instead
 *
 * This function is kept for backward compatibility during migration.
 * It still updates the global variables directly.
 */
void restoreConfigParameter(String key);

// Special save functions for string values (kept for compatibility)
void savePortalFileConfig(const String& pathIn);
void saveClonedSSIDConfig(const String& ssid);
void savePasswordConfig(const String& pass);

#endif // CONFIG_MANAGER_H
