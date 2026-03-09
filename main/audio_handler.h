#ifndef AUDIO_HANDLER_H
#define AUDIO_HANDLER_H

#include "esp_http_server.h"
#include "driver/i2s_std.h"

// Global variables (declared in main.c)
extern i2s_chan_handle_t tx_handle;
extern i2s_chan_handle_t rx_handle;
extern int ws_fd;
extern httpd_handle_t server;

// Functions
void init_audio();
void mic_task(void *p);
esp_err_t ws_handler(httpd_req_t *req);

#endif
