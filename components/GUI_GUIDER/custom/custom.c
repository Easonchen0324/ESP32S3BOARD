/*
* Copyright 2024 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/


/*********************
 *      INCLUDES
 *********************/
#include <stdio.h>
#include "lvgl.h"
#include "custom.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void main_screen_btn_1_event_cb(lv_event_t *event);

/**********************
 *  STATIC VARIABLES
 **********************/

/**
 * Create a demo application
 */

void custom_init(lv_ui *ui)
{
    lv_obj_add_event_cb(ui->main_screen_btn_1, main_screen_btn_1_event_cb, LV_EVENT_CLICKED, ui);
}

static void main_screen_btn_1_event_cb(lv_event_t *event)
{
    lv_ui *ui = lv_event_get_user_data(event);

    if (ui->screen_1_del)
    {
        setup_scr_screen_1(ui);
        ui->screen_1_del = false;
    }

    lv_screen_load(ui->screen_1);
}
