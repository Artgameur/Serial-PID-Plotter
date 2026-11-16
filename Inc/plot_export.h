// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Export of the plotted data to a CSV file, chosen with the Windows "Save as" dialog.

   The dialog and the file writing run in their own thread (the UI keeps running); the data is a
   copy taken when the export starts.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef PLOT_EXPORT_H
#define PLOT_EXPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "plotter.h"

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define PLOT_EXPORT_HEADER_LEN  TEST_HEADER_LEN   // "Name [unit]" of a column
#define PLOT_EXPORT_MESSAGE_LEN 300

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  uint32_t   varCount;
  uint32_t   sampleCount;
  uint64_t   firstSample;                 // number of the first sample (since the app started)
  uint32_t   endTick;                     // lv_tick_get() and wall clock (Unix ms, UTC) when
  int64_t    endUnixMs;                   // the copy was taken: give each sample's date and time
  char       headers[PLOTTER_MAX_VARS][PLOT_EXPORT_HEADER_LEN];
  char       settings[TEST_SETTINGS_LEN]; // device settings: last rows of the file (empty: none)
  char       commands[TEST_COMMANDS_LEN]; // commands sent
  uint32_t * ticks;                       // [sampleCount]: lv_tick_get() of each sample
  float *    values;                      // [sampleCount][varCount]: displayed values, NAN = none
} PlotExport_t;

typedef enum
{
  PLOT_EXPORT_IDLE,
  PLOT_EXPORT_BUSY,                       // dialog open / file being written
  PLOT_EXPORT_DONE,                       // result message ready (read once)
} PlotExportState_t;

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// Allocates a copy for sampleCount x varCount values (ticks and values) and records the current
// time (fill it right away); NULL if out of memory
PlotExport_t * plotExportAlloc(uint32_t sampleCount, uint32_t varCount);
void           plotExportFree(PlotExport_t * data);

// Opens the "Save as" dialog (default name: plot_<date>_<time>.csv), then writes the file. Takes
// ownership of data (freed when done). false (data freed) if an export is already running or the
// thread could not be started.
bool plotExportStart(PlotExport_t * data);

// Copy of a test (same file as an export of the plots); NULL if empty or out of memory
PlotExport_t * plotExportFromTest(const TestData_t * test);

// Writes data to path (UTF-8) in the background, without dialog. Takes ownership of data; false
// (data freed) when it could not start. Failures of the writing are counted:
// plotExportSaveFailures() gives how many since it was last called.
bool     plotExportSaveTo(PlotExport_t * data, const char * path);
uint32_t plotExportSaveFailures(void);

// Call from the LVGL thread: when DONE, message receives the result and the state goes back to IDLE
PlotExportState_t plotExportPoll(char * message, uint32_t size);

// plotExportStart with the plots' Export button disabled meanwhile and the result shown (plotter.c)
void plotterExport(PlotExport_t * data);

#ifdef SERIAL_DASHBOARD_CLI               // only in the command line tool (agent/), not in the app
// Writes data to path (UTF-8) now; data stays the caller's
bool plotExportCliWrite(const PlotExport_t * data, const char * path);
#endif

#endif //PLOT_EXPORT_H
