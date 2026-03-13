#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\llm_chat.cpp"
/*
 * llm_chat.cpp - LLM Chat Stream Module for Evil-Cardputer
 *
 * Provides streaming chat interface to LLM APIs (Ollama compatible)
 */

#include "llm_chat.h"
#include <M5Cardputer.h>
#include <ArduinoJson.h>
#include <vector>
#include "gui/gui.h"

using LB = GUI::LegacyBridge;

// ============================================================================
// LLM Configuration
// ============================================================================

String llmHost        = "";
int    llmhttpsPort   = 443;
String llmapiPath     = "/evilOllama/api/generate";
String llmUser        = "";
String llmPass        = "";
String llmModel       = "tinyllama";
int    llmMaxTokens   = 512;

// ============================================================================
// Base64 Encoding
// ============================================================================

String encodeBase64(const String& input) {
  const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String output = "";
  int i = 0;
  uint8_t array3[3];
  uint8_t array4[4];

  int inputLen = input.length();
  int index = 0;

  while (inputLen--) {
    array3[i++] = input[index++];
    if (i == 3) {
      array4[0] = (array3[0] & 0xfc) >> 2;
      array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
      array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
      array4[3] = array3[2] & 0x3f;

      for (i = 0; i < 4; i++)
        output += base64_chars[array4[i]];
      i = 0;
    }
  }

  if (i) {
    for (int j = i; j < 3; j++)
      array3[j] = '\0';

    array4[0] = (array3[0] & 0xfc) >> 2;
    array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
    array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
    array4[3] = array3[2] & 0x3f;

    for (int j = 0; j < i + 1; j++)
      output += base64_chars[array4[j]];

    while ((i++ < 3))
      output += '=';
  }

  return output;
}

// ============================================================================
// LLM Chat Stream
// ============================================================================

// Render a page of lines from a scrollable buffer
static void renderPage(const std::vector<String>& lines, int scrollOffset,
                       int linesPerPage, int lineHeight,
                       const String& trailingLine = "") {
    LB::clear();
    for (int i = 0; i < linesPerPage; i++) {
      int idx = scrollOffset + i;
      if (idx >= 0 && idx < (int)lines.size()) {
        LB::setCursor(5, 10 + i * lineHeight);
        LB::println(lines[idx].c_str());
      }
    }
    if (trailingLine.length() > 0) {
      int visibleCount = (int)lines.size() - scrollOffset;
      if (visibleCount >= 0 && visibleCount < linesPerPage) {
        LB::setCursor(5, 10 + visibleCount * lineHeight);
        LB::println(trailingLine.c_str());
      }
    }
}

void evilLLMChatStream() {
  if (WiFi.localIP().toString() == "0.0.0.0") {
    Serial.println(F("[INFO] Not connected to a network."));
    waitAndReturnToMenu("Not connected to a network.");
    return;
  }

  inMenu = false;

  const int charWidth = 8;
  const int lineHeight = 13;
  const int screenWidth = 208;
  const int linesPerPage = 9;

  LB::clear();
  LB::setCursor(5, 5);
  LB::println("Prompt >");

  while (true) {
    String userPrompt = getUserInput("Prompt > ");
    if (userPrompt.length() == 0) continue;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);

    if (!client.connect(llmHost.c_str(), llmhttpsPort)) {
      waitAndReturnToMenu("Connection failed.");
      return;
    }

    String requestBody =
      "{\"model\":\"" + llmModel +
      "\",\"prompt\":\"" + userPrompt +
      "\",\"stream\":true,\"options\":{\"num_predict\":" + String(llmMaxTokens) + "}}";

    String authRaw = llmUser + ":" + llmPass;
    String authB64 = encodeBase64(authRaw);

    String request =
      "POST " + llmapiPath + " HTTP/1.1\r\n" +
      "Host: " + llmHost + "\r\n" +
      "Authorization: Basic " + authB64 + "\r\n" +
      "Content-Type: application/json\r\n" +
      "Content-Length: " + String(requestBody.length()) + "\r\n" +
      "Connection: close\r\n\r\n" +
      requestBody;

    LB::clear();
    LB::setCursor(5, 5);
    LB::println("Send.");
    LB::println("Waiting answer...");

    client.print(request);

    while (client.connected() && !M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
      String line = client.readStringUntil('\n');
      if (line == "\r") break;
    }

    std::vector<String> lines;
    String currentLine = "";
    int scrollOffset = 0;
    bool gotFirstToken = false;
    unsigned long streamStart = millis();

    LB::clear();

    while (client.connected()) {
      M5.update();
      M5Cardputer.update();

      // User interruption
      if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        client.stop();
        waitAndReturnToMenu("Stream interrupted by user");
        return;
      }

      // Timeout if no token received
      if (!gotFirstToken && millis() - streamStart > 10000) {
        client.stop();
        waitAndReturnToMenu("LLM not responding (timeout)");
        return;
      }

      // Scroll navigation
      if (M5Cardputer.Keyboard.isKeyPressed(';') && scrollOffset > 0) {
        scrollOffset--;
        renderPage(lines, scrollOffset, linesPerPage, lineHeight);
        delay(150);
        continue;
      } else if (M5Cardputer.Keyboard.isKeyPressed('.') && scrollOffset < (int)lines.size()) {
        scrollOffset++;
        renderPage(lines, scrollOffset, linesPerPage, lineHeight);
        delay(150);
        continue;
      }

      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, line);
      if (err) continue;
      if (doc["done"] == true) break;

      if (doc.containsKey("response")) {
        gotFirstToken = true;
        String token = doc["response"].as<String>();
        token.replace("\\n", "\n");

        String word = "";
        for (int i = 0; i <= (int)token.length(); i++) {
          char c = token[i];
          bool isEnd = (i == (int)token.length());
          bool isSpace = (c == ' ' || c == '\n' || isEnd);

          if (!isEnd && !isSpace) {
            word += c;
            continue;
          }

          int wordPixelLength = word.length() * charWidth;
          int linePixelLength = currentLine.length() * charWidth;

          if (linePixelLength + wordPixelLength > screenWidth) {
            lines.push_back(currentLine);
            currentLine = "";
            linePixelLength = 0;
          }

          if (wordPixelLength > screenWidth) {
            for (int j = 0; j < (int)word.length(); j++) {
              currentLine += word[j];
              if ((int)(currentLine.length() * charWidth) >= screenWidth) {
                lines.push_back(currentLine);
                currentLine = "";
              }
            }
          } else {
            currentLine += word;
          }

          if (!isEnd && c != '\n') currentLine += c;
          if (c == '\n') {
            lines.push_back(currentLine);
            currentLine = "";
          }

          word = "";
        }

        if (((int)lines.size() - scrollOffset) < linesPerPage) {
          renderPage(lines, scrollOffset, linesPerPage, lineHeight, currentLine);
        }
      }
    }

    if (currentLine.length() > 0) lines.push_back(currentLine);
    lines.push_back("______");

    while (true) {
      M5.update();
      M5Cardputer.update();

      if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        waitAndReturnToMenu("evilChatStream Stopped");
        return;
      } else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        LB::clear();
        LB::setCursor(5, 5);
        LB::println("Prompt >");
        break;
      } else if (M5Cardputer.Keyboard.isKeyPressed(';') && scrollOffset > 0) {
        scrollOffset--;
        LB::clear();
      } else if (M5Cardputer.Keyboard.isKeyPressed('.') && scrollOffset < (int)lines.size()) {
        scrollOffset++;
        LB::clear();
      }

      renderPage(lines, scrollOffset, linesPerPage, lineHeight);
      delay(100);
    }
  }
}
