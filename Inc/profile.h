// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Optional profiling (CMake option SD_PROFILE, off by default): time spent per stage,
          per-port data counters, CPU and memory, written once per second to a CSV file. Without
          SD_PROFILE every PROF_xxx macro is empty and nothing of this is compiled in.

   Environment (profiling builds only):
     SD_PROFILE_LOG=<file.csv>       where the rows go (default: %TEMP%\serial_dashboard_profile.csv)
     SD_PROFILE_SECONDS=<n>          the app ends after n seconds
     SD_PROFILE_TAB=<0|1|2>          tab shown at start (Serial, Plot, Compare)
     SD_PROFILE_RECORD_AFTER=<s>     a test recording starts on C1 after s seconds
     SD_PROFILE_STALL=<ms>,<s>       the LVGL thread is held ms milliseconds every s seconds
     SD_SIM_PORTS=<1..4>             simulated ports SIM1..SIMn listed with the real ones
     SD_SIM_RATE=<lines/s>           lines per second of each simulated port (default 1000)
     SD_SIM_VALUES=<n>               values per line, the first one a sequence number (default 8)
*/
// ------------------------------------------------------------------------------------------------

#ifndef PROFILE_H
#define PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// Stages timed (exclusive: a stage's time does not include the stages it calls)
typedef enum
{
  PROF_ACQ,             // serial reader threads: reading the driver and queueing chunks
  PROF_QUEUE,           // LVGL thread: taking the chunks out of the queues
  PROF_TERMINAL,        // received bytes formatted for the terminal
  PROF_TERMINAL_LABEL,  // terminal label updated with the new text
  PROF_PARSE,           // lines split and parsed into values
  PROF_RECORD,          // test recording: samples, conditions, plateau
  PROF_PLOT_STORE,      // samples stored in the plot history
  PROF_PLOT_PREP,       // plot chart data prepared (min / max per pixel column, Y range)
  PROF_COMPARE_PREP,    // Compare graph data prepared
  PROF_LAYOUT,          // LVGL layout before a refresh
  PROF_RENDER,          // LVGL rendering and flush to the window
  PROF_PICTURE,         // report picture saved (snapshot, conversion, PNG, file)
  PROF_SETTINGS_SAVE,   // settings file written
  PROF_PORT_SCAN,       // serial port list checked
  PROF_UI_LOOP,         // whole lv_timer_handler() call: all the LVGL thread's work
  PROF_STAGE_COUNT
} ProfStage_t;

#define PROF_CONN_MAX         4

#ifdef SD_PROFILE

uint64_t profNow(void);

// LVGL thread: a stage starts / ends. Stages nest: the enclosing one is paused meanwhile, so each
// one gets only its own time.
void profPush(ProfStage_t stage);
void profPop(void);

// Other threads (serial readers): time since start (profNow()) added to stage
void profAddThreadSafe(ProfStage_t stage, uint64_t start);

// Data lines parsed on conn; values[0] of a simulated line is its sequence number
void profLine(uint32_t conn, const float * values, uint32_t count);
void profSamplesPlotted(uint32_t count);
void profSamplesRecorded(uint32_t count);
void profChartPoints(uint32_t viewSamples, uint32_t points);

// LVGL thread: once at start (display events, scenario), then after each lv_timer_handler()
void profInit(lv_display_t * display);
void profScenarioStart(lv_obj_t * tabview);
void profLoop(uint64_t handlerStart);

// Simulated ports (serial.c)
uint32_t profSimPorts(void);
uint32_t profSimRate(void);
uint32_t profSimValues(void);

#define PROF_PUSH(stage)                profPush(stage)
#define PROF_POP()                      profPop()
#define PROF_START(var)                 uint64_t var = profNow()
#define PROF_END_THREAD(stage, var)     profAddThreadSafe((stage), (var))
#define PROF_DO(code)                   code

#else

#define PROF_PUSH(stage)
#define PROF_POP()
#define PROF_START(var)
#define PROF_END_THREAD(stage, var)
#define PROF_DO(code)

#endif

#endif //PROFILE_H
