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

/* Audio transmission - semaphore-driven (event-based from microphone) */
static TaskHandle_t g_udp_audio_task_handle = NULL;  /* UDP audio task handle */

/* Current buffer being transmitted */
static uint8_t g_current_buffer_being_sent = 0;      /* Which buffer we're sending (1 or 2) */

/* Stats tracking */
static uint32_t g_audio_frames_sent = 0;
static uint32_t g_audio_samples_sent = 0;
static uint32_t g_audio_samples_dropped = 0;
static uint32_t g_audio_last_stats_time = 0;

/* Send audio frame to all active clients via UDP
 * Zero-copy implementation: uses PBUF_REF to point directly to audio buffer
 * Safe because double-buffering ensures buffer isn't modified while sending
 */
static void udp_audio_send_frame(int16_t *buffer) {
    if (buffer == NULL) {
        g_audio_samples_dropped += AUDIO_BUFFER_SIZE;
        return;
    }
    
    /* Send to all active UDP clients */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        udp_client_t *client = &g_udp_clients[i];
        
        if (!client->active) {
            continue;
        }
        
        if (g_micDebug >= 2) {
            printf("[UDP] Sending %d samples to client %d\n", AUDIO_BUFFER_SIZE, i);
        }
        
        /* Create pbuf that references existing buffer (zero-copy) */
        uint32_t frame_bytes = AUDIO_BUFFER_SIZE * sizeof(int16_t);
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, frame_bytes, PBUF_REF);
        if (p == NULL) {
            /* Out of memory - skip this frame */
            g_audio_samples_dropped += AUDIO_BUFFER_SIZE;
            if (g_micDebug) {
                printf("[UDP] ERROR: pbuf_alloc (REF) failed for client %d\n", i);
            }
            continue;
        }
        
        /* Point directly to existing buffer - NO memcpy */
        p->payload = (void *)buffer;
        
        /* Send UDP packet to client */
        err_t err = udp_sendto(audio_pcb, p, &client->addr, client->port);
        
        /* Free pbuf header only (buffer remains in place) */
        pbuf_free(p);
        
        if (err != ERR_OK) {
            g_audio_samples_dropped += AUDIO_BUFFER_SIZE;
            if (g_micDebug) {
                printf("[UDP] ERROR: udp_sendto failed (err=%d) for client %d\n", err, i);
            }
            continue;
        }
        
        /* Success */
        client->samples_sent += AUDIO_BUFFER_SIZE;
        g_audio_samples_sent += AUDIO_BUFFER_SIZE;
    }
    
    g_audio_frames_sent++;
    
    /* Print stats every second (debug only) */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - g_audio_last_stats_time >= 1000) {
        uint32_t ms_elapsed = now - g_audio_last_stats_time;
        float rate = (g_audio_samples_sent * 1000.0f) / ms_elapsed;
        float drop_rate = (g_audio_samples_dropped * 1000.0f) / ms_elapsed;
        float frame_hz = (g_audio_frames_sent * 1000.0f) / ms_elapsed;
        if (g_micDebug) {
            printf("[AudioRate] TX: %.0f Hz, DROP: %.0f Hz (%.1f frames/sec)\n", rate, drop_rate, frame_hz);
        }
        g_audio_samples_sent = 0;
        g_audio_samples_dropped = 0;
        g_audio_frames_sent = 0;
        g_audio_last_stats_time = now;
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

/* Audio task - receives messages from queue when buffer is ready */
void udp_audio_task(void *parameters) {
    printf("UDP Audio Task Started\n");
    
    g_udp_audio_task_handle = xTaskGetCurrentTaskHandle();
    g_current_buffer_being_sent = 0;
    
    static uint32_t last_sequence = 0;
    static uint32_t frame_count = 0;
    AudioBufferMessage_t msg;
    
    while (1) {
        /* Wait for message from audio queue (non-blocking, timeout 1s) */
        BaseType_t result = xQueueReceive(g_audioQueueUDP, &msg, pdMS_TO_TICKS(1000));
        
        if (result == pdTRUE) {
            /* Message received - audio data is ready */
            
            /* Check for missed buffers (sequence numbering) */
            if (last_sequence > 0 && msg.sequence != last_sequence + 1) {
                uint32_t missed = msg.sequence - last_sequence - 1;
                g_audio_samples_dropped += missed * msg.sample_count;
                if (g_micDebug >= 1) {
                    printf("[UDP] Frame %lu: DROPPED %lu buffers (seq %lu → %lu)\n",
                           frame_count, missed, last_sequence, msg.sequence);
                }
            }
            last_sequence = msg.sequence;
            
            /* Track buffer switch */
            if (msg.buffer_id != g_current_buffer_being_sent) {
                g_current_buffer_being_sent = msg.buffer_id;
                if (g_micDebug >= 1) {
                    printf("[UDP] Frame %lu: Sending buffer %d (seq=%lu, %u samples)\n", 
                           frame_count, g_current_buffer_being_sent, msg.sequence, msg.sample_count);
                }
            }
            
            /* Validate buffer pointer */
            if (msg.buffer_ptr == NULL) {
                g_audio_samples_dropped += msg.sample_count;
                if (g_micDebug >= 1) {
                    printf("[UDP] ERROR: NULL buffer pointer in message\n");
                }
                frame_count++;
                continue;
            }
            
            /* Send the complete buffer */
            udp_audio_send_frame(msg.buffer_ptr);
            frame_count++;
            
        } else {
            /* Timeout waiting for queue message */
            if (g_server_running && g_micDebug >= 2) {
                printf("[UDP] Timeout waiting for audio buffer (server running)\n");
            }
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
    
    /* Initialize client array */
    memset(g_udp_clients, 0, sizeof(g_udp_clients));
    
    /* Reset buffer tracking */
    g_current_buffer_being_sent = 0;
    
    /* If PCB already exists from previous run, clean it up */
    if (audio_pcb != NULL) {
        printf("UDP Audio Server: Cleaning up old PCB\n");
        udp_remove(audio_pcb);
        audio_pcb = NULL;
    }
    
    /* Create UDP PCB */
    printf("UDP Audio Server: Creating UDP PCB...\n");
    audio_pcb = udp_new();
    if (audio_pcb == NULL) {
        printf("UDP Audio Server: ERROR - Failed to create UDP PCB (lwIP not initialized?)\n");
        return -1;
    }
    printf("UDP Audio Server: PCB created successfully\n");
    
    /* Bind to port */
    printf("UDP Audio Server: Binding to port %d...\n", UDP_AUDIO_PORT);
    if (udp_bind(audio_pcb, IP_ADDR_ANY, UDP_AUDIO_PORT) != ERR_OK) {
        printf("UDP Audio Server: ERROR - Failed to bind to port\n");
        udp_remove(audio_pcb);
        audio_pcb = NULL;
        return -1;
    }
    printf("UDP Audio Server: Successfully bound to port %d\n", UDP_AUDIO_PORT);
    
    /* Set receive callback to track clients */
    printf("UDP Audio Server: Setting recv callback...\n");
    udp_recv(audio_pcb, udp_recv_callback, NULL);
    
    printf("UDP Audio Server: Listening on port %d (send UDP packet to register)\n", UDP_AUDIO_PORT);
    printf("UDP Audio Server: Using semaphore-driven flow (event-based from microphone)\n");
    printf("UDP Audio Server: Sending %d samples per buffer\n", AUDIO_BUFFER_SIZE);
    
    g_server_running = 1;
    printf("UDP Audio Server: Started - Python client should send to port %d\n", UDP_AUDIO_PORT);
    
    return 0;
}

void udp_server_stop(void) {
    if (!g_server_running) {
        return;
    }
    
    /* No timer to cancel with semaphore-driven approach */
    
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
