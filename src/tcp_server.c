#include <stdio.h>
#include <string.h>
#include <lwip/tcp.h>
#include <lwip/err.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tcp_server.h"

#define TCP_SERVER_PORT 5000
#define MAX_CLIENTS 4

// Server state
static struct tcp_pcb *server_pcb = NULL;
static int g_server_running = 0;

// Client session data
typedef struct {
    uint32_t count;
    int active;
} client_session_t;

static client_session_t g_clients[MAX_CLIENTS];
static int g_next_client = 0;

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
        return ERR_OK;
    }
    
    if (p == NULL) {
        // Connection closed
        session->active = 0;
        printf("Client disconnected\n");
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
    
    printf("TCP Server: Starting on port %d...\n", TCP_SERVER_PORT);
    
    // Initialize client sessions
    memset(g_clients, 0, sizeof(g_clients));
    g_next_client = 0;
    
    // Create listening PCB
    server_pcb = tcp_new();
    if (server_pcb == NULL) {
        printf("TCP Server: Failed to create PCB\n");
        return -1;
    }
    
    // Bind to port
    err_t err = tcp_bind(server_pcb, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (err != ERR_OK) {
        printf("TCP Server: Failed to bind (error: %d)\n", err);
        tcp_close(server_pcb);
        server_pcb = NULL;
        return -1;
    }
    
    // Listen
    server_pcb = tcp_listen(server_pcb);
    if (server_pcb == NULL) {
        printf("TCP Server: Failed to listen\n");
        return -1;
    }
    
    // Set accept callback
    tcp_accept(server_pcb, server_accept_cb);
    
    g_server_running = 1;
    printf("TCP Server: Listening on port %d\n", TCP_SERVER_PORT);
    
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
    
    if (server_pcb != NULL) {
        tcp_close(server_pcb);
        server_pcb = NULL;
    }
    
    // Mark all clients as inactive
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].active = 0;
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
