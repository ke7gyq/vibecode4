/**
 * UDP Sequence Number Analyzer for VibeCode4 Audio Streaming
 *
 * Listens to UDP audio packets and analyzes sequence number patterns to diagnose frame loss.
 * Outputs statistics on loss distribution, timing, and patterns.
 *
 * Usage:
 *     ./udp_sequence_analyzer --host 192.168.12.200 --port 5001 --duration 30
 *
 * Compile: g++ -std=c++17 -O2 -o udp_sequence_analyzer udp_sequence_analyzer.cpp
 */

#include <iostream>
#include <vector>
#include <map>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <cmath>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>

struct Gap {
    uint32_t seq_before;
    uint32_t seq_after;
    uint32_t gap_size;
    double gap_time;
};

struct SequenceEntry {
    uint32_t seq_num;
    double time;
};

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
    // Little-endian: read 4 bytes
    return (static_cast<uint32_t>(data[0]) |
            (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[3]) << 24));
}

void analyze_udp_sequences(const std::string& host, int port, int duration) {
    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "[ERROR] Failed to create socket: " << strerror(errno) << std::endl;
        return;
    }

    // Set socket options for optimal performance
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "[WARNING] Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
    }

    // Increase receive buffer size to minimize packet loss due to buffer overflow
    // Most systems: default ~107KB, we want larger to handle bursts
    int rcvbuf_size = 16 * 1024 * 1024;  // 16 MB receive buffer
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        std::cerr << "[WARNING] Could not set receive buffer to 16MB: " << strerror(errno) << std::endl;
        // Try smaller size
        rcvbuf_size = 4 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));
    }

    // Bind to port
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Failed to bind socket: " << strerror(errno) << std::endl;
        close(sock);
        return;
    }

    // Set socket timeout for non-blocking behavior
    // Use 100ms timeout: frequent enough to check duration, but not so frequent it wastes CPU
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;  // 100ms timeout instead of 2s
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << "[WARNING] Failed to set timeout: " << strerror(errno) << std::endl;
    }

    std::cout << "[INFO] Listening on 0.0.0.0:" << port << " for " << duration << " seconds" << std::endl;
    std::cout << "[INFO] Sending registration to " << host << ":" << port << "..." << std::endl;

    // Send HELLO registration packet
    struct sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host.c_str(), &dest_addr.sin_addr) <= 0) {
        std::cerr << "[ERROR] Invalid IP address: " << host << std::endl;
        close(sock);
        return;
    }

    const char* hello_msg = "HELLO\0";
    if (sendto(sock, hello_msg, 6, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) < 0) {
        std::cerr << "[WARNING] Could not send registration: " << strerror(errno) << std::endl;
    } else {
        std::cout << "[INFO] Registration sent, waiting for stream..." << std::endl;
    }

    sleep(1);  // Give RP2350 time to start sending
    std::cout << "[INFO] Extracting sequence numbers from UDP packets..." << std::endl << std::endl;

    // Setup signal handler for Ctrl+C
    signal(SIGINT, signal_handler);

    auto start = std::chrono::high_resolution_clock::now();
    uint32_t last_seq = 0;
    bool first_packet = true;
    bool printed_first_msg = false;

    // Pre-allocate containers to avoid reallocations during packet capture
    std::vector<SequenceEntry> sequences;
    sequences.reserve(100000);  // Reserve space for up to 100k packets
    
    std::vector<Gap> gaps;
    gaps.reserve(10000);  // Reserve space for up to 10k gaps
    
    std::map<uint32_t, int> gap_sizes;
    std::vector<uint32_t> current_loss_streak;
    current_loss_streak.reserve(1000);  // Reserve for consecutive losses
    
    std::vector<uint32_t> consecutive_losses;
    consecutive_losses.reserve(10000);  // Reserve for loss clusters
    
    std::vector<double> receive_times;
    receive_times.reserve(100000);  // Reserve for timestamps

    uint32_t packet_count = 0;
    uint64_t bytes_received = 0;
    uint8_t buffer[2048];

    // ===== MAIN PACKET CAPTURE LOOP (optimized for minimal overhead) =====
    while (!should_exit) {
        struct sockaddr_in src_addr;
        std::memset(&src_addr, 0, sizeof(src_addr));
        socklen_t src_len = sizeof(src_addr);

        // Receive packet
        int n = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&src_addr, &src_len);

        // Check elapsed time regularly (fixes: duration timeout was only checked on socket timeouts)
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed > duration) {
            break;  // Duration expired - exit cleanly
        }

        if (n < 0) {
            // Timeout occurred (no data)
            continue;
        }

        bytes_received += n;
        packet_count++;

        // Store timestamp for this packet
        receive_times.push_back(elapsed);

        if (n < 4) {
            std::cerr << "[ERROR] Packet too small (" << n << " bytes)" << std::endl;
            continue;
        }

        uint32_t seq_num = extract_sequence(buffer, n);
        sequences.push_back({seq_num, elapsed});

        if (first_packet) {
            if (!printed_first_msg) {
                std::cout << "[INFO] First packet received at t=" << std::fixed << std::setprecision(3)
                          << elapsed << "s with seq=" << seq_num << std::endl;
                printed_first_msg = true;
            }
            first_packet = false;
            last_seq = seq_num;
        } else {
            int32_t gap = static_cast<int32_t>(seq_num) - static_cast<int32_t>(last_seq) - 1;
            if (gap > 0) {
                gaps.push_back({last_seq, seq_num, static_cast<uint32_t>(gap), elapsed});
                gap_sizes[gap]++;
                current_loss_streak.push_back(gap);
            } else if (gap < 0) {
                // Sequence number wrapped or went backwards
                gaps.push_back({last_seq, seq_num, static_cast<uint32_t>(gap), elapsed});
                gap_sizes[-gap]++;
                current_loss_streak.push_back(-gap);
            } else {
                if (!current_loss_streak.empty()) {
                    uint32_t streak_total = 0;
                    for (auto val : current_loss_streak) streak_total += val;
                    consecutive_losses.push_back(streak_total);
                    current_loss_streak.clear();
                }
            }
            last_seq = seq_num;
        }
    }

    close(sock);

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();

    // =====================
    // Analysis and Reporting
    // =====================
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "SEQUENCE NUMBER ANALYSIS RESULTS" << std::endl;
    std::cout << std::string(70, '=') << std::endl;

    uint32_t total_seq_span = 0;
    uint32_t total_expected_frames = 0;
    uint32_t total_received_frames = sequences.size();
    uint32_t total_lost_frames = 0;

    if (!sequences.empty()) {
        total_seq_span = sequences.back().seq_num - sequences.front().seq_num + 1;
        total_expected_frames = total_seq_span;
        for (const auto& gap : gaps) {
            total_lost_frames += gap.gap_size;
        }
    }

    std::cout << "\n[SUMMARY]" << std::endl;
    std::cout << "  Duration:           " << std::fixed << std::setprecision(2) << elapsed << "s" << std::endl;
    std::cout << "  Packets received:   " << packet_count << std::endl;
    std::cout << "  Bytes received:     " << bytes_received << std::endl;
    std::cout << "  Avg packet rate:    " << std::fixed << std::setprecision(1)
              << (elapsed > 0 ? packet_count / elapsed : 0) << " pkt/s" << std::endl;
    std::cout << "  Avg bandwidth:      " << std::fixed << std::setprecision(1)
              << (elapsed > 0 ? bytes_received / elapsed / 1024.0 : 0) << " KB/s" << std::endl;

    std::cout << "\n[SEQUENCE COVERAGE]" << std::endl;
    if (!sequences.empty()) {
        std::cout << "  First sequence:     " << sequences.front().seq_num << std::endl;
        std::cout << "  Last sequence:      " << sequences.back().seq_num << std::endl;
    } else {
        std::cout << "  First sequence:     N/A" << std::endl;
        std::cout << "  Last sequence:      N/A" << std::endl;
    }
    std::cout << "  Total span:         " << total_seq_span << " frames" << std::endl;
    std::cout << "  Frames received:    " << total_received_frames << std::endl;
    std::cout << "  Frames lost:        " << total_lost_frames << std::endl;

    if (total_expected_frames > 0) {
        double loss_rate = 100.0 * total_lost_frames / total_expected_frames;
        std::cout << "  Loss rate:          " << std::fixed << std::setprecision(2) << loss_rate << "%" << std::endl;
    } else {
        std::cout << "  Loss rate:          N/A (no data received)" << std::endl;
    }

    if (!gaps.empty()) {
        std::cout << "\n[GAP STATISTICS]" << std::endl;
        uint32_t min_gap = UINT32_MAX;
        uint32_t max_gap = 0;
        for (const auto& gap : gaps) {
            min_gap = std::min(min_gap, gap.gap_size);
            max_gap = std::max(max_gap, gap.gap_size);
        }

        std::cout << "  Total gap events:   " << gaps.size() << std::endl;
        std::cout << "  Average gap size:   " << std::fixed << std::setprecision(2)
                  << (gaps.empty() ? 0.0 : static_cast<double>(total_lost_frames) / gaps.size())
                  << " frames" << std::endl;
        std::cout << "  Min gap size:       " << (min_gap == UINT32_MAX ? 0 : min_gap) << " frames" << std::endl;
        std::cout << "  Max gap size:       " << max_gap << " frames" << std::endl;

        std::cout << "\n[GAP DISTRIBUTION]" << std::endl;
        for (const auto& [size, count] : gap_sizes) {
            double pct = 100.0 * count / gaps.size();
            int bar_len = std::min(40, static_cast<int>(pct / 2.5));
            std::string bar(bar_len, '#');  // Progress bar character

            std::cout << "  Loss of " << std::setw(2) << size << " frame(s): " << std::setw(3) << count
                      << " events (" << std::fixed << std::setprecision(1) << std::setw(5) << pct << "%) "
                      << bar << std::endl;
        }

        std::cout << "\n[LOSS CLUSTERING]" << std::endl;
        if (!consecutive_losses.empty()) {
            uint32_t total_consecutive = 0;
            for (auto loss : consecutive_losses) {
                total_consecutive += loss;
            }
            uint32_t max_cluster = *std::max_element(consecutive_losses.begin(), consecutive_losses.end());

            std::cout << "  Consecutive loss events:  " << consecutive_losses.size() << std::endl;
            std::cout << "  Total frames in clusters: " << total_consecutive << std::endl;
            std::cout << "  Avg frames per cluster:   " << std::fixed << std::setprecision(2)
                      << static_cast<double>(total_consecutive) / consecutive_losses.size() << std::endl;
            std::cout << "  Max frames in one cluster:" << max_cluster << std::endl;
        } else {
            std::cout << "  No consecutive loss events" << std::endl;
        }

        std::cout << "\n[TEMPORAL ANALYSIS]" << std::endl;
        if (!receive_times.empty()) {
            std::vector<double> inter_arrivals;
            for (size_t i = 0; i < receive_times.size() - 1; i++) {
                inter_arrivals.push_back(receive_times[i + 1] - receive_times[i]);
            }

            double avg_interval = 0;
            for (auto interval : inter_arrivals) {
                avg_interval += interval;
            }
            avg_interval /= inter_arrivals.size();

            double min_interval = *std::min_element(inter_arrivals.begin(), inter_arrivals.end());
            double max_interval = *std::max_element(inter_arrivals.begin(), inter_arrivals.end());

            double expected_frame_time = 1.0 / 46.0;  // ~46 kHz frame rate

            std::cout << "  Expected frame interval: " << std::fixed << std::setprecision(2)
                      << (expected_frame_time * 1000) << " ms" << std::endl;
            std::cout << "  Avg packet interval:     " << std::fixed << std::setprecision(2)
                      << (avg_interval * 1000) << " ms" << std::endl;
            std::cout << "  Min packet interval:     " << std::fixed << std::setprecision(2)
                      << (min_interval * 1000) << " ms" << std::endl;
            std::cout << "  Max packet interval:     " << std::fixed << std::setprecision(2)
                      << (max_interval * 1000) << " ms" << std::endl;

            // Detect stalls (gap > 3x expected interval)
            double stall_threshold = expected_frame_time * 3;
            int stall_count = 0;
            double max_stall = 0;
            for (auto interval : inter_arrivals) {
                if (interval > stall_threshold) {
                    stall_count++;
                    max_stall = std::max(max_stall, interval);
                }
            }

            if (stall_count > 0) {
                std::cout << "  Stall events (>" << std::fixed << std::setprecision(1)
                          << (stall_threshold * 1000) << "ms): " << stall_count << std::endl;
                std::cout << "  Max stall duration:      " << std::fixed << std::setprecision(1)
                          << (max_stall * 1000) << " ms" << std::endl;
            }
        }
    }

    std::cout << "\n[DETAILED GAP LOG (first 20)]" << std::endl;
    for (size_t i = 0; i < std::min(size_t(20), gaps.size()); i++) {
        std::cout << "  Gap " << std::setw(2) << (i + 1) << ": seq " << gaps[i].seq_before << " → "
                  << gaps[i].seq_after << " (lost " << gaps[i].gap_size << " frame(s)) at t="
                  << std::fixed << std::setprecision(3) << gaps[i].gap_time << "s" << std::endl;
    }

    if (gaps.size() > 20) {
        std::cout << "  ... and " << (gaps.size() - 20) << " more gaps" << std::endl;
    }

    std::cout << "\n" << std::string(70, '=') << std::endl;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS]" << std::endl;
    std::cout << "\nAnalyze UDP sequence numbers to diagnose frame loss patterns\n" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --host HOST       RP2350 IP address (default: 192.168.12.200)" << std::endl;
    std::cout << "  --port PORT       UDP port (default: 5001)" << std::endl;
    std::cout << "  --duration SECS   Listen duration in seconds (default: 30)" << std::endl;
    std::cout << "  --help            Show this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string host = "192.168.12.200";
    int port = 5001;
    int duration = 30;

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "[ERROR] Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    try {
        analyze_udp_sequences(host, port, duration);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
