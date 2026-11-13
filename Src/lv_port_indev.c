/**
 * @file lv_port_indev.c
 */

#if 1

#include "lv_port_indev.h"
#include <stdbool.h>
#include <windows.h>

#ifndef MY_DISP_HOR_RES
#define MY_DISP_HOR_RES 480
#endif

#ifndef MY_DISP_VER_RES
#define MY_DISP_VER_RES 320
#endif

static void mouse_read(lv_indev_t * indev, lv_indev_data_t * data);

static lv_indev_t * indev_mouse = NULL;

void lv_port_indev_init(void)
{
    indev_mouse = lv_indev_create();
    lv_indev_set_type(indev_mouse, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_mouse, mouse_read);
}

static void mouse_read(lv_indev_t * indev, lv_indev_data_t * data)
{
    LV_UNUSED(indev);

    static POINT last_pt = {0, 0};
    POINT pt;
    HWND hwnd = GetForegroundWindow();

    if(hwnd != NULL && GetCursorPos(&pt)) {
        ScreenToClient(hwnd, &pt);
        if(pt.x < 0) pt.x = 0;
        if(pt.y < 0) pt.y = 0;
        if(pt.x >= MY_DISP_HOR_RES) pt.x = MY_DISP_HOR_RES - 1;
        if(pt.y >= MY_DISP_VER_RES) pt.y = MY_DISP_VER_RES - 1;
        last_pt = pt;
    }

    data->point.x = (lv_coord_t)last_pt.x;
    data->point.y = (lv_coord_t)last_pt.y;
    data->state = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

#else

typedef int keep_pedantic_happy;

#endif
