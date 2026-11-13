#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "lvgl.h"
#include "app_window.h"
#include "screens.h"
#include "settings.h"
#include "lvgl/examples/lv_examples.h"
#include "lvgl/demos/lv_demos.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define SCREEN_BUFFER_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT)

static HANDLE serial_handle = INVALID_HANDLE_VALUE;
static HWND g_hwnd = NULL;
static lv_obj_t *title_label = NULL;
static lv_obj_t *port_label = NULL;
static lv_obj_t *rx_label = NULL;
static lv_obj_t *status_panel = NULL;
static lv_obj_t *slider = NULL;
static lv_obj_t *slider_value_label = NULL;
static lv_indev_t *mouse_indev = NULL;
static lv_coord_t mouse_x = 0;
static lv_coord_t mouse_y = 0;
static bool mouse_pressed = false;
static bool mouse_valid = false;

static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);

    data->point.x = mouse_x;
    data->point.y = mouse_y;
    data->state = mouse_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;

    if (!mouse_valid)
    {
        data->state = LV_INDEV_STATE_REL;
        data->point.x = 0;
        data->point.y = 0;
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_MOUSEMOVE:
            mouse_x = (lv_coord_t)LOWORD(lParam);
            mouse_y = (lv_coord_t)HIWORD(lParam);
            mouse_valid = true;
            mouse_pressed = (wParam & MK_LBUTTON) != 0;
            return 0;
        case WM_LBUTTONDOWN:
            mouse_x = (lv_coord_t)LOWORD(lParam);
            mouse_y = (lv_coord_t)HIWORD(lParam);
            mouse_valid = true;
            mouse_pressed = true;
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            mouse_x = (lv_coord_t)LOWORD(lParam);
            mouse_y = (lv_coord_t)HIWORD(lParam);
            mouse_valid = true;
            mouse_pressed = false;
            ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            mouse_pressed = false;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (g_hwnd == NULL)
    {
        lv_display_flush_ready(disp);
        return;
    }
    lv_color_t *color_p = (lv_color_t *)px_map;


    static uint32_t pixel_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];
    memset(pixel_buffer, 0, sizeof(pixel_buffer));

    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2;
    int32_t y2 = area->y2;

    for (int32_t y = y1; y <= y2; ++y)
    {
        for (int32_t x = x1; x <= x2; ++x)
        {
            int idx = (y * SCREEN_WIDTH) + x;
            int local_idx = ((y - y1) * (x2 - x1 + 1)) + (x - x1);
            pixel_buffer[idx] = lv_color_to_u32(color_p[local_idx]);
        }
    }

    HDC hdc = GetDC(g_hwnd);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = SCREEN_WIDTH;
    bmi.bmiHeader.biHeight = -SCREEN_HEIGHT;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(
        hdc,
        0,
        0,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        0,
        0,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        pixel_buffer,
        &bmi,
        DIB_RGB_COLORS,
        SRCCOPY);

    ReleaseDC(g_hwnd, hdc);
    lv_display_flush_ready(disp);
}

static void slider_event_cb(lv_event_t *event)
{
    lv_obj_t *slider_obj = lv_event_get_target(event);
    if (slider_value_label != NULL)
    {
        lv_label_set_text_fmt(slider_value_label, "Value: %d", lv_slider_get_value(slider_obj));
    }
}

static void set_status(const char *message)
{
    if (rx_label != NULL)
    {
        lv_label_set_text(rx_label, message);
    }
}

static void list_ports(void)
{
    char text[256] = "COM ports: ";
    bool any_found = false;

    for (int i = 1; i <= 256; ++i)
    {
        char port_name[32];
        char full_name[32];
        HANDLE h;

        snprintf(port_name, sizeof(port_name), "COM%d", i);
        snprintf(full_name, sizeof(full_name), "\\\\.\\%s", port_name);

        h = CreateFileA(
            full_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);

        if (h != INVALID_HANDLE_VALUE)
        {
            char entry[32];
            size_t used = strlen(text);
            snprintf(entry, sizeof(entry), "%s ", port_name);
            if (used + strlen(entry) < sizeof(text))
            {
                strncat(text, entry, sizeof(text) - used - 1);
            }
            any_found = true;
            CloseHandle(h);
        }
    }

    if (!any_found)
    {
        strncat(text, "not available", sizeof(text) - strlen(text) - 1);
    }

    if (port_label != NULL)
    {
        lv_label_set_text(port_label, text);
    }
}

static void open_serial_port(void)
{
    const char *port_name = "COM3";
    char full_port_name[32];
    DCB dcb = {0};
    COMMTIMEOUTS timeouts = {0};

    snprintf(full_port_name, sizeof(full_port_name), "\\\\.\\%s", port_name);
    serial_handle = CreateFileA(
        full_port_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (serial_handle == INVALID_HANDLE_VALUE)
    {
        set_status("Status: COM open failed");
        return;
    }

    if (!GetCommState(serial_handle, &dcb))
    {
        set_status("Status: GetCommState failed");
        CloseHandle(serial_handle);
        serial_handle = INVALID_HANDLE_VALUE;
        return;
    }

    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;

    if (!SetCommState(serial_handle, &dcb))
    {
        set_status("Status: SetCommState failed");
        CloseHandle(serial_handle);
        serial_handle = INVALID_HANDLE_VALUE;
        return;
    }

    timeouts.ReadIntervalTimeout = 1;
    timeouts.ReadTotalTimeoutConstant = 1;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1;
    timeouts.WriteTotalTimeoutMultiplier = 0;

    if (!SetCommTimeouts(serial_handle, &timeouts))
    {
        set_status("Status: SetCommTimeouts failed");
        CloseHandle(serial_handle);
        serial_handle = INVALID_HANDLE_VALUE;
        return;
    }

    set_status("Status: COM3 connected");
}

static void serial_poll_cb(lv_timer_t *timer)
{
    char buffer[128] = {0};
    DWORD bytes_read = 0;
    LV_UNUSED(timer);

    if (serial_handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    if (ReadFile(serial_handle, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0)
    {
        buffer[bytes_read] = '\0';

        char message[256];
        snprintf(message, sizeof(message), "RX: %s", buffer);
        set_status(message);
    }
}

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
        uint32_t time_till_next = lv_timer_handler();
        // handle LV_NO_TIMER_READY. Another option is to always sleep a few milliseconds
        if(time_till_next == LV_NO_TIMER_READY) time_till_next = LV_DEF_REFR_PERIOD;
        lv_sleep_ms(time_till_next);
    }
    return 0;
}

/*
int main(void)
{
    loadScreen();
    return 0;
}
*/
/*
int main(void)
{
    static lv_color_t display_buffer[SCREEN_BUFFER_SIZE];
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_drv_t display_driver;
    lv_obj_t *screen;
    WNDCLASSEXA window_class = {0};
    MSG msg;

    lv_init();

    lv_disp_draw_buf_init(&draw_buf, display_buffer, NULL, SCREEN_BUFFER_SIZE);

    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = SCREEN_WIDTH;
    display_driver.ver_res = SCREEN_HEIGHT;
    display_driver.draw_buf = &draw_buf;
    display_driver.flush_cb = flush_cb;
    lv_disp_drv_register(&display_driver);

    {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = mouse_read_cb;
        mouse_indev = lv_indev_drv_register(&indev_drv);
    }

    window_class.cbSize = sizeof(WNDCLASSEXA);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.lpszClassName = "SerialDashboardWindow";
    if (!RegisterClassExA(&window_class))
    {
        fprintf(stderr, "RegisterClassExA failed\n");
        return 1;
    }

    g_hwnd = CreateWindowExA(
        0,
        "SerialDashboardWindow",
        "Serial Dashboard",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        NULL,
        NULL,
        window_class.hInstance,
        NULL);

    if (g_hwnd == NULL)
    {
        fprintf(stderr, "CreateWindowExA failed\n");
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);

    screen = lv_obj_create(NULL);
    lv_scr_load(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101828), LV_PART_MAIN);

    status_panel = lv_obj_create(screen);
    lv_obj_set_size(status_panel, 440, 220);
    lv_obj_align(status_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(status_panel, lv_color_hex(0x1f2937), LV_PART_MAIN);
    lv_obj_set_style_radius(status_panel, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_panel, 18, LV_PART_MAIN);

    title_label = lv_label_create(status_panel);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(title_label, "Serial Dashboard");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xffffff), LV_PART_MAIN);

    port_label = lv_label_create(status_panel);
    lv_obj_align(port_label, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_label_set_text(port_label, "COM ports: scanning...");

    rx_label = lv_label_create(status_panel);
    lv_obj_align(rx_label, LV_ALIGN_TOP_LEFT, 0, 80);
    lv_label_set_text(rx_label, "Status: waiting");

    slider = lv_slider_create(screen);
    lv_obj_set_size(slider, 220, 20);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    slider_value_label = lv_label_create(screen);
    lv_obj_align_to(slider_value_label, slider, LV_ALIGN_OUT_TOP_MID, 0, -12);
    lv_label_set_text(slider_value_label, "Value: 50");

    list_ports();
    open_serial_port();

    lv_timer_create(serial_poll_cb, 100, NULL);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        lv_tick_inc(5);
        lv_task_handler();
        Sleep(5);
    }

    return 0;
}
*/