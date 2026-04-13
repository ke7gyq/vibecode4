/**
 * UDP Audio Client for VibeCode4
 *
 * Receives audio from Raspberry Pi Pico via UDP and saves to raw audio file or analyzes tone.
 * Handles sequence numbers and reports packet loss statistics.
 *
 * Usage:
 *     ./udp_audio_client --host 192.168.12.200 --port 5001 --duration 30 --output audio.raw
 *     ./udp_audio_client --host 192.168.12.200 --port 5001 --duration 10 --analyze-tone
 *
 * Compile: g++ -std=c++17 -O2 -lm -o udp_audio_client udp_audio_client.cpp
 */

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>

static bool should_exit = false;

void signal_handler(int) {
    should_exit = true;
}

/**
 * Extract the sequence number from the first 4 bytes (little-endian)
 */
uint32_t extract_sequence(const uint8_t* data, size_t len) {
    if (len < 4) {
        return 0;
    }
    return (static_cast<uint32_t>(data[0]) |
            (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[3]) << 24));
}

/**
 * Analyze audio buffer for tone frequency (simple energy in frequency bands)
 */
void analyze_tone(const int16_t* samples, int count, int sample_rate) {
    // Simple tone detection: compute RMS and report
    int64_t sum_sq = 0;
    for (int i = 0; i < count; i++) {
        sum_sq += (int64_t)samples[i] * samples[i];
    }
    double rms = std::sqrt((double)sum_sq / count);
    
    // Peak detection
    int16_t peak = 0;
    for (int i = 0; i < count; i++) {
        if (std::abs(samples[i]) > peak) {
            peak = std::abs(samples[i]);
        }
    }
    
    std::cout << "  [AUDIO] RMS: " << std::fixed << std::setprecision(1) 
              << rms << ", Peak: " << peak << std::endl;
}

void receive_audio(const std::string& host, int port, int duration, 
                   const std::string& output_file, bool analyze_tone_flag) {
    std::cout << "[INFO] UDP Audio Client" << std::endl;
    std::cout << "[INFO] Target: " << host << ":" << port << std::endl;
    std::cout << "[INFO] Duration: " << duration << " seconds" << std::endl;
    if (!output_file.empty()) {
        std::cout << "[INFO] Output file: " << output_file << std::endl;
    }
    if (analyze_tone_flag) {
        std::cout << "[INFO] Analyzing tone characteristics" << std::endl;
    }
    std::cout << std::endl;

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "[ERROR] Failed to create socket: " << strerror(errno) << std::endl;
        return;
    }

    // Set socket options
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Increase receive buffer
    int rcvbuf_size = 16 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));

    // Bind to listen on any interface
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Failed to bind socket: " << strerror(errno) << std::endl;
        close(sock);
        return;
    }

    // Set socket timeout for responsive exit
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Send registration packet
    struct sockaddr_in pico_addr;
    pico_addr.sin_family = AF_INET;
    pico_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &pico_addr.sin_addr);

    if (sendto(sock, "HELLO", 5, 0, (struct sockaddr*)&pico_addr, sizeof(pico_addr)) < 0) {
        std::cerr << "[WARNING] Failed to send registration packet: " << strerror(errno) << std::endl;
    } else {
        std::cout << "[INFO] Registration packet sent to " << host << ":" << port << std::endl;
    }

    // Setup signal handler
    signal(SIGINT, signal_handler);

    // Open output file if specified
    std::ofstream outfile;
    if (!output_file.empty()) {
        outfile.open(output_file, std::ios::binary);
        if (!outfile) {
            std::cerr << "[ERROR] Failed to open output file: " << output_file << std::endl;
            close(sock);
            return;
        }
    }

    // Statistics
    auto start_time = std::chrono::steady_clock::now();
    uint32_t total_frames = 0;
    uint32_t lost_frames = 0;
    uint32_t last_seq = 0;
    bool first_packet = true;
    auto last_stats_time = start_time;
    
    const int FRAME_SIZE = 528;  // samples per frame
    const int FRAME_BYTES = FRAME_SIZE * 2;

    std::cout << "[INFO] Listening for audio packets..." << std::endl << std::endl;

    uint8_t buffer[65536];
    struct sockaddr_in src_addr;
    socklen_t src_addr_len = sizeof(src_addr);

    while (!should_exit) {
        // Check duration
        auto now = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(now - start_time).count();
        if (elapsed_sec > duration) {
            std::cout << "[INFO] Duration limit reached" << std::endl;
            break;
        }

        // Receive packet
        int n = recvfrom(sock, buffer, sizeof(buffer), 0, 
                        (struct sockaddr*)&src_addr, &src_addr_len);
        
        if (n < 0) {
            // Timeout - just continue
            continue;
        }

        if (n < 4 + FRAME_BYTES) {
            continue;  // Invalid packet size
        }

        // Extract sequence and audio
        uint32_t seq = extract_sequence(buffer, n);
        int16_t* audio_samples = (int16_t*)(buffer + 4);

        // Detect frame loss
        if (!first_packet) {
            uint32_t gap = seq - last_seq - 1;
            if (gap > 0) {
                lost_frames += gap;
                std::cout << "[WARNING] Lost " << gap << " frames (seq " 
                         << last_seq << " -> " << seq << ")" << std::endl;
            }
        }

        first_packet = false;
        last_seq = seq;
        total_frames++;

        // Write audio data to file
        if (outfile) {
            outfile.write((char*)audio_samples, FRAME_BYTES);
        }

        // Analyze tone if requested
        if (analyze_tone_flag && total_frames % 10 == 0) {
            analyze_tone(audio_samples, FRAME_SIZE, 48000);
        }

        // Print statistics every second
        now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_stats_time).count() >= 1.0) {
            double total_frames_expected = (lost_frames + total_frames);
            double loss_pct = total_frames_expected > 0 ? 
                (100.0 * lost_frames / total_frames_expected) : 0.0;
            
            std::cout << "[STATS] Frames: " << total_frames 
                     << ", Lost: " << lost_frames 
                     << ", Loss: " << std::fixed << std::setprecision(2) << loss_pct << "%"
                     << ", Elapsed: " << std::setprecision(1) << elapsed_sec << "s" << std::endl;
            
            last_stats_time = now;
        }
    }

    // Final statistics
    std::cout << std::endl << "======================================" << std::endl;
    auto total_time = std::chrono::steady_clock::now() - start_time;
    double elapsed = std::chrono::duration<double>(total_time).count();
    
    double total_frames_expected = lost_frames + total_frames;
    double loss_pct = total_frames_expected > 0 ? 
        (100.0 * lost_frames / total_frames_expected) : 0.0;
    
    std::cout << "[FINAL STATS]" << std::endl;
    std::cout << "  Duration: " << std::fixed << std::setprecision(2) << elapsed << " seconds" << std::endl;
    std::cout << "  Frames received: " << total_frames << std::endl;
    std::cout << "  Frames lost: " << lost_frames << std::endl;
    std::cout << "  Total frames: " << (uint32_t)total_frames_expected << std::endl;
    std::cout << "  Loss rate: " << std::setprecision(2) << loss_pct << "%" << std::endl;
    
    if (!output_file.empty()) {
        std::cout << "  Audio saved to: " << output_file << std::endl;
    }

    if (outfile) {
        outfile.close();
    }

    close(sock);
}

int main(int argc, char* argv[]) {
    std::string host = "192.168.12.200";
    int port = 5001;
    int duration = 15;
    std::string output_file;
    bool analyze_tone_flag = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration = std::stoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--analyze-tone") {
            analyze_tone_flag = true;
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "  --host <ip>         Pico IP address (default: 192.168.12.200)" << std::endl;
            std::cout << "  --port <port>       UDP port (default: 5001)" << std::endl;
            std::cout << "  --duration <sec>    Capture duration in seconds (default: 15)" << std::endl;
            std::cout << "  --output <file>     Save audio as raw PCM (default: none)" << std::endl;
            std::cout << "  --analyze-tone      Analyze tone characteristics" << std::endl;
            return 0;
        }
    }

    receive_audio(host, port, duration, output_file, analyze_tone_flag);
    return 0;
}
