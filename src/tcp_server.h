#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>

/**
 * Start the TCP server that sends "Hello X" messages to connected clients
 * Server listens on port 5000 and accepts telnet connections
 * Returns 0 on success, non-zero on error
 */
int tcp_server_start(void);

/**
 * Stop the TCP server
 */
void tcp_server_stop(void);

/**
 * Get server status
 * Returns 1 if running, 0 if stopped
 */
int tcp_server_is_running(void);

#endif
