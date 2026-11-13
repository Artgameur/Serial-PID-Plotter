// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Export of the plotted data to a CSV file, chosen with the Windows "Save as" dialog.

   File: UTF-8 (with BOM, so Excel shows the units' symbols), comma separated, '.' decimals.
   Columns: sample number, date and time, seconds since the first row, then one per variable
   ("Name [unit]", displayed values: gain and offset applied). Missing values are empty cells.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "plot_export.h"

#include <math.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>

#include "lvgl.h"
#include "lang.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define EXPORT_PATH_LEN       MAX_PATH
#define FILETIME_PER_MS       10000ULL    // FILETIME: 100 ns units
#define UNIX_TO_FILETIME_MS   11644473600000LL

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

// Written by the export thread, read by the LVGL thread once the state is DONE
static volatile LONG  exportState = PLOT_EXPORT_IDLE;
static char           exportMessage[PLOT_EXPORT_MESSAGE_LEN];
static PlotExport_t * exportData;
static wchar_t        exportPath[EXPORT_PATH_LEN];   // UTF-16: any folder / file name
static HWND           exportOwner;
static volatile LONG  saveFailures;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// File written without dialog
typedef struct
{
  PlotExport_t * data;
  wchar_t        path[EXPORT_PATH_LEN * 2];
} SaveJob_t;

static unsigned __stdcall exportThread(void * arg);
static unsigned __stdcall saveThread(void * arg);
static bool               pathAsk(void);
static bool               fileWrite(const PlotExport_t * data, const wchar_t * path);
static void               quotedWrite(const char * text, FILE * file);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

PlotExport_t * plotExportAlloc(uint32_t sampleCount, uint32_t varCount)
{
  PlotExport_t * data = calloc(1, sizeof(PlotExport_t));

  if (data == NULL)
  {
    return NULL;
  }

  data->sampleCount = sampleCount;
  data->varCount    = varCount;
  data->ticks       = malloc(((size_t) sampleCount + 1) * sizeof(uint32_t));
  data->values      = malloc((((size_t) sampleCount * varCount) + 1) * sizeof(float));

  if ((data->ticks == NULL) || (data->values == NULL))
  {
    plotExportFree(data);
    return NULL;
  }

  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  data->endFileTime = ((uint64_t) now.dwHighDateTime << 32) | now.dwLowDateTime;
  data->endTick     = lv_tick_get();
  return data;
}

void plotExportFree(PlotExport_t * data)
{
  if (data != NULL)
  {
    free(data->ticks);
    free(data->values);
    free(data);
  }
}

// Times as recorded: the file gets the date and time of the test. A test from a file has no
// date: it keeps the current one.
PlotExport_t * plotExportFromTest(const TestData_t * test)
{
  PlotExport_t * data = (test->sampleCount > 0) ? plotExportAlloc(test->sampleCount, test->columnCount) : NULL;

  if (data == NULL)
  {
    return NULL;
  }

  uint32_t lastMs = test->times[test->sampleCount - 1];

  data->firstSample = 1;
  data->endTick     = lastMs;

  if (test->startMs != 0)
  {
    data->endFileTime = (uint64_t) (test->startMs + lastMs + UNIX_TO_FILETIME_MS) * FILETIME_PER_MS;
  }

  memcpy(data->headers, test->headers, sizeof(test->headers));
  memcpy(data->settings, test->settings, sizeof(test->settings));
  memcpy(data->commands, test->commands, sizeof(test->commands));
  memcpy(data->ticks, test->times, test->sampleCount * sizeof(uint32_t));
  memcpy(data->values, test->values, (size_t) test->sampleCount * test->columnCount * sizeof(float));
  return data;
}

bool plotExportSaveTo(PlotExport_t * data, const char * path)
{
  SaveJob_t * job = malloc(sizeof(SaveJob_t));

  if ((job == NULL) || (MultiByteToWideChar(CP_UTF8, 0, path, -1, job->path, sizeof(job->path) / sizeof(job->path[0])) == 0))
  {
    free(job);
    plotExportFree(data);
    return false;
  }

  job->data = data;

  uintptr_t thread = _beginthreadex(NULL, 0, saveThread, job, 0, NULL);

  if (thread == 0)
  {
    plotExportFree(data);
    free(job);
    return false;
  }

  CloseHandle((HANDLE) thread);
  return true;
}

uint32_t plotExportSaveFailures(void)
{
  return (uint32_t) InterlockedExchange(&saveFailures, 0);
}

bool plotExportStart(PlotExport_t * data)
{
  SYSTEMTIME local;
  char       name[64];

  if (InterlockedCompareExchange(&exportState, PLOT_EXPORT_BUSY, PLOT_EXPORT_IDLE) != PLOT_EXPORT_IDLE)
  {
    plotExportFree(data);
    return false;
  }

  exportData = data;
  GetLocalTime(&local);
  snprintf(name, sizeof(name), "plot_%04u-%02u-%02u_%02u%02u%02u.csv", local.wYear, local.wMonth, local.wDay,
           local.wHour, local.wMinute, local.wSecond);
  MultiByteToWideChar(CP_UTF8, 0, name, -1, exportPath, EXPORT_PATH_LEN);
  // The dialog is modal to the app's window
  exportOwner = (HWND) lv_windows_get_display_window_handle(lv_display_get_default());

  uintptr_t thread = _beginthreadex(NULL, 0, exportThread, NULL, 0, NULL);

  if (thread == 0)
  {
    exportData = NULL;
    plotExportFree(data);
    InterlockedExchange(&exportState, PLOT_EXPORT_IDLE);
    return false;
  }

  CloseHandle((HANDLE) thread);
  return true;
}

PlotExportState_t plotExportPoll(char * message, uint32_t size)
{
  if (InterlockedCompareExchange(&exportState, PLOT_EXPORT_DONE, PLOT_EXPORT_DONE) != PLOT_EXPORT_DONE)
  {
    return (PlotExportState_t) exportState;
  }

  snprintf(message, size, "%s", exportMessage);
  InterlockedExchange(&exportState, PLOT_EXPORT_IDLE);
  return PLOT_EXPORT_DONE;
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

#ifdef SERIAL_DASHBOARD_CLI
bool plotExportCliWrite(const PlotExport_t * data, const char * path)
{
  wchar_t widePath[MAX_PATH * 2];

  return (MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, sizeof(widePath) / sizeof(widePath[0])) != 0) &&
         fileWrite(data, widePath);
}
#endif

static unsigned __stdcall saveThread(void * arg)
{
  SaveJob_t * job = arg;

  if (!fileWrite(job->data, job->path))
  {
    InterlockedIncrement(&saveFailures);
  }

  plotExportFree(job->data);
  free(job);
  return 0;
}

static unsigned __stdcall exportThread(void * arg)
{
  LV_UNUSED(arg);

  // The Explorer style dialog uses COM
  HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  char    path[EXPORT_PATH_LEN * 3];       // UTF-8 (LVGL text)

  if (!pathAsk())
  {
    // Canceled: no message
    exportMessage[0] = '\0';
  }
  else
  {
    WideCharToMultiByte(CP_UTF8, 0, exportPath, -1, path, sizeof(path), NULL, NULL);

    if (fileWrite(exportData, exportPath))
    {
      snprintf(exportMessage, sizeof(exportMessage), tr("Exported %lu samples to %s"),
               (unsigned long) exportData->sampleCount, path);
    }
    else
    {
      snprintf(exportMessage, sizeof(exportMessage), tr("Could not write %s"), path);
    }
  }

  if (SUCCEEDED(com))
  {
    CoUninitialize();
  }

  plotExportFree(exportData);
  exportData = NULL;
  InterlockedExchange(&exportState, PLOT_EXPORT_DONE);
  return 0;
}

// exportPath: default file name in, chosen path out (Unicode dialog: accents in the title and in
// the folder / file names)
static bool pathAsk(void)
{
  OPENFILENAMEW dialog = {0};
  wchar_t       title[64];

  MultiByteToWideChar(CP_UTF8, 0, tr("Export plot data"), -1, title, sizeof(title) / sizeof(title[0]));

  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner   = exportOwner;
  // "\0" separated pairs: not a single text for tr()
  dialog.lpstrFilter = (langGet() == LANG_FR) ? L"Fichier CSV (*.csv)\0*.csv\0Tous les fichiers (*.*)\0*.*\0"
                                              : L"CSV file (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
  dialog.lpstrFile   = exportPath;
  dialog.nMaxFile    = EXPORT_PATH_LEN;
  dialog.lpstrDefExt = L"csv";
  dialog.lpstrTitle  = title;
  dialog.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

  return GetSaveFileNameW(&dialog) != FALSE;
}

static bool fileWrite(const PlotExport_t * data, const wchar_t * path)
{
  FILE * file = _wfopen(path, L"wb");

  if (file == NULL)
  {
    return false;
  }

  // BOM, then the header
  fputs("\xEF\xBB\xBF", file);
  fputs(tr("Sample,Date time,Time (s)"), file);

  for (uint32_t v = 0; v < data->varCount; ++v)
  {
    // Quoted: a name can contain a comma
    fputc(',', file);
    quotedWrite(data->headers[v], file);
  }

  fputs("\r\n", file);

  for (uint32_t s = 0; s < data->sampleCount; ++s)
  {
    // Wall clock of the sample: the copy's time minus the sample's age
    uint32_t       age  = data->endTick - data->ticks[s];
    ULARGE_INTEGER time = {.QuadPart = data->endFileTime - ((uint64_t) age * FILETIME_PER_MS)};
    FILETIME       utc  = {time.LowPart, time.HighPart};
    SYSTEMTIME     utcTime;
    SYSTEMTIME     local;

    FileTimeToSystemTime(&utc, &utcTime);
    if (!SystemTimeToTzSpecificLocalTime(NULL, &utcTime, &local))
    {
      local = utcTime;
    }

    fprintf(file, "%llu,%04u-%02u-%02u %02u:%02u:%02u.%03u,%.3f", (unsigned long long) (data->firstSample + s),
            local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond, local.wMilliseconds,
            (double) (data->ticks[s] - data->ticks[0]) / 1000.0);

    const float * row = &data->values[(size_t) s * data->varCount];

    for (uint32_t v = 0; v < data->varCount; ++v)
    {
      if (isnan(row[v]))
      {
        fputc(',', file);
      }
      else
      {
        fprintf(file, ",%.7g", (double) row[v]);
      }
    }

    fputs("\r\n", file);
  }

  // Last row, so the file stays a plain table for a spreadsheet
  if (data->settings[0] != '\0')
  {
    fputs("#settings,", file);
    quotedWrite(data->settings, file);
    fputs("\r\n", file);
  }

  if (data->commands[0] != '\0')
  {
    fputs("#commands,", file);
    quotedWrite(data->commands, file);
    fputs("\r\n", file);
  }

  bool ok = !ferror(file);
  return (fclose(file) == 0) && ok;
}

static void quotedWrite(const char * text, FILE * file)
{
  fputc('"', file);

  for (const char * c = text; *c != '\0'; ++c)
  {
    if (*c == '"')
    {
      fputc('"', file);
    }

    fputc(*c, file);
  }

  fputc('"', file);
}
