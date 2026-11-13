/**
 * @file lv_port_disp_template.c
 *
 */

/*Copy this file as "lv_port_disp.c" and set this value to "1" to enable content*/
#if 1

/*********************
 *      INCLUDES
 *********************/
#include "lv_port_disp.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/*********************
 *      DEFINES
 *********************/
#ifndef MY_DISP_HOR_RES
    #define MY_DISP_HOR_RES    320
#endif

#ifndef MY_DISP_VER_RES
    #define MY_DISP_VER_RES    240
#endif

#define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565)) /*will be 2 for RGB565 */

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void disp_init(void);

static void disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map);

/**********************
 *  STATIC VARIABLES
 **********************/
static uint32_t * s_framebuffer = NULL;
static HWND s_hwnd = NULL;
static BITMAPINFO s_bmi;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_port_disp_init(void)
{
    /*-------------------------
     * Initialize your display
     * -----------------------*/
    disp_init();

    /*------------------------------------
     * Create a display and set a flush_cb
     * -----------------------------------*/
    lv_display_t * disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);
    lv_display_set_flush_cb(disp, disp_flush);

    LV_ATTRIBUTE_MEM_ALIGN
    static uint8_t buf_1[MY_DISP_HOR_RES * 40 * BYTE_PER_PIXEL];
    lv_display_set_buffers(disp, buf_1, NULL, sizeof(buf_1), LV_DISPLAY_RENDER_MODE_PARTIAL);

}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/*Initialize your display and the required peripherals.*/
static void disp_init(void)
{
    s_framebuffer = (uint32_t *)calloc((size_t)MY_DISP_HOR_RES * (size_t)MY_DISP_VER_RES, sizeof(uint32_t));

    memset(&s_bmi, 0, sizeof(s_bmi));
    s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth = MY_DISP_HOR_RES;
    s_bmi.bmiHeader.biHeight = -MY_DISP_VER_RES;
    s_bmi.bmiHeader.biPlanes = 1;
    s_bmi.bmiHeader.biBitCount = 32;
    s_bmi.bmiHeader.biCompression = BI_RGB;
}

volatile bool disp_flush_enabled = true;

/* Enable updating the screen (the flushing process) when disp_flush() is called by LVGL
 */
void disp_enable_update(void)
{
    disp_flush_enabled = true;
}

/* Disable updating the screen (the flushing process) when disp_flush() is called by LVGL
 */
void disp_disable_update(void)
{
    disp_flush_enabled = false;
}

/*Flush the content of the internal buffer the specific area on the display.
 *`px_map` contains the rendered image as raw pixel map and it should be copied to `area` on the display.
 *You can use DMA or any hardware acceleration to do this operation in the background but
 *'lv_display_flush_ready()' has to be called when it's finished.*/
static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{
    if(s_framebuffer == NULL) {
        lv_display_flush_ready(disp_drv);
        return;
    }

    int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t x2 = area->x2 >= MY_DISP_HOR_RES ? (MY_DISP_HOR_RES - 1) : area->x2;
    int32_t y2 = area->y2 >= MY_DISP_VER_RES ? (MY_DISP_VER_RES - 1) : area->y2;

    if(disp_flush_enabled) {
        lv_color_t * src = (lv_color_t *)px_map;
        int32_t src_w = area->x2 - area->x1 + 1;

        for(int32_t y = y1; y <= y2; y++) {
            for(int32_t x = x1; x <= x2; x++) {
                int32_t src_idx = (y - area->y1) * src_w + (x - area->x1);
                int32_t dst_idx = y * MY_DISP_HOR_RES + x;
                s_framebuffer[dst_idx] = lv_color_to_u32(src[src_idx]);
            }
        }

        if(s_hwnd == NULL || !IsWindow(s_hwnd)) {
            s_hwnd = GetForegroundWindow();
        }

        if(s_hwnd != NULL) {
            HDC hdc = GetDC(s_hwnd);
            if(hdc != NULL) {
                StretchDIBits(
                    hdc,
                    0,
                    0,
                    MY_DISP_HOR_RES,
                    MY_DISP_VER_RES,
                    0,
                    0,
                    MY_DISP_HOR_RES,
                    MY_DISP_VER_RES,
                    s_framebuffer,
                    &s_bmi,
                    DIB_RGB_COLORS,
                    SRCCOPY);
                ReleaseDC(s_hwnd, hdc);
            }
        }
    }

    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_display_flush_ready(disp_drv);
}

#else /*Enable this file at the top*/

/*This dummy typedef exists purely to silence -Wpedantic.*/
typedef int keep_pedantic_happy;
#endif
