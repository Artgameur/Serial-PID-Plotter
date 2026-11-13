// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Main window of the app (LVGL Windows driver): minimum size, max frame rate.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef APP_WINDOW_H
#define APP_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

// ================================================================================================
// ================================================================================================
//            ENUM DECLARATION
// ================================================================================================
// ================================================================================================

// Mouse cursor shown over the UI
typedef enum
{
  APP_CURSOR_ARROW,
  APP_CURSOR_SIZE_WE,       // left / right
  APP_CURSOR_SIZE_NS,       // up / down
  APP_CURSOR_SIZE_ALL,      // both
  APP_CURSOR_SIZE_NWSE,     // diagonal (bottom right corner)
  APP_CURSOR_GRAB,          // open hand: can be dragged
  APP_CURSOR_GRABBING,      // closed hand: being dragged
} AppCursor_t;

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// display: created by lv_windows_create_display() with simulator_mode = false (resizable).
// The window can't be smaller than minWidth x minHeight UI pixels.
void appWindowInit(lv_display_t * display, int32_t minWidth, int32_t minHeight);

// Pointer input device of the window (lv_windows_acquire_pointer_indev): quick clicks are not
// lost when LVGL is busy
void appWindowSetPointer(lv_indev_t * pointer);

// Called in the LVGL thread with the UI coordinates of a right click
typedef void (*AppRightClickCb_t)(lv_point_t point);
void appWindowSetRightClickCb(AppRightClickCb_t cb);

// Called in the LVGL thread with the pointer's UI coordinates: cursor to show there
typedef AppCursor_t (*AppCursorCb_t)(lv_point_t point);
void appWindowSetCursorCb(AppCursorCb_t cb);

// The cursor is chosen again at the next pointer read, even if the pointer did not move (ex: a
// drag started)
void appWindowCursorRefresh(void);

// The mouse is over the app's window content (not over another window / outside)
bool appWindowPointerInside(void);

// Wall clock: milliseconds since 1970-01-01 UTC (for time_t / localtime: divide by 1000)
int64_t appWindowClockMs(void);

// UI scale (100 = normal): the minimum window size follows it
void appWindowSetScale(int32_t percent);

// Screen refresh rate limit (frames per second)
void appWindowSetMaxFps(uint32_t fps);

// Window icon (title bar, taskbar, Alt+Tab) drawn in these colors (0xRRGGBB): rounded square in
// background, signal trace in foreground. The .exe file keeps its built-in icon.
void appWindowSetIconColors(uint32_t background, uint32_t foreground);

// Title bar color and its title's color (0xRRGGBB, Windows 11); dark: dark caption buttons (and
// a dark title bar on Windows 10, which does not support the colors)
void appWindowSetTitleBarColors(uint32_t caption, uint32_t text, bool dark);

// Windows notification. The app has an icon in the notification area from the first one until it
// closes: Windows needs one to show them.
void appWindowNotify(const char * title, const char * text);

// Starts the app again in the same window position and size (ex: to show a loaded settings
// profile) and ends this instance; returns only if the new instance could not be started
void appWindowRestart(void);

// Windows "Open" / "Save as" dialog, modal to owner (HWND of the app's window, read in the LVGL
// thread: lv_windows_get_display_window_handle()). Blocks until it closes:
// call it from a thread of its own (it sets up COM for it), not from the LVGL thread.
// title: UTF-8. filter: "\0" separated pairs (L"CSV file (*.csv)\0*.csv\0"). defExt: added to a
// name without one (NULL: none). path: default name in, chosen path out (pathLen characters; with
// OFN_ALLOWMULTISELECT, the folder then the names). flags: OFN_xxx added to OFN_PATHMUSTEXIST |
// OFN_NOCHANGEDIR | OFN_EXPLORER. false: canceled.
bool appWindowFileDialog(void * owner, bool save, const char * title, const wchar_t * filter,
                         const wchar_t * defExt, wchar_t * path, uint32_t pathLen, uint32_t flags);

#endif //APP_WINDOW_H
