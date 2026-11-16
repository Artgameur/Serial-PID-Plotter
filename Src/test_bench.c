// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Test bench: CSV and PNG files of the tests, in the folder chosen in its box.

   Files: <date>_<time>_<name>, the name without the characters a file name cannot have. The CSV
   has the format of the export, so the Compare tab can open it. The folder is created when needed.

   A picture is taken with lv_snapshot on the LVGL thread: it also works for an object of a tab
   that is not shown. Its transparent parts get the screen's background color, or white in a
   report. For those, the caller lays the object out at that size before the picture
   (benchPictureGet): width on the page x resolution, the height from the format ("16:8"). The
   text is 8 pt at that size, and the file has the resolution, so a document places it at its size.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "test_bench.h"

#include <math.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include "src/libs/lodepng/lodepng.h"

#include "app_window.h"
#include "lang.h"
#include "plot_export.h"
#include "plotter.h"
#include "profile.h"
#include "settings.h"
#include "ui_widgets.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define BENCH_FOLDER_LEN      (MAX_PATH * 3)         // UTF-8
#define BENCH_NAME_LEN        (TEST_NAME_LEN + 24)   // "<date>_<time>_<name>"
#define BENCH_PATH_LEN        (BENCH_FOLDER_LEN + BENCH_NAME_LEN + 8)
#define BENCH_MESSAGE_LEN     (BENCH_PATH_LEN + 64)
#define BENCH_POLL_MS         500                    // background results checked this often
#define BENCH_DEFAULT_FOLDER  "\\Documents\\Serial Dashboard"
#define BENCH_BYTES_RGB       3
#define BENCH_BYTES_ARGB      4
#define BENCH_PICTURE_SCREEN  0           // order of pictureOptions
#define BENCH_PICTURE_REPORT  1
#define BENCH_SIZE_MIN        100         // pixels, either side
#define BENCH_SIZE_MAX        6000
#define BENCH_FORMAT_DEFAULT  2.0f        // width / height of a format that cannot be read
#define BENCH_FORMAT_LEN      12
#define BENCH_TEXT_PT         8           // height of the text on the page
#define BENCH_POINTS_PER_INCH 72
#define BENCH_TEXT_MIN_PX     10
#define BENCH_TEXT_MAX_PX     120
#define BENCH_TEXT_SCREEN_PX  14          // text on screen (before uiPx)
#define BENCH_METERS_PER_INCH 0.0254
#define BENCH_NUMBER_WIDTH    uiPx(72)
#define BENCH_NOTIFY_TITLE    "Serial Dashboard"
#define BENCH_PICTURE_JOBS    2           // pictures encoded at once in the background (each holds
                                          // its snapshot, up to 6000 x 6000 x 4 bytes); more: on
                                          // the LVGL thread, as before
#define PICTURE_FREE          0
#define PICTURE_BUSY          1
#define PICTURE_DONE          2

// lodepng allocates with lv_malloc() in the encoding thread: thread safe with the C library's
// malloc only
#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CLIB
#error "test_bench.c encodes pictures in a thread: LV_USE_STDLIB_MALLOC must be LV_STDLIB_CLIB"
#endif

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static lv_obj_t * autoSwitch;
static lv_obj_t * notifySwitch;
static lv_obj_t * folderInput;
static lv_obj_t * browseBtn;
static lv_obj_t * pictureDropdown;
static lv_obj_t * reportRows[3];
static lv_obj_t * formatInput;
static lv_obj_t * widthInput;
static lv_obj_t * ppiInput;
static lv_obj_t * sizeLabel;

static const char * pictureOptions = "As on screen\nReport";

// Folder dialog, in its own thread. Written by the thread, read once browseDone is set.
static HWND          browseOwner;
static wchar_t       browsePath[MAX_PATH];
static volatile LONG browseDone;
static bool          browseChosen;

// Picture encoded and written in its own thread. The snapshot is taken and destroyed on the LVGL
// thread (pollTimer); the thread only reads its pixels.
typedef struct
{
  volatile LONG   state;                  // PICTURE_xxx
  lv_draw_buf_t * snapshot;
  lv_color_t      background;
  bool            reported;
  int32_t         ppi;
  bool            savedMessage;           // "Picture saved" when done (otherwise only a failure)
  bool            ok;
  char            path[BENCH_PATH_LEN];
} PictureJob_t;

static PictureJob_t pictureJobs[BENCH_PICTURE_JOBS];

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static bool baseNameGet(const char * name, char * base, uint32_t size);
static void pathGet(const char * base, const char * extension, char * path, uint32_t size);
static bool pictureSave(lv_obj_t * obj, const char * path, bool savedMessage, bool wait);
static bool pictureEncode(const lv_draw_buf_t * snapshot, lv_color_t background, bool reported, int32_t ppi, const char * path);
static unsigned __stdcall pictureThread(void * arg);
static void pictureJobsCheck(void);

static lv_obj_t * reportInputCreate(lv_obj_t * row, lv_group_t * group, const char * key, const char * fallback, const char * accepted);
static bool formatRead(float * ratio);
static void pictureSizeGet(BenchPicture_t * picture);

static void pollTimer(lv_timer_t * tim);
static void browseEvent(lv_event_t * e);
static void pictureEvent(lv_event_t * e);
static unsigned __stdcall browseThread(void * arg);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void benchBoxCreate(lv_obj_t * parent, lv_group_t * group)
{
  char       folder[BENCH_FOLDER_LEN];
  lv_obj_t * header;
  lv_obj_t * box  = uiBoxCreate(parent, tr("Test bench"), &header);
  lv_obj_t * row  = uiSettingRowCreate(box, tr("Auto save"));
  const char * home = getenv("USERPROFILE");

  // Saves the tests that end, CSV and picture
  autoSwitch = lv_switch_create(row);
  settingsBindSwitch(autoSwitch, "benchAuto");
  uiTooltipSet(autoSwitch, tr("Each test that ends is saved in the folder: CSV file and picture of the graphs"));

  // Off by default
  row = uiSettingRowCreate(box, tr("Notify"));
  notifySwitch = lv_switch_create(row);
  settingsBindSwitch(notifySwitch, "benchNotify");
  uiTooltipSet(notifySwitch, tr("Windows notification when a test or a chain ends by itself, not when you stop it"));

  row = uiSettingRowCreate(box, tr("Folder"));
  folderInput = uiTextareaCreate(row, tr("Folder"));
  lv_textarea_set_max_length(folderInput, BENCH_FOLDER_LEN - 1);
  snprintf(folder, sizeof(folder), "%s%s", (home != NULL) ? home : "C:", BENCH_DEFAULT_FOLDER);
  lv_textarea_set_text(folderInput, folder);
  settingsBindTextarea(folderInput, "benchFolder");
  lv_group_add_obj(group, folderInput);

  browseBtn = uiButtonCreate(row, LV_SYMBOL_DIRECTORY, browseEvent);
  uiTooltipSet(browseBtn, tr("Choose the folder"));

  row = uiSettingRowCreate(box, tr("Picture"));
  pictureDropdown = uiDropdownCreate(row, tr(pictureOptions), BENCH_PICTURE_REPORT);
  uiTooltipSet(pictureDropdown, tr("A report picture is on white, with text and lines sized for the page."));
  settingsBindDropdown(pictureDropdown, "benchPicture");
  lv_obj_add_event_cb(pictureDropdown, pictureEvent, LV_EVENT_VALUE_CHANGED, NULL);

  reportRows[0] = uiSettingRowCreate(box, tr("Format"));
  formatInput   = reportInputCreate(reportRows[0], group, "benchFormat", "16:8", "0123456789.:");
  uiTooltipSet(formatInput, tr("Width : height of the pictures, ex: 16:8"));

  reportRows[1] = uiSettingRowCreate(box, tr("Page width"));
  widthInput    = reportInputCreate(reportRows[1], group, "benchInches", "6.5", "0123456789.");
  uiTooltipSet(widthInput, tr("Width of the picture on the page"));
  lv_label_set_text(lv_label_create(reportRows[1]), "in");

  reportRows[2] = uiSettingRowCreate(box, tr("Resolution"));
  ppiInput      = reportInputCreate(reportRows[2], group, "benchPpi", "300", "0123456789");
  uiTooltipSet(ppiInput, tr("Pixels per inch: 300 for a printed report"));
  lv_label_set_text(lv_label_create(reportRows[2]), "ppi");

  sizeLabel = lv_label_create(box);
  lv_obj_set_style_text_color(sizeLabel, lv_color_hex(COLOR_INFO), 0);
  pictureEvent(NULL);

  lv_timer_create(pollTimer, BENCH_POLL_MS, NULL);
}

bool benchAutoSave(void)
{
  return lv_obj_has_state(autoSwitch, LV_STATE_CHECKED);
}

void benchNotify(const char * text)
{
  if (lv_obj_has_state(notifySwitch, LV_STATE_CHECKED))
  {
    appWindowNotify(BENCH_NOTIFY_TITLE, text);
  }
}

bool benchPictureGet(BenchPicture_t * picture)
{
  if (lv_dropdown_get_selected(pictureDropdown) == BENCH_PICTURE_SCREEN)
  {
    return false;
  }

  pictureSizeGet(picture);

  int32_t textPx = LV_CLAMP(BENCH_TEXT_MIN_PX, (BENCH_TEXT_PT * picture->ppi) / BENCH_POINTS_PER_INCH, BENCH_TEXT_MAX_PX);

  picture->font  = uiFontAt(textPx);
  picture->scale = (textPx * 100) / uiPx(BENCH_TEXT_SCREEN_PX);
  return true;
}

void benchTestSave(const TestData_t * test, lv_obj_t * obj)
{
  static char    path[BENCH_PATH_LEN];
  static char    message[BENCH_MESSAGE_LEN];
  char           base[BENCH_NAME_LEN];
  PlotExport_t * data = plotExportFromTest(test);

  if (!baseNameGet(test->name, base, sizeof(base)))
  {
    plotExportFree(data);
    plotterToast(tr("Could not create the test bench folder."));
    return;
  }

  pathGet(base, "csv", path, sizeof(path));

  if ((data == NULL) || !plotExportSaveTo(data, path))
  {
    snprintf(message, sizeof(message), tr("Could not write %s"), path);
    plotterToast(message);
  }

  pathGet(base, "png", path, sizeof(path));

  // A failure in the background is told by pictureJobsCheck
  if (!pictureSave(obj, path, false, false))
  {
    snprintf(message, sizeof(message), tr("Could not write %s"), path);
    plotterToast(message);
  }
}

void benchScreenshot(lv_obj_t * obj, const char * name)
{
  static char path[BENCH_PATH_LEN];
  static char message[BENCH_MESSAGE_LEN];
  char        base[BENCH_NAME_LEN];

  if (!baseNameGet(name, base, sizeof(base)))
  {
    plotterToast(tr("Could not create the test bench folder."));
    return;
  }

  pathGet(base, "png", path, sizeof(path));

  // Done in the background: the message comes then (pictureJobsCheck)
  if (!pictureSave(obj, path, true, false))
  {
    snprintf(message, sizeof(message), tr("Could not write %s"), path);
    plotterToast(message);
  }
}

#ifdef SERIAL_DASHBOARD_CLI
bool benchCliPictureSet(const char * format, const char * inches, const char * ppi)
{
  float ratio;

  lv_dropdown_set_selected(pictureDropdown, BENCH_PICTURE_REPORT);
  lv_textarea_set_text(formatInput, format);
  lv_textarea_set_text(widthInput, inches);
  lv_textarea_set_text(ppiInput, ppi);
  return formatRead(&ratio);
}

bool benchCliPictureWrite(lv_obj_t * obj, const char * path)
{
  return pictureSave(obj, path, false, true);
}
#endif

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

static lv_obj_t * reportInputCreate(lv_obj_t * row, lv_group_t * group, const char * key, const char * fallback, const char * accepted)
{
  lv_obj_t * input = uiTextareaCreate(row, "");

  lv_obj_set_flex_grow(input, 0);
  lv_obj_set_width(input, BENCH_NUMBER_WIDTH);
  lv_textarea_set_accepted_chars(input, accepted);
  lv_textarea_set_max_length(input, BENCH_FORMAT_LEN - 1);
  lv_textarea_set_text(input, fallback);
  settingsBindTextarea(input, key);
  lv_obj_add_event_cb(input, pictureEvent, LV_EVENT_VALUE_CHANGED, NULL);
  lv_group_add_obj(group, input);
  return input;
}

// "16:8": width / height; false when it is not two numbers above 0
static bool formatRead(float * ratio)
{
  char *  end;
  const char * text  = lv_textarea_get_text(formatInput);
  float   width = strtof(text, &end);

  if ((end == text) || (*end != ':'))
  {
    return false;
  }

  text = end + 1;

  float height = strtof(text, &end);

  if ((end == text) || (*end != '\0') || !(width > 0.0f) || !(height > 0.0f))
  {
    return false;
  }

  *ratio = width / height;
  return true;
}

// Width on the page x resolution, the height from the format. Past the limits, the sides keep
// the format as well as they can.
static void pictureSizeGet(BenchPicture_t * picture)
{
  float ratio = BENCH_FORMAT_DEFAULT;

  formatRead(&ratio);

  picture->ppi    = LV_MAX(atol(lv_textarea_get_text(ppiInput)), 1);
  picture->width  = (int32_t) lroundf(strtof(lv_textarea_get_text(widthInput), NULL) * (float) picture->ppi);
  picture->width  = LV_CLAMP(BENCH_SIZE_MIN, picture->width, BENCH_SIZE_MAX);
  picture->height = LV_CLAMP(BENCH_SIZE_MIN, (int32_t) lroundf((float) picture->width / ratio), BENCH_SIZE_MAX);
}

// "<date>_<time>_<name>", the folder created when needed; false when it cannot be
static bool baseNameGet(const char * name, char * base, uint32_t size)
{
  wchar_t   folder[MAX_PATH];
  time_t    now   = time(NULL);
  struct tm local = *localtime(&now);
  size_t    used  = strftime(base, size, "%Y%m%d_%H%M%S_", &local);

  snprintf(&base[used], size - used, "%s", name);

  for (char * c = &base[used]; *c != '\0'; ++c)
  {
    if (strchr("\\/:*?\"<>|\t", *c) != NULL)
    {
      *c = '-';
    }
  }

  if (MultiByteToWideChar(CP_UTF8, 0, lv_textarea_get_text(folderInput), -1, folder, MAX_PATH) == 0)
  {
    return false;
  }

  int result = SHCreateDirectoryExW(NULL, folder, NULL);

  return (result == ERROR_SUCCESS) || (result == ERROR_ALREADY_EXISTS) || (result == ERROR_FILE_EXISTS);
}

static void pathGet(const char * base, const char * extension, char * path, uint32_t size)
{
  snprintf(path, size, "%s\\%s.%s", lv_textarea_get_text(folderInput), base, extension);
}

// The snapshot is taken here (LVGL thread); the conversion, PNG encoding and file writing are done
// in a thread (wait = false, a free job) or here (wait = true, or none free). false: no snapshot,
// or (here) the file could not be written; a background failure is told by pictureJobsCheck.
static bool pictureSave(lv_obj_t * obj, const char * path, bool savedMessage, bool wait)
{
  PROF_PUSH(PROF_PICTURE);
  lv_draw_buf_t * snapshot = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888);

  if (snapshot == NULL)
  {
    PROF_POP();
    return false;
  }

  BenchPicture_t picture;
  bool           reported   = benchPictureGet(&picture);
  lv_color_t     background = reported ? lv_color_white() : lv_obj_get_style_bg_color(lv_screen_active(), LV_PART_MAIN);

#ifdef SD_PROFILE
  // Measurement of the former way (everything on the LVGL thread)
  wait = wait || (getenv("SD_PROFILE_PICTURE_SYNC") != NULL);
#endif

  for (uint32_t j = 0; (j < BENCH_PICTURE_JOBS) && !wait; ++j)
  {
    PictureJob_t * job = &pictureJobs[j];

    if (job->state != PICTURE_FREE)
    {
      continue;
    }

    job->snapshot     = snapshot;
    job->background   = background;
    job->reported     = reported;
    job->ppi          = reported ? picture.ppi : 0;
    job->savedMessage = savedMessage;
    job->ok           = false;
    snprintf(job->path, sizeof(job->path), "%s", path);
    job->state        = PICTURE_BUSY;

    uintptr_t thread = _beginthreadex(NULL, 0, pictureThread, job, 0, NULL);

    if (thread != 0)
    {
      CloseHandle((HANDLE) thread);
      PROF_POP();
      return true;
    }

    job->state = PICTURE_FREE;
    break;
  }

  bool ok = pictureEncode(snapshot, background, reported, reported ? picture.ppi : 0, path);

  lv_draw_buf_destroy(snapshot);
  PROF_POP();
  return ok;
}

static unsigned __stdcall pictureThread(void * arg)
{
  PictureJob_t * job = arg;

  job->ok = pictureEncode(job->snapshot, job->background, job->reported, job->ppi, job->path);
  InterlockedExchange(&job->state, PICTURE_DONE);
  return 0;
}

// LVGL thread: pictures done in the background (snapshot destroyed, result told)
static void pictureJobsCheck(void)
{
  static char message[BENCH_MESSAGE_LEN];

  for (uint32_t j = 0; j < BENCH_PICTURE_JOBS; ++j)
  {
    PictureJob_t * job = &pictureJobs[j];

    if (InterlockedCompareExchange(&job->state, PICTURE_DONE, PICTURE_DONE) != PICTURE_DONE)
    {
      continue;
    }

    lv_draw_buf_destroy(job->snapshot);
    job->snapshot = NULL;

    if (!job->ok || job->savedMessage)
    {
      snprintf(message, sizeof(message), tr(job->ok ? "Picture saved: %s" : "Could not write %s"), job->path);
      plotterToast(message);
    }

    InterlockedExchange(&job->state, PICTURE_FREE);
  }
}

// Any thread: no LVGL object or style used (the snapshot's pixels are only read)
static bool pictureEncode(const lv_draw_buf_t * snapshot, lv_color_t background, bool reported, int32_t ppi, const char * path)
{
  uint32_t        width  = snapshot->header.w;
  uint32_t        height = snapshot->header.h;
  uint8_t *       rgb    = malloc((size_t) width * height * BENCH_BYTES_RGB);
  unsigned char * png    = NULL;
  size_t          pngSize = 0;
  bool            ok     = false;

  if (rgb != NULL)
  {
    uint8_t *       write = rgb;

    // Blue, green, red, alpha in memory
    for (uint32_t y = 0; y < height; ++y)
    {
      const uint8_t * read = snapshot->data + ((size_t) y * snapshot->header.stride);

      for (uint32_t x = 0; x < width; ++x, read += BENCH_BYTES_ARGB)
      {
        uint32_t alpha = read[3];

        *write++ = (uint8_t) (((read[2] * alpha) + (background.red * (255 - alpha))) / 255);
        *write++ = (uint8_t) (((read[1] * alpha) + (background.green * (255 - alpha))) / 255);
        *write++ = (uint8_t) (((read[0] * alpha) + (background.blue * (255 - alpha))) / 255);
      }
    }

    LodePNGState state;

    lodepng_state_init(&state);
    state.info_raw.colortype       = LCT_RGB;
    state.info_raw.bitdepth        = 8;
    state.info_png.color.colortype = LCT_RGB;
    state.info_png.color.bitdepth  = 8;

    // A document then places the picture at its size on the page
    if (reported)
    {
      state.info_png.phys_defined = 1;
      state.info_png.phys_unit    = 1;
      state.info_png.phys_x       = (unsigned) lround((double) ppi / BENCH_METERS_PER_INCH);
      state.info_png.phys_y       = state.info_png.phys_x;
    }

    unsigned encodeError = lodepng_encode(&png, &pngSize, rgb, width, height, &state);

    lodepng_state_cleanup(&state);

    if (encodeError == 0)
    {
      wchar_t widePath[MAX_PATH * 2];
      FILE *  file = NULL;

      if (MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, sizeof(widePath) / sizeof(widePath[0])) != 0)
      {
        file = _wfopen(widePath, L"wb");
      }

      if (file != NULL)
      {
        ok = fwrite(png, 1, pngSize, file) == pngSize;
        ok = (fclose(file) == 0) && ok;
      }
    }
  }

  // lodepng allocates with LVGL's functions
  lv_free(png);
  free(rgb);
  return ok;
}

// A CSV written in the background failed; pictures done in the background; the folder dialog
// closed
static void pollTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  pictureJobsCheck();

  if (plotExportSaveFailures() > 0)
  {
    plotterToast(tr("Could not write a CSV file of the test bench."));
  }

  if (InterlockedExchange(&browseDone, 0) != 0)
  {
    lv_obj_set_state(browseBtn, LV_STATE_DISABLED, false);

    if (browseChosen)
    {
      char folder[BENCH_FOLDER_LEN];

      WideCharToMultiByte(CP_UTF8, 0, browsePath, -1, folder, sizeof(folder), NULL, NULL);
      lv_textarea_set_text(folderInput, folder);
    }
  }
}

// The fields of a report, and the size they give
static void pictureEvent(lv_event_t * e)
{
  bool           report = lv_dropdown_get_selected(pictureDropdown) != BENCH_PICTURE_SCREEN;
  BenchPicture_t picture;
  float          ratio;

  LV_UNUSED(e);

  for (uint32_t r = 0; r < (sizeof(reportRows) / sizeof(reportRows[0])); ++r)
  {
    lv_obj_set_hidden(reportRows[r], !report);
  }

  lv_obj_set_hidden(sizeLabel, !report);

  if (!formatRead(&ratio))
  {
    lv_label_set_text(sizeLabel, tr("Format: two numbers, ex: 16:8"));
    lv_obj_set_style_text_color(sizeLabel, lv_color_hex(COLOR_ERROR), 0);
    return;
  }

  pictureSizeGet(&picture);
  lv_label_set_text_fmt(sizeLabel, tr("Pictures of %ld x %ld pixels"), (long) picture.width, (long) picture.height);
  lv_obj_set_style_text_color(sizeLabel, lv_color_hex(COLOR_INFO), 0);
}

// The dialog runs in its own thread, so the tests keep recording
static void browseEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  browseOwner = (HWND) lv_windows_get_display_window_handle(lv_display_get_default());

  uintptr_t thread = _beginthreadex(NULL, 0, browseThread, NULL, 0, NULL);

  if (thread != 0)
  {
    CloseHandle((HANDLE) thread);
    lv_obj_set_state(browseBtn, LV_STATE_DISABLED, true);
  }
}

static unsigned __stdcall browseThread(void * arg)
{
  LV_UNUSED(arg);

  HRESULT     com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  wchar_t     title[64];
  BROWSEINFOW info = {0};

  MultiByteToWideChar(CP_UTF8, 0, tr("Folder of the test bench"), -1, title, sizeof(title) / sizeof(title[0]));

  info.hwndOwner = browseOwner;
  info.lpszTitle = title;
  info.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

  LPITEMIDLIST list = SHBrowseForFolderW(&info);

  browseChosen = (list != NULL) && SHGetPathFromIDListW(list, browsePath);
  CoTaskMemFree(list);

  if (SUCCEEDED(com))
  {
    CoUninitialize();
  }

  InterlockedExchange(&browseDone, 1);
  return 0;
}
