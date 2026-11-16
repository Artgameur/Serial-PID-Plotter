// ------------------------------------------------------------------------------------------------
/*!@file
   @brief The app's window on macOS and Linux: LVGL's SDL2 driver, with what app_window.c adds on
          Windows (right click, mouse wheel scrolling, latched clicks, cursors, minimum size,
          frame rate, restart in the same place, notifications).

   SDL's events are read by LVGL's SDL driver in the LVGL thread; an event watch sees them there
   too (right click, wheel, presses), and inputTimer handles them in the LVGL thread.

   Not done here (Windows only): the window icon drawn in the theme's colors and the title bar
   colors (the system draws the title bar).

   NOT YET BUILT OR TESTED: written on Windows, to be built and checked on macOS / Linux.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#ifndef _WIN32

#include "app_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif

#include LV_SDL_INCLUDE_PATH

#include "platform.h"
#include "ui_widgets.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE / VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

#define SCALE_MIN             50
#define SCALE_MAX             400
#define FPS_MIN               1
#define FPS_MAX               120
#define INPUT_POLL_MS         20
#define POINTER_READ_MS       10          // pointer read period (LVGL default: refresh period)
#define WHEEL_NOTCH           120         // uiWheelScroll's delta of a notch
#define WINDOW_ARG            "--window=" // x,y,width,height,maximized: window of the instance restarted
#define ARGS_MAX              64

static lv_display_t * appDisplay;
static SDL_Window *   window;
static int32_t        scale = 100;
static int32_t        minUiWidth;
static int32_t        minUiHeight;

// Set by the event watch, handled by inputTimer (both in the LVGL thread)
static AppRightClickCb_t rightClickCb;
static bool              rightClickPending;
static lv_point_t        rightClickPoint;
static int32_t           wheelDelta;      // WHEEL_NOTCH per notch
static lv_point_t        wheelPoint;

// Left button pressed and released between two pointer reads (ex: LVGL busy drawing): the press
// is latched until a read sees it
static lv_indev_read_cb_t driverPointerRead;
static bool               pressLatched;
static lv_point_t         pressPoint;

static AppCursorCb_t cursorCb;
static AppCursor_t   cursorShape = APP_CURSOR_ARROW;
static bool          cursorRefresh;
static SDL_Cursor *  cursors[APP_CURSOR_GRABBING + 1];

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static int  eventWatch(void * userData, SDL_Event * event);
static void inputTimer(lv_timer_t * tim);
static void pointerRead(lv_indev_t * indev, lv_indev_data_t * data);
static int  argumentsGet(char ** args, int max, char * storage, size_t size);
static void windowArgApply(void);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void appWindowInit(lv_display_t * display, int32_t minWidth, int32_t minHeight)
{
  // SDL's system cursors; the grab hands: the pointing hand (SDL has no open / closed hand)
  static const SDL_SystemCursor shapes[APP_CURSOR_GRABBING + 1] =
  {
    SDL_SYSTEM_CURSOR_ARROW, SDL_SYSTEM_CURSOR_SIZEWE, SDL_SYSTEM_CURSOR_SIZENS, SDL_SYSTEM_CURSOR_SIZEALL,
    SDL_SYSTEM_CURSOR_SIZENWSE, SDL_SYSTEM_CURSOR_HAND, SDL_SYSTEM_CURSOR_HAND,
  };

  appDisplay  = display;
  minUiWidth  = minWidth;
  minUiHeight = minHeight;
  window      = lv_sdl_window_get_window(display);

  for (uint32_t c = 0; c <= APP_CURSOR_GRABBING; ++c)
  {
    cursors[c] = SDL_CreateSystemCursor(shapes[c]);
  }

  if (window != NULL)
  {
    SDL_SetWindowMinimumSize(window, minWidth, minHeight);
    windowArgApply();
  }

  SDL_AddEventWatch(eventWatch, NULL);
  lv_timer_create(inputTimer, INPUT_POLL_MS, NULL);
}

// Wraps the driver's pointer read with the press latch and reads more often
void appWindowSetPointer(lv_indev_t * pointer)
{
  driverPointerRead = lv_indev_get_read_cb(pointer);
  lv_indev_set_read_cb(pointer, pointerRead);

  lv_timer_t * timer = lv_indev_get_read_timer(pointer);
  if (timer != NULL)
  {
    lv_timer_set_period(timer, POINTER_READ_MS);
  }
}

void appWindowSetRightClickCb(AppRightClickCb_t cb)
{
  rightClickCb = cb;
}

void appWindowSetCursorCb(AppCursorCb_t cb)
{
  cursorCb = cb;
}

void appWindowCursorRefresh(void)
{
  cursorRefresh = true;
}

bool appWindowPointerInside(void)
{
  return (window != NULL) && (SDL_GetMouseFocus() == window);
}

int64_t appWindowClockMs(void)
{
  return platformUnixMs();
}

// The minimum size grows with the UI scale; the window is enlarged if it is now too small
void appWindowSetScale(int32_t percent)
{
  int width;
  int height;

  if (window == NULL)
  {
    return;
  }

  scale = LV_CLAMP(SCALE_MIN, percent, SCALE_MAX);

  int minWidth  = (int) ((minUiWidth * scale) / 100);
  int minHeight = (int) ((minUiHeight * scale) / 100);

  SDL_SetWindowMinimumSize(window, minWidth, minHeight);
  SDL_GetWindowSize(window, &width, &height);

  if ((width < minWidth) || (height < minHeight))
  {
    SDL_SetWindowSize(window, LV_MAX(width, minWidth), LV_MAX(height, minHeight));
  }
}

void appWindowSetMaxFps(uint32_t fps)
{
  lv_timer_t * timer = (appDisplay != NULL) ? lv_display_get_refr_timer(appDisplay) : NULL;

  if (timer != NULL)
  {
    lv_timer_set_period(timer, 1000 / LV_CLAMP(FPS_MIN, fps, FPS_MAX));
  }
}

// Windows only: the system draws the title bar and the window icon
void appWindowSetIconColors(uint32_t background, uint32_t foreground)
{
  LV_UNUSED(background);
  LV_UNUSED(foreground);
}

void appWindowSetTitleBarColors(uint32_t caption, uint32_t text, bool dark)
{
  LV_UNUSED(caption);
  LV_UNUSED(text);
  LV_UNUSED(dark);
}

void appWindowNotify(const char * title, const char * text)
{
  platformNotify(title, text);
}

// The program started again with the same arguments and the window's place, then this one ends;
// returns only if it could not be started (exec replaces this process)
void appWindowRestart(void)
{
  char   path[PLATFORM_PATH_LEN];
  char   storage[PLATFORM_PATH_LEN * 4];
  char   windowArg[96];
  char * args[ARGS_MAX + 2];
  int    count = argumentsGet(args, ARGS_MAX, storage, sizeof(storage));

#ifdef __APPLE__
  uint32_t size = sizeof(path);

  if (_NSGetExecutablePath(path, &size) != 0)
  {
    return;
  }
#else
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);

  if (len <= 0)
  {
    return;
  }

  path[len] = '\0';
#endif

  // The former --window argument replaced by the window's place now
  int kept = (count > 0) ? 1 : 0;

  for (int a = 1; a < count; ++a)
  {
    if (strncmp(args[a], WINDOW_ARG, strlen(WINDOW_ARG)) != 0)
    {
      args[kept++] = args[a];
    }
  }

  if (window != NULL)
  {
    int x;
    int y;
    int width;
    int height;

    SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &width, &height);
    snprintf(windowArg, sizeof(windowArg), "%s%d,%d,%d,%d,%d", WINDOW_ARG, x, y, width, height,
             ((SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0) ? 1 : 0);
    args[kept++] = windowArg;
  }

  args[0]    = path;
  args[kept] = NULL;
  execv(path, args);
}

void appWindowWait(uint32_t ms)
{
  platformSleepMs(ms);
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// SDL coordinates are the UI's (LVGL's SDL driver draws 1:1, zoom 1)
static int eventWatch(void * userData, SDL_Event * event)
{
  LV_UNUSED(userData);

  if ((event->type == SDL_MOUSEBUTTONDOWN) && (event->button.button == SDL_BUTTON_LEFT))
  {
    pressLatched = true;
    pressPoint   = (lv_point_t) {event->button.x, event->button.y};
  }
  else if ((event->type == SDL_MOUSEBUTTONUP) && (event->button.button == SDL_BUTTON_RIGHT))
  {
    rightClickPending = true;
    rightClickPoint   = (lv_point_t) {event->button.x, event->button.y};
  }
  else if (event->type == SDL_MOUSEWHEEL)
  {
    int x;
    int y;
    int notches = (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -event->wheel.y : event->wheel.y;

    SDL_GetMouseState(&x, &y);
    wheelDelta += notches * WHEEL_NOTCH;
    wheelPoint  = (lv_point_t) {x, y};
  }

  return 1;
}

static void inputTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  if (rightClickPending && (rightClickCb != NULL))
  {
    rightClickPending = false;
    rightClickCb(rightClickPoint);
  }

  if (wheelDelta != 0)
  {
    int32_t delta = wheelDelta;

    wheelDelta = 0;
    uiWheelScroll(wheelPoint, delta);
  }
}

static void pointerRead(lv_indev_t * indev, lv_indev_data_t * data)
{
  bool latched = pressLatched;

  pressLatched = false;
  driverPointerRead(indev, data);

  if (latched && (data->state == LV_INDEV_STATE_RELEASED))
  {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point = pressPoint;
  }

  // The cursor only changes when the pointer moves or is pressed / released
  static lv_point_t       lastPoint = {-1, -1};
  static lv_indev_state_t lastState = LV_INDEV_STATE_RELEASED;

  bool refresh = cursorRefresh;

  cursorRefresh = false;

  if ((cursorCb != NULL) &&
      (refresh || (data->point.x != lastPoint.x) || (data->point.y != lastPoint.y) || (data->state != lastState)))
  {
    AppCursor_t shape = cursorCb(data->point);

    lastPoint = data->point;
    lastState = data->state;

    if ((shape != cursorShape) && (shape <= APP_CURSOR_GRABBING) && (cursors[shape] != NULL))
    {
      cursorShape = shape;
      SDL_SetCursor(cursors[shape]);
    }
  }
}

// The program's arguments (argv), copied in storage; returns how many
static int argumentsGet(char ** args, int max, char * storage, size_t size)
{
  int count = 0;

#ifdef __APPLE__
  int     argc = *_NSGetArgc();
  char ** argv = *_NSGetArgv();
  size_t  used = 0;

  for (int a = 0; (a < argc) && (count < max); ++a)
  {
    size_t len = strlen(argv[a]) + 1;

    if ((used + len) > size)
    {
      break;
    }

    memcpy(&storage[used], argv[a], len);
    args[count++] = &storage[used];
    used += len;
  }
#else
  // /proc/self/cmdline: the arguments, each ending with a 0
  FILE * file = fopen("/proc/self/cmdline", "rb");
  size_t len  = (file != NULL) ? fread(storage, 1, size - 1, file) : 0;

  if (file != NULL)
  {
    fclose(file);
  }

  storage[len] = '\0';

  for (size_t i = 0; (i < len) && (count < max); i += strlen(&storage[i]) + 1)
  {
    args[count++] = &storage[i];
  }
#endif

  return count;
}

// "--window=x,y,width,height,maximized": the window of the instance that restarted
static void windowArgApply(void)
{
  char   storage[PLATFORM_PATH_LEN * 4];
  char * args[ARGS_MAX + 2];
  int    count = argumentsGet(args, ARGS_MAX, storage, sizeof(storage));

  for (int a = 1; a < count; ++a)
  {
    int x;
    int y;
    int width;
    int height;
    int maximized;

    if ((strncmp(args[a], WINDOW_ARG, strlen(WINDOW_ARG)) == 0) &&
        (sscanf(args[a] + strlen(WINDOW_ARG), "%d,%d,%d,%d,%d", &x, &y, &width, &height, &maximized) == 5) &&
        (width > 0) && (height > 0))
    {
      SDL_SetWindowPosition(window, x, y);
      SDL_SetWindowSize(window, width, height);

      if (maximized != 0)
      {
        SDL_MaximizeWindow(window);
      }
    }
  }
}

#endif
