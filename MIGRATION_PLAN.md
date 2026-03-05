# План полной миграции с M5.Display на GUI Framework v3.0.0

## Обзор

### Текущее состояние
- **GUI Framework v3.0.0-phase3** полностью реализован, но **не подключён** к прошивке
- `guiInit()` / `guiStart()` **нигде не вызываются**
- `GUI_LEGACY_BRIDGE_MODE = 2` (QUEUED) — мост готов, но не инициализирован
- **2286 прямых вызовов M5.Display** в пользовательском коде (16 файлов, из них ~105 в gui/ — это реализация)
- **36 уникальных методов** M5.Display используются в коде

### Целевое состояние
- Все вызовы M5.Display заменены на `GUI::LegacyBridge::*` или `GUI::Draw::*`
- Рендеринг идёт через асинхронную очередь на Core 0
- Double buffering + DMA + dirty region tracking активны
- Прямые вызовы M5.Display остаются ТОЛЬКО внутри `gui/` (реализация рендерера)

### Стратегия: LegacyBridge как промежуточный слой

LegacyBridge **полностью реализован** и покрывает все 36 методов M5.Display. Миграция — это механическая замена `M5.Display.xxx(...)` → `GUI::LegacyBridge::xxx(...)` с сохранением семантики. Это безопасно: LegacyBridge в режиме QUEUED маршрутизирует вызовы в ту же очередь рендеринга.

---

## Фаза 0: Активация GUI Framework

### 0.1 Инициализация в setup()

В `Evil-Cardputer-v1-5-0.ino`, в функции `setup()`, **после** `M5.begin()` и **перед** первым использованием дисплея:

```cpp
#include "gui/gui.h"

// В setup(), после M5.begin():
GUI::begin();  // вызывает guiInit() + guiStart()
GUI::LegacyBridge::init();
```

### 0.2 Проверка работоспособности

После добавления `GUI::begin()` — скомпилировать и убедиться, что:
- FreeRTOS render task запускается на Core 0
- LegacyBridge инициализируется в режиме QUEUED
- Прошивка работает как раньше (GUI пока ничего не делает)

### 0.3 Потенциальные проблемы
- **Память PSRAM**: double buffer требует ~63KB PSRAM (240×135×2 ×2). Cardputer ADV имеет 8MB PSRAM — не проблема
- **Стек render task**: 4KB (настроено в gui_config.h). Может потребоваться увеличение при сложных операциях
- **Конфликт Wire/SPI**: render task на Core 0 использует SPI. I2C (Wire) тоже на Core 0 по умолчанию — мониторить

---

## Фаза 1: Миграция малых файлов (6 файлов, ~159 вызовов)

Начинаем с файлов с минимальным количеством вызовов для отработки процесса.

### Порядок миграции

| # | Файл | Вызовов | Сложность |
|---|-------|---------|-----------|
| 1 | `menu_engine.cpp` | 6 | Тривиальная |
| 2 | `bluetooth_keyboard.cpp` | 23 | Низкая |
| 3 | `llm_chat.cpp` | 26 | Низкая |
| 4 | `sip_attacks.cpp` | 27 | Низкая (есть scroll) |
| 5 | `terminals.cpp` | 27 | Низкая |
| 6 | `hardware.cpp` | 32 | Средняя (drawJpg) |

### Шаблон миграции для каждого файла

1. Добавить `#include "gui/legacy/gui_legacy_bridge.h"` (или `#include "gui/gui.h"`)
2. Добавить `using LB = GUI::LegacyBridge;` для краткости
3. Заменить все `M5.Display.xxx(...)` → `LB::xxx(...)`
4. Скомпилировать, проверить ошибки
5. Тестировать на устройстве

### Таблица замен API (полная)

| M5.Display метод | LegacyBridge метод | Примечания |
|---|---|---|
| `clear()` | `LB::clear()` | 1:1 |
| `fillScreen(color)` | `LB::fillScreen(color)` | 1:1 |
| `display()` | `LB::display()` | В QUEUED режиме — no-op/flush |
| `setCursor(x, y)` | `LB::setCursor(x, y)` | 1:1 |
| `getCursorY()` | `LB::getCursorY()` | 1:1 |
| `setTextColor(c)` | `LB::setTextColor(c)` | 1:1 |
| `setTextColor(fg, bg)` | `LB::setTextColor(fg, bg)` | 1:1 |
| `setTextSize(s)` | `LB::setTextSize(s)` | 1:1, поддерживает float |
| `setTextFont(f)` | `LB::setTextFont(f)` | 1:1 |
| `setFont(&font)` | **Нет прямого аналога** | См. раздел "Особые случаи" |
| `print(...)` | `LB::print(...)` | Все перегрузки |
| `println(...)` | `LB::println(...)` | Все перегрузки |
| `printf(fmt, ...)` | `LB::printf(fmt, ...)` | 1:1 |
| `textWidth(str)` | `LB::textWidth(str)` | 1:1, делегирует M5.Display |
| `fontHeight()` | `LB::fontHeight()` | 1:1, делегирует M5.Display |
| `drawPixel(x,y,c)` | `LB::drawPixel(x,y,c)` | 1:1 |
| `drawLine(x0,y0,x1,y1,c)` | `LB::drawLine(x0,y0,x1,y1,c)` | 1:1 |
| `drawFastHLine(x,y,w,c)` | `LB::drawFastHLine(x,y,w,c)` | Оптимизирован как drawLine |
| `drawFastVLine(x,y,h,c)` | `LB::drawFastVLine(x,y,h,c)` | Оптимизирован как drawLine |
| `drawRect(x,y,w,h,c)` | `LB::drawRect(x,y,w,h,c)` | 1:1 |
| `fillRect(x,y,w,h,c)` | `LB::fillRect(x,y,w,h,c)` | 1:1 |
| `drawRoundRect(x,y,w,h,r,c)` | `LB::drawRoundRect(x,y,w,h,r,c)` | 1:1 |
| `fillRoundRect(x,y,w,h,r,c)` | `LB::fillRoundRect(x,y,w,h,r,c)` | 1:1 |
| `drawCircle(x,y,r,c)` | `LB::drawCircle(x,y,r,c)` | 1:1 |
| `fillCircle(x,y,r,c)` | `LB::fillCircle(x,y,r,c)` | 1:1 |
| `fillTriangle(...)` | `LB::fillTriangle(...)` | 1:1 |
| `drawJpg(data,len,x,y,w,h)` | `LB::drawJpg(data,len,x,y,w,h)` | 1:1 |
| `setBrightness(b)` | `LB::setBrightness(b)` | 1:1 |
| `getBrightness()` | `LB::getBrightness()` | 1:1, делегирует M5.Display |
| `setRotation(r)` | `LB::setRotation(r)` | 1:1 |
| `getRotation()` | `LB::getRotation()` | 1:1, делегирует M5.Display |
| `setClipRect(x,y,w,h)` | `LB::setClipRect(x,y,w,h)` | 1:1 |
| `clearClipRect()` | `LB::clearClipRect()` | 1:1 |
| `startWrite()` | `LB::startWrite()` | 1:1 |
| `endWrite()` | `LB::endWrite()` | 1:1 |
| `scroll(dx,dy)` | `LB::scroll(dx,dy)` | 1:1 |
| `width()` | `LB::width()` | 1:1 |
| `height()` | `LB::height()` | 1:1 |

---

## Фаза 2: Миграция средних файлов (4 файла, ~320 вызовов)

| # | Файл | Вызовов | Сложность |
|---|-------|---------|-----------|
| 7 | `ldap_dump.cpp` | 46 | Средняя |
| 8 | `llm_gateway.cpp` | 49 | Средняя |
| 9 | `remote_desktop.cpp` | 57 | Высокая (drawJpg, framerate-critical) |
| 10 | `file_editor.cpp` | 68 | Средняя |

### Особое внимание: remote_desktop.cpp

Remote Desktop — самый критичный по производительности модуль:
- `drawJpg()` вызывается в цикле отрисовки кадров JPEG-потока
- Задержки в очереди рендеринга могут привести к видимому лагу
- **Рекомендация**: для RD рассмотреть `GUI_LEGACY_BRIDGE_HYBRID` режим (режим 3), где `drawJpg` идёт напрямую, или оставить drawJpg как прямой вызов M5.Display до полной оптимизации очереди
- Альтернатива: RD может использовать `GUI::Draw` напрямую с `sync()` после каждого кадра

---

## Фаза 3: Миграция ble_attacks.cpp (148 вызовов)

| # | Файл | Вызовов | Сложность |
|---|-------|---------|-----------|
| 11 | `ble_attacks.cpp` | 148 | Высокая (много UI) |

Крупный файл с собственным UI-слоем для BLE-атак. Много `setCursor`/`setTextColor`/`printf` паттернов. Механическая замена.

---

## Фаза 4: Миграция основного .ino файла (1695 вызовов)

Это **ядро миграции** — 74% всех вызовов.

### 4.1 Подготовка

Перед массовой заменой:
1. Убедиться что все предыдущие фазы работают стабильно
2. Создать git ветку для этой фазы отдельно
3. Подготовить скрипт замены (или делать вручную посекционно)

### 4.2 Стратегия: секционная миграция

`.ino` файл содержит десятки UI-функций. Мигрировать по логическим секциям:

| Секция | Примерное кол-во вызовов | Описание |
|--------|--------------------------|----------|
| `drawMenu()` / `drawTaskbar()` | ~100 | Главное меню, таскбар |
| `loopOptions()` | ~50 | Обработчик выбора пунктов |
| WiFi UI функции | ~200 | WiFi сканер, деаут, снифф |
| BLE UI функции | ~100 | BLE экраны |
| IR UI функции | ~80 | IR бластер, клонер |
| BadUSB UI | ~60 | Bad USB экраны |
| Settings UI | ~80 | Настройки |
| Splash / Boot UI | ~30 | Экран загрузки, анимации |
| Остальные модули | ~900+ | Все прочие экраны |

### 4.3 Автоматизация замены

Для .ino файла можно использовать `sed` или скрипт для механической замены:

```bash
# Пример sed-скрипта (применять осторожно, проверять diff)
sed -i 's/M5\.Display\.fillRect/LB::fillRect/g' file.ino
sed -i 's/M5\.Display\.setCursor/LB::setCursor/g' file.ino
sed -i 's/M5\.Display\.setTextColor/LB::setTextColor/g' file.ino
# ... и так для каждого метода
```

**Важно**: после автозамены обязательна ручная проверка diff-а.

### 4.4 Добавление `using LB = GUI::LegacyBridge;` в .ino

В начало файла (после includes):
```cpp
#include "gui/gui.h"
using LB = GUI::LegacyBridge;
```

---

## Особые случаи

### 1. `M5.Display.setFont(&font)` — нет в LegacyBridge

В коде встречается 1 вызов `setFont()`. LegacyBridge имеет `setTextFont(uint8_t)`, но не `setFont(const GFXfont*)`.

**Решение**: Добавить метод в LegacyBridge:
```cpp
static void setFont(const lgfx::GFXfont* font);
// или
static void setFont(const lgfx::IFont* font);
```
Реализация — прокси к `M5.Display.setFont()` + обновление внутреннего состояния.

### 2. `textWidth()` и `fontHeight()` — query-методы

Эти методы **читают** состояние дисплея, а не рисуют. В LegacyBridge они делегируют напрямую к `M5.Display.textWidth()` / `M5.Display.fontHeight()`. Это корректно — query-методы не нужно ставить в очередь.

### 3. Цветовые константы (`TFT_RED`, `TFT_WHITE`, etc.)

315 использований `TFT_*` констант в .ino. Они останутся — LegacyBridge принимает `uint16_t` цвета через перегрузки. Позже можно мигрировать на тематические цвета через `GUI::themeColors()`.

### 4. Глобальные цветовые переменные

286 использований `menuBackgroundColor`, `menuTextFocusedColor` и т.д. Они останутся как есть на этапе миграции. В будущем можно заменить на `LB::getMenuBackground()` и т.д.

### 5. `startWrite()` / `endWrite()` — транзакции DMA

В коде используются для пакетных операций. LegacyBridge поддерживает их. В режиме QUEUED они фактически no-op (очередь сама группирует операции).

### 6. `display()` — flush

LegacyBridge::display() в режиме QUEUED — no-op или вызов endFrame(). Семантика сохраняется.

### 7. Remote Desktop drawJpg — производительность

RD получает JPEG-кадры по сети и немедленно рисует. Проход через очередь добавит до 1 кадра задержки.

**Варианты**:
- A) Использовать LB::drawJpg() + LB::sync() после каждого кадра (безопасно, но блокирующе)
- B) Оставить прямой M5.Display.drawJpg() только для RD
- C) Использовать HYBRID режим мостa (режим 3) для RD модуля

**Рекомендация**: вариант A для начала, оптимизировать после профилирования.

---

## Фаза 5: Очистка и оптимизация

### 5.1 Удаление прямых M5.Display вызовов

После миграции всех файлов:
```bash
# Проверка — должно показать 0 результатов (кроме gui/ директории)
grep -r "M5\.Display\." --include="*.ino" --include="*.cpp" --exclude-dir=gui
```

### 5.2 Переход с LegacyBridge на GUI::Draw (опционально)

После стабилизации на LegacyBridge можно постепенно заменять:
```cpp
// Было (LegacyBridge):
LB::setCursor(x, y);
LB::setTextColor(c);
LB::printf("text %d", val);

// Стало (GUI::Draw):
GUI::Draw::drawText(x, y, "text " + String(val), c, fontSize);
```

Преимущества `GUI::Draw`:
- Нет глобального состояния (цвет/курсор/шрифт передаются в каждый вызов)
- Чище для параллельного кода
- Лучше интеграция с dirty region tracking

### 5.3 Интеграция тематических цветов

Заменить:
```cpp
// Было:
LB::fillRect(0, 0, 240, 135, menuBackgroundColor);
// Стало:
LB::fillRect(0, 0, LB::width(), LB::height(), LB::getMenuBackground());
```

### 5.4 Адаптация под DisplayProfileManager

После миграции на LegacyBridge, рендерер может быть переключён на другой дисплей (ILI9488) через DisplayProfileManager без изменения пользовательского кода — достаточно переконфигурировать M5.Display в рендерере.

---

## Фаза 6: Финальная валидация

### 6.1 Чеклист

- [ ] 0 вызовов M5.Display вне gui/ директории
- [ ] GUI::begin() вызывается в setup()
- [ ] LegacyBridge::init() вызывается после GUI::begin()
- [ ] Render task работает на Core 0
- [ ] Double buffer активен (PSRAM)
- [ ] Все UI-экраны отображаются корректно
- [ ] Remote Desktop работает без заметного лага
- [ ] BLE атаки — UI корректный
- [ ] Настройки — все пункты работают
- [ ] Scroll Unit работает в меню
- [ ] Яркость дисплея регулируется
- [ ] scroll() работает (sip_attacks, skyjack)

### 6.2 Тестирование производительности

- Замерить FPS Remote Desktop до и после миграции
- Замерить время отрисовки меню
- Мониторить заполненность RenderQueue: `GUI::queueFillPercent()`
- При переполнении очереди — увеличить `GUI_QUEUE_SIZE` в gui_config.h (сейчас 256)

---

## Сводка по объёму работ

| Фаза | Файлов | Вызовов | Приоритет |
|-------|--------|---------|-----------|
| 0: Активация GUI | 1 | 0 | Критический |
| 1: Малые файлы | 6 | 159 | Высокий |
| 2: Средние файлы | 4 | 320 | Высокий |
| 3: ble_attacks.cpp | 1 | 148 | Средний |
| 4: .ino файл | 1 | 1695 | Высокий |
| 5: Очистка | — | — | Средний |
| 6: Валидация | — | — | Критический |
| **Итого** | **13** | **~2286** | — |

### Добавление метода setFont в LegacyBridge

Единственное изменение в GUI Framework, необходимое для миграции:

**gui/legacy/gui_legacy_bridge.h** — добавить в public секцию:
```cpp
static void setFont(const lgfx::IFont* font);
```

**gui/legacy/gui_legacy_bridge.cpp** — реализация:
```cpp
void LegacyBridge::setFont(const lgfx::IFont* font) {
    M5.Display.setFont(font);
}
```

---

## Риски

| Риск | Вероятность | Влияние | Митигация |
|------|-------------|---------|-----------|
| Переполнение очереди при быстрых перерисовках | Средняя | Потеря кадров | Увеличить QUEUE_SIZE, добавить sync() |
| Задержка рендеринга в Remote Desktop | Высокая | Видимый лаг | HYBRID режим или прямой drawJpg |
| Конфликт SPI между Core 0 (render) и Core 1 (main) | Низкая | Артефакты | M5Unified уже имеет SPI mutex |
| Нехватка стека render task | Низкая | Crash | Увеличить GUI_RENDER_STACK_SIZE |
| Гонки при setCursor/setTextColor (глобальное состояние) | Средняя | Мерцание | LegacyBridge хранит состояние отдельно от M5.Display |
