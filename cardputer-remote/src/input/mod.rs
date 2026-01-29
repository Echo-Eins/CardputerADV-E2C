//! Input simulation module - mouse and keyboard control
//!
//! Maps Cardputer input commands to Windows input events

use crate::protocol::{ClickAction, InputMode, KeyEvent, MouseButton, MouseClick, MouseMove};
use enigo::{Enigo, Key, KeyboardControllable, MouseButton as EnigoButton, MouseControllable};
use std::collections::HashMap;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum InputError {
    #[error("Failed to initialize input controller")]
    InitError,

    #[error("Invalid keycode: {0}")]
    InvalidKeycode(u8),

    #[error("Input simulation failed")]
    SimulationFailed,
}

/// Input controller for mouse and keyboard
pub struct InputController {
    enigo: Enigo,
    current_mode: InputMode,
    /// Cumulative mouse position (for relative movement)
    mouse_speed: i32,
    /// Keycode mapping from USB HID to enigo keys
    keymap: HashMap<u8, Key>,
}

impl InputController {
    /// Create a new input controller
    pub fn new() -> Result<Self, InputError> {
        let enigo = Enigo::new();
        let keymap = Self::build_keymap();

        Ok(Self {
            enigo,
            current_mode: InputMode::Mouse,
            mouse_speed: 5, // Pixels per movement command
            keymap,
        })
    }

    /// Build USB HID keycode to enigo Key mapping
    fn build_keymap() -> HashMap<u8, Key> {
        let mut map = HashMap::new();

        // Letters (0x04 - 0x1D = a-z)
        for i in 0..26 {
            let c = (b'a' + i) as char;
            map.insert(0x04 + i, Key::Layout(c));
        }

        // Numbers (0x1E - 0x27 = 1-9, 0)
        for i in 0..9 {
            let c = (b'1' + i) as char;
            map.insert(0x1E + i, Key::Layout(c));
        }
        map.insert(0x27, Key::Layout('0'));

        // Special keys
        map.insert(0x28, Key::Return);      // Enter
        map.insert(0x29, Key::Escape);      // Escape
        map.insert(0x2A, Key::Backspace);   // Backspace
        map.insert(0x2B, Key::Tab);         // Tab
        map.insert(0x2C, Key::Space);       // Space
        map.insert(0x2D, Key::Layout('-')); // Minus
        map.insert(0x2E, Key::Layout('=')); // Equal
        map.insert(0x2F, Key::Layout('[')); // Left bracket
        map.insert(0x30, Key::Layout(']')); // Right bracket
        map.insert(0x31, Key::Layout('\\')); // Backslash
        map.insert(0x33, Key::Layout(';')); // Semicolon
        map.insert(0x34, Key::Layout('\'')); // Quote
        map.insert(0x35, Key::Layout('`')); // Grave
        map.insert(0x36, Key::Layout(',')); // Comma
        map.insert(0x37, Key::Layout('.')); // Period
        map.insert(0x38, Key::Layout('/')); // Slash

        // Function keys (0x3A - 0x45 = F1-F12)
        map.insert(0x3A, Key::F1);
        map.insert(0x3B, Key::F2);
        map.insert(0x3C, Key::F3);
        map.insert(0x3D, Key::F4);
        map.insert(0x3E, Key::F5);
        map.insert(0x3F, Key::F6);
        map.insert(0x40, Key::F7);
        map.insert(0x41, Key::F8);
        map.insert(0x42, Key::F9);
        map.insert(0x43, Key::F10);
        map.insert(0x44, Key::F11);
        map.insert(0x45, Key::F12);

        // Navigation keys
        map.insert(0x49, Key::Layout('\x7F')); // Insert (using Delete as placeholder)
        map.insert(0x4A, Key::Home);
        map.insert(0x4B, Key::PageUp);
        map.insert(0x4C, Key::Delete);
        map.insert(0x4D, Key::End);
        map.insert(0x4E, Key::PageDown);

        // Arrow keys
        map.insert(0x4F, Key::RightArrow);
        map.insert(0x50, Key::LeftArrow);
        map.insert(0x51, Key::DownArrow);
        map.insert(0x52, Key::UpArrow);

        map
    }

    /// Get current input mode
    pub fn get_mode(&self) -> InputMode {
        self.current_mode
    }

    /// Switch input mode
    pub fn switch_mode(&mut self, mode: InputMode) {
        self.current_mode = mode;
    }

    /// Toggle between mouse and keyboard mode
    pub fn toggle_mode(&mut self) -> InputMode {
        self.current_mode = match self.current_mode {
            InputMode::Mouse => InputMode::Keyboard,
            InputMode::Keyboard => InputMode::Mouse,
        };
        self.current_mode
    }

    /// Set mouse movement speed (pixels per command)
    pub fn set_mouse_speed(&mut self, speed: i32) {
        self.mouse_speed = speed.clamp(1, 50);
    }

    /// Handle mouse movement
    pub fn mouse_move(&mut self, movement: MouseMove) {
        let dx = movement.dx as i32 * self.mouse_speed;
        let dy = movement.dy as i32 * self.mouse_speed;
        self.enigo.mouse_move_relative(dx, dy);
    }

    /// Handle mouse click
    pub fn mouse_click(&mut self, click: MouseClick) {
        let button = match click.button {
            MouseButton::Left => EnigoButton::Left,
            MouseButton::Right => EnigoButton::Right,
            MouseButton::Middle => EnigoButton::Middle,
        };

        match click.action {
            ClickAction::Press => self.enigo.mouse_down(button),
            ClickAction::Release => self.enigo.mouse_up(button),
            ClickAction::Click => self.enigo.mouse_click(button),
            ClickAction::DoubleClick => {
                self.enigo.mouse_click(button);
                std::thread::sleep(std::time::Duration::from_millis(50));
                self.enigo.mouse_click(button);
            }
        }
    }

    /// Handle key press
    pub fn key_press(&mut self, event: KeyEvent) {
        // Apply modifiers
        if event.modifiers & 0x01 != 0 {
            self.enigo.key_down(Key::Control);
        }
        if event.modifiers & 0x02 != 0 {
            self.enigo.key_down(Key::Shift);
        }
        if event.modifiers & 0x04 != 0 {
            self.enigo.key_down(Key::Alt);
        }
        if event.modifiers & 0x08 != 0 {
            self.enigo.key_down(Key::Meta);
        }

        // Press the key
        if let Some(&key) = self.keymap.get(&event.keycode) {
            self.enigo.key_down(key);
        }
    }

    /// Handle key release
    pub fn key_release(&mut self, event: KeyEvent) {
        // Release the key
        if let Some(&key) = self.keymap.get(&event.keycode) {
            self.enigo.key_up(key);
        }

        // Release modifiers
        if event.modifiers & 0x01 != 0 {
            self.enigo.key_up(Key::Control);
        }
        if event.modifiers & 0x02 != 0 {
            self.enigo.key_up(Key::Shift);
        }
        if event.modifiers & 0x04 != 0 {
            self.enigo.key_up(Key::Alt);
        }
        if event.modifiers & 0x08 != 0 {
            self.enigo.key_up(Key::Meta);
        }
    }

    /// Type a string (for keyboard mode)
    pub fn type_string(&mut self, text: &str) {
        self.enigo.key_sequence(text);
    }

    /// Handle arrow key input in mouse mode (convert to mouse movement)
    pub fn arrow_to_mouse(&mut self, keycode: u8) {
        let movement = match keycode {
            0x4F => MouseMove { dx: 1, dy: 0 },  // Right
            0x50 => MouseMove { dx: -1, dy: 0 }, // Left
            0x51 => MouseMove { dx: 0, dy: 1 },  // Down
            0x52 => MouseMove { dx: 0, dy: -1 }, // Up
            _ => return,
        };
        self.mouse_move(movement);
    }
}

/// Modifier key flags
pub mod modifiers {
    pub const CTRL: u8 = 0x01;
    pub const SHIFT: u8 = 0x02;
    pub const ALT: u8 = 0x04;
    pub const GUI: u8 = 0x08; // Windows/Command key
}

/// USB HID keycodes for common keys
pub mod keycodes {
    // Letters
    pub const KEY_A: u8 = 0x04;
    pub const KEY_Z: u8 = 0x1D;

    // Numbers
    pub const KEY_1: u8 = 0x1E;
    pub const KEY_0: u8 = 0x27;

    // Special
    pub const KEY_ENTER: u8 = 0x28;
    pub const KEY_ESCAPE: u8 = 0x29;
    pub const KEY_BACKSPACE: u8 = 0x2A;
    pub const KEY_TAB: u8 = 0x2B;
    pub const KEY_SPACE: u8 = 0x2C;

    // Arrow keys
    pub const KEY_RIGHT: u8 = 0x4F;
    pub const KEY_LEFT: u8 = 0x50;
    pub const KEY_DOWN: u8 = 0x51;
    pub const KEY_UP: u8 = 0x52;

    // Function keys
    pub const KEY_F1: u8 = 0x3A;
    pub const KEY_F12: u8 = 0x45;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_keymap_coverage() {
        let keymap = InputController::build_keymap();

        // Test letters
        for i in 0..26 {
            assert!(keymap.contains_key(&(0x04 + i)));
        }

        // Test numbers
        for i in 0..10 {
            assert!(keymap.contains_key(&(0x1E + i)));
        }

        // Test arrow keys
        assert!(keymap.contains_key(&keycodes::KEY_UP));
        assert!(keymap.contains_key(&keycodes::KEY_DOWN));
        assert!(keymap.contains_key(&keycodes::KEY_LEFT));
        assert!(keymap.contains_key(&keycodes::KEY_RIGHT));
    }

    #[test]
    fn test_mode_toggle() {
        let mut controller = InputController {
            enigo: Enigo::new(),
            current_mode: InputMode::Mouse,
            mouse_speed: 5,
            keymap: HashMap::new(),
        };

        assert_eq!(controller.get_mode(), InputMode::Mouse);

        controller.toggle_mode();
        assert_eq!(controller.get_mode(), InputMode::Keyboard);

        controller.toggle_mode();
        assert_eq!(controller.get_mode(), InputMode::Mouse);
    }
}
