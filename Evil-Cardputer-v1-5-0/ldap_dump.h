/*
 * ldap_dump.h - LDAP Domain Dump Module for Evil-Cardputer
 * Inspired by: https://github.com/dirkjanm/ldapdomaindump
 *
 * Contains:
 *   - LDAP UI Console
 *   - LDAP Bind/Search operations
 *   - Domain enumeration (Users, Groups, Computers, Policy, Trusts, GPOs)
 */

#ifndef LDAP_DUMP_H
#define LDAP_DUMP_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <SD.h>
#include <vector>

// ============================================================================
// External dependencies from main file
// ============================================================================

// UI functions - defined in main .ino, Arduino handles linkage automatically
extern void waitAndReturnToMenu(String message);
extern String getUserInput(bool isPassword);
extern void enterDebounce();

// Network functions - defined in main .ino
// Note: arpRequest and connectWithTimeout removed to avoid C/C++ linkage conflicts
// Arduino's preprocessor resolves these automatically from the .ino file
extern void send_arp(const char* base_ip, std::vector<IPAddress>& hosts);
extern void read_arp_table(const char* base_ip, int start, int end, std::vector<IPAddress>& hosts);

// ============================================================================
// LDAP Module Globals (accessible from main)
// ============================================================================

extern String ldapDomainDN;
extern String ldapDomainNetbios;
extern String ldapUsername;
extern String ldapPassword;

// ============================================================================
// Public Functions
// ============================================================================

// Main entry point - called from menu
void runLDAPDomainDump();

// ASN.1/TLV parsing (may be needed by other modules)
bool readTLV(const uint8_t* buf, int len, int& pos, uint8_t& tag, int& valLen, const uint8_t*& val);
int encodeSeqLength(uint8_t *pkt, int lenPos, int contentLen);

#endif // LDAP_DUMP_H
