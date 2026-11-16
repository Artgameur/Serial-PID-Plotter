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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "lang.h"
#include "platform.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define EXPORT_PATH_LEN       PLATFORM_PATH_LEN

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

// Written by the export thread, read by the LVGL thread once the state is DONE
static atomic_int     exportState = PLOT_EXPORT_IDLE;
static char           exportMessage[PLOT_EXPORT_MESSAGE_LEN];
static PlotExport_t * exportData;
static char           exportPath[EXPORT_PATH_LEN];   // UTF-8
static atomic_uint    saveFailures;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// File written without dialog
typedef struct
{
  PlotExport_t * data;
  char           path[EXPORT_PATH_LEN];
} SaveJob_t;

static void exportThread(void * arg);
static void saveThread(void * arg);
static bool fileWrite(const PlotExport_t * data, const char * path);
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

  data->endUnixMs = platformUnixMs();
  data->endTick   = lv_tick_get();
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
    data->endUnixMs = test->startMs + lastMs;
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

  if ((job == NULL) || ((size_t) snprintf(job->path, sizeof(job->path), "%s", path) >= sizeof(job->path)))
  {
    free(job);
    plotExportFree(data);
    return false;
  }

  job->data = data;

  if (!platformThreadStart(saveThread, job))
  {
    plotExportFree(data);
    free(job);
    return false;
  }

  return true;
}

uint32_t plotExportSaveFailures(void)
{
  return atomic_exchange(&saveFailures, 0);
}

bool plotExportStart(PlotExport_t * data)
{
  PlatformTime_t local;
  int            idle = PLOT_EXPORT_IDLE;

  if (!atomic_compare_exchange_strong(&exportState, &idle, PLOT_EXPORT_BUSY))
  {
    plotExportFree(data);
    return false;
  }

  exportData = data;
  platformLocalTime(platformUnixMs(), &local);
  snprintf(exportPath, sizeof(exportPath), "plot_%04d-%02d-%02d_%02d%02d%02d.csv", local.year, local.month, local.day,
           local.hour, local.minute, local.second);

  if (!platformThreadStart(exportThread, NULL))
  {
    exportData = NULL;
    plotExportFree(data);
    atomic_store(&exportState, PLOT_EXPORT_IDLE);
    return false;
  }

  return true;
}

PlotExportState_t plotExportPoll(char * message, uint32_t size)
{
  int state = atomic_load(&exportState);

  if (state != PLOT_EXPORT_DONE)
  {
    return (PlotExportState_t) state;
  }

  snprintf(message, size, "%s", exportMessage);
  atomic_store(&exportState, PLOT_EXPORT_IDLE);
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
  return fileWrite(data, path);
}
#endif

static void saveThread(void * arg)
{
  SaveJob_t * job = arg;

  if (!fileWrite(job->data, job->path))
  {
    atomic_fetch_add(&saveFailures, 1);
  }

  plotExportFree(job->data);
  free(job);
}

static void exportThread(void * arg)
{
  LV_UNUSED(arg);

  // exportPath: default file name in, chosen path out
  PlatformFilter_t filter = {tr("CSV file"), "csv", tr("All files")};
  bool             chosen = platformSaveDialog(tr("Export plot data"), &filter, exportPath, sizeof(exportPath));
  const char *     path   = exportPath;

  if (!chosen)
  {
    // Canceled: no message
    exportMessage[0] = '\0';
  }
  else
  {
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

  plotExportFree(exportData);
  exportData = NULL;
  atomic_store(&exportState, PLOT_EXPORT_DONE);
}

static bool fileWrite(const PlotExport_t * data, const char * path)
{
  FILE * file = platformFopen(path, "wb");

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
    uint32_t       age = data->endTick - data->ticks[s];
    PlatformTime_t local;

    platformLocalTime(data->endUnixMs - (int64_t) age, &local);

    fprintf(file, "%llu,%04d-%02d-%02d %02d:%02d:%02d.%03d,%.3f", (unsigned long long) (data->firstSample + s),
            local.year, local.month, local.day, local.hour, local.minute, local.second, local.ms,
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
