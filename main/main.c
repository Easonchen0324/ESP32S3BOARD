/**
 ******************************************************************************
 * @file        main.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       SPILCD实验
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ******************************************************************************
 * @attention
 * 
 * 实验平台:正点原子 ESP32-S3 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 ******************************************************************************
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "led.h"
#include "myiic.h"
#include "my_spi.h"
#include "spilcd.h"
#include "xl9555.h"
#include <stdio.h>


/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    esp_err_t ret;
    uint8_t x = 0;

    ret = nvs_flash_init();     /* 初始化NVS */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    led_init();                 /* LED初始化 */
    my_spi_init();              /* SPI初始化 */
    myiic_init();               /* IIC初始化 */  
    xl9555_init();              /* 初始化按键 */
    spilcd_init();              /* LCD屏初始化 */
 
    while (1)
    {
        switch (x)
        {
            case 0:
            {
                spilcd_clear(WHITE);
                break;
            }
            case 1:
            {
                spilcd_clear(BLACK);
                break;
            }
            case 2:
            {
                spilcd_clear(BLUE);
                break;
            }
            case 3:
            {
                spilcd_clear(RED);
                break;
            }
            case 4:
            {
                spilcd_clear(MAGENTA);
                break;
            }
            case 5:
            {
                spilcd_clear(GREEN);
                break;
            }
            case 6:
            {
                spilcd_clear(CYAN);
                break;
            }
            case 7:
            {
                spilcd_clear(YELLOW);
                break;
            }
            case 8:
            {
                spilcd_clear(BRRED);
                break;
            }
            case 9:
            {
                spilcd_clear(GRAY);
                break;
            }
            case 10:
            {
                spilcd_clear(LGRAY);
                break;
            }
            case 11:
            {
                spilcd_clear(BROWN);
                break;
            }
        }

        spilcd_show_string(10, 40, 240, 32, 32, "ESP32-S3", RED);
        spilcd_show_string(10, 80, 240, 24, 24, "SPILCD TEST", RED);
        spilcd_show_string(10, 110, 240, 16, 16, "ATOM@ALIENTEK", RED);
        x++;

        if (x == 12)
        {
            x = 0;
        }

        LED0_TOGGLE();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
