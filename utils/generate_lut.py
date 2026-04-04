#!/usr/bin/env python3
"""
OpenPDM2PCM LUT Generator

Generates a C header file (LUT_Params.h) containing the Look-Up Table (LUT) values
for the OpenPDM2PCM filter based on filter configuration parameters.

The LUT is a 3D array: int32_t lut[256][DECIMATION_MAX/8][SINCN]
where:
  - 256: possible byte values (0-255)
  - DECIMATION_MAX/8: decimation factor divided by 8
  - SINCN: 3 sinc filters

This script replicates the Open_PDM_Filter_Init logic from OpenPDMFilter.c
"""

import math
import sys
import os
import re


def parse_microphone_config(config_path):
    """Parse filter configuration from microphone_config.h
    
    Args:
        config_path: Path to microphone_config.h
        
    Returns:
        Dictionary with parsed configuration values
    """
    config = {}
    
    if not os.path.exists(config_path):
        print(f"Warning: {config_path} not found, using defaults")
        return {
            'LP_HZ': 10000.0,
            'HP_HZ': 50.0,
            'Fs': 48000,
            'Decimation': 64,
            'MaxVolume': 16,
            'Gain': 1,
            'SINCN': 3,
            'DECIMATION_MAX': 128,
        }
    
    with open(config_path, 'r') as f:
        content = f.read()
    
    # Parse #define values with AUDIO_FILTER_ prefix
    patterns = {
        'LP_HZ': (r'#define\s+AUDIO_FILTER_LP_HZ\s+([\d.]+)', float),
        'HP_HZ': (r'#define\s+AUDIO_FILTER_HP_HZ\s+([\d.]+)', float),
        'Fs': (r'#define\s+AUDIO_FILTER_FS\s+(\d+)', int),
        'Decimation': (r'#define\s+AUDIO_FILTER_DECIMATION\s+(\d+)', int),
        'MaxVolume': (r'#define\s+AUDIO_FILTER_MAX_VOLUME\s+(\d+)', int),
        'Gain': (r'#define\s+AUDIO_FILTER_GAIN\s+(\d+)', int),
        'SINCN': (r'#define\s+AUDIO_FILTER_SINCN\s+(\d+)', int),
        'DECIMATION_MAX': (r'#define\s+AUDIO_FILTER_DECIMATION_MAX\s+(\d+)', int),
    }
    
    for key, (pattern, cast_fn) in patterns.items():
        match = re.search(pattern, content)
        if match:
            config[key] = cast_fn(match.group(1))
    
    # Defaults if not found
    if 'SINCN' not in config:
        config['SINCN'] = 3
    if 'DECIMATION_MAX' not in config:
        config['DECIMATION_MAX'] = 128
    
    return config


class OpenPDMFilterLUTGenerator:
    """Generates LUT values for OpenPDM2PCM filter"""
    
    def __init__(self, config_path=None):
        """Initialize the generator with configuration
        
        Args:
            config_path: Path to microphone_config.h for reading parameters
                        If None, uses default internal configuration
        """
        # Load configuration from header file or use defaults
        self.FILTER_CONFIG = parse_microphone_config(config_path or 'microphone_config.h')
        
        self.SINCN = self.FILTER_CONFIG.get('SINCN', 3)
        self.DECIMATION_MAX = self.FILTER_CONFIG.get('DECIMATION_MAX', 128)
        
        self.decimation = self.FILTER_CONFIG['Decimation']
        self.sinc1 = [0] * self.DECIMATION_MAX
        self.sinc2 = [0] * (self.DECIMATION_MAX * 2)
        self.sinc = [0] * (self.DECIMATION_MAX * self.SINCN)
        self.coef = [[0] * self.DECIMATION_MAX for _ in range(self.SINCN)]
        self.lut = [[[0] * self.SINCN for _ in range(self.DECIMATION_MAX // 8)] 
                    for _ in range(256)]
        
        self.div_const = 0
        self.sub_const = 0
    
    def convolve(self, signal, signal_len, kernel, kernel_len):
        """Convolve two arrays
        
        Args:
            signal: Input signal array
            signal_len: Length of signal
            kernel: Kernel array
            kernel_len: Length of kernel
            
        Returns:
            Result array of length signal_len + kernel_len - 1
        """
        result_len = signal_len + kernel_len - 1
        result = [0] * result_len
        
        for n in range(result_len):
            kmin = max(0, n - kernel_len + 1)
            kmax = min(n, signal_len - 1)
            
            for k in range(kmin, kmax + 1):
                result[n] += signal[k] * kernel[n - k]
        
        return result
    
    def initialize_filter(self):
        """Initialize the filter - replicate Open_PDM_Filter_Init logic"""
        
        # Initialize sinc1 array (all ones)
        for i in range(self.decimation):
            self.sinc1[i] = 1
        
        # Calculate sinc2 = convolve(sinc1, sinc1)
        self.sinc2 = self.convolve(self.sinc1[:self.decimation], 
                                   self.decimation,
                                   self.sinc1[:self.decimation], 
                                   self.decimation)
        
        # Calculate sinc = convolve(sinc2, sinc1) with padding
        self.sinc[0] = 0
        self.sinc[self.decimation * self.SINCN - 1] = 0
        sinc_temp = self.convolve(self.sinc2, 
                                  self.decimation * 2 - 1,
                                  self.sinc1[:self.decimation], 
                                  self.decimation)
        
        # Copy to sinc array (accounting for padding)
        for i in range(1, self.decimation * self.SINCN - 1):
            if i - 1 < len(sinc_temp):
                self.sinc[i] = sinc_temp[i - 1]
        
        # Build coefficient array
        total_sum = 0
        for j in range(self.SINCN):
            for i in range(self.decimation):
                self.coef[j][i] = self.sinc[j * self.decimation + i]
                total_sum += self.sinc[j * self.decimation + i]
        
        # Calculate constants for normalization
        self.sub_const = total_sum >> 1
        # Use gain from config, with fallback to default of 16
        gain = self.FILTER_CONFIG.get('Gain', 1)
        filter_gain = 16  # Standard filter gain constant
        self.div_const = (self.sub_const * self.FILTER_CONFIG['MaxVolume'] // 
                         32768 // filter_gain)
        if self.div_const == 0:
            self.div_const = 1
        
        # Generate LUT
        self._generate_lut()
    
    def _generate_lut(self):
        """Generate the Look-Up Table"""
        
        for s in range(self.SINCN):
            coef_p = self.coef[s]
            for c in range(256):
                for d in range(self.decimation // 8):
                    lut_value = (
                        ((c >> 7)       ) * coef_p[d * 8    ] +
                        ((c >> 6) & 0x01) * coef_p[d * 8 + 1] +
                        ((c >> 5) & 0x01) * coef_p[d * 8 + 2] +
                        ((c >> 4) & 0x01) * coef_p[d * 8 + 3] +
                        ((c >> 3) & 0x01) * coef_p[d * 8 + 4] +
                        ((c >> 2) & 0x01) * coef_p[d * 8 + 5] +
                        ((c >> 1) & 0x01) * coef_p[d * 8 + 6] +
                        ((c     ) & 0x01) * coef_p[d * 8 + 7]
                    )
                    self.lut[c][d][s] = int(lut_value)
    
    def generate_header_file(self, output_path):
        """Generate the C header file with LUT values
        
        Args:
            output_path: Path where to write the header file
        """
        
        with open(output_path, 'w') as f:
            # Write file header
            f.write('/*\n')
            f.write(' * LUT_Params.h - Auto-generated OpenPDM2PCM LUT values\n')
            f.write(' * Generated by: generate_lut.py\n')
            f.write(' *\n')
            f.write(' * Filter Configuration:\n')
            f.write(f' *   Decimation: {self.FILTER_CONFIG["Decimation"]}\n')
            f.write(f' *   Output Sample Rate: {self.FILTER_CONFIG["Fs"]} Hz\n')
            f.write(f' *   LP Filter: {self.FILTER_CONFIG["LP_HZ"]} Hz\n')
            f.write(f' *   HP Filter: {self.FILTER_CONFIG["HP_HZ"]} Hz\n')
            f.write(f' *   Max Volume: {self.FILTER_CONFIG["MaxVolume"]}\n')
            f.write(' */\n\n')
            
            f.write('#ifndef LUT_PARAMS_H\n')
            f.write('#define LUT_PARAMS_H\n\n')
            
            f.write('#include <stdint.h>\n\n')
            
            # Write constants
            f.write('/* Filter Constants */\n')
            f.write(f'#define LUT_DECIMATION {self.decimation}\n')
            f.write(f'#define LUT_SINCN {self.SINCN}\n')
            f.write(f'#define LUT_DIV_CONST {self.div_const}\n')
            f.write(f'#define LUT_SUB_CONST {self.sub_const}LL\n\n')
            
            # Write LUT array
            f.write('/* 3D LUT array: lut[256][DECIMATION/8][SINCN] */\n')
            f.write('const int32_t lut[256][64/8][3] = {\n')
            
            for c in range(256):
                f.write('    {{ /* byte value 0x{:02X} */\n'.format(c))
                for d in range(self.decimation // 8):
                    f.write('        { ')
                    for s in range(self.SINCN):
                        f.write(f'{self.lut[c][d][s]:10d}')
                        if s < self.SINCN - 1:
                            f.write(', ')
                    f.write(' },\n')
                f.write('    }')
                if c < 255:
                    f.write(',\n')
                else:
                    f.write('\n')
            
            f.write('};\n\n')
            
            # Write coefficient array (for reference/debugging)
            # Write sinc intermediate arrays (needed for OpenPDMFilter_Init when USE_CONST_LUT=1)
            f.write('/* Intermediate sinc arrays for filter initialization */\n')
            f.write(f'const uint32_t sinc1[{self.DECIMATION_MAX}] = {{\n')
            for i in range(self.DECIMATION_MAX):
                if i < self.decimation:
                    f.write('    1')
                else:
                    f.write('    0')
                if i < self.DECIMATION_MAX - 1:
                    f.write(',')
                if (i + 1) % 16 == 0:
                    f.write('\n')
                else:
                    f.write(' ')
            f.write('};\n\n')
            
            f.write(f'const uint32_t sinc2[{self.DECIMATION_MAX * 2}] = {{\n')
            for i in range(self.DECIMATION_MAX * 2):
                if i < len(self.sinc2):
                    f.write(f'    {self.sinc2[i]}')
                else:
                    f.write('    0')
                if i < self.DECIMATION_MAX * 2 - 1:
                    f.write(',')
                if (i + 1) % 8 == 0:
                    f.write('\n')
                else:
                    f.write(' ')
            f.write('};\n\n')
            
            f.write(f'const uint32_t sinc[{self.DECIMATION_MAX * self.SINCN}] = {{\n')
            for i in range(self.DECIMATION_MAX * self.SINCN):
                if i < len(self.sinc):
                    f.write(f'    {self.sinc[i]}')
                else:
                    f.write('    0')
                if i < self.DECIMATION_MAX * self.SINCN - 1:
                    f.write(',')
                if (i + 1) % 8 == 0:
                    f.write('\n')
                else:
                    f.write(' ')
            f.write('};\n\n')
            
            # Write Sinc³ coefficients: coef[SINCN][DECIMATION]
            f.write('/* Sinc³ coefficients: coef[SINCN][DECIMATION] */\n')
            f.write('const int32_t coef[3][64] = {\n')
            for s in range(self.SINCN):
                f.write(f'    {{ /* sinc filter {s} */\n')
                for i in range(self.decimation):
                    f.write(f'        {self.coef[s][i]},\n')
                f.write('    }')
                if s < self.SINCN - 1:
                    f.write(',\n')
                else:
                    f.write('\n')
            f.write('};\n\n')
            
            f.write('#endif /* LUT_PARAMS_H */\n')


def main():
    """Main entry point"""
    
    config_file = None
    output_file = 'LUT_Params.h'
    
    # Parse command line arguments
    if len(sys.argv) > 1:
        config_file = sys.argv[1]
    if len(sys.argv) > 2:
        output_file = sys.argv[2]
    
    print("OpenPDM2PCM LUT Generator")
    print("=" * 50)
    
    # Create generator with config file
    generator = OpenPDMFilterLUTGenerator(config_path=config_file)
    
    print(f"Filter Configuration (from {config_file or 'defaults'}):")
    for key, value in generator.FILTER_CONFIG.items():
        print(f"  {key}: {value}")
    
    print("\nInitializing filter and generating LUT...")
    generator.initialize_filter()
    
    print(f"Calculated constants:")
    print(f"  div_const: {generator.div_const}")
    print(f"  sub_const: {generator.sub_const}")
    
    print(f"\nGenerating {output_file}...")
    generator.generate_header_file(output_file)
    
    print(f"✓ Successfully generated {output_file}")
    print(f"  - LUT array size: 256 × {generator.decimation//8} × 3 entries")
    print(f"  - Coefficient arrays included for reference")
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
