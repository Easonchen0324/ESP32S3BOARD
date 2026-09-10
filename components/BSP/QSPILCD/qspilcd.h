#ifndef QSPILCD_H
#define QSPILCD_H

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#define QSPILCD_H_RES             240
#define QSPILCD_V_RES             300
#define QSPILCD_DRAW_BUFF_HEIGHT   80

esp_err_t qspilcd_init(void);
esp_err_t qspilcd_touch_init(void);

esp_lcd_panel_io_handle_t qspilcd_get_io_handle(void);
esp_lcd_panel_handle_t qspilcd_get_panel_handle(void);
esp_lcd_touch_handle_t qspilcd_get_touch_handle(void);

#endif
