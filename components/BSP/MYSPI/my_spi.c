/**
 ****************************************************************************************************
 * @file        my_spi.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       MYSPI驱动代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
  * 实验平台:正点原子 ESP32-S3 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#include "my_spi.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdio.h>

static const char *TAG = "my_spi";
static sdmmc_card_t *s_sd_card;

/**
 * @brief       spi初始化
 * @param       无
 * @retval      esp_err_t
 */
esp_err_t my_spi_init(void)
{
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .sclk_io_num     = SPI_SCLK_PIN,    /* 时钟引脚 */
        .mosi_io_num     = SPI_MOSI_PIN,    /* 主机输出从机输入引脚 */
        .miso_io_num     = SPI_MISO_PIN,    /* 主机输入从机输出引脚 */
        .quadwp_io_num   = -1,              /* 用于Quad模式的WP引脚,未使用时设置为-1 */
        .quadhd_io_num   = -1,              /* 用于Quad模式的HD引脚,未使用时设置为-1 */
        .max_transfer_sz = 320 * 240 * sizeof(uint16_t),   /* 最大传输大小(整屏(RGB565格式)) */
    };
    /* 初始化SPI总线 */
    ret = spi_bus_initialize(MY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = MY_SPI_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = host.slot;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &s_sd_card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(MY_SPI_HOST);
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_sd_card);

    return ESP_OK;
}
