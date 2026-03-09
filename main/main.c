#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h

// Ваши модули
#include "audio_handler.h"
//#include "camera_handler.h"

#define TAG "MAIN_APP"

// Глобальные переменные (они объявлены как extern в audio_handler.h)
i2s_chan_handle_t tx_handle = NULL;
i2s_chan_handle_t rx_handle = NULL;
int ws_fd = -1;
httpd_handle_t server = NULL;

// Подключаем HTML из папки /data (через CMakeLists.txt)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Обработчик главной страницы
esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

// Функция инициализации Wi-Fi (если она не вынесена в другой файл)
void wifi_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Fnet-900",
            .password = "gyux_modem",
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();
}
// Заглушка для иконки, чтобы браузер не выдавал ошибку 404
esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/x-icon");
    return httpd_resp_send(req, NULL, 0); // Отправляем пустой ответ
}

void app_main() {
    // 1. Система и Сеть
    wifi_init();

    // 2. Железо (Камера и Аудио)
    init_audio();   // Функция из audio_handler.c
    //init_camera();  // Функция из camera_handler.c

    // 3. Задачи FreeRTOS
    // Создаем ТОЛЬКО ОДНУ задачу для микрофона
    xTaskCreate(mic_task, "mic_task", 8192, NULL, 5, NULL);

    // 4. Запуск Сервера
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240;
    config.lru_purge_enable = true; // Важно для стабильности WebSocket
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Главная страница
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler };
        httpd_register_uri_handler(server, &uri_root);

        // WebSocket для звука
        httpd_uri_t uri_ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
        httpd_register_uri_handler(server, &uri_ws);
        
        // Тут можно добавить регистратор для камеры (camera_stream_handler)
		httpd_uri_t uri_favicon = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_favicon);
        
        ESP_LOGI(TAG, "Server is up with Favicon handler!");        
        ESP_LOGI(TAG, "Server is up and running!");
    } else {
        ESP_LOGE(TAG, "Failed to start server!");
    }
}
