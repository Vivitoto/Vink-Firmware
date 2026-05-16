#include <stdint.h>

#include "epd_board.h"
#include "epdiy.h"

#include "../output_lcd/lcd_driver.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#ifndef CONFIG_IDF_TARGET_ESP32S3
#define GPIO_NUM_6  -1
#define GPIO_NUM_7  -1
#define GPIO_NUM_8  -1
#define GPIO_NUM_9  -1
#define GPIO_NUM_10 -1
#define GPIO_NUM_11 -1
#define GPIO_NUM_12 -1
#define GPIO_NUM_13 -1
#define GPIO_NUM_14 -1
#define GPIO_NUM_15 -1
#define GPIO_NUM_16 -1
#define GPIO_NUM_17 -1
#define GPIO_NUM_18 -1
#define GPIO_NUM_45 -1
#define GPIO_NUM_46 -1
#endif

#define TAG "epdiy-m5papers3"

// M5GFX PaperS3 pin map from M5GFX.cpp board_M5PaperS3 plus the
// official schematic/product page:
// data[0..7] = 6,14,7,12,9,11,8,10; spv=17, ckv=18, sph/XSTL=13,
// le/XLE=15, cl/XCL=16, bus_width=8, pclk=16 MHz.
// Important naming detail: the product page labels GPIO45 as PWR, but the
// ED047TC1 datasheet calls that panel-side signal XOE and M5GFX names it
// pin_oe. GPIO46 is the schematic BST_EN net and M5GFX's pin_pwr.
#define M5P_D0     GPIO_NUM_6
#define M5P_D1     GPIO_NUM_14
#define M5P_D2     GPIO_NUM_7
#define M5P_D3     GPIO_NUM_12
#define M5P_D4     GPIO_NUM_9
#define M5P_D5     GPIO_NUM_11
#define M5P_D6     GPIO_NUM_8
#define M5P_D7     GPIO_NUM_10
#define M5P_BST_EN GPIO_NUM_46
#define M5P_SPV    GPIO_NUM_17
#define M5P_CKV    GPIO_NUM_18
#define M5P_SPH    GPIO_NUM_13
#define M5P_XOE    GPIO_NUM_45
#define M5P_LE     GPIO_NUM_15
#define M5P_CL     GPIO_NUM_16

static lcd_bus_config_t lcd_config = {
    .clock = M5P_CL,
    .ckv = M5P_CKV,
    .start_pulse = M5P_SPH,
    .leh = M5P_LE,
    .stv = M5P_SPV,
    .data[0] = M5P_D0,
    .data[1] = M5P_D1,
    .data[2] = M5P_D2,
    .data[3] = M5P_D3,
    .data[4] = M5P_D4,
    .data[5] = M5P_D5,
    .data[6] = M5P_D6,
    .data[7] = M5P_D7,
};

static void set_output(gpio_num_t pin, int level) {
    if ((int)pin < 0) return;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

static void epd_board_init(uint32_t epd_row_width) {
    (void)epd_row_width;

    set_output(M5P_BST_EN, 0);
    set_output(M5P_XOE, 0);
    set_output(M5P_SPV, 0);
    set_output(M5P_CKV, 1);
    set_output(M5P_SPH, 1);
    set_output(M5P_LE, 0);
    set_output(M5P_CL, 0);

    const EpdDisplay_t* display = epd_get_display();
    LcdEpdConfig_t config = {
        // Use the M5GFX-validated PaperS3 LCD clock first. ED047TC1 declares
        // 20 MHz, but the official M5GFX PaperS3 path uses 16 MHz on this board.
        .pixel_clock = 16 * 1000 * 1000,
        .ckv_high_time = 60,
        .line_front_porch = 4,
        .le_high_time = 4,
        .bus_width = display->bus_width,
        .bus = lcd_config,
    };
    ESP_LOGI(TAG, "init PaperS3 epdiy LCD backend %dx%d bus=%d pclk=%d", display->width,
             display->height, display->bus_width, (int)config.pixel_clock);
    epd_lcd_init(&config, display->width, display->height);
}

static void epd_board_deinit(void) {
    epd_lcd_deinit();
    set_output(M5P_XOE, 0);
    set_output(M5P_BST_EN, 0);
    set_output(M5P_SPV, 0);
}

static void epd_board_set_ctrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
    // PaperS3 exposes XOE plus the LCD timing pins; no separate EPD MODE pin is
    // configured in the M5GFX board profile. EPDiy's epd_set_mode() toggles both
    // ep_output_enable and ep_mode, so map either request to XOE.
    if (mask->ep_output_enable || mask->ep_mode) {
        gpio_set_level(M5P_XOE, (state->ep_output_enable || state->ep_mode) ? 1 : 0);
    }
    if (mask->ep_stv) {
        gpio_set_level(M5P_SPV, state->ep_stv ? 1 : 0);
    }
    if (mask->ep_sth) {
        gpio_set_level(M5P_SPH, state->ep_sth ? 1 : 0);
    }
    if (mask->ep_latch_enable) {
        gpio_set_level(M5P_LE, state->ep_latch_enable ? 1 : 0);
    }
}

static void epd_board_poweron(epd_ctrl_state_t* state) {
    if (!state) return;
    gpio_set_level(M5P_XOE, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(M5P_BST_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(M5P_SPV, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    state->ep_output_enable = true;
    state->ep_mode = false;
    state->ep_stv = true;
    state->ep_sth = true;
}

static void epd_board_poweroff(epd_ctrl_state_t* state) {
    if (!state) return;
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(M5P_BST_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(M5P_XOE, 0);
    gpio_set_level(M5P_SPV, 0);

    state->ep_output_enable = false;
    state->ep_mode = false;
    state->ep_stv = false;
}

static float epd_board_ambient_temperature(void) {
    return 21.0f;
}

const EpdBoardDefinition epd_board_m5papers3 = {
    .init = epd_board_init,
    .deinit = epd_board_deinit,
    .set_ctrl = epd_board_set_ctrl,
    .poweron = epd_board_poweron,
    .measure_vcom = NULL,
    .poweroff = epd_board_poweroff,
    .set_vcom = NULL,
    .get_temperature = epd_board_ambient_temperature,
    .gpio_set_direction = NULL,
    .gpio_read = NULL,
    .gpio_write = NULL,
};
