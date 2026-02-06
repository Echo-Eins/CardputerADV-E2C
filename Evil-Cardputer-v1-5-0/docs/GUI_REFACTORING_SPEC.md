# Техническое Задание: Рефакторинг Визуального Слоя

## Оглавление

1. [Текущее состояние](#1-текущее-состояние)
2. [Цели рефакторинга](#2-цели-рефакторинга)
3. [Целевая архитектура](#3-целевая-архитектура)
4. [API спецификация](#4-api-спецификация)
5. [План миграции](#5-план-миграции)
6. [Файловая структура](#6-файловая-структура)
7. [Тестирование](#7-тестирование)

---

## 1. Текущее Состояние

### 1.1 Статистика использования Display API

| Файл | Вызовы | Доля |
|------|--------|------|
| Evil-Cardputer-v1-5-0.ino | 1,896 | 80% |
| ble_attacks.cpp | 148 | 6% |
| file_editor.cpp | 68 | 3% |
| remote_desktop.cpp | 57 | 2% |
| llm_gateway.cpp | 49 | 2% |
| ldap_dump.cpp | 46 | 2% |
| terminals.cpp | 27 | 1% |
| Остальные | 76 | 4% |
| **ИТОГО** | **2,367+** | 100% |

### 1.2 Основные проблемы

```
┌─────────────────────────────────────────────────────────────────┐
│                    ТЕКУЩАЯ АРХИТЕКТУРА                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│   │   Menu   │  │   RDP    │  │  Editor  │  │   BLE    │       │
│   └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘       │
│        │             │             │             │              │
│        ▼             ▼             ▼             ▼              │
│   ┌─────────────────────────────────────────────────────────┐  │
│   │              M5.Display (ПРЯМЫЕ ВЫЗОВЫ)                 │  │
│   │  - 2,367+ вызовов разбросаны по всему коду              │  │
│   │  - Блокирующие операции                                  │  │
│   │  - Нет абстракции                                        │  │
│   │  - Жесткая привязка к железу                             │  │
│   └─────────────────────────────────────────────────────────┘  │
│                              │                                  │
│                              ▼                                  │
│   ┌─────────────────────────────────────────────────────────┐  │
│   │           ST7789V SPI (240x135, блокирующий)            │  │
│   └─────────────────────────────────────────────────────────┘  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**Критические проблемы:**

| # | Проблема | Влияние |
|---|----------|---------|
| 1 | **Блокирующий рендеринг** | display() блокирует CPU на 5-15мс |
| 2 | **Нет двойной буферизации** | Мерцание при сложных обновлениях |
| 3 | **Полная перерисовка** | Перерисовка всего экрана даже при мелких изменениях |
| 4 | **Tight coupling** | Невозможно тестировать UI без железа |
| 5 | **Один дисплей** | Нет поддержки внешних дисплеев |
| 6 | **Нет темизации** | Цвета захардкожены в каждом модуле |
| 7 | **Input блокирует render** | Пропуск нажатий при долгом рендере |

### 1.3 Текущее использование Canvas

Только **taskbar** использует Canvas (частичная буферизация):
```cpp
M5Canvas taskBarCanvas(&M5.Display);
taskBarCanvas.createSprite(240, 12);
// ... рендеринг в canvas ...
taskBarCanvas.pushSprite(0, 0);
```

Остальные 99% кода рендерят напрямую в framebuffer.

---

## 2. Цели Рефакторинга

### 2.1 Функциональные требования

| ID | Требование | Приоритет |
|----|------------|-----------|
| F1 | Асинхронный рендеринг (не блокирует main loop) | P0 |
| F2 | Двойная буферизация для всех операций | P0 |
| F3 | Dirty-region tracking (обновление только изменений) | P1 |
| F4 | Поддержка внешнего дисплея (I2C/SPI) | P1 |
| F5 | Разный контент на разных дисплеях | P2 |
| F6 | Централизованная система тем | P1 |
| F7 | Декларативный API для виджетов | P2 |
| F8 | Приоритетная очередь рендеринга | P1 |

### 2.2 Нефункциональные требования

| ID | Требование | Метрика |
|----|------------|---------|
| NF1 | Время блокировки main loop | < 1мс на кадр |
| NF2 | Использование SRAM | < 20KB для GUI |
| NF3 | Использование PSRAM | < 200KB для буферов |
| NF4 | FPS для RDP | >= 15 fps |
| NF5 | Latency input→display | < 50мс |
| NF6 | Совместимость с legacy | 100% старых модулей работают |

### 2.3 Ограничения платформы

```
┌─────────────────────────────────────────────────────────────────┐
│                    ESP32-S3 CARDPUTER                           │
├─────────────────────────────────────────────────────────────────┤
│  CPU: Dual-core 240MHz (Core 0 + Core 1)                        │
│  SRAM: 320KB (для кода и стека)                                 │
│  PSRAM: 8MB (для буферов и данных)                              │
│  Display: ST7789V 240x135 @ 16-bit (65KB framebuffer)           │
│  SPI: 40MHz max                                                  │
│  DMA: Доступен для SPI                                          │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. Целевая Архитектура

### 3.1 Общая схема

```
┌──────────────────────────────────────────────────────────────────────┐
│                         APPLICATION LAYER                             │
├──────────────────────────────────────────────────────────────────────┤
│                                                                       │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│   │  Module  │  │  Module  │  │  Module  │  │  Legacy  │            │
│   │  (RDP)   │  │  (Chat)  │  │ (Editor) │  │  (Menu)  │            │
│   └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘            │
│        │             │             │             │                    │
│        │   ┌─────────▼─────────────▼─────────────▼──────────┐        │
│        │   │              GUI::View API                      │        │
│        │   │  - Декларативные виджеты                        │        │
│        │   │  - Layout constraints                           │        │
│        └───▶  - Event callbacks                              │        │
│            │  - Style hints                                   │        │
│            └─────────────────────┬───────────────────────────┘        │
│                                  │                                    │
│   ┌──────────────────────────────▼───────────────────────────┐       │
│   │              GUI::LegacyBridge (Compatibility)           │       │
│   │  - Перехват M5.Display вызовов                           │       │
│   │  - Трансляция в новый API                                │       │
│   └──────────────────────────────┬───────────────────────────┘       │
│                                  │                                    │
├──────────────────────────────────┼────────────────────────────────────┤
│                         GUI CORE │                                    │
├──────────────────────────────────┼────────────────────────────────────┤
│                                  │                                    │
│   ┌──────────────────────────────▼───────────────────────────┐       │
│   │                   GUI::Compositor                         │       │
│   │  - Scene graph (дерево виджетов)                         │       │
│   │  - Dirty region tracking                                  │       │
│   │  - Z-ordering / layering                                  │       │
│   │  - Hit testing для input                                  │       │
│   └──────────────────────────────┬───────────────────────────┘       │
│                                  │                                    │
│   ┌──────────────────────────────▼───────────────────────────┐       │
│   │                   GUI::RenderQueue                        │       │
│   │  - Ring buffer команд рендеринга                         │       │
│   │  - Priority levels (urgent/normal/deferred)              │       │
│   │  - Batching одинаковых операций                          │       │
│   └──────────────────────────────┬───────────────────────────┘       │
│                                  │                                    │
│   ┌──────────────────────────────▼───────────────────────────┐       │
│   │                   GUI::Renderer (Core 0)                  │       │
│   │  - Async task на отдельном ядре                          │       │
│   │  - Double-buffered framebuffer                            │       │
│   │  - DMA transfer to display                                │       │
│   │  - Partial updates (dirty rects)                          │       │
│   └──────────────────────────────┬───────────────────────────┘       │
│                                  │                                    │
│   ┌──────────────────────────────▼───────────────────────────┐       │
│   │                   GUI::Theme                              │       │
│   │  - Color palette                                          │       │
│   │  - Font styles                                            │       │
│   │  - Widget skins                                           │       │
│   │  - Per-module overrides                                   │       │
│   └──────────────────────────────┬───────────────────────────┘       │
│                                  │                                    │
├──────────────────────────────────┼────────────────────────────────────┤
│                     DRIVER LAYER │                                    │
├──────────────────────────────────┼────────────────────────────────────┤
│                                  │                                    │
│   ┌──────────────────────────────▼───────────────────────────┐       │
│   │                 GUI::DisplayManager                       │       │
│   │  - Display enumeration                                    │       │
│   │  - Hot-plug detection                                     │       │
│   │  - View-to-display routing                                │       │
│   └────────────┬─────────────────────────────┬───────────────┘       │
│                │                             │                        │
│   ┌────────────▼────────────┐   ┌────────────▼────────────┐          │
│   │   InternalDisplay       │   │   ExternalDisplay       │          │
│   │   (ST7789V via SPI)     │   │   (I2C OLED / SPI LCD)  │          │
│   │   240x135, 16-bit RGB   │   │   Variable resolution   │          │
│   │   DMA double-buffer     │   │   Optional              │          │
│   └─────────────────────────┘   └─────────────────────────┘          │
│                                                                       │
└───────────────────────────────────────────────────────────────────────┘
```

### 3.2 Распределение по ядрам CPU

```
┌─────────────────────────────────┐  ┌─────────────────────────────────┐
│           CORE 1 (APP)          │  │          CORE 0 (RENDER)        │
├─────────────────────────────────┤  ├─────────────────────────────────┤
│                                 │  │                                 │
│  ┌───────────────────────────┐  │  │  ┌───────────────────────────┐  │
│  │      Main Loop            │  │  │  │    GUI::RenderTask        │  │
│  │  - Input polling          │  │  │  │  - Dequeue commands       │  │
│  │  - Network handling       │  │  │  │  - Execute render ops     │  │
│  │  - Business logic         │  │  │  │  - DMA to display         │  │
│  │  - GUI command generation │──┼──┼──▶  - Swap buffers           │  │
│  └───────────────────────────┘  │  │  └───────────────────────────┘  │
│                                 │  │                                 │
│  ┌───────────────────────────┐  │  │  ┌───────────────────────────┐  │
│  │      WiFi/BLE Task        │  │  │  │    I2C Display Task       │  │
│  │  (Arduino core)           │  │  │  │  (External display)       │  │
│  └───────────────────────────┘  │  │  └───────────────────────────┘  │
│                                 │  │                                 │
│  Priority: Normal               │  │  Priority: High                 │
│  Stack: 8KB                     │  │  Stack: 4KB                     │
│                                 │  │                                 │
└─────────────────────────────────┘  └─────────────────────────────────┘
                │                                    ▲
                │         RenderQueue                │
                └────────────────────────────────────┘
                      (Lock-free ring buffer)
```

### 3.3 Memory Layout

```
┌──────────────────────────────────────────────────────────────────┐
│ PSRAM (8MB)                                                      │
├──────────────────────────────────────────────────────────────────┤
│ ├── Framebuffer A (65KB) ─────────┐                              │
│ │   240 * 135 * 2 = 64,800 bytes  │ Double                       │
│ ├── Framebuffer B (65KB) ─────────┘ Buffer                       │
│ │                                                                │
│ ├── External Display FB (variable, 0-32KB)                       │
│ │                                                                │
│ ├── JPEG decode buffer (32KB)                                    │
│ │                                                                │
│ └── Render cache / sprites (remaining)                           │
├──────────────────────────────────────────────────────────────────┤
│ SRAM (320KB)                                                     │
├──────────────────────────────────────────────────────────────────┤
│ ├── RenderQueue ring buffer (4KB)                                │
│ │   256 commands * 16 bytes each                                 │
│ │                                                                │
│ ├── Widget tree (8-16KB depending on complexity)                 │
│ │                                                                │
│ ├── Dirty region list (1KB)                                      │
│ │   Max 64 regions * 16 bytes                                    │
│ │                                                                │
│ ├── Theme data (2KB)                                             │
│ │                                                                │
│ └── Application heap (remaining ~290KB)                          │
└──────────────────────────────────────────────────────────────────┘
```

---

## 4. API Спецификация

### 4.1 Основные типы

```cpp
// gui_types.h

#pragma once
#include <cstdint>

namespace GUI {

// Цвет в формате RGB565 (16 бит)
using Color = uint16_t;

// Геометрия
struct Point {
    int16_t x, y;
};

struct Size {
    uint16_t width, height;
};

struct Rect {
    int16_t x, y;
    uint16_t width, height;

    bool contains(Point p) const;
    bool intersects(const Rect& other) const;
    Rect intersection(const Rect& other) const;
};

// Выравнивание
enum class Align : uint8_t {
    Left,
    Center,
    Right,
    Top,
    Middle,
    Bottom
};

// Приоритет рендеринга
enum class RenderPriority : uint8_t {
    Urgent = 0,     // Немедленно (input feedback)
    Normal = 1,     // Стандартный
    Deferred = 2    // Отложенный (фон, анимации)
};

// Идентификатор дисплея
enum class DisplayTarget : uint8_t {
    Internal = 0,
    External = 1,
    All = 255
};

} // namespace GUI
```

### 4.2 Theme API

```cpp
// gui_theme.h

#pragma once
#include "gui_types.h"

namespace GUI {

struct ColorPalette {
    Color background;       // Фон
    Color foreground;       // Текст
    Color primary;          // Акцент (кнопки, выделение)
    Color secondary;        // Вторичный акцент
    Color success;          // Успех (зеленый)
    Color warning;          // Предупреждение (желтый)
    Color error;            // Ошибка (красный)
    Color disabled;         // Неактивный элемент
    Color border;           // Границы
    Color highlight;        // Подсветка при наведении
};

struct FontStyle {
    uint8_t font;           // Индекс шрифта (1-8)
    float size;             // Масштаб (1.0 - 3.0)
    bool bold;              // Жирный (эмуляция через повторный рендер)
};

struct Theme {
    const char* name;
    ColorPalette colors;

    struct {
        FontStyle normal;
        FontStyle header;
        FontStyle mono;
        FontStyle small;
    } fonts;

    struct {
        uint8_t padding;        // Внутренний отступ
        uint8_t margin;         // Внешний отступ
        uint8_t borderRadius;   // Скругление углов
        uint8_t borderWidth;    // Толщина границы
    } spacing;
};

// Предустановленные темы
extern const Theme THEME_DEFAULT;       // Текущая тема (NAVY/GREEN)
extern const Theme THEME_DARK;          // Темная тема
extern const Theme THEME_LIGHT;         // Светлая тема
extern const Theme THEME_HACKER;        // Зеленый на черном
extern const Theme THEME_HIGH_CONTRAST; // Высокий контраст

class ThemeManager {
public:
    static ThemeManager& instance();

    void setTheme(const Theme& theme);
    const Theme& current() const;

    // Получить цвет с учетом состояния
    Color getColor(const char* name, bool disabled = false) const;

    // Кастомизация для модуля
    void pushOverride(const char* module, const ColorPalette& override);
    void popOverride();

private:
    const Theme* m_current;
    // Stack для override'ов
};

} // namespace GUI
```

### 4.3 Render Queue API

```cpp
// gui_render_queue.h

#pragma once
#include "gui_types.h"
#include <atomic>

namespace GUI {

// Типы команд рендеринга
enum class RenderOpType : uint8_t {
    Nop = 0,
    FillRect,
    DrawRect,
    DrawLine,
    DrawText,
    DrawImage,
    DrawJpeg,
    SetClip,
    ClearClip,
    PushBuffer,     // Скопировать буфер в framebuffer

    // Композитные
    BeginFrame,
    EndFrame,

    // Системные
    SetBrightness,
    Sync            // Ждать завершения
};

// Команда рендеринга (16 байт max для выравнивания)
struct RenderOp {
    RenderOpType type;
    RenderPriority priority;
    DisplayTarget target;
    uint8_t reserved;

    union {
        struct { Rect rect; Color color; } fill;
        struct { Rect rect; Color color; uint8_t width; } draw;
        struct { Point p1, p2; Color color; } line;
        struct { Point pos; Color fg, bg; uint8_t font; float size;
                 const char* text; } text;
        struct { Rect rect; const uint8_t* data; uint32_t len; } image;
        struct { Rect clip; } clip;
        struct { uint8_t level; } brightness;
    };
};

class RenderQueue {
public:
    static constexpr size_t QUEUE_SIZE = 256;

    static RenderQueue& instance();

    // Добавить команду (неблокирующий, возвращает false если очередь полна)
    bool push(const RenderOp& op);

    // Забрать команду (блокирующий для render task)
    bool pop(RenderOp& op, uint32_t timeoutMs = portMAX_DELAY);

    // Статистика
    size_t pending() const;
    bool full() const;
    bool empty() const;

    // Очистить очередь (при смене экрана)
    void clear();

    // Синхронизация
    void sync();  // Ждать обработки всех команд

private:
    RenderOp m_buffer[QUEUE_SIZE];
    std::atomic<size_t> m_head{0};
    std::atomic<size_t> m_tail{0};
    SemaphoreHandle_t m_semaphore;
};

} // namespace GUI
```

### 4.4 Widget Base API

```cpp
// gui_widget.h

#pragma once
#include "gui_types.h"
#include "gui_theme.h"
#include <vector>
#include <functional>

namespace GUI {

// Forward declarations
class Widget;
class Container;
class View;

// События ввода
struct InputEvent {
    enum Type { KeyPress, KeyRelease, KeyRepeat, Touch } type;
    union {
        struct { uint8_t code; char character; } key;
        struct { int16_t x, y; } touch;
    };
};

// Базовый виджет
class Widget {
public:
    virtual ~Widget() = default;

    // Геометрия
    void setPosition(int16_t x, int16_t y);
    void setSize(uint16_t w, uint16_t h);
    void setBounds(const Rect& bounds);
    Rect bounds() const { return m_bounds; }

    // Видимость и состояние
    void setVisible(bool visible);
    void setEnabled(bool enabled);
    bool isVisible() const { return m_visible; }
    bool isEnabled() const { return m_enabled; }

    // Dirty tracking
    void markDirty();
    void markDirty(const Rect& region);
    bool isDirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }

    // Отрисовка (вызывается Compositor'ом)
    virtual void render(RenderQueue& queue) = 0;

    // Обработка ввода
    virtual bool handleInput(const InputEvent& event) { return false; }

    // Иерархия
    Widget* parent() const { return m_parent; }

protected:
    Rect m_bounds{0, 0, 0, 0};
    bool m_visible = true;
    bool m_enabled = true;
    bool m_dirty = true;
    Widget* m_parent = nullptr;

    friend class Container;
};

// Контейнер виджетов
class Container : public Widget {
public:
    void addChild(Widget* child);
    void removeChild(Widget* child);
    void clearChildren();

    const std::vector<Widget*>& children() const { return m_children; }

    void render(RenderQueue& queue) override;
    bool handleInput(const InputEvent& event) override;

protected:
    std::vector<Widget*> m_children;
};

} // namespace GUI
```

### 4.5 Встроенные виджеты

```cpp
// gui_widgets.h

#pragma once
#include "gui_widget.h"

namespace GUI {

// Текстовая метка
class Label : public Widget {
public:
    explicit Label(const char* text = "");

    void setText(const char* text);
    void setAlign(Align h, Align v = Align::Middle);
    void setTextColor(Color color);
    void setFont(const FontStyle& font);

    void render(RenderQueue& queue) override;

private:
    String m_text;
    Align m_hAlign = Align::Left;
    Align m_vAlign = Align::Middle;
    Color m_textColor = 0xFFFF;
    FontStyle m_font;
};

// Кнопка
class Button : public Widget {
public:
    using Callback = std::function<void()>;

    explicit Button(const char* label, Callback onClick = nullptr);

    void setLabel(const char* label);
    void setOnClick(Callback cb);
    void setStyle(const char* styleName);  // "primary", "danger", "ghost"

    void render(RenderQueue& queue) override;
    bool handleInput(const InputEvent& event) override;

private:
    String m_label;
    Callback m_onClick;
    bool m_pressed = false;
};

// Текстовое поле ввода
class TextInput : public Widget {
public:
    using OnChange = std::function<void(const String&)>;
    using OnSubmit = std::function<void(const String&)>;

    explicit TextInput(const char* placeholder = "");

    void setPlaceholder(const char* text);
    void setValue(const char* value);
    void setPassword(bool isPassword);
    void setMaxLength(size_t max);
    void setOnChange(OnChange cb);
    void setOnSubmit(OnSubmit cb);

    const String& value() const { return m_value; }

    void render(RenderQueue& queue) override;
    bool handleInput(const InputEvent& event) override;

private:
    String m_placeholder;
    String m_value;
    bool m_password = false;
    size_t m_maxLength = 256;
    size_t m_cursorPos = 0;
    OnChange m_onChange;
    OnSubmit m_onSubmit;
};

// Прокручиваемый список
class ScrollList : public Container {
public:
    void setItems(const std::vector<String>& items);
    void setSelectedIndex(int index);
    int selectedIndex() const { return m_selectedIndex; }

    using OnSelect = std::function<void(int index, const String& item)>;
    void setOnSelect(OnSelect cb);

    void render(RenderQueue& queue) override;
    bool handleInput(const InputEvent& event) override;

private:
    std::vector<String> m_items;
    int m_selectedIndex = 0;
    int m_scrollOffset = 0;
    OnSelect m_onSelect;
};

// Прогресс-бар
class ProgressBar : public Widget {
public:
    void setValue(float value);  // 0.0 - 1.0
    void setShowPercent(bool show);
    void setColor(Color fill, Color background);

    void render(RenderQueue& queue) override;

private:
    float m_value = 0.0f;
    bool m_showPercent = true;
    Color m_fillColor;
    Color m_bgColor;
};

// Изображение (JPEG/raw bitmap)
class Image : public Widget {
public:
    void setJpeg(const uint8_t* data, size_t len);
    void setRaw(const uint8_t* data, uint16_t w, uint16_t h);
    void setScaleMode(const char* mode);  // "fit", "fill", "none"

    void render(RenderQueue& queue) override;

private:
    const uint8_t* m_data = nullptr;
    size_t m_len = 0;
    bool m_isJpeg = false;
};

} // namespace GUI
```

### 4.6 Layout Containers

```cpp
// gui_layout.h

#pragma once
#include "gui_widget.h"

namespace GUI {

// Вертикальная укладка
class Column : public Container {
public:
    void setSpacing(uint8_t spacing);
    void setAlign(Align align);  // Left, Center, Right
    void setPadding(uint8_t padding);

    void layout();  // Пересчитать позиции детей
    void render(RenderQueue& queue) override;

private:
    uint8_t m_spacing = 2;
    uint8_t m_padding = 0;
    Align m_align = Align::Left;
};

// Горизонтальная укладка
class Row : public Container {
public:
    void setSpacing(uint8_t spacing);
    void setAlign(Align align);  // Top, Middle, Bottom
    void setPadding(uint8_t padding);

    void layout();
    void render(RenderQueue& queue) override;

private:
    uint8_t m_spacing = 2;
    uint8_t m_padding = 0;
    Align m_align = Align::Middle;
};

// Слои (z-ordering)
class Stack : public Container {
public:
    // Дети отрисовываются друг поверх друга
    void render(RenderQueue& queue) override;
};

// Прокручиваемая область
class ScrollArea : public Container {
public:
    void setScrollOffset(int16_t x, int16_t y);
    void scrollBy(int16_t dx, int16_t dy);
    Point scrollOffset() const;

    void render(RenderQueue& queue) override;
    bool handleInput(const InputEvent& event) override;

private:
    Point m_scroll{0, 0};
};

} // namespace GUI
```

### 4.7 Legacy Bridge (Совместимость)

```cpp
// gui_legacy.h

#pragma once
#include "gui_render_queue.h"

namespace GUI {

// Класс-обертка, имитирующий M5.Display API
// Позволяет старому коду работать без изменений
class LegacyDisplay {
public:
    static LegacyDisplay& instance();

    // === M5.Display compatible API ===

    void clear();
    void fillScreen(uint16_t color);
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color);

    void setCursor(int32_t x, int32_t y);
    void setTextColor(uint16_t fg);
    void setTextColor(uint16_t fg, uint16_t bg);
    void setTextSize(float size);
    void setTextFont(uint8_t font);

    void print(const char* str);
    void print(const String& str);
    void print(int value);
    void print(float value, int decimals = 2);
    void println(const char* str = "");
    void println(const String& str);
    void println(int value);
    size_t printf(const char* format, ...);

    void display();  // Flush (теперь non-blocking)

    int32_t width() const { return 240; }
    int32_t height() const { return 135; }
    int32_t getCursorX() const { return m_cursorX; }
    int32_t getCursorY() const { return m_cursorY; }
    int32_t textWidth(const char* str) const;
    int32_t fontHeight() const;

    void setBrightness(uint8_t level);
    uint8_t getBrightness() const;

    // Для JPEG рендеринга (RDP)
    void drawJpg(const uint8_t* data, size_t len, int32_t x, int32_t y,
                 int32_t maxW = 0, int32_t maxH = 0);

private:
    int32_t m_cursorX = 0;
    int32_t m_cursorY = 0;
    uint16_t m_textColor = 0xFFFF;
    uint16_t m_bgColor = 0x0000;
    float m_textSize = 1.0f;
    uint8_t m_font = 1;
    uint8_t m_brightness = 128;

    RenderQueue& m_queue;

    LegacyDisplay();
};

} // namespace GUI

// Макрос для безболезненной миграции
// Можно включить/выключить в конфиге
#ifdef GUI_LEGACY_COMPAT
    #define M5_Display GUI::LegacyDisplay::instance()
    // Перехватывает M5.Display вызовы
#endif
```

### 4.8 Display Manager

```cpp
// gui_display.h

#pragma once
#include "gui_types.h"
#include "gui_widget.h"

namespace GUI {

// Абстракция дисплея
class Display {
public:
    virtual ~Display() = default;

    virtual Size size() const = 0;
    virtual uint8_t bitsPerPixel() const = 0;
    virtual bool isConnected() const = 0;

    // Framebuffer операции
    virtual uint8_t* getBackBuffer() = 0;
    virtual void swapBuffers() = 0;
    virtual void flush(const Rect& region) = 0;

    // Brightness
    virtual void setBrightness(uint8_t level) = 0;
    virtual uint8_t getBrightness() const = 0;
};

// Встроенный дисплей ST7789V
class InternalDisplay : public Display {
public:
    InternalDisplay();

    Size size() const override { return {240, 135}; }
    uint8_t bitsPerPixel() const override { return 16; }
    bool isConnected() const override { return true; }

    uint8_t* getBackBuffer() override;
    void swapBuffers() override;
    void flush(const Rect& region) override;

    void setBrightness(uint8_t level) override;
    uint8_t getBrightness() const override;

private:
    uint8_t* m_buffers[2];  // Double buffer in PSRAM
    uint8_t m_currentBuffer = 0;
    uint8_t m_brightness = 128;
};

// I2C OLED дисплей (SSD1306, SH1106)
class I2CDisplay : public Display {
public:
    I2CDisplay(uint8_t address, uint16_t width, uint16_t height);

    Size size() const override;
    uint8_t bitsPerPixel() const override { return 1; }  // Monochrome
    bool isConnected() const override;

    uint8_t* getBackBuffer() override;
    void swapBuffers() override;
    void flush(const Rect& region) override;

    void setBrightness(uint8_t level) override;
    uint8_t getBrightness() const override;

private:
    uint8_t m_address;
    Size m_size;
    uint8_t* m_buffer;
};

// Менеджер дисплеев
class DisplayManager {
public:
    static DisplayManager& instance();

    // Инициализация
    void begin();

    // Доступ к дисплеям
    Display* internal() { return &m_internal; }
    Display* external() { return m_external; }

    // Сканирование внешних дисплеев
    void scanExternal();
    bool hasExternal() const { return m_external != nullptr; }

    // Routing
    void setRootView(View* view, DisplayTarget target);

private:
    InternalDisplay m_internal;
    Display* m_external = nullptr;

    View* m_internalView = nullptr;
    View* m_externalView = nullptr;
};

} // namespace GUI
```

---

## 5. План Миграции

### 5.1 Фазы реализации

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          ПЛАН МИГРАЦИИ                                    │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ФАЗА 0: Подготовка                                                      │
│  ├── Создание структуры файлов GUI                                       │
│  ├── Базовые типы и константы                                            │
│  └── Unit-тесты инфраструктура                                           │
│                                                                          │
│  ФАЗА 1: Core Infrastructure                                             │
│  ├── RenderQueue (lock-free ring buffer)                                 │
│  ├── Renderer task (Core 0)                                              │
│  ├── Double-buffering для internal display                               │
│  └── Dirty region tracking                                               │
│                                                                          │
│  ФАЗА 2: Legacy Bridge                                                   │
│  ├── LegacyDisplay class (M5.Display API)                                │
│  ├── Макросы совместимости                                               │
│  ├── Тестирование со старым кодом                                        │
│  └── Постепенный rollout (feature flag)                                  │
│                                                                          │
│  ФАЗА 3: Theme System                                                    │
│  ├── ColorPalette и FontStyle                                            │
│  ├── Предустановленные темы                                              │
│  ├── Загрузка темы из SD                                                 │
│  └── Миграция глобальных цветов                                          │
│                                                                          │
│  ФАЗА 4: Widget System                                                   │
│  ├── Widget base class                                                   │
│  ├── Container и layout                                                  │
│  ├── Базовые виджеты (Label, Button, TextInput)                          │
│  └── ScrollList и ProgressBar                                            │
│                                                                          │
│  ФАЗА 5: Миграция модулей                                                │
│  ├── Новые модули на GUI::View (terminals, llm_chat)                     │
│  ├── Критические модули (remote_desktop, file_editor)                    │
│  ├── Меню и навигация                                                    │
│  └── Остальные модули                                                    │
│                                                                          │
│  ФАЗА 6: External Display                                                │
│  ├── I2C display driver                                                  │
│  ├── Display enumeration                                                 │
│  ├── View routing                                                        │
│  └── Разный контент на разных дисплеях                                   │
│                                                                          │
│  ФАЗА 7: Cleanup                                                         │
│  ├── Удаление legacy bridge                                              │
│  ├── Оптимизация памяти                                                  │
│  └── Документация                                                        │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### 5.2 Детальный план по фазам

#### Фаза 0: Подготовка (1-2 дня)

| Задача | Описание | Файлы |
|--------|----------|-------|
| 0.1 | Создать директорию `gui/` | `gui/` |
| 0.2 | Базовые типы | `gui/gui_types.h` |
| 0.3 | CMake/PlatformIO конфигурация | `platformio.ini` |
| 0.4 | Тестовый фреймворк | `test/test_gui.cpp` |

#### Фаза 1: Core Infrastructure (3-5 дней)

| Задача | Описание | Файлы | Приоритет |
|--------|----------|-------|-----------|
| 1.1 | RenderQueue (lock-free) | `gui/gui_render_queue.h/cpp` | P0 |
| 1.2 | RenderOp types | `gui/gui_render_ops.h` | P0 |
| 1.3 | Renderer task | `gui/gui_renderer.h/cpp` | P0 |
| 1.4 | PSRAM framebuffer alloc | `gui/gui_framebuffer.h/cpp` | P0 |
| 1.5 | DMA transfer setup | `gui/gui_dma.cpp` | P0 |
| 1.6 | Dirty region tracker | `gui/gui_dirty.h/cpp` | P1 |
| 1.7 | Sync primitives | интеграция с FreeRTOS | P0 |

**Критерии готовности:**
- [ ] fillRect через queue работает без блокировки
- [ ] Double-buffering без мерцания
- [ ] CPU usage Core 0 < 50% при 30 fps

#### Фаза 2: Legacy Bridge (2-3 дня)

| Задача | Описание | Файлы |
|--------|----------|-------|
| 2.1 | LegacyDisplay class | `gui/gui_legacy.h/cpp` |
| 2.2 | print/println | `gui/gui_legacy.cpp` |
| 2.3 | fillRect/drawRect/drawLine | `gui/gui_legacy.cpp` |
| 2.4 | cursor/font management | `gui/gui_legacy.cpp` |
| 2.5 | drawJpg support | `gui/gui_legacy.cpp` |
| 2.6 | Feature flag | `config.h` |
| 2.7 | Regression testing | все существующие модули |

**Критерии готовности:**
- [ ] Все модули работают через bridge без изменений кода
- [ ] Производительность не хуже оригинала
- [ ] RDP работает плавно

#### Фаза 3: Theme System (1-2 дня)

| Задача | Описание | Файлы |
|--------|----------|-------|
| 3.1 | Theme struct | `gui/gui_theme.h` |
| 3.2 | Preset themes | `gui/gui_theme.cpp` |
| 3.3 | ThemeManager singleton | `gui/gui_theme.cpp` |
| 3.4 | SD card theme loading | `gui/gui_theme_loader.cpp` |
| 3.5 | Migrate existing colors | `Evil-Cardputer-v1-5-0.ino` |

#### Фаза 4: Widget System (5-7 дней)

| Задача | Описание | Файлы |
|--------|----------|-------|
| 4.1 | Widget base | `gui/gui_widget.h/cpp` |
| 4.2 | Container class | `gui/gui_widget.cpp` |
| 4.3 | Column/Row layouts | `gui/gui_layout.h/cpp` |
| 4.4 | Label widget | `gui/widgets/gui_label.cpp` |
| 4.5 | Button widget | `gui/widgets/gui_button.cpp` |
| 4.6 | TextInput widget | `gui/widgets/gui_textinput.cpp` |
| 4.7 | ScrollList widget | `gui/widgets/gui_scrolllist.cpp` |
| 4.8 | ProgressBar widget | `gui/widgets/gui_progress.cpp` |
| 4.9 | Image widget | `gui/widgets/gui_image.cpp` |
| 4.10 | Input routing | `gui/gui_input.h/cpp` |

#### Фаза 5: Миграция модулей (7-14 дней)

**Порядок миграции (по сложности и зависимостям):**

| # | Модуль | Вызовов | Сложность | Приоритет |
|---|--------|---------|-----------|-----------|
| 1 | terminals.cpp | 27 | Низкая | P1 |
| 2 | llm_chat.cpp | 26 | Низкая | P1 |
| 3 | bluetooth_keyboard.cpp | 23 | Низкая | P2 |
| 4 | sip_attacks.cpp | 27 | Низкая | P2 |
| 5 | ldap_dump.cpp | 46 | Средняя | P2 |
| 6 | llm_gateway.cpp | 49 | Средняя | P1 |
| 7 | remote_desktop.cpp | 57 | Высокая | P0 |
| 8 | file_editor.cpp | 68 | Высокая | P1 |
| 9 | ble_attacks.cpp | 148 | Высокая | P2 |
| 10 | Evil-Cardputer-v1-5-0.ino | 1,896 | Очень высокая | P1 |

**Стратегия миграции модуля:**

```cpp
// БЫЛО (legacy):
void myModule() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setCursor(5, 5);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.println("Hello");
}

// СТАЛО (new API):
class MyModuleView : public GUI::View {
public:
    void build() override {
        root = new GUI::Column({
            new GUI::Label("Hello", GUI::Style::Header)
        });
    }
};

void myModule() {
    MyModuleView view;
    GUI::display.show(&view);

    while (!exitCondition) {
        GUI::input.poll();
        // Business logic
        delay(10);
    }
}
```

#### Фаза 6: External Display (3-5 дней)

| Задача | Описание | Файлы |
|--------|----------|-------|
| 6.1 | Display interface | `gui/gui_display.h` |
| 6.2 | I2C OLED driver | `gui/drivers/gui_ssd1306.cpp` |
| 6.3 | I2C scan | `gui/gui_display_scan.cpp` |
| 6.4 | View routing | `gui/gui_display_manager.cpp` |
| 6.5 | Dual display demo | `examples/dual_display.cpp` |

#### Фаза 7: Cleanup (2-3 дня)

| Задача | Описание |
|--------|----------|
| 7.1 | Удаление legacy bridge (опционально) |
| 7.2 | Memory optimization |
| 7.3 | API documentation |
| 7.4 | Migration guide |
| 7.5 | Performance benchmarks |

---

## 6. Файловая Структура

```
Evil-Cardputer-v1-5-0/
├── Evil-Cardputer-v1-5-0.ino    # Main sketch
├── config.h                     # Feature flags (GUI_LEGACY_COMPAT, etc.)
│
├── gui/                         # NEW: GUI Framework
│   ├── gui.h                    # Main include (includes all)
│   ├── gui_types.h              # Basic types (Color, Rect, Point, etc.)
│   ├── gui_config.h             # Compile-time config
│   │
│   ├── core/
│   │   ├── gui_render_queue.h   # Lock-free command queue
│   │   ├── gui_render_queue.cpp
│   │   ├── gui_renderer.h       # Render task (Core 0)
│   │   ├── gui_renderer.cpp
│   │   ├── gui_compositor.h     # Scene graph & dirty tracking
│   │   ├── gui_compositor.cpp
│   │   ├── gui_framebuffer.h    # PSRAM buffer management
│   │   └── gui_framebuffer.cpp
│   │
│   ├── theme/
│   │   ├── gui_theme.h          # Theme structs
│   │   ├── gui_theme.cpp        # ThemeManager
│   │   ├── gui_theme_default.cpp # Built-in themes
│   │   └── gui_theme_loader.cpp  # SD card loading
│   │
│   ├── widgets/
│   │   ├── gui_widget.h         # Base Widget class
│   │   ├── gui_widget.cpp
│   │   ├── gui_container.h      # Container base
│   │   ├── gui_container.cpp
│   │   ├── gui_label.cpp
│   │   ├── gui_button.cpp
│   │   ├── gui_textinput.cpp
│   │   ├── gui_scrolllist.cpp
│   │   ├── gui_progress.cpp
│   │   └── gui_image.cpp
│   │
│   ├── layout/
│   │   ├── gui_layout.h
│   │   ├── gui_column.cpp
│   │   ├── gui_row.cpp
│   │   ├── gui_stack.cpp
│   │   └── gui_scroll.cpp
│   │
│   ├── display/
│   │   ├── gui_display.h        # Display interface
│   │   ├── gui_display_internal.cpp  # ST7789V driver
│   │   ├── gui_display_i2c.cpp  # I2C OLED driver
│   │   └── gui_display_manager.cpp
│   │
│   ├── input/
│   │   ├── gui_input.h          # Input events
│   │   └── gui_input.cpp        # Keyboard polling
│   │
│   └── legacy/
│       ├── gui_legacy.h         # M5.Display compatibility
│       └── gui_legacy.cpp
│
├── remote_desktop.cpp           # Migrated to GUI::View
├── file_editor.cpp              # Migrated to GUI::View
├── terminals.cpp                # Migrated to GUI::View
├── llm_gateway.cpp              # Migrated to GUI::View
├── ... (other modules)
│
├── docs/
│   ├── GUI_ARCHITECTURE.md      # High-level design
│   ├── GUI_REFACTORING_SPEC.md  # This document
│   ├── GUI_API_REFERENCE.md     # API docs
│   └── GUI_MIGRATION_GUIDE.md   # How to migrate modules
│
└── test/
    ├── test_gui_queue.cpp
    ├── test_gui_widgets.cpp
    └── test_gui_theme.cpp
```

---

## 7. Тестирование

### 7.1 Unit Tests

```cpp
// test/test_gui_queue.cpp

#include <unity.h>
#include "gui/core/gui_render_queue.h"

void test_queue_push_pop() {
    GUI::RenderQueue queue;

    GUI::RenderOp op;
    op.type = GUI::RenderOpType::FillRect;
    op.fill.rect = {0, 0, 100, 100};
    op.fill.color = 0xFFFF;

    TEST_ASSERT_TRUE(queue.push(op));
    TEST_ASSERT_EQUAL(1, queue.pending());

    GUI::RenderOp result;
    TEST_ASSERT_TRUE(queue.pop(result, 0));
    TEST_ASSERT_EQUAL(GUI::RenderOpType::FillRect, result.type);
}

void test_queue_full() {
    GUI::RenderQueue queue;

    // Fill queue
    for (int i = 0; i < GUI::RenderQueue::QUEUE_SIZE; i++) {
        GUI::RenderOp op;
        op.type = GUI::RenderOpType::Nop;
        TEST_ASSERT_TRUE(queue.push(op));
    }

    // Should be full
    TEST_ASSERT_TRUE(queue.full());

    // Push should fail
    GUI::RenderOp op;
    TEST_ASSERT_FALSE(queue.push(op));
}

void test_queue_concurrent() {
    // Test lock-free behavior with two tasks
    // ...
}
```

### 7.2 Integration Tests

```cpp
// test/test_gui_legacy.cpp

#include <unity.h>
#include "gui/legacy/gui_legacy.h"

void test_legacy_print() {
    auto& display = GUI::LegacyDisplay::instance();

    display.clear();
    display.setCursor(10, 20);
    display.setTextColor(0xFFFF);
    display.print("Hello");
    display.display();

    // Verify cursor moved
    TEST_ASSERT_EQUAL(10 + 5 * 6, display.getCursorX());  // 5 chars * 6px
}

void test_legacy_fillRect() {
    auto& display = GUI::LegacyDisplay::instance();

    display.fillRect(0, 0, 50, 50, 0xF800);  // Red
    display.display();

    // Visual inspection or framebuffer comparison
}
```

### 7.3 Performance Benchmarks

```cpp
// test/bench_gui.cpp

void bench_render_throughput() {
    auto& queue = GUI::RenderQueue::instance();

    unsigned long start = micros();
    const int ITERATIONS = 1000;

    for (int i = 0; i < ITERATIONS; i++) {
        GUI::RenderOp op;
        op.type = GUI::RenderOpType::FillRect;
        op.fill.rect = {0, 0, 240, 135};
        op.fill.color = i & 0xFFFF;
        queue.push(op);
    }

    queue.sync();
    unsigned long elapsed = micros() - start;

    Serial.printf("Throughput: %.2f ops/sec\n",
                  ITERATIONS * 1000000.0 / elapsed);
    Serial.printf("Latency: %.2f us/op\n",
                  (float)elapsed / ITERATIONS);
}

void bench_legacy_vs_new() {
    // Compare old M5.Display vs new GUI system

    // Old way
    unsigned long t1 = micros();
    for (int i = 0; i < 100; i++) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(0, 0);
        M5.Display.println("Test");
        M5.Display.display();
    }
    unsigned long old_time = micros() - t1;

    // New way
    unsigned long t2 = micros();
    auto& display = GUI::LegacyDisplay::instance();
    for (int i = 0; i < 100; i++) {
        display.fillScreen(TFT_BLACK);
        display.setCursor(0, 0);
        display.println("Test");
        display.display();
    }
    GUI::RenderQueue::instance().sync();
    unsigned long new_time = micros() - t2;

    Serial.printf("Old: %lu us, New: %lu us, Speedup: %.2fx\n",
                  old_time, new_time, (float)old_time / new_time);
}
```

### 7.4 Visual Regression Tests

```cpp
// test/test_visual.cpp

// Capture framebuffer and compare with golden image
void test_visual_menu() {
    // Render menu
    MainMenuView view;
    GUI::display.show(&view);
    GUI::RenderQueue::instance().sync();

    // Get framebuffer
    uint8_t* fb = GUI::DisplayManager::instance().internal()->getBackBuffer();

    // Compare with golden image from SD card
    File golden = SD.open("/test/golden_menu.raw");
    uint8_t* expected = (uint8_t*)ps_malloc(65 * 1024);
    golden.read(expected, 65 * 1024);
    golden.close();

    // Compare (with tolerance for anti-aliasing)
    int diff = 0;
    for (size_t i = 0; i < 64800; i++) {
        if (abs(fb[i] - expected[i]) > 2) diff++;
    }

    free(expected);

    TEST_ASSERT_LESS_THAN(100, diff);  // Max 100 different pixels
}
```

---

## Заключение

Данное ТЗ описывает полный путь миграции от текущей "спагетти"-архитектуры с 2,367 прямыми вызовами M5.Display к современной, асинхронной GUI системе.

**Ключевые преимущества после миграции:**

1. **Производительность**: Рендеринг не блокирует main loop
2. **Масштабируемость**: Легко добавлять новые модули
3. **Расширяемость**: Поддержка внешних дисплеев
4. **Тестируемость**: UI можно тестировать без железа
5. **Customization**: Темы и стили без перекомпиляции

**Оценка трудозатрат:**

| Фаза | Время |
|------|-------|
| Фаза 0: Подготовка | 1-2 дня |
| Фаза 1: Core | 3-5 дней |
| Фаза 2: Legacy Bridge | 2-3 дня |
| Фаза 3: Theme | 1-2 дня |
| Фаза 4: Widgets | 5-7 дней |
| Фаза 5: Миграция | 7-14 дней |
| Фаза 6: External | 3-5 дней |
| Фаза 7: Cleanup | 2-3 дня |
| **ИТОГО** | **24-41 день** |

Рекомендуется начать с Фаз 0-2, чтобы получить работающий legacy bridge, а затем постепенно мигрировать модули по мере необходимости.
