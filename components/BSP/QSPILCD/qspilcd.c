#include "qspilcd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lcd_tk018.h"
#include "xl9555.h"

#define QSPILCD_HOST              SPI3_HOST
#define QSPILCD_PIN_PCLK          GPIO_NUM_6
#define QSPILCD_PIN_CS            GPIO_NUM_16
#define QSPILCD_PIN_DATA0         GPIO_NUM_15
#define QSPILCD_PIN_DC            GPIO_NUM_7
#define QSPILCD_PIN_BACKLIGHT     GPIO_NUM_35

#define QSPILCD_TOUCH_PORT        I2C_NUM_1
#define QSPILCD_TOUCH_PIN_SCL     GPIO_NUM_5
#define QSPILCD_TOUCH_PIN_SDA     GPIO_NUM_4
#define QSPILCD_TOUCH_FREQ_HZ     100000

static const char *TAG = "qspilcd";
static esp_lcd_panel_io_handle_t s_lcd_io_handle;
static esp_lcd_panel_handle_t s_panel_handle;
static esp_lcd_touch_handle_t s_touch_handle;

esp_err_t qspilcd_init(void)
{
    const gpio_config_t backlight_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << QSPILCD_PIN_BACKLIGHT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG, "configure backlight failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(QSPILCD_PIN_BACKLIGHT, 0), TAG, "turn off backlight failed");

    const spi_bus_config_t bus_config = SPD2010_PANEL_BUS_SPI_CONFIG(
        QSPILCD_PIN_PCLK, QSPILCD_PIN_DATA0,
        QSPILCD_H_RES * QSPILCD_V_RES * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(QSPILCD_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "initialize SPI bus failed");

    const esp_lcd_panel_io_spi_config_t io_config =
        SPD2010_PANEL_IO_SPI_CONFIG(QSPILCD_PIN_CS, QSPILCD_PIN_DC, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(QSPILCD_HOST, &io_config, &s_lcd_io_handle), TAG, "create panel IO failed");

    const spd2010_vendor_config_t vendor_config = {
        .flags.use_qspi_interface = 0,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_tk018(s_lcd_io_handle, &panel_config, &s_panel_handle), TAG, "create SPD2010 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "reset panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "initialize panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "turn on panel failed");
    return gpio_set_level(QSPILCD_PIN_BACKLIGHT, 1);
}

esp_err_t qspilcd_touch_init(void)
{
    xl9555_pin_write(CT_RST_IO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9555_pin_write(CT_RST_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_bus_handle_t touch_i2c_bus = NULL;
    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = QSPILCD_TOUCH_PORT,
        .scl_io_num = QSPILCD_TOUCH_PIN_SCL,
        .sda_io_num = QSPILCD_TOUCH_PIN_SDA,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_config, &touch_i2c_bus), TAG, "initialize touch I2C bus failed");

    static const uint8_t touch_addresses[] = {0x15, 0x38};
    uint8_t touch_address = 0;
    for (size_t i = 0; i < sizeof(touch_addresses) / sizeof(touch_addresses[0]); i++)
    {
        if (i2c_master_probe(touch_i2c_bus, touch_addresses[i], 100) == ESP_OK)
        {
            touch_address = touch_addresses[i];
            ESP_LOGI(TAG, "touch controller found at I2C address 0x%02X", touch_address);
            break;
        }
    }
    if (touch_address == 0)
    {
        ESP_LOGE(TAG, "touch controller not found at I2C addresses 0x15 or 0x38");
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    touch_io_config.dev_addr = touch_address;
    touch_io_config.scl_speed_hz = QSPILCD_TOUCH_FREQ_HZ;
    esp_lcd_panel_io_handle_t touch_io_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(touch_i2c_bus, &touch_io_config, &touch_io_handle), TAG, "create touch IO failed");

    const esp_lcd_touch_config_t touch_config = {
        .x_max = QSPILCD_H_RES,
        .y_max = QSPILCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .mirror_y = 1,
        },
    };
    return esp_lcd_touch_new_i2c_ft5x06(touch_io_handle, &touch_config, &s_touch_handle);
}

esp_lcd_panel_io_handle_t qspilcd_get_io_handle(void)
{
    return s_lcd_io_handle;
}

esp_lcd_panel_handle_t qspilcd_get_panel_handle(void)
{
    return s_panel_handle;
}

esp_lcd_touch_handle_t qspilcd_get_touch_handle(void)
{
    return s_touch_handle;
}
