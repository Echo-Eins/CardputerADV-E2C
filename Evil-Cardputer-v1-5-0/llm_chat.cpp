/*
 * llm_chat.cpp - LLM Chat Stream Module for Evil-Cardputer
 *
 * Provides streaming chat interface to LLM APIs (Ollama compatible)
 */

#include "llm_chat.h"
#include <M5Cardputer.h>
#include <ArduinoJson.h>
#include <vector>

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

  M5.Display.clear();
  M5.Display.setCursor(5, 5);
  M5.Display.println("Prompt >");

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

    M5.Display.clear();
    M5.Display.setCursor(5, 5);
    M5.Display.println("Send.");
    M5.Display.println("Waiting answer...");

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

    M5.Display.clear();

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

      // Scroll up
      if (M5Cardputer.Keyboard.isKeyPressed(';') && scrollOffset > 0) {
        scrollOffset--;
        M5.Display.clear();
        for (int i = 0; i < linesPerPage; i++) {
          int idx = scrollOffset + i;
          if (idx < lines.size()) {
            M5.Display.setCursor(5, 10 + i * lineHeight);
            M5.Display.println(lines[idx]);
          }
        }
        delay(150);
        continue;
      } else if (M5Cardputer.Keyboard.isKeyPressed('.') && scrollOffset < lines.size()) {
        scrollOffset++;
        M5.Display.clear();
        for (int i = 0; i < linesPerPage; i++) {
          int idx = scrollOffset + i;
          if (idx < lines.size()) {
            M5.Display.setCursor(5, 10 + i * lineHeight);
            M5.Display.println(lines[idx]);
          }
        }
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
        for (int i = 0; i <= token.length(); i++) {
          char c = token[i];
          bool isEnd = (i == token.length());
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
            for (int j = 0; j < word.length(); j++) {
              currentLine += word[j];
              if ((currentLine.length() * charWidth) >= screenWidth) {
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

        if ((lines.size() - scrollOffset) < linesPerPage) {
          M5.Display.clear();
          for (int i = 0; i < linesPerPage; i++) {
            int idx = scrollOffset + i;
            if (idx < lines.size()) {
              M5.Display.setCursor(5, 10 + i * lineHeight);
              M5.Display.println(lines[idx]);
            }
          }

          if (currentLine.length() > 0) {
            M5.Display.setCursor(5, 10 + (lines.size() - scrollOffset) * lineHeight);
            M5.Display.println(currentLine);
          }
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
        M5.Display.clear();
        M5.Display.setCursor(5, 5);
        M5.Display.println("Prompt >");
        break;
      } else if (M5Cardputer.Keyboard.isKeyPressed(';') && scrollOffset > 0) {
        scrollOffset--;
        M5.Display.clear();
      } else if (M5Cardputer.Keyboard.isKeyPressed('.') && scrollOffset < lines.size()) {
        scrollOffset++;
        M5.Display.clear();
      }

      for (int i = 0; i < linesPerPage; i++) {
        int idx = scrollOffset + i;
        if (idx < lines.size()) {
          M5.Display.setCursor(5, 10 + i * lineHeight);
          M5.Display.println(lines[idx]);
        }
      }

      delay(100);
    }
  }
}
