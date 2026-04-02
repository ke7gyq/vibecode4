#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <lwip/udp.h>
#include <lwip/err.h>
#include <pico/time.h>
#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tcp_server.h"  // Interface header (kept for backward compatibility)
#include "microphone.h"

#define UDP_AUDIO_PORT 5001
#define MAX_CLIENTS 4

// UDP server state
static struct udp_pcb *audio_pcb = NULL;
static int g_server_running = 0;

// Client tracking - store last known client address
typedef struct {
    ip_addr_t addr;
    uint16_t port;
    int active;
    uint32_t samples_sent;
    uint32_t last_stats_time;
} udp_client_t;

static udp_client_t g_udp_clients[MAX_CLIENTS];

/* ==================== Audio Server (UDP Port 5001) ==================== */

// Audio transmission via UDP - timer-based
#define FRAME_SAMPLES 528       /* Send full audio buffer per frame (AUDIO_BUFFER_SIZE) */
#define FRAME_BYTES (FRAME_SAMPLES * sizeof(int16_t))
static struct repeating_timer g_audio_timer;
static TaskHandle_t g_udp_audio_task_handle = NULL;  // UDP audio task handle

/* Audio buffer read tracking - prevent reading stale buffers */
static uint8_t g_current_buffer_being_read = 0;      // Which buffer we're currently reading from
static int16_t g_frame_send_buffer[FRAME_SAMPLES];   /* Reusable frame buffer (avoid stack overflow) */

// Stats tracking
static uint32_t g_timer_calls = 0;
static uint32_t g_timer_samples_sent = 0;
static uint32_t g_timer_samples_dropped = 0;
static uint32_t g_timer_last_stats_time = 0;

// Timer interrupt handler - notify audio task
static bool audio_timer_interrupt(struct repeating_timer *t) {
    /* Only process frames if server is running */
    if (!g_server_running) {
        return true;
    }
    
    if (g_udp_audio_task_handle != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(g_udp_audio_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    g_timer_calls++;
    return true;
}

// Send audio frame to all active clients via UDP
// Uses real microphone buffer instead of synthetic data
static void udp_audio_send_frame(void) {
    // Safety: don't process frames if no buffer is ready yet
    // (prevents accessing uninitialized g_audioBuffers)
    if (g_audioReady == 0) {
        if (g_timer_calls % 100 == 0 && g_micDebug) {
            printf("[UDP] Waiting for audio buffer (g_audioReady=%d)...\n", g_audioReady);
        }
        return;  // Still waiting for first buffer from microphone
    }
    
    // Track which buffer we're reading to detect buffer switches
    if (g_audioReady != g_current_buffer_being_read) {
        g_current_buffer_being_read = g_audioReady;
        if (g_micDebug) {
            printf("[UDP] Buffer switch: now reading from buffer %d\n", g_current_buffer_being_read);
        }
    }
    
    // Get current microphone buffer pointer
    int16_t *mic_buffer = NULL;
    if (g_current_buffer_being_read == 1) {
        mic_buffer = g_audioBuffers.buffer1;
    } else if (g_current_buffer_being_read == 2) {
        mic_buffer = g_audioBuffers.buffer2;
    } else {
        // Should not happen (g_audioReady should be 1 or 2)
        g_timer_samples_dropped += FRAME_SAMPLES;
        return;
    }
    
    // Copy FRAME_SAMPLES from start of current buffer
    int samples_to_copy = (FRAME_SAMPLES <= AUDIO_BUFFER_SIZE) ? FRAME_SAMPLES : AUDIO_BUFFER_SIZE;
    memcpy(g_frame_send_buffer, mic_buffer, samples_to_copy * sizeof(int16_t));
    
    // Pad with zeros if necessary
    if (samples_to_copy < FRAME_SAMPLES) {
        memset(&g_frame_send_buffer[samples_to_copy], 0, (FRAME_SAMPLES - samples_to_copy) * sizeof(int16_t));
    }
    
    // Send to all active UDP clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        udp_client_t *client = &g_udp_clients[i];
        
        if (!client->active) {
            continue;
        }
        
        if (g_micDebug >= 2 && g_timer_calls % 50 == 0) {
            printf("[UDP] Sending frame to client %d\n", i);
        }
        
        // Create pbuf for this frame
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, FRAME_BYTES, PBUF_RAM);
        if (p == NULL) {
            // Out of memory - skip this frame
            g_timer_samples_dropped += FRAME_SAMPLES;
            if (g_micDebug) {
                printf("[UDP] ERROR: pbuf_alloc failed for client %d\n", i);
            }
            continue;
        }
        
        // Copy frame data into pbuf
        memcpy(p->payload, (const void *)g_frame_send_buffer, FRAME_BYTES);
        
        // Send UDP packet to client
        err_t err = udp_sendto(audio_pcb, p, &client->addr, client->port);
        
        // Free pbuf (udp_sendto doesn't own it)
        pbuf_free(p);
        
        if (err != ERR_OK) {
            g_timer_samples_dropped += FRAME_SAMPLES;
            if (g_micDebug) {
                printf("[UDP] ERROR: udp_sendto failed (err=%d) for client %d\n", err, i);
            }
            continue;
        }
        
        // Success
        client->samples_sent += FRAME_SAMPLES;
        g_timer_samples_sent += FRAME_SAMPLES;
    }
    
    // Print stats every second (debug only)
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - g_timer_last_stats_time >= 1000) {
        uint32_t ms_elapsed = now - g_timer_last_stats_time;
        float rate = (g_timer_samples_sent * 1000.0f) / ms_elapsed;
        float drop_rate = (g_timer_samples_dropped * 1000.0f) / ms_elapsed;
        float timer_hz = (g_timer_calls * 1000.0f) / ms_elapsed;
        if (g_micDebug) {
            printf("[AudioRate] TX: %.0f Hz, DROP: %.0f Hz (%.0f IRQs/sec)\n", rate, drop_rate, timer_hz);
        }
        g_timer_samples_sent = 0;
        g_timer_samples_dropped = 0;
        g_timer_calls = 0;
        g_timer_last_stats_time = now;
    }
}

// UDP receive callback - track client address when they send first packet
static void udp_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, 
                              const ip_addr_t *addr, uint16_t port) {
    if (p == NULL) {
        return;
    }
    
    // Register this client if we have space
    int found = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_udp_clients[i].active && 
            ip_addr_cmp(&g_udp_clients[i].addr, addr) &&
            g_udp_clients[i].port == port) {
            found = i;
            break;
        }
    }
    
    if (found == -1) {
        // New client - find empty slot
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_udp_clients[i].active) {
                g_udp_clients[i].addr = *addr;
                g_udp_clients[i].port = port;
                g_udp_clients[i].active = 1;
                g_udp_clients[i].samples_sent = 0;
                g_udp_clients[i].last_stats_time = to_ms_since_boot(get_absolute_time());
                printf("UDP Audio client connected: ");
                ip_addr_debug_print(LWIP_DBG_ON, addr);
                printf(" port %u\n", port);
                break;
            }
        }
    }
    
    pbuf_free(p);
}

// Audio task - waits for timer notifications and sends frames
void udp_audio_task(void *parameters) {
    printf("UDP Audio Task Started\n");
    
    g_udp_audio_task_handle = xTaskGetCurrentTaskHandle();
    
    while (1) {
        // Wait for notification with timeout
        uint32_t ulNotificationValue = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        
        if (ulNotificationValue > 0) {
            udp_audio_send_frame();
        }
    }
}

/* ==================== Server Control ==================== */

int udp_server_start(void) {
    if (g_server_running) {
        printf("UDP Audio Server: Already running\n");
        return -1;
    }
    
    printf("UDP Audio Server: Starting on port %d...\n", UDP_AUDIO_PORT);
    
    // Initialize client array
    memset(g_udp_clients, 0, sizeof(g_udp_clients));
    
    // Reset buffer read tracking
    g_current_buffer_being_read = 0;
    
    // If PCB already exists from previous run, clean it up
    if (audio_pcb != NULL) {
        printf("UDP Audio Server: Cleaning up old PCB\n");
        udp_remove(audio_pcb);
        audio_pcb = NULL;
    }
    
    // Create UDP PCB
    printf("UDP Audio Server: Creating UDP PCB...\n");
    audio_pcb = udp_new();
    if (audio_pcb == NULL) {
        printf("UDP Audio Server: ERROR - Failed to create UDP PCB (lwIP not initialized?)\n");
        return -1;
    }
    printf("UDP Audio Server: PCB created successfully\n");
    
    // Bind to port
    printf("UDP Audio Server: Binding to port %d...\n", UDP_AUDIO_PORT);
    if (udp_bind(audio_pcb, IP_ADDR_ANY, UDP_AUDIO_PORT) != ERR_OK) {
        printf("UDP Audio Server: ERROR - Failed to bind to port\n");
        udp_remove(audio_pcb);
        audio_pcb = NULL;
        return -1;
    }
    printf("UDP Audio Server: Successfully bound to port %d\n", UDP_AUDIO_PORT);
    
    // Set receive callback to track clients
    printf("UDP Audio Server: Setting recv callback...\n");
    udp_recv(audio_pcb, udp_recv_callback, NULL);
    
    printf("UDP Audio Server: Listening on port %d (send UDP packet to register)\n", UDP_AUDIO_PORT);
    
    // Start audio transmission timer: fires every 12ms = 83 Hz
    // 83 Hz * 528 samples/frame = 43,824 samples/sec ≈ 43.8 kHz throughput
    // UDP handles this rate without backpressure issues TCP had
    printf("UDP Audio Server: Starting 12ms timer...\n");
    if (!add_repeating_timer_ms(12, audio_timer_interrupt, NULL, &g_audio_timer)) {
        printf("UDP Audio Server: ERROR - Failed to create timer\n");
        udp_remove(audio_pcb);
        audio_pcb = NULL;
        return -1;
    }
    printf("UDP Audio Server: Timer started (12ms = 83 Hz, 43.8 kHz throughput)\n");
    
    g_server_running = 1;
    printf("UDP Audio Server: Started - Python client should send to port %d\n", UDP_AUDIO_PORT);
    
    return 0;
}

void udp_server_stop(void) {
    if (!g_server_running) {
        return;
    }
    
    cancel_repeating_timer(&g_audio_timer);
    
    if (audio_pcb != NULL) {
        udp_remove(audio_pcb);
        audio_pcb = NULL;
    }
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_udp_clients[i].active = 0;
    }
    
    g_server_running = 0;
    printf("UDP Audio Server: Stopped\n");
}

int udp_server_is_running(void) {
    return g_server_running;
}
