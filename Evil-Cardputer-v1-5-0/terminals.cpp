/*
 * terminals.cpp - REPL Terminals for Evil-Cardputer
 *
 * Expression evaluator with recursive descent parser.
 * Device shell with built-in commands.
 */

#include "terminals.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFi.h>
#include <map>
#include <cmath>
#include <deque>
#include "gui/gui.h"

using LB = GUI::LegacyBridge;

// ============================================================================
// Variables Storage
// ============================================================================

static std::map<String, double> variables;

void setVariable(const String& name, double value) {
    variables[name] = value;
}

double getVariable(const String& name) {
    auto it = variables.find(name);
    if (it != variables.end()) {
        return it->second;
    }
    return NAN;
}

void clearVariables() {
    variables.clear();
    // Built-in constants
    variables["pi"] = 3.14159265358979323846;
    variables["e"] = 2.71828182845904523536;
}

// ============================================================================
// Expression Parser (Recursive Descent)
// ============================================================================

class ExprParser {
public:
    ExprParser(const String& expr) : input(expr), pos(0), error("") {}

    double parse() {
        skipWhitespace();
        if (pos >= input.length()) {
            error = "Empty expression";
            return NAN;
        }
        double result = parseAssignment();
        skipWhitespace();
        if (pos < input.length()) {
            error = "Unexpected: " + String(input[pos]);
            return NAN;
        }
        return result;
    }

    String getError() { return error; }

private:
    String input;
    size_t pos;
    String error;

    char peek() {
        if (pos >= input.length()) return '\0';
        return input[pos];
    }

    char get() {
        if (pos >= input.length()) return '\0';
        return input[pos++];
    }

    void skipWhitespace() {
        while (pos < input.length() && isspace(input[pos])) pos++;
    }

    // assignment = identifier '=' expression | expression
    double parseAssignment() {
        size_t startPos = pos;
        skipWhitespace();

        // Try to parse as assignment
        if (isalpha(peek()) || peek() == '_') {
            String name = parseIdentifier();
            skipWhitespace();
            if (peek() == '=') {
                get(); // consume '='
                skipWhitespace();
                double value = parseExpression();
                if (!isnan(value)) {
                    setVariable(name, value);
                }
                return value;
            }
        }

        // Not an assignment, reset and parse as expression
        pos = startPos;
        return parseExpression();
    }

    // expression = term (('+' | '-') term)*
    double parseExpression() {
        double left = parseTerm();
        if (isnan(left)) return NAN;

        while (true) {
            skipWhitespace();
            char op = peek();
            if (op == '+' || op == '-') {
                get();
                double right = parseTerm();
                if (isnan(right)) return NAN;
                left = (op == '+') ? left + right : left - right;
            } else {
                break;
            }
        }
        return left;
    }

    // term = power (('*' | '/' | '%') power)*
    double parseTerm() {
        double left = parsePower();
        if (isnan(left)) return NAN;

        while (true) {
            skipWhitespace();
            char op = peek();
            if (op == '*' || op == '/' || op == '%') {
                get();
                double right = parsePower();
                if (isnan(right)) return NAN;
                if (op == '*') left *= right;
                else if (op == '/') {
                    if (right == 0) { error = "Division by zero"; return NAN; }
                    left /= right;
                }
                else left = fmod(left, right);
            } else {
                break;
            }
        }
        return left;
    }

    // power = unary ('^' power)?
    double parsePower() {
        double base = parseUnary();
        if (isnan(base)) return NAN;

        skipWhitespace();
        if (peek() == '^') {
            get();
            double exp = parsePower(); // Right-associative
            if (isnan(exp)) return NAN;
            return pow(base, exp);
        }
        return base;
    }

    // unary = ('-' | '+')? factor
    double parseUnary() {
        skipWhitespace();
        if (peek() == '-') {
            get();
            return -parseUnary();
        }
        if (peek() == '+') {
            get();
            return parseUnary();
        }
        return parseFactor();
    }

    // factor = number | identifier | function | '(' expression ')'
    double parseFactor() {
        skipWhitespace();

        // Parentheses
        if (peek() == '(') {
            get();
            double result = parseExpression();
            skipWhitespace();
            if (peek() != ')') {
                error = "Expected ')'";
                return NAN;
            }
            get();
            return result;
        }

        // Number
        if (isdigit(peek()) || peek() == '.') {
            return parseNumber();
        }

        // Identifier or function
        if (isalpha(peek()) || peek() == '_') {
            String name = parseIdentifier();
            skipWhitespace();

            // Function call
            if (peek() == '(') {
                get();
                skipWhitespace();

                std::vector<double> args;
                if (peek() != ')') {
                    args.push_back(parseExpression());
                    while (peek() == ',') {
                        get();
                        args.push_back(parseExpression());
                    }
                }
                skipWhitespace();
                if (peek() != ')') {
                    error = "Expected ')' in function call";
                    return NAN;
                }
                get();
                return callFunction(name, args);
            }

            // Variable
            double value = getVariable(name);
            if (isnan(value)) {
                error = "Unknown variable: " + name;
            }
            return value;
        }

        error = "Unexpected character: " + String(peek());
        return NAN;
    }

    double parseNumber() {
        String numStr = "";
        bool hasDecimal = false;

        while (isdigit(peek()) || peek() == '.') {
            if (peek() == '.') {
                if (hasDecimal) break;
                hasDecimal = true;
            }
            numStr += get();
        }

        // Scientific notation
        if (peek() == 'e' || peek() == 'E') {
            numStr += get();
            if (peek() == '+' || peek() == '-') numStr += get();
            while (isdigit(peek())) numStr += get();
        }

        return numStr.toDouble();
    }

    String parseIdentifier() {
        String name = "";
        while (isalnum(peek()) || peek() == '_') {
            name += get();
        }
        return name;
    }

    double callFunction(const String& name, const std::vector<double>& args) {
        // Math functions
        if (name == "sin" && args.size() == 1) return sin(args[0]);
        if (name == "cos" && args.size() == 1) return cos(args[0]);
        if (name == "tan" && args.size() == 1) return tan(args[0]);
        if (name == "asin" && args.size() == 1) return asin(args[0]);
        if (name == "acos" && args.size() == 1) return acos(args[0]);
        if (name == "atan" && args.size() == 1) return atan(args[0]);
        if (name == "atan2" && args.size() == 2) return atan2(args[0], args[1]);
        if (name == "sqrt" && args.size() == 1) return sqrt(args[0]);
        if (name == "cbrt" && args.size() == 1) return cbrt(args[0]);
        if (name == "pow" && args.size() == 2) return pow(args[0], args[1]);
        if (name == "exp" && args.size() == 1) return exp(args[0]);
        if (name == "log" && args.size() == 1) return log(args[0]);
        if (name == "log10" && args.size() == 1) return log10(args[0]);
        if (name == "log2" && args.size() == 1) return log2(args[0]);
        if (name == "abs" && args.size() == 1) return fabs(args[0]);
        if (name == "floor" && args.size() == 1) return floor(args[0]);
        if (name == "ceil" && args.size() == 1) return ceil(args[0]);
        if (name == "round" && args.size() == 1) return round(args[0]);
        if (name == "min" && args.size() == 2) return fmin(args[0], args[1]);
        if (name == "max" && args.size() == 2) return fmax(args[0], args[1]);
        if (name == "rad" && args.size() == 1) return args[0] * M_PI / 180.0;
        if (name == "deg" && args.size() == 1) return args[0] * 180.0 / M_PI;

        error = "Unknown function: " + name;
        return NAN;
    }
};

double evalExpression(const String& expr, String& error) {
    ExprParser parser(expr);
    double result = parser.parse();
    error = parser.getError();
    return result;
}

// ============================================================================
// Terminal Display State
// ============================================================================

static std::deque<String> outputLines;
static int outputScroll = 0;

static void termClear() {
    outputLines.clear();
    outputScroll = 0;
}

static void termPrint(const String& text, uint16_t color = TFT_WHITE) {
    // Word wrap long lines
    String remaining = text;
    while (remaining.length() > 38) {
        outputLines.push_back(remaining.substring(0, 38));
        remaining = remaining.substring(38);
    }
    if (remaining.length() > 0) {
        outputLines.push_back(remaining);
    }

    // Limit history
    while (outputLines.size() > TERM_MAX_OUTPUT_LINES) {
        outputLines.pop_front();
    }
}

static void termRender(const String& prompt, const String& currentInput = "") {
    LB::fillScreen(TFT_BLACK);

    // Output area
    int maxLines = 9;
    int startLine = max(0, (int)outputLines.size() - maxLines + outputScroll);

    LB::setTextColor(TFT_WHITE);
    for (int i = 0; i < maxLines - 1 && (startLine + i) < (int)outputLines.size(); i++) {
        LB::setCursor(2, 2 + i * TERM_LINE_HEIGHT);
        String line = outputLines[startLine + i];
        if (line.length() > 38) line = line.substring(0, 38);
        LB::print(line.c_str());
    }

    // Input line at bottom
    LB::fillRect(0, TERM_SCREEN_HEIGHT - TERM_LINE_HEIGHT - 2, TERM_SCREEN_WIDTH, TERM_LINE_HEIGHT + 2, TFT_DARKGREY);
    LB::setTextColor(TFT_GREEN);
    LB::setCursor(2, TERM_SCREEN_HEIGHT - TERM_LINE_HEIGHT);
    LB::print(prompt.c_str());
    LB::setTextColor(TFT_WHITE);

    String visible = currentInput;
    int maxInputLen = 38 - prompt.length();
    if ((int)visible.length() > maxInputLen) {
        visible = visible.substring(visible.length() - maxInputLen);
    }
    LB::print(visible.c_str());
    LB::print("_");
}

// ============================================================================
// Calculator REPL
// ============================================================================

void calculatorRepl() {
    termClear();
    clearVariables();

    termPrint("=== Calculator ===");
    termPrint("Math: +,-,*,/,^,%");
    termPrint("Funcs: sin,cos,sqrt...");
    termPrint("Vars: x=5, then x+1");
    termPrint("Type 'help' or 'exit'");
    termPrint("");

    String input = "";
    bool running = true;

    while (running) {
        termRender("> ", input);

        M5Cardputer.update();

        if (!M5Cardputer.Keyboard.isChange()) {
            delay(30);
            continue;
        }

        if (!M5Cardputer.Keyboard.isPressed()) {
            delay(10);
            continue;
        }

        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        // Scroll
        if (M5Cardputer.Keyboard.isKeyPressed(';') && status.fn) {
            if (outputScroll < 0) outputScroll++;
            delay(100);
            continue;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.') && status.fn) {
            int maxScroll = -(int)outputLines.size() + 8;
            if (outputScroll > maxScroll) outputScroll--;
            delay(100);
            continue;
        }

        // Enter - evaluate
        if (status.enter) {
            if (input.length() > 0) {
                termPrint("> " + input);
                outputScroll = 0;

                if (input == "exit" || input == "quit") {
                    running = false;
                } else if (input == "help") {
                    termPrint("Operators: + - * / ^ %");
                    termPrint("Funcs: sin cos tan sqrt");
                    termPrint("       log exp pow abs");
                    termPrint("       floor ceil round");
                    termPrint("       rad deg min max");
                    termPrint("Consts: pi e");
                    termPrint("Assign: x = 5");
                    termPrint("'clear' to reset vars");
                } else if (input == "clear") {
                    clearVariables();
                    termPrint("Variables cleared");
                } else if (input == "vars") {
                    for (auto& kv : variables) {
                        termPrint(kv.first + " = " + String(kv.second, 6));
                    }
                } else {
                    String error;
                    double result = evalExpression(input, error);
                    if (error.length() > 0) {
                        termPrint("Error: " + error);
                    } else {
                        // Store result in 'ans'
                        setVariable("ans", result);
                        termPrint("= " + String(result, 10));
                    }
                }
                input = "";
            }
            delay(150);
            continue;
        }

        // Backspace
        if (status.del) {
            if (input.length() > 0) {
                input.remove(input.length() - 1);
            } else {
                running = false;
            }
            delay(80);
            continue;
        }

        // Character input
        for (auto ch : status.word) {
            if (ch >= 0x20 && ch <= 0x7E) {
                input += (char)ch;
            }
        }

        delay(30);
    }

    inMenu = true;
}

// ============================================================================
// Device Shell
// ============================================================================

static void shellHelp() {
    termPrint("=== Shell Commands ===");
    termPrint("info    - Device info");
    termPrint("wifi    - WiFi status");
    termPrint("scan    - Scan WiFi");
    termPrint("mem     - Free memory");
    termPrint("ls [p]  - List SD card");
    termPrint("cat [f] - Show file");
    termPrint("rm [f]  - Delete file");
    termPrint("reboot  - Restart device");
    termPrint("clear   - Clear screen");
    termPrint("exit    - Return to menu");
}

static void shellExecute(const String& cmd) {
    String command = cmd;
    command.trim();

    if (command.length() == 0) return;

    // Parse command and arguments
    int spaceIdx = command.indexOf(' ');
    String verb = (spaceIdx > 0) ? command.substring(0, spaceIdx) : command;
    String args = (spaceIdx > 0) ? command.substring(spaceIdx + 1) : "";
    verb.toLowerCase();
    args.trim();

    if (verb == "help" || verb == "?") {
        shellHelp();
    }
    else if (verb == "exit" || verb == "quit") {
        termPrint("Goodbye!");
    }
    else if (verb == "clear" || verb == "cls") {
        termClear();
    }
    else if (verb == "info") {
        termPrint("Chip: " + String(ESP.getChipModel()));
        termPrint("Cores: " + String(ESP.getChipCores()));
        termPrint("Freq: " + String(ESP.getCpuFreqMHz()) + " MHz");
        termPrint("Flash: " + String(ESP.getFlashChipSize() / 1024) + " KB");
        termPrint("SDK: " + String(ESP.getSdkVersion()));
    }
    else if (verb == "mem" || verb == "memory") {
        termPrint("Free heap: " + String(ESP.getFreeHeap() / 1024) + " KB");
        termPrint("Min heap: " + String(ESP.getMinFreeHeap() / 1024) + " KB");
        termPrint("PSRAM: " + String(ESP.getPsramSize() / 1024) + " KB");
        termPrint("Free PSRAM: " + String(ESP.getFreePsram() / 1024) + " KB");
    }
    else if (verb == "wifi") {
        if (WiFi.status() == WL_CONNECTED) {
            termPrint("Connected: " + WiFi.SSID());
            termPrint("IP: " + WiFi.localIP().toString());
            termPrint("RSSI: " + String(WiFi.RSSI()) + " dBm");
            termPrint("MAC: " + WiFi.macAddress());
        } else {
            termPrint("Not connected");
            termPrint("Status: " + String(WiFi.status()));
        }
    }
    else if (verb == "scan") {
        termPrint("Scanning WiFi...");
        int n = WiFi.scanNetworks();
        termPrint("Found " + String(n) + " networks:");
        for (int i = 0; i < min(n, 10); i++) {
            termPrint(String(i+1) + ". " + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + ")");
        }
        WiFi.scanDelete();
    }
    else if (verb == "ls" || verb == "dir") {
        String path = (args.length() > 0) ? args : "/";
        File dir = SD.open(path.c_str());
        if (!dir) {
            termPrint("Cannot open: " + path);
        } else if (!dir.isDirectory()) {
            termPrint("Not a directory");
            dir.close();
        } else {
            termPrint("Contents of " + path + ":");
            File entry;
            int count = 0;
            while ((entry = dir.openNextFile()) && count < 20) {
                String name = entry.name();
                if (entry.isDirectory()) {
                    termPrint("[DIR] " + name);
                } else {
                    termPrint(name + " (" + String(entry.size()) + ")");
                }
                entry.close();
                count++;
            }
            dir.close();
        }
    }
    else if (verb == "cat" || verb == "type") {
        if (args.length() == 0) {
            termPrint("Usage: cat <filename>");
        } else {
            File f = SD.open(args.c_str());
            if (!f) {
                termPrint("Cannot open: " + args);
            } else {
                int lines = 0;
                while (f.available() && lines < 20) {
                    String line = f.readStringUntil('\n');
                    if (line.endsWith("\r")) line.remove(line.length() - 1);
                    termPrint(line);
                    lines++;
                }
                if (f.available()) {
                    termPrint("... (truncated)");
                }
                f.close();
            }
        }
    }
    else if (verb == "rm" || verb == "del") {
        if (args.length() == 0) {
            termPrint("Usage: rm <filename>");
        } else if (SD.remove(args.c_str())) {
            termPrint("Deleted: " + args);
        } else {
            termPrint("Failed to delete: " + args);
        }
    }
    else if (verb == "mkdir") {
        if (args.length() == 0) {
            termPrint("Usage: mkdir <dirname>");
        } else if (SD.mkdir(args.c_str())) {
            termPrint("Created: " + args);
        } else {
            termPrint("Failed to create: " + args);
        }
    }
    else if (verb == "reboot" || verb == "restart") {
        termPrint("Rebooting...");
        delay(500);
        ESP.restart();
    }
    else if (verb == "uptime") {
        unsigned long ms = millis();
        unsigned long secs = ms / 1000;
        unsigned long mins = secs / 60;
        unsigned long hrs = mins / 60;
        termPrint("Uptime: " + String(hrs) + "h " + String(mins % 60) + "m " + String(secs % 60) + "s");
    }
    else if (verb == "echo") {
        termPrint(args);
    }
    else {
        termPrint("Unknown: " + verb);
        termPrint("Type 'help' for commands");
    }
}

void deviceShell() {
    termClear();

    termPrint("=== Device Shell ===");
    termPrint("Type 'help' for commands");
    termPrint("");

    String input = "";
    bool running = true;

    while (running) {
        termRender("$ ", input);

        M5Cardputer.update();

        if (!M5Cardputer.Keyboard.isChange()) {
            delay(30);
            continue;
        }

        if (!M5Cardputer.Keyboard.isPressed()) {
            delay(10);
            continue;
        }

        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        // Scroll with FN
        if (M5Cardputer.Keyboard.isKeyPressed(';') && status.fn) {
            if (outputScroll < 0) outputScroll++;
            delay(100);
            continue;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.') && status.fn) {
            int maxScroll = -(int)outputLines.size() + 8;
            if (outputScroll > maxScroll) outputScroll--;
            delay(100);
            continue;
        }

        // Enter - execute
        if (status.enter) {
            if (input.length() > 0) {
                termPrint("$ " + input);
                outputScroll = 0;

                if (input == "exit" || input == "quit") {
                    running = false;
                } else {
                    shellExecute(input);
                }
                input = "";
            }
            delay(150);
            continue;
        }

        // Backspace
        if (status.del) {
            if (input.length() > 0) {
                input.remove(input.length() - 1);
            } else {
                running = false;
            }
            delay(80);
            continue;
        }

        // Character input
        for (auto ch : status.word) {
            if (ch >= 0x20 && ch <= 0x7E) {
                input += (char)ch;
            }
        }

        delay(30);
    }

    inMenu = true;
}

// ============================================================================
// Terminal Menu
// ============================================================================

void terminalsMenu() {
    const char* options[] = {
        "Calculator",
        "Device Shell",
        "Lua (coming soon)",
        "JavaScript (coming soon)"
    };
    const int numOptions = 4;
    int selected = 0;

    while (true) {
        // Header
        LB::fillScreen(TFT_BLACK);
        LB::setTextColor(TFT_CYAN);
        LB::setCursor(5, 5);
        LB::println("=== Terminals ===");
        LB::setTextColor(TFT_DARKGREY);
        LB::println("Select with ;/. Enter");
        LB::println("");

        // Menu items
        for (int i = 0; i < numOptions; i++) {
            LB::setCursor(10, 40 + i * 15);
            LB::setTextColor(i == selected ? TFT_GREEN : TFT_WHITE);
            LB::print(i == selected ? "> " : "  ");
            LB::println(options[i]);
        }

        // Footer hint
        LB::setTextColor(TFT_DARKGREY);
        LB::setCursor(5, TERM_SCREEN_HEIGHT - 12);
        LB::print("Backspace to exit");

        // Wait for input
        while (true) {
            M5Cardputer.update();

            if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                inMenu = true;
                delay(150);
                return;
            }

            if (M5Cardputer.Keyboard.isKeyPressed(';')) {
                selected = (selected + numOptions - 1) % numOptions;
                delay(120);
                break;
            }

            if (M5Cardputer.Keyboard.isKeyPressed('.')) {
                selected = (selected + 1) % numOptions;
                delay(120);
                break;
            }

            if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                switch (selected) {
                    case 0: calculatorRepl(); break;
                    case 1: deviceShell(); break;
                    case 2:
                    case 3:
                        waitAndReturnToMenu("Coming soon!");
                        break;
                }
                delay(150);
                break;
            }

            delay(30);
        }
    }
}
