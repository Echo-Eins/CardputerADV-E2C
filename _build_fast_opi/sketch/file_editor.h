#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\file_editor.h"
/*
 * file_editor.h - Enhanced File Manager and Text Editor
 *
 * Features:
 * - Browse SD card directories
 * - View/edit .txt files with scrolling
 * - Create files and folders
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

// Text editor for a specific file
// Returns true if file was modified and saved
bool textEditorOpen(const char* path);

// Create new text file
bool createNewFile(const char* dirPath);

// Create new folder
bool createNewFolder(const char* dirPath);

#endif // FILE_EDITOR_H
