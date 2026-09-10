#pragma once

#include <stdint.h>

#include "esp_lcd_panel_vendor.h"

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} spd2010_lcd_init_cmd_t;

typedef struct {
    const spd2010_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int use_qspi_interface: 1;
    } flags;
} spd2010_vendor_config_t;

esp_err_t esp_lcd_new_panel_tk018(const esp_lcd_panel_io_handle_t io,
                                  const esp_lcd_panel_dev_config_t *panel_dev_config,
                                  esp_lcd_panel_handle_t *ret_panel);

#define SPD2010_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                       \
        .sclk_io_num = sclk,                                                \
        .data0_io_num = d0,                                                 \
        .data1_io_num = d1,                                                 \
        .data2_io_num = d2,                                                 \
        .data3_io_num = d3,                                                 \
        .max_transfer_sz = max_trans_sz,                                    \
    }

#define SPD2010_PANEL_IO_QSPI_CONFIG(cs, cb, cb_ctx)  \
    {                                                  \
        .cs_gpio_num = cs,                             \
        .dc_gpio_num = -1,                             \
        .spi_mode = 3,                                 \
        .pclk_hz = 25 * 1000 * 1000,                   \
        .trans_queue_depth = 10,                       \
        .on_color_trans_done = cb,                     \
        .user_ctx = cb_ctx,                            \
        .lcd_cmd_bits = 32,                            \
        .lcd_param_bits = 8,                           \
        .flags = { .quad_mode = true },                \
    }
