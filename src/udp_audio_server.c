#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <lwip/udp.h>
#include <lwip/err.h>
#include <pico/time.h>
#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tcp_server.h"  // Interface header (kept for backward compatibility)
#include "microphone.h"
#include "network.h"

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
static uint32_t g_audio_frames_sent_attempts = 0;  /* Track send attempts vs success */
static uint32_t g_audio_sendto_errors = 0;         /* Track udp_sendto failures */

/* UDP task state (was local static, now module-level for proper reset on restart) */
static uint32_t g_udp_last_sequence = 0;      /* Last sequence number received */
static uint32_t g_udp_frame_count = 0;        /* Frames processed this session */
static uint32_t g_udp_frames_skipped = 0;     /* Frames skipped due to backpressure */
static uint32_t g_udp_last_report_time = 0;   /* Last stats report time */

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
        
        if (g_micDebug >= 6) {
            printf("[UDP] Sending %d samples to client %d\n", AUDIO_BUFFER_SIZE, i);
        }
        
        /* Create pbuf that references existing buffer (zero-copy) */
        uint32_t frame_bytes = AUDIO_BUFFER_SIZE * sizeof(int16_t);
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, frame_bytes, PBUF_REF);
        if (p == NULL) {
            /* Out of memory - skip this frame */
            g_audio_samples_dropped += AUDIO_BUFFER_SIZE;
            if (g_micDebug >= 2) {
                printf("[UDP] ERROR: pbuf_alloc FAILED for client %d\n", i);
            }
            continue;
        }
        
        /* Point directly to existing buffer - NO memcpy */
        p->payload = (void *)buffer;
        
        /* Send UDP packet to client */
        uint32_t send_start = to_ms_since_boot(get_absolute_time());
        err_t err = udp_sendto(audio_pcb, p, &client->addr, client->port);
        uint32_t send_time = to_ms_since_boot(get_absolute_time()) - send_start;
        
        g_audio_frames_sent_attempts++;  /* Count send attempt */
        
        if (send_time > 5 && g_micDebug >= 3) {
            printf("[UDP_SEND] WARNING: send took %lu ms for client %d (buffer pressure?)\n", send_time, i);
        }
        
        /* Free pbuf header (buffer remains valid - it's persistent audio data) */
        pbuf_free(p);
        
        if (err != ERR_OK) {
            g_audio_samples_dropped += AUDIO_BUFFER_SIZE;
            g_audio_sendto_errors++;  /* Count failures */
            if (g_micDebug >= 2) {
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
    printf("[UDP_RECV_CB] CALLBACK INVOKED from ");
    ip_addr_debug_print(LWIP_DBG_ON, addr);
    printf(" port %u\n", port);
    
    if (p == NULL) {
        printf("[UDP_RECV_CB] NULL pbuf, returning\n");
        return;
    }
    
    printf("[UDP_RECV_CB] Packet received: %u bytes\n", p->tot_len);
    
    // Register this client if we have space
    int found = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_udp_clients[i].active && 
            ip_addr_cmp(&g_udp_clients[i].addr, addr) &&
            g_udp_clients[i].port == port) {
            found = i;
            printf("[UDP_RECV_CB] Client already registered at index %d\n", i);
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
                printf("[UDP_RECV_CB] NEW CLIENT registered at index %d: ", i);
                ip_addr_debug_print(LWIP_DBG_ON, addr);
                printf(" port %u\n", port);
                
                /* CRITICAL DEBUG: Verify client was actually set */
                if (g_micDebug >= 4) {
                    printf("[UDP_RECV_CB] VERIFY: clients[%d].active=%u, port=%u\n", i, g_udp_clients[i].active, g_udp_clients[i].port);
                }
                break;
            }
        }
    }
    
    pbuf_free(p);
}

/* Audio task - receives messages from queue when buffer is ready */
/* Added: Backpressure and frame skipping to prevent network I/O from blocking */
void udp_audio_task(void *parameters) {
    printf("UDP Audio Task Started\n");
    
    g_udp_audio_task_handle = xTaskGetCurrentTaskHandle();
    uint32_t frame_count = 0;
    uint32_t take_count = 0;
    
    while (1) {
        AudioBufferMessage_t msg;
        take_count++;
        
        /* Queue-based: block until microphone sends next message */
        /* FreeRTOS queue handles memory barriers internally */
        BaseType_t queue_result = xQueueReceive(g_audioQueueUDP, &msg, portMAX_DELAY);
        
        if (queue_result != pdTRUE) {
            if (g_micDebug >= 1) {
                printf("[UDP] ERROR: xQueueReceive failed\n");
            }
            continue;
        }
        
        /* Extract buffer pointer from message */
        int16_t *buffer_ptr = (msg.buffer_id == 1) ? 
                              g_audioBuffers.buffer1 : g_audioBuffers.buffer2;
        uint32_t sample_count = msg.sample_count;
        uint32_t sequence = msg.sequence;
        
        if (buffer_ptr == NULL || sample_count == 0) {
            g_audio_samples_dropped += sample_count;
            if (g_micDebug >= 1) {
                printf("[UDP] ERROR: Invalid buffer (ptr=%p, samples=%u, seq=%lu)\n",
                       (void*)buffer_ptr, sample_count, sequence);
            }
            g_udp_frame_count++;
            continue;
        }
        
        /* Check for missed buffers (sequence numbering) */
        if (g_udp_last_sequence > 0 && sequence != g_udp_last_sequence + 1) {
            uint32_t missed = sequence - g_udp_last_sequence - 1;
            g_audio_samples_dropped += missed * sample_count;
            if (g_micDebug >= 1) {
                printf("[UDP] Frame %lu: DROPPED %lu buffers (seq %lu → %lu)\n",
                       g_udp_frame_count, missed, g_udp_last_sequence, sequence);
            }
        }
        g_udp_last_sequence = sequence;
        
        /* Count active clients */
        int active_clients = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_udp_clients[i].active) {
                active_clients++;
            }
        }
        
        /* Time the send operation to detect buffer pressure */
        uint32_t frame_send_start = to_ms_since_boot(get_absolute_time());
        
        /* Send the frame */
        udp_audio_send_frame(buffer_ptr);
        
        uint32_t frame_send_time = to_ms_since_boot(get_absolute_time()) - frame_send_start;
        
        if (frame_send_time > 10 && g_micDebug >= 3) {
            printf("[UDP] WARNING: Frame %lu send took %lu ms (buffer pressure?)\n", frame_count, frame_send_time);
        }
        
        if (g_micDebug >= 6) {
            printf("[UDP] SEND_COMPLETE: Frame %lu (seq=%lu) sent\n", g_udp_frame_count, sequence);
        }
        g_udp_frame_count++;
        
        /* Periodic reporting - VERBOSE to track degradation */
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - g_udp_last_report_time >= 2000) {  /* Report every 2s instead of 5s */
            g_udp_last_report_time = now;
            
            /* Count active clients */
            int active_clients = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_udp_clients[i].active) active_clients++;
            }
            
            /* Calculate frame rate */
            static uint32_t prev_frame_count = 0;
            uint32_t frames_in_interval = g_udp_frame_count - prev_frame_count;
            uint32_t frame_rate_hz = (frames_in_interval * 528 * 1000) / 2000;  /* samples/sec */
            prev_frame_count = g_udp_frame_count;
            
            if (g_micDebug >= 2) {
                printf("[UDP_STATS] Frames: sent=%lu attempts=%lu errors=%lu dropped_samples=%lu rate=%lu Hz\n",
                     g_udp_frame_count, g_audio_frames_sent_attempts, g_audio_sendto_errors, 
                     g_audio_samples_dropped, frame_rate_hz);
            }
            
            if (g_micDebug >= 6) {
                printf("[UDP] t=%lu: rate=%luHz, frames_sent=%lu, clients=%d (semaphore-driven)\n",
                       now /

 1000, frame_rate_hz, g_udp_frame_count, active_clients);
            }
            
            if (active_clients == 0 && g_micDebug >= 5) {
                printf("[UDP] WARNING: No active clients!\n");
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
    
    /* Ensure WiFi and lwIP are fully initialized */
    printf("[UDP] Ensuring lwIP/WiFi initialized...\n");
    wifi_init();
    printf("[UDP] lwIP/WiFi ready\n");
    
    printf("\n========== UDP Audio Server: Starting ==========\n");
    printf("UDP Audio Server: Starting on port %d...\n", UDP_AUDIO_PORT);
    printf("UDP Audio Server: Using QUEUE-based flow (FreeRTOS queue from microphone)\n");
    printf("UDP Audio Server: Sending %d samples per buffer\n", AUDIO_BUFFER_SIZE);;
    
    /* DIAGNOSTIC: Log startup conditions BEFORE any allocations */
    printf("[DIAGNOSTIC] Startup conditions (BEFORE):\n");
    printf("  - Semaphore: resetting (event-driven system)\n");
    printf("  - Active clients: ");
    int client_count_before = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_udp_clients[i].active) {
            client_count_before++;
            printf("%u ", (unsigned)g_udp_clients[i].port);
        }
    }
    printf("(%d total)\n", client_count_before);
    printf("  - Audio PCB exists: %s\n", audio_pcb != NULL ? "YES" : "NO");
    printf("  - Buffer being sent: %u\n", g_current_buffer_being_sent);
    
    /* Initialize client array */
    printf("[DIAGNOSTIC] Clearing client array...\n");
    memset(g_udp_clients, 0, sizeof(g_udp_clients));
    
    /* Reset buffer tracking and all UDP task counters for clean start */
    g_current_buffer_being_sent = 0;
    g_udp_last_sequence = 0;        /* Reset sequence counter to avoid false "missed frames" */
    g_udp_frame_count = 0;          /* Reset frame counter */
    g_udp_frames_skipped = 0;       /* Reset skip counter */
    g_udp_last_report_time = 0;     /* Reset report timer */
    
    /* If PCB already exists from previous run, clean it up */
    if (audio_pcb != NULL) {
        printf("[DIAGNOSTIC] Old PCB exists, cleaning up...\n");
        udp_remove(audio_pcb);
        audio_pcb = NULL;
    }
    
    /* Create UDP PCB */
    printf("[DIAGNOSTIC] Creating new UDP PCB...\n");
    audio_pcb = udp_new();
    if (audio_pcb == NULL) {
        printf("UDP Audio Server: ERROR - Failed to create UDP PCB (lwIP not initialized?)\n");
        return -1;
    }
    printf("[DIAGNOSTIC] PCB created: %p\n", (void*)audio_pcb);
    
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
    printf("[DEBUG] PCB before udp_recv: %p\n", (void*)audio_pcb);
    printf("[DEBUG] Callback function: %p\n", (void*)udp_recv_callback);
    udp_recv(audio_pcb, udp_recv_callback, NULL);
    printf("[DEBUG] udp_recv completed (returns void)\n");
    
    printf("UDP Audio Server: Listening on port %d (send UDP packet to register)\n", UDP_AUDIO_PORT);
    printf("UDP Audio Server: Using semaphore-driven flow (event-based from microphone)\n");
    printf("UDP Audio Server: Sending %d samples per buffer\n", AUDIO_BUFFER_SIZE);
    
    /* Set server running BEFORE diagnostic so it reflects actual state */
    g_server_running = 1;
    
    /* DIAGNOSTIC: Log final state AFTER startup completes */
    printf("[DIAGNOSTIC] Startup state (AFTER):\n");
    printf("  - Queue: ready (FreeRTOS semaphore-based)\n");
    printf("  - Active clients: 0 (none connected yet)\n");
    printf("  - Audio PCB: %p\n", (void*)audio_pcb);
    printf("  - Buffer being sent: %u\n", g_current_buffer_being_sent);
    printf("  - Server running: %s\n", g_server_running ? "YES" : "NO");
    printf("========== UDP Audio Server: Started Successfully ==========\n\n");
    
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
