/*
 * generate_lut.cpp - OpenPDM2PCM LUT Generator (C++ port)
 * Generates LUT_Params.h for filter configuration
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class OpenPDMFilterLUTGenerator {
private:
    json config;
    int decimation;
    int SINCN;
    int DECIMATION_MAX;
    
    std::vector<int> sinc1;
    std::vector<int> sinc2;
    std::vector<int> sinc;
    std::vector<std::vector<int>> coef;
    std::vector<std::vector<std::vector<int32_t>>> lut;
    
    uint32_t div_const;
    uint32_t sub_const;
    
    std::vector<int> convolve(const std::vector<int>& signal, int signal_len,
                              const std::vector<int>& kernel, int kernel_len) {
        int result_len = signal_len + kernel_len - 1;
        std::vector<int> result(result_len, 0);
        
        for (int n = 0; n < result_len; n++) {
            int kmin = (n >= kernel_len - 1) ? n - kernel_len + 1 : 0;
            int kmax = (n < signal_len) ? n : signal_len - 1;
            
            for (int k = kmin; k <= kmax; k++) {
                result[n] += signal[k] * kernel[n - k];
            }
        }
        
        return result;
    }
    
    void generate_lut() {
        for (int s = 0; s < SINCN; s++) {
            for (int c = 0; c < 256; c++) {
                for (int d = 0; d < decimation / 8; d++) {
                    int32_t lut_value = 0;
                    lut_value += ((c >> 7)       ) * coef[s][d * 8    ];
                    lut_value += ((c >> 6) & 0x01) * coef[s][d * 8 + 1];
                    lut_value += ((c >> 5) & 0x01) * coef[s][d * 8 + 2];
                    lut_value += ((c >> 4) & 0x01) * coef[s][d * 8 + 3];
                    lut_value += ((c >> 3) & 0x01) * coef[s][d * 8 + 4];
                    lut_value += ((c >> 2) & 0x01) * coef[s][d * 8 + 5];
                    lut_value += ((c >> 1) & 0x01) * coef[s][d * 8 + 6];
                    lut_value += ((c     ) & 0x01) * coef[s][d * 8 + 7];
                    
                    lut[c][d][s] = lut_value;
                }
            }
        }
    }
    
public:
    OpenPDMFilterLUTGenerator(const std::string& config_path) {
        std::ifstream f(config_path);
        if (!f.is_open()) {
            std::cerr << "Warning: " << config_path << " not found, using defaults\n";
            config = {
                {"LP_HZ", 10000.0},
                {"HP_HZ", 50.0},
                {"Fs", 48000},
                {"Decimation", 64},
                {"MaxVolume", 16},
                {"Gain", 1},
                {"SINCN", 3},
                {"DECIMATION_MAX", 128}
            };
        } else {
            try {
                f >> config;
            } catch (const std::exception& e) {
                std::cerr << "Warning: Failed to parse " << config_path << ": " << e.what() << "\n";
                config = {
                    {"LP_HZ", 10000.0},
                    {"HP_HZ", 50.0},
                    {"Fs", 48000},
                    {"Decimation", 64},
                    {"MaxVolume", 16},
                    {"Gain", 1},
                    {"SINCN", 3},
                    {"DECIMATION_MAX", 128}
                };
            }
            f.close();
        }
        
        // Extract audio filter config or use defaults
        auto audio_filter = config.value("audio_filter", json::object());
        SINCN = audio_filter.value("sinc_order", 3);
        DECIMATION_MAX = audio_filter.value("decimation_max", 128);
        decimation = audio_filter.value("decimation_factor", 64);
        
        // Initialize arrays
        sinc1.resize(DECIMATION_MAX, 0);
        sinc2.resize(DECIMATION_MAX * 2, 0);
        sinc.resize(DECIMATION_MAX * SINCN, 0);
        coef.resize(SINCN, std::vector<int>(DECIMATION_MAX, 0));
        lut.resize(256, std::vector<std::vector<int32_t>>(DECIMATION_MAX / 8, 
                                                          std::vector<int32_t>(SINCN, 0)));
    }
    
    void initialize_filter() {
        // Initialize sinc1 array (all ones)
        for (int i = 0; i < decimation; i++) {
            sinc1[i] = 1;
        }
        
        // Calculate sinc2 = convolve(sinc1, sinc1)
        sinc2 = convolve(sinc1, decimation, sinc1, decimation);
        
        // Calculate sinc = convolve(sinc2, sinc1)
        sinc[0] = 0;
        sinc[decimation * SINCN - 1] = 0;
        auto sinc_temp = convolve(sinc2, decimation * 2 - 1, sinc1, decimation);
        for (size_t i = 1; i < (size_t)(decimation * SINCN - 1) && i - 1 < sinc_temp.size(); i++) {
            sinc[i] = sinc_temp[i - 1];
        }
        
        // Build coefficient array
        uint32_t total_sum = 0;
        for (int j = 0; j < SINCN; j++) {
            for (int i = 0; i < decimation; i++) {
                coef[j][i] = sinc[j * decimation + i];
                total_sum += sinc[j * decimation + i];
            }
        }
        
        // Calculate constants
        sub_const = total_sum >> 1;
        int filter_gain = 16;
        div_const = (sub_const * (int)config.value("audio_filter", json::object()).value("max_volume", 16) / 
                    32768 / filter_gain);
        if (div_const == 0) div_const = 1;
        
        generate_lut();
    }
    
    bool write_header_file(const std::string& output_path) {
        std::ofstream out(output_path);
        if (!out.is_open()) {
            std::cerr << "Error: Cannot open " << output_path << " for writing\n";
            return false;
        }
        
        auto audio_filter = config.value("audio_filter", json::object());
        
        out << "/*\n";
        out << " * LUT_Params.h - Auto-generated OpenPDM2PCM LUT values\n";
        out << " * Generated by: generate_lut\n";
        out << " *\n";
        out << " * Filter Configuration:\n";
        out << " *   Decimation: " << decimation << "\n";
        out << " *   Output Sample Rate: " << (int)audio_filter.value("sample_rate_hz", 48000) << " Hz\n";
        out << " *   LP Filter: " << (double)audio_filter.value("lowpass_hz", 10000.0) << " Hz\n";
        out << " *   HP Filter: " << (double)audio_filter.value("highpass_hz", 50.0) << " Hz\n";
        out << " *   Max Volume: " << (int)audio_filter.value("max_volume", 16) << "\n";
        out << " */\n\n";
        
        out << "#ifndef LUT_PARAMS_H\n";
        out << "#define LUT_PARAMS_H\n\n";
        out << "#include <stdint.h>\n\n";
        
        out << "/* Filter Constants */\n";
        out << "#define LUT_DECIMATION " << decimation << "\n";
        out << "#define LUT_SINCN " << SINCN << "\n";
        out << "#define LUT_DIV_CONST " << div_const << "\n";
        out << "#define LUT_SUB_CONST " << sub_const << "LL\n\n";
        
        out << "/* 3D LUT array: lut[256][DECIMATION/8][SINCN] */\n";
        out << "const int32_t lut[256][" << (decimation / 8) << "][" << SINCN << "] = {\n";
        
        for (int c = 0; c < 256; c++) {
            out << "    { /* byte value 0x" << std::hex << (c < 16 ? "0" : "") << c << std::dec << " */\n";
            for (int d = 0; d < decimation / 8; d++) {
                out << "        { ";
                for (int s = 0; s < SINCN; s++) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%10d", lut[c][d][s]);
                    out << buf;
                    if (s < SINCN - 1) out << ", ";
                }
                out << " },\n";
            }
            out << "    }";
            if (c < 255) out << ",";
            out << "\n";
        }
        
        out << "};\n\n";
        
        out << "#endif /* LUT_PARAMS_H */\n";
        out.close();
        return true;
    }
};

int main(int argc, char* argv[]) {
    std::string config_path = "configuration.json";
    std::string output_path = "generated/LUT_Params.h";
    
    if (argc > 1) {
        config_path = argv[1];
    }
    if (argc > 2) {
        output_path = argv[2];
    }
    
    OpenPDMFilterLUTGenerator gen(config_path);
    gen.initialize_filter();
    
    if (!gen.write_header_file(output_path)) {
        return 1;
    }
    
    std::cout << "Generated " << output_path << " from " << config_path << "\n";
    return 0;
}
