/*
 * file_editor.cpp - SD file manager and text/JSON editor
 */

#include "file_editor.h"
#include "display_config.h"
#include "display_runtime.h"
#include "gui/gui.h"
#include "input_compat.h"
#include "runtime_memory.h"
#include "scroll_input.h"
#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <vector>

using LB = GUI::LegacyBridge;

namespace {

constexpr size_t FE_MAX_FILE_BYTES = 128U * 1024U;
constexpr size_t FE_MAX_RESIDENT_BYTES = 48U * 1024U;
constexpr size_t FE_MAX_LINES = 1024;
constexpr size_t FE_MAX_DIRECTORY_ENTRIES = 192;
constexpr size_t FE_MEMORY_RESERVE = 36U * 1024U;
constexpr int FE_JSON_GUTTER_WIDTH = 25;

struct EditorState {
    std::vector<String> lines;
    int cursorLine = 0;
    int cursorCol = 0;
    int viewTopLine = 0;
    int viewLeftCol = 0;
    bool modified = false;
    bool savedThisSession = false;
    bool jsonMode = false;
    bool jsonValid = true;
    bool lastSaveApplied = false;
    String filePath;
    String fileName;
    String lastError;
};

EditorState editor;
String currentPath = "/";
int browserCursor = 0;
int browserTopIndex = 0;

void editorRender();
bool editorSave();
void browserRender(const std::vector<String>& entries,
                   const std::vector<bool>& isFolder);
void showHelp();

void feFlashHeader(const char* text, uint16_t bgColor, uint16_t fgColor,
                   int ms) {
    LB::fillRect(0, 0, FE_SCREEN_WIDTH, FE_HEADER_HEIGHT, bgColor);
    LB::setTextColor(fgColor);
    LB::setCursor(5, 2);
    LB::print(text);
    delay(ms);
}

void feShowMessage(const char* title, const String& detail, uint16_t color,
                   int ms = 1800) {
    LB::fillScreen(TFT_BLACK);
    LB::setTextColor(color);
    LB::setCursor(4, 4);
    LB::println(title);
    LB::setTextColor(TFT_WHITE);
    int y = 20;
    for (int start = 0; start < static_cast<int>(detail.length()) &&
                        y < FE_SCREEN_HEIGHT - 10;
         start += 37, y += FE_LINE_HEIGHT) {
        LB::setCursor(4, y);
        LB::print(detail.substring(start, start + 37));
    }
    delay(ms);
}

void fePromptScreen(const char* label) {
    LB::fillScreen(TFT_BLACK);
    LB::setTextColor(TFT_CYAN);
    LB::setCursor(5, 5);
    LB::println(label);
    LB::setTextColor(TFT_WHITE);
    LB::setCursor(5, 20);
    LB::print("> ");
}

void feDrawHeader(const String& text, uint16_t color) {
    LB::fillRect(0, 0, FE_SCREEN_WIDTH, FE_HEADER_HEIGHT, TFT_DARKGREY);
    LB::setTextColor(color);
    LB::setCursor(2, 2);
    LB::print(text);
}

void feDrawFooter(const char* text) {
    LB::fillRect(0, FE_SCREEN_HEIGHT - 10, FE_SCREEN_WIDTH, 10, TFT_DARKGREY);
    LB::setTextColor(TFT_CYAN);
    LB::setCursor(2, FE_SCREEN_HEIGHT - 9);
    LB::print(text);
}

String getFileName(const String& path) {
    const int lastSlash = path.lastIndexOf('/');
    if (lastSlash >= 0 && lastSlash < static_cast<int>(path.length()) - 1) {
        return path.substring(lastSlash + 1);
    }
    return path;
}

bool hasExtension(const String& path, const char* extension) {
    String lowerPath(path);
    String lowerExtension(extension);
    lowerPath.toLowerCase();
    lowerExtension.toLowerCase();
    return lowerPath.endsWith(lowerExtension);
}

bool isEditableFile(const String& path) {
    return hasExtension(path, ".txt") || hasExtension(path, ".json");
}

bool isSystemJson(const String& path) {
    return path.equalsIgnoreCase(DISPLAY_CONFIG_PATH);
}

int editorContentX() {
    return editor.jsonMode ? FE_JSON_GUTTER_WIDTH : 2;
}

int editorVisibleCols() {
    return (FE_SCREEN_WIDTH - editorContentX() - 2) / FE_CHAR_WIDTH;
}

String makeIndent(int count) {
    String result;
    result.reserve(count);
    for (int i = 0; i < count; ++i) result += ' ';
    return result;
}

int leadingSpaces(const String& value) {
    int count = 0;
    while (count < static_cast<int>(value.length()) && value[count] == ' ') {
        ++count;
    }
    return count;
}

String trimLeft(const String& value) {
    int start = 0;
    while (start < static_cast<int>(value.length()) &&
           (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substring(start);
}

void editorSetText(const String& text) {
    editor.lines.clear();
    int start = 0;
    while (start <= static_cast<int>(text.length())) {
        const int newline = text.indexOf('\n', start);
        String line =
            newline < 0 ? text.substring(start) : text.substring(start, newline);
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        editor.lines.push_back(line);
        if (newline < 0) break;
        start = newline + 1;
        if (start == static_cast<int>(text.length())) {
            editor.lines.push_back("");
            break;
        }
    }
    if (editor.lines.empty()) editor.lines.push_back("");
}

bool editorBuildText(String& result, String& error) {
    result = "";
    size_t estimated = 0;
    for (const String& line : editor.lines) estimated += line.length() + 1;
    if (!result.reserve(estimated)) {
        error = "Not enough contiguous memory for editor text";
        return false;
    }
    for (size_t i = 0; i < editor.lines.size(); ++i) {
        result += editor.lines[i];
        if (i + 1 < editor.lines.size()) result += '\n';
    }
    return true;
}

size_t editorResidentLimit() {
    if (RuntimeMemory::externalAvailable()) return FE_MAX_FILE_BYTES;
    const RuntimeMemory::Snapshot memory = RuntimeMemory::snapshot();
    if (memory.freeInternal <= FE_MEMORY_RESERVE) return 0;
    size_t budget = memory.freeInternal - FE_MEMORY_RESERVE;
    if (budget > memory.largestInternal) budget = memory.largestInternal;
    budget /= 3;
    return budget < FE_MAX_RESIDENT_BYTES ? budget : FE_MAX_RESIDENT_BYTES;
}

bool validateJson(const String& source, String& error) {
    JsonDocument doc;
    const DeserializationError parseError = deserializeJson(doc, source);
    if (parseError) {
        error = String("JSON ") + parseError.c_str();
        return false;
    }
    return true;
}

bool formatJson(const String& source, String& formatted, String& error) {
    if (!RuntimeMemory::externalAvailable() && source.length() > 16384) {
        error = "JSON is too large to format in resident memory";
        return false;
    }
    JsonDocument doc;
    const DeserializationError parseError = deserializeJson(doc, source);
    if (parseError) {
        error = String("JSON ") + parseError.c_str();
        return false;
    }

    formatted = "";
    if (!formatted.reserve(source.length() + source.length() / 4 + 16)) {
        error = "Not enough memory for formatted JSON";
        return false;
    }
    if (serializeJsonPretty(doc, formatted) == 0) {
        error = "JSON formatting failed";
        return false;
    }
    return true;
}

void editorClampCursor() {
    if (editor.lines.empty()) editor.lines.push_back("");
    if (editor.cursorLine < 0) editor.cursorLine = 0;
    if (editor.cursorLine >= static_cast<int>(editor.lines.size())) {
        editor.cursorLine = static_cast<int>(editor.lines.size()) - 1;
    }
    const int lineLength = editor.lines[editor.cursorLine].length();
    if (editor.cursorCol < 0) editor.cursorCol = 0;
    if (editor.cursorCol > lineLength) editor.cursorCol = lineLength;
}

void editorEnsureCursorVisible() {
    editorClampCursor();
    if (editor.cursorLine < editor.viewTopLine) {
        editor.viewTopLine = editor.cursorLine;
    }
    if (editor.cursorLine >= editor.viewTopLine + FE_MAX_VISIBLE_LINES) {
        editor.viewTopLine =
            editor.cursorLine - FE_MAX_VISIBLE_LINES + 1;
    }

    const int visibleCols = editorVisibleCols();
    if (editor.cursorCol < editor.viewLeftCol) {
        editor.viewLeftCol = editor.cursorCol;
    }
    if (editor.cursorCol >= editor.viewLeftCol + visibleCols) {
        editor.viewLeftCol = editor.cursorCol - visibleCols + 1;
    }
    if (editor.viewLeftCol < 0) editor.viewLeftCol = 0;
}

bool editorFormatJson() {
    String formatted;
    String error;
    String original;
    if (!editorBuildText(original, error)) {
        editor.jsonValid = false;
        editor.lastError = error;
        return false;
    }
    if (!formatJson(original, formatted, error)) {
        editor.jsonValid = false;
        editor.lastError = error;
        return false;
    }

    if (formatted != original) editor.modified = true;
    editorSetText(formatted);
    editor.jsonValid = true;
    editor.lastError = "";
    editorClampCursor();
    editorEnsureCursorVisible();
    return true;
}

bool writeTextAtomically(const String& path, const String& data,
                         bool keepBackup, bool& hadOriginal, String& error) {
    const String tempPath = path + ".fe_tmp";
    const String backupPath = path + (keepBackup ? ".bak" : ".fe_bak");
    DisplayRuntime::ScopedSdDisplayRelease sdGuard;

    if (SD.exists(tempPath.c_str()) && !SD.remove(tempPath.c_str())) {
        error = "Cannot remove stale temporary file";
        return false;
    }

    File output = SD.open(tempPath.c_str(), FILE_WRITE);
    if (!output) {
        error = "Cannot create temporary file";
        return false;
    }

    const size_t written = output.print(data);
    output.flush();
    output.close();
    if (written != data.length()) {
        SD.remove(tempPath.c_str());
        error = "Incomplete SD write";
        return false;
    }

    hadOriginal = SD.exists(path.c_str());
    if (hadOriginal) {
        if (SD.exists(backupPath.c_str()) && !SD.remove(backupPath.c_str())) {
            SD.remove(tempPath.c_str());
            error = "Cannot rotate previous backup";
            return false;
        }
        if (!SD.rename(path.c_str(), backupPath.c_str())) {
            SD.remove(tempPath.c_str());
            error = "Cannot create file backup";
            return false;
        }
    }

    if (!SD.rename(tempPath.c_str(), path.c_str())) {
        if (hadOriginal) SD.rename(backupPath.c_str(), path.c_str());
        SD.remove(tempPath.c_str());
        error = "Cannot install saved file";
        return false;
    }

    if (!keepBackup && hadOriginal) SD.remove(backupPath.c_str());
    return true;
}

bool restoreSystemBackup(const String& path, bool hadOriginal, String& error) {
    const String backupPath = path + ".bak";
    DisplayRuntime::ScopedSdDisplayRelease sdGuard;

    if (SD.exists(path.c_str()) && !SD.remove(path.c_str())) {
        error = "Cannot remove rejected config";
        return false;
    }
    if (!hadOriginal) return true;
    if (!SD.exists(backupPath.c_str()) ||
        !SD.rename(backupPath.c_str(), path.c_str())) {
        error = "Cannot restore config backup";
        return false;
    }
    return true;
}

void drawJsonSegment(const String& line, int tokenStart, int tokenEnd,
                     uint16_t color, int y) {
    const int visibleStart = editor.viewLeftCol;
    const int visibleEnd = visibleStart + editorVisibleCols();
    int drawStart = tokenStart > visibleStart ? tokenStart : visibleStart;
    int drawEnd = tokenEnd < visibleEnd ? tokenEnd : visibleEnd;
    if (drawStart >= drawEnd) return;

    LB::setTextColor(color);
    LB::setCursor(editorContentX() +
                      (drawStart - visibleStart) * FE_CHAR_WIDTH,
                  y);
    LB::print(line.substring(drawStart, drawEnd));
}

void drawJsonLine(const String& line, int y) {
    const int length = line.length();
    int i = 0;
    while (i < length) {
        const char ch = line[i];
        if (ch == '"') {
            const int start = i++;
            bool escaped = false;
            while (i < length) {
                const char current = line[i++];
                if (current == '"' && !escaped) break;
                if (current == '\\' && !escaped) {
                    escaped = true;
                } else {
                    escaped = false;
                }
            }
            int probe = i;
            while (probe < length &&
                   (line[probe] == ' ' || line[probe] == '\t')) {
                ++probe;
            }
            drawJsonSegment(line, start, i,
                            probe < length && line[probe] == ':'
                                ? TFT_CYAN
                                : TFT_GREEN,
                            y);
        } else if ((ch >= '0' && ch <= '9') || ch == '-') {
            const int start = i++;
            while (i < length) {
                const char current = line[i];
                if ((current >= '0' && current <= '9') || current == '.' ||
                    current == 'e' || current == 'E' || current == '+' ||
                    current == '-') {
                    ++i;
                } else {
                    break;
                }
            }
            drawJsonSegment(line, start, i, TFT_YELLOW, y);
        } else if ((ch >= 'a' && ch <= 'z') ||
                   (ch >= 'A' && ch <= 'Z')) {
            const int start = i++;
            while (i < length &&
                   ((line[i] >= 'a' && line[i] <= 'z') ||
                    (line[i] >= 'A' && line[i] <= 'Z'))) {
                ++i;
            }
            drawJsonSegment(line, start, i, TFT_MAGENTA, y);
        } else {
            const uint16_t color =
                (ch == '{' || ch == '}' || ch == '[' || ch == ']' ||
                 ch == ':' || ch == ',')
                    ? TFT_ORANGE
                    : TFT_WHITE;
            drawJsonSegment(line, i, i + 1, color, y);
            ++i;
        }
    }
}

void editorInsertChar(char ch) {
    String& line = editor.lines[editor.cursorLine];
    if (editor.jsonMode) {
        const char next =
            editor.cursorCol < static_cast<int>(line.length())
                ? line[editor.cursorCol]
                : '\0';
        if ((ch == '}' || ch == ']' || ch == '"') && next == ch) {
            ++editor.cursorCol;
            editorEnsureCursorVisible();
            return;
        }

        char closing = '\0';
        if (ch == '{') closing = '}';
        if (ch == '[') closing = ']';
        if (ch == '"') closing = '"';
        if (closing != '\0') {
            line = line.substring(0, editor.cursorCol) + String(ch) +
                   String(closing) + line.substring(editor.cursorCol);
            ++editor.cursorCol;
            editor.modified = true;
            editor.jsonValid = true;
            editorEnsureCursorVisible();
            return;
        }
    }

    line = line.substring(0, editor.cursorCol) + String(ch) +
           line.substring(editor.cursorCol);
    ++editor.cursorCol;
    editor.modified = true;
    editorEnsureCursorVisible();
}

void editorInsertNewLine() {
    String& current = editor.lines[editor.cursorLine];
    const String before = current.substring(0, editor.cursorCol);
    const String after = current.substring(editor.cursorCol);

    if (!editor.jsonMode) {
        current = before;
        ++editor.cursorLine;
        editor.lines.insert(editor.lines.begin() + editor.cursorLine, after);
        editor.cursorCol = 0;
    } else {
        String beforeTrimmed(before);
        beforeTrimmed.trim();
        String afterTrimmed = trimLeft(after);
        const bool opensBlock =
            beforeTrimmed.endsWith("{") || beforeTrimmed.endsWith("[");
        const bool closesBlock =
            afterTrimmed.startsWith("}") || afterTrimmed.startsWith("]");
        const int baseIndent = leadingSpaces(before);

        current = before;
        ++editor.cursorLine;
        if (opensBlock && closesBlock) {
            editor.lines.insert(editor.lines.begin() + editor.cursorLine,
                                makeIndent(baseIndent + 2));
            editor.lines.insert(editor.lines.begin() + editor.cursorLine + 1,
                                makeIndent(baseIndent) + afterTrimmed);
            editor.cursorCol = baseIndent + 2;
        } else {
            const int indent = baseIndent + (opensBlock ? 2 : 0);
            editor.lines.insert(editor.lines.begin() + editor.cursorLine,
                                makeIndent(indent) + afterTrimmed);
            editor.cursorCol = indent;
        }
    }

    editor.modified = true;
    editor.viewLeftCol = 0;
    editorEnsureCursorVisible();
}

bool requestEditorExit() {
    if (!editor.modified) return true;
    if (!confirmPopup("Save changes?")) return true;
    if (editorSave()) return true;
    feShowMessage("Save failed", editor.lastError, TFT_RED);
    return false;
}

}  // namespace

bool textEditorOpen(const char* path) {
    editor = EditorState();
    editor.filePath = String(path);
    editor.fileName = getFileName(editor.filePath);
    editor.jsonMode = hasExtension(editor.filePath, ".json");

    bool fileExists = false;
    bool fileTooLarge = false;
    const size_t residentLimit = editorResidentLimit();
    {
        DisplayRuntime::ScopedSdDisplayRelease sdGuard;
        File input = SD.open(path, FILE_READ);
        if (input) {
            fileExists = true;
            if (input.size() > FE_MAX_FILE_BYTES ||
                input.size() > residentLimit) {
                fileTooLarge = true;
            } else {
                while (input.available()) {
                    String line = input.readStringUntil('\n');
                    if (line.endsWith("\r")) line.remove(line.length() - 1);
                    editor.lines.push_back(line);
                    if (editor.lines.size() >= FE_MAX_LINES) {
                        fileTooLarge = input.available() > 0;
                        break;
                    }
                }
            }
            input.close();
        }
    }

    if (fileTooLarge) {
        waitAndReturnToMenu(
            "File exceeds safe resident limit (" +
            String(static_cast<unsigned>(residentLimit / 1024U)) + " KB)");
        return false;
    }

    if (editor.lines.empty()) {
        editor.lines.push_back(editor.jsonMode ? "{}" : "");
        editor.modified = !fileExists || editor.jsonMode;
    }

    // JSON is validated on save and formatted only by explicit FN+F. Parsing
    // and pretty-printing on open previously held four full representations.

    bool exitEditor = false;
    bool needRender = true;
    while (!exitEditor) {
        if (needRender) {
            editorRender();
            needRender = false;
        }

        M5Cardputer.update();
        M5.update();
        ScrollInput::poll();
        const ScrollEvent wheel = ScrollInput::getMenuEvent();
        const bool wheelMoved = wheel == ScrollEvent::ScrollUp ||
                                wheel == ScrollEvent::ScrollDown;

        const bool enterPressed = InputCompat::isEnterPressed();
        const bool backPressed = InputCompat::isBackPressed();
        const bool keyboardEvent =
            M5Cardputer.Keyboard.isChange() &&
            M5Cardputer.Keyboard.isPressed();
        if (!keyboardEvent && !enterPressed && !backPressed && !wheelMoved) {
            delay(20);
            continue;
        }

        Keyboard_Class::KeysState status =
            M5Cardputer.Keyboard.keysState();

        if (backPressed) {
            if (requestEditorExit()) exitEditor = true;
            needRender = true;
            delay(120);
            continue;
        }

        if (status.fn) {
            if (M5Cardputer.Keyboard.isKeyPressed('s')) {
                if (editorSave()) {
                    feFlashHeader(editor.lastSaveApplied
                                      ? "Saved and applied"
                                      : "Saved",
                                  TFT_GREEN, TFT_BLACK, 650);
                } else {
                    feShowMessage("Save failed", editor.lastError, TFT_RED);
                }
                needRender = true;
                delay(100);
                continue;
            }
            if (M5Cardputer.Keyboard.isKeyPressed('f') &&
                editor.jsonMode) {
                if (editorFormatJson()) {
                    feFlashHeader("JSON formatted", TFT_GREEN, TFT_BLACK, 600);
                } else {
                    feShowMessage("Invalid JSON", editor.lastError, TFT_RED);
                }
                needRender = true;
                delay(100);
                continue;
            }
            if (M5Cardputer.Keyboard.isKeyPressed('q')) {
                if (requestEditorExit()) exitEditor = true;
                needRender = true;
                delay(100);
                continue;
            }
            if (M5Cardputer.Keyboard.isKeyPressed('h')) {
                showHelp();
                needRender = true;
                delay(100);
                continue;
            }
            delay(80);
            continue;
        }

        if (M5Cardputer.Keyboard.isKeyPressed(';') ||
            wheel == ScrollEvent::ScrollUp) {
            if (editor.cursorLine > 0) --editor.cursorLine;
            editorClampCursor();
            editorEnsureCursorVisible();
            needRender = true;
            delay(80);
            continue;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.') ||
            wheel == ScrollEvent::ScrollDown) {
            if (editor.cursorLine + 1 <
                static_cast<int>(editor.lines.size())) {
                ++editor.cursorLine;
            }
            editorClampCursor();
            editorEnsureCursorVisible();
            needRender = true;
            delay(80);
            continue;
        }
        if (M5Cardputer.Keyboard.isKeyPressed(',')) {
            if (editor.cursorCol > 0) {
                --editor.cursorCol;
            } else if (editor.cursorLine > 0) {
                --editor.cursorLine;
                editor.cursorCol = editor.lines[editor.cursorLine].length();
            }
            editorEnsureCursorVisible();
            needRender = true;
            delay(80);
            continue;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('/')) {
            if (editor.cursorCol <
                static_cast<int>(editor.lines[editor.cursorLine].length())) {
                ++editor.cursorCol;
            } else if (editor.cursorLine + 1 <
                       static_cast<int>(editor.lines.size())) {
                ++editor.cursorLine;
                editor.cursorCol = 0;
            }
            editorEnsureCursorVisible();
            needRender = true;
            delay(80);
            continue;
        }

        if (enterPressed || status.enter) {
            editorInsertNewLine();
            needRender = true;
            delay(100);
            continue;
        }

        if (status.del) {
            if (editor.cursorCol > 0) {
                String& line = editor.lines[editor.cursorLine];
                line = line.substring(0, editor.cursorCol - 1) +
                       line.substring(editor.cursorCol);
                --editor.cursorCol;
                editor.modified = true;
            } else if (editor.cursorLine > 0) {
                const int previousLength =
                    editor.lines[editor.cursorLine - 1].length();
                editor.lines[editor.cursorLine - 1] +=
                    editor.lines[editor.cursorLine];
                editor.lines.erase(editor.lines.begin() + editor.cursorLine);
                --editor.cursorLine;
                editor.cursorCol = previousLength;
                editor.modified = true;
            }
            editorEnsureCursorVisible();
            needRender = true;
            delay(80);
            continue;
        }

        if (M5Cardputer.Keyboard.isKeyPressed('\t')) {
            String& line = editor.lines[editor.cursorLine];
            const int spaces = editor.jsonMode ? 2 : 4;
            const String indentation = makeIndent(spaces);
            line = line.substring(0, editor.cursorCol) + indentation +
                   line.substring(editor.cursorCol);
            editor.cursorCol += spaces;
            editor.modified = true;
            editorEnsureCursorVisible();
            needRender = true;
            delay(100);
            continue;
        }

        for (auto ch : status.word) {
            if (ch >= 0x20 && ch <= 0x7E) {
                editorInsertChar(static_cast<char>(ch));
                needRender = true;
            }
        }
        delay(30);
    }

    return editor.savedThisSession;
}

namespace {

void editorRender() {
    LB::fillScreen(TFT_BLACK);

    String header = editor.jsonMode
                        ? (editor.jsonValid ? "JSON " : "JSON! ")
                        : "TXT ";
    header += editor.fileName;
    if (editor.modified) header += "*";
    header += " L" + String(editor.cursorLine + 1) + ":" +
              String(editor.cursorCol + 1);
    if (header.length() > 38) header = header.substring(0, 35) + "...";
    const uint16_t headerColor =
        editor.jsonMode && !editor.jsonValid
            ? TFT_RED
            : (editor.modified ? TFT_YELLOW : TFT_WHITE);
    feDrawHeader(header, headerColor);

    for (int i = 0; i < FE_MAX_VISIBLE_LINES; ++i) {
        const int lineIndex = editor.viewTopLine + i;
        if (lineIndex >= static_cast<int>(editor.lines.size())) break;
        const int y = FE_HEADER_HEIGHT + 2 + i * FE_LINE_HEIGHT;
        const String& line = editor.lines[lineIndex];

        if (lineIndex == editor.cursorLine) {
            LB::fillRect(0, y - 1, FE_SCREEN_WIDTH, FE_LINE_HEIGHT, TFT_NAVY);
        }

        if (editor.jsonMode) {
            char number[5];
            snprintf(number, sizeof(number), "%3d", lineIndex + 1);
            LB::setTextColor(TFT_DARKGREY);
            LB::setCursor(1, y);
            LB::print(number);
            drawJsonLine(line, y);
        } else {
            String visible;
            if (editor.viewLeftCol < static_cast<int>(line.length())) {
                visible = line.substring(editor.viewLeftCol);
                if (visible.length() >
                    static_cast<unsigned>(editorVisibleCols())) {
                    visible =
                        visible.substring(0, editorVisibleCols());
                }
            }
            LB::setTextColor(TFT_WHITE);
            LB::setCursor(editorContentX(), y);
            LB::print(visible);
        }

        if (lineIndex == editor.cursorLine) {
            const int cursorX =
                editorContentX() +
                (editor.cursorCol - editor.viewLeftCol) * FE_CHAR_WIDTH;
            if (cursorX >= editorContentX() && cursorX < FE_SCREEN_WIDTH) {
                LB::fillRect(cursorX, y - 1, 2, FE_LINE_HEIGHT, TFT_GREEN);
            }
        }
    }

    feDrawFooter(editor.jsonMode
                     ? "FN+S Save F Format Q Exit"
                     : "FN+S Save Q Exit H Help");
}

bool editorSave() {
    editor.lastError = "";
    editor.lastSaveApplied = false;
    String data;
    if (!editorBuildText(data, editor.lastError)) return false;

    if (editor.jsonMode) {
        if (!validateJson(data, editor.lastError)) {
            editor.jsonValid = false;
            return false;
        }
        editor.jsonValid = true;
    }

    const bool systemConfig = isSystemJson(editor.filePath);
    bool hadOriginal = false;
    if (!writeTextAtomically(editor.filePath, data, systemConfig, hadOriginal,
                             editor.lastError)) {
        return false;
    }

    if (systemConfig) {
        String applyError;
        if (!fileEditorApplySystemConfig(editor.filePath.c_str(), applyError)) {
            String restoreError;
            const bool restored =
                restoreSystemBackup(editor.filePath, hadOriginal, restoreError);
            String rollbackError;
            if (restored && hadOriginal) {
                fileEditorApplySystemConfig(editor.filePath.c_str(),
                                            rollbackError);
            }

            editor.lastError = "Apply failed: " + applyError;
            if (!restored) editor.lastError += "; " + restoreError;
            if (rollbackError.length() > 0) {
                editor.lastError += "; rollback: " + rollbackError;
            }
            return false;
        }
        editor.lastSaveApplied = true;
    }

    editor.modified = false;
    editor.savedThisSession = true;
    return true;
}

void showHelp() {
    LB::fillScreen(TFT_BLACK);
    LB::setTextColor(TFT_CYAN);
    LB::setCursor(5, 5);
    LB::println("Editor Help");
    LB::setTextColor(TFT_WHITE);
    LB::println(";/.   Up/Down");
    LB::println(",//   Left/Right");
    LB::println("Enter New line");
    LB::println("Del   Delete");
    LB::println("FN+S  Atomic save");
    LB::println("FN+F  Format JSON");
    LB::println("FN+Del/Q Exit");
    LB::setTextColor(TFT_DARKGREY);
    LB::println("Press any key...");

    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) break;
        delay(50);
    }
    delay(150);
}

void browserRender(const std::vector<String>& entries,
                   const std::vector<bool>& isFolder) {
    LB::fillScreen(TFT_BLACK);
    String header = currentPath;
    if (header.length() > 36) {
        header = "..." + header.substring(header.length() - 33);
    }
    feDrawHeader(header, TFT_YELLOW);

    const int total = entries.size();
    for (int i = 0;
         i < FE_MAX_VISIBLE_LINES && browserTopIndex + i < total; ++i) {
        const int index = browserTopIndex + i;
        const int y = FE_HEADER_HEIGHT + 2 + i * FE_LINE_HEIGHT;
        String name = entries[index];
        const bool folder = isFolder[index];
        if (folder && name != "..") name += "/";

        if (index == browserCursor) {
            LB::fillRect(0, y - 1, FE_SCREEN_WIDTH, FE_LINE_HEIGHT, TFT_NAVY);
            LB::setTextColor(TFT_GREEN);
        } else if (folder) {
            LB::setTextColor(TFT_CYAN);
        } else if (hasExtension(name, ".json")) {
            LB::setTextColor(TFT_YELLOW);
        } else if (hasExtension(name, ".txt")) {
            LB::setTextColor(TFT_WHITE);
        } else {
            LB::setTextColor(TFT_DARKGREY);
        }

        LB::setCursor(2, y);
        if (name.length() > 38) name = name.substring(0, 35) + "...";
        LB::print(name);
    }
    feDrawFooter("Enter/Edit N New D Del M Dir");
}

}  // namespace

bool createNewFile(const char* dirPath) {
    fePromptScreen("New file (.txt/.json):");
    String name = getUserInput(false);
    name.trim();
    if (name.length() == 0) return false;
    if (name == "." || name == ".." || name.indexOf('/') >= 0 ||
        name.indexOf('\\') >= 0 || name.indexOf(':') >= 0) {
        waitAndReturnToMenu("Invalid file name");
        return false;
    }
    if (name.lastIndexOf('.') < 0) name += ".txt";

    String fullPath(dirPath);
    if (!fullPath.endsWith("/")) fullPath += "/";
    fullPath += name;

    {
        DisplayRuntime::ScopedSdDisplayRelease sdGuard;
        if (SD.exists(fullPath.c_str())) {
            waitAndReturnToMenu("File already exists");
            return false;
        }
    }

    textEditorOpen(fullPath.c_str());
    return true;
}

bool createNewFolder(const char* dirPath) {
    fePromptScreen("Create new folder:");
    String name = getUserInput(false);
    name.trim();
    if (name.length() == 0) return false;
    if (name == "." || name == ".." || name.indexOf('/') >= 0 ||
        name.indexOf('\\') >= 0 || name.indexOf(':') >= 0) {
        waitAndReturnToMenu("Invalid folder name");
        return false;
    }

    String fullPath(dirPath);
    if (!fullPath.endsWith("/")) fullPath += "/";
    fullPath += name;

    bool created = false;
    {
        DisplayRuntime::ScopedSdDisplayRelease sdGuard;
        created = SD.mkdir(fullPath.c_str());
    }
    if (!created) {
        waitAndReturnToMenu("Failed to create folder");
        return false;
    }
    return true;
}

void fileEditorMain() {
    currentPath = "/";
    browserCursor = 0;
    browserTopIndex = 0;
    bool running = true;

    while (running) {
        std::vector<String> entries;
        std::vector<bool> isFolder;
        bool directoryOpened = false;
        bool directoryTruncated = false;
        {
            DisplayRuntime::ScopedSdDisplayRelease sdGuard;
            File directory = SD.open(currentPath.c_str());
            if (directory) {
                directoryOpened = true;
                File entry;
                while ((entry = directory.openNextFile())) {
                    if (entries.size() >= FE_MAX_DIRECTORY_ENTRIES) {
                        directoryTruncated = true;
                        entry.close();
                        break;
                    }
                    entries.push_back(String(entry.name()));
                    isFolder.push_back(entry.isDirectory());
                    entry.close();
                }
                directory.close();
            }
        }

        if (!directoryOpened) {
            waitAndReturnToMenu("Failed to open directory");
            return;
        }
        if (directoryTruncated) {
            Serial.printf("[FileEditor] directory limited to %u entries\n",
                          static_cast<unsigned>(FE_MAX_DIRECTORY_ENTRIES));
        }

        if (currentPath != "/") {
            entries.insert(entries.begin(), "..");
            isFolder.insert(isFolder.begin(), true);
        }
        if (entries.empty()) {
            entries.push_back("(empty)");
            isFolder.push_back(false);
        }

        const int total = entries.size();
        if (browserCursor < 0) browserCursor = 0;
        if (browserCursor >= total) browserCursor = total - 1;
        const int maxTop =
            total > FE_MAX_VISIBLE_LINES ? total - FE_MAX_VISIBLE_LINES : 0;
        if (browserTopIndex < 0) browserTopIndex = 0;
        if (browserTopIndex > maxTop) browserTopIndex = maxTop;

        bool needRender = true;
        bool refreshDirectory = false;
        while (!refreshDirectory && running) {
            if (needRender) {
                browserRender(entries, isFolder);
                needRender = false;
            }

            M5Cardputer.update();
            M5.update();
            ScrollInput::poll();
            const ScrollEvent wheel = ScrollInput::getMenuEvent();

            if (M5Cardputer.Keyboard.isKeyPressed(';') ||
                wheel == ScrollEvent::ScrollUp) {
                if (browserCursor > 0) {
                    --browserCursor;
                    if (browserCursor < browserTopIndex) {
                        browserTopIndex = browserCursor;
                    }
                } else {
                    browserCursor = total - 1;
                    browserTopIndex = maxTop;
                }
                needRender = true;
                delay(100);
            } else if (M5Cardputer.Keyboard.isKeyPressed('.') ||
                       wheel == ScrollEvent::ScrollDown) {
                if (browserCursor < total - 1) {
                    ++browserCursor;
                    if (browserCursor >=
                        browserTopIndex + FE_MAX_VISIBLE_LINES) {
                        browserTopIndex =
                            browserCursor - FE_MAX_VISIBLE_LINES + 1;
                    }
                } else {
                    browserCursor = 0;
                    browserTopIndex = 0;
                }
                needRender = true;
                delay(100);
            } else if (InputCompat::isEnterPressed()) {
                const String selected = entries[browserCursor];
                if (selected == "(empty)") {
                    delay(100);
                    continue;
                }
                if (selected == "..") {
                    const int slash = currentPath.lastIndexOf('/');
                    currentPath =
                        slash > 0 ? currentPath.substring(0, slash) : "/";
                    browserCursor = 0;
                    browserTopIndex = 0;
                    refreshDirectory = true;
                } else if (isFolder[browserCursor]) {
                    currentPath +=
                        (currentPath == "/" ? "" : "/") + selected;
                    browserCursor = 0;
                    browserTopIndex = 0;
                    refreshDirectory = true;
                } else if (isEditableFile(selected)) {
                    const String fullPath =
                        currentPath + (currentPath == "/" ? "" : "/") +
                        selected;
                    textEditorOpen(fullPath.c_str());
                    needRender = true;
                }
                delay(150);
            } else if (M5Cardputer.Keyboard.isKeyPressed('e')) {
                const String selected = entries[browserCursor];
                if (!isFolder[browserCursor] &&
                    isEditableFile(selected)) {
                    const String fullPath =
                        currentPath + (currentPath == "/" ? "" : "/") +
                        selected;
                    textEditorOpen(fullPath.c_str());
                    needRender = true;
                }
                delay(150);
            } else if (M5Cardputer.Keyboard.isKeyPressed('n')) {
                createNewFile(currentPath.c_str());
                refreshDirectory = true;
                delay(150);
            } else if (M5Cardputer.Keyboard.isKeyPressed('m')) {
                createNewFolder(currentPath.c_str());
                refreshDirectory = true;
                delay(150);
            } else if (M5Cardputer.Keyboard.isKeyPressed('d')) {
                const String selected = entries[browserCursor];
                if (selected != ".." && selected != "(empty)") {
                    const String fullPath =
                        currentPath + (currentPath == "/" ? "" : "/") +
                        selected;
                    const String prompt = "Delete " + selected + "?";
                    if (confirmPopup(prompt)) {
                        bool removed = false;
                        {
                            DisplayRuntime::ScopedSdDisplayRelease sdGuard;
                            removed = isFolder[browserCursor]
                                          ? SD.rmdir(fullPath.c_str())
                                          : SD.remove(fullPath.c_str());
                        }
                        if (!removed) {
                            feShowMessage("Delete failed", fullPath, TFT_RED);
                        }
                        if (browserCursor > 0) --browserCursor;
                        refreshDirectory = true;
                    }
                    needRender = true;
                }
                delay(150);
            } else if (InputCompat::isBackPressed()) {
                running = false;
            }
            delay(20);
        }
    }
    inMenu = true;
}
