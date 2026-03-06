# Phase 4: Полная миграция .ino файла (Evil-Cardputer-v1-5-0.ino)

**Файл:** 28169 строк, **~2167 вызовов** дисплея
- 1965 вызовов `M5.Display.` / `M5.Lcd.`
- 202 вызова через `auto& d = M5Cardputer.Display;` (d.)
- 192 функции с вызовами дисплея

**Спец. случаи:**
- `M5Canvas taskBarCanvas(&M5.Display)` — строка 743
- `static LGFX_Sprite g_spr(&M5.Display)` — строка 22715
- `static M5Canvas ow_listSpr(&M5.Display)` / `ow_panelSpr` — строки 27529-27530
- `createSprite(M5.Display.width(), 12)` — строка 1627
- `auto& d = M5Cardputer.Display;` паттерн — 14 мест (строки 4245-27270)
- `M5.Lcd.` вызовы вместо `M5.Display.` — ~100 мест

---

## Шаг 0: Подготовка (prerequisite)

**Действия:**
1. Добавить `using LB = GUI::LegacyBridge;` после существующего `#include "gui/gui.h"` (уже в файле)
2. Обработать спец. случаи спрайтов:
   - Строка 743: `M5Canvas taskBarCanvas(&M5.Display);` → оставить как есть (Canvas работает с физическим дисплеем напрямую) ИЛИ добавить `LB::getDisplay()` если метод есть
   - Строка 22715: `static LGFX_Sprite g_spr(&M5.Display);` → аналогично
   - Строки 27529-27530: `M5Canvas ow_listSpr/ow_panelSpr` → аналогично
3. Заменить все `auto& d = M5Cardputer.Display;` → `// Using LB (LegacyBridge)` и `d.` → `LB::` (14 мест)
4. Заменить все `M5.Lcd.` → `LB::` (кроме `M5.Lcd.width()`/`M5.Lcd.textWidth()` → `LB::width()`/`LB::textWidth()`)

**Кол-во вызовов:** ~302 (100 M5.Lcd + 202 d.)
**Строки:** разбросаны по файлу

---

## Шаг 1: Setup & Boot (61 вызов)

**Функция:** `setup()` — строки 824-1561
**Действия:**
- Массовая замена `M5.Display.` → `LB::` в пределах функции
- Замена `M5.Lcd.` → `LB::` (строки 833, 1325-1359, 1534-1536)
- Извлечь хелпер для SD Card Error экрана (строки 1161-1175, 14 вызовов):
  ```cpp
  static void showSdCardError() { ... }
  ```
- Извлечь хелпер для Auto-Connect WiFi selector UI (строки 1437-1460):
  ```cpp
  static void drawWifiSelector(const AutoConnectNet* nets[], int count, int selected) { ... }
  ```
- Рефакторить boot-splash текст (строки 1325-1359) в хелпер:
  ```cpp
  static void drawBootSplash(const char* text1, const char* text2, const char* text3) { ... }
  ```

---

## Шаг 2: Menu, TaskBar & Navigation (25 вызовов)

**Функции:**
- `drawMenu()` — строки 1838-1867, 9 вызовов
- `loopOptions()` — строки 5817-5922, 16 вызовов
- `drawTaskBar()` — строки 1632-1728, 0 прямых (использует Canvas)
- `loop()` — строки 1738-1837, 8 вызовов
- `initTaskBarSprite()` — строка 1625, 1 вызов

**Действия:**
- Замена `M5.Display.` → `LB::`
- `taskBarCanvas.createSprite(M5.Display.width(), 12)` → `taskBarCanvas.createSprite(LB::width(), 12)` (если LB::width() есть)
- `loopOptions()` — извлечь хелпер для отрисовки элемента списка:
  ```cpp
  static void drawOptionItem(int y, const String& text, bool selected, bool title) { ... }
  ```

---

## Шаг 3: WiFi Scanner & Network List (97 вызовов)

**Функции:**
- `scanWifiNetworks()` — строки 2563-2595, 5 вызовов
- `showWifiList()` — строки 2596-2804, 30 вызовов
- `showWifiDetails()` — строки 2805-2900, 19 вызовов
- `showWifiPasswordsMenu()` — строки 5923-6051, 31 вызовов
- `wifi_scan()` — строки 10469-10522, 12 вызовов

**Действия:**
- Массовая замена `M5.Display.` / `M5.Lcd.` → `LB::`
- `showWifiList()` — извлечь хелпер для отрисовки строки Wi-Fi сети:
  ```cpp
  static void drawWifiListItem(int y, int index, const String& ssid, int rssi, bool open, bool selected) { ... }
  ```
- `showWifiDetails()` — извлечь хелпер для информационного экрана:
  ```cpp
  static void drawWifiInfoScreen(const char* ssid, int rssi, const char* security, const char* bssid, int channel) { ... }
  ```

---

## Шаг 4: WiFi Connect, SSID/Password/MAC Input (123 вызова)

**Функции:**
- `setWifiSSID()` — строки 9353-9413, 22 вызова
- `setWifiPassword()` — строки 9414-9477, 26 вызовов
- `setMacAddress()` — строки 9478-9542, 26 вызовов
- `connectToWiFi()` — строки 11161-11203, 16 вызовов
- `inputWifiPassword()` — строки 11204-11255, 11 вызовов
- `connectWifi()` — строки 11256-11358, 8 вызовов
- `disconnectWiFi()` — строки 15828-15839, 5 вызовов
- `reconnectWiFi()` — строки 15860-15890, 9 вызовов

**Действия:**
- Массовая замена `M5.Display.` → `LB::`
- Общий паттерн ввода (clear + prompt + cursor + input loop) — извлечь:
  ```cpp
  static void inoPromptScreen(const char* label, uint16_t labelColor = TFT_CYAN) { ... }
  ```
- `setWifiSSID/Password/Mac` имеют идентичную структуру — извлечь:
  ```cpp
  static void drawInputField(const String& input, int cursorPos, int y, uint16_t color) { ... }
  ```

---

## Шаг 5: Captive Portal & Credentials (36 вызовов)

**Функции:**
- `changePortal()` — строки 5002-5085, 11 вызовов
- `displayCredentials()` — строки 5145-5198, 9 вызовов
- `saveCurrentPortalAndSSID()` — строки 6990-7011, 5 вызовов
- `setCaptivePortalIP()` — строки 7012-7069, 11 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`
- Использовать `inoPromptScreen()` из Шага 4

---

## Шаг 6: Popups & Common UI (30 вызовов)

**Функции:**
- `okPopup()` — строки 5199-5225, 10 вызовов
- `confirmPopup()` — строки 5226-5271, 14 вызовов
- `waitAndReturnToMenu()` — строки 5758-5816, 6 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`
- Эти функции УЖЕ являются хелперами — просто заменить вызовы

---

## Шаг 7: Monitor Pages & Probes (85 вызовов)

**Функции:**
- `displayMonitorPage1()` — строки 5325-5417, 20 вызовов
- `displayMonitorPage2()` — строки 5465-5625, 14 вызовов
- `displayMonitorPage3()` — строки 5626-5715, 17 вызовов
- `listProbes()` — строки 7865-7959, 8 вызовов
- `deleteProbe()` — строки 7960-8056, 6 вызовов
- `showProbesAndSelect()` — строки 8057-8118, 6 вызовов
- `displayWaitingForProbe()` — строки 8731-8765, 14 вызовов

**Действия:**
- Замена `M5.Display.` / `M5.Lcd.` → `LB::`
- `displayMonitorPage1/2/3` — извлечь общий хелпер для строки статистики:
  ```cpp
  static void drawMonitorStatLine(int y, const char* label, const String& value, uint16_t color) { ... }
  ```

---

## Шаг 8: Settings & Configuration (98 вызовов)

**Функции:**
- `toggleI2C()` — строки 6052-6080, 4 вызова
- `showI2CDevices()` — строки 6081-6141, 18 вызовов
- `showDisplaySelection()` — строки 6142-6182, 9 вызовов
- `setCPUFrequency()` — строки 6221-6282, 13 вызовов
- `setGPSBaudrate()` — строки 6283-6346, 11 вызовов
- `setStartupSound()` — строки 6474-6575, 13 вызовов
- `setStartupImage()` — строки 6695-6816, 14 вызовов
- `brightness()` — строки 6834-6893, 14 вызовов
- `adjustVolume()` — строки 6894-6952, 12 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`
- Слайдеры `brightness()` и `adjustVolume()` имеют общий паттерн — извлечь:
  ```cpp
  static void drawSliderScreen(const char* title, int value, int maxVal, uint16_t barColor) { ... }
  ```
- Селекторы `setCPUFrequency/setGPSBaudrate/setStartupSound/Image` — извлечь:
  ```cpp
  static void drawSelectorItem(int y, const char* text, bool selected) { ... }
  ```

---

## Шаг 9: Karma, Beacon & Probe Attacks (79 вызовов)

**Функции:**
- `packetSnifferKarma()` — строки 7457-7516, 2 вызова
- `updateDisplayWithSSIDKarma()` — строки 7542-7588, 17 вызовов
- `drawStartButtonKarma()` / `drawStopButtonKarma()` — строки 7589-7604, 10 вызовов
- `startScanKarma()` / `stopScanKarma()` — строки 7605-7688, 10 вызовов
- `drawMenuKarma()` — строки 7735-7759, 9 вызовов
- `executeMenuItemKarma()` — строки 7760-7771, 2 вызова
- `startAPWithSSIDKarma()` — строки 7772-7864, 19 вызовов
- `activateAPForAutoKarma()` — строки 8637-8730, 5 вызовов
- `beaconAttack()` — строки 9304-9352, 5 вызовов

**Действия:**
- Замена `M5.Display.` / `M5.Lcd.` → `LB::`
- `updateDisplayWithSSIDKarma()` — рефакторить отрисовку в секции
- `drawStart/StopButtonKarma()` → объединить в один хелпер:
  ```cpp
  static void drawKarmaButton(bool running) { ... }
  ```

---

## Шаг 10: Deauth & EAPOL Sniffer (121 вызов)

**Функции:**
- `snifferCallback()` — строки 9616-9762, 23 вызова
- `deauthDetect()` — строки 9795-9901, 11 вызовов
- `deauthAttack()` — строки 10075-10214, 19 вызовов
- `snifferCallbackDeauth()` — строки 10221-10292, 8 вызовов
- `broadcastDeauthAttack()` / `sendDeauthToClient()` — строки 10307-10368, 8 вызовов
- `sendBroadcastDeauths()` — строки 10369-10422, 8 вызовов
- `deauthClients()` — строки 10665-10848, 13 вызовов
- `showPcapInfo()` — строки 10974-11003, 11 вызовов
- `displayPcapList()` — строки 11089-11101, 7 вызовов
- `autoDeauther()` — строки 18369-18507, 13 вызовов

**Действия:**
- Замена `M5.Display.` / `M5.Lcd.` → `LB::`
- Общий паттерн: заголовок с каналом + PPS + статистика — извлечь:
  ```cpp
  static void drawSnifferStats(int channel, int pps, int handshakes, int eapol, int deauth) { ... }
  ```

---

## Шаг 11: SSH & Network Tools (107 вызовов)

**Функции:**
- `testConnectivity()` — строки 11359-11398, 10 вызовов
- `sshConnectTask()` — строки 11411-11467, 12 вызовов
- `sshConnect()` — строки 11514-11597, 13 вызовов
- `sshTask()` — строки 11636-11853, 29 вызовов
- `scanIpPort()` — строки 11854-11910, 8 вызовов
- `webCrawling()` — строки 12090-12228, 25 вызовов
- `displayHostOptions()` — строки 12366-12432, 7 вызовов
- `displayHostsAndScanPorts()` — строки 14650-14808, 3 вызова

**Действия:**
- Замена `M5.Display.` → `LB::`
- `sshTask()` — терминальный рендер, сложная логика скролла; просто заменить вызовы
- `webCrawling()` — URL-лист + навигация; может использовать `drawOptionItem()` из Шага 2

---

## Шаг 12: Wardriving & GPS (51 вызов)

**Функции:**
- `setGPSBaudrate()` — уже в Шаге 8
- `toggleGpsPinsMode()` — строки 6969-6989, 4 вызова
- `wardrivingMode()` — строки 8836-8998, 36 вызовов

**Действия:**
- Замена `M5.Display.` / `M5.Lcd.` → `LB::`
- `wardrivingMode()` — GPS данные + сканирование; извлечь:
  ```cpp
  static void drawGpsInfo(double lat, double lng, int sats, double alt, const String& time) { ... }
  ```

---

## Шаг 13: Pwnagotchi Spam & BadUSB (76 вызовов)

**Функции:**
- `displayPwnagotchiDetails()` — строки 9763-9774, 3 вызова
- `displaySpamStatus()` — строки 12689-12749, 19 вызовов
- `key_input()` — строки 12979-13142, 19 вызовов
- `showScriptOptions()` / `runScript()` / `badUSB()` — строки 13178-13309, 6 вызовов
- `runMouseJiggler()` — строки 18508-18612, 24 вызова

**Действия:**
- Замена `M5.Display.` → `LB::`
- `displaySpamStatus()` — извлечь карточку статуса:
  ```cpp
  static void drawSpamCard(int y, const char* face, const char* name, int channel) { ... }
  ```

---

## Шаг 14: Wardriving Master & WiFi Visualizer (76 вызовов)

**Функции:**
- `displayGeneralInfo()` — строки 13386-13467, 17 вызовов
- `displayReceivedData()` — строки 13468-13530, 31 вызов
- `displayStatus()` — строки 13791-13846, 16 вызовов
- `wifiVisualizer()` — строки 13892-14068, 28 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`
- `wifiVisualizer()` — спектральный анализатор, много геометрии — просто заменить
- `displayReceivedData()` / `displayGeneralInfo()` — общие информационные карточки

---

## Шаг 15: All Traffic Sniffer & MITM (54 вызова)

**Функции:**
- `allTrafficSniffer()` — строки 14170-14296, 24 вызова (`M5.Lcd.` паттерн)
- `sniffNetwork()` — строки 14386-14482, 15 вызовов (`M5.Lcd.` паттерн)

**Действия:**
- Замена `M5.Lcd.` → `LB::`
- Общий паттерн сниффера: канал + счётчики + курсор — использовать общие хелперы если подходят

---

## Шаг 16: Network Analysis & Port Scanning (79 вызовов)

**Функции:**
- `displayUrls()` — строки 11911-11931, 7 вызовов
- `FullNetworkAnalysis()` — строки 14522-14601, 11 вызовов
- `displayResults()` — строки 14809-14821, 3 вызова
- `fetchWebsites()` — строки 14835-14889, 3 вызова
- `saveWebsiteContent()` — строки 14914-14971, 12 вызовов
- `displayFileList()` — строки 14972-15028, 8 вызовов
- `viewFileContent()` — строки 15029-15087, 11 вызовов
- `ListNetworkAnalysis()` — строки 15088-15108, 4 вызова
- `reverseTCPTunnel()` — строки 15118-15208, 20 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`

---

## Шаг 17: DHCP Attacks (146 вызовов) ⚡ самый крупный блок

**Функции:**
- `rogueDHCP()` — строки 15388-15474, 1 вызов
- `configureStaticIP()` — строки 15840-15859, 10 вызовов
- `detectDHCPServer()` — строки 15891-15967, 17 вызовов
- `startDHCPStarvation()` — строки 15968-16134, **58 вызовов**
- `sendDHCPDiscover()` — строки 16150-16253, 3 вызова
- `sendDHCPRequest()` — строки 16254-16364, 3 вызова
- `rogueDHCPAuto()` — строки 16462-16520, 1 вызов
- `DHCPAttackAuto()` — строки 16521-16617, **42 вызова**
- `getNetworkBase()` — строки 16641-16674, 11 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`
- `startDHCPStarvation()` / `DHCPAttackAuto()` — очень крупные, извлечь:
  ```cpp
  static void drawDHCPStatus(const char* mode, int leased, int total, int phase) { ... }
  static void drawDHCPProgress(int current, int total, const char* label) { ... }
  ```

---

## Шаг 18: Printer & SNMP (47 вызовов)

**Функции:**
- `detectPrinter()` — строки 16675-16738, 16 вызовов
- `printFile()` — строки 16739-16876, 30 вызовов
- `checkPrinterStatus()` — строки 17151-17315, 1 вызов

**Действия:**
- Замена `M5.Display.` → `LB::`
- `printFile()` — извлечь статусный хелпер

---

## Шаг 19: Honeypot & Responder (15 вызовов)

**Функции:**
- `wpadAbuse()` — строка 4434, 1 вызов
- `startHoneypot()` — строки 17316-17338, 6 вызовов
- `redrawScreenWithLogs()` — строки 17398-17415, 5 вызовов
- `responder()` — строки 19758-19760, 2 вызова

**Действия:**
- Замена `M5.Display.` / `M5.Lcd.` → `LB::`

---

## Шаг 20: NTLM/SMB Animations (через `auto& d` паттерн, 104 вызова)

**Функции:**
- `showWaitingAnimationNTLM()` — строки 4244-4374, 20 d. вызовов
- `updateHashUiNTLM()` — строки 4374-4430, 26 d. вызовов
- `addDetectionPoint()` / `showWaitingAnimation()` — строки 19533-19625, 21 d. вызов
- `showActiveAnimation()` — строки 19625-19680, 10 d. вызовов
- `updateHashUI()` — строки 19680-19722, 27 d. вызовов

**Действия:**
- Заменить `auto& d = M5Cardputer.Display;` → удалить строку
- Заменить `d.` → `LB::` во всех вызовах
- Эти функции содержат сложные анимации (HSV цвета, drawCircle) — просто заменить вызовы

---

## Шаг 21: NTLM Crack & Hash UI (56 вызовов)

**Функции:**
- `drawNTLMInitUser()` — строки 23970-23989, 8 вызовов
- `drawNTLMTries()` — строки 23990-24000, 7 вызовов
- `drawNTLMResult()` — строки 24001-24018, 6 вызовов
- `drawProgressBar()` — строки 24056-24071, 3 вызова
- `drawHashrate()` — строки 24072-24086, 8 вызовов
- `crackNTLMv2()` — строки 24087-24302, 5 вызовов
- `drawCleanerUI()` — строки 24303-24355, 25 вызовов
- `drawResultUI()` — строки 24356-24380, 17 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`

---

## Шаг 22: Evil Twin & Auto Deauther (47 вызовов)

**Функции:**
- `autoDeauther()` — строки 18369-18507, 13 вызовов (M5.Lcd.)
- `runMouseJiggler()` — уже в Шаге 13
- `startEvilTwin()` — строки 18613-18725, 10 вызовов
- `displayAPInfo()` — строки 18290-18303, 10 вызовов (M5.Lcd.)

**Действия:**
- Замена `M5.Display.` / `M5.Lcd.` → `LB::`

---

## Шаг 23: Chat Mesh & Dead Drop (40 вызовов)

**Функции:**
- `drawChatWindow()` — строки 18783-18801, 8 вызовов
- `handleKeyboard()` — строки 18948-18983, 5 вызовов
- `EvilChatMesh()` — строки 18984-19105, 4 вызова
- `ddDrawHeader()` — строки 25448-25455, 5 вызовов
- `ddDrawTextLine()` — строки 25456-25463, 5 вызовов
- `ddDrawTileFrame()` — строки 25464-25472, 5 вызовов
- `ddDrawSparkline()` — строки 25473-25498, 4 вызова
- `ddDashboardInit()` — строки 25499-25532, 9 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`
- Dead Drop функции (`dd*`) уже хорошо структурированы — просто заменить

---

## Шаг 24: SSDP, Skyjack & UPnP (68 вызовов)

**Функции:**
- `selectTypesUI()` — строки 24793-24857, 9 d. вызовов (auto& d паттерн)
- `updateFakeSSDPUI()` — строки 24870-24909, 28 d. вызовов (auto& d паттерн)
- `fakeSSDP()` — строки 24909-25144, 10 вызовов
- `drawSkyjackStatus()` — строки 25145-25164, 6 вызовов
- `skyjackDroneMode()` — строки 25190-25369, 4 вызова
- `upnpAllHostsAllPorts()` — строки 26415-26499, 18 вызовов
- `upnpTargetNATWorkflow()` — строки 26500-26696, 28 вызовов
- `listUPnPMappings()` — строки 26719-26909, 13 вызовов

**Действия:**
- Замена `M5.Display.` / `d.` → `LB::`
- Удалить `auto& d = M5Cardputer.Display;` в selectTypesUI/updateFakeSSDPUI

---

## Шаг 25: CCTV Scanner & MJPEG Viewer (81 вызов)

**Функции:**
- `uiPrintAt()` / `uiDrawHeader()` / `uiDrawContent()` / `uiRefresh()` / `uiAppend()` — строки 21200-21255, 9 вызовов
- `chooseScanModeMenu()` — строки 21283-21337, 15 вызовов
- `promptIPv4()` — строки 21338-21357, 10 вызовов
- `local_scan_CCTV()` — строки 21394-21442, 7 вызовов
- `uiText()` — строки 22191-22196, 3 вызова
- `scanCCTVCamerasFromFile()` — строки 22630-22714, 6 вызовов
- `drawTopBar()` — строки 22862-22874, 8 вызовов (LGFX_Sprite g_spr)
- `drawScaledJpg()` — строки 22875-22928, 2 вызова
- `menuDrawStatic()` / `menuDrawFields()` / `runMenu()` — строки 23022-23109, 5 вызовов
- `mjpegViewerFS()` — строки 23110-23504, 7 вызовов
- `scanCCTV_SpyDectection()` — строки 23794-23876, 13 вызовов
- `scanCCTVCameras()` — строки 23877-23969, 6 вызовов

**Спец. случай:** `LGFX_Sprite g_spr(&M5.Display)` на строке 22715 — спрайт, привязанный к дисплею
**Действия:**
- Замена `M5.Display.` → `LB::`
- Спрайт `g_spr` — оставить привязку к `M5.Display` (спрайты работают с физическим дисплеем напрямую)
- `menuSelectList()` — строки 26087-26147, 10 вызовов — общий UI, заменить

---

## Шаг 26: EAP/IMSI Monitor (61 d. вызов)

**Функции:**
- `eapMon_drawHeader()` — строка 27076
- `eapMon_drawCards()` — строка 27095
- `eapMon_drawStatusBar()` — строка 27122
- `eapMon_drawLastSeen()` — строка 27149
- `eapMon_drawList()` — строка 27179
- `eapMon_drawIdleAnim()` — строка 27215
- `eapMon_updateStatusUi()` — строка 27235
- `eapMon_renderIfNeeded()` — строка 27264
- `imsiCatcher()` — строка 27331

**Действия:**
- Заменить `auto& d = M5Cardputer.Display;` → удалить
- Заменить `d.` → `LB::`
- Эти функции уже хорошо структурированы (eapMon_draw*) — просто заменить

---

## Шаг 27: Open WiFi Dashboard (16 вызовов + спрайты)

**Функции:**
- `ow_printClippedSSID_px()` — строка 27534, 4 вызова
- `ow_printClippedSSID()` — строка 27592, 3 вызова
- `ow_drawFrame()` — строка 27824, 9 вызовов

**Спец. случай:** `M5Canvas ow_listSpr(&M5.Display)` / `ow_panelSpr(&M5.Display)` — строки 27529-27530
**Действия:**
- Замена `M5.Display.` → `LB::`
- Спрайты `ow_listSpr` / `ow_panelSpr` — оставить привязку к `M5.Display`

---

## Шаг 28: Разное (оставшиеся ~50 вызовов)

**Функции с малым числом вызовов** (1-7 каждая):
- `doTheThing()` — строки 2255-2279 (M5.Lcd), 6 вызовов
- `checkSerialCommands()` — строка 2370, 1 вызов
- `showDisplaySelection()` — строки 6142-6182, 9 вызовов
- `restoreConfigParameter()` — строки 7070-7345, 3 вызова
- `probeAttack()` — строки 8296-8391, 14 вызовов
- `displayAPStatus()` — строки 8766-8814, 23 вызова
- `setDeviceMacAddress()` — строки 9558-9615, 6 вызовов
- `print_connections()` — строки 10527-10569, 4 вызова
- `promiscuous_callback()` — строки 10570-10657, 11 вызовов
- `getUserInput()` — строки 11468-11501, 8 вызовов
- `local_scan_setup()` — строки 12263-12324, 7 вызовов
- `afterScanOptions()` — строки 12433-12510, 7 вызовов
- `scanPorts()` — строки 12511-12569, 13 вызовов
- `skimmerDetection()` — строки 12896-12978, 10 вызовов
- `sdToUsb()` — строки 18085-18148, 4 вызова
- `updateDisplay()` — строки 15727-15760, 5 вызовов
- `saveCurrentNetworkConfig()` — строки 15798-15827, 9 вызовов
- `File Manager` — строки 20200-20501, см. Шаг ниже
- `UART Shell` — строки 20608-20969, см. Шаг ниже
- `drawSpycamScreenHC()` — строки 23737-23753, 5 вызовов

**Действия:**
- Массовая замена `M5.Display.` / `M5.Lcd.` → `LB::`

---

## Шаг 29: File Manager & UART Shell (64 вызова)

**Функции:**
- `previewTextFile()` — строки 20200-20329, 9 вызовов
- `fileManager()` — строки 20330-20501, 13 вызовов
- `renderScreen()` — строки 20608-20653, 13 вызовов
- `drawBaudMenu()` — строки 20691-20708, 9 вызовов
- `detectBaud()` — строки 20729-20779, 15 вызовов
- `startUARTShell()` — строки 20780-20968, 5 вызовов

**Действия:**
- Замена `M5.Display.` → `LB::`
- `renderScreen()` — VT100 терминал; просто заменить

---

## Шаг 30: Финальная проверка

**Действия:**
1. `grep -c 'M5\.Display\.\|M5Cardputer\.Display\.\|M5\.Lcd\.' Evil-Cardputer-v1-5-0.ino` — должно быть 0 (кроме спрайтов)
2. Допустимые остатки: `M5Canvas ... (&M5.Display)`, `LGFX_Sprite ... (&M5.Display)` — спрайты привязаны к физическому дисплею
3. Проверить компиляцию
4. Поискать `M5.Display` без точки после (в аргументах конструкторов) — уже учтены в спрайтах

---

## Сводная таблица

| Шаг | Область | Вызовы | Функции | Хелперы |
|-----|---------|--------|---------|---------|
| 0 | Подготовка (LB alias, M5.Lcd, auto& d) | ~302 | — | — |
| 1 | Setup & Boot | 61 | 1 | showSdCardError, drawBootSplash, drawWifiSelector |
| 2 | Menu & Navigation | 25 | 4 | drawOptionItem |
| 3 | WiFi Scanner | 97 | 5 | drawWifiListItem, drawWifiInfoScreen |
| 4 | WiFi Connect/Input | 123 | 8 | inoPromptScreen, drawInputField |
| 5 | Captive Portal | 36 | 4 | (reuse inoPromptScreen) |
| 6 | Popups | 30 | 3 | (already helpers) |
| 7 | Monitor & Probes | 85 | 7 | drawMonitorStatLine |
| 8 | Settings | 98 | 9 | drawSliderScreen, drawSelectorItem |
| 9 | Karma & Beacon | 79 | 9 | drawKarmaButton |
| 10 | Deauth & EAPOL | 121 | 10 | drawSnifferStats |
| 11 | SSH & Network | 107 | 8 | — |
| 12 | Wardriving & GPS | 51 | 2 | drawGpsInfo |
| 13 | Pwnagotchi & BadUSB | 76 | 6 | drawSpamCard |
| 14 | Wardriving Master & Viz | 76 | 4 | — |
| 15 | All Traffic Sniffer | 54 | 2 | — |
| 16 | Network Analysis | 79 | 9 | — |
| 17 | DHCP Attacks | 146 | 9 | drawDHCPStatus, drawDHCPProgress |
| 18 | Printer & SNMP | 47 | 3 | — |
| 19 | Honeypot & Responder | 15 | 4 | — |
| 20 | NTLM Animations (d.) | 104 | 5 | — |
| 21 | NTLM Crack UI | 56 | 8 | — |
| 22 | Evil Twin & AutoDeauth | 47 | 3 | — |
| 23 | Chat & Dead Drop | 40 | 7 | — |
| 24 | SSDP, Skyjack, UPnP | 68 | 8 | — |
| 25 | CCTV & MJPEG | 81 | 12 | — |
| 26 | EAP/IMSI Monitor (d.) | 61 | 9 | — |
| 27 | Open WiFi Dashboard | 16 | 3 | — |
| 28 | Разное (мелкие функции) | ~150 | ~20 | — |
| 29 | File Manager & UART | 64 | 6 | — |
| 30 | Финальная проверка | — | — | — |
| **Итого** | | **~2167** | **192** | **~15 хелперов** |

---

## Порядок выполнения (рекомендация)

Шаги **0 → 1 → 2 → ... → 30** строго последовательно.
Шаг 0 критичен — добавляет `using LB` и обрабатывает все спец. паттерны.
Шаги 1-29 можно делать в любом порядке, но рекомендуется по возрастанию номера строк для минимизации конфликтов при редактировании.

Ожидаемая автоматизация: ~80% замен — механические `M5.Display.`→`LB::` и `M5.Lcd.`→`LB::`. ~20% — извлечение хелперов и рефакторинг.
