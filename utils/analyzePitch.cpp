/*
 * analyzePitch.cpp - Audio frequency analyzer (C++ port)
 * Analyzes WAV files to detect dominant frequency using FFT
 */

#include <iostream>
#include <vector>
#include <cmath>
#include <sndfile.h>
#include <complex>
#include <algorithm>
#include <iomanip>

class AudioAnalyzer {
private:
    std::vector<float> samples;
    int sample_rate;
    int num_frames;
    int num_channels;
    
    // Simple FFT using Cooley-Tukey algorithm
    void fft(std::vector<std::complex<float>>& x) {
        int N = x.size();
        if (N <= 1) return;
        
        // Divide: separate even and odd indices
        std::vector<std::complex<float>> even(N / 2), odd(N / 2);
        for (int k = 0; k < N / 2; k++) {
            even[k] = x[2 * k];
            odd[k] = x[2 * k + 1];
        }
        
        // Conquer: recursively apply FFT
        fft(even);
        fft(odd);
        
        // Combine: update x with FFT results
        for (int k = 0; k < N / 2; k++) {
            float angle = -2.0f * (float)M_PI * k / (float)N;
            std::complex<float> t = std::polar(1.0f, angle) * odd[k];
            x[k] = even[k] + t;
            x[k + N / 2] = even[k] - t;
        }
    }
    
    // Hann window function
    std::vector<float> hann_window(int size) {
        std::vector<float> window(size);
        for (int i = 0; i < size; i++) {
            window[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (size - 1)));
        }
        return window;
    }
    
public:
    bool load_wav(const std::string& filename) {
        SF_INFO sf_info = {};
        SNDFILE* infile = sf_open(filename.c_str(), SFM_READ, &sf_info);
        
        if (!infile) {
            std::cerr << "Error: Cannot open " << filename << " (" << sf_strerror(NULL) << ")\n";
            return false;
        }
        
        sample_rate = sf_info.samplerate;
        num_frames = sf_info.frames;
        num_channels = sf_info.channels;
        
        std::cout << "Audio file: " << filename << "\n";
        std::cout << "  Channels: " << num_channels << "\n";
        std::cout << "  Sample rate: " << sample_rate << " Hz\n";
        std::cout << "  Duration: " << std::fixed << std::setprecision(2) 
                  << (float)num_frames / sample_rate << " seconds\n";
        std::cout << "  Frames: " << num_frames << "\n";
        
        // Read all frames
        std::vector<float> buffer(num_frames * num_channels);
        sf_count_t num_read = sf_readf_float(infile, buffer.data(), num_frames);
        
        if (num_read != num_frames) {
            std::cerr << "Warning: Read " << num_read << " of " << num_frames << " frames\n";
        }
        
        sf_close(infile);
        
        // Extract first channel if stereo
        samples.clear();
        if (num_channels == 1) {
            samples = buffer;
        } else if (num_channels == 2) {
            for (size_t i = 0; i < buffer.size(); i += 2) {
                samples.push_back(buffer[i]);
            }
        } else {
            std::cerr << "Error: Only mono and stereo supported\n";
            return false;
        }
        
        return true;
    }
    
    float analyze_frequency() {
        if (samples.empty()) {
            std::cerr << "Error: No audio data loaded\n";
            return -1;
        }
        
        // Skip first and last 10% to avoid edge artifacts
        int skip_start = (int)(samples.size() * 0.1f);
        int skip_end = (int)(samples.size() * 0.9f);
        int analyze_len = skip_end - skip_start;
        
        // Make analyze_len a power of 2 for FFT
        int fft_size = 1;
        while (fft_size < analyze_len) fft_size *= 2;
        
        // Extract window and apply Hann windowing
        std::vector<std::complex<float>> windowed(fft_size);
        auto hann = hann_window(analyze_len);
        
        for (int i = 0; i < analyze_len; i++) {
            float sample = samples[skip_start + i] * hann[i];
            windowed[i] = std::complex<float>(sample, 0.0f);
        }
        
        // Pad with zeros
        for (int i = analyze_len; i < fft_size; i++) {
            windowed[i] = std::complex<float>(0.0f, 0.0f);
        }
        
        // Perform FFT
        fft(windowed);
        
        // Compute magnitudes
        std::vector<float> magnitude(fft_size / 2);
        for (int i = 1; i < fft_size / 2; i++) {
            magnitude[i] = std::abs(windowed[i]);
        }
        magnitude[0] = 0;  // Skip DC component
        
        // Find peak frequency
        int peak_idx = 1;
        float peak_mag = magnitude[1];
        for (int i = 2; i < (int)magnitude.size(); i++) {
            if (magnitude[i] > peak_mag) {
                peak_mag = magnitude[i];
                peak_idx = i;
            }
        }
        
        // Convert bin index to frequency
        float bin_width = (float)sample_rate / fft_size;
        float peak_freq = peak_idx * bin_width;
        
        std::cout << "\n" << std::string(50, '=') << "\n";
        std::cout << "Detected frequency: " << std::fixed << std::setprecision(2) << peak_freq << " Hz\n";
        std::cout << std::string(50, '=') << "\n";
        
        return peak_freq;
    }
    
    static void compare_frequencies(float measured_freq, float expected_freq = 440.0f) {
        if (measured_freq < 0) return;
        
        float ratio = measured_freq / expected_freq;
        float percent_off = (ratio - 1.0f) * 100.0f;
        
        std::cout << "\nExpected frequency: " << std::fixed << std::setprecision(2) << expected_freq << " Hz\n";
        std::cout << "Measured frequency: " << measured_freq << " Hz\n";
        std::cout << "Ratio (measured/expected): " << std::setprecision(4) << ratio << "\n";
        std::cout << "Percent difference: " << std::setprecision(2) << std::showpos << percent_off << "%\n";
        
        if (ratio < 0.98f) {
            std::cout << "\n⚠ Audio appears SLOWER/DEEPER than expected\n";
            std::cout << "  → Pico may be sampling at " << std::fixed << std::setprecision(0) 
                      << (48000.0f * ratio) << " Hz instead of 48000 Hz\n";
        } else if (ratio > 1.02f) {
            std::cout << "\n⚠ Audio appears FASTER/HIGHER than expected\n";
            std::cout << "  → Pico may be sampling at " << std::fixed << std::setprecision(0) 
                      << (48000.0f * ratio) << " Hz instead of 48000 Hz\n";
        } else {
            std::cout << "\n✓ Frequency matches expected value within 2%\n";
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <wav_file> [expected_frequency]\n";
        std::cerr << "Example: " << argv[0] << " audio_from_pico.wav 440\n";
        return 1;
    }
    
    float expected_freq = 440.0f;
    if (argc > 2) {
        expected_freq = std::atof(argv[2]);
    }
    
    AudioAnalyzer analyzer;
    if (!analyzer.load_wav(argv[1])) {
        return 1;
    }
    
    float measured_freq = analyzer.analyze_frequency();
    if (measured_freq > 0) {
        analyzer.compare_frequencies(measured_freq, expected_freq);
    }
    
    return 0;
}
