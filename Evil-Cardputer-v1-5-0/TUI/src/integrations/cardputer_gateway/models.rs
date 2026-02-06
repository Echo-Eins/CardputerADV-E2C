//! Models endpoint for Cardputer LLM Chat Gateway
//!
//! Queries Ollama for available models and classifies them as local or cloud.
//! Cloud models are identified by the `-cloud` suffix in their name.

use anyhow::Result;
use std::process::Command;

use super::dto::{ModelInfo, ModelType, ModelsResponse};

/// Get list of available Ollama models
pub async fn list_models(model_type_filter: Option<ModelType>) -> Result<ModelsResponse> {
    let output = Command::new("ollama")
        .arg("list")
        .output()?;

    if !output.status.success() {
        return Ok(ModelsResponse { models: Vec::new() });
    }

    let stdout = String::from_utf8_lossy(&output.stdout);
    let models = parse_model_list(&stdout, model_type_filter)?;

    Ok(ModelsResponse { models })
}

/// Check if Ollama is available
pub async fn check_ollama_available() -> bool {
    match Command::new("ollama").arg("--version").output() {
        Ok(output) => output.status.success(),
        Err(_) => false,
    }
}

/// Parse ollama list output into ModelInfo structs
fn parse_model_list(output: &str, type_filter: Option<ModelType>) -> Result<Vec<ModelInfo>> {
    let mut models = Vec::new();
    let mut lines = output.lines().filter(|line| !line.trim().is_empty());

    // Skip header line
    let header = match lines.next() {
        Some(line) => line,
        None => return Ok(models),
    };

    // Find column positions
    let headers = split_columns(header);
    let name_idx = find_column(&headers, "NAME").unwrap_or(0);
    let size_idx = find_column(&headers, "SIZE").unwrap_or(2);

    for line in lines {
        let cols = split_columns(line);
        if cols.is_empty() || cols.len() <= name_idx {
            continue;
        }

        let name = cols[name_idx].to_string();
        let size_display = cols.get(size_idx).map(|s| s.to_string()).unwrap_or_default();

        // Classify model type: -cloud suffix = cloud, otherwise local
        let model_type = if name.ends_with("-cloud") || name.contains("-cloud:") {
            ModelType::Cloud
        } else {
            ModelType::Local
        };

        // Apply filter if specified
        if let Some(filter) = type_filter {
            if model_type != filter {
                continue;
            }
        }

        // Parse params from size if available (e.g., "7B", "13B")
        let params_display = extract_params(&name);

        models.push(ModelInfo {
            name,
            model_type,
            size_display,
            params_display,
        });
    }

    Ok(models)
}

/// Split a line into columns by whitespace
fn split_columns(line: &str) -> Vec<&str> {
    line.split_whitespace().collect()
}

/// Find column index by header name
fn find_column(headers: &[&str], name: &str) -> Option<usize> {
    headers.iter().position(|h| h.eq_ignore_ascii_case(name))
}

/// Extract parameter count from model name (e.g., "llama3:7b" -> "7B")
fn extract_params(name: &str) -> String {
    // Common patterns: model:7b, model-7b, model:13b-q4
    let lower = name.to_lowercase();

    // Look for patterns like :7b, :13b, :70b, etc.
    for part in lower.split([':', '-']) {
        if let Some(params) = parse_param_string(part) {
            return params;
        }
    }

    String::new()
}

/// Parse a string like "7b", "13b", "70b" into "7B", "13B", "70B"
fn parse_param_string(s: &str) -> Option<String> {
    let s = s.trim();
    if s.ends_with('b') && s.len() > 1 {
        let num_part = &s[..s.len() - 1];
        if num_part.chars().all(|c| c.is_ascii_digit() || c == '.') {
            return Some(format!("{}B", num_part.to_uppercase()));
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_model_list() {
        let output = r#"NAME                    ID            SIZE      MODIFIED
llama3:8b               abc123        4.7 GB    2 days ago
gemma2:9b               def456        5.5 GB    1 week ago
gpt4-cloud:latest       xyz789        0 B       3 days ago
"#;

        let models = parse_model_list(output, None).unwrap();
        assert_eq!(models.len(), 3);
        assert_eq!(models[0].name, "llama3:8b");
        assert_eq!(models[0].model_type, ModelType::Local);
        assert_eq!(models[2].name, "gpt4-cloud:latest");
        assert_eq!(models[2].model_type, ModelType::Cloud);
    }

    #[test]
    fn test_parse_model_list_with_filter() {
        let output = r#"NAME                    ID            SIZE      MODIFIED
llama3:8b               abc123        4.7 GB    2 days ago
gpt4-cloud:latest       xyz789        0 B       3 days ago
"#;

        let local_models = parse_model_list(output, Some(ModelType::Local)).unwrap();
        assert_eq!(local_models.len(), 1);
        assert_eq!(local_models[0].name, "llama3:8b");

        let cloud_models = parse_model_list(output, Some(ModelType::Cloud)).unwrap();
        assert_eq!(cloud_models.len(), 1);
        assert_eq!(cloud_models[0].name, "gpt4-cloud:latest");
    }

    #[test]
    fn test_extract_params() {
        assert_eq!(extract_params("llama3:8b"), "8B");
        assert_eq!(extract_params("gemma2:9b-q4"), "9B");
        assert_eq!(extract_params("mixtral:8x7b"), "8X7B"); // might need special handling
        assert_eq!(extract_params("phi3:3.8b"), "3.8B");
    }
}
