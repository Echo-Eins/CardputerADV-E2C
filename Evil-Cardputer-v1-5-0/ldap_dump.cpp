/*
 * ldap_dump.cpp - LDAP Domain Dump Module for Evil-Cardputer
 * Inspired by: https://github.com/dirkjanm/ldapdomaindump
 *
 * Contains:
 *   - LDAP UI Console
 *   - LDAP Bind/Search operations
 *   - Domain enumeration (Users, Groups, Computers, Policy, Trusts, GPOs)
 */

#include "ldap_dump.h"
#include "runtime_memory.h"
#include "input_compat.h"
#include <M5Cardputer.h>
#include "gui/gui.h"

using LB = GUI::LegacyBridge;

// Forward declarations for functions defined in main .ino
// Declared here to avoid C/C++ linkage conflicts with Arduino preprocessor
extern "C" {
    bool arpRequest(IPAddress host);
    bool connectWithTimeout(WiFiClient& client, IPAddress ip, uint16_t port, uint32_t timeout_ms);
}
void send_arp(char* base_ip, std::vector<IPAddress>& hosts);
void read_arp_table(char* base_ip, int start, int end, std::vector<IPAddress>& hosts);

// ============================================================================
// LDAP UI / LOG CONSOLE
// ============================================================================

static const int LDAP_LOG_MAX_LINES   = 80;
static const int LDAP_LOG_PAGE_LINES  = 9;

static String ldapLogLines[LDAP_LOG_MAX_LINES];
static int    ldapLogCount   = 0;
static int    ldapLogScroll  = 0;
static bool   ldapLogFollow  = true;

static IPAddress ldapUiDcIP;
static String    ldapUiBaseDN  = "";
static String    ldapUiPhase   = "IDLE";

// ============================================================================
// LDAP Globals
// ============================================================================

String ldapDomainDN = "";
String ldapDomainNetbios = "";
String ldapUsername = "";
String ldapPassword = "";

static const int LDAP_BUF_SIZE = 8192;
static uint8_t* ldapRespBuf = nullptr;

// ============================================================================
// UI Functions
// ============================================================================

// Common prompt screen for credential/text input
static void ldapUiPromptScreen(const char* label) {
    LB::clear();
    LB::setCursor(5, 0);
    LB::setTextSize(1.5);
    LB::setTextColor(TFT_WHITE, TFT_BLACK);
    LB::println(label);
}

void ldapUiDrawHeader()
{
  LB::fillScreen(TFT_BLACK);
  LB::setTextSize(1);
  LB::setTextColor(TFT_WHITE, TFT_BLACK);

  LB::setCursor(5, 0);
  LB::println("[ LDAP ENUM / LOG ]");

  LB::setCursor(5, 10);
  String dcLine = "DC: " + ldapUiDcIP.toString() + "  Phase: " + ldapUiPhase;
  LB::println(dcLine);

  LB::setCursor(5, 20);
  String dnLine = "BaseDN: " + ldapUiBaseDN;
  if (dnLine.length() > 36) {
    dnLine = dnLine.substring(0, 36);
  }
  LB::println(dnLine);

  LB::setCursor(5, 30);
  LB::println("---------------------------");
}

void ldapUiDrawLogs()
{
  LB::setTextSize(1);
  LB::setTextColor(TFT_WHITE, TFT_BLACK);
  LB::setCursor(5, 40);

  int total = ldapLogCount;
  if (total < 0) total = 0;

  int first = ldapLogScroll;
  if (first < 0) first = 0;
  if (first > total) first = total;

  int last  = first + LDAP_LOG_PAGE_LINES;
  if (last > total) last = total;

  for (int i = first; i < last; ++i) {
    LB::println(ldapLogLines[i]);
  }

  for (int i = last; i < first + LDAP_LOG_PAGE_LINES; ++i) {
    LB::println("");
  }

  LB::println("---------------------------");

  char info[40];
  int page = (LDAP_LOG_PAGE_LINES > 0)
             ? (first / LDAP_LOG_PAGE_LINES) + 1
             : 1;
  snprintf(info, sizeof(info),
           "--- %d/%d lines (Pg %d) ---",
           last, total, page);
  LB::println(info);

  LB::println("[;] UP  [.] DOWN  [ESC] BACK");
}

void ldapUiRedraw()
{
  ldapUiDrawHeader();
  ldapUiDrawLogs();
}

void ldapUiResetLog(const IPAddress &dcIP, const String &baseDN)
{
  ldapUiDcIP   = dcIP;
  ldapUiBaseDN = baseDN;
  ldapUiPhase  = "INIT";

  ldapLogCount  = 0;
  ldapLogScroll = 0;
  ldapLogFollow = true;

  ldapUiRedraw();
}

void ldapUiSetPhase(const String &phase)
{
  ldapUiPhase = phase;
  ldapUiRedraw();
}

void ldapUiLogLine(const String &msg)
{
  Serial.println(msg);

  if (ldapLogCount < LDAP_LOG_MAX_LINES) {
    ldapLogLines[ldapLogCount] = msg;
    ldapLogCount++;
  } else {
    for (int i = 1; i < LDAP_LOG_MAX_LINES; ++i) {
      ldapLogLines[i - 1] = ldapLogLines[i];
    }
    ldapLogLines[LDAP_LOG_MAX_LINES - 1] = msg;
  }

  if (ldapLogFollow) {
    if (ldapLogCount > LDAP_LOG_PAGE_LINES) {
      ldapLogScroll = ldapLogCount - LDAP_LOG_PAGE_LINES;
    } else {
      ldapLogScroll = 0;
    }
  }

  ldapUiRedraw();
}

void ldapUiShowViewer()
{
  ldapLogFollow = false;
  ldapUiRedraw();

  while (true) {
    M5Cardputer.update();

    bool up   = M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = M5Cardputer.Keyboard.isKeyPressed('.');
    bool esc  = InputCompat::isBackPressed();

    bool needRedraw = false;

    if (up) {
      if (ldapLogScroll > 0) {
        ldapLogScroll--;
        needRedraw = true;
      }
    } else if (down) {
      int maxStart = max(0, ldapLogCount - LDAP_LOG_PAGE_LINES);
      if (ldapLogScroll < maxStart) {
        ldapLogScroll++;
        needRedraw = true;
      }
    }

    if (needRedraw) {
      ldapUiRedraw();
      delay(80);
    }

    if (esc) {
      break;
    }

    delay(20);
  }
}

void ldapUiUpdateContext(const IPAddress &dcIP, const String &baseDN)
{
  ldapUiDcIP   = dcIP;
  ldapUiBaseDN = baseDN;
  ldapUiRedraw();
}

// ============================================================================
// String Sanitization
// ============================================================================

String sanitizeLDAPString(const uint8_t* data, int len)
{
    String out = "";
    out.reserve(len);

    for (int i = 0; i < len; i++)
    {
        uint8_t c = data[i];

        if (c >= 32 && c <= 126)
        {
            if (c == '<')      out += "&lt;";
            else if (c == '>') out += "&gt;";
            else if (c == '&') out += "&amp;";
            else               out += (char)c;
        }
        else if (c == '\t' || c == '\n' || c == '\r')
        {
            out += ' ';
        }
    }

    return out;
}

String ldapNormalizeUsername(const String &rawUser)
{
    if (rawUser.indexOf('@') != -1 || rawUser.indexOf('\\') != -1) {
        return rawUser;
    }

    if (ldapDomainDN.length() == 0) {
        return rawUser;
    }

    String fqdn = "";
    int pos = 0;
    while (true) {
        int idx = ldapDomainDN.indexOf("DC=", pos);
        if (idx < 0) break;
        int end = ldapDomainDN.indexOf(',', idx);
        if (end < 0) end = ldapDomainDN.length();
        String dc = ldapDomainDN.substring(idx + 3, end);
        if (fqdn.length() > 0) fqdn += ".";
        fqdn += dc;
        pos = end + 1;
    }

    if (fqdn.length() == 0) {
        return rawUser;
    }

    return rawUser + "@" + fqdn;
}

// ============================================================================
// ASN.1 / TLV Functions
// ============================================================================

bool readTLV(const uint8_t* buf, int len, int& pos, uint8_t& tag, int& valLen, const uint8_t*& val)
{
    if (pos >= len) return false;

    tag = buf[pos++];

    if (pos >= len) return false;
    uint8_t L = buf[pos++];

    if (L < 0x80)
    {
        valLen = L;
    }
    else
    {
        int nb = L & 0x7F;
        if (nb < 1 || nb > 4) return false;
        if (pos + nb > len) return false;

        valLen = 0;
        while (nb--)
        {
            valLen = (valLen << 8) | buf[pos++];
        }
    }

    if (pos + valLen > len) return false;

    val = &buf[pos];
    pos += valLen;

    return true;
}

int encodeSeqLength(uint8_t *pkt, int lenPos, int contentLen)
{
    if (contentLen < 128) {
        pkt[lenPos] = (uint8_t)contentLen;
        return 1;
    } else if (contentLen < 256) {
        memmove(pkt + lenPos + 2, pkt + lenPos + 1, contentLen);
        pkt[lenPos]   = 0x81;
        pkt[lenPos+1] = (uint8_t)contentLen;
        return 2;
    } else {
        memmove(pkt + lenPos + 3, pkt + lenPos + 1, contentLen);
        pkt[lenPos]   = 0x82;
        pkt[lenPos+1] = (uint8_t)((contentLen >> 8) & 0xFF);
        pkt[lenPos+2] = (uint8_t)(contentLen & 0xFF);
        return 3;
    }
}

// ============================================================================
// Attribute Extraction
// ============================================================================

bool extractAttributeValue(
    const uint8_t* buf, int len,
    const char* attrName,
    String& out
){
    out = "";
    int pos = 0;

    while (pos < len)
    {
        uint8_t tag;
        int vlen;
        const uint8_t* val;

        if (!readTLV(buf, len, pos, tag, vlen, val))
            break;

        if (tag != 0x30) {
            continue;
        }

        int p2 = 0;
        uint8_t t2;
        int l2;
        const uint8_t* v2;

        if (!readTLV(val, vlen, p2, t2, l2, v2))
            continue;
        if (t2 != 0x04)
            continue;

        String name = "";
        for (int i = 0; i < l2; i++)
            name += (char)v2[i];

        if (!name.equalsIgnoreCase(attrName)) {
            continue;
        }

        if (!readTLV(val, vlen, p2, t2, l2, v2))
            return false;
        if (t2 != 0x31)
            return false;

        int p3 = 0;
        uint8_t t3;
        int l3;
        const uint8_t* v3;

        if (!readTLV(v2, l2, p3, t3, l3, v3))
            return false;
        if (t3 != 0x04)
            return false;

        out = sanitizeLDAPString(v3, l3);
        return true;
    }

    return false;
}

// ============================================================================
// LDAP Filter Encoding
// ============================================================================

void encodeLDAPFilter(
    uint8_t* pkt,
    int& p,
    const String& filter
){
    int eq = filter.indexOf('=');

    if (eq <= 0 || eq >= filter.length() - 1) {
        String attr = filter;
        attr.trim();

        pkt[p++] = 0x87;
        pkt[p++] = attr.length();
        memcpy(&pkt[p], attr.c_str(), attr.length());
        p += attr.length();
        return;
    }

    String attr = filter.substring(0, eq);
    String val  = filter.substring(eq + 1);
    attr.trim();
    val.trim();

    pkt[p++] = 0xA3;
    int lenPos = p++;

    pkt[p++] = 0x04;
    pkt[p++] = attr.length();
    memcpy(&pkt[p], attr.c_str(), attr.length());
    p += attr.length();

    pkt[p++] = 0x04;
    pkt[p++] = val.length();
    memcpy(&pkt[p], val.c_str(), val.length());
    p += val.length();

    pkt[lenPos] = p - lenPos - 1;
}

// ============================================================================
// Debug Functions
// ============================================================================

void hexDump(const char* label, const uint8_t* buf, int len, int maxBytes = 256)
{
    Serial.printf("%s (len=%d, showing %d bytes):\n",
                  label, len, (len < maxBytes ? len : maxBytes));

    int shown = (len < maxBytes ? len : maxBytes);
    for (int i = 0; i < shown; i++) {
        Serial.printf("%02X ", buf[i]);
        if ((i % 16) == 15) Serial.println();
    }
    Serial.println();
}

void debugLDAPRequestStructure(const uint8_t* pkt, int len) {
    int pos = 0;
    uint8_t tag;
    int vlen;
    const uint8_t* val;

    if (!readTLV(pkt, len, pos, tag, vlen, val)) {
        return;
    }

    int mPos = 0;
    uint8_t t;
    int l;
    const uint8_t* v;

    if (!readTLV(val, vlen, mPos, t, l, v)) {
        return;
    }
    int msgId = 0;
    if (t == 0x02) {
        for (int i = 0; i < l; i++) msgId = (msgId << 8) | v[i];
    }

    if (!readTLV(val, vlen, mPos, t, l, v)) {
        return;
    }

    if (mPos < vlen) {
        uint8_t t2;
        int l2;
        const uint8_t* v2;
        readTLV(val, vlen, mPos, t2, l2, v2);
    }
}

void debugLDAPResultCode(const uint8_t* buf, int len)
{
    bool found = false;
    for (int i = 0; i < len - 3; i++) {
        if (buf[i] == 0x65) {
            int end = i + 50;
            if (end > len) end = len;

            for (int j = i; j < end - 2; j++) {
                if (buf[j] == 0x0A && buf[j+1] == 0x01) {
                    found = true;
                    break;
                }
            }
        }
        if (found) break;
    }
}

// ============================================================================
// LDAP Bind
// ============================================================================

bool ldapBind(WiFiClient& client, const String& user = "", const String& pass = "") {
    uint8_t pkt[256];
    int p = 0;

    pkt[p++] = 0x30;
    int lenPos = p++;

    pkt[p++] = 0x02; pkt[p++] = 0x01; pkt[p++] = 0x01;
    pkt[p++] = 0x60;
    int bindLenPos = p++;

    pkt[p++] = 0x02; pkt[p++] = 0x01; pkt[p++] = 0x03;

    pkt[p++] = 0x04; pkt[p++] = user.length();
    for (int i = 0; i < user.length(); ++i) pkt[p++] = user[i];

    pkt[p++] = 0x80; pkt[p++] = pass.length();
    for (int i = 0; i < pass.length(); ++i) pkt[p++] = pass[i];

    pkt[bindLenPos] = p - bindLenPos - 1;
    pkt[lenPos]     = p - lenPos - 1;

    client.write(pkt, p);
    delay(150);

    uint8_t resp[128];
    int len = client.read(resp, sizeof(resp));

    if (len < 8) {
        client.stop();
        return false;
    }

    for (int i = 0; i < len - 3; ++i) {
        if (resp[i] == 0x61 && resp[i+2] == 0x00 && resp[i+3] == 0x00) {
            return true;
        }
    }

    client.stop();
    return false;
}

// ============================================================================
// RootDSE Query
// ============================================================================

int buildRootDSERequest(uint8_t *pkt, int maxLen)
{
    int p = 0;

    if (maxLen < 64) {
        Serial.println("[RootDSE] ERROR: buffer too small");
        return 0;
    }

    pkt[p++] = 0x30;
    int ldapLenPos      = p++;
    int ldapContentStart = p;

    pkt[p++] = 0x02; pkt[p++] = 0x01; pkt[p++] = 0x01;

    pkt[p++] = 0x63;
    int srLenPos       = p++;
    int srContentStart = p;

    pkt[p++] = 0x04; pkt[p++] = 0x00;

    pkt[p++] = 0x0A; pkt[p++] = 0x01; pkt[p++] = 0x00;
    pkt[p++] = 0x0A; pkt[p++] = 0x01; pkt[p++] = 0x00;
    pkt[p++] = 0x02; pkt[p++] = 0x01; pkt[p++] = 0x00;
    pkt[p++] = 0x02; pkt[p++] = 0x01; pkt[p++] = 0x00;
    pkt[p++] = 0x01; pkt[p++] = 0x01; pkt[p++] = 0x00;

    encodeLDAPFilter(pkt, p, "objectClass");

    pkt[p++] = 0x30;
    int attrSeqLenPos = p++;

    const char *attr = "defaultNamingContext";
    int attrLen = strlen(attr);
    pkt[p++] = 0x04;
    pkt[p++] = attrLen;
    memcpy(pkt + p, attr, attrLen);
    p += attrLen;

    int attrContentLen = p - (attrSeqLenPos + 1);
    int attrLenBytes   = encodeSeqLength(pkt, attrSeqLenPos, attrContentLen);
    p += (attrLenBytes - 1);

    int srContentLen = p - srContentStart;
    int srLenBytes   = encodeSeqLength(pkt, srLenPos, srContentLen);
    p += (srLenBytes - 1);

    int ldapContentLen = p - ldapContentStart;
    int ldapLenBytes   = encodeSeqLength(pkt, ldapLenPos, ldapContentLen);
    p += (ldapLenBytes - 1);

    Serial.printf("[RootDSE] buildRootDSERequest: final len=%d\n", p);
    return p;
}

bool getDefaultNamingContext(WiFiClient &client)
{
    Serial.println("[RootDSE] Sending RootDSE request...");

    uint8_t pkt[128];
    int pktLen = buildRootDSERequest(pkt, sizeof(pkt));
    if (pktLen <= 0) {
        Serial.println("[RootDSE] ERROR: buildRootDSERequest() failed");
        return false;
    }

    Serial.printf("[RootDSE] Request len=%d\n", pktLen);
    hexDump("[RootDSE] Request HEX", pkt, pktLen, pktLen);

    int written = client.write(pkt, pktLen);
    Serial.printf("[RootDSE] client.write() returned %d\n", written);

    uint8_t buf[512];
    int total = 0;
    uint32_t t0 = millis();

    while (millis() - t0 < 1000)
    {
        int avail = client.available();
        if (avail > 0)
        {
            int toRead = min(avail, (int)(sizeof(buf) - total));
            if (toRead <= 0) break;

            int r = client.read(buf + total, toRead);
            if (r > 0) {
                total += r;
                t0 = millis();
            }
        }
        delay(5);
    }

    Serial.printf("[RootDSE] Received %d bytes\n", total);
    if (total > 0) {
        hexDump("[RootDSE] Response HEX", buf, total, total);
    }

    if (total < 10)
    {
        Serial.println("[RootDSE] FAIL (no data)");
        return false;
    }

    int pos = 0;
    uint8_t tag;
    int vlen;
    const uint8_t* val;

    if (!readTLV(buf, total, pos, tag, vlen, val) || tag != 0x30) {
        Serial.printf("[RootDSE] ERROR: top-level not SEQUENCE (tag=0x%02X)\n", tag);
        return false;
    }

    const uint8_t* msgBuf = val;
    int msgLen = vlen;

    int mPos = 0;
    uint8_t t;
    int l;
    const uint8_t* v;

    if (!readTLV(msgBuf, msgLen, mPos, t, l, v) || t != 0x02) {
        Serial.println("[RootDSE] ERROR: cannot read messageID");
        return false;
    }

    int msgId = 0;
    for (int i = 0; i < l; i++) msgId = (msgId << 8) | v[i];
    Serial.printf("[RootDSE] messageID=%d\n", msgId);

    if (!readTLV(msgBuf, msgLen, mPos, t, l, v)) {
        Serial.println("[RootDSE] ERROR: cannot read protocolOp");
        return false;
    }
    if (t != 0x64) {
        Serial.printf("[RootDSE] ERROR: protocolOp is not SearchResultEntry (tag=0x%02X)\n", t);
        return false;
    }

    const uint8_t* entryBuf = v;
    int entryLen = l;

    int ePos = 0;
    uint8_t t2;
    int l2;
    const uint8_t* v2;

    if (!readTLV(entryBuf, entryLen, ePos, t2, l2, v2)) {
        Serial.println("[RootDSE] ERROR: cannot read objectName");
        return false;
    }

    if (!readTLV(entryBuf, entryLen, ePos, t2, l2, v2) || t2 != 0x30) {
        Serial.printf("[RootDSE] ERROR: attributes is not SEQUENCE (tag=0x%02X)\n", t2);
        return false;
    }

    const uint8_t* attrBuf = v2;
    int attrLen = l2;

    String dnStr;
    if (!extractAttributeValue(attrBuf, attrLen, "defaultNamingContext", dnStr)) {
        Serial.println("[RootDSE] ERROR: defaultNamingContext not found in attributes");
        return false;
    }

    ldapDomainDN = dnStr;

    int x = ldapDomainDN.indexOf("DC=");
    if (x != -1)
    {
        int c = ldapDomainDN.indexOf(',', x);
        if (c < 0) c = ldapDomainDN.length();
        ldapDomainNetbios = ldapDomainDN.substring(x+3, c);
    }

    Serial.println("[RootDSE] DN      = " + ldapDomainDN);
    Serial.println("[RootDSE] NETBIOS = " + ldapDomainNetbios);

    return true;
}

// ============================================================================
// Paged Search Request Builder
// ============================================================================

int buildLDAPSearchPaged(
    uint8_t *pkt,
    int maxLen,
    const char *baseDN,
    const char *filter,
    const char **attrs,
    int attrCount,
    int pageSize,
    const uint8_t *cookie,
    int cookieLen
) {
    int p = 0;

    if (!pkt || maxLen < 64) {
        return 0;
    }

    String baseStr   = baseDN  ? String(baseDN)  : String("");
    String filterStr = filter  ? String(filter)  : String("");

    pkt[p++] = 0x30;
    int ldapLenPos      = p++;
    int ldapContentStart = p;

    pkt[p++] = 0x02; pkt[p++] = 0x01; pkt[p++] = 0x05;

    pkt[p++] = 0x63;
    int srLenPos       = p++;
    int srContentStart = p;

    uint8_t dnLen = baseStr.length();
    if (p + 2 + dnLen > maxLen) {
        return 0;
    }
    pkt[p++] = 0x04;
    pkt[p++] = dnLen;
    memcpy(pkt + p, baseStr.c_str(), dnLen);
    p += dnLen;

    pkt[p++] = 0x0A; pkt[p++] = 0x01; pkt[p++] = 0x02;
    pkt[p++] = 0x0A; pkt[p++] = 0x01; pkt[p++] = 0x00;
    pkt[p++] = 0x02; pkt[p++] = 0x01; pkt[p++] = 0x00;
    pkt[p++] = 0x02; pkt[p++] = 0x01; pkt[p++] = 0x00;
    pkt[p++] = 0x01; pkt[p++] = 0x01; pkt[p++] = 0x00;

    encodeLDAPFilter(pkt, p, filterStr);

    pkt[p++] = 0x30;
    int attrSeqLenPos = p++;
    int usedAttrs = attrCount;
    if (usedAttrs < 0) usedAttrs = 0;
    if (usedAttrs > 16) usedAttrs = 16;

    for (int i = 0; i < usedAttrs; ++i) {
        const char *a = attrs[i];
        if (!a) continue;
        int alen = strlen(a);
        pkt[p++] = 0x04;
        pkt[p++] = alen;
        memcpy(pkt + p, a, alen);
        p += alen;
    }

    {
        int attrContentLen = p - (attrSeqLenPos + 1);
        int bytes = encodeSeqLength(pkt, attrSeqLenPos, attrContentLen);
        p += (bytes - 1);
    }

    {
        int srContentLen = p - srContentStart;
        int bytes = encodeSeqLength(pkt, srLenPos, srContentLen);
        p += (bytes - 1);
    }

    pkt[p++] = 0xA0;
    int ctrlSeqLenPos = p++;
    int ctrlStart     = p;

    pkt[p++] = 0x30;
    int ctrlInnerLenPos = p++;
    int ctrlInnerStart  = p;

    const char *oid = "1.2.840.113556.1.4.319";
    int oidLen = strlen(oid);
    pkt[p++] = 0x04; pkt[p++] = oidLen;
    memcpy(pkt + p, oid, oidLen);
    p += oidLen;

    pkt[p++] = 0x01; pkt[p++] = 0x01; pkt[p++] = 0x01;

    pkt[p++] = 0x04;
    int cvLenPos = p++;
    int cvStart  = p;

    pkt[p++] = 0x30;
    int cvSeqLenPos = p++;
    int cvSeqStart  = p;

    pkt[p++] = 0x02;
    pkt[p++] = 0x01;
    pkt[p++] = (uint8_t)pageSize;

    pkt[p++] = 0x04;
    if (cookieLen < 0x80) {
        pkt[p++] = (uint8_t)cookieLen;
    } else if (cookieLen <= 0xFF) {
        pkt[p++] = 0x81;
        pkt[p++] = (uint8_t)cookieLen;
    } else {
        pkt[p++] = 0x82;
        pkt[p++] = (uint8_t)((cookieLen >> 8) & 0xFF);
        pkt[p++] = (uint8_t)(cookieLen & 0xFF);
    }

    if (cookieLen > 0) {
        memcpy(pkt + p, cookie, cookieLen);
        p += cookieLen;
    }

    {
        int contentLen = p - cvSeqStart;
        int bytes = encodeSeqLength(pkt, cvSeqLenPos, contentLen);
        p += (bytes - 1);
    }

    {
        int contentLen = p - cvStart;
        int bytes = encodeSeqLength(pkt, cvLenPos, contentLen);
        p += (bytes - 1);
    }

    {
        int contentLen = p - ctrlInnerStart;
        int bytes = encodeSeqLength(pkt, ctrlInnerLenPos, contentLen);
        p += (bytes - 1);
    }

    {
        int contentLen = p - ctrlStart;
        int bytes = encodeSeqLength(pkt, ctrlSeqLenPos, contentLen);
        p += (bytes - 1);
    }

    int ldapContentLen = p - ldapContentStart;
    {
        int bytes = encodeSeqLength(pkt, ldapLenPos, ldapContentLen);
        p += (bytes - 1);
    }

    return p;
}

// ============================================================================
// Paging Cookie Extraction
// ============================================================================

bool extractPagingCookie(
    const uint8_t* buf,
    int len,
    uint8_t* cookieOut,
    int maxCookieLen,
    int &cookieLenOut,
    int &serverPageSizeOut
){
    cookieLenOut      = 0;
    serverPageSizeOut = 0;

    const char* oidStr = "1.2.840.113556.1.4.319";
    int oidLen = strlen(oidStr);

    for (int i = 0; i <= len - oidLen; ++i) {
        if (memcmp(buf + i, oidStr, oidLen) != 0)
            continue;

        int p = i + oidLen;
        int guard = 0;
        while (p < len && buf[p] != 0x04 && guard < 64) {
            ++p;
            ++guard;
        }
        if (p >= len || buf[p] != 0x04) {
            return false;
        }

        int pos = p;
        uint8_t tag;
        int vLen;
        const uint8_t* val;

        if (!readTLV(buf, len, pos, tag, vLen, val) || tag != 0x04) {
            return false;
        }

        if (vLen <= 0) {
            return false;
        }

        int pos2 = 0;
        uint8_t tag2;
        int len2;
        const uint8_t* val2;

        if (!readTLV(val, vLen, pos2, tag2, len2, val2) || tag2 != 0x30) {
            return false;
        }

        if (len2 <= 0) {
            return false;
        }

        int pos3 = 0;
        uint8_t tag3;
        int len3;
        const uint8_t* val3;

        if (!readTLV(val2, len2, pos3, tag3, len3, val3) || tag3 != 0x02) {
            return false;
        }

        int pageSize = 0;
        for (int k = 0; k < len3; ++k) {
            pageSize = (pageSize << 8) | val3[k];
        }
        serverPageSizeOut = pageSize;

        if (!readTLV(val2, len2, pos3, tag3, len3, val3) || tag3 != 0x04) {
            return false;
        }

        if (len3 <= 0) {
            cookieLenOut = 0;
            return true;
        }

        if (len3 > maxCookieLen) {
            len3 = maxCookieLen;
        }

        memcpy(cookieOut, val3, len3);
        cookieLenOut = len3;

        return true;
    }

    return false;
}

// ============================================================================
// Search Response Parser
// ============================================================================

int parseLDAPSearchResponse(
    const uint8_t* buf,
    int len,
    const std::vector<String>& attrs,
    fs::File &outFile
){
    int pos = 0;
    int entryCount = 0;

    while (pos < len) {
        uint8_t tag;
        int vlen;
        const uint8_t* val;

        if (!readTLV(buf, len, pos, tag, vlen, val)) {
            break;
        }

        if (tag != 0x30) {
            continue;
        }

        int mPos = 0;
        uint8_t t;
        int l;
        const uint8_t* v;

        if (!readTLV(val, vlen, mPos, t, l, v)) {
            continue;
        }

        if (!readTLV(val, vlen, mPos, t, l, v)) {
            continue;
        }

        if (t == 0x64) {
            const uint8_t* entryBuf = v;
            int entryLen = l;

            int ePos = 0;
            uint8_t t3;
            int l3;
            const uint8_t* v3;

            if (!readTLV(entryBuf, entryLen, ePos, t3, l3, v3)) {
                continue;
            }

            if (!readTLV(entryBuf, entryLen, ePos, t3, l3, v3)) {
                continue;
            }
            if (t3 != 0x30) {
                continue;
            }

            const uint8_t* attrBuf = v3;
            int attrLen = l3;

            std::vector<String> values;
            values.resize(attrs.size());

            bool hasSomething = false;
            for (int i = 0; i < (int)attrs.size(); i++) {
                String valStr;
                if (extractAttributeValue(attrBuf, attrLen, attrs[i].c_str(), valStr)) {
                    values[i] = valStr;
                    if (valStr.length() > 0) hasSomething = true;
                } else {
                    values[i] = "";
                }
            }

            if (hasSomething) {
                outFile.print("<tr>");
                for (int i = 0; i < (int)values.size(); i++) {
                    outFile.print("<td>");
                    outFile.print(values[i]);
                    outFile.print("</td>");
                }
                outFile.println("</tr>");
                entryCount++;
            }
        }
        else if (t == 0x65) {
            debugLDAPResultCode(val, vlen);
        }
        else if (t == 0x73) {
            Serial.println("[LDAP] SearchResultReference (referral) received (ignored).");
        }
    }

    return entryCount;
}

// ============================================================================
// Paged Search Loop
// ============================================================================

void ldapSearchPagedLoop(const IPAddress &dcIP,
                         const String &baseDN,
                         const String &filter,
                         const std::vector<String> &attrs,
                         fs::File &outFile)
{
    const int MAX_ATTRS = 16;
    int attrCount = attrs.size();
    if (attrCount > MAX_ATTRS) {
        attrCount = MAX_ATTRS;
    }

    const char *attrListC[MAX_ATTRS];
    for (int i = 0; i < attrCount; ++i) {
        attrListC[i] = attrs[i].c_str();
    }

    uint8_t cookie[1024];
    int cookieLen = 0;
    memset(cookie, 0, sizeof(cookie));

    int page         = 1;
    int totalEntries = 0;

    int pageSize = 5;

    Serial.println("[LDAP] Starting paged search loop...");

    while (true) {

        Serial.println("[LDAP] Search: doing AUTH bind...");

        const int MAX_CONN_ATTEMPTS = 3;
        const int MAX_BIND_ATTEMPTS = 3;

        WiFiClient cli;
        bool tcpOk  = false;
        bool bindOk = false;

        for (int attempt = 1; attempt <= MAX_CONN_ATTEMPTS; ++attempt) {
            cli.stop();
            if (cli.connect(dcIP, 389)) {
                tcpOk = true;
                if (attempt > 1) {
                    Serial.printf("[LDAP] connect() succeeded after %d attempts\n", attempt);
                    ldapUiLogLine("[INFO] Page " + String(page) +
                                  ": TCP connect OK after " + String(attempt) + " attempts");
                }
                break;
            }

            Serial.printf("[LDAP] ERROR: connect() failed in search loop (attempt %d/%d)\n",
                          attempt, MAX_CONN_ATTEMPTS);
            ldapUiLogLine("[ERROR] Page " + String(page) +
                          ": TCP connect failed (attempt " + String(attempt) + "/" +
                          String(MAX_CONN_ATTEMPTS) + ")");

            delay(300);
        }

        if (!tcpOk) {
            Serial.println("[LDAP] FATAL: giving up after TCP retries");
            ldapUiLogLine("[FATAL] Page " + String(page) +
                          ": TCP connect failed after retries");
            cli.stop();
            break;
        }

        for (int attempt = 1; attempt <= MAX_BIND_ATTEMPTS; ++attempt) {
            if (ldapBind(cli, ldapUsername, ldapPassword)) {
                bindOk = true;
                if (attempt > 1) {
                    Serial.printf("[LDAP] AUTH bind succeeded after %d attempts\n", attempt);
                    ldapUiLogLine("[INFO] Page " + String(page) +
                                  ": AUTH bind OK after " + String(attempt) + " attempts");
                }
                break;
            }

            Serial.printf("[LDAP] ERROR: AUTH bind failed in search loop (attempt %d/%d)\n",
                          attempt, MAX_BIND_ATTEMPTS);
            ldapUiLogLine("[ERROR] Page " + String(page) +
                          ": AUTH bind failed (attempt " + String(attempt) + "/" +
                          String(MAX_BIND_ATTEMPTS) + ")");

            delay(200);
        }

        if (!bindOk) {
            Serial.println("[LDAP] FATAL: giving up after AUTH bind retries");
            ldapUiLogLine("[FATAL] Page " + String(page) +
                          ": AUTH bind failed after retries");
            cli.stop();
            break;
        }

        uint8_t pkt[2048];
        int pktLen = buildLDAPSearchPaged(
            pkt,
            sizeof(pkt),
            baseDN.c_str(),
            filter.c_str(),
            attrListC,
            attrCount,
            pageSize,
            cookie,
            cookieLen
        );

        if (pktLen <= 0) {
            Serial.printf("[LDAP] ERROR: buildLDAPSearchPaged() returned %d\n", pktLen);
            cli.stop();
            break;
        }

        Serial.printf("[LDAP] Sending paged SearchRequest (len=%d) for page %d (cookieLen=%d, pageSize=%d)\n",
                      pktLen, page, cookieLen, pageSize);

        debugLDAPRequestStructure(pkt, pktLen);

        cli.write(pkt, pktLen);

        int respLen = 0;
        memset(ldapRespBuf, 0, LDAP_BUF_SIZE);

        uint32_t t0 = millis();
        while (millis() - t0 < 2000) {
            int avail = cli.available();
            if (avail > 0) {
                int toRead = avail;
                if (respLen + toRead > LDAP_BUF_SIZE) {
                    toRead = LDAP_BUF_SIZE - respLen;
                }
                if (toRead <= 0) break;

                int r = cli.read(ldapRespBuf + respLen, toRead);
                if (r > 0) {
                    respLen += r;
                    t0 = millis();
                }
            } else {
                delay(5);
            }
        }
        cli.stop();

        if (respLen <= 0) {
            Serial.printf("[LDAP] ERROR: empty or invalid response from DC (respLen=%d)\n", respLen);
            break;
        }

        Serial.printf("[LDAP] Search response received (%d bytes)\n", respLen);

        int entriesThisPage = parseLDAPSearchResponse(ldapRespBuf, respLen, attrs, outFile);
        totalEntries += entriesThisPage;
        ldapUiLogLine("[PAGE] " + String(page) + ": " + String(totalEntries) + " entries");

        Serial.printf("[LDAP] Page %d: %d entries parsed (total so far: %d)\n", page, entriesThisPage, totalEntries);

        uint8_t newCookie[1024];
        int newCookieLen = 0;
        int serverPageSize = 0;

        if (!extractPagingCookie(
                ldapRespBuf,
                respLen,
                newCookie,
                sizeof(newCookie),
                newCookieLen,
                serverPageSize))
        {
            Serial.println("[LDAP] WARNING: extractPagingCookie() failed or OID not found -> stop.");
            break;
        }

        if (newCookieLen == 0) {
            Serial.println("[LDAP] Cookie empty -> enumeration complete.");
            break;
        }

        memcpy(cookie, newCookie, newCookieLen);
        cookieLen = newCookieLen;

        page++;
    }

    Serial.printf("[LDAP] Total entries parsed in this search (all pages): %d\n", totalEntries);
}

// ============================================================================
// DC Detection and Bind
// ============================================================================

bool detectAndBindToDC(IPAddress &dcIP)
{
    Serial.println();
    Serial.println("----------------------------------------");
    Serial.println("[LDAP] START detectAndBindToDC()");
    Serial.println("----------------------------------------");

    ldapUiPromptScreen("Enter /24 or IP:");
    IPAddress local = WiFi.localIP();
    LB::print("Current : ");
    LB::println(local.toString());
    enterDebounce();

    String netInput = getUserInput("NET or IP:");
    netInput.trim();
    Serial.println("[INPUT] User typed: " + netInput);

    if (netInput.length() < 7 || netInput.indexOf('.') == -1)
    {
        Serial.println("[ERROR] Invalid IP or /24 format.");
        waitAndReturnToMenu("Invalid IP");
        return false;
    }

    int dotCount = 0;
    for (char c : netInput) if (c == '.') dotCount++;

    bool isSingleIP = (dotCount == 3);
    std::vector<IPAddress> hosts;

    IPAddress subnet = WiFi.subnetMask();
    Serial.println("[INFO] Local IP : " + local.toString());
    Serial.println("[INFO] Netmask  : " + subnet.toString());

    ldapUiResetLog(IPAddress(0,0,0,0), "");
    ldapUiSetPhase("SCAN");

    if (isSingleIP){
        if (!dcIP.fromString(netInput)) {
            Serial.println("[ERROR] Invalid IP format");
            ldapUiLogLine("[ERROR] Invalid IP format");
            waitAndReturnToMenu("Invalid IP");
            return false;
        }

        ldapUiLogLine("[MODE] Direct IP: " + dcIP.toString());

        WiFiClient c;
        bool reachable = false;
        if (c.connect(dcIP, 389)) {
            ldapUiLogLine("[TEST] Checking LDAP port...");
            if (ldapBind(c, "", "")) {
                ldapUiLogLine("[TEST] Simple bind OK");
                if (getDefaultNamingContext(c)) {
                    ldapUiLogLine("[RootDSE] BaseDN = " + ldapDomainDN);
                    ldapUiUpdateContext(dcIP, ldapDomainDN);
                } else {
                    ldapUiLogLine("[WARN] RootDSE query failed - BaseDN unknown");
                }
                reachable = true;
            } else {
                ldapUiLogLine("[WARN] Simple bind failed (still continuing)");
            }
            c.stop();
        }

        if (!reachable) {
            ldapUiLogLine("[ERROR] LDAP service not responding");
            Serial.println("[LDAP] No response from DC IP");
        }

        enterDebounce();
        ldapUiPromptScreen("AD Login:");
        String rawUser = getUserInput("AD Login:");
        ldapUsername = ldapNormalizeUsername(rawUser);
        ldapUiLogLine("[AUTH] Login: " + ldapUsername);
        enterDebounce();
        ldapUiPromptScreen("AD Password:");
        ldapPassword = getUserInput("AD Password:");

        ldapUiLogLine("[AUTH] Trying authenticated bind...");
        WiFiClient cli;
        if (!cli.connect(dcIP, 389))
        {
            ldapUiLogLine("[ERROR] TCP connect failed");
            return false;
        }

        bool ok = ldapBind(cli, ldapUsername, ldapPassword);
        if (ok){
          if (getDefaultNamingContext(cli)){
              ldapUiLogLine("[RootDSE] BaseDN = " + ldapDomainDN);
              ldapUiUpdateContext(dcIP, ldapDomainDN);
          } else {
              ldapUiLogLine("[WARN] RootDSE query failed - BaseDN unknown");
          }
        }
        cli.stop();
        ldapUiLogLine(ok ? "[AUTH] SUCCESS" : "[AUTH] FAILED");
        return ok;
    }

    // MODE /24 NETWORK SCAN
    String baseStr = netInput + ".";
    char base_ip[16];
    memset(base_ip, 0, sizeof(base_ip));
    baseStr.toCharArray(base_ip, sizeof(base_ip));

    uint8_t o1, o2, o3;
    sscanf(base_ip, "%hhu.%hhu.%hhu.", &o1, &o2, &o3);

    ldapUiLogLine("[SCAN] Network: " + baseStr + "0-254");
    bool sameSubnet =
        ((local[0] & subnet[0]) == (o1 & subnet[0])) &&
        ((local[1] & subnet[1]) == (o2 & subnet[1])) &&
        ((local[2] & subnet[2]) == (o3 & subnet[2]));

    ldapUiLogLine(String("[SCAN] Mode: ") + (sameSubnet ? "ARP" : "TCP"));
    Serial.println(sameSubnet ? "[SCAN] Same subnet -> ARP" : "[SCAN] TCP fallback");

    if (sameSubnet)
    {
        send_arp(base_ip, hosts);
        read_arp_table(base_ip, 1, 254, hosts);
        for (int i = 1; i <= 254; i++)
        {
            IPAddress target(o1, o2, o3, i);
            if (arpRequest(target))
            {
                if (std::find(hosts.begin(), hosts.end(), target) == hosts.end())
                    hosts.push_back(target);
            }
            delayMicroseconds(80);
        }
    }
    else
    {
        for (int i = 1; i <= 254; i++)
        {
            IPAddress target(o1, o2, o3, i);
            WiFiClient tmp;
            if (connectWithTimeout(tmp, target, 389, 200))
            {
                hosts.push_back(target);
                tmp.stop();
            }
            delay(10);
        }
    }

    // LDAP Probing
    for (auto ip : hosts)
    {
        WiFiClient cli;
        if (!cli.connect(ip, 389)) continue;
        if (!ldapBind(cli, "", "")) continue;
        if (!getDefaultNamingContext(cli)) continue;
        cli.stop();
        dcIP = ip;
        ldapUiUpdateContext(dcIP, ldapDomainDN);
        break;
    }

    if (dcIP[0] == 0)
    {
        ldapUiLogLine("[SCAN] No DC found.");
        return false;
    }

    enterDebounce();
    ldapUiPromptScreen("AD Login:");
    String rawUser = getUserInput("AD Login:");
    ldapUsername = ldapNormalizeUsername(rawUser);
    ldapUiLogLine("[AUTH] Login: " + ldapUsername);

    enterDebounce();
    ldapUiPromptScreen("AD Password:");
    ldapPassword = getUserInput("AD Password:");

    WiFiClient cli;
    if (!cli.connect(dcIP, 389))
    {
        ldapUiLogLine("[ERROR] TCP connect failed during AUTH");
        return false;
    }

    bool ok = ldapBind(cli, ldapUsername, ldapPassword);
    cli.stop();
    ldapUiLogLine(ok ? "[AUTH] SUCCESS" : "[AUTH] FAILED");
    return ok;
}

// ============================================================================
// HTML Template
// ============================================================================

static const char LDAP_HTML_UI[] PROGMEM =
"<style>:root{--bg:#05070d;--panel:#0b0f1e;--hdr:#140019;--grid:#3a003a;--txt:#e6e6e6;--mut:#8a8a8a;--red:#ff003c;--pink:#ff2a6d;--cyan:#00eaff;--violet:#8f00ff;--hover:#1a0024}body{margin:0;padding:16px;background:radial-gradient(circle at 50% 0,#12001a,#05070d 60%);color:var(--txt);font-family:monospace;font-size:13px}h2{margin:0 0 14px;font-size:20px;color:var(--red);letter-spacing:1px;text-transform:uppercase;text-shadow:0 0 6px rgba(255,0,60,.6),0 0 18px rgba(255,0,60,.4);border-bottom:1px solid var(--grid);padding-bottom:6px}table{width:100%;border-collapse:collapse;background:var(--panel);box-shadow:0 0 0 1px var(--grid),0 0 32px rgba(255,0,90,.25)}th{background:linear-gradient(180deg,#24001f,#140019);color:var(--pink);border:1px solid var(--grid);padding:7px 10px;text-align:left;white-space:nowrap;font-weight:700;text-shadow:0 0 6px rgba(255,42,109,.5);cursor:pointer;user-select:none}th::after{content:\"\";float:right;opacity:.4}th.sorted-asc::after{content:\" ▲\";color:var(--cyan);opacity:1}th.sorted-desc::after{content:\" ▼\";color:var(--red);opacity:1}td{border:1px solid var(--grid);padding:7px 10px;white-space:nowrap;max-width:520px;overflow:hidden;text-overflow:ellipsis}tr:nth-child(even) td{background:#090c1f}tr:hover td{background:var(--hover);box-shadow:inset 0 0 0 9999px rgba(255,0,90,.08),inset 0 0 12px rgba(255,0,90,.6)}td:empty{color:var(--mut);font-style:italic}td.ts{color:var(--cyan)}td.ts::after{content:\" ⏱\";opacity:.4}td.ts-old{color:var(--mut)}td.ts-recent{color:#00ff9c;font-weight:600}::-webkit-scrollbar{height:8px}::-webkit-scrollbar-thumb{background:linear-gradient(180deg,var(--red),var(--violet))}</style>"
"<script>document.addEventListener(\"DOMContentLoaded\",()=>{document.querySelectorAll(\"th\").forEach((h,i)=>{h.addEventListener(\"click\",()=>{const t=h.closest(\"table\"),r=[...t.querySelectorAll(\"tr\")].slice(1),asc=!h.classList.contains(\"sorted-asc\");t.querySelectorAll(\"th\").forEach(x=>x.classList.remove(\"sorted-asc\",\"sorted-desc\"));h.classList.add(asc?\"sorted-asc\":\"sorted-desc\");r.sort((x,y)=>{let A=x.children[i].dataset.raw||x.children[i].innerText.trim(),B=y.children[i].dataset.raw||y.children[i].innerText.trim();return!isNaN(A)&&!isNaN(B)?asc?A-B:B-A:asc?A.localeCompare(B,void 0,{numeric:!0}):B.localeCompare(A,void 0,{numeric:!0})});r.forEach(e=>t.appendChild(e))})});const E=11644473600000n,F=t=>new Date(Number(BigInt(t)/10000n-E)),L=s=>new Date(s.slice(0,4)+\"-\"+s.slice(4,6)+\"-\"+s.slice(6,8)+\"T\"+s.slice(8,10)+\":\"+s.slice(10,12)+\":\"+s.slice(12,14)+\"Z\"),M=d=>d.getUTCFullYear()+\"-\"+String(d.getUTCMonth()+1).padStart(2,\"0\")+\"-\"+String(d.getUTCDate()).padStart(2,\"0\")+\" \"+String(d.getUTCHours()).padStart(2,\"0\")+\":\"+String(d.getUTCMinutes()).padStart(2,\"0\")+\":\"+String(d.getUTCSeconds()).padStart(2,\"0\")+\" UTC\";document.querySelectorAll(\"td\").forEach(td=>{const v=td.innerText.trim();let d=null;/^\\d{16,}$/.test(v)?d=F(v):/^\\d{14}\\.0Z$/.test(v)&&(d=L(v));if(d&&!isNaN(d)){td.dataset.raw=v,td.innerText=M(d),td.title=\"RAW: \"+v,td.classList.add(\"ts\");const a=(Date.now()-d.getTime())/864e5;a>1825?td.classList.add(\"ts-old\"):a<90&&td.classList.add(\"ts-recent\")}})});</script>";

// ============================================================================
// Extract and Save to HTML File
// ============================================================================

void ldapExtractAndSave(
    const IPAddress& dcIP,
    String& baseDN,
    const String& filter,
    const char* filename,
    const char* title,
    const std::vector<String>& attributes
){
    Serial.println();
    Serial.println("--------------------------------------------");
    Serial.println("[LDAP] START ldapExtractAndSave()");
    Serial.println("--------------------------------------------");
    Serial.println("[LDAP] Target:   " + dcIP.toString());
    Serial.println("[LDAP] BaseDN:   " + baseDN);
    Serial.println("[LDAP] Filter:   " + filter);
    Serial.println("[LDAP] File:     " + String(filename));
    Serial.println("[LDAP] Title:    " + String(title));
    for (auto &a : attributes) {
        Serial.println("   - " + a);
    }

    if (baseDN.length() < 3){
        Serial.println("[LDAP] ERROR: BaseDN invalid.");
        ldapUiLogLine("[ERROR] BaseDN invalid, aborting extract.");
        return;
    }

    SD.mkdir("/evil/LDAP");
    String folder = "/evil/LDAP/" + ldapDomainNetbios;
    SD.mkdir(folder);

    String fullPath = folder + "/" + filename;

    File fil = SD.open(fullPath, FILE_WRITE);
    if (!fil){
        Serial.println("[LDAP] ERROR: Cannot open file: " + fullPath);
        ldapUiLogLine(String("[ERROR] Cannot open file: ") + fullPath);
        return;
    }
    Serial.println("[LDAP] File opened -> " + fullPath);

    fil.println("<html><head><meta charset='utf-8'>");
    fil.print(FPSTR(LDAP_HTML_UI));
    fil.println("<title>" + String(title) + "</title></head><body>");
    fil.println("<h2>" + String(title) + "</h2><table>");

    fil.print("<tr>");
    for (auto &a : attributes) {
        fil.print("<th>");
        fil.print(a);
        fil.print("</th>");
    }
    fil.println("</tr>");

    Serial.println("[LDAP] Starting paged search loop...");
    ldapUiLogLine("[LDAP] Paged search started...");
    ldapSearchPagedLoop(dcIP, baseDN, filter, attributes, fil);
    ldapUiLogLine("[LDAP] Paged search finished.");

    fil.println("</table></body></html>");
    fil.close();

    Serial.println("[LDAP] File closed.");
    Serial.println("[LDAP] END ldapExtractAndSave()");
    Serial.println("--------------------------------------------");
}

// ============================================================================
// Main Entry Point
// ============================================================================

void runLDAPDomainDump() {
    IPAddress dcIP;

    if (!detectAndBindToDC(dcIP)) {
        ldapUiLogLine("[ERROR] DetectAndBindToDC() failed.");
        waitAndReturnToMenu("LDAP Bind failed");
        return;
    }

    ldapRespBuf = static_cast<uint8_t*>(RuntimeMemory::allocateInternal(
        LDAP_BUF_SIZE, true, 16U * 1024U));
    if (!ldapRespBuf) {
        Serial.println(String("[LDAP] Response buffer unavailable; ") +
                       RuntimeMemory::describe());
        waitAndReturnToMenu("LDAP memory unavailable");
        return;
    }
    struct LdapResponseBufferGuard {
        ~LdapResponseBufferGuard() {
            RuntimeMemory::release(ldapRespBuf);
            ldapRespBuf = nullptr;
        }
    } ldapResponseBufferGuard;

    SD.mkdir("/evil/LDAP");
    String folder = "/evil/LDAP/" + ldapDomainNetbios;
    SD.mkdir(folder);

    ldapUiResetLog(dcIP, ldapDomainDN);
    ldapUiSetPhase("DUMP");

    ldapUiLogLine("[SCAN] DC: " + dcIP.toString());
    ldapUiLogLine("[LDAP] BaseDN: " + ldapDomainDN);
    if (ldapUsername.length() > 0) {
        ldapUiLogLine("[AUTH] User: " + ldapUsername);
    } else {
        ldapUiLogLine("[AUTH] Anonymous bind");
    }
    ldapUiLogLine("[LDAP] Starting full domain dump...");

    Serial.println();
    Serial.println("--------------------------------------------");
    Serial.println("[LDAP] DOMAIN DUMP");
    Serial.println("--------------------------------------------");
    Serial.println("[LDAP] DC       : " + dcIP.toString());
    Serial.println("[LDAP] BaseDN   : " + ldapDomainDN);
    Serial.println("[LDAP] NETBIOS  : " + ldapDomainNetbios);
    Serial.println("[LDAP] Username : " + ldapUsername);

    // DOMAIN USERS
    ldapUiLogLine("[LDAP] Dumping domain users...");
    ldapUiSetPhase("DUMP USERS");

    ldapExtractAndSave(
        dcIP,
        ldapDomainDN,
        "objectCategory=person",
        "domain_users.html",
        "Domain Users",
        {
            "cn",
            "name",
            "sAMAccountName",
            "memberOf",
            "primaryGroupID",
            "whenCreated",
            "whenChanged",
            "lastLogon",
            "userAccountControl",
            "pwdLastSet",
            "description",
            "servicePrincipalName"
        }
    );
    ldapUiLogLine("[LDAP] Done: Domain Users");

    // DOMAIN GROUPS
    ldapUiSetPhase("DUMP Groups");
    ldapUiLogLine("[LDAP] Dumping domain groups...");
    ldapExtractAndSave(
        dcIP,
        ldapDomainDN,
        "objectCategory=group",
        "domain_groups.html",
        "Domain Groups",
        {
            "cn",
            "sAMAccountName",
            "memberOf",
            "member",
            "description",
            "whenCreated",
            "whenChanged"
        }
    );
    ldapUiLogLine("[LDAP] Done: Domain Groups");

    // DOMAIN COMPUTERS
    ldapUiLogLine("[LDAP] Dumping domain computers...");
    ldapUiSetPhase("DUMP Computers");
    ldapExtractAndSave(
        dcIP,
        ldapDomainDN,
        "objectCategory=computer",
        "domain_computers.html",
        "Domain Computer Accounts",
        {
            "cn",
            "sAMAccountName",
            "dNSHostName",
            "operatingSystem",
            "operatingSystemServicePack",
            "operatingSystemVersion",
            "lastLogon",
            "userAccountControl",
            "whenCreated",
            "description"
        }
    );
    ldapUiLogLine("[LDAP] Done: Domain Computers");

    // DOMAIN POLICY
    ldapUiLogLine("[LDAP] Dumping domain policy...");
    ldapUiSetPhase("DUMP Policy");
    ldapExtractAndSave(
        dcIP,
        ldapDomainDN,
        "objectClass=domain",
        "domain_policy.html",
        "Domain Policy",
        {
            "distinguishedName",
            "lockoutObservationWindow",
            "lockoutDuration",
            "lockoutThreshold",
            "maxPwdAge",
            "minPwdAge",
            "minPwdLength",
            "pwdHistoryLength",
            "pwdProperties",
            "ms-DS-MachineAccountQuota"
        }
    );
    ldapUiLogLine("[LDAP] Done: Domain Policy");

    // TRUSTS
    ldapUiLogLine("[LDAP] Dumping domain trusts...");
    ldapUiSetPhase("DUMP Trusts");
    ldapExtractAndSave(
        dcIP,
        ldapDomainDN,
        "objectClass=trustedDomain",
        "domain_trusts.html",
        "Domain Trusts",
        {
            "trustPartner",
            "trustDirection",
            "trustType"
        }
    );
    ldapUiLogLine("[LDAP] Done: Domain Trusts");

    // GPOs
    ldapUiSetPhase("DUMP GPO");
    ldapUiLogLine("[LDAP] Dumping GPOs...");

    ldapExtractAndSave(
        dcIP,
        ldapDomainDN,
        "objectClass=groupPolicyContainer",
        "domain_gpo.html",
        "Group Policy Objects",
        {
            "displayName",
            "name",
            "distinguishedName",
            "gPCFileSysPath",
            "gPCMachineExtensionNames",
            "gPCUserExtensionNames",
            "whenCreated",
            "whenChanged",
            "versionNumber"
        }
    );

    ldapUiLogLine("[LDAP] Done: GPOs");

    // DONE
    ldapUiSetPhase("DONE");
    ldapUiLogLine("[LDAP] Dump completed.");
    ldapUiLogLine(String("[LDAP] Files in /evil/LDAP/") + ldapDomainNetbios + "/");
    ldapUiLogLine(" - domain_users.html");
    ldapUiLogLine(" - domain_groups.html");
    ldapUiLogLine(" - domain_computers.html");
    ldapUiLogLine(" - domain_policy.html");
    ldapUiLogLine(" - domain_trusts.html");
    ldapUiLogLine(" - domain_gpo.html");

    ldapUiShowViewer();

    waitAndReturnToMenu("LDAP Dump done");
}
