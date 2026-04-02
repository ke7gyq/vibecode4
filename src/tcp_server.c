#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <lwip/tcp.h>
#include <lwip/err.h>
#include <pico/time.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tcp_server.h"
#include "microphone.h"

#define TCP_SERVER_PORT_HELLO 5000
#define TCP_SERVER_PORT_AUDIO 5001
#define MAX_CLIENTS 4

// Shared ring buffer for audio data (single source, multiple readers)
// Size: 16× AUDIO_BUFFER_SIZE = ~256ms at 48kHz (plenty of headroom for TCP and poll mismatches)
#define AUDIO_RING_BUFFER_SIZE (AUDIO_BUFFER_SIZE * 16)
typedef struct {
    int16_t data[AUDIO_RING_BUFFER_SIZE];
    uint32_t write_index;  // Write pointer (in samples)
    uint32_t read_index;   // Read pointer (in samples)
} audio_ring_buffer_t;

// Shared buffer written by microphone, read by all clients
static audio_ring_buffer_t g_shared_audio_buffer = {0};

// Server state
static struct tcp_pcb *server_pcb_hello = NULL;
static struct tcp_pcb *server_pcb_audio = NULL;
static int g_server_running = 0;

// Client session data - hello port
typedef struct {
    uint32_t count;
    int active;
} client_session_t;

static client_session_t g_clients[MAX_CLIENTS];
static int g_next_client = 0;

// Client session data - audio port
typedef struct {
    struct tcp_pcb *pcb;              // TCP connection for this audio client
    uint32_t read_index;              // Per-client read position in shared buffer
    int active;
    uint32_t samples_sent;            // Statistics: total samples sent
    uint32_t last_stats_time;         // Statistics: time of last report
} audio_client_session_t;

static audio_client_session_t g_audio_clients[MAX_CLIENTS];
static int g_next_audio_client = 0;

/**
 * Ring buffer: Enqueue audio samples
 * Returns number of samples actually enqueued (may be less if buffer full)
 */
static uint32_t audio_ring_buffer_write(audio_ring_buffer_t *rb, const int16_t *data, uint32_t count) {
    if (count == 0) return 0;
    
    // Calculate available space in ring buffer
    uint32_t write_idx = rb->write_index % AUDIO_RING_BUFFER_SIZE;
    uint32_t read_idx = rb->read_index % AUDIO_RING_BUFFER_SIZE;
    uint32_t available;
    
    if (write_idx >= read_idx) {
        available = AUDIO_RING_BUFFER_SIZE - (write_idx - read_idx) - 1;
    } else {
        available = read_idx - write_idx - 1;
    }
    
    if (available == 0) return 0;  // Buffer full
    
    uint32_t to_write = (count > available) ? available : count;
    
    // Write data, wrapping around if needed
    uint32_t space_to_end = AUDIO_RING_BUFFER_SIZE - write_idx;
    
    if (to_write <= space_to_end) {
        memcpy(&rb->data[write_idx], data, to_write * sizeof(int16_t));
    } else {
        uint32_t first_part = space_to_end;
        memcpy(&rb->data[write_idx], data, first_part * sizeof(int16_t));
        memcpy(&rb->data[0], &data[first_part], (to_write - first_part) * sizeof(int16_t));
    }
    
    rb->write_index += to_write;
    return to_write;
}

/**
 * Ring buffer: Dequeue audio samples (shared buffer version)
 * Each client maintains its own read position
 * Returns number of samples actually dequeued
 */
static uint32_t audio_ring_buffer_read(audio_ring_buffer_t *rb, uint32_t *client_read_index, int16_t *data, uint32_t count) {
    if (count == 0) return 0;
    
    // Calculate available data from client's read position to writer's write position
    uint32_t write_idx = rb->write_index % AUDIO_RING_BUFFER_SIZE;
    uint32_t read_idx = *client_read_index % AUDIO_RING_BUFFER_SIZE;
    uint32_t available;
    
    if (write_idx >= read_idx) {
        available = write_idx - read_idx;
    } else {
        available = AUDIO_RING_BUFFER_SIZE - (read_idx - write_idx);
    }
    
    if (available == 0) return 0;  // Buffer empty
    
    uint32_t to_read = (count > available) ? available : count;
    
    // Read data, wrapping around if needed
    uint32_t space_to_end = AUDIO_RING_BUFFER_SIZE - read_idx;
    
    if (to_read <= space_to_end) {
        memcpy(data, &rb->data[read_idx], to_read * sizeof(int16_t));
    } else {
        uint32_t first_part = space_to_end;
        memcpy(data, &rb->data[read_idx], first_part * sizeof(int16_t));
        memcpy(&data[first_part], &rb->data[0], (to_read - first_part) * sizeof(int16_t));
    }
    
    *client_read_index += to_read;
    return to_read;
}

/**
 * Poll callback - sends audio data in chunks
 * Only sends what fits in the TCP send buffer
 */
static err_t audio_client_poll_cb(void *arg, struct tcp_pcb *pcb) {
    if (arg == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    
    audio_client_session_t *session = (audio_client_session_t *)arg;
    
    if (!session->active) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    
    // Try to send audio data from shared ring buffer
    // Local buffer: 512 samples = 1KB (safe for embedded stacks, won't overflow)
    int16_t frame_buffer[512];
    
    uint32_t samples_to_read = 512;
    uint32_t samples_available = audio_ring_buffer_read(&g_shared_audio_buffer, &session->read_index, frame_buffer, samples_to_read);
    
    // Debug: track empty polls
    static uint32_t empty_poll_count = 0;
    if (samples_available == 0) {
        empty_poll_count++;
        if (g_micDebug >= 4 && empty_poll_count % 500 == 0) {  // Print every 500 empty polls (~0.5 sec)
            printf("[AudioPoll] Empty: %lu, SharedBuf write=%lu, client read=%lu\n", 
                   empty_poll_count, g_shared_audio_buffer.write_index, session->read_index);
        }
        return ERR_OK;  // No data ready
    }
    
    // Data available - reset counter
    empty_poll_count = 0;
    
    // Check available space in TCP send buffer
    uint16_t tcp_available = tcp_sndbuf(pcb);
    uint32_t bytes_to_send = samples_available * sizeof(int16_t);
    
    if (tcp_available < bytes_to_send) {
        // Can't fit in send buffer - put data back in ring buffer!
        // This is a bit of a hack, but we need to avoid dropping audio
        // Instead, just don't send this time
        return ERR_OK;
    }
    
    // Send the frame
    err_t err = tcp_write(pcb, frame_buffer, bytes_to_send, TCP_WRITE_FLAG_COPY);
    
    if (err != ERR_OK && err != ERR_MEM) {
        printf("Audio: tcp_write failed (err=%d), closing\n", err);
        session->active = 0;
        return ERR_OK;
    }
    
    // If we got ERR_MEM, we couldn't write - this is OK, we'll try again on next poll
    if (err == ERR_MEM) {
        return ERR_OK;
    }
    
    // Track statistics
    session->samples_sent += samples_available;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    if (current_time - session->last_stats_time >= 1000) {
        uint32_t ms_elapsed = current_time - session->last_stats_time;
        float sample_rate = (session->samples_sent * 1000.0f) / (float)ms_elapsed;
        if (g_micDebug >= 4) {
            printf("[AudioSend] Rate: %.0f Hz, Total sent: %lu samples\n", 
                   sample_rate, session->samples_sent);
        }
        session->samples_sent = 0;
        session->last_stats_time = current_time;
    }
    
    // Flush output
    tcp_output(pcb);
    
    return ERR_OK;
}

// Note: audio_stream_task removed - now using single shared buffer updated directly by microphone ISR

/**
 * Error callback for audio clients
 */
static void audio_client_err_cb(void *arg, err_t err) {
    if (arg != NULL) {
        audio_client_session_t *session = (audio_client_session_t *)arg;
        session->active = 0;
        printf("Audio Client error: %d\n", err);
    }
}

/**
 * Receive callback for audio clients (discard any input)
 */
static err_t audio_client_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (arg == NULL) {
        if (p != NULL) pbuf_free(p);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    
    audio_client_session_t *session = (audio_client_session_t *)arg;
    
    if (err != ERR_OK) {
        if (p != NULL) pbuf_free(p);
        session->active = 0;
        tcp_close(pcb);  // Properly close connection
        return ERR_OK;
    }
    
    if (p == NULL) {
        // Connection closed by peer
        session->active = 0;
        printf("Audio client disconnected\n");
        tcp_close(pcb);  // Properly close connection
        return ERR_OK;
    }
    
    // Discard any received data
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    
    return ERR_OK;
}

/**
 * Accept callback for audio port (5001)
 */
static err_t audio_server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) {
        printf("Audio: Accept error: %d\n", err);
        return ERR_ARG;
    }
    
    printf("Audio client connected\n");
    
    // Find an available session slot (no task cleanup needed anymore)
    audio_client_session_t *session = &g_audio_clients[g_next_audio_client % MAX_CLIENTS];
    
    // If this slot was previously active, mark it inactive
    if (session->active) {
        printf("Audio: Reusing session slot\n");
    }
    
    g_next_audio_client++;
    
    // Fully reset the session
    session->pcb = newpcb;
    session->active = 1;
    session->samples_sent = 0;
    session->last_stats_time = to_ms_since_boot(get_absolute_time());
    
    // Initialize client read index to slightly behind current write position
    // This ensures there's data available to read immediately (within one buffer cycle)
    // Without this, client read_index == write_index, and both advance together with no data
    if (g_shared_audio_buffer.write_index >= AUDIO_BUFFER_SIZE) {
        session->read_index = g_shared_audio_buffer.write_index - AUDIO_BUFFER_SIZE;
    } else {
        session->read_index = g_shared_audio_buffer.write_index;  // First few buffers, no history
    }
    
    // Set callbacks
    tcp_arg(newpcb, session);
    tcp_err(newpcb, audio_client_err_cb);
    tcp_recv(newpcb, audio_client_recv_cb);
    tcp_poll(newpcb, audio_client_poll_cb, 1);  // Poll every tick for minimum latency
    
    printf("Audio: Streaming started for client (read_index=%lu)\n", session->read_index);
    return ERR_OK;
}

/**
 * Send hello message to a client
 */
static err_t send_hello(struct tcp_pcb *pcb, uint32_t count) {
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "Hello %u\n", count);
    
    if (tcp_write(pcb, msg, len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        printf("tcp_write failed\n");
        return ERR_BUF;
    }
    
    if (tcp_output(pcb) != ERR_OK) {
        printf("tcp_output failed\n");
        return ERR_ABRT;
    }
    
    return ERR_OK;
}

/**
 * Poll callback - sends next hello message
 */
static err_t client_poll_cb(void *arg, struct tcp_pcb *pcb) {
    if (arg == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    
    client_session_t *session = (client_session_t *)arg;
    
    if (!session->active) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    
    // Send next hello message
    err_t err = send_hello(pcb, session->count);
    if (err == ERR_OK) {
        session->count++;
    }
    
    return err;
}

/**
 * Error callback
 */
static void client_err_cb(void *arg, err_t err) {
    if (arg != NULL) {
        client_session_t *session = (client_session_t *)arg;
        session->active = 0;
        printf("Client error: %d\n", err);
    }
}

/**
 * Receive callback - read and discard data
 */
static err_t client_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (arg == NULL) {
        if (p != NULL) pbuf_free(p);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    
    client_session_t *session = (client_session_t *)arg;
    
    if (err != ERR_OK) {
        if (p != NULL) pbuf_free(p);
        session->active = 0;
        tcp_close(pcb);  // Properly close connection
        return ERR_OK;
    }
    
    if (p == NULL) {
        // Connection closed by peer
        session->active = 0;
        printf("Client disconnected\n");
        tcp_close(pcb);  // Properly close connection
        return ERR_OK;
    }
    
    // Just discard the received data (echo not needed, just server->client streaming)
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    
    return ERR_OK;
}

/**
 * Accept callback - handle new connections
 */
static err_t server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) {
        printf("Accept error: %d\n", err);
        return ERR_ARG;
    }
    
    printf("New client connection\n");
    
    // Create session for this client
    client_session_t *session = &g_clients[g_next_client % MAX_CLIENTS];
    g_next_client++;
    
    session->count = 0;
    session->active = 1;
    
    // Set callbacks
    tcp_arg(newpcb, session);
    tcp_err(newpcb, client_err_cb);
    tcp_recv(newpcb, client_recv_cb);
    tcp_poll(newpcb, client_poll_cb, 10);  // Poll every ~2 seconds
    
    // Send initial hello
    send_hello(newpcb, 0);
    session->count = 1;
    
    return ERR_OK;
}

/**
 * Start the TCP server
 */
int tcp_server_start(void) {
    if (g_server_running) {
        printf("TCP Server: Already running\n");
        return -1;
    }
    
    printf("TCP Server: Starting on ports %d (hello) and %d (audio)...\n", 
           TCP_SERVER_PORT_HELLO, TCP_SERVER_PORT_AUDIO);
    
    // Initialize client sessions for hello
    memset(g_clients, 0, sizeof(g_clients));
    g_next_client = 0;
    
    // Initialize audio client sessions
    memset(g_audio_clients, 0, sizeof(g_audio_clients));
    g_next_audio_client = 0;
    
    // Initialize shared audio buffer
    memset(&g_shared_audio_buffer, 0, sizeof(g_shared_audio_buffer));
    g_shared_audio_buffer.write_index = 0;
    
    // ===== Setup Hello Server (Port 5000) =====
    server_pcb_hello = tcp_new();
    if (server_pcb_hello == NULL) {
        printf("TCP Server: Failed to create hello PCB\n");
        return -1;
    }
    
    err_t err = tcp_bind(server_pcb_hello, IP_ADDR_ANY, TCP_SERVER_PORT_HELLO);
    if (err != ERR_OK) {
        printf("TCP Server: Failed to bind hello port (error: %d)\n", err);
        tcp_close(server_pcb_hello);
        server_pcb_hello = NULL;
        return -1;
    }
    
    server_pcb_hello = tcp_listen(server_pcb_hello);
    if (server_pcb_hello == NULL) {
        printf("TCP Server: Failed to listen on hello port\n");
        return -1;
    }
    
    tcp_accept(server_pcb_hello, server_accept_cb);
    printf("TCP Server: Listening for hello on port %d\n", TCP_SERVER_PORT_HELLO);
    
    // ===== Setup Audio Server (Port 5001) =====
    server_pcb_audio = tcp_new();
    if (server_pcb_audio == NULL) {
        printf("TCP Server: Failed to create audio PCB\n");
        tcp_close(server_pcb_hello);
        server_pcb_hello = NULL;
        return -1;
    }
    
    err = tcp_bind(server_pcb_audio, IP_ADDR_ANY, TCP_SERVER_PORT_AUDIO);
    if (err != ERR_OK) {
        printf("TCP Server: Failed to bind audio port (error: %d)\n", err);
        tcp_close(server_pcb_hello);
        server_pcb_hello = NULL;
        tcp_close(server_pcb_audio);
        server_pcb_audio = NULL;
        return -1;
    }
    
    server_pcb_audio = tcp_listen(server_pcb_audio);
    if (server_pcb_audio == NULL) {
        printf("TCP Server: Failed to listen on audio port\n");
        tcp_close(server_pcb_hello);
        server_pcb_hello = NULL;
        return -1;
    }
    
    tcp_accept(server_pcb_audio, audio_server_accept_cb);
    printf("TCP Server: Listening for audio on port %d\n", TCP_SERVER_PORT_AUDIO);
    
    g_server_running = 1;
    printf("TCP Server: All ports ready\n");
    
    return 0;
}

/**
 * Stop the TCP server
 */
void tcp_server_stop(void) {
    if (!g_server_running) {
        printf("TCP Server: Not running\n");
        return;
    }
    
    printf("TCP Server: Stopping...\n");
    
    // Close hello server
    if (server_pcb_hello != NULL) {
        tcp_close(server_pcb_hello);
        server_pcb_hello = NULL;
    }
    
    // Close audio server
    if (server_pcb_audio != NULL) {
        tcp_close(server_pcb_audio);
        server_pcb_audio = NULL;
    }
    
    // Mark all hello clients as inactive
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].active = 0;
    }
    
    // Mark all audio clients as inactive
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_audio_clients[i].active = 0;
    }
    
    g_server_running = 0;
    printf("TCP Server: Stopped\n");
}

/**
 * Get server status
 */
int tcp_server_is_running(void) {
    return g_server_running;
}

/**
 * Write audio frame to shared buffer
 * Called from microphone ISR context on DMA completion
 * Poll callbacks will read this buffer at 1ms intervals
 * Returns number of samples written (may be less if buffer full)
 */
uint32_t tcp_server_write_audio_frame(const int16_t *audio_data, uint32_t sample_count) {
    static uint32_t write_count = 0;
    static uint32_t total_written = 0;
    
    uint32_t written = audio_ring_buffer_write(&g_shared_audio_buffer, audio_data, sample_count);
    
    if (written > 0) {
        total_written += written;
        write_count++;
        
        // Print every 100 successful writes (~5.3 seconds at 48kHz with 528 sample buffers)
        if (g_micDebug >= 4 && write_count % 100 == 0) {
            printf("[AudioWrite] %lu writes, %lu total samples, buf_write_idx=%lu\n",
                   write_count, total_written, g_shared_audio_buffer.write_index);
        }
    } else {
        // Buffer full
        static uint32_t full_count = 0;
        full_count++;
        if (g_micDebug >= 4 && full_count % 50 == 0) {
            printf("[AudioWrite] Buffer FULL! (full_count=%lu)\n", full_count);
        }
    }
    
    return written;
}
