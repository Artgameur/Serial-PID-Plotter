#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "app_window.h"
#include "screens.h"
#include "settings.h"
#include "profile.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

int main()
{
    lv_init();
    int32_t zoom_level = 100;
    bool allow_dpi_override = false;
    bool simulator_mode = false;
    lv_display_t* display = lv_windows_create_display(
        L"Serial Dashboard",
        SCREEN_WIDTH, SCREEN_HEIGHT,
        zoom_level, allow_dpi_override, simulator_mode);
    if (!display) return -1;

    // simulator_mode = false: the window is resizable and the LVGL display follows its size
    // (minimum size, UI scale and max frame rate: app_window.c)
    appWindowInit(display, SCREEN_WIDTH, SCREEN_HEIGHT);
    PROF_DO(profInit(display);)
    lv_lock();
    lv_indev_t* pointer_device = lv_windows_acquire_pointer_indev(display);
    if (!pointer_device) return -1;
    appWindowSetPointer(pointer_device);
    lv_indev_t* keypad_device = lv_windows_acquire_keypad_indev(display);
    if (!keypad_device) return -1;
    lv_indev_t* encoder_device = lv_windows_acquire_encoder_indev(display);
    if (!encoder_device)  return -1;
    // The driver's encoder is the mouse wheel: each step moved the focus to the next widget, which
    // closed an open dropdown. The wheel scrolls instead (app_window.c).
    lv_indev_enable(encoder_device, false);
    settingsInit();
    loadScreen();
    lv_unlock();
    while (1)
    {
        PROF_START(handlerStart);
        uint32_t time_till_next = lv_timer_handler();
        PROF_DO(profLoop(handlerStart);)
        // handle LV_NO_TIMER_READY. Another option is to always sleep a few milliseconds
        if(time_till_next == LV_NO_TIMER_READY) time_till_next = LV_DEF_REFR_PERIOD;
        appWindowWait(time_till_next);
    }
    return 0;
}
