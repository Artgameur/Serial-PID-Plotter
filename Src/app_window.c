// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Main window of the app (LVGL Windows driver): minimum size, max frame rate.

   The UI is drawn 1:1 in the window (the UI scale is applied by the UI itself, see uiScaleSet);
   only the minimum window size follows the scale.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "app_window.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#include "lvgl.h"
#include "src/drivers/windows/lv_windows_context.h"
#include "profile.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define SCALE_MIN             50
#define SCALE_MAX             400
#define FPS_MIN               1
#define FPS_MAX               120
#define INPUT_POLL_MS         20
#define POINTER_READ_MS       10          // pointer read period (LVGL default: refresh period)
#define WHEEL_SCROLL_PX       48          // per wheel notch, at 100 % UI scale
#define WINDOW_ARG            "--window=" // x,y,width,height,maximized: window of the instance restarted
#define APP_ICON_ID           1           // icon resource (Assets/app.rc)

// Title bar attributes (DwmSetWindowAttribute), missing from older MinGW headers. Colors: Windows
// 11 only; dark mode: Windows 10 (build 18985+) and 11.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR    34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR   35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR      36
#endif

// Window icon drawn in the theme's colors (same design as Assets/app.ico): rounded square with a
// signal trace and a baseline, in a 256 x 256 design grid, ICON_SAMPLES^2 samples per pixel
#define ICON_GRID             256.0f
#define ICON_SAMPLES          4
#define ICON_MARGIN           8.0f        // none at 24 px and less
#define ICON_RADIUS           52.0f
#define ICON_TRACE_WIDTH      20.0f
#define ICON_BASE_WIDTH       8.0f
#define ICON_BASE_Y           216.0f
#define ICON_BASE_X1          36.0f
#define ICON_BASE_X2          220.0f

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static lv_display_t * appDisplay;
static HWND           appHwnd;
static WNDPROC        driverWindowProc;
static int32_t        scale = 100;
static int32_t        minUiWidth;
static int32_t        minUiHeight;

// Right click (the driver only handles the left button) and mouse wheel (the driver only turns
// it into encoder steps): set by the window's thread, handled in the LVGL thread by inputTimer
static AppRightClickCb_t rightClickCb;
static volatile LONG     rightClickPending;
static volatile LONG     rightClickX;
static volatile LONG     rightClickY;
static volatile LONG     wheelDelta;          // sum of WM_MOUSEWHEEL deltas (120 per notch)
static volatile LONG     wheelX;
static volatile LONG     wheelY;

// Left button: the driver only keeps the current state, so a click pressed and released between
// two pointer reads (ex: LVGL busy drawing) was lost. Presses are latched until a read sees them.
static lv_indev_read_cb_t driverPointerRead;
static volatile LONG      pressLatched;
static volatile LONG      pressX;
static volatile LONG      pressY;

// Mouse cursor: chosen in the LVGL thread (cursorCb, at each pointer read), shown by the window's
// thread (WM_SETCURSOR)
static AppCursorCb_t cursorCb;
static volatile LONG cursorShape = APP_CURSOR_ARROW;
static volatile LONG cursorRefresh;           // choose the cursor again at the next read

// Hand cursors (Windows has none): 16 x 16 pixel art, X = outline, o = fill, space = transparent;
// sized like the system arrow (handCursorCreate). Hot spot: the middle.
#define HAND_SIZE             16
static const char * const handOpen[HAND_SIZE] =
{
  "       XX       ",
  "   XX XooXXX    ",
  "  XooXXooXooX   ",
  "  XooXXooXooX X ",
  "   XooXooXooXXoX",
  "   XooXooXooXooX",
  " XX XoooooooXooX",
  "XooXXoooooooooX ",
  "XoooXooooooooX  ",
  " XooooooooooooX ",
  "  XoooooooooooX ",
  "  XooooooooooX  ",
  "   XoooooooooX  ",
  "    XoooooooX   ",
  "     XooooooX   ",
  "     XooooooX   ",
};
static const char * const handClosed[HAND_SIZE] =
{
  "                ",
  "                ",
  "                ",
  "    XX XX XX    ",
  "   XooXooXooXX  ",
  "   XooooooooXoX ",
  "    XoooooooooX ",
  "   XXoooooooooX ",
  "  XoooooooooooX ",
  "  XoooooooooooX ",
  "   XooooooooooX ",
  "   XoooooooooX  ",
  "    XooooooooX  ",
  "     XoooooooX  ",
  "     XoooooooX  ",
  "                ",
};
static HCURSOR handCursors[2];                // open, closed

// Signal trace of the icon (design grid), simpler at 16 px
static const float iconTrace[][2]      = {{36, 148}, {78, 148}, {106, 80}, {142, 192}, {172, 112}, {194, 148}, {220, 148}};
static const float iconTraceSmall[][2] = {{28, 150}, {84, 150}, {116, 76}, {150, 190}, {180, 128}, {228, 128}};

// Icons set on the window: the previous ones are destroyed at the next change (the window may
// still use them until it handles the new ones)
static HICON iconsShown[2];
static HICON iconsPrevious[2];

static const UINT notifyIconId = 1;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void    minWindowSizeGet(HWND hwnd, LONG * width, LONG * height);
static void    windowArgApply(void);
static void    iconSet(void);
static void    notifyIconRemove(void);
static HICON   iconDraw(int32_t size, uint32_t background, uint32_t foreground);
static float   segmentDistance(float x, float y, const float * a, const float * b);
static bool    inRoundRect(float x, float y, float min, float max, float radius);
static void    inputTimer(lv_timer_t * tim);
static void    pointerRead(lv_indev_t * indev, lv_indev_data_t * data);
static LPCWSTR cursorIdGet(LONG shape);
static HCURSOR handCursorCreate(const char * const * art);
static void    wheelScroll(lv_point_t point, int32_t delta);
static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// The driver's flush copies the whole window after each frame (StretchBlt, HALFTONE). With the UI
// drawn 1:1, only the redrawn areas are copied instead (BitBlt from the driver's frame buffer).
// The window's content is kept by the desktop compositor between frames; the driver redraws all of
// it after a resize. Other zoom / simulator mode: the driver's own flush.
static lv_display_flush_cb_t driverFlush;

static void flushAreas(lv_display_t * display, const lv_area_t * area, uint8_t * pixels)
{
  HWND                          hwnd    = lv_windows_get_display_window_handle(display);
  lv_windows_window_context_t * context = (hwnd != NULL) ? lv_windows_get_window_context(hwnd) : NULL;
  bool                          whole   = (context == NULL) || (context->zoom_level != 100) || context->simulator_mode;

  PROF_PUSH(PROF_FLUSH);

#ifdef SD_PROFILE
  // Measurement of the former way
  whole = whole || (getenv("SD_FLUSH_FULL") != NULL);
#endif

  if (whole)
  {
    driverFlush(display, area, pixels);
    PROF_POP();
    return;
  }

  HDC hdc = GetDC(hwnd);

  if (hdc != NULL)
  {
    BitBlt(hdc, area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area),
           context->display_framebuffer_context_handle, area->x1, area->y1, SRCCOPY);
    ReleaseDC(hwnd, hdc);
  }

  lv_display_flush_ready(display);
  PROF_POP();
}

// Main loop: Sleep() wakes on the system timer step (~15.6 ms by default), so the LVGL timers ran
// late (16 ms periods every 15.6 or 31.2 ms, ~40 frames per second at most). A high resolution
// waitable timer wakes when asked (Windows 10 1803+; otherwise Sleep()).
void appWindowWait(uint32_t ms)
{
  static HANDLE timer;
  static bool   tried;

  if (ms == 0)
  {
    return;
  }

  if (!tried)
  {
    tried = true;
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  }

  LARGE_INTEGER due = {.QuadPart = -(LONGLONG) ms * 10000};   // 100 ns units, relative

  if ((timer != NULL) && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
  {
    WaitForSingleObject(timer, INFINITE);
  }
  else
  {
    Sleep(ms);
  }
}

void appWindowInit(lv_display_t * display, int32_t minWidth, int32_t minHeight)
{
  appDisplay  = display;
  minUiWidth  = minWidth;
  minUiHeight = minHeight;
  appHwnd     = lv_windows_get_display_window_handle(display);
  driverFlush = lv_display_get_flush_cb(display);

  if (driverFlush != NULL)
  {
    lv_display_set_flush_cb(display, flushAreas);
  }

  if (appHwnd != NULL)
  {
    driverWindowProc = (WNDPROC) SetWindowLongPtrW(appHwnd, GWLP_WNDPROC, (LONG_PTR) windowProc);

    // The driver's window has a sunken client edge (WS_EX_CLIENTEDGE): drawn as part of the frame,
    // which is transparent on Windows 10 / 11, so maximized (frame off screen, edge still on it) a
    // few pixel line showed what is behind the window. Removed; the client area takes its place.
    LONG_PTR exStyle = GetWindowLongPtrW(appHwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_CLIENTEDGE) != 0)
    {
      SetWindowLongPtrW(appHwnd, GWL_EXSTYLE, exStyle & ~(LONG_PTR) WS_EX_CLIENTEDGE);
      SetWindowPos(appHwnd, NULL, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }

    windowArgApply();
    iconSet();
  }

  handCursors[0] = handCursorCreate(handOpen);
  handCursors[1] = handCursorCreate(handClosed);

  lv_timer_create(inputTimer, INPUT_POLL_MS, NULL);
}

// Window icon in the given colors (0xRRGGBB), at the sizes of the taskbar / Alt+Tab and of the
// title bar. Posted: the window belongs to the driver's thread.
void appWindowSetIconColors(uint32_t background, uint32_t foreground)
{
  HICON iconBig;
  HICON iconSmall;

  if (appHwnd == NULL)
  {
    return;
  }

  iconBig   = iconDraw(GetSystemMetrics(SM_CXICON), background, foreground);
  iconSmall = iconDraw(GetSystemMetrics(SM_CXSMICON), background, foreground);

  if ((iconBig == NULL) || (iconSmall == NULL))
  {
    DestroyIcon(iconBig);
    DestroyIcon(iconSmall);
    return;
  }

  PostMessageW(appHwnd, WM_SETICON, ICON_BIG, (LPARAM) iconBig);
  PostMessageW(appHwnd, WM_SETICON, ICON_SMALL, (LPARAM) iconSmall);

  for (uint32_t i = 0; i < 2; ++i)
  {
    if (iconsPrevious[i] != NULL)
    {
      DestroyIcon(iconsPrevious[i]);
    }

    iconsPrevious[i] = iconsShown[i];
  }

  iconsShown[0] = iconBig;
  iconsShown[1] = iconSmall;
}

// Title bar (and window border) in caption, its title in text (0xRRGGBB); dark: dark caption
// buttons, and the dark title bar where the colors are not supported (Windows 10)
void appWindowSetTitleBarColors(uint32_t caption, uint32_t text, bool dark)
{
  // COLORREF: 0x00BBGGRR
  COLORREF captionRef = RGB((caption >> 16) & 0xFF, (caption >> 8) & 0xFF, caption & 0xFF);
  COLORREF textRef    = RGB((text >> 16) & 0xFF, (text >> 8) & 0xFF, text & 0xFF);
  BOOL     darkMode   = dark ? TRUE : FALSE;

  if (appHwnd == NULL)
  {
    return;
  }

  // Each one fails without effect on the Windows versions that do not have it
  DwmSetWindowAttribute(appHwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
  DwmSetWindowAttribute(appHwnd, DWMWA_CAPTION_COLOR, &captionRef, sizeof(captionRef));
  DwmSetWindowAttribute(appHwnd, DWMWA_BORDER_COLOR, &captionRef, sizeof(captionRef));
  DwmSetWindowAttribute(appHwnd, DWMWA_TEXT_COLOR, &textRef, sizeof(textRef));
}

// Starts a new instance of the app with the same window position and size, then ends this one.
// Returns only if the new instance could not be started.
// Modified, or added when it is not there yet (first time, or Windows' shell was restarted)
void appWindowNotify(const char * title, const char * text)
{
  NOTIFYICONDATAW data = {.cbSize = sizeof(data), .hWnd = appHwnd, .uID = notifyIconId};

  if (appHwnd == NULL)
  {
    return;
  }

  data.uFlags      = NIF_ICON | NIF_TIP | NIF_INFO;
  data.dwInfoFlags = NIIF_INFO;
  data.hIcon       = (iconsShown[1] != NULL) ? iconsShown[1] : LoadIconW(NULL, (LPCWSTR) IDI_APPLICATION);

  // Too long for its field: cut off there
  MultiByteToWideChar(CP_UTF8, 0, title, -1, data.szTip, (int) (sizeof(data.szTip) / sizeof(data.szTip[0])));
  MultiByteToWideChar(CP_UTF8, 0, title, -1, data.szInfoTitle, (int) (sizeof(data.szInfoTitle) / sizeof(data.szInfoTitle[0])));
  MultiByteToWideChar(CP_UTF8, 0, text, -1, data.szInfo, (int) (sizeof(data.szInfo) / sizeof(data.szInfo[0])));
  data.szTip[(sizeof(data.szTip) / sizeof(data.szTip[0])) - 1]                   = L'\0';
  data.szInfoTitle[(sizeof(data.szInfoTitle) / sizeof(data.szInfoTitle[0])) - 1] = L'\0';
  data.szInfo[(sizeof(data.szInfo) / sizeof(data.szInfo[0])) - 1]                = L'\0';

  if (!Shell_NotifyIconW(NIM_MODIFY, &data))
  {
    Shell_NotifyIconW(NIM_ADD, &data);
  }
}

void appWindowRestart(void)
{
  char                exePath[MAX_PATH];
  char                commandLine[MAX_PATH + 96];
  WINDOWPLACEMENT     placement = {.length = sizeof(placement)};
  RECT                rect = {0};
  bool                maximized = false;
  STARTUPINFOA        startup = {.cb = sizeof(startup)};
  PROCESS_INFORMATION process;

  if (GetModuleFileNameA(NULL, exePath, sizeof(exePath)) == 0)
  {
    return;
  }

  // Maximized: the size it goes back to when restored
  if ((appHwnd != NULL) && GetWindowPlacement(appHwnd, &placement))
  {
    maximized = (placement.showCmd == SW_SHOWMAXIMIZED);
    if (maximized)
    {
      rect = placement.rcNormalPosition;
    }
    else
    {
      GetWindowRect(appHwnd, &rect);
    }
  }

  notifyIconRemove();

  if (rect.right > rect.left)
  {
    snprintf(commandLine, sizeof(commandLine), "\"%s\" %s%ld,%ld,%ld,%ld,%d", exePath, WINDOW_ARG, rect.left,
             rect.top, rect.right - rect.left, rect.bottom - rect.top, maximized ? 1 : 0);
  }
  else
  {
    snprintf(commandLine, sizeof(commandLine), "\"%s\"", exePath);
  }

  if (CreateProcessA(exePath, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
  {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ExitProcess(0);
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
  InterlockedExchange(&cursorRefresh, 1);
}

// FILETIME: 100 ns units since 1601-01-01
int64_t appWindowClockMs(void)
{
  FILETIME now;

  GetSystemTimeAsFileTime(&now);
  return (int64_t) (((((uint64_t) now.dwHighDateTime << 32) | now.dwLowDateTime) - 116444736000000000ULL) / 10000ULL);
}

// The driver keeps the last pointer position when the mouse leaves the window
bool appWindowPointerInside(void)
{
  POINT screen;
  POINT pointer;
  RECT  client;

  if ((appHwnd == NULL) || !GetCursorPos(&screen) || (WindowFromPoint(screen) != appHwnd))
  {
    return false;
  }

  pointer = screen;
  ScreenToClient(appHwnd, &pointer);
  GetClientRect(appHwnd, &client);
  return PtInRect(&client, pointer) != FALSE;
}

// Wraps the driver's pointer read with the press latch (see pointerRead) and reads more often
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

// The minimum size grows with the UI scale; the window is enlarged if it is now too small
void appWindowSetScale(int32_t percent)
{
  RECT window;
  LONG minWidth;
  LONG minHeight;

  if (appHwnd == NULL)
  {
    return;
  }

  scale = LV_CLAMP(SCALE_MIN, percent, SCALE_MAX);

  // Async: the window belongs to the driver's thread
  minWindowSizeGet(appHwnd, &minWidth, &minHeight);
  GetWindowRect(appHwnd, &window);

  if (((window.right - window.left) < minWidth) || ((window.bottom - window.top) < minHeight))
  {
    SetWindowPos(appHwnd, NULL, 0, 0, LV_MAX(window.right - window.left, minWidth),
                 LV_MAX(window.bottom - window.top, minHeight),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
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

bool appWindowFileDialog(void * owner, bool save, const char * title, const wchar_t * filter,
                         const wchar_t * defExt, wchar_t * path, uint32_t pathLen, uint32_t flags)
{
  // The Explorer style dialog uses COM
  HRESULT       com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  OPENFILENAMEW dialog = {0};
  wchar_t       wideTitle[64];

  // Unicode dialog: accents in the title and in the folder / file names
  MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle, sizeof(wideTitle) / sizeof(wideTitle[0]));

  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner   = (HWND) owner;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile   = path;
  dialog.nMaxFile    = pathLen;
  dialog.lpstrDefExt = defExt;
  dialog.lpstrTitle  = wideTitle;
  dialog.Flags       = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | flags;

  bool chosen = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);

  if (SUCCEEDED(com))
  {
    CoUninitialize();
  }

  return chosen;
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// Window size (with borders) for a client area of minUiWidth x minUiHeight at the UI scale
static void minWindowSizeGet(HWND hwnd, LONG * width, LONG * height)
{
  RECT rect = {0, 0, MulDiv(minUiWidth, scale, 100), MulDiv(minUiHeight, scale, 100)};

  AdjustWindowRectEx(&rect, (DWORD) GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                     (DWORD) GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  *width  = rect.right - rect.left;
  *height = rect.bottom - rect.top;
}

// Window of the instance that restarted the app (appWindowRestart), if any. Async: the window
// belongs to the driver's thread.
static void windowArgApply(void)
{
  const char * arg = strstr(GetCommandLineA(), WINDOW_ARG);
  long         x;
  long         y;
  long         width;
  long         height;
  int          maximized;

  if ((arg == NULL) ||
      (sscanf(arg + strlen(WINDOW_ARG), "%ld,%ld,%ld,%ld,%d", &x, &y, &width, &height, &maximized) != 5) ||
      (width <= 0) || (height <= 0))
  {
    return;
  }

  SetWindowPos(appHwnd, NULL, (int) x, (int) y, (int) width, (int) height,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);

  if (maximized != 0)
  {
    ShowWindowAsync(appHwnd, SW_MAXIMIZE);
  }
}

// The driver's window has no icon: the program's (built in from Assets/app.ico, if any), at the
// sizes of the title bar and of the taskbar / Alt+Tab
// Otherwise it stays in the notification area until the mouse goes over it
static void notifyIconRemove(void)
{
  NOTIFYICONDATAW data = {.cbSize = sizeof(data), .hWnd = appHwnd, .uID = notifyIconId};

  Shell_NotifyIconW(NIM_DELETE, &data);
}

static void iconSet(void)
{
  HINSTANCE instance = GetModuleHandleW(NULL);
  HICON     iconBig   = (HICON) LoadImageW(instance, MAKEINTRESOURCEW(APP_ICON_ID), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
  HICON     iconSmall = (HICON) LoadImageW(instance, MAKEINTRESOURCEW(APP_ICON_ID), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

  // Until the theme's colors are applied (appWindowSetIconColors)
  if (iconBig != NULL)
  {
    PostMessageW(appHwnd, WM_SETICON, ICON_BIG, (LPARAM) iconBig);
  }

  if (iconSmall != NULL)
  {
    PostMessageW(appHwnd, WM_SETICON, ICON_SMALL, (LPARAM) iconSmall);
  }
}

// size x size icon: background where the rounded square covers each pixel (transparent around),
// foreground where the trace / baseline cover it, both anti-aliased
static HICON iconDraw(int32_t size, uint32_t background, uint32_t foreground)
{
  BITMAPINFO info = {0};
  uint32_t * pixels = NULL;
  float      unit   = ICON_GRID / (float) size;                  // design units per pixel
  float      margin = (size <= 24) ? 0.0f : ICON_MARGIN;
  float      radius = LV_MAX(ICON_RADIUS, 2.0f * unit);
  float      traceHalf = LV_MAX(ICON_TRACE_WIDTH, 1.6f * unit) / 2.0f;
  float      baseHalf  = LV_MAX(ICON_BASE_WIDTH, 1.0f * unit) / 2.0f;
  bool       simple = size <= 16;
  const float (*trace)[2] = simple ? iconTraceSmall : iconTrace;
  uint32_t   points = simple ? (sizeof(iconTraceSmall) / sizeof(iconTraceSmall[0]))
                             : (sizeof(iconTrace) / sizeof(iconTrace[0]));
  const float baseA[2] = {ICON_BASE_X1, ICON_BASE_Y};
  const float baseB[2] = {ICON_BASE_X2, ICON_BASE_Y};

  info.bmiHeader.biSize        = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth       = size;
  info.bmiHeader.biHeight      = -size;   // top-down
  info.bmiHeader.biPlanes      = 1;
  info.bmiHeader.biBitCount    = 32;
  info.bmiHeader.biCompression = BI_RGB;

  HBITMAP color = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, (void **) &pixels, NULL, 0);
  HBITMAP mask  = CreateBitmap(size, size, 1, 1, NULL);

  if ((color == NULL) || (mask == NULL) || (pixels == NULL))
  {
    DeleteObject(color);
    DeleteObject(mask);
    return NULL;
  }

  for (int32_t py = 0; py < size; ++py)
  {
    for (int32_t px = 0; px < size; ++px)
    {
      uint32_t inside = 0;
      uint32_t onLine = 0;

      for (uint32_t sy = 0; sy < ICON_SAMPLES; ++sy)
      {
        for (uint32_t sx = 0; sx < ICON_SAMPLES; ++sx)
        {
          float x = ((float) px + (((float) sx + 0.5f) / ICON_SAMPLES)) * unit;
          float y = ((float) py + (((float) sy + 0.5f) / ICON_SAMPLES)) * unit;

          if (!inRoundRect(x, y, margin, ICON_GRID - margin, radius))
          {
            continue;
          }

          bool line = (size >= 32) && (segmentDistance(x, y, baseA, baseB) <= baseHalf);

          for (uint32_t i = 0; (i + 1 < points) && !line; ++i)
          {
            line = segmentDistance(x, y, trace[i], trace[i + 1]) <= traceHalf;
          }

          inside++;
          onLine += line ? 1 : 0;
        }
      }

      // Straight (not premultiplied) alpha
      uint32_t alpha = (inside * 255) / (ICON_SAMPLES * ICON_SAMPLES);
      uint32_t mix   = (inside > 0) ? ((onLine * 256) / inside) : 0;
      uint32_t rgb   = 0;

      for (uint32_t shift = 0; shift < 24; shift += 8)
      {
        uint32_t bg = (background >> shift) & 0xFF;
        uint32_t fg = (foreground >> shift) & 0xFF;
        rgb |= (((bg * (256 - mix)) + (fg * mix)) >> 8) << shift;
      }

      pixels[(py * size) + px] = (alpha << 24) | rgb;
    }
  }

  ICONINFO iconInfo = {TRUE, 0, 0, mask, color};
  HICON    icon     = CreateIconIndirect(&iconInfo);

  // The icon has its own copies
  DeleteObject(color);
  DeleteObject(mask);
  return icon;
}

static float segmentDistance(float x, float y, const float * a, const float * b)
{
  float dx  = b[0] - a[0];
  float dy  = b[1] - a[1];
  float len = (dx * dx) + (dy * dy);
  float t   = (len > 0.0f) ? LV_CLAMP(0.0f, (((x - a[0]) * dx) + ((y - a[1]) * dy)) / len, 1.0f) : 0.0f;
  float ex  = x - (a[0] + (t * dx));
  float ey  = y - (a[1] + (t * dy));

  return sqrtf((ex * ex) + (ey * ey));
}

// Square min..max (both axes) with rounded corners
static bool inRoundRect(float x, float y, float min, float max, float radius)
{
  float cx = LV_CLAMP(min + radius, x, max - radius);
  float cy = LV_CLAMP(min + radius, y, max - radius);

  if ((x < min) || (x > max) || (y < min) || (y > max))
  {
    return false;
  }

  return (((x - cx) * (x - cx)) + ((y - cy) * (y - cy))) <= (radius * radius);
}

// A press the driver no longer reports (already released) is reported for this read, at the
// press position; the release is then seen at the next read, so the click is not lost
static void pointerRead(lv_indev_t * indev, lv_indev_data_t * data)
{
  LONG latched = InterlockedExchange(&pressLatched, 0);

  driverPointerRead(indev, data);

  if ((latched != 0) && (data->state == LV_INDEV_STATE_RELEASED))
  {
    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = (int32_t) pressX;
    data->point.y = (int32_t) pressY;
  }

  // The cursor only changes when the pointer moves or is pressed / released: the search under the
  // pointer is skipped otherwise
  static lv_point_t      lastPoint = {-1, -1};
  static lv_indev_state_t lastState = LV_INDEV_STATE_RELEASED;

  bool refresh = InterlockedExchange(&cursorRefresh, 0) != 0;

  if ((cursorCb != NULL) &&
      (refresh || (data->point.x != lastPoint.x) || (data->point.y != lastPoint.y) || (data->state != lastState)))
  {
    LONG shape = (LONG) cursorCb(data->point);

    lastPoint = data->point;
    lastState = data->state;

    // Changed without a mouse move (ex: a drag started): shown now, not at the next move
    if ((InterlockedExchange(&cursorShape, shape) != shape) && (appHwnd != NULL))
    {
      PostMessageW(appHwnd, WM_SETCURSOR, (WPARAM) appHwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
  }
}

// Cursor from pixel art (see handOpen). The system arrow fills about half of the cursor's size
// (16 of 32 px at 100 %): the art is 1:1 there, enlarged by whole steps with the display scale
// (2x from 64 px).
static HCURSOR handCursorCreate(const char * const * art)
{
  int32_t    size  = GetSystemMetrics(SM_CXCURSOR);
  int32_t    scale = LV_MAX(1, size / (2 * HAND_SIZE));
  BITMAPINFO info  = {0};
  uint32_t * pixels = NULL;

  info.bmiHeader.biSize        = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth       = size;
  info.bmiHeader.biHeight      = -size;   // top-down
  info.bmiHeader.biPlanes      = 1;
  info.bmiHeader.biBitCount    = 32;
  info.bmiHeader.biCompression = BI_RGB;

  HBITMAP color = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, (void **) &pixels, NULL, 0);
  HBITMAP mask  = CreateBitmap(size, size, 1, 1, NULL);

  if ((color == NULL) || (mask == NULL) || (pixels == NULL))
  {
    DeleteObject(color);
    DeleteObject(mask);
    return NULL;
  }

  // Centered in the cursor (when the size is not a multiple of HAND_SIZE)
  int32_t offset = (size - (HAND_SIZE * scale)) / 2;

  for (int32_t y = 0; y < size; ++y)
  {
    for (int32_t x = 0; x < size; ++x)
    {
      int32_t ax = (x - offset) / scale;
      int32_t ay = (y - offset) / scale;
      char    c  = ((x >= offset) && (y >= offset) && (ax < HAND_SIZE) && (ay < HAND_SIZE)) ? art[ay][ax] : ' ';

      pixels[(y * size) + x] = (c == 'X') ? 0xFF000000 : ((c == 'o') ? 0xFFFFFFFF : 0x00000000);
    }
  }

  ICONINFO cursorInfo = {FALSE, (DWORD) (size / 2), (DWORD) (size / 2), mask, color};
  HCURSOR  cursor     = (HCURSOR) CreateIconIndirect(&cursorInfo);

  DeleteObject(color);
  DeleteObject(mask);
  return cursor;
}

// System cursor ids (values of IDC_xxx, which are ANSI or wide depending on UNICODE)
static LPCWSTR cursorIdGet(LONG shape)
{
  switch (shape)
  {
    case APP_CURSOR_SIZE_WE:   return MAKEINTRESOURCEW(32644);   // IDC_SIZEWE
    case APP_CURSOR_SIZE_NS:   return MAKEINTRESOURCEW(32645);   // IDC_SIZENS
    case APP_CURSOR_SIZE_ALL:  return MAKEINTRESOURCEW(32646);   // IDC_SIZEALL
    case APP_CURSOR_SIZE_NWSE: return MAKEINTRESOURCEW(32642);   // IDC_SIZENWSE
    default:                   return MAKEINTRESOURCEW(32512);   // IDC_ARROW
  }
}

static void inputTimer(lv_timer_t * tim)
{
  LONG delta = InterlockedExchange(&wheelDelta, 0);

  LV_UNUSED(tim);

  if ((InterlockedExchange(&rightClickPending, 0) != 0) && (rightClickCb != NULL))
  {
    lv_point_t point = {(int32_t) rightClickX, (int32_t) rightClickY};
    rightClickCb(point);
  }

  if (delta != 0)
  {
    lv_point_t point = {(int32_t) wheelX, (int32_t) wheelY};
    wheelScroll(point, (int32_t) delta);
  }
}

// Scrolls the nearest scrollable container under the pointer that has hidden content in the
// wheel direction (ex: settings windows, terminal log)
static void wheelScroll(lv_point_t point, int32_t delta)
{
  int32_t dy = (delta * LV_MAX(1, MulDiv(WHEEL_SCROLL_PX, (int) lv_display_get_dpi(appDisplay), LV_DPI_DEF))) /
               WHEEL_DELTA;

  // Objects on the top layer (above the screen) first, then the screen (an open dropdown list is
  // the screen's last child, so it is found before what is behind it)
  lv_obj_t * hit = lv_indev_search_obj(lv_layer_top(), &point);

  if (hit == NULL)
  {
    hit = lv_indev_search_obj(lv_screen_active(), &point);
  }

  for (lv_obj_t * obj = hit; obj != NULL; obj = lv_obj_get_parent(obj))
  {
    bool canScroll = (dy > 0) ? (lv_obj_get_scroll_top(obj) > 0) : (lv_obj_get_scroll_bottom(obj) > 0);

    if (lv_obj_is_scrollable(obj) && canScroll)
    {
      lv_obj_scroll_by_bounded(obj, 0, dy, LV_ANIM_OFF);
      return;
    }
  }
}

// Runs in the window's thread, around the driver's window procedure
static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  // Cursor over the UI (the window's border keeps its own resize cursors)
  if ((message == WM_SETCURSOR) && (LOWORD(lParam) == HTCLIENT))
  {
    LONG    shape = cursorShape;
    HCURSOR hand  = (shape == APP_CURSOR_GRAB) ? handCursors[0] : ((shape == APP_CURSOR_GRABBING) ? handCursors[1] : NULL);
    POINT   screen;
    POINT   pointer;
    RECT    client;

    // Also posted by pointerRead: only while the mouse is over the UI (this window on top there)
    GetCursorPos(&screen);
    pointer = screen;
    ScreenToClient(hwnd, &pointer);
    GetClientRect(hwnd, &client);

    if (PtInRect(&client, pointer) && (WindowFromPoint(screen) == hwnd))
    {
      SetCursor((hand != NULL) ? hand : LoadCursorW(NULL, cursorIdGet(shape)));
    }

    return TRUE;
  }

  if (message == WM_DESTROY)
  {
    notifyIconRemove();
  }

  // Client coordinates = UI coordinates (the UI is drawn 1:1)
  if (message == WM_LBUTTONDOWN)
  {
    pressX = GET_X_LPARAM(lParam);
    pressY = GET_Y_LPARAM(lParam);
    InterlockedExchange(&pressLatched, 1);
  }
  else if (message == WM_RBUTTONUP)
  {
    rightClickX = GET_X_LPARAM(lParam);
    rightClickY = GET_Y_LPARAM(lParam);
    InterlockedExchange(&rightClickPending, 1);
  }
  else if (message == WM_MOUSEWHEEL)
  {
    // The wheel position is in screen coordinates
    POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd, &point);
    wheelX = point.x;
    wheelY = point.y;
    InterlockedExchangeAdd(&wheelDelta, GET_WHEEL_DELTA_WPARAM(wParam));
  }

  LRESULT result = CallWindowProcW(driverWindowProc, hwnd, message, wParam, lParam);

  if (message == WM_GETMINMAXINFO)
  {
    MINMAXINFO * info = (MINMAXINFO *) lParam;
    minWindowSizeGet(hwnd, &info->ptMinTrackSize.x, &info->ptMinTrackSize.y);
    return 0;
  }

  return result;
}
