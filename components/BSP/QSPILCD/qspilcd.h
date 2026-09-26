#ifndef QSPILCD_H
#define QSPILCD_H

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#define QSPILCD_H_RES             240
#define QSPILCD_V_RES             300
/*
 * LVGL 绘图缓冲必须放在连续的内部 DMA 内存中。
 * 40 行约占 19.2 KiB，既高于屏幕面积的 1/10，又能给 Wi-Fi 等组件留出足够内存。
 */
#define QSPILCD_DRAW_BUFF_HEIGHT   40

esp_err_t qspilcd_init(void);
esp_err_t qspilcd_touch_init(void);

esp_lcd_panel_io_handle_t qspilcd_get_io_handle(void);
esp_lcd_panel_handle_t qspilcd_get_panel_handle(void);
esp_lcd_touch_handle_t qspilcd_get_touch_handle(void);

#endif
