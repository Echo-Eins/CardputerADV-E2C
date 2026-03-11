#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\sip_attacks.h"
/*
 * sip_attacks.h - SIP/VoIP Attack Module for Evil-Cardputer
 *
 * Contains:
 *   - SIP Scanner (OPTIONS scan)
 *   - SIP Extension Enumeration (INVITE)
 *   - SIP Message Spoofing
 *   - SIP REGISTER Flooding
 *   - SIP Ring All (mass INVITE)
 */

#ifndef SIP_ATTACKS_H
#define SIP_ATTACKS_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <vector>

// ============================================================================
// Forward declarations for external dependencies (defined in main .ino)
// ============================================================================
extern void waitAndReturnToMenu(String message);
extern bool inMenu;
extern int menuBackgroundColor;

// ============================================================================
// SIP Module Public Functions
// ============================================================================

/**
 * SIP OPTIONS Scan - discovers SIP endpoints on network
 * Sends OPTIONS requests to IP/CIDR range and reports responses
 */
void sipScan();

/**
 * SIP Extension Enumeration - finds valid extensions on PBX
 * Uses INVITE requests to enumerate extensions via response codes
 */
void sipEnumExtensions();

/**
 * SIP Message Spoofing - sends spoofed MESSAGE to extension
 * Allows sending text messages with fake caller ID
 */
void sipSpoofMessage();

/**
 * SIP REGISTER Flood - floods target with REGISTER packets
 * Rate-limited packet flood for testing SIP infrastructure
 */
void sipFlood();

/**
 * SIP Ring All - mass INVITE to ring all phones on network
 * Sends INVITE to all IPs in CIDR, waits, then CANCEL
 */
void sipRingAll();

#endif // SIP_ATTACKS_H
