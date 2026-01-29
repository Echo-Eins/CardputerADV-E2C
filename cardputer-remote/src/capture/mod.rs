//! Screen capture module - captures screen and compresses to JPEG
//!
//! Uses scrap for cross-platform capture and turbojpeg for fast compression

use image::{imageops::FilterType, DynamicImage, RgbImage};
use scrap::{Capturer, Display};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Arc;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum CaptureError {
    #[error("No display found")]
    NoDisplay,

    #[error("Failed to initialize capturer: {0}")]
    InitError(String),

    #[error("Capture failed: {0}")]
    CaptureError(String),

    #[error("JPEG compression failed: {0}")]
    CompressionError(String),

    #[error("Invalid capture region")]
    InvalidRegion,
}

/// Screen capturer with delta detection
pub struct ScreenCapturer {
    capturer: Capturer,
    width: usize,
    height: usize,
    target_width: u32,
    target_height: u32,
    jpeg_quality: u8,
    capture_region: Option<CaptureRegion>,
    last_frame_hash: AtomicU32,
    frame_counter: AtomicU32,
    running: Arc<AtomicBool>,
}

#[derive(Debug, Clone, Copy)]
pub struct CaptureRegion {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

impl ScreenCapturer {
    /// Create a new screen capturer
    pub fn new(
        target_width: u32,
        target_height: u32,
        jpeg_quality: u8,
        capture_region: Option<[u32; 4]>,
    ) -> Result<Self, CaptureError> {
        let display = Display::primary().map_err(|e| CaptureError::InitError(e.to_string()))?;

        let width = display.width();
        let height = display.height();

        let capturer =
            Capturer::new(display).map_err(|e| CaptureError::InitError(e.to_string()))?;

        let region = capture_region.map(|r| CaptureRegion {
            x: r[0],
            y: r[1],
            width: r[2],
            height: r[3],
        });

        // Validate region if specified
        if let Some(ref r) = region {
            if r.x + r.width > width as u32 || r.y + r.height > height as u32 {
                return Err(CaptureError::InvalidRegion);
            }
        }

        Ok(Self {
            capturer,
            width,
            height,
            target_width,
            target_height,
            jpeg_quality,
            capture_region: region,
            last_frame_hash: AtomicU32::new(0),
            frame_counter: AtomicU32::new(0),
            running: Arc::new(AtomicBool::new(true)),
        })
    }

    /// Get the running flag for external control
    pub fn get_running_flag(&self) -> Arc<AtomicBool> {
        self.running.clone()
    }

    /// Capture a single frame
    /// Returns None if frame hasn't changed (delta detection)
    pub fn capture_frame(&mut self) -> Result<Option<CapturedFrame>, CaptureError> {
        // Try to capture
        let buffer = loop {
            match self.capturer.frame() {
                Ok(frame) => break frame,
                Err(e) => {
                    if e.kind() == std::io::ErrorKind::WouldBlock {
                        // Frame not ready, wait a bit
                        std::thread::sleep(std::time::Duration::from_millis(10));
                        continue;
                    }
                    return Err(CaptureError::CaptureError(e.to_string()));
                }
            }
        };

        // Convert BGRA to RGB
        let rgb_data = self.bgra_to_rgb(&buffer);

        // Compute simple hash for delta detection
        let hash = self.compute_hash(&rgb_data);
        let last_hash = self.last_frame_hash.load(Ordering::Relaxed);

        if hash == last_hash && last_hash != 0 {
            // Frame hasn't changed significantly
            return Ok(None);
        }

        self.last_frame_hash.store(hash, Ordering::Relaxed);

        // Create image
        let (src_width, src_height) = if let Some(ref region) = self.capture_region {
            (region.width as usize, region.height as usize)
        } else {
            (self.width, self.height)
        };

        let image = RgbImage::from_raw(src_width as u32, src_height as u32, rgb_data)
            .ok_or_else(|| CaptureError::CaptureError("Failed to create image".into()))?;

        // Resize to target resolution
        let dynamic = DynamicImage::ImageRgb8(image);
        let resized = dynamic.resize_exact(
            self.target_width,
            self.target_height,
            FilterType::Triangle, // Good balance of speed and quality
        );

        // Compress to JPEG
        let jpeg_data = self.compress_jpeg(&resized)?;

        let sequence = self.frame_counter.fetch_add(1, Ordering::Relaxed);

        Ok(Some(CapturedFrame {
            sequence,
            jpeg_data,
            width: self.target_width,
            height: self.target_height,
        }))
    }

    /// Convert BGRA buffer to RGB, optionally extracting region
    fn bgra_to_rgb(&self, bgra: &[u8]) -> Vec<u8> {
        let stride = self.width * 4; // BGRA = 4 bytes per pixel

        let (start_x, start_y, crop_width, crop_height) = if let Some(ref region) = self.capture_region
        {
            (
                region.x as usize,
                region.y as usize,
                region.width as usize,
                region.height as usize,
            )
        } else {
            (0, 0, self.width, self.height)
        };

        let mut rgb = Vec::with_capacity(crop_width * crop_height * 3);

        for y in start_y..(start_y + crop_height) {
            for x in start_x..(start_x + crop_width) {
                let offset = y * stride + x * 4;
                if offset + 2 < bgra.len() {
                    rgb.push(bgra[offset + 2]); // R
                    rgb.push(bgra[offset + 1]); // G
                    rgb.push(bgra[offset]);     // B
                }
            }
        }

        rgb
    }

    /// Compute a simple hash for delta detection
    /// Uses sampling to avoid processing entire frame
    fn compute_hash(&self, rgb: &[u8]) -> u32 {
        let sample_count = 256;
        let step = rgb.len() / sample_count;

        if step == 0 {
            return 0;
        }

        let mut hash: u32 = 0;
        for i in 0..sample_count {
            let idx = i * step;
            if idx < rgb.len() {
                hash = hash.wrapping_mul(31).wrapping_add(rgb[idx] as u32);
            }
        }
        hash
    }

    /// Compress image to JPEG using turbojpeg or fallback
    fn compress_jpeg(&self, image: &DynamicImage) -> Result<Vec<u8>, CaptureError> {
        // Use image crate's JPEG encoder as fallback
        // In production, we'd use turbojpeg for better performance
        let mut jpeg_data = Vec::new();
        let mut cursor = std::io::Cursor::new(&mut jpeg_data);

        image
            .write_to(&mut cursor, image::ImageFormat::Jpeg)
            .map_err(|e| CaptureError::CompressionError(e.to_string()))?;

        // Note: image crate doesn't support quality setting directly
        // For production, use turbojpeg with quality parameter

        Ok(jpeg_data)
    }

    /// Stop capturing
    pub fn stop(&self) {
        self.running.store(false, Ordering::Relaxed);
    }
}

/// Captured frame data
#[derive(Debug, Clone)]
pub struct CapturedFrame {
    /// Frame sequence number
    pub sequence: u32,
    /// JPEG compressed data
    pub jpeg_data: Vec<u8>,
    /// Frame width
    pub width: u32,
    /// Frame height
    pub height: u32,
}

impl CapturedFrame {
    /// Get size of JPEG data
    pub fn size(&self) -> usize {
        self.jpeg_data.len()
    }
}

/// High-performance JPEG compressor using turbojpeg
#[cfg(feature = "turbojpeg")]
pub struct TurboJpegCompressor {
    quality: i32,
}

#[cfg(feature = "turbojpeg")]
impl TurboJpegCompressor {
    pub fn new(quality: u8) -> Self {
        Self {
            quality: quality as i32,
        }
    }

    pub fn compress(&self, rgb_data: &[u8], width: u32, height: u32) -> Result<Vec<u8>, CaptureError> {
        use turbojpeg::{Compressor, Image, PixelFormat};

        let image = Image {
            pixels: rgb_data,
            width: width as usize,
            pitch: width as usize * 3,
            height: height as usize,
            format: PixelFormat::RGB,
        };

        let mut compressor = Compressor::new().map_err(|e| CaptureError::CompressionError(e.to_string()))?;
        compressor.set_quality(self.quality);

        compressor
            .compress_to_vec(image)
            .map_err(|e| CaptureError::CompressionError(e.to_string()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_hash_consistency() {
        let data = vec![0u8; 1024];
        let capturer = ScreenCapturer {
            capturer: unsafe { std::mem::zeroed() }, // Don't actually use this
            width: 100,
            height: 100,
            target_width: 240,
            target_height: 135,
            jpeg_quality: 70,
            capture_region: None,
            last_frame_hash: AtomicU32::new(0),
            frame_counter: AtomicU32::new(0),
            running: Arc::new(AtomicBool::new(true)),
        };

        let hash1 = capturer.compute_hash(&data);
        let hash2 = capturer.compute_hash(&data);
        assert_eq!(hash1, hash2);
    }
}
