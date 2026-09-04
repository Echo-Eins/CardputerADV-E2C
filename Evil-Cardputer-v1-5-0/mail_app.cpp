/*
 * mail_app.cpp - Bounded IMAPS/SMTPS mail reader and composer.
 *
 * Supported transport: implicit TLS (normally IMAP 993 and SMTP 465).
 * The app intentionally avoids background sockets and downloads only bounded
 * message bodies.
 */

#include "mail_app.h"

#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <time.h>
#include <vector>

#include "display_runtime.h"
#include "file_editor.h"
#include "input_compat.h"
#include "netcore.h"
#include "network_transport.h"
#include "runtime_memory.h"
#include "scroll_input.h"

extern String getUserInput(bool isPassword);
extern bool confirmPopup(String message);
extern bool inMenu;

namespace MailApp {
namespace {

constexpr const char* CONFIG_PATH = "/evil/config/mail.json";
constexpr const char* MAIL_ROOT = "/evil/mail";
constexpr const char* DRAFT_ROOT = "/evil/mail/drafts";
constexpr const char* SENT_ROOT = "/evil/mail/sent";
constexpr size_t MAX_MESSAGES = 24;
constexpr size_t MAX_MESSAGE_WIRE = 65536;
constexpr size_t MAX_BODY_FILE = 32768;

struct MailConfig {
    String accountName = "Primary";
    String displayName;
    String email;
    String username;
    String password;
    String imapHost;
    uint16_t imapPort = 993;
    String smtpHost;
    uint16_t smtpPort = 465;
    bool tlsInsecure = true;
    String caCertPath;
    uint8_t maxMessages = 16;

    bool complete() const {
        return email.length() > 2 && username.length() > 0 &&
               password.length() > 0 && imapHost.length() > 0 &&
               smtpHost.length() > 0;
    }
};

struct MessageSummary {
    uint32_t uid = 0;
    uint32_t sequence = 0;
    bool seen = false;
    char from[96] = {};
    char subject[128] = {};
    char date[48] = {};
};

enum class UiEvent : uint8_t {
    NONE,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    ENTER,
    BACK,
    REFRESH,
};

struct InputState {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool enter = false;
    bool back = false;
    bool refresh = false;

    UiEvent poll() {
        M5.update();
        M5Cardputer.update();
        ScrollInput::poll();
        const ScrollEvent wheel = ScrollInput::getMenuEvent();

        const bool nowUp = M5Cardputer.Keyboard.isKeyPressed(';');
        const bool nowDown = M5Cardputer.Keyboard.isKeyPressed('.');
        const bool nowLeft = M5Cardputer.Keyboard.isKeyPressed(',');
        const bool nowRight = M5Cardputer.Keyboard.isKeyPressed('/');
        const bool nowEnter = InputCompat::isEnterPressed();
        const bool nowBack = InputCompat::isBackPressed();
        const bool nowRefresh = M5Cardputer.Keyboard.isKeyPressed('r');

        UiEvent result = UiEvent::NONE;
        if ((nowUp && !up) || wheel == ScrollEvent::ScrollUp)
            result = UiEvent::UP;
        else if ((nowDown && !down) || wheel == ScrollEvent::ScrollDown)
            result = UiEvent::DOWN;
        else if (nowLeft && !left)
            result = UiEvent::LEFT;
        else if (nowRight && !right)
            result = UiEvent::RIGHT;
        else if (nowEnter && !enter)
            result = UiEvent::ENTER;
        else if (nowBack && !back)
            result = UiEvent::BACK;
        else if (nowRefresh && !refresh)
            result = UiEvent::REFRESH;

        up = nowUp;
        down = nowDown;
        left = nowLeft;
        right = nowRight;
        enter = nowEnter;
        back = nowBack;
        refresh = nowRefresh;
        return result;
    }
};

struct MailState {
    MailConfig config;
    MessageSummary* messages = nullptr;
    size_t messageCount = 0;
    size_t messageCapacity = 0;
};

MailState* g_state = nullptr;
#define g_config (g_state->config)
#define g_messages (g_state->messages)
#define g_messageCount (g_state->messageCount)
#define g_messageCapacity (g_state->messageCapacity)

lgfx::LGFX_Device& display() {
    return GUI::runtimeDisplay();
}

void copyBounded(char* output, size_t capacity, const String& value) {
    if (!output || capacity == 0) return;
    snprintf(output, capacity, "%s", value.c_str());
}

void drawTitle(const String& title, uint16_t color = TFT_CYAN) {
    lgfx::LGFX_Device& d = display();
    d.fillScreen(TFT_BLACK);
    d.setTextSize(1);
    d.setTextWrap(false);
    d.fillRect(0, 0, d.width(), 14, TFT_DARKGREY);
    d.setTextColor(color, TFT_DARKGREY);
    d.setCursor(3, 3);
    d.print(title);
}

void drawFooter(const String& text) {
    lgfx::LGFX_Device& d = display();
    const int y = d.height() - 12;
    d.fillRect(0, y, d.width(), 12, TFT_DARKGREY);
    d.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
    d.setCursor(3, y + 2);
    String shown = text;
    const int maxChars = (d.width() - 6) / 6;
    if (shown.length() > static_cast<size_t>(maxChars))
        shown = shown.substring(0, maxChars);
    d.print(shown);
}

void showStatus(const String& title, const String& message,
                uint16_t color = TFT_WHITE) {
    drawTitle(title);
    lgfx::LGFX_Device& d = display();
    d.setTextWrap(true);
    d.setTextColor(color, TFT_BLACK);
    d.setCursor(5, 24);
    d.print(message);
    d.setTextWrap(false);
}

void modal(const String& title, const String& message,
           uint16_t color = TFT_WHITE) {
    showStatus(title, message, color);
    drawFooter("Enter / FN+Del");
    InputState input;
    while (true) {
        const UiEvent event = input.poll();
        if (event == UiEvent::ENTER || event == UiEvent::BACK) break;
        delay(10);
    }
}

String lowerCopy(String value) {
    value.toLowerCase();
    return value;
}

String trimCopy(String value) {
    value.trim();
    return value;
}

String safeProtocolValue(String value) {
    value.replace("\r", " ");
    value.replace("\n", " ");
    return value;
}

String imapQuote(String value) {
    value.replace("\\", "\\\\");
    value.replace("\"", "\\\"");
    return String("\"") + value + "\"";
}

bool loadConfig(MailConfig& config, String& error) {
    String json;
    bool truncated = false;
    if (!NetworkTransport::readFile(CONFIG_PATH, json, 16384, error,
                                    &truncated)) {
        return false;
    }
    if (truncated) {
        error = "mail.json is too large";
        return false;
    }

    JsonDocument doc;
    const DeserializationError parseError = deserializeJson(doc, json);
    if (parseError) {
        error = String("mail.json: ") + parseError.c_str();
        return false;
    }

    config.accountName = doc["account_name"] | "Primary";
    config.displayName = doc["display_name"] | "";
    config.email = doc["email"] | "";
    config.username = doc["username"] | config.email;
    config.password = doc["password"] | "";
    config.imapHost = doc["imap"]["host"] | "";
    config.imapPort = doc["imap"]["port"] | 993;
    config.smtpHost = doc["smtp"]["host"] | "";
    config.smtpPort = doc["smtp"]["port"] | 465;
    config.tlsInsecure = doc["tls"]["insecure"] | true;
    config.caCertPath = doc["tls"]["ca_cert"] | "";
    int count = doc["sync"]["max_messages"] | 16;
    if (count < 1) count = 1;
    if (count > static_cast<int>(MAX_MESSAGES)) count = MAX_MESSAGES;
    config.maxMessages = static_cast<uint8_t>(count);
    error = "";
    return true;
}

bool saveConfig(const MailConfig& config, String& error) {
    JsonDocument doc;
    doc["version"] = 1;
    doc["account_name"] = config.accountName;
    doc["display_name"] = config.displayName;
    doc["email"] = config.email;
    doc["username"] = config.username;
    doc["password"] = config.password;
    doc["imap"]["host"] = config.imapHost;
    doc["imap"]["port"] = config.imapPort;
    doc["smtp"]["host"] = config.smtpHost;
    doc["smtp"]["port"] = config.smtpPort;
    doc["tls"]["insecure"] = config.tlsInsecure;
    doc["tls"]["ca_cert"] = config.caCertPath;
    doc["sync"]["max_messages"] = config.maxMessages;

    String json;
    serializeJsonPretty(doc, json);
    return NetworkTransport::writeFileAtomic(CONFIG_PATH, json, error);
}

void inferServers(MailConfig& config) {
    const int at = config.email.lastIndexOf('@');
    if (at < 1 || at >= static_cast<int>(config.email.length()) - 1) return;
    String domain = lowerCopy(config.email.substring(at + 1));
    if (domain == "gmail.com" || domain == "googlemail.com") {
        config.imapHost = "imap.gmail.com";
        config.smtpHost = "smtp.gmail.com";
    } else if (domain == "yahoo.com" || domain.endsWith(".yahoo.com")) {
        config.imapHost = "imap.mail.yahoo.com";
        config.smtpHost = "smtp.mail.yahoo.com";
    } else if (domain == "icloud.com" || domain == "me.com" ||
               domain == "mac.com") {
        config.imapHost = "imap.mail.me.com";
        config.smtpHost = "smtp.mail.me.com";
    } else {
        config.imapHost = String("imap.") + domain;
        config.smtpHost = String("smtp.") + domain;
    }
}

String promptValue(const String& label, const String& current,
                   bool password = false) {
    showStatus("Mail account",
               label + "\nCurrent: " +
                   (password && current.length() ? String("********")
                                                 : current) +
                   "\nEmpty keeps current.");
    drawFooter("Type value, Enter");
    String value = getUserInput(password);
    value.trim();
    return value.length() ? value : current;
}

bool accountWizard(MailConfig& config) {
    MailConfig edited = config;
    edited.email = promptValue("Email address", edited.email);
    if (edited.email.indexOf('@') < 1) {
        modal("Mail account", "A valid email address is required.", TFT_RED);
        return false;
    }
    if (edited.imapHost.length() == 0 || edited.smtpHost.length() == 0)
        inferServers(edited);
    edited.displayName = promptValue("Display name", edited.displayName);
    edited.username = promptValue(
        "Login name", edited.username.length() ? edited.username : edited.email);
    edited.password = promptValue("App password", edited.password, true);
    edited.imapHost = promptValue("IMAPS host", edited.imapHost);
    edited.smtpHost = promptValue("SMTPS host", edited.smtpHost);

    showStatus("TLS verification",
               "Verified TLS needs a PEM CA certificate on SD.\n"
               "Yes: verified TLS\nNo: insecure TLS");
    const bool verify = confirmPopup("Use verified TLS?");
    edited.tlsInsecure = !verify;
    if (verify) {
        edited.caCertPath =
            promptValue("CA certificate path", edited.caCertPath);
        if (edited.caCertPath.length() == 0) {
            modal("Mail account", "CA path cannot be empty.", TFT_RED);
            return false;
        }
    } else {
        edited.caCertPath = "";
    }

    String error;
    if (!saveConfig(edited, error)) {
        modal("Mail account", error, TFT_RED);
        return false;
    }
    config = edited;
    modal("Mail account",
          String("Saved. ") +
              (config.tlsInsecure ? "TLS verification is DISABLED."
                                  : "TLS verification enabled."),
          config.tlsInsecure ? TFT_ORANGE : TFT_GREEN);
    return true;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

String base64Decode(const String& input, size_t limit = MAX_MESSAGE_WIRE) {
    String output;
    const size_t expected = (input.length() * 3) / 4;
    output.reserve((expected < limit ? expected : limit) + 1);
    uint32_t accumulator = 0;
    int bits = 0;
    for (size_t i = 0; i < input.length() && output.length() < limit; ++i) {
        const char c = input[i];
        if (c == '=') break;
        const int value = base64Value(c);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output += static_cast<char>((accumulator >> bits) & 0xFF);
        }
    }
    return output;
}

String quotedPrintableDecode(const String& input,
                             size_t limit = MAX_MESSAGE_WIRE) {
    String output;
    output.reserve((input.length() < limit ? input.length() : limit) + 1);
    for (size_t i = 0; i < input.length() && output.length() < limit; ++i) {
        if (input[i] == '=' && i + 1 < input.length()) {
            if (input[i + 1] == '\r' && i + 2 < input.length() &&
                input[i + 2] == '\n') {
                i += 2;
                continue;
            }
            if (input[i + 1] == '\n') {
                ++i;
                continue;
            }
            if (i + 2 < input.length() && isxdigit(input[i + 1]) &&
                isxdigit(input[i + 2])) {
                char hex[3] = {input[i + 1], input[i + 2], 0};
                output += static_cast<char>(strtoul(hex, nullptr, 16));
                i += 2;
                continue;
            }
        }
        output += input[i];
    }
    return output;
}

String decodeEncodedWords(const String& input) {
    String output;
    size_t cursor = 0;
    while (cursor < input.length()) {
        const int start = input.indexOf("=?", cursor);
        if (start < 0) {
            output += input.substring(cursor);
            break;
        }
        output += input.substring(cursor, start);
        const int q1 = input.indexOf('?', start + 2);
        const int q2 = q1 >= 0 ? input.indexOf('?', q1 + 1) : -1;
        const int end = q2 >= 0 ? input.indexOf("?=", q2 + 1) : -1;
        if (q1 < 0 || q2 < 0 || end < 0) {
            output += input.substring(start);
            break;
        }
        String mode = input.substring(q1 + 1, q2);
        String encoded = input.substring(q2 + 1, end);
        mode.toUpperCase();
        if (mode == "B") {
            output += base64Decode(encoded, 4096);
        } else if (mode == "Q") {
            encoded.replace('_', ' ');
            output += quotedPrintableDecode(encoded, 4096);
        } else {
            output += encoded;
        }
        cursor = static_cast<size_t>(end + 2);
        while (cursor < input.length() && input[cursor] == ' ' &&
               input.indexOf("=?", cursor) == static_cast<int>(cursor + 1)) {
            ++cursor;
        }
    }
    output.trim();
    return output;
}

String headerValue(const String& headers, const String& requestedName) {
    String wanted = lowerCopy(requestedName);
    String currentName;
    String currentValue;
    size_t cursor = 0;
    auto commit = [&]() -> String {
        if (lowerCopy(currentName) == wanted) return trimCopy(currentValue);
        return String();
    };

    while (cursor <= headers.length()) {
        int end = headers.indexOf('\n', cursor);
        if (end < 0) end = headers.length();
        String line = headers.substring(cursor, end);
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        cursor = static_cast<size_t>(end + 1);

        if ((line.startsWith(" ") || line.startsWith("\t")) &&
            currentName.length()) {
            line.trim();
            currentValue += " " + line;
        } else {
            String found = commit();
            if (found.length()) return found;
            const int colon = line.indexOf(':');
            if (colon > 0) {
                currentName = line.substring(0, colon);
                currentValue = line.substring(colon + 1);
                currentValue.trim();
            } else {
                currentName = "";
                currentValue = "";
            }
        }
        if (end == static_cast<int>(headers.length())) break;
    }
    return commit();
}

String contentTypeParameter(const String& contentType, const String& name) {
    String lower = lowerCopy(contentType);
    String key = lowerCopy(name) + "=";
    const int position = lower.indexOf(key);
    if (position < 0) return "";
    int start = position + key.length();
    if (start >= static_cast<int>(contentType.length())) return "";
    if (contentType[start] == '"' || contentType[start] == '\'') {
        const char quote = contentType[start++];
        int end = contentType.indexOf(quote, start);
        if (end < 0) end = contentType.length();
        return contentType.substring(start, end);
    }
    int end = contentType.indexOf(';', start);
    if (end < 0) end = contentType.length();
    String value = contentType.substring(start, end);
    value.trim();
    return value;
}

String stripHtml(const String& html) {
    String output;
    output.reserve((html.length() < MAX_MESSAGE_WIRE ? html.length()
                                                     : MAX_MESSAGE_WIRE) +
                   1);
    bool inTag = false;
    String tag;
    bool space = false;
    for (size_t i = 0; i < html.length() &&
                       output.length() < MAX_MESSAGE_WIRE;
         ++i) {
        const char c = html[i];
        if (c == '<') {
            inTag = true;
            tag = "";
            continue;
        }
        if (inTag) {
            if (c == '>') {
                inTag = false;
                tag.toLowerCase();
                tag.trim();
                if (tag.startsWith("br") || tag.startsWith("/p") ||
                    tag.startsWith("/div") || tag.startsWith("/li") ||
                    tag.startsWith("/h")) {
                    if (!output.endsWith("\n")) output += '\n';
                    space = false;
                }
            } else if (tag.length() < 32) {
                tag += c;
            }
            continue;
        }
        if (c == '&') {
            const int semicolon = html.indexOf(';', i + 1);
            if (semicolon > 0 && semicolon - static_cast<int>(i) < 12) {
                String entity = html.substring(i + 1, semicolon);
                if (entity == "amp")
                    output += '&';
                else if (entity == "lt")
                    output += '<';
                else if (entity == "gt")
                    output += '>';
                else if (entity == "quot")
                    output += '"';
                else if (entity == "nbsp")
                    output += ' ';
                else
                    output += '?';
                i = semicolon;
                continue;
            }
        }
        if (c == '\r') continue;
        if (c == '\n') {
            if (!output.endsWith("\n")) output += '\n';
            space = false;
        } else if (c == ' ' || c == '\t') {
            if (!space && !output.endsWith("\n")) output += ' ';
            space = true;
        } else {
            output += c;
            space = false;
        }
    }
    output.trim();
    return output;
}

String decodeTransfer(const String& body, String encoding) {
    encoding.toLowerCase();
    if (encoding.indexOf("base64") >= 0) return base64Decode(body);
    if (encoding.indexOf("quoted-printable") >= 0)
        return quotedPrintableDecode(body);
    return body.substring(
        0, body.length() < MAX_MESSAGE_WIRE ? body.length() : MAX_MESSAGE_WIRE);
}

String extractMimeText(const String& part, int depth, int& rank) {
    rank = 0;
    if (depth > 4 || part.length() == 0) return "";
    int separator = part.indexOf("\r\n\r\n");
    int separatorSize = 4;
    if (separator < 0) {
        separator = part.indexOf("\n\n");
        separatorSize = 2;
    }
    String headers = separator >= 0 ? part.substring(0, separator) : "";
    String body =
        separator >= 0 ? part.substring(separator + separatorSize) : part;
    String contentType = headerValue(headers, "Content-Type");
    if (contentType.length() == 0) contentType = "text/plain";
    String lowerType = lowerCopy(contentType);

    if (lowerType.startsWith("multipart/")) {
        const String boundary = contentTypeParameter(contentType, "boundary");
        if (boundary.length() == 0) return "";
        const String marker = "--" + boundary;
        String best;
        int bestRank = 0;
        int cursor = body.indexOf(marker);
        while (cursor >= 0) {
            int start = cursor + marker.length();
            if (body.substring(start, start + 2) == "--") break;
            if (body.substring(start, start + 2) == "\r\n")
                start += 2;
            else if (start < static_cast<int>(body.length()) &&
                     body[start] == '\n')
                ++start;
            int next = body.indexOf(marker, start);
            if (next < 0) break;
            String child = body.substring(start, next);
            child.trim();
            int childRank = 0;
            String decoded = extractMimeText(child, depth + 1, childRank);
            if (childRank > bestRank && decoded.length()) {
                best = decoded;
                bestRank = childRank;
                if (bestRank == 2) break;
            }
            cursor = next;
        }
        rank = bestRank;
        return best;
    }

    const String disposition =
        lowerCopy(headerValue(headers, "Content-Disposition"));
    if (disposition.startsWith("attachment")) return "";
    const String decoded =
        decodeTransfer(body, headerValue(headers, "Content-Transfer-Encoding"));
    if (lowerType.startsWith("text/plain")) {
        rank = 2;
        return decoded;
    }
    if (lowerType.startsWith("text/html")) {
        rank = 1;
        return stripHtml(decoded);
    }
    return "";
}

String parseMessageBody(const String& raw) {
    int rank = 0;
    String body = extractMimeText(raw, 0, rank);
    if (body.length() == 0) body = "(No readable text/plain or text/html part)";
    body.replace("\r\n", "\n");
    body.replace('\r', '\n');
    return body;
}

bool parseLiteralLength(const String& line, size_t& length) {
    const int right = line.lastIndexOf('}');
    const int left = line.lastIndexOf('{');
    if (left < 0 || right != static_cast<int>(line.length()) - 1 ||
        right <= left + 1)
        return false;
    String value = line.substring(left + 1, right);
    value.replace("+", "");
    for (size_t i = 0; i < value.length(); ++i) {
        if (!isdigit(value[i])) return false;
    }
    length = static_cast<size_t>(strtoul(value.c_str(), nullptr, 10));
    return true;
}

class ImapSession {
public:
    bool open(const MailConfig& config, String& error) {
        _config = &config;
        NetworkTransport::TlsOptions tls;
        tls.insecure = config.tlsInsecure;
        tls.caCertPath = config.caCertPath;
        showStatus("Mail", "Connecting to IMAP...");
        if (!_connection.connect("mail-imap", config.imapHost, config.imapPort,
                                 true, tls, error))
            return false;

        String greeting;
        if (!_connection.readLine(greeting, 2048, 15000, error)) return false;
        const String lower = lowerCopy(greeting);
        if (!lower.startsWith("* ok") && !lower.startsWith("* preauth")) {
            error = "Invalid IMAP greeting";
            return false;
        }
        if (!lower.startsWith("* preauth")) {
            if (!simpleCommand("LOGIN " + imapQuote(config.username) + " " +
                                   imapQuote(config.password),
                               nullptr, error))
                return false;
        }
        return true;
    }

    bool selectInbox(uint32_t& exists, String& error) {
        String response;
        if (!simpleCommand("SELECT INBOX", &response, error)) return false;
        exists = 0;
        size_t cursor = 0;
        while (cursor < response.length()) {
            int end = response.indexOf('\n', cursor);
            if (end < 0) end = response.length();
            String line = response.substring(cursor, end);
            if (line.startsWith("* ") && line.indexOf(" EXISTS") > 2) {
                exists = static_cast<uint32_t>(
                    strtoul(line.c_str() + 2, nullptr, 10));
            }
            cursor = static_cast<size_t>(end + 1);
        }
        return true;
    }

    bool fetchSummary(uint32_t sequence, MessageSummary& summary,
                      String& error) {
        const String tag = nextTag();
        const String command =
            tag + " FETCH " + String(sequence) +
            " (UID FLAGS BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])";
        if (!_connection.writeLine(command, error)) return false;

        String headers;
        bool taggedOk = false;
        while (true) {
            String line;
            if (!_connection.readLine(line, 8192, 15000, error)) return false;
            if (line.startsWith(tag + " ")) {
                taggedOk = lowerCopy(line).startsWith(lowerCopy(tag) + " ok");
                if (!taggedOk) error = line;
                break;
            }
            const int uidAt = line.indexOf("UID ");
            if (uidAt >= 0) {
                summary.uid = static_cast<uint32_t>(
                    strtoul(line.c_str() + uidAt + 4, nullptr, 10));
            }
            if (line.indexOf("\\Seen") >= 0) summary.seen = true;
            size_t literal = 0;
            if (parseLiteralLength(line, literal)) {
                bool truncated = false;
                if (!_connection.readExactString(literal, headers, 8192, 15000,
                                                 error, &truncated))
                    return false;
                if (truncated) {
                    error = "Message header is too large";
                    return false;
                }
            }
        }
        if (!taggedOk) return false;
        summary.sequence = sequence;
        copyBounded(summary.from, sizeof(summary.from),
                    decodeEncodedWords(headerValue(headers, "From")));
        copyBounded(summary.subject, sizeof(summary.subject),
                    decodeEncodedWords(headerValue(headers, "Subject")));
        copyBounded(summary.date, sizeof(summary.date),
                    headerValue(headers, "Date"));
        if (summary.subject[0] == '\0')
            copyBounded(summary.subject, sizeof(summary.subject),
                        "(no subject)");
        if (summary.from[0] == '\0')
            copyBounded(summary.from, sizeof(summary.from), "(unknown sender)");
        return true;
    }

    bool fetchBody(uint32_t uid, String& body, bool& truncated, String& error) {
        const String tag = nextTag();
        const String command =
            tag + " UID FETCH " + String(uid) +
            " (BODY.PEEK[]<0." + String(MAX_MESSAGE_WIRE) + ">)";
        if (!_connection.writeLine(command, error)) return false;

        String raw;
        bool found = false;
        bool taggedOk = false;
        while (true) {
            String line;
            if (!_connection.readLine(line, 8192, 20000, error)) return false;
            if (line.startsWith(tag + " ")) {
                taggedOk = lowerCopy(line).startsWith(lowerCopy(tag) + " ok");
                if (!taggedOk) error = line;
                break;
            }
            size_t literal = 0;
            if (parseLiteralLength(line, literal)) {
                found = true;
                if (!_connection.readExactString(literal, raw,
                                                 MAX_MESSAGE_WIRE, 20000,
                                                 error, &truncated))
                    return false;
            }
        }
        if (!taggedOk || !found) {
            if (error.length() == 0) error = "IMAP returned no message body";
            return false;
        }
        body = parseMessageBody(raw);
        String ignored;
        simpleCommand("UID STORE " + String(uid) +
                          " +FLAGS.SILENT (\\Seen)",
                      &ignored, error);
        error = "";
        return true;
    }

    void logout() {
        String error;
        simpleCommand("LOGOUT", nullptr, error);
        _connection.close(0);
    }

private:
    NetworkTransport::Connection _connection;
    const MailConfig* _config = nullptr;
    uint16_t _tag = 1;

    String nextTag() {
        char value[12];
        snprintf(value, sizeof(value), "A%04u", _tag++);
        return value;
    }

    bool simpleCommand(const String& command, String* response, String& error) {
        const String tag = nextTag();
        if (!_connection.writeLine(tag + " " + command, error)) return false;
        if (response) *response = "";
        while (true) {
            String line;
            if (!_connection.readLine(line, 8192, 15000, error)) return false;
            if (line.startsWith(tag + " ")) {
                const bool ok =
                    lowerCopy(line).startsWith(lowerCopy(tag) + " ok");
                if (!ok) error = line;
                return ok;
            }
            if (response && response->length() < 16384) {
                *response += line;
                *response += '\n';
            }
            size_t literal = 0;
            if (parseLiteralLength(line, literal)) {
                String discarded;
                bool truncated = false;
                if (!_connection.readExactString(literal, discarded, 0, 15000,
                                                 error, &truncated))
                    return false;
            }
        }
    }
};

String normalizeCrlf(const String& input) {
    String output;
    output.reserve(input.length() + 32);
    for (size_t i = 0; i < input.length(); ++i) {
        const char c = input[i];
        if (c == '\r') {
            if (i + 1 < input.length() && input[i + 1] == '\n') ++i;
            output += "\r\n";
        } else if (c == '\n') {
            output += "\r\n";
        } else {
            output += c;
        }
    }
    return output;
}

String rfc2047(const String& value) {
    bool asciiOnly = true;
    for (size_t i = 0; i < value.length(); ++i) {
        if (static_cast<uint8_t>(value[i]) >= 0x80) {
            asciiOnly = false;
            break;
        }
    }
    return asciiOnly ? safeProtocolValue(value)
                     : String("=?UTF-8?B?") +
                           NetworkTransport::base64Encode(value) + "?=";
}

String messageDate() {
    time_t now = time(nullptr);
    if (now < 1700000000) return "";
    struct tm value;
    gmtime_r(&now, &value);
    char buffer[48];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S +0000", &value);
    return buffer;
}

bool validAddress(const String& value) {
    return value.indexOf('@') > 0 && value.indexOf('\r') < 0 &&
           value.indexOf('\n') < 0 && value.indexOf('<') < 0 &&
           value.indexOf('>') < 0;
}

String buildEml(const MailConfig& config, const String& recipient,
                const String& subject, const String& body) {
    String output;
    output.reserve(body.length() + 768);
    output += "From: ";
    if (config.displayName.length())
        output += rfc2047(config.displayName) + " <" + config.email + ">";
    else
        output += config.email;
    output += "\r\nTo: " + recipient;
    output += "\r\nSubject: " + rfc2047(subject);
    const String date = messageDate();
    if (date.length()) output += "\r\nDate: " + date;
    output += "\r\nMessage-ID: <";
    output += String(static_cast<uint32_t>(esp_random()), HEX);
    output += ".";
    output += String(millis());
    output += "@cardputer.local>";
    output += "\r\nMIME-Version: 1.0";
    output += "\r\nContent-Type: text/plain; charset=UTF-8";
    output += "\r\nContent-Transfer-Encoding: 8bit";
    output += "\r\n\r\n";
    output += normalizeCrlf(body);
    if (!output.endsWith("\r\n")) output += "\r\n";
    return output;
}

class SmtpSession {
public:
    bool send(const MailConfig& config, const String& recipient,
              const String& eml, String& error) {
        NetworkTransport::TlsOptions tls;
        tls.insecure = config.tlsInsecure;
        tls.caCertPath = config.caCertPath;
        showStatus("Mail", "Connecting to SMTP...");
        if (!_connection.connect("mail-smtp", config.smtpHost, config.smtpPort,
                                 true, tls, error))
            return false;

        int code = 0;
        if (!readResponse(code, error) || code != 220) return failCode(code, error);
        if (!command("EHLO cardputer.local", 250, error)) return false;
        if (!command("AUTH LOGIN", 334, error)) return false;
        if (!command(NetworkTransport::base64Encode(config.username), 334,
                     error))
            return false;
        if (!command(NetworkTransport::base64Encode(config.password), 235,
                     error))
            return false;
        if (!command("MAIL FROM:<" + config.email + ">", 250, error))
            return false;
        if (!command("RCPT TO:<" + recipient + ">", 250, error)) return false;
        if (!command("DATA", 354, error)) return false;

        size_t cursor = 0;
        while (cursor <= eml.length()) {
            int end = eml.indexOf("\r\n", cursor);
            if (end < 0) end = eml.length();
            String line = eml.substring(cursor, end);
            if (line.startsWith(".")) line = "." + line;
            if (!_connection.writeLine(line, error)) return false;
            cursor = static_cast<size_t>(end + 2);
            if (end == static_cast<int>(eml.length())) break;
        }
        if (!_connection.writeLine(".", error)) return false;
        if (!readResponse(code, error) || code != 250) return failCode(code, error);
        command("QUIT", 221, error);
        _connection.close(0);
        error = "";
        return true;
    }

private:
    NetworkTransport::Connection _connection;

    bool failCode(int code, String& error) {
        if (error.length() == 0) error = "SMTP error " + String(code);
        _connection.close(code ? code : -1);
        return false;
    }

    bool command(const String& line, int expected, String& error) {
        if (!_connection.writeLine(line, error)) return false;
        int code = 0;
        if (!readResponse(code, error)) return false;
        if (code != expected) return failCode(code, error);
        return true;
    }

    bool readResponse(int& code, String& error) {
        code = 0;
        while (true) {
            String line;
            if (!_connection.readLine(line, 2048, 15000, error)) return false;
            if (line.length() < 3 || !isdigit(line[0]) || !isdigit(line[1]) ||
                !isdigit(line[2])) {
                error = "Malformed SMTP response";
                return false;
            }
            code = line.substring(0, 3).toInt();
            if (line.length() < 4 || line[3] == ' ') {
                if (code >= 400) error = line;
                return true;
            }
        }
    }
};

void drawMenu(const String& title, const char* const* items, size_t count,
              size_t selected, const String& status = "") {
    drawTitle(title);
    lgfx::LGFX_Device& d = display();
    const int rowHeight = 12;
    const int availableRows = (d.height() - 28) / rowHeight;
    size_t start = 0;
    if (selected >= static_cast<size_t>(availableRows))
        start = selected - availableRows + 1;
    for (int row = 0; row < availableRows; ++row) {
        const size_t index = start + row;
        if (index >= count) break;
        const int y = 15 + row * rowHeight;
        const bool active = index == selected;
        if (active) d.fillRect(0, y, d.width(), rowHeight, TFT_NAVY);
        d.setTextColor(active ? TFT_WHITE : TFT_LIGHTGREY,
                       active ? TFT_NAVY : TFT_BLACK);
        d.setCursor(4, y + 2);
        d.print(active ? "> " : "  ");
        d.print(items[index]);
    }
    drawFooter(status.length() ? status : ";/. Move  Enter");
}

bool syncInbox() {
    g_messageCount = 0;
    if (!g_config.complete()) {
        modal("Mail", "Configure the account first.", TFT_ORANGE);
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        modal("Mail", "WiFi is not connected.", TFT_RED);
        return false;
    }

    ImapSession session;
    String error;
    if (!session.open(g_config, error)) {
        modal("IMAP error", error, TFT_RED);
        return false;
    }
    uint32_t exists = 0;
    if (!session.selectInbox(exists, error)) {
        session.logout();
        modal("IMAP error", error, TFT_RED);
        return false;
    }

    uint32_t wanted =
        exists < g_config.maxMessages ? exists : g_config.maxMessages;
    if (wanted > g_messageCapacity) wanted = g_messageCapacity;
    for (uint32_t index = 0; index < wanted; ++index) {
        showStatus("Sync inbox",
                   String(index + 1) + "/" + String(wanted) + " messages");
        const uint32_t sequence = exists - index;
        MessageSummary summary;
        if (session.fetchSummary(sequence, summary, error)) {
            g_messages[g_messageCount++] = summary;
        } else {
            Serial.printf("[Mail] FETCH %lu: %s\n",
                          static_cast<unsigned long>(sequence), error.c_str());
            error = "";
        }
    }
    session.logout();
    return true;
}

std::vector<String> wrapText(const String& value, size_t width,
                             size_t maxLines = 1024) {
    std::vector<String> lines;
    size_t cursor = 0;
    while (cursor < value.length() && lines.size() < maxLines) {
        if (value[cursor] == '\n') {
            lines.push_back("");
            ++cursor;
            continue;
        }
        size_t end = cursor + width;
        if (end >= value.length()) {
            end = value.length();
        } else {
            size_t breakAt = end;
            while (breakAt > cursor && value[breakAt] != ' ' &&
                   value[breakAt] != '\n')
                --breakAt;
            if (breakAt > cursor) end = breakAt;
        }
        const int newline = value.indexOf('\n', cursor);
        if (newline >= 0 && static_cast<size_t>(newline) < end)
            end = static_cast<size_t>(newline);
        String line = value.substring(cursor, end);
        line.trim();
        lines.push_back(line);
        cursor = end;
        while (cursor < value.length() &&
               (value[cursor] == ' ' || value[cursor] == '\n'))
            ++cursor;
    }
    return lines;
}

void viewMessage(const MessageSummary& summary, const String& body,
                 bool truncated) {
    lgfx::LGFX_Device& d = display();
    const size_t chars = (d.width() - 8) / 6;
    const std::vector<String> lines = wrapText(body, chars > 8 ? chars : 8);
    size_t scroll = 0;
    InputState input;
    while (true) {
        drawTitle(String(summary.subject));
        const int bodyTop = 28;
        d.setTextColor(TFT_YELLOW, TFT_BLACK);
        d.setCursor(3, 16);
        String sender = summary.from;
        const int maxChars = (d.width() - 6) / 6;
        if (sender.length() > static_cast<size_t>(maxChars))
            sender = sender.substring(0, maxChars);
        d.print(sender);
        d.fillRect(0, bodyTop, d.width(), d.height() - bodyTop - 12,
                   TFT_BLACK);
        const size_t visible = (d.height() - bodyTop - 12) / 10;
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        for (size_t row = 0; row < visible && scroll + row < lines.size();
             ++row) {
            d.setCursor(3, bodyTop + row * 10);
            d.print(lines[scroll + row]);
        }
        drawFooter(String(truncated ? "TRUNCATED  " : "") +
                   ";/. Scroll  FN+Del");

        const UiEvent event = input.poll();
        if (event == UiEvent::BACK || event == UiEvent::ENTER) return;
        if (event == UiEvent::UP && scroll > 0) --scroll;
        if (event == UiEvent::DOWN && scroll + visible < lines.size()) ++scroll;
        if (event == UiEvent::LEFT)
            scroll = scroll > visible ? scroll - visible : 0;
        if (event == UiEvent::RIGHT && scroll + visible < lines.size()) {
            scroll += visible;
            if (scroll >= lines.size()) scroll = lines.size() - 1;
        }
        delay(10);
    }
}

void openMessage(size_t index) {
    if (index >= g_messageCount) return;
    showStatus("Mail", "Downloading message...");
    ImapSession session;
    String error;
    if (!session.open(g_config, error)) {
        modal("IMAP error", error, TFT_RED);
        return;
    }
    uint32_t exists = 0;
    if (!session.selectInbox(exists, error)) {
        session.logout();
        modal("IMAP error", error, TFT_RED);
        return;
    }
    String body;
    bool truncated = false;
    if (!session.fetchBody(g_messages[index].uid, body, truncated, error)) {
        session.logout();
        modal("IMAP error", error, TFT_RED);
        return;
    }
    session.logout();
    g_messages[index].seen = true;
    viewMessage(g_messages[index], body, truncated);
}

void inbox() {
    if (g_messageCount == 0 && !syncInbox()) return;
    size_t selected = 0;
    InputState input;
    while (true) {
        drawTitle("Inbox");
        lgfx::LGFX_Device& d = display();
        const int rowHeight = 20;
        const int visible = (d.height() - 27) / rowHeight;
        size_t start = selected >= static_cast<size_t>(visible)
                           ? selected - visible + 1
                           : 0;
        for (int row = 0; row < visible; ++row) {
            const size_t index = start + row;
            if (index >= g_messageCount) break;
            const int y = 15 + row * rowHeight;
            const bool active = index == selected;
            if (active) d.fillRect(0, y, d.width(), rowHeight, TFT_NAVY);
            d.setTextColor(active ? TFT_WHITE
                                  : (g_messages[index].seen ? TFT_LIGHTGREY
                                                            : TFT_GREEN),
                           active ? TFT_NAVY : TFT_BLACK);
            d.setCursor(3, y + 1);
            d.print(g_messages[index].seen ? "  " : "* ");
            String subject = g_messages[index].subject;
            const int maxChars = (d.width() - 16) / 6;
            if (subject.length() > static_cast<size_t>(maxChars))
                subject = subject.substring(0, maxChars);
            d.print(subject);
            d.setCursor(15, y + 10);
            String from = g_messages[index].from;
            if (from.length() > static_cast<size_t>(maxChars - 2))
                from = from.substring(0, maxChars - 2);
            d.print(from);
        }
        drawFooter("r Refresh  Enter  FN+Del");
        const UiEvent event = input.poll();
        if (event == UiEvent::BACK) return;
        if (event == UiEvent::UP && selected > 0) --selected;
        if (event == UiEvent::DOWN && selected + 1 < g_messageCount)
            ++selected;
        if (event == UiEvent::ENTER && g_messageCount) openMessage(selected);
        if (event == UiEvent::REFRESH) {
            syncInbox();
            selected = 0;
        }
        delay(10);
    }
}

String timestampName(const char* prefix, const char* extension) {
    time_t now = time(nullptr);
    char buffer[64];
    if (now >= 1700000000) {
        struct tm value;
        gmtime_r(&now, &value);
        char stamp[24];
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%SZ", &value);
        snprintf(buffer, sizeof(buffer), "%s_%s.%s", prefix, stamp, extension);
    } else {
        snprintf(buffer, sizeof(buffer), "%s_%lu.%s", prefix,
                 static_cast<unsigned long>(millis()), extension);
    }
    return buffer;
}

void compose() {
    if (!g_config.complete()) {
        modal("Mail", "Configure the account first.", TFT_ORANGE);
        return;
    }
    const String recipient = promptValue("Recipient", "");
    if (!validAddress(recipient)) {
        modal("Compose", "Invalid recipient address.", TFT_RED);
        return;
    }
    const String subject = promptValue("Subject", "");

    String error;
    NetworkTransport::ensureDirectory(DRAFT_ROOT, &error);
    const String draftPath =
        String(DRAFT_ROOT) + "/" + timestampName("draft", "txt");
    if (!NetworkTransport::writeFileAtomic(
            draftPath,
            String("Write the message body here.\nRemove this instruction.\n"),
            error)) {
        modal("Draft error", error, TFT_RED);
        return;
    }

    textEditorOpen(draftPath.c_str());
    String body;
    bool truncated = false;
    if (!NetworkTransport::readFile(draftPath, body, MAX_BODY_FILE, error,
                                    &truncated)) {
        modal("Draft error", error, TFT_RED);
        return;
    }
    if (truncated) {
        modal("Draft error", "Draft exceeds 32 KB.", TFT_RED);
        return;
    }
    body.trim();
    if (body.length() == 0) {
        modal("Compose", "Empty draft kept on SD.", TFT_ORANGE);
        return;
    }
    if (!confirmPopup("Send this message?")) return;

    const String eml = buildEml(g_config, recipient, subject, body);
    showStatus("Mail", "Sending...");
    SmtpSession smtp;
    if (!smtp.send(g_config, recipient, eml, error)) {
        modal("SMTP error",
              error + "\nDraft: " + draftPath, TFT_RED);
        return;
    }

    NetworkTransport::ensureDirectory(SENT_ROOT, &error);
    const String sentPath =
        String(SENT_ROOT) + "/" + timestampName("sent", "eml");
    if (!NetworkTransport::writeFileAtomic(sentPath, eml, error)) {
        modal("Mail sent",
              "Server accepted the message, but local copy failed:\n" + error,
              TFT_ORANGE);
        return;
    }
    NetworkTransport::removeFile(draftPath);
    modal("Mail sent", "Message accepted by SMTP server.", TFT_GREEN);
}

void editJson() {
    String error;
    textEditorOpen(CONFIG_PATH);
    MailConfig updated;
    if (!loadConfig(updated, error)) {
        modal("mail.json", error, TFT_RED);
        return;
    }
    g_config = updated;
    modal("mail.json", "Configuration reloaded.", TFT_GREEN);
}

bool allocateMessageIndex() {
    if (RuntimeMemory::externalAvailable()) {
        g_messages = static_cast<MessageSummary*>(
            RuntimeMemory::allocateExternal(
                MAX_MESSAGES * sizeof(MessageSummary), true));
        if (g_messages) {
            g_messageCapacity = MAX_MESSAGES;
            return true;
        }
    }

    const size_t capacities[] = {16, 12, 8, 4};
    for (size_t capacity : capacities) {
        g_messages = static_cast<MessageSummary*>(
            RuntimeMemory::allocateInternal(
                capacity * sizeof(MessageSummary), true, 40U * 1024U));
        if (g_messages) {
            g_messageCapacity = capacity;
            return true;
        }
    }
    return false;
}

}  // namespace

void run() {
    MailState state;
    g_state = &state;
    inMenu = false;
    NetCore::begin();
    String error;
    NetworkTransport::ensureDirectory("/evil/config", &error);
    NetworkTransport::ensureDirectory(MAIL_ROOT, &error);

    if (!loadConfig(g_config, error)) {
        g_config = MailConfig();
        saveConfig(g_config, error);
    }
    if (!g_messages && !allocateMessageIndex()) {
        modal("Mail", String("Unable to allocate message index; ") +
                          RuntimeMemory::describe(),
              TFT_RED);
        g_state = nullptr;
        inMenu = true;
        return;
    }
    if (g_config.maxMessages > g_messageCapacity)
        g_config.maxMessages = g_messageCapacity;

    const char* const items[] = {
        "Inbox", "Compose", "Account wizard", "Edit mail.json", "Back"};
    size_t selected = 0;
    InputState input;
    while (InputCompat::isEnterPressed()) {
        M5Cardputer.update();
        delay(10);
    }

    bool running = true;
    while (running) {
        String status;
        if (!g_config.complete())
            status = "Account not configured";
        else if (g_config.tlsInsecure)
            status = "WARNING: insecure TLS";
        else
            status = "Verified TLS";
        drawMenu("Mail", items, sizeof(items) / sizeof(items[0]), selected,
                 status);

        const UiEvent event = input.poll();
        if (event == UiEvent::BACK) break;
        if (event == UiEvent::UP && selected > 0) --selected;
        if (event == UiEvent::DOWN && selected + 1 <
                                          sizeof(items) / sizeof(items[0]))
            ++selected;
        if (event == UiEvent::ENTER) {
            switch (selected) {
                case 0:
                    inbox();
                    break;
                case 1:
                    compose();
                    break;
                case 2:
                    accountWizard(g_config);
                    break;
                case 3:
                    editJson();
                    break;
                default:
                    running = false;
                    break;
            }
        }
        NetCore::poll();
        delay(10);
    }
    RuntimeMemory::release(g_messages);
    g_messages = nullptr;
    g_messageCount = 0;
    g_messageCapacity = 0;
    g_state = nullptr;
    inMenu = true;
}

}  // namespace MailApp
