/*
 * file_editor.cpp - Enhanced File Manager and Text Editor
 */

#include "file_editor.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <vector>

// ============================================================================
// Editor State
// ============================================================================

static struct {
    std::vector<String> lines;      // File content as lines
    int cursorLine;                 // Current line (0-indexed)
    int cursorCol;                  // Current column (0-indexed)
    int viewTopLine;                // First visible line
    int viewLeftCol;                // First visible column (horizontal scroll)
    bool modified;                  // Has content changed?
    String filePath;                // Full path to file
    String fileName;                // Just the filename
} editor;

// ============================================================================
// File Browser State
// ============================================================================

static String currentPath = "/";
static int browserCursor = 0;
static int browserTopIndex = 0;

// ============================================================================
// Forward Declarations
// ============================================================================

static void editorRender();
static void editorHandleInput();
static bool editorSave();
static void browserRender(const std::vector<String>& entries, const std::vector<bool>& isFolder);
static void showHelp();

// ============================================================================
// Utility Functions
// ============================================================================

static String getFileName(const String& path) {
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash >= 0 && lastSlash < path.length() - 1) {
        return path.substring(lastSlash + 1);
    }
    return path;
}

// ============================================================================
// Text Editor Implementation
// ============================================================================

bool textEditorOpen(const char* path) {
    // Initialize editor state
    editor.lines.clear();
    editor.cursorLine = 0;
    editor.cursorCol = 0;
    editor.viewTopLine = 0;
    editor.viewLeftCol = 0;
    editor.modified = false;
    editor.filePath = String(path);
    editor.fileName = getFileName(editor.filePath);

    // Load file
    File f = SD.open(path, FILE_READ);
    if (!f) {
        // New file - start with empty line
        editor.lines.push_back("");
    } else {
        while (f.available()) {
            String line = f.readStringUntil('\n');
            if (line.endsWith("\r")) {
                line.remove(line.length() - 1);
            }
            editor.lines.push_back(line);
        }
        f.close();

        if (editor.lines.empty()) {
            editor.lines.push_back("");
        }
    }

    // Editor main loop
    bool exitEditor = false;
    bool needRender = true;

    while (!exitEditor) {
        if (needRender) {
            editorRender();
            needRender = false;
        }

        M5Cardputer.update();
        M5.update();

        if (!M5Cardputer.Keyboard.isChange()) {
            delay(20);
            continue;
        }

        if (!M5Cardputer.Keyboard.isPressed()) {
            delay(10);
            continue;
        }

        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        // FN + key combinations
        if (status.fn) {
            // FN + S = Save
            if (M5Cardputer.Keyboard.isKeyPressed('s')) {
                if (editorSave()) {
                    // Show save confirmation briefly
                    M5.Display.fillRect(0, 0, FE_SCREEN_WIDTH, FE_HEADER_HEIGHT, TFT_GREEN);
                    M5.Display.setTextColor(TFT_BLACK);
                    M5.Display.setCursor(5, 2);
                    M5.Display.print("Saved!");
                    delay(500);
                } else {
                    M5.Display.fillRect(0, 0, FE_SCREEN_WIDTH, FE_HEADER_HEIGHT, TFT_RED);
                    M5.Display.setTextColor(TFT_WHITE);
                    M5.Display.setCursor(5, 2);
                    M5.Display.print("Save failed!");
                    delay(1000);
                }
                needRender = true;
                delay(100);
                continue;
            }

            // FN + Q = Quit (with save prompt if modified)
            if (M5Cardputer.Keyboard.isKeyPressed('q')) {
                if (editor.modified) {
                    if (confirmPopup("Save changes?")) {
                        editorSave();
                    }
                }
                exitEditor = true;
                delay(100);
                continue;
            }

            // FN + H = Help
            if (M5Cardputer.Keyboard.isKeyPressed('h')) {
                showHelp();
                needRender = true;
                delay(100);
                continue;
            }

            delay(100);
            continue;
        }

        // Navigation keys
        // Up: ;
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            if (editor.cursorLine > 0) {
                editor.cursorLine--;
                // Adjust column if new line is shorter
                if (editor.cursorCol > editor.lines[editor.cursorLine].length()) {
                    editor.cursorCol = editor.lines[editor.cursorLine].length();
                }
                // Scroll view if needed
                if (editor.cursorLine < editor.viewTopLine) {
                    editor.viewTopLine = editor.cursorLine;
                }
            }
            needRender = true;
            delay(80);
            continue;
        }

        // Down: .
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            if (editor.cursorLine < editor.lines.size() - 1) {
                editor.cursorLine++;
                if (editor.cursorCol > editor.lines[editor.cursorLine].length()) {
                    editor.cursorCol = editor.lines[editor.cursorLine].length();
                }
                if (editor.cursorLine >= editor.viewTopLine + FE_MAX_VISIBLE_LINES) {
                    editor.viewTopLine = editor.cursorLine - FE_MAX_VISIBLE_LINES + 1;
                }
            }
            needRender = true;
            delay(80);
            continue;
        }

        // Left: ,
        if (M5Cardputer.Keyboard.isKeyPressed(',')) {
            if (editor.cursorCol > 0) {
                editor.cursorCol--;
                if (editor.cursorCol < editor.viewLeftCol) {
                    editor.viewLeftCol = editor.cursorCol;
                }
            } else if (editor.cursorLine > 0) {
                // Go to end of previous line
                editor.cursorLine--;
                editor.cursorCol = editor.lines[editor.cursorLine].length();
                if (editor.cursorLine < editor.viewTopLine) {
                    editor.viewTopLine = editor.cursorLine;
                }
            }
            needRender = true;
            delay(80);
            continue;
        }

        // Right: /
        if (M5Cardputer.Keyboard.isKeyPressed('/')) {
            if (editor.cursorCol < editor.lines[editor.cursorLine].length()) {
                editor.cursorCol++;
                if (editor.cursorCol >= editor.viewLeftCol + FE_MAX_VISIBLE_COLS) {
                    editor.viewLeftCol = editor.cursorCol - FE_MAX_VISIBLE_COLS + 1;
                }
            } else if (editor.cursorLine < editor.lines.size() - 1) {
                // Go to start of next line
                editor.cursorLine++;
                editor.cursorCol = 0;
                editor.viewLeftCol = 0;
                if (editor.cursorLine >= editor.viewTopLine + FE_MAX_VISIBLE_LINES) {
                    editor.viewTopLine = editor.cursorLine - FE_MAX_VISIBLE_LINES + 1;
                }
            }
            needRender = true;
            delay(80);
            continue;
        }

        // Enter - new line
        if (status.enter) {
            String& currentLine = editor.lines[editor.cursorLine];
            String afterCursor = currentLine.substring(editor.cursorCol);
            currentLine = currentLine.substring(0, editor.cursorCol);

            editor.cursorLine++;
            editor.lines.insert(editor.lines.begin() + editor.cursorLine, afterCursor);
            editor.cursorCol = 0;
            editor.viewLeftCol = 0;
            editor.modified = true;

            if (editor.cursorLine >= editor.viewTopLine + FE_MAX_VISIBLE_LINES) {
                editor.viewTopLine++;
            }

            needRender = true;
            delay(100);
            continue;
        }

        // Backspace - delete character
        if (status.del) {
            if (editor.cursorCol > 0) {
                String& line = editor.lines[editor.cursorLine];
                line = line.substring(0, editor.cursorCol - 1) + line.substring(editor.cursorCol);
                editor.cursorCol--;
                editor.modified = true;
            } else if (editor.cursorLine > 0) {
                // Merge with previous line
                int prevLen = editor.lines[editor.cursorLine - 1].length();
                editor.lines[editor.cursorLine - 1] += editor.lines[editor.cursorLine];
                editor.lines.erase(editor.lines.begin() + editor.cursorLine);
                editor.cursorLine--;
                editor.cursorCol = prevLen;
                editor.modified = true;

                if (editor.cursorLine < editor.viewTopLine) {
                    editor.viewTopLine = editor.cursorLine;
                }
            }
            needRender = true;
            delay(80);
            continue;
        }

        // Tab - insert spaces
        if (M5Cardputer.Keyboard.isKeyPressed('\t')) {
            String& line = editor.lines[editor.cursorLine];
            line = line.substring(0, editor.cursorCol) + "    " + line.substring(editor.cursorCol);
            editor.cursorCol += 4;
            editor.modified = true;
            needRender = true;
            delay(100);
            continue;
        }

        // Printable characters
        for (auto ch : status.word) {
            if (ch >= 0x20 && ch <= 0x7E) {
                String& line = editor.lines[editor.cursorLine];
                line = line.substring(0, editor.cursorCol) + String((char)ch) + line.substring(editor.cursorCol);
                editor.cursorCol++;
                editor.modified = true;
                needRender = true;
            }
        }

        delay(30);
    }

    return editor.modified;
}

static void editorRender() {
    M5.Display.fillScreen(TFT_BLACK);

    // Header
    M5.Display.fillRect(0, 0, FE_SCREEN_WIDTH, FE_HEADER_HEIGHT, TFT_DARKGREY);
    M5.Display.setTextColor(editor.modified ? TFT_YELLOW : TFT_WHITE);
    M5.Display.setCursor(2, 2);

    String header = editor.fileName;
    if (editor.modified) header += "*";
    header += " L" + String(editor.cursorLine + 1) + ":" + String(editor.cursorCol + 1);
    if (header.length() > 38) header = header.substring(0, 35) + "...";
    M5.Display.print(header);

    // Content
    M5.Display.setTextColor(TFT_WHITE);
    for (int i = 0; i < FE_MAX_VISIBLE_LINES; i++) {
        int lineIdx = editor.viewTopLine + i;
        if (lineIdx >= editor.lines.size()) break;

        int y = FE_HEADER_HEIGHT + 2 + i * FE_LINE_HEIGHT;
        String& line = editor.lines[lineIdx];

        // Get visible portion
        String visible;
        if (editor.viewLeftCol < line.length()) {
            visible = line.substring(editor.viewLeftCol);
            if (visible.length() > FE_MAX_VISIBLE_COLS) {
                visible = visible.substring(0, FE_MAX_VISIBLE_COLS);
            }
        }

        // Draw cursor line highlight
        if (lineIdx == editor.cursorLine) {
            M5.Display.fillRect(0, y - 1, FE_SCREEN_WIDTH, FE_LINE_HEIGHT, TFT_NAVY);
        }

        M5.Display.setCursor(2, y);
        M5.Display.print(visible);

        // Draw cursor
        if (lineIdx == editor.cursorLine) {
            int cursorX = 2 + (editor.cursorCol - editor.viewLeftCol) * FE_CHAR_WIDTH;
            if (cursorX >= 0 && cursorX < FE_SCREEN_WIDTH) {
                M5.Display.fillRect(cursorX, y - 1, 2, FE_LINE_HEIGHT, TFT_GREEN);
            }
        }
    }

    // Footer with shortcuts
    M5.Display.fillRect(0, FE_SCREEN_HEIGHT - 10, FE_SCREEN_WIDTH, 10, TFT_DARKGREY);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(2, FE_SCREEN_HEIGHT - 9);
    M5.Display.print("FN+S:Save FN+Q:Quit FN+H:Help");
}

static bool editorSave() {
    File f = SD.open(editor.filePath.c_str(), FILE_WRITE);
    if (!f) return false;

    for (int i = 0; i < editor.lines.size(); i++) {
        f.print(editor.lines[i]);
        if (i < editor.lines.size() - 1) {
            f.print("\n");
        }
    }
    f.close();
    editor.modified = false;
    return true;
}

static void showHelp() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(5, 5);
    M5.Display.println("=== Editor Help ===");
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.println();
    M5.Display.println(";/.    Up/Down");
    M5.Display.println(",//    Left/Right");
    M5.Display.println("Enter  New line");
    M5.Display.println("Bksp   Delete");
    M5.Display.println();
    M5.Display.println("FN+S   Save");
    M5.Display.println("FN+Q   Quit");
    M5.Display.println();
    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.println("Press any key...");

    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) break;
        delay(50);
    }
    delay(150);
}

// ============================================================================
// Create File/Folder
// ============================================================================

bool createNewFile(const char* dirPath) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(5, 5);
    M5.Display.println("Create new file:");
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(5, 20);
    M5.Display.print("> ");

    String name = getUserInput(false);
    if (name.length() == 0) return false;

    // Ensure .txt extension
    if (!name.endsWith(".txt")) {
        name += ".txt";
    }

    String fullPath = String(dirPath);
    if (!fullPath.endsWith("/")) fullPath += "/";
    fullPath += name;

    // Create empty file
    File f = SD.open(fullPath.c_str(), FILE_WRITE);
    if (!f) {
        waitAndReturnToMenu("Failed to create file");
        return false;
    }
    f.close();

    // Open in editor
    textEditorOpen(fullPath.c_str());
    return true;
}

bool createNewFolder(const char* dirPath) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(5, 5);
    M5.Display.println("Create new folder:");
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(5, 20);
    M5.Display.print("> ");

    String name = getUserInput(false);
    if (name.length() == 0) return false;

    String fullPath = String(dirPath);
    if (!fullPath.endsWith("/")) fullPath += "/";
    fullPath += name;

    if (!SD.mkdir(fullPath.c_str())) {
        waitAndReturnToMenu("Failed to create folder");
        return false;
    }

    return true;
}

// ============================================================================
// File Browser Implementation
// ============================================================================

static void browserRender(const std::vector<String>& entries, const std::vector<bool>& isFolder) {
    M5.Display.fillScreen(TFT_BLACK);

    // Header
    M5.Display.fillRect(0, 0, FE_SCREEN_WIDTH, FE_HEADER_HEIGHT, TFT_DARKGREY);
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.setCursor(2, 2);

    String header = currentPath;
    if (header.length() > 36) header = "..." + header.substring(header.length() - 33);
    M5.Display.print(header);

    // Entries
    int total = entries.size();
    for (int i = 0; i < FE_MAX_VISIBLE_LINES && (browserTopIndex + i) < total; i++) {
        int idx = browserTopIndex + i;
        int y = FE_HEADER_HEIGHT + 2 + i * FE_LINE_HEIGHT;

        String name = entries[idx];
        bool folder = isFolder[idx];

        if (folder && name != "..") name += "/";

        // Highlight selected
        if (idx == browserCursor) {
            M5.Display.fillRect(0, y - 1, FE_SCREEN_WIDTH, FE_LINE_HEIGHT, TFT_NAVY);
            M5.Display.setTextColor(TFT_GREEN);
        } else if (folder) {
            M5.Display.setTextColor(TFT_CYAN);
        } else if (name.endsWith(".txt")) {
            M5.Display.setTextColor(TFT_WHITE);
        } else {
            M5.Display.setTextColor(TFT_DARKGREY);
        }

        M5.Display.setCursor(2, y);
        if (name.length() > 38) name = name.substring(0, 35) + "...";
        M5.Display.print(name);
    }

    // Footer
    M5.Display.fillRect(0, FE_SCREEN_HEIGHT - 10, FE_SCREEN_WIDTH, 10, TFT_DARKGREY);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(2, FE_SCREEN_HEIGHT - 9);
    M5.Display.print("E:Edit N:New D:Del M:Mkdir");
}

void fileEditorMain() {
    currentPath = "/";
    browserCursor = 0;
    browserTopIndex = 0;

    bool running = true;

    while (running) {
        // Read directory
        std::vector<String> entries;
        std::vector<bool> isFolder;

        File dir = SD.open(currentPath.c_str());
        if (!dir) {
            waitAndReturnToMenu("Failed to open directory");
            return;
        }

        File entry;
        while ((entry = dir.openNextFile())) {
            entries.push_back(String(entry.name()));
            isFolder.push_back(entry.isDirectory());
            entry.close();
        }
        dir.close();

        // Add parent directory option
        if (currentPath != "/") {
            entries.insert(entries.begin(), "..");
            isFolder.insert(isFolder.begin(), true);
        }

        int total = entries.size();
        if (total == 0) {
            entries.push_back("(empty)");
            isFolder.push_back(false);
            total = 1;
        }

        browserCursor = constrain(browserCursor, 0, total - 1);
        browserTopIndex = constrain(browserTopIndex, 0, max(0, total - FE_MAX_VISIBLE_LINES));

        bool needRender = true;
        bool refreshDir = false;

        while (!refreshDir && running) {
            if (needRender) {
                browserRender(entries, isFolder);
                needRender = false;
            }

            M5Cardputer.update();
            M5.update();

            // Up: ;
            if (M5Cardputer.Keyboard.isKeyPressed(';')) {
                if (browserCursor > 0) {
                    browserCursor--;
                    if (browserCursor < browserTopIndex) {
                        browserTopIndex = browserCursor;
                    }
                } else {
                    browserCursor = total - 1;
                    browserTopIndex = max(0, total - FE_MAX_VISIBLE_LINES);
                }
                needRender = true;
                delay(100);
            }
            // Down: .
            else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
                if (browserCursor < total - 1) {
                    browserCursor++;
                    if (browserCursor >= browserTopIndex + FE_MAX_VISIBLE_LINES) {
                        browserTopIndex = browserCursor - FE_MAX_VISIBLE_LINES + 1;
                    }
                } else {
                    browserCursor = 0;
                    browserTopIndex = 0;
                }
                needRender = true;
                delay(100);
            }
            // Enter: open folder or file
            else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                String sel = entries[browserCursor];

                if (sel == "(empty)") {
                    delay(100);
                    continue;
                }

                if (sel == "..") {
                    int slash = currentPath.lastIndexOf('/');
                    currentPath = (slash > 0) ? currentPath.substring(0, slash) : "/";
                    browserCursor = 0;
                    browserTopIndex = 0;
                    refreshDir = true;
                } else if (isFolder[browserCursor]) {
                    currentPath += (currentPath == "/" ? "" : "/") + sel;
                    browserCursor = 0;
                    browserTopIndex = 0;
                    refreshDir = true;
                } else if (sel.endsWith(".txt")) {
                    String fullPath = currentPath + (currentPath == "/" ? "" : "/") + sel;
                    textEditorOpen(fullPath.c_str());
                    needRender = true;
                }
                delay(150);
            }
            // E: Edit (same as enter for .txt files)
            else if (M5Cardputer.Keyboard.isKeyPressed('e')) {
                String sel = entries[browserCursor];
                if (!isFolder[browserCursor] && sel.endsWith(".txt")) {
                    String fullPath = currentPath + (currentPath == "/" ? "" : "/") + sel;
                    textEditorOpen(fullPath.c_str());
                    needRender = true;
                }
                delay(150);
            }
            // N: New file
            else if (M5Cardputer.Keyboard.isKeyPressed('n')) {
                createNewFile(currentPath.c_str());
                refreshDir = true;
                delay(150);
            }
            // M: Make directory
            else if (M5Cardputer.Keyboard.isKeyPressed('m')) {
                createNewFolder(currentPath.c_str());
                refreshDir = true;
                delay(150);
            }
            // D: Delete
            else if (M5Cardputer.Keyboard.isKeyPressed('d')) {
                String sel = entries[browserCursor];
                if (sel != ".." && sel != "(empty)") {
                    String fullPath = currentPath + (currentPath == "/" ? "" : "/") + sel;
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Delete %s?", sel.c_str());

                    if (confirmPopup(buf)) {
                        if (isFolder[browserCursor]) {
                            SD.rmdir(fullPath.c_str());
                        } else {
                            SD.remove(fullPath.c_str());
                        }
                        browserCursor = max(0, browserCursor - 1);
                        refreshDir = true;
                    }
                    needRender = true;
                }
                delay(150);
            }
            // Backspace: Exit
            else if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                running = false;
            }

            delay(20);
        }
    }

    inMenu = true;
}
