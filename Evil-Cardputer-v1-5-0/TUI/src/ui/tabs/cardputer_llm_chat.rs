//! Cardputer LLM Chat Gateway tab UI
//!
//! Displays gateway status, active sessions, and provides controls
//! for starting/stopping the gateway.

use ratatui::{
    layout::{Alignment, Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Paragraph},
    Frame,
};

use crate::app::App;

/// Render the Cardputer LLM Chat tab
pub fn render(f: &mut Frame, area: Rect, app: &App) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(10), // Status panel
            Constraint::Length(7),  // Config panel
            Constraint::Min(5),     // Sessions panel
            Constraint::Length(5),  // Actions panel
        ])
        .split(area);

    render_status_panel(f, chunks[0], app);
    render_config_panel(f, chunks[1], app);
    render_sessions_panel(f, chunks[2], app);
    render_actions_panel(f, chunks[3]);
}

fn render_status_panel(f: &mut Frame, area: Rect, app: &App) {
    let block = Block::default()
        .title(" Gateway Status ")
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Cyan));

    let inner = block.inner(area);
    f.render_widget(block, area);

    let state = app.state.cardputer_gateway_state.read();

    let running = state.running;
    let status_color = if running { Color::Green } else { Color::Red };
    let status_text = if running { "RUNNING" } else { "STOPPED" };

    let uptime_text = if let Some(started) = state.started_at {
        let secs = started.elapsed().as_secs();
        let hours = secs / 3600;
        let mins = (secs % 3600) / 60;
        let secs = secs % 60;
        format!("{:02}:{:02}:{:02}", hours, mins, secs)
    } else {
        "--:--:--".to_string()
    };

    let error_line = if let Some(ref err) = state.last_error {
        Line::from(vec![
            Span::raw("Error:  "),
            Span::styled(err.clone(), Style::default().fg(Color::Red)),
        ])
    } else {
        Line::from(vec![
            Span::raw("Error:  "),
            Span::styled("None", Style::default().fg(Color::Gray)),
        ])
    };

    let lines = vec![
        Line::from(vec![
            Span::raw("Status:   "),
            Span::styled(status_text, Style::default().fg(status_color).add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![
            Span::raw("Bind:     "),
            Span::styled(
                format!("{}:{}", state.bind_address, state.port),
                Style::default().fg(Color::White),
            ),
        ]),
        Line::from(vec![
            Span::raw("Uptime:   "),
            Span::styled(uptime_text, Style::default().fg(Color::White)),
        ]),
        Line::from(vec![
            Span::raw("Requests: "),
            Span::styled(
                format!("{}", state.total_requests),
                Style::default().fg(Color::Cyan),
            ),
        ]),
        Line::from(vec![
            Span::raw("Auth:     "),
            Span::styled(
                format!("{} sessions", state.active_auth_sessions),
                Style::default().fg(Color::Yellow),
            ),
        ]),
        Line::from(vec![
            Span::raw("Chats:    "),
            Span::styled(
                format!("{} sessions", state.active_chat_sessions),
                Style::default().fg(Color::Magenta),
            ),
        ]),
        error_line,
    ];

    let paragraph = Paragraph::new(lines);
    f.render_widget(paragraph, inner);
}

fn render_config_panel(f: &mut Frame, area: Rect, app: &App) {
    let block = Block::default()
        .title(" Configuration ")
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Blue));

    let inner = block.inner(area);
    f.render_widget(block, area);

    let config = app.state.config.read();
    let gateway_config = &config.integrations.cardputer_llm_chat;

    let enabled_color = if gateway_config.enabled { Color::Green } else { Color::Red };
    let enabled_text = if gateway_config.enabled { "Yes" } else { "No" };

    let server_key_text: (String, Color) = if gateway_config.server_private_key.is_empty() {
        ("(not set)".to_string(), Color::Red)
    } else {
        let len = gateway_config.server_private_key.len().min(8);
        (format!("{}...", &gateway_config.server_private_key[..len]), Color::Green)
    };

    let cardputer_key_text: (String, Color) = if gateway_config.cardputer_public_key.is_empty() {
        ("(not set)".to_string(), Color::Red)
    } else {
        let len = gateway_config.cardputer_public_key.len().min(8);
        (format!("{}...", &gateway_config.cardputer_public_key[..len]), Color::Green)
    };

    let lines = vec![
        Line::from(vec![
            Span::raw("Enabled:         "),
            Span::styled(enabled_text, Style::default().fg(enabled_color)),
        ]),
        Line::from(vec![
            Span::raw("Server Key:      "),
            Span::styled(server_key_text.0.clone(), Style::default().fg(server_key_text.1)),
        ]),
        Line::from(vec![
            Span::raw("Cardputer Key:   "),
            Span::styled(cardputer_key_text.0.clone(), Style::default().fg(cardputer_key_text.1)),
        ]),
        Line::from(vec![
            Span::raw("Session Timeout: "),
            Span::styled(
                format!("{}s", gateway_config.session_timeout_secs),
                Style::default().fg(Color::White),
            ),
            Span::raw("  Chat Timeout: "),
            Span::styled(
                if gateway_config.chat_timeout_secs == 0 {
                    "unlimited".to_string()
                } else {
                    format!("{}s", gateway_config.chat_timeout_secs)
                },
                Style::default().fg(Color::White),
            ),
        ]),
    ];

    let paragraph = Paragraph::new(lines);
    f.render_widget(paragraph, inner);
}

fn render_sessions_panel(f: &mut Frame, area: Rect, app: &App) {
    let state = app.state.cardputer_gateway_state.read();
    let total_sessions = state.active_auth_sessions + state.active_chat_sessions;

    let block = Block::default()
        .title(format!(" Active Sessions ({}) ", total_sessions))
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Magenta));

    let inner = block.inner(area);
    f.render_widget(block, area);

    if total_sessions == 0 {
        let empty_message = Paragraph::new("No active sessions")
            .style(Style::default().fg(Color::Gray))
            .alignment(Alignment::Center);
        f.render_widget(empty_message, inner);
    } else {
        // Show session summary
        let lines = vec![
            Line::from(vec![
                Span::styled("Auth Sessions: ", Style::default().fg(Color::Yellow)),
                Span::styled(
                    format!("{}", state.active_auth_sessions),
                    Style::default().fg(Color::White),
                ),
            ]),
            Line::from(vec![
                Span::styled("Chat Sessions: ", Style::default().fg(Color::Magenta)),
                Span::styled(
                    format!("{}", state.active_chat_sessions),
                    Style::default().fg(Color::White),
                ),
            ]),
            Line::from(""),
            Line::from(Span::styled(
                "(Session details available via /health endpoint)",
                Style::default().fg(Color::Gray),
            )),
        ];

        let paragraph = Paragraph::new(lines);
        f.render_widget(paragraph, inner);
    }
}

fn render_actions_panel(f: &mut Frame, area: Rect) {
    let block = Block::default()
        .title(" Actions ")
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Yellow));

    let inner = block.inner(area);
    f.render_widget(block, area);

    let help_lines = vec![
        Line::from(vec![
            Span::styled("[Ctrl+G]", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),
            Span::raw(" Toggle Gateway On/Off (global hotkey)"),
        ]),
        Line::from(vec![
            Span::raw("Configure keys in "),
            Span::styled("config.toml", Style::default().fg(Color::Yellow)),
            Span::raw(" → "),
            Span::styled("[integrations.cardputer_llm_chat]", Style::default().fg(Color::Cyan)),
        ]),
    ];

    let paragraph = Paragraph::new(help_lines)
        .alignment(Alignment::Center);

    f.render_widget(paragraph, inner);
}
