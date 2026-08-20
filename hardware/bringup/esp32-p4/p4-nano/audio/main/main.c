#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "es8311.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "p4_nano_audio"

#define AUDIO_I2C_PORT I2C_NUM_0
#define AUDIO_I2C_SDA GPIO_NUM_7
#define AUDIO_I2C_SCL GPIO_NUM_8
#define AUDIO_I2C_ADDRESS 0x18

#define AUDIO_I2S_PORT I2S_NUM_0
#define AUDIO_I2S_MCLK GPIO_NUM_13
#define AUDIO_I2S_BCLK GPIO_NUM_12
#define AUDIO_I2S_WS GPIO_NUM_10
#define AUDIO_I2S_DOUT GPIO_NUM_9

#define AUDIO_PA_GPIO GPIO_NUM_53
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_TONE_FREQUENCY 1000
#define AUDIO_MCLK_FREQUENCY (AUDIO_SAMPLE_RATE * 256)
#define AUDIO_CODEC_VOLUME 55
#define AUDIO_TONE_AMPLITUDE 4096
#define AUDIO_TONE_FRAMES AUDIO_SAMPLE_RATE
#define AUDIO_SILENCE_FRAMES AUDIO_SAMPLE_RATE
#define AUDIO_FRAME_BYTES 4
#define AUDIO_CHUNK_FRAMES 1024
#define AUDIO_WRITE_TIMEOUT_MS 500
#define AUDIO_MAX_PARTIAL_WRITES 8
#define AUDIO_PA_STARTUP_DELAY_MS 150

/* One exact 1 kHz period at 48 kHz, with a peak of 4096. */
static const int16_t s_tone_period[48] = {
    0,    535,  1060, 1567, 2048, 2508, 2896, 3276,
    3547, 3783, 3958, 4076, 4096, 4076, 3958, 3783,
    3547, 3276, 2896, 2508, 2048, 1567, 1060, 535,
    0,    -535, -1060, -1567, -2048, -2508, -2896, -3276,
    -3547, -3783, -3958, -4076, -4096, -4076, -3958, -3783,
    -3547, -3276, -2896, -2508, -2048, -1567, -1060, -535,
};

static es8311_handle_t s_codec;
static i2s_chan_handle_t s_tx;
static bool s_pa_configured;
static bool s_codec_ready;
static bool s_codec_muted;
static bool s_i2c_installed;
static bool s_i2s_enabled;
static int16_t s_pcm_buffer[AUDIO_CHUNK_FRAMES * 2];
static size_t s_tone_frames_written;
static size_t s_silence_frames_written;

static esp_err_t crc32_update(uint32_t *crc, const uint8_t *data, size_t length)
{
    if (crc == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (length-- > 0) {
        *crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) {
            *crc = (*crc & 1U) != 0U ? (*crc >> 1U) ^ 0xEDB88320U : *crc >> 1U;
        }
    }
    return ESP_OK;
}

static uint32_t tone_crc32(void)
{
    uint32_t crc = UINT32_MAX;
    for (size_t frame = 0; frame < AUDIO_TONE_FRAMES; ++frame) {
        const uint16_t sample = (uint16_t)s_tone_period[frame % 48];
        const uint8_t pcm[4] = {
            (uint8_t)(sample & 0xffU),
            (uint8_t)(sample >> 8U),
            (uint8_t)(sample & 0xffU),
            (uint8_t)(sample >> 8U),
        };
        ESP_ERROR_CHECK(crc32_update(&crc, pcm, sizeof(pcm)));
    }
    return crc ^ UINT32_MAX;
}

static void fill_pcm(uint8_t *buffer, size_t frames, size_t frame_offset, bool tone)
{
    int16_t *samples = (int16_t *)buffer;
    for (size_t frame = 0; frame < frames; ++frame) {
        const int16_t sample = tone ? s_tone_period[(frame_offset + frame) % 48] : 0;
        samples[frame * 2] = sample;
        samples[frame * 2 + 1] = sample;
    }
}

static esp_err_t configure_pa_safe_off(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << AUDIO_PA_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gpio_set_level(AUDIO_PA_GPIO, 0);
    if (ret == ESP_OK) {
        s_pa_configured = true;
        ESP_LOGI(TAG, "P4-NANO AUDIO PA SAFE-OFF: PASS");
    }
    return ret;
}

static esp_err_t init_i2c(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = AUDIO_I2C_SDA,
        .scl_io_num = AUDIO_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t ret = i2c_param_config(AUDIO_I2C_PORT, &config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2c_driver_install(AUDIO_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_OK) {
        s_i2c_installed = true;
    }
    return ret;
}

static esp_err_t detect_es8311(void)
{
    uint8_t register_address = 0x00;
    uint8_t register_value = 0;
    return i2c_master_write_read_device(
        AUDIO_I2C_PORT,
        AUDIO_I2C_ADDRESS,
        &register_address,
        sizeof(register_address),
        &register_value,
        sizeof(register_value),
        pdMS_TO_TICKS(100));
}

static esp_err_t init_codec(void)
{
    s_codec = es8311_create(AUDIO_I2C_PORT, AUDIO_I2C_ADDRESS);
    if (s_codec == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const es8311_clock_config_t clock_config = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = AUDIO_MCLK_FREQUENCY,
        .sample_frequency = AUDIO_SAMPLE_RATE,
    };
    esp_err_t ret = es8311_init(s_codec, &clock_config, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = es8311_sample_frequency_config(s_codec, AUDIO_MCLK_FREQUENCY, AUDIO_SAMPLE_RATE);
    if (ret != ESP_OK) {
        return ret;
    }
    s_codec_ready = true;
    return ESP_OK;
}

static esp_err_t configure_codec(void)
{
    esp_err_t ret = es8311_voice_mute(s_codec, true);
    if (ret != ESP_OK) {
        return ret;
    }
    s_codec_muted = true;
    ESP_LOGI(TAG, "P4-NANO AUDIO CODEC MUTE: PASS");

    int actual_volume = -1;
    ret = es8311_voice_volume_set(s_codec, AUDIO_CODEC_VOLUME, &actual_volume);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "P4-NANO AUDIO CODEC VOLUME: PASS requested=%d actual=%d",
                 AUDIO_CODEC_VOLUME, actual_volume);
    }
    return ret;
}

static esp_err_t init_i2s(void)
{
    const i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&channel_config, &s_tx, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,
            .bclk = AUDIO_I2S_BCLK,
            .ws = AUDIO_I2S_WS,
            .dout = AUDIO_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    standard_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ret = i2s_channel_init_std_mode(s_tx, &standard_config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2s_channel_enable(s_tx);
    if (ret == ESP_OK) {
        s_i2s_enabled = true;
        ESP_LOGI(TAG, "P4-NANO AUDIO I2S INIT: PASS controller=0 sample_rate=%d mclk=%d format=philips/stereo/16bit",
                 AUDIO_SAMPLE_RATE, AUDIO_MCLK_FREQUENCY);
    }
    return ret;
}

static esp_err_t write_pcm_frames(bool tone, size_t frames, size_t *written_frames)
{
    size_t frame_offset = 0;

    if (written_frames != NULL) {
        *written_frames = 0;
    }

    while (frame_offset < frames) {
        const size_t chunk_frames = (frames - frame_offset) > AUDIO_CHUNK_FRAMES
                                         ? AUDIO_CHUNK_FRAMES
                                         : frames - frame_offset;
        const size_t chunk_bytes = chunk_frames * AUDIO_FRAME_BYTES;
        fill_pcm((uint8_t *)s_pcm_buffer, chunk_frames, frame_offset, tone);

        size_t chunk_offset = 0;
        unsigned partial_writes = 0;
        while (chunk_offset < chunk_bytes) {
            size_t bytes_written = 0;
            esp_err_t ret = i2s_channel_write(
                s_tx,
                (uint8_t *)s_pcm_buffer + chunk_offset,
                chunk_bytes - chunk_offset,
                &bytes_written,
                pdMS_TO_TICKS(AUDIO_WRITE_TIMEOUT_MS));
            if (ret != ESP_OK) {
                return ret;
            }
            if (bytes_written == 0 || (bytes_written % AUDIO_FRAME_BYTES) != 0) {
                return ESP_ERR_INVALID_SIZE;
            }
            chunk_offset += bytes_written;
            if (++partial_writes > AUDIO_MAX_PARTIAL_WRITES) {
                return ESP_ERR_TIMEOUT;
            }
        }
        frame_offset += chunk_frames;
        if (tone) {
            s_tone_frames_written += chunk_frames;
        } else {
            s_silence_frames_written += chunk_frames;
        }
        if (written_frames != NULL) {
            *written_frames += chunk_frames;
        }
    }
    return ESP_OK;
}

static void delete_audio_devices(void)
{
    if (s_i2s_enabled) {
        (void)i2s_channel_disable(s_tx);
        s_i2s_enabled = false;
    }
    if (s_tx != NULL) {
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_codec != NULL) {
        es8311_delete(s_codec);
        s_codec = NULL;
        s_codec_ready = false;
    }
    if (s_i2c_installed) {
        (void)i2c_driver_delete(AUDIO_I2C_PORT);
        s_i2c_installed = false;
    }
}

static void safe_shutdown(void)
{
    if (s_codec_ready && !s_codec_muted) {
        (void)es8311_voice_mute(s_codec, true);
        s_codec_muted = true;
    }
    if (s_pa_configured) {
        (void)gpio_set_level(AUDIO_PA_GPIO, 0);
    }
    delete_audio_devices();
}

static esp_err_t finish_audio(void)
{
    esp_err_t first_error = ESP_OK;

    esp_err_t ret = es8311_voice_mute(s_codec, true);
    if (ret == ESP_OK) {
        s_codec_muted = true;
        ESP_LOGI(TAG, "P4-NANO AUDIO CODEC FINAL MUTE: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO AUDIO CODEC FINAL MUTE: FAIL (%s)", esp_err_to_name(ret));
        first_error = ret;
    }

    ret = write_pcm_frames(false, AUDIO_SILENCE_FRAMES / 10, NULL);
    if (ret != ESP_OK && first_error == ESP_OK) {
        first_error = ret;
    }

    ret = gpio_set_level(AUDIO_PA_GPIO, 0);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "P4-NANO AUDIO PA DISABLE: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO AUDIO PA DISABLE: FAIL (%s)", esp_err_to_name(ret));
        if (first_error == ESP_OK) {
            first_error = ret;
        }
    }

    delete_audio_devices();
    return first_error;
}

static esp_err_t run_audio_test(void)
{
    s_tone_frames_written = 0;
    s_silence_frames_written = 0;

    esp_err_t ret = configure_pa_safe_off();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO PA SAFE-OFF: FAIL (%s)", esp_err_to_name(ret));
        return ret;
    }

    ret = init_i2c();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO I2C INIT: FAIL (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "P4-NANO AUDIO I2C INIT: PASS controller=0 sda=7 scl=8");

    ret = detect_es8311();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO ES8311 DETECT: FAIL address=0x18 (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "P4-NANO AUDIO ES8311 DETECT: PASS address=0x18");

    ret = init_codec();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO CODEC INIT: FAIL (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "P4-NANO AUDIO CODEC INIT: PASS mclk=%d sample_rate=%d resolution=16",
             AUDIO_MCLK_FREQUENCY, AUDIO_SAMPLE_RATE);

    ret = configure_codec();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO CODEC CONFIG: FAIL (%s)", esp_err_to_name(ret));
        return ret;
    }

    ret = init_i2s();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO I2S INIT: FAIL (%s)", esp_err_to_name(ret));
        return ret;
    }

    const uint32_t crc = tone_crc32();
    ESP_LOGI(TAG, "P4-NANO AUDIO PCM CONFIG: sample_rate=%d tone_hz=%d amplitude=%d frames_per_tone=%d bytes_per_tone=%d total_tone_frames=%d total_tone_bytes=%d total_pcm_bytes=%d",
             AUDIO_SAMPLE_RATE, AUDIO_TONE_FREQUENCY, AUDIO_TONE_AMPLITUDE,
             AUDIO_TONE_FRAMES, AUDIO_TONE_FRAMES * AUDIO_FRAME_BYTES,
             AUDIO_TONE_FRAMES * 3, AUDIO_TONE_FRAMES * AUDIO_FRAME_BYTES * 3,
             (AUDIO_TONE_FRAMES + AUDIO_SILENCE_FRAMES) * AUDIO_FRAME_BYTES * 3);
    ESP_LOGI(TAG, "P4-NANO AUDIO PCM CRC32: 0x%08" PRIx32, crc);

    size_t silence_frames = 0;
    ret = write_pcm_frames(false, AUDIO_SAMPLE_RATE / 10, &silence_frames);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO SILENCE PRIME: FAIL (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "P4-NANO AUDIO SILENCE PRIME: PASS frames=%u", (unsigned)silence_frames);

    ret = gpio_set_level(AUDIO_PA_GPIO, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO PA ENABLE: FAIL (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "P4-NANO AUDIO PA ENABLE: PASS");
    vTaskDelay(pdMS_TO_TICKS(AUDIO_PA_STARTUP_DELAY_MS));

    ret = es8311_voice_mute(s_codec, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO AUDIO CODEC UNMUTE: FAIL (%s)", esp_err_to_name(ret));
        return ret;
    }
    s_codec_muted = false;

    for (int interval = 1; interval <= 3; ++interval) {
        size_t tone_frames = 0;
        ret = write_pcm_frames(true, AUDIO_TONE_FRAMES, &tone_frames);
        if (ret != ESP_OK || tone_frames != AUDIO_TONE_FRAMES) {
            ESP_LOGE(TAG, "P4-NANO AUDIO TONE #%d: FAIL (%s) frames=%u",
                     interval, ret == ESP_OK ? "incomplete" : esp_err_to_name(ret),
                     (unsigned)tone_frames);
            return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
        }
        ESP_LOGI(TAG, "P4-NANO AUDIO TONE #%d: PASS frames=48000 bytes=192000", interval);

        size_t interval_silence = 0;
        ret = write_pcm_frames(false, AUDIO_SILENCE_FRAMES, &interval_silence);
        if (ret != ESP_OK || interval_silence != AUDIO_SILENCE_FRAMES) {
            ESP_LOGE(TAG, "P4-NANO AUDIO SILENCE #%d: FAIL (%s) frames=%u",
                     interval, ret == ESP_OK ? "incomplete" : esp_err_to_name(ret),
                     (unsigned)interval_silence);
            return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
        }
    }

    ret = finish_audio();
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "P4-NANO AUDIO PCM WRITE: PASS");
    ESP_LOGI(TAG, "P4-NANO AUDIO PCM COUNTS: tone_frames=%u tone_bytes=%u silence_frames=%u silence_bytes=%u total_bytes=%u",
             (unsigned)s_tone_frames_written,
             (unsigned)(s_tone_frames_written * AUDIO_FRAME_BYTES),
             (unsigned)s_silence_frames_written,
             (unsigned)(s_silence_frames_written * AUDIO_FRAME_BYTES),
             (unsigned)((s_tone_frames_written + s_silence_frames_written) * AUDIO_FRAME_BYTES));
    ESP_LOGI(TAG, "P4-NANO AUDIO DIGITAL RESULT: PASS");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "P4-NANO AUDIO DIAGNOSTIC START");
    const esp_err_t ret = run_audio_test();
    if (ret != ESP_OK) {
        safe_shutdown();
        ESP_LOGE(TAG, "P4-NANO AUDIO DIGITAL RESULT: FAIL (%s)", esp_err_to_name(ret));
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
