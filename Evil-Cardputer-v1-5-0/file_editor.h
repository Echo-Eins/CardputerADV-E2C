/*
 * file_editor.h - Enhanced File Manager and text/JSON editor
 *
 * Features:
 * - Browse SD card directories
 * - View/edit .txt and .json files with scrolling
 * - Validate and pretty-format JSON
 * - Create text/JSON files and folders
 * - Delete files/folders
 * - Cursor-based text editing
 */

#ifndef FILE_EDITOR_H
#define FILE_EDITOR_H

#include <Arduino.h>
#include <vector>

// ============================================================================
// External dependencies
// ============================================================================

extern void waitAndReturnToMenu(String message);
extern bool confirmPopup(String message);
extern String getUserInput(bool isPassword);
extern bool inMenu;

// Implemented by the main firmware. Known system JSON files are reloaded
// after an atomic save. Returning false makes the editor restore the backup.
extern bool fileEditorApplySystemConfig(const char* path, String& error);

// ============================================================================
// Constants
// ============================================================================

#define FE_SCREEN_WIDTH         240
#define FE_SCREEN_HEIGHT        135
#define FE_LINE_HEIGHT          11
#define FE_CHAR_WIDTH           6
#define FE_MAX_VISIBLE_LINES    9
#define FE_MAX_VISIBLE_COLS     38
#define FE_HEADER_HEIGHT        13

// ============================================================================
// Public Functions
// ============================================================================

// Main entry point - enhanced file manager
void fileEditorMain();

// Text/JSON editor for a specific file
// Returns true if file was modified and saved
bool textEditorOpen(const char* path);

// Create a new .txt or .json file. Names without an extension become .txt.
bool createNewFile(const char* dirPath);

// Create new folder
bool createNewFolder(const char* dirPath);

#endif // FILE_EDITOR_H
