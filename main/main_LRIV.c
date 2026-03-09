#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#define TAG "ESP32_AUDIO_WEB"
#define SAMPLE_RATE 16000
#define BUF_SIZE 512

// Твои пины
#define MIC_WS 1
#define MIC_SCK 2
#define MIC_DIN 42
#define SPK_DOUT 39
#define SPK_BCLK 40
#define SPK_LRCK 41

// Wi-Fi (замени на свои)
#define WIFI_SSID "Fnet-900"
#define WIFI_PASS "gyux_modem"

i2s_chan_handle_t tx_handle = NULL;
i2s_chan_handle_t rx_handle = NULL;
httpd_handle_t server = NULL;
int ws_fd = -1; // Дескриптор для WebSocket

// --- HTML страница ---
const char* index_html = 
"<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ESP32 Intercom</title>"
"<style>"
"  body{font-family:sans-serif; text-align:center; background:#f0f0f0;}"
"  button{padding:20px; margin:10px; font-size:18px; width:80%; border-radius:10px; border:none; cursor:pointer; transition:0.3s;}"
"  .off{background:#ccc;} .active{background:#4CAF50; color:white; box-shadow:0 0 20px rgba(76,175,80,0.5);}"
"  .stop{background:#f44336; color:white;}"
"</style></head><body>"
"<h1>Audio Control</h1>"
"<button id='btnListen' class='off'>СЛУШАТЬ ESP</button><br>"
"<button id='btnTalk' class='off'>ГОВОРИТЬ В ESP</button><br>"
"<button id='btnStop' class='stop'>ВЫКЛЮЧИТЬ ВСЁ</button>"

"<script>"
"let ws; let audioCtx; let processor; let micStream;"
"const bListen = document.getElementById('btnListen');"
"const bTalk = document.getElementById('btnTalk');"
"const bStop = document.getElementById('btnStop');"

"async function stopAll() {"
"  if(ws) ws.close(); ws = null;"
"  if(micStream) micStream.getTracks().forEach(t => t.stop());"
"  if(processor) processor.disconnect();"
"  if(audioCtx) await audioCtx.close();"
"  audioCtx = null; bListen.className='off'; bTalk.className='off';"
"}"

"function initWS() {"
"  ws = new WebSocket(`ws://${location.host}/ws`); ws.binaryType = 'arraybuffer';"
"  ws.onmessage = (e) => {"
"    if(!audioCtx || bTalk.classList.contains('active')) return;"
"    const f32 = new Float32Array(e.data.byteLength/2); const view = new DataView(e.data);"
"    for(let i=0; i<f32.length; i++) f32[i] = view.getInt16(i*2, true)/32768;"
"    const buf = audioCtx.createBuffer(1, f32.length, 16000);"
"    buf.copyToChannel(f32, 0); const src = audioCtx.createBufferSource();"
"    src.buffer = buf; src.connect(audioCtx.destination); src.start();"
"  };"
"}"

"bListen.onclick = async () => {"
"  await stopAll(); audioCtx = new AudioContext({sampleRate:16000});"
"  initWS(); bListen.className='active';"
"};"

"bTalk.onclick = async () => {"
"  await stopAll(); audioCtx = new AudioContext({sampleRate:16000});"
"  initWS(); bTalk.className='active';"
"  micStream = await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true, sampleRate:16000}});"
"  const src = audioCtx.createMediaStreamSource(micStream);"
"  processor = audioCtx.createScriptProcessor(512, 1, 1);"
"  src.connect(processor); processor.connect(audioCtx.destination);"
"  processor.onaudioprocess = (e) => {"
"    if(ws && ws.readyState === 1) {"
"      const f32 = e.inputBuffer.getChannelData(0); const i16 = new Int16Array(f32.length);"
"      for(let i=0; i<f32.length; i++) i16[i] = f32[i] * 0x7FFF;"
"      ws.send(i16.buffer);"
"    }"
"  };"
"};"

"bStop.onclick = () => stopAll();"
"</script></body></html>";



// --- Обработчик WebSocket ---
esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // Если кто-то уже подключен, старый сокет закроется автоматически при записи
        ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Новое соединение. FD: %d", ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    // Узнаем размер
    if (httpd_ws_recv_frame(req, &ws_pkt, 0) != ESP_OK) return ESP_FAIL;

    if (ws_pkt.len > 0) {
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        ws_pkt.payload = buf;
        if (httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len) == ESP_OK) {
            
            // --- РЕГУЛИРОВКА ГРОМКОСТИ ---
            int16_t *samples = (int16_t *)ws_pkt.payload;
            int count = ws_pkt.len / 2;
            for (int i = 0; i < count; i++) {
                samples[i] = samples[i] >> 2; // В 4 раза тише, чтобы убрать искажения
            }
            
            size_t bw;
            i2s_channel_write(tx_handle, samples, ws_pkt.len, &bw, 100);
        }
        free(buf);
    }
    return ESP_OK;
}

// --- Задача чтения микрофона ---
void mic_task(void *p) {
    int32_t *raw = malloc(BUF_SIZE * 4);
    int16_t *out = malloc(BUF_SIZE * 2);
    size_t br;
    while(1) {
        if (i2s_channel_read(rx_handle, raw, BUF_SIZE * 4, &br, portMAX_DELAY) == ESP_OK) {
            int samples_count = br / 4;
            for(int i=0; i<samples_count; i++) {
                // Если тихо при >> 15, попробуй >> 14 (станет в 2 раза громче)
                out[i] = (int16_t)(raw[i] >> 14); 
            }
            if (ws_fd != -1) {
                httpd_ws_frame_t ws_pkt = { .payload = (uint8_t*)out, .len = samples_count*2, .type = HTTPD_WS_TYPE_BINARY };
                // Если отправка не удалась, сбрасываем ws_fd, чтобы не спамить в лог
                if (httpd_ws_send_frame_async(server, ws_fd, &ws_pkt) != ESP_OK) {
                    ws_fd = -1; 
                }
            }
        }
    }
}


// --- Настройка I2S ---
void init_audio() {
    i2s_chan_config_t m_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&m_cfg, NULL, &rx_handle);
    i2s_std_config_t m_std = { .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .bclk = MIC_SCK, .ws = MIC_WS, .din = MIC_DIN, .mclk = -1 } };
    m_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    i2s_channel_init_std_mode(rx_handle, &m_std);

    i2s_chan_config_t s_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&s_cfg, &tx_handle, NULL);
    i2s_std_config_t s_std = { .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .bclk = SPK_BCLK, .ws = SPK_LRCK, .dout = SPK_DOUT, .mclk = -1 } };
    i2s_channel_init_std_mode(tx_handle, &s_std);
    i2s_channel_enable(rx_handle); i2s_channel_enable(tx_handle);
}

// --- Настройка Wi-Fi ---
void wifi_init() {
    nvs_flash_init(); esp_netif_init(); esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_config_t wifi_cfg = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start(); esp_wifi_connect();
}

esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

// 1. Создай пустую функцию-заглушку  dlya favicon
// Поместить перед app_main
esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/x-icon");
    return httpd_resp_send(req, NULL, 0); 
}

void app_main() {
    wifi_init();
    init_audio();
    
    // Стек 4096 для микрофона — это ок, но для S3 лучше 8192, если будут лаги
    xTaskCreate(mic_task, "mic", 8192, NULL, 5, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;
    cfg.stack_size = 10240; // Еще немного накинем для стабильности WebSockets

    // 1. СНАЧАЛА ЗАПУСКАЕМ СЕРВЕР
    if (httpd_start(&server, &cfg) == ESP_OK) {
        
        // 2. ТЕПЕРЬ РЕГИСТРИРУЕМ ОБРАБОТЧИКИ
        
        // Главная страница
        httpd_uri_t hu = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = index_get_handler,
        };
        httpd_register_uri_handler(server, &hu);

        // WebSocket
        httpd_uri_t ws = {
            .uri          = "/ws",
            .method       = HTTP_GET,
            .handler      = ws_handler,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &ws);

        // Фавикон (чтобы не спамило ошибками)
        httpd_uri_t favicon_uri = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
        };
        httpd_register_uri_handler(server, &favicon_uri);

        ESP_LOGI(TAG, "Server started on port: '%d'", cfg.server_port);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
    }
}
