// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Optional profiling (SD_PROFILE): see profile.h. Not compiled in without SD_PROFILE.
*/
// ------------------------------------------------------------------------------------------------
#include "profile.h"

#ifdef SD_PROFILE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#include "compare.h"

// ================================================================================================
//            PRIVATE DEFINE / STRUCTURE / VARIABLE DECLARATION
// ================================================================================================

#define PROF_STACK_MAX        16
#define PROF_PERIOD_MS        1000
#define PROF_SLOW_FRAME_MS    33          // frames (refreshes) longer than this are counted

typedef struct
{
  uint64_t total;                         // QPC ticks in the period
  uint64_t max;                           // longest single run in the period (exclusive time)
  uint32_t calls;
} ProfStat_t;

typedef struct
{
  ProfStage_t stage;
  uint64_t    start;                      // when it (re)started running
  uint64_t    own;                        // its exclusive time so far
} ProfFrame_t;

static const char * const stageNames[PROF_STAGE_COUNT] =
{
  "acq", "queue", "terminal", "terminal_label", "parse", "record", "plot_store", "plot_prep",
  "compare_prep", "layout", "render", "picture", "settings_save", "port_scan", "ui_loop",
};

static LARGE_INTEGER    frequency;
static ProfStat_t       stats[PROF_STAGE_COUNT];
static volatile LONG64  acqTotal;         // from the reader threads
static volatile LONG64  acqMax;
static volatile LONG    acqCalls;
static ProfFrame_t      stack[PROF_STACK_MAX];
static uint32_t         depth;

static uint64_t         lines[PROF_CONN_MAX];
static uint64_t         seqGaps[PROF_CONN_MAX];
static uint64_t         seqLost[PROF_CONN_MAX];  // samples missing according to the sequence numbers
static int64_t          seqLast[PROF_CONN_MAX] = {-1, -1, -1, -1};
static uint64_t         samplesPlotted;
static uint64_t         samplesRecorded;
static uint64_t         viewSamples;
static uint64_t         chartPoints;
static uint32_t         frames;
static uint32_t         slowFrames;
static uint64_t         frameStart;
static uint64_t         frameMax;

// Totals since the start (summary at the end)
static uint64_t         linesTotal[PROF_CONN_MAX];
static uint64_t         lostTotal[PROF_CONN_MAX];
static uint64_t         handlerMaxEver;

static FILE *           logFile;
static uint64_t         periodStart;
static uint64_t         appStart;
static ULONGLONG        cpuLast;
static uint32_t         simPorts;
static uint32_t         simRate = 1000;
static uint32_t         simValues = 8;
static lv_obj_t *       scenarioTabview;

// ================================================================================================
//            PRIVATE FUNCTIONS
// ================================================================================================

static double ticksMs(uint64_t ticks)
{
  return ((double) ticks * 1000.0) / (double) frequency.QuadPart;
}

static void statAdd(ProfStage_t stage, uint64_t ticks)
{
  stats[stage].total += ticks;
  stats[stage].max    = (ticks > stats[stage].max) ? ticks : stats[stage].max;
  stats[stage].calls++;
}

static ULONGLONG cpuTicks(void)
{
  FILETIME created, exited, kernel, user;

  GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
  return (((ULONGLONG) kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
         (((ULONGLONG) user.dwHighDateTime << 32) | user.dwLowDateTime);
}

static uint32_t envInt(const char * name, uint32_t fallback)
{
  const char * text = getenv(name);
  return ((text != NULL) && (text[0] != '\0')) ? (uint32_t) strtoul(text, NULL, 10) : fallback;
}

static void displayEvent(lv_event_t * e)
{
  switch (lv_event_get_code(e))
  {
    case LV_EVENT_REFR_START:
      frameStart = profNow();
      profPush(PROF_LAYOUT);
      break;

    case LV_EVENT_RENDER_START:
      profPush(PROF_RENDER);
      break;

    case LV_EVENT_RENDER_READY:
      profPop();
      break;

    case LV_EVENT_REFR_READY:
    {
      profPop();

      uint64_t length = profNow() - frameStart;

      frames++;
      slowFrames += (ticksMs(length) > PROF_SLOW_FRAME_MS) ? 1 : 0;
      frameMax    = (length > frameMax) ? length : frameMax;
      break;
    }

    default:
      break;
  }
}

static void header(void)
{
  fprintf(logFile, "t_s,cpu_pct,private_mb,working_mb,frames,slow_frames,frame_max_ms,ui_loop_max_ms");

  for (uint32_t s = 0; s < PROF_STAGE_COUNT; ++s)
  {
    fprintf(logFile, ",%s_ms,%s_max_ms,%s_calls", stageNames[s], stageNames[s], stageNames[s]);
  }

  for (uint32_t c = 0; c < PROF_CONN_MAX; ++c)
  {
    fprintf(logFile, ",c%u_lines,c%u_seq_gaps,c%u_seq_lost", c + 1, c + 1, c + 1);
  }

  fprintf(logFile, ",samples_plotted,samples_recorded,view_samples,chart_points\n");
}

static void rowWrite(double seconds)
{
  ULONGLONG cpu = cpuTicks();
  double    cpuPct = ((double) (cpu - cpuLast) / 10000.0) / ticksMs(profNow() - periodStart) * 100.0;
  PROCESS_MEMORY_COUNTERS_EX memory = {.cb = sizeof(memory)};

  cpuLast = cpu;
  K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *) &memory, sizeof(memory));

  // Reader threads' part
  stats[PROF_ACQ].total = (uint64_t) InterlockedExchange64(&acqTotal, 0);
  stats[PROF_ACQ].max   = (uint64_t) InterlockedExchange64(&acqMax, 0);
  stats[PROF_ACQ].calls = (uint32_t) InterlockedExchange(&acqCalls, 0);

  fprintf(logFile, "%.1f,%.1f,%.1f,%.1f,%u,%u,%.2f,%.2f", seconds, cpuPct, memory.PrivateUsage / 1048576.0,
          memory.WorkingSetSize / 1048576.0, frames, slowFrames, ticksMs(frameMax), ticksMs(stats[PROF_UI_LOOP].max));

  for (uint32_t s = 0; s < PROF_STAGE_COUNT; ++s)
  {
    fprintf(logFile, ",%.3f,%.3f,%u", ticksMs(stats[s].total), ticksMs(stats[s].max), stats[s].calls);
  }

  for (uint32_t c = 0; c < PROF_CONN_MAX; ++c)
  {
    fprintf(logFile, ",%llu,%llu,%llu", (unsigned long long) lines[c], (unsigned long long) seqGaps[c],
            (unsigned long long) seqLost[c]);
    linesTotal[c] += lines[c];
    lostTotal[c]  += seqLost[c];
    lines[c] = seqGaps[c] = seqLost[c] = 0;
  }

  fprintf(logFile, ",%llu,%llu,%llu,%llu\n", (unsigned long long) samplesPlotted, (unsigned long long) samplesRecorded,
          (unsigned long long) viewSamples, (unsigned long long) chartPoints);
  fflush(logFile);

  memset(stats, 0, sizeof(stats));
  samplesPlotted = samplesRecorded = viewSamples = chartPoints = 0;
  frames = slowFrames = 0;
  frameMax = 0;
}

static void summaryWrite(void)
{
  if (logFile == NULL)
  {
    return;
  }

  for (uint32_t c = 0; c < PROF_CONN_MAX; ++c)
  {
    linesTotal[c] += lines[c];
    lostTotal[c]  += seqLost[c];
    fprintf(logFile, "# summary c%u: lines=%llu seq_lost=%llu\n", c + 1, (unsigned long long) linesTotal[c],
            (unsigned long long) lostTotal[c]);
  }

  fprintf(logFile, "# summary ui_loop_max_ms=%.2f run_s=%.1f\n", ticksMs(handlerMaxEver),
          ticksMs(profNow() - appStart) / 1000.0);
  fclose(logFile);
  logFile = NULL;
}

static void exitTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);
  exit(0);
}

static void tabTimer(lv_timer_t * tim)
{
  lv_tabview_set_active(scenarioTabview, (uint32_t) (uintptr_t) lv_timer_get_user_data(tim), LV_ANIM_OFF);
}

// The LVGL thread held for ms (as a long picture save or graph render would)
static void stallTimer(lv_timer_t * tim)
{
  Sleep((DWORD) (uintptr_t) lv_timer_get_user_data(tim));
}

static void recordTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);
  compareTestStart("SIM", 0);
}

// ================================================================================================
//            PUBLIC FUNCTIONS
// ================================================================================================

uint64_t profNow(void)
{
  LARGE_INTEGER now;

  QueryPerformanceCounter(&now);
  return (uint64_t) now.QuadPart;
}

void profPush(ProfStage_t stage)
{
  uint64_t now = profNow();

  if (depth > 0)
  {
    stack[depth - 1].own += now - stack[depth - 1].start;
  }

  if (depth < PROF_STACK_MAX)
  {
    stack[depth++] = (ProfFrame_t) {stage, now, 0};
  }
}

void profPop(void)
{
  uint64_t now = profNow();

  if (depth == 0)
  {
    return;
  }

  ProfFrame_t * frame = &stack[--depth];

  statAdd(frame->stage, frame->own + (now - frame->start));

  if (depth > 0)
  {
    stack[depth - 1].start = now;
  }
}

void profAddThreadSafe(ProfStage_t stage, uint64_t start)
{
  LONG64 ticks = (LONG64) (profNow() - start);
  LONG64 seen  = acqMax;

  LV_UNUSED(stage);   // only PROF_ACQ runs outside the LVGL thread
  InterlockedAdd64(&acqTotal, ticks);
  InterlockedIncrement(&acqCalls);

  while ((ticks > seen) && (InterlockedCompareExchange64(&acqMax, ticks, seen) != seen))
  {
    seen = acqMax;
  }
}

void profLine(uint32_t conn, const float * values, uint32_t count)
{
  if (conn >= PROF_CONN_MAX)
  {
    return;
  }

  lines[conn]++;

  // Simulated lines: the first value counts 0, 1, 2... (modulo 1000000)
  if ((simPorts > 0) && (count > 0))
  {
    int64_t seq = (int64_t) llroundf(values[0]);

    if (seqLast[conn] >= 0)
    {
      int64_t step = (seq - seqLast[conn] + 1000000) % 1000000;

      // 0: the same line twice (not a loss)
      if (step != 1)
      {
        seqGaps[conn]++;
        seqLost[conn] += (step > 0) ? (uint64_t) (step - 1) : 0;
      }
    }

    seqLast[conn] = seq;
  }
}

void profSamplesPlotted(uint32_t count)
{
  samplesPlotted += count;
}

void profSamplesRecorded(uint32_t count)
{
  samplesRecorded += count;
}

void profChartPoints(uint32_t view, uint32_t points)
{
  viewSamples += view;
  chartPoints += points;
}

uint32_t profSimPorts(void)
{
  return simPorts;
}

uint32_t profSimRate(void)
{
  return simRate;
}

uint32_t profSimValues(void)
{
  return simValues;
}

void profInit(lv_display_t * display)
{
  char         path[MAX_PATH * 2];
  const char * logPath = getenv("SD_PROFILE_LOG");

  QueryPerformanceFrequency(&frequency);
  appStart    = profNow();
  periodStart = appStart;
  cpuLast     = cpuTicks();
  simPorts    = LV_MIN(envInt("SD_SIM_PORTS", 0), PROF_CONN_MAX);
  simRate     = LV_MAX(envInt("SD_SIM_RATE", 1000), 1);
  simValues   = LV_CLAMP(1, envInt("SD_SIM_VALUES", 8), 32);

  if ((logPath == NULL) || (logPath[0] == '\0'))
  {
    char temp[MAX_PATH];

    GetTempPathA(sizeof(temp), temp);
    snprintf(path, sizeof(path), "%sserial_dashboard_profile.csv", temp);
    logPath = path;
  }

  logFile = fopen(logPath, "w");

  if (logFile != NULL)
  {
    header();
  }

  lv_display_add_event_cb(display, displayEvent, LV_EVENT_REFR_START, NULL);
  lv_display_add_event_cb(display, displayEvent, LV_EVENT_RENDER_START, NULL);
  lv_display_add_event_cb(display, displayEvent, LV_EVENT_RENDER_READY, NULL);
  lv_display_add_event_cb(display, displayEvent, LV_EVENT_REFR_READY, NULL);
  atexit(summaryWrite);
}

void profScenarioStart(lv_obj_t * tabview)
{
  uint32_t seconds = envInt("SD_PROFILE_SECONDS", 0);
  uint32_t record  = envInt("SD_PROFILE_RECORD_AFTER", 0);
  uint32_t tab     = envInt("SD_PROFILE_TAB", UINT32_MAX);

  scenarioTabview = tabview;

  if (tab != UINT32_MAX)
  {
    // After the first refreshes, as a click would
    lv_timer_t * timer = lv_timer_create(tabTimer, 200, (void *) (uintptr_t) tab);
    lv_timer_set_repeat_count(timer, 1);
  }

  if (record > 0)
  {
    lv_timer_t * timer = lv_timer_create(recordTimer, record * 1000, NULL);
    lv_timer_set_repeat_count(timer, 1);
  }

  if (seconds > 0)
  {
    lv_timer_create(exitTimer, seconds * 1000, NULL);
  }

  // "<ms>,<every s>"
  const char * stall = getenv("SD_PROFILE_STALL");
  unsigned     stallMs = 0;
  unsigned     stallEvery = 0;

  if ((stall != NULL) && (sscanf(stall, "%u,%u", &stallMs, &stallEvery) == 2) && (stallMs > 0) && (stallEvery > 0))
  {
    lv_timer_create(stallTimer, stallEvery * 1000, (void *) (uintptr_t) stallMs);
  }
}

void profLoop(uint64_t handlerStart)
{
  uint64_t now    = profNow();
  uint64_t length = now - handlerStart;

  statAdd(PROF_UI_LOOP, length);
  handlerMaxEver = (length > handlerMaxEver) ? length : handlerMaxEver;

  if ((logFile != NULL) && (ticksMs(now - periodStart) >= PROF_PERIOD_MS))
  {
    rowWrite(ticksMs(now - appStart) / 1000.0);
    periodStart = profNow();
  }
}

#endif
