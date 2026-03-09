#include "audio_handler.h"
#include "esp_log.h"

#define TAG "AUDIO_MOD"
#define SAMPLE_RATE 16000
#define BUF_SIZE 512

// Your pins
#define MIC_WS 1
#define MIC_SCK 2
#define MIC_DIN 42
#define SPK_DOUT 39
#define SPK_BCLK 40
#define SPK_LRCK 41

void init_audio() {
    // Microphone config (I2S_NUM_0)
    i2s_chan_config_t m_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&m_cfg, NULL, &rx_handle);
    i2s_std_config_t m_std = { 
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .bclk = MIC_SCK, .ws = MIC_WS, .din = MIC_DIN, .mclk = -1 } 
    };
    m_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    i2s_channel_init_std_mode(rx_handle, &m_std);

    // Speaker config (I2S_NUM_1)
    i2s_chan_config_t s_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&s_cfg, &tx_handle, NULL);
    i2s_std_config_t s_std = { 
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .bclk = SPK_BCLK, .ws = SPK_LRCK, .dout = SPK_DOUT, .mclk = -1 } 
    };
    i2s_channel_init_std_mode(tx_handle, &s_std);

    i2s_channel_enable(rx_handle);
    i2s_channel_enable(tx_handle);
    ESP_LOGI(TAG, "Audio Hardware Initialized");
}

esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ws_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // 1. Узнаем размер кадра
    if (httpd_ws_recv_frame(req, &ws_pkt, 0) == ESP_OK && ws_pkt.len > 0) {
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) return ESP_ERR_NO_MEM;

        ws_pkt.payload = buf; // <-- ВОТ ЭТА ВАЖНЕЙШАЯ СТРОЧКА!

        // 2. Теперь читаем данные в подготовленный буфер
        if (httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len) == ESP_OK) {
            int16_t *samples = (int16_t *)ws_pkt.payload;
            int count = ws_pkt.len / 2;
            for (int i = 0; i < count; i++) {
                samples[i] = samples[i] >> 2; 
            }
            size_t bw;
            i2s_channel_write(tx_handle, samples, ws_pkt.len, &bw, 100);
        }
        free(buf); // Освобождаем память после использования
    }
    return ESP_OK;
}


void mic_task(void *p) {
    int32_t *raw = malloc(BUF_SIZE * 4);
    int16_t *out = malloc(BUF_SIZE * 2);
    size_t br;
    while(1) {
        if (i2s_channel_read(rx_handle, raw, BUF_SIZE * 4, &br, portMAX_DELAY) == ESP_OK) {
            int count = br / 4;
            for(int i=0; i<count; i++) out[i] = (int16_t)(raw[i] >> 14);
            if (ws_fd != -1) {
                httpd_ws_frame_t pkt = { .payload = (uint8_t*)out, .len = count*2, .type = HTTPD_WS_TYPE_BINARY };
                if (httpd_ws_send_frame_async(server, ws_fd, &pkt) != ESP_OK) ws_fd = -1;
            }
        }
    }
}
