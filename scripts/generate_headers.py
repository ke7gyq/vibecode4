#!/usr/bin/env python3
"""
Generate C header files from configuration.json

Reads centralized configuration.json and generates C preprocessor macros
for all system parameters. This ensures a single source of truth for
configuration across the entire project.

Usage:
    python3 generate_headers.py <config_file> <output_header>
    python3 generate_headers.py ../configuration.json ../src/generated/config_constants.h
"""

import json
import sys
import os
from datetime import datetime


def generate_header_from_config(config_file, output_header):
    """
    Generate C header file with #define macros from configuration.json
    
    Args:
        config_file: Path to configuration.json
        output_header: Path to output config_constants.h
    """
    
    # Read configuration
    try:
        with open(config_file, 'r') as f:
            config = json.load(f)
    except Exception as e:
        print(f"ERROR: Failed to read {config_file}: {e}")
        return False
    
    # Generate header content
    header_lines = []
    
    # Add header guards and documentation
    header_lines.append("#ifndef CONFIG_CONSTANTS_H")
    header_lines.append("#define CONFIG_CONSTANTS_H")
    header_lines.append("")
    header_lines.append("/**")
    header_lines.append(" * Auto-generated configuration constants header")
    header_lines.append(f" * Generated: {datetime.now().isoformat()}")
    header_lines.append(" * Source: configuration.json")
    header_lines.append(" * DO NOT EDIT MANUALLY - regenerate using scripts/generate_headers.py")
    header_lines.append(" */")
    header_lines.append("")
    
    # Display configuration
    header_lines.append("/* ============== DISPLAY CONFIGURATION ============== */")
    header_lines.append("")
    if 'display' in config:
        disp = config['display']
        header_lines.append(f"#define DISPLAY_CONTROLLER      \"{disp.get('controller', 'ST7789')}\"")
        header_lines.append(f"#define SCREEN_WIDTH            {disp.get('screen_width', 320)}")
        header_lines.append(f"#define SCREEN_HEIGHT           {disp.get('screen_height', 240)}")
        header_lines.append(f"#define DISPLAY_ORIENTATION     \"{disp.get('orientation', 'portrait')}\"")
        header_lines.append("")
        
        # Pin definitions
        pins = disp.get('pins', {})
        header_lines.append("/* Display pins */")
        header_lines.append(f"#define PIN_DIN                 {pins.get('DIN', 0)}")
        header_lines.append(f"#define PIN_CLK                 {pins.get('CLK', 1)}")
        header_lines.append(f"#define PIN_CS                  {pins.get('CS', 2)}")
        header_lines.append(f"#define PIN_DC                  {pins.get('DC', 3)}")
        header_lines.append(f"#define PIN_RESET               {pins.get('RESET', 4)}")
        header_lines.append(f"#define PIN_BL                  {pins.get('BL', 5)}")
        header_lines.append("")
        
        header_lines.append(f"#define SERIAL_CLK_DIV          {disp.get('serial_clock_divider', 1.0)}")
        header_lines.append("")
    
    # Audio filter configuration
    header_lines.append("/* ============== AUDIO FILTER CONFIGURATION ============== */")
    header_lines.append("")
    if 'audio_filter' in config:
        audio = config['audio_filter']
        header_lines.append(f"#define AUDIO_FILTER_FS         {audio.get('sample_rate_hz', 48000)}")
        header_lines.append(f"#define AUDIO_FILTER_LP_HZ      {audio.get('lowpass_hz', 10000)}")
        header_lines.append(f"#define AUDIO_FILTER_HP_HZ      {audio.get('highpass_hz', 50)}")
        header_lines.append(f"#define AUDIO_FILTER_DECIMATION {audio.get('decimation_factor', 64)}")
        header_lines.append(f"#define AUDIO_FILTER_DECIMATION_MAX {audio.get('decimation_max', 128)}")
        header_lines.append(f"#define AUDIO_FILTER_SINCN      {audio.get('sinc_order', 3)}")
        header_lines.append(f"#define AUDIO_FILTER_MAX_VOLUME {audio.get('max_volume', 16)}")
        header_lines.append(f"#define AUDIO_FILTER_GAIN       {audio.get('gain', 1)}")
        header_lines.append("")
    
    # Spectrogram configuration
    header_lines.append("/* ============== SPECTROGRAM CONFIGURATION ============== */")
    header_lines.append("")
    if 'spectrogram' in config:
        spec = config['spectrogram']
        header_lines.append(f"#define SPECTROGRAM_FFT_SIZE    {spec.get('fft_size', 256)}")
        header_lines.append(f"#define SPECTROGRAM_SAMPLE_RATE {spec.get('fft_sample_rate_hz', 6000)}")
        header_lines.append(f"#define SPECTROGRAM_DOWNSAMPLE_FACTOR {spec.get('downsample_factor', 8)}")
        header_lines.append(f"#define SPECTROGRAM_NUM_BINS    {spec.get('num_display_bins', 16)}")
        header_lines.append(f"#define SPECTROGRAM_BIN_SIZE    {spec.get('fft_size', 256) // spec.get('num_display_bins', 16)}")
        header_lines.append(f"#define SPECTROGRAM_BINS_SKIP   {spec.get('bins_skip', 5)}")
        header_lines.append(f"#define SPECTROGRAM_MAG_MAX     {spec.get('magnitude_max', 2000.0)}")
        header_lines.append(f"#define GAIN_NORMALIZATION      {spec.get('gain_normalization', 10)}")
        header_lines.append("")
    
    # Waterfall configuration
    header_lines.append("/* ============== WATERFALL CONFIGURATION ============== */")
    header_lines.append("")
    if 'waterfall' in config:
        wf = config['waterfall']
        header_lines.append(f"#define WATERFALL_MAX_FREQ_HZ   {wf.get('max_frequency_hz', 3000)}")
        header_lines.append(f"#define WATERFALL_ACCM_FRAMES   {wf.get('accumulation_frames', 9)}")
        header_lines.append(f"#define WATERFALL_PIXELS_PER_BAR {wf.get('pixels_per_bar', 24)}")
        header_lines.append(f"#define WATERFALL_BINS_PER_BAR  {wf.get('bins_per_bar', 5)}")
        header_lines.append(f"#define WATERFALL_FFT_BINS      {wf.get('fft_bins_available', 128)}")
        header_lines.append(f"#define WATERFALL_USED_BINS     {wf.get('fft_bins_used', 120)}")
        header_lines.append("")
    
    # Footer
    header_lines.append("#endif /* CONFIG_CONSTANTS_H */")
    header_lines.append("")
    
    # Write output header
    try:
        os.makedirs(os.path.dirname(output_header), exist_ok=True)
        with open(output_header, 'w') as f:
            f.write('\n'.join(header_lines))
        print(f"✓ Generated: {output_header}")
        return True
    except Exception as e:
        print(f"ERROR: Failed to write {output_header}: {e}")
        return False


def main():
    """Main entry point"""
    
    # Determine paths
    if len(sys.argv) > 2:
        config_file = sys.argv[1]
        output_header = sys.argv[2]
    else:
        # Default paths relative to script location
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.dirname(script_dir)
        config_file = os.path.join(project_root, 'configuration.json')
        output_header = os.path.join(project_root, 'src', 'generated', 'config_constants.h')
    
    print(f"Generating headers from: {config_file}")
    print(f"Output header: {output_header}")
    print("")
    
    if generate_header_from_config(config_file, output_header):
        print("✓ Header generation complete")
        sys.exit(0)
    else:
        print("✗ Header generation failed")
        sys.exit(1)


if __name__ == '__main__':
    main()
