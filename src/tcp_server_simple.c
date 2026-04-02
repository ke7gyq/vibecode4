#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <lwip/tcp.h>
#include <lwip/err.h>
#include <pico/time.h>
#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tcp_server.h"
#include "microphone.h"

#define TCP_SERVER_PORT_HELLO 5000
#define TCP_SERVER_PORT_AUDIO 5001
#define MAX_CLIENTS 4

// Server state
static struct tcp_pcb *server_pcb_hello = NULL;
static struct tcp_pcb *server_pcb_audio = NULL;
static int g_server_running = 0;

// Hello client session
typedef struct {
    uint32_t count;
    int active;
} hello_session_t;

static hello_session_t g_hello_sessions[MAX_CLIENTS];
static int g_next_hello = 0;

// Audio client session - ultra simple
typedef struct {
    struct tcp_pcb *pcb;
    uint32_t last_read_index;  // Track what we last read from microphone buffers
    uint8_t last_buffer_id;    // Track which buffer (1 or 2) we last read from
    int active;
    uint32_t samples_sent;
    uint32_t last_stats_time;
} audio_session_t;

static audio_session_t g_audio_clients[MAX_CLIENTS];
static int g_next_audio_client = 0;

/* ==================== Hello Server (Port 5000) ==================== */

static err_t hello_client_poll_cb(void *arg, struct tcp_pcb *pcb) {
    if (arg == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    
    hello_session_t *session = (hello_session_t *)arg;
    if (!session->active) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "Hello %u\n", session->count++);
    
    if (tcp_write(pcb, (const void *)msg, len, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        tcp_output(pcb);
    }
    
    return ERR_OK;
}

static err_t hello_client_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        // Connection closed
        if (arg != NULL) {
            ((hello_session_t *)arg)->active = 0;
        }
        tcp_close(pcb);
        return ERR_OK;
    }
    
    pbuf_free(p);
    tcp_recved(pcb, p->tot_len);
    return ERR_OK;
}

static err_t hello_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_ARG;
    }
    
    hello_session_t *session = &g_hello_sessions[g_next_hello++ % MAX_CLIENTS];
    session->count = 0;
    session->active = 1;
    
    tcp_arg(newpcb, session);
    tcp_recv(newpcb, hello_client_recv_cb);
    tcp_poll(newpcb, hello_client_poll_cb, 1);
    
    printf("Hello client connected\n");
    return ERR_OK;
}

/* ==================== Audio Server (Port 5001) - TASK NOTIFICATION DRIVEN ==================== */

// 512 samples @ 83 Hz = 42,496 samples/sec ≈ 42 kHz throughput
#define FRAME_SAMPLES 512
#define FRAME_BYTES (FRAME_SAMPLES * sizeof(int16_t))
static struct repeating_timer g_audio_timer;

// Task handle for audio transmission (set by tcp_audio_task_create)
static TaskHandle_t g_tcp_audio_task_handle = NULL;

// Stats tracking
static uint32_t g_timer_calls = 0;
static uint32_t g_timer_samples_sent = 0;
static uint32_t g_timer_samples_dropped = 0;
static uint32_t g_timer_last_stats_time = 0;
static uint16_t g_frame_pattern = 0;  // Global pattern counter

// Timer interrupt handler - MINIMAL, just notify the audio task
static bool audio_timer_interrupt(struct repeating_timer *t) {
    if (g_tcp_audio_task_handle != NULL) {
        // Send task notification from ISR
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(g_tcp_audio_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    g_timer_calls++;
    return true;  // Continue timer
}

// Polling function - called when audio task is notified
// Generates one frame and sends to all active clients
static void tcp_audio_send_frame(void) {
    // Allocate fresh buffer on stack for this frame
    int16_t frame_buf[FRAME_SAMPLES];
    
    // Generate frame data (incremental pattern)
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        frame_buf[i] = (int16_t)g_frame_pattern++;
    }
    
    // Send to all active audio clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        audio_session_t *session = &g_audio_clients[i];
        
        if (!session->active || session->pcb == NULL) {
            continue;
        }
        
        // Check TCP space - only write if there's SUBSTANTIAL room
        // (Prevent write queue from backing up and corrupting lwIP state)
        uint16_t tcp_space = tcp_sndbuf(session->pcb);
        uint16_t min_space = FRAME_BYTES * 2;  // Require 2x frame size available
        if (tcp_space < min_space) {
            // Buffer full or nearly full - skip this frame to avoid deadlock
            g_timer_samples_dropped += FRAME_SAMPLES;
            continue;
        }
        
        // Space available - write frame with COPY to ensure data stays valid
        err_t err = tcp_write(session->pcb, (const void *)frame_buf, FRAME_BYTES, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            // Write failed - skip
            g_timer_samples_dropped += FRAME_SAMPLES;
            continue;
        }
        
        // Success - track stats and flush
        session->samples_sent += FRAME_SAMPLES;
        g_timer_samples_sent += FRAME_SAMPLES;
        tcp_output(session->pcb);
    }
    
    // Print stats every second
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - g_timer_last_stats_time >= 1000) {
        uint32_t ms_elapsed = now - g_timer_last_stats_time;
        float rate = (g_timer_samples_sent * 1000.0f) / ms_elapsed;
        float drop_rate = (g_timer_samples_dropped * 1000.0f) / ms_elapsed;
        float timer_hz = (g_timer_calls * 1000.0f) / ms_elapsed;
        printf("[AudioRate] TX: %.0f Hz, DROP: %.0f Hz (%.0f IRQs/sec)\n", rate, drop_rate, timer_hz);
        g_timer_samples_sent = 0;
        g_timer_samples_dropped = 0;
        g_timer_calls = 0;
        g_timer_last_stats_time = now;
    }
}

// TCP Audio Task - waits for timer interrupt notifications and sends frames
void tcp_audio_task(void *parameters) {
    printf("TCP Audio Task Started\n");
    
    // Set ourselves as the notified task
    g_tcp_audio_task_handle = xTaskGetCurrentTaskHandle();
    
    while (1) {
        // Wait for task notification with 250ms timeout (safety measure)
        // ulTaskNotifyTake clears the notification and returns the count
        // pdTRUE = auto-reset (clear count), 250ms timeout to prevent infinite block
        uint32_t ulNotificationValue = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        
        // Process ONE notification only - don't loop
        // This prevents task starvation of lower-priority tasks
        if (ulNotificationValue > 0) {
            // One notification received - send a single frame
            tcp_audio_send_frame();
        }
        // Loop back and wait for next notification
    }
}

static err_t audio_client_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        // Connection closed
        if (arg != NULL) {
            ((audio_session_t *)arg)->active = 0;
        }
        tcp_close(pcb);
        printf("Audio client disconnected\n");
        return ERR_OK;
    }
    
    pbuf_free(p);
    tcp_recved(pcb, p->tot_len);
    return ERR_OK;
}

static err_t audio_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) {
        printf("Audio: Accept error\n");
        return ERR_ARG;
    }
    
    audio_session_t *session = &g_audio_clients[g_next_audio_client++ % MAX_CLIENTS];
    session->pcb = newpcb;
    session->active = 1;
    session->last_read_index = 0;
    session->last_buffer_id = 0;
    session->samples_sent = 0;
    session->last_stats_time = to_ms_since_boot(get_absolute_time());
    
    tcp_arg(newpcb, session);
    tcp_recv(newpcb, audio_client_recv_cb);
    // NOTE: NO tcp_poll() used - using high-frequency timer instead
    
    printf("Audio client connected\n");
    return ERR_OK;
}

/* ==================== Server Control ==================== */

int tcp_server_start(void) {
    if (g_server_running) {
        printf("TCP Server: Already running\n");
        return -1;
    }
    
    printf("TCP Server: Starting (hello:%d, audio:%d)...\n", TCP_SERVER_PORT_HELLO, TCP_SERVER_PORT_AUDIO);
    
    // Initialize arrays
    memset(g_hello_sessions, 0, sizeof(g_hello_sessions));
    g_next_hello = 0;
    memset(g_audio_clients, 0, sizeof(g_audio_clients));
    g_next_audio_client = 0;
    
    // Hello server
    server_pcb_hello = tcp_new();
    if (server_pcb_hello == NULL) {
        printf("TCP Server: Failed to create hello PCB\n");
        return -1;
    }
    
    if (tcp_bind(server_pcb_hello, IP_ADDR_ANY, TCP_SERVER_PORT_HELLO) != ERR_OK) {
        printf("TCP Server: Failed to bind hello port\n");
        tcp_close(server_pcb_hello);
        return -1;
    }
    
    server_pcb_hello = tcp_listen(server_pcb_hello);
    if (server_pcb_hello == NULL) {
        printf("TCP Server: Failed to listen on hello port\n");
        return -1;
    }
    
    tcp_accept(server_pcb_hello, hello_accept_cb);
    printf("TCP Server: Listening for hello on port %d\n", TCP_SERVER_PORT_HELLO);
    
    // Audio server
    server_pcb_audio = tcp_new();
    if (server_pcb_audio == NULL) {
        printf("TCP Server: Failed to create audio PCB\n");
        tcp_close(server_pcb_hello);
        return -1;
    }
    
    if (tcp_bind(server_pcb_audio, IP_ADDR_ANY, TCP_SERVER_PORT_AUDIO) != ERR_OK) {
        printf("TCP Server: Failed to bind audio port\n");
        tcp_close(server_pcb_hello);
        tcp_close(server_pcb_audio);
        return -1;
    }
    
    server_pcb_audio = tcp_listen(server_pcb_audio);
    if (server_pcb_audio == NULL) {
        printf("TCP Server: Failed to listen on audio port\n");
        tcp_close(server_pcb_hello);
        return -1;
    }
    
    tcp_accept(server_pcb_audio, audio_accept_cb);
    printf("TCP Server: Listening for audio on port %d\n", TCP_SERVER_PORT_AUDIO);
    
    // Start audio transmission timer: fires every 12.5ms = 80 Hz
    // 80 Hz * 512 samples/frame = 40,960 samples/sec ≈ 41 kHz throughput
    // Smaller frames fit better in TCP buffers than large 2KB chunks
    add_repeating_timer_ms(12, audio_timer_interrupt, NULL, &g_audio_timer);  // ~12ms ≈ 83 Hz
    printf("TCP Server: Audio timer started (12ms ≈ 83 Hz, 41 kHz throughput)\n");
    
    g_server_running = 1;
    printf("TCP Server: Started (connect via: telnet <IP> 5000)\n");
    
    return 0;
}

void tcp_server_stop(void) {
    if (!g_server_running) {
        return;
    }
    
    cancel_repeating_timer(&g_audio_timer);
    
    if (server_pcb_hello != NULL) {
        tcp_close(server_pcb_hello);
        server_pcb_hello = NULL;
    }
    
    if (server_pcb_audio != NULL) {
        tcp_close(server_pcb_audio);
        server_pcb_audio = NULL;
    }
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_hello_sessions[i].active = 0;
        g_audio_clients[i].active = 0;
    }
    
    g_server_running = 0;
    printf("TCP Server: Stopped\n");
}

int tcp_server_is_running(void) {
    return g_server_running;
}

uint32_t tcp_server_write_audio_frame(const int16_t *audio_data, uint32_t sample_count) {
    // In simple mode, we don't need this - microphone just fills buffers
    // and poll callbacks read directly from g_audioBuffers
    (void)audio_data;
    (void)sample_count;
    return 0;
}
