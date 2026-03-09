#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#define TAG "AUDIO_S3_STABLE"
#define SAMPLE_RATE 16000
#define BUF_SIZE 1024

// Ваши пины из первого сообщения
#define MIC_WS    GPIO_NUM_1
#define MIC_SCK   GPIO_NUM_2
#define MIC_DIN   GPIO_NUM_42

#define SPK_DOUT  GPIO_NUM_39
#define SPK_BCLK  GPIO_NUM_40
#define SPK_LRCK  GPIO_NUM_41

void app_main(void)
{
	i2s_chan_handle_t tx_handle = NULL; // Хендл для динамика
    i2s_chan_handle_t rx_handle = NULL; // Хендл для микрофона
    // --- Конфиг МИКРОФОНА (32 бита) ---
    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&mic_chan_cfg, NULL, &rx_handle);
	i2s_std_config_t mic_std_cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
		.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
		.gpio_cfg = { .mclk = -1, .bclk = MIC_SCK, .ws = MIC_WS, .din = MIC_DIN }
	};
	// Важно: принудительно слушаем левый канал (т.к. L/R на GND)
	mic_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; 
	i2s_channel_init_std_mode(rx_handle, &mic_std_cfg);


    // --- Конфиг ДИНАМИКА (16 бит) ---
    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&spk_chan_cfg, &tx_handle, NULL);

	i2s_std_config_t spk_std_cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
		.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
		.gpio_cfg = { .mclk = -1, .bclk = SPK_BCLK, .ws = SPK_LRCK, .dout = SPK_DOUT }
	};
	spk_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
	i2s_channel_init_std_mode(tx_handle, &spk_std_cfg);


    i2s_channel_enable(rx_handle);
    i2s_channel_enable(tx_handle);

    // Буферы: читаем 32-битные числа, отправляем 16-битные
    int32_t *mic_ptr = malloc(BUF_SIZE * 4); 
    int16_t *spk_ptr = malloc(BUF_SIZE * 2);
    size_t bytes_read, bytes_written;

	while (1) {
		if (i2s_channel_read(rx_handle, mic_ptr, BUF_SIZE * 4, &bytes_read, portMAX_DELAY) == ESP_OK) {
			int samples = bytes_read / 4;

			for (int i = 0; i < samples; i++) {
				// Попробуй именно 16, чтобы выровнять 24-битные данные под 16-битный выход
				spk_ptr[i] = (int16_t)(mic_ptr[i] >> 16); 
			}

			i2s_channel_write(tx_handle, spk_ptr, samples * 2, &bytes_written, portMAX_DELAY);
		}
	}

}