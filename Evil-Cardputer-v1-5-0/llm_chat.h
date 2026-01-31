/*
 * llm_chat.h - LLM Chat Stream Module for Evil-Cardputer
 *
 * Provides streaming chat interface to LLM APIs (Ollama compatible)
 */

#ifndef LLM_CHAT_H
#define LLM_CHAT_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ============================================================================
// External dependencies from main file
// ============================================================================

extern void waitAndReturnToMenu(String message);
extern String getUserInput(bool isPassword);
extern bool inMenu;

// ============================================================================
// LLM Configuration (accessible from main for settings restore)
// ============================================================================

extern String llmHost;
extern int    llmhttpsPort;
extern String llmapiPath;
extern String llmUser;
extern String llmPass;
extern String llmModel;
extern int    llmMaxTokens;

// ============================================================================
// Public Functions
// ============================================================================

// Main entry point - called from menu
void evilLLMChatStream();

// Utility function (may be useful elsewhere)
String encodeBase64(const String& input);

#endif // LLM_CHAT_H
