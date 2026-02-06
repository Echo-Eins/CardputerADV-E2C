/*
 * terminals.h - REPL Terminals for Evil-Cardputer
 *
 * Provides multiple terminal/REPL environments:
 * - Calculator: Math expression evaluator with variables
 * - Shell: Device control commands
 * - Future: Lua, JavaScript (Duktape) interpreters
 */

#ifndef TERMINALS_H
#define TERMINALS_H

#include <Arduino.h>
#include <vector>
#include <map>

// ============================================================================
// External dependencies
// ============================================================================

extern void waitAndReturnToMenu(String message);
extern String getUserInput(bool isPassword);
extern bool inMenu;

// ============================================================================
// Constants
// ============================================================================

#define TERM_SCREEN_WIDTH       240
#define TERM_SCREEN_HEIGHT      135
#define TERM_LINE_HEIGHT        11
#define TERM_MAX_HISTORY        50
#define TERM_MAX_OUTPUT_LINES   100

// ============================================================================
// Terminal Types
// ============================================================================

enum TerminalType {
    TERM_CALCULATOR = 0,
    TERM_SHELL,
    TERM_LUA,       // Placeholder
    TERM_JS,        // Placeholder
    TERM_COUNT
};

// ============================================================================
// Public Functions
// ============================================================================

// Main terminal selector menu
void terminalsMenu();

// Individual terminals
void calculatorRepl();
void deviceShell();

// Expression evaluator (can be used standalone)
// Returns NaN on error
double evalExpression(const String& expr, String& error);

// Set/get variable for calculator
void setVariable(const String& name, double value);
double getVariable(const String& name);
void clearVariables();

#endif // TERMINALS_H
