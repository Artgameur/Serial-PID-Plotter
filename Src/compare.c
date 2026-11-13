// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Compare tab: tests and the chart overlaying them.

   A test comes from a command flagged in the commands window (t = 0 is when it is sent), from the
   Record button, from the data of the plots, or from CSV files of the export. A recording has its
   own buffer (the plots keep only their last samples, and it goes on while they are paused) and
   ends when the condition is true, when the max test time has passed (it then did not get
   stable), or with the Stop button. With Auto stable, it also ends when the watched variable has
   stopped changing, even far from the constant (ex: the fridge cannot reach the setpoint): over
   the Auto stable time, the 3 highest and 3 lowest peaks of its average over the Smooth time are
   within the tolerance of each other. That average removes noise, but an oscillation slower than
   about twice Smooth stays: it keeps the test going. A chain goes on after it.

   Chain: a list of tests, each with its commands ("Kp:2 | Ki:0.5 | SP:5"): they are sent in order,
   the delay of the Between field apart (a device can miss commands sent at once), and the last
   one starts the test (t = 0). When a test ends by itself, the next one's commands are sent.
   Stopping a test by hand stops the chain.

   The Prepare commands bring the setup to the start point. They are sent before the first test
   (with a Start when condition), and after each row once any of its commands was sent, however
   its test ends: also when it is stopped, or when a command of the row fails. One that fails does
   not stop the others, but it stops the chain (the next test would not start from the start
   point).

   Start when: before each row, also the first one, the chain waits until this condition has been
   true for the Held for time without a break (ex: the fridge back above 20 before a test that
   takes it to 17). No test is recorded meanwhile: the samples go to waitData, for the condition.
   It has no time limit, the Stop button cancels it.

   Settled condition: the watched variable stays within the tolerance of the constant variable
   for the last N samples. The tolerance is in percent, and the "1 unit = % " field converts it to
   units: it is the same condition as the custom "held(abs(watch-ref)<=band,N)".

   Device settings: the request command is sent one Between delay after a test starts (its gains
   are set by then), always after the row's commands and before Prepare. For a second, the lines
   starting with the reply text are that test's settings:
   shown in the tests list, saved in its CSV, and in the legend for what the test's commands set
   (KP:10|SP:17 shows kP and SP, not kI and kD). They do not reach the plots.

   Each graph has its legend in a small box over its chart, a line per curve; drag it to move it.

   Up to 4 graphs, one above the other, each with its own variables, tests and zoom. Variables are
   matched between tests by their column name ("Name [unit]"). Each test has its color, each
   variable of a graph a shade of it. The constant variable is drawn once per graph, in gray.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "compare.h"

#include <math.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <windows.h>
#include <commdlg.h>

#include "app_window.h"
#include "lang.h"
#include "plot_export.h"
#include "plotter.h"
#include "settings.h"
#include "test_bench.h"
#include "test_data.h"
#include "ui_widgets.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define COMPARE_TESTS_MAX     16
#define COMPARE_CHAIN_MAX     COMPARE_TESTS_MAX   // commands of a chain, each making a test
#define COMPARE_CHAIN_TEXT_LEN 121        // 120 characters: its commands
#define COMPARE_CHAIN_DELAY_MAX 60000     // ms between commands
#define COMPARE_REPLY_MS      1000        // the device's settings are expected within this time
#define COMPARE_REPLY_LEN     41
#define COMPARE_TIME_TEXT_LEN 8           // "12:03:15" at the end of a test's default name
#define COMPARE_LEGEND_MARGIN uiPx(4)     // legend box from the chart's corner
#define COMPARE_CHAIN_KEY_LEN 16
#define COMPARE_CHAIN_CONN_MAX 4          // connections a command can be sent on
#define COMPARE_CHAIN_SEP     '\t'        // between connection number and command, when saved
#define COMPARE_CHAIN_CONN_WIDTH uiPx(56)
#define COMPARE_VARS_MAX      4           // variables of a graph
#define COMPARE_GRAPHS_MAX    4
#define COMPARE_GRAPHS_ALL    0x0F        // a new test is on every graph
#define COMPARE_GRAPH_BTN_W   uiPx(24)    // graph buttons of a test / variable
#define COMPARE_GRAPH_BTN_H   uiPx(26)
#define COMPARE_GRAPHS_KEY_LEN 16
#define COMPARE_NO_COLOR      UINT32_MAX
#define COMPARE_CURVES_MAX    ((COMPARE_TESTS_MAX * COMPARE_VARS_MAX) + 1)   // + the constant
#define COMPARE_NAMES_MAX     64
#define COMPARE_NAME_LIST_LEN (COMPARE_NAMES_MAX * TEST_HEADER_LEN)
#define COMPARE_CONDITION_LEN 161
#define COMPARE_SELECTED_SEP  '\t'        // between the selected variables, when saved
#define COMPARE_TIMER_MS      500         // recording shown / names checked this often
#define COMPARE_COLUMNS_MIN   100         // pixel columns of the curves
#define COMPARE_COLUMNS_MAX   2000
#define COMPARE_RANGE_MIN_S   0.01        // smallest X zoom
#define COMPARE_Y_RANGE_MIN   0.0005f     // smallest Y range
#define COMPARE_PANEL_WIDTH   38          // settings column, in percent of the tab
#define COMPARE_PANEL_MIN     uiPx(250)
#define COMPARE_NUMBER_WIDTH  uiPx(72)
#define COMPARE_COLOR_CONST   COLOR_INFO
#define COMPARE_SHADE_COUNT   COMPARE_VARS_MAX
#define COMPARE_CONST_MARGIN  0.001f      // the constant "differs" by more than this and the band


// Sizes that follow the size of a picture (pictureScale)
#define PICTURE_PX(px)        ((uiPx(px) * pictureScale) / 100)
#define AXIS_Y_WIDTH          PICTURE_PX(24)      // at least; more for longer values
#define AXIS_X_HEIGHT         PICTURE_PX(16)
#define GRID_Y_SPACING_PX     PICTURE_PX(24)
#define GRID_X_SPACING_PX     PICTURE_PX(60)
#define SEL_MIN_PX            uiPx(4)

#define STOP_SETTLED          0           // order of stopOptions
#define STOP_CUSTOM           1
#define NUMBER_NOT_SET        -1

#define OPEN_PATHS_LEN        32768       // file names of a multiple selection (UTF-16)
#define OPEN_POLL_MS          200
#define OPEN_NAME_LEN         (MAX_PATH * 3)

// The settled condition, in the custom conditions' language
#define CONDITION_SETTLED     "held(abs(watch-ref)<=band,N)"

_Static_assert(TEST_COLUMNS_MAX == PLOTTER_MAX_VARS, "a test holds all the variables");
_Static_assert(TEST_HEADER_LEN == PLOT_EXPORT_HEADER_LEN, "same column names");

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef enum
{
  END_NONE,                               // being recorded, or not recorded here
  END_CONDITION,
  END_STOPPED,
  END_FULL,
  END_NOT_STABLE,                         // max test time passed
  END_PLATEAU,                            // stopped changing (Auto stable)
} TestEnd_t;

typedef struct
{
  TestData_t * data;
  uint32_t     graphMask;                 // bit g: shown in graph g
  uint32_t     color;                     // 0xRRGGBB
  TestEnd_t    end;
  uint32_t     settledMs;                 // start of the N last samples (settled condition)
  lv_obj_t *   infoLabel;                 // on the test's row
  float        plateauValue;              // where it stopped changing (END_PLATEAU)
  bool         nameIsCommand;             // named after its command: not in the legend
} Entry_t;

// One test of a chain: its commands
typedef struct
{
  char     text[COMPARE_CHAIN_TEXT_LEN];
  uint32_t conn;                          // serial connection it is sent on (0 = first)
} ChainStep_t;

// One line of the chart
typedef struct
{
  const TestData_t * data;
  uint32_t           column;
  lv_color_t         color;
  bool               settings;            // first of its test in the graph: legend with its settings
  const char *       label;               // test in the legend (empty: none)
} Curve_t;

// One graph: legend, Y axis + chart, X axis, what it shows, its zoom
typedef struct
{
  lv_obj_t *          box;
  lv_obj_t *          chart;
  lv_obj_t *          yAxis;
  lv_obj_t *          xAxis;
  lv_obj_t *          hintLabel;
  lv_obj_t *          legend;             // box over the chart
  lv_obj_t *          legendLabels[COMPARE_CURVES_MAX];
  lv_chart_series_t * series[COMPARE_CURVES_MAX];
  Curve_t             curves[COMPARE_CURVES_MAX];
  uint32_t            curveCount;
  double              viewStart;          // seconds
  double              viewEnd;
  float               yMin;
  float               yMax;
  bool                zoomed;
  char                selected[COMPARE_VARS_MAX][TEST_HEADER_LEN];
  uint32_t            selectedCount;
} Graph_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static const uint32_t testColors[COMPARE_TESTS_MAX] =
{
  0x2563EB, 0xDC2626, 0x16A34A, 0xD97706, 0x9333EA, 0x0891B2, 0xDB2777, 0x65A30D,
  0x4B5563, 0x0284C7, 0xE11D48, 0xCA8A04, 0x4F46E5, 0x0D9488, 0xC026D3, 0x92400E,
};

// Option lists of the connection dropdown, by number of connections
static const char * chainConnOptions[COMPARE_CHAIN_CONN_MAX] = {"C1", "C1\nC2", "C1\nC2\nC3", "C1\nC2\nC3\nC4"};

// Graph buttons of a test / variable, by number of graphs
static const char * const graphMaps[COMPARE_GRAPHS_MAX][COMPARE_GRAPHS_MAX + 1] =
{
  {"1", ""}, {"1", "2", ""}, {"1", "2", "3", ""}, {"1", "2", "3", "4", ""},
};
static const char * graphCountOptions = "1\n2\n3\n4";

// Variable i of a graph: the test's color with this opacity over the chart
static const lv_opa_t shadeOpa[COMPARE_SHADE_COUNT] = {LV_OPA_COVER, LV_OPA_70, LV_OPA_50, LV_OPA_30};

static const char * stopOptions = "Settled\nCustom condition";

// Tests; recording: index of the one recorded, -1 = none
static Entry_t  entries[COMPARE_TESTS_MAX];
static uint32_t entryCount;
static int32_t  recording = -1;

// Recording: variable of each column, t = 0, the condition
static uint32_t   recordVars[TEST_COLUMNS_MAX];
static uint32_t   recordStartTick;
static TestPlateau_t plateau;
static TestExpr_t condition;
static bool       conditionValid;
static float      conditionBand;          // band in units
static uint32_t   conditionSamples;       // N

// Chain: the list edited, and the copy being run (chainIndex: its step, -1 = not running)
static ChainStep_t      chainSteps[COMPARE_CHAIN_MAX];
static uint32_t         chainStepCount;
static ChainStep_t      chainRun[COMPARE_CHAIN_MAX];
static uint32_t         chainRunCount;
static int32_t          chainIndex = -1;
static uint32_t         chainCommand;     // command of the step being sent
static lv_timer_t *     chainDelayTimer;  // running: waits before the next command
static bool             chainStepSent;    // a command of the step was sent: Prepare is due after it

// Waiting for the Start when condition before the current step (waitData not NULL)
static TestData_t *     waitData;
static TestExpr_t       waitCondition;
static uint32_t         waitStartTick;
static bool             waitHolding;      // true since waitHoldMs, without a break
static uint32_t         waitHoldMs;
static bool             waitNeedsStart;   // start is not known yet: the first sample's value
static float            chainStartValue;  // watched variable when the last test started

// Prepare commands being sent (kept here: the chain may be over), then the next step when goOn
static char             afterText[COMPARE_CHAIN_TEXT_LEN];
static char             afterFailed[COMPARE_CHAIN_TEXT_LEN];   // first one not sent
static uint32_t         afterConn;
static uint32_t         afterCommand;
static bool             afterGoOn;
static lv_timer_t *     afterTimer;       // running: waits before the next one
static uint32_t         chainConnCount = 1;
static CompareSendCb_t  chainSendCb;
static bool             chainAdvance;     // the test that ended lets the chain go on

// Device settings of the test being started
static TestData_t *     settingsTest;     // may have been deleted: see settingsEntry
static uint32_t         settingsConn;
static lv_timer_t *     settingsTimer;    // running: waits before sending the request
static bool             settingsCapturing;
static uint32_t         settingsTick;     // when the request was sent

// Variable names: of the variables received, of the tests, and the ones chosen
static char     names[COMPARE_NAMES_MAX][TEST_HEADER_LEN];
static uint32_t nameCount;
static char     nameOptions[COMPARE_NAME_LIST_LEN];
static char     constantName[TEST_HEADER_LEN];
static char     watchName[TEST_HEADER_LEN];

// Graphs
static Graph_t    graphs[COMPARE_GRAPHS_MAX];
static uint32_t   graphCount = 1;
static lv_obj_t * graphsBox;
static int32_t    pictureScale = 100;     // percent, while laid out for a picture
static bool       pictureActive;
static lv_obj_t * noteLabel;
static float      curveBuffer[2 * COMPARE_COLUMNS_MAX];
static bool       chartDirty;
static int32_t    selGraph = -1;          // graph of the zoom box being dragged
static lv_point_t selStart;
static lv_point_t selEnd;

// Settings column
static lv_obj_t *   recordBtnLabel;
static lv_obj_t *   statusLabel;
static lv_obj_t *   stopDropdown;
static lv_obj_t *   constantDropdown;
static lv_obj_t *   watchDropdown;
static lv_obj_t *   samplesInput;
static lv_obj_t *   toleranceInput;
static lv_obj_t *   conversionInput;
static lv_obj_t *   conditionRow;
static lv_obj_t *   conditionInput;
static lv_obj_t *   conditionHelp;
static lv_obj_t *   conditionNote;
static lv_obj_t *   testMaxInput;
static lv_obj_t *   plateauSwitch;
static lv_obj_t *   plateauTimeInput;
static lv_obj_t *   plateauSmoothInput;
static lv_obj_t *   chainRunBtnLabel;
static lv_obj_t *   chainNote;
static lv_obj_t *   settingsInput;
static lv_obj_t *   replyInput;
static lv_obj_t *   chainDelayInput;
static lv_obj_t *   prepareInput;
static lv_obj_t *   waitInput;
static lv_obj_t *   waitHoldInput;
static lv_obj_t *   chainList;
static lv_obj_t *   testsList;
static lv_obj_t *   openBtn;
static lv_obj_t *   varsList;
static lv_obj_t *   graphCountDropdown;
static lv_group_t * group;

// Open dialog, in its own thread (the UI and the recording keep running). Written by the thread,
// read by the LVGL thread once the state is DONE.
static volatile LONG openState = PLOT_EXPORT_IDLE;
static wchar_t       openPaths[OPEN_PATHS_LEN];
static HWND          openOwner;
static TestData_t *  openTests[COMPARE_TESTS_MAX];
static uint32_t      openTestCount;
static uint32_t      openFailures;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void settingsColumnCreate(lv_obj_t * parent);
static void chartColumnCreate(lv_obj_t * parent);
static void graphCreate(lv_obj_t * parent, uint32_t g);
static lv_obj_t * graphButtonsCreate(lv_obj_t * row, uint32_t mask, uint32_t color, lv_event_cb_t cb, void * index);
static bool graphButtonGet(lv_obj_t * buttons, uint32_t * g);
static lv_obj_t * numberCreate(lv_obj_t * row, const char * key, const char * fallback);

static uint32_t colorUnused(void);
static bool     entryAdd(TestData_t * data, TestEnd_t end);
static void     recordFinish(TestEnd_t end);
static uint32_t testMaxMs(void);
static uint32_t plateauMs(void);
static uint32_t plateauSmoothMs(void);
static TestExprError_t waitCompile(const char (*headers)[TEST_HEADER_LEN], uint32_t count, float start, uint32_t * errorAt);
static void     waitStart(void);
static void     waitSample(const float * row, uint32_t tick);
static void     waitDoneAsync(void * data);
static uint32_t waitHeldMs(void);
static void     chainLoad(void);
static void     chainSave(void);
static void     chainListRebuild(void);
static void     chainListRebuildAsync(void * data);
static void     chainNoteUpdate(void);
static void     chainSend(void);
static void     chainDelayTimerCb(lv_timer_t * tim);
static int32_t  settingsEntry(void);
static void     settingsRequest(uint32_t conn);
static void     settingsTimerCb(lv_timer_t * tim);
static void     settingsSend(void);
static const char * legendLabel(const Entry_t * entry);
static void     settingsCaptureEnd(void);
static uint32_t chainDelayMs(void);
static void     afterStart(bool goOn);
static void     afterSend(void);
static void     afterTimerCb(lv_timer_t * tim);
static void     chainNextAsync(void * data);
static void     recordEndAsync(void * data);
static void     chainFinish(const char * message);
static void     chainEndNotify(const char * message);
static void     conditionUpdate(void);
static void     plateauNoteAdd(void);
static void     statusUpdate(void);
static void     entryInfoUpdate(uint32_t i);
static void     testsListRebuild(void);
static void     testsListRebuildAsync(void * data);
static void     namesRefresh(bool force);
static void     nameAdd(char list[][TEST_HEADER_LEN], uint32_t * count, const char * name);
static void     dropdownNamesSet(lv_obj_t * dropdown, const char * name);
static void     varsListRebuild(void);
static void     selectedAdd(uint32_t g, const char * name);
static void     selectedSave(uint32_t g);
static void     selectedLoad(uint32_t g);
static bool     selectedHas(uint32_t g, const char * name);
static void     selectionDefault(void);
static int32_t  headerFind(const char (*headers)[TEST_HEADER_LEN], uint32_t count, const char * name);

static void    pictureBegin(void);
static void    pictureEnd(void);
static void    pictureStylesSet(const lv_font_t * font);
static void    chartRender(void);
static void    yAxesWidthUpdate(void);
static void    graphRender(Graph_t * graph, uint32_t g);
static void    curvesBuild(Graph_t * graph, uint32_t g);
static void    constantNoteUpdate(void);
static void    viewFit(Graph_t * graph);
static int32_t timeToX(const Graph_t * graph, const lv_area_t * area, double time);
static double  xToTime(const Graph_t * graph, const lv_area_t * area, int32_t x);
static int32_t valueToY(const Graph_t * graph, const lv_area_t * area, float value);
static float   yToValue(const Graph_t * graph, const lv_area_t * area, int32_t y);

static void gridDrawEvent(lv_event_t * e);
static void chartPostEvent(lv_event_t * e);
static void chartPressEvent(lv_event_t * e);
static void chartSizeEvent(lv_event_t * e);
static void chartDrawTaskEvent(lv_event_t * e);
static void legendDragEvent(lv_event_t * e);
static void yAxisDrawEvent(lv_event_t * e);
static void xAxisDrawEvent(lv_event_t * e);

static void refreshTimer(lv_timer_t * tim);
static void recordEvent(lv_event_t * e);
static void conditionEvent(lv_event_t * e);
static void constantEvent(lv_event_t * e);
static void watchEvent(lv_event_t * e);
static void varCheckEvent(lv_event_t * e);
static void testGraphEvent(lv_event_t * e);
static void graphCountEvent(lv_event_t * e);
static void screenshotEvent(lv_event_t * e);
static void testNameEvent(lv_event_t * e);
static void testSaveEvent(lv_event_t * e);
static void testDeleteEvent(lv_event_t * e);
static void captureEvent(lv_event_t * e);
static void zoomResetEvent(lv_event_t * e);
static void openEvent(lv_event_t * e);
static void openPollTimer(lv_timer_t * tim);
static bool chainWaitValid(void);
static void chainRunEvent(lv_event_t * e);
static void chainAddEvent(lv_event_t * e);
static void chainTextEvent(lv_event_t * e);
static void chainConnEvent(lv_event_t * e);
static void chainUpEvent(lv_event_t * e);
static void chainDeleteEvent(lv_event_t * e);

static unsigned __stdcall openThread(void * arg);
static void openFile(const wchar_t * path);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void compareInit(lv_obj_t * tab)
{
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(tab, uiPx(4), 0);
  lv_obj_set_style_pad_column(tab, uiPx(6), 0);
  lv_obj_set_scrollable(tab, false);

  group = lv_group_create();

  snprintf(constantName, sizeof(constantName), "%s", settingsGetString("compareConstant", ""));
  snprintf(watchName, sizeof(watchName), "%s", settingsGetString("compareWatch", ""));

  settingsColumnCreate(tab);
  chartColumnCreate(tab);

  for (uint32_t g = 0; g < COMPARE_GRAPHS_MAX; ++g)
  {
    selectedLoad(g);
  }

  chainLoad();
  chainListRebuild();
  chainNoteUpdate();

  namesRefresh(true);
  conditionUpdate();
  testsListRebuild();
  statusUpdate();
  chartRender();

  lv_timer_create(refreshTimer, COMPARE_TIMER_MS, NULL);
}

void compareSetSendCb(CompareSendCb_t cb)
{
  chainSendCb = cb;
}

// The commands of the chain show their connection with several
void compareSetConnCount(uint32_t count)
{
  chainConnCount = LV_CLAMP(1, count, COMPARE_CHAIN_CONN_MAX);
  chainListRebuild();
}

void compareTestStart(const char * command, uint32_t conn)
{
  char       headers[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  char       text[TEST_NAME_LEN + 48];
  uint32_t   count;

  // Started by hand during the wait of a chain: not the test it waits for
  if (waitData != NULL)
  {
    chainFinish(tr("Chain stopped."));
  }

  if (recording >= 0)
  {
    recordFinish(END_STOPPED);
  }

  if (entryCount >= COMPARE_TESTS_MAX)
  {
    plotterToast(tr("Test not recorded: 16 tests at most, delete one in the Compare tab."));
    return;
  }

  count = plotterRecordStart(recordVars, headers);

  if (count == 0)
  {
    plotterToast(tr("Test not recorded: no variable received yet."));
    return;
  }

  TestData_t * test = testDataCreate(count);

  if (test == NULL)
  {
    plotterToast(tr("Test not recorded: not enough memory."));
    return;
  }

  time_t    now   = time(NULL);
  struct tm local = *localtime(&now);

  // The user renames it with what changed (ex: "Kp 2")
  snprintf(test->name, TEST_NAME_LEN, "%.38s %02d:%02d:%02d", command, local.tm_hour, local.tm_min, local.tm_sec);
  memcpy(test->headers, headers, sizeof(headers[0]) * count);
  test->startMs = appWindowClockMs();
  snprintf(test->commands, TEST_COMMANDS_LEN, "%s", command);

  if (!entryAdd(test, END_NONE))
  {
    testDataFree(test);
    return;
  }

  recording       = (int32_t) (entryCount - 1);
  recordStartTick = lv_tick_get();

  entries[recording].nameIsCommand = true;
  memset(&plateau, 0, sizeof(plateau));

  conditionUpdate();
  namesRefresh(false);
  selectionDefault();
  testsListRebuild();
  statusUpdate();
  compareZoomReset();
  settingsRequest(conn);

  snprintf(text, sizeof(text), tr((conditionValid || (testMaxMs() > 0) || (plateauMs() > 0)) ? "Test started: %s" : "Test started, stop it in the Compare tab: %s"), test->name);
  plotterToast(text);
}

// The reply text is left out of the settings. Several lines are joined.
bool compareLineCapture(uint32_t conn, const char * line)
{
  if (!settingsCapturing || (conn != settingsConn))
  {
    return false;
  }

  if (lv_tick_elaps(settingsTick) >= COMPARE_REPLY_MS)
  {
    settingsCaptureEnd();
    return false;
  }

  const char * reply  = lv_textarea_get_text(replyInput);
  size_t       length = strlen(reply);
  int32_t      i      = settingsEntry();

  line += strspn(line, " \t");

  if ((i < 0) || (strncmp(line, reply, length) != 0))
  {
    return false;
  }

  TestData_t * test = entries[i].data;
  size_t       used = strlen(test->settings);

  line += length;
  line += strspn(line, " \t");

  snprintf(&test->settings[used], TEST_SETTINGS_LEN - used, "%s%s", (used > 0) ? ", " : "", line);
  entryInfoUpdate((uint32_t) i);
  chartDirty = true;
  return true;
}

bool compareRecording(void)
{
  return (recording >= 0) || (waitData != NULL);
}

void compareSampleAdd(const float * row, uint32_t tick)
{
  if (recording < 0)
  {
    waitSample(row, tick);
    return;
  }

  TestData_t * test = entries[recording].data;
  float        values[TEST_COLUMNS_MAX];
  int32_t      age = (int32_t) (tick - recordStartTick);

  // Received before the command was sent
  if (age < 0)
  {
    return;
  }

  // The curves need the times in order (several connections have their own reader)
  uint32_t last   = (test->sampleCount > 0) ? test->times[test->sampleCount - 1] : 0;
  uint32_t timeMs = LV_MAX((uint32_t) age, last);

  for (uint32_t c = 0; c < test->columnCount; ++c)
  {
    values[c] = row[recordVars[c]];
  }

  if (!testDataAppend(test, timeMs, values))
  {
    recordFinish(END_FULL);
    return;
  }

  chartDirty = true;

  int32_t watch = testDataColumnFind(test, watchName);

  // The value to come back to before the next test
  if ((watch >= 0) && (test->sampleCount == 1))
  {
    chainStartValue = values[watch];
  }

  if (conditionValid && testExprIsTrue(&condition, test))
  {
    recordFinish(END_CONDITION);
  }
  else if ((watch >= 0) && (plateauMs() > 0) && (conditionBand >= 0.0f) &&
           testPlateauUpdate(&plateau, values[watch], timeMs, conditionBand, plateauMs(), plateauSmoothMs()))
  {
    recordFinish(END_PLATEAU);
  }
}

void compareZoomReset(void)
{
  for (uint32_t g = 0; g < COMPARE_GRAPHS_MAX; ++g)
  {
    graphs[g].zoomed = false;
  }

  chartRender();
}

// Only the graph under the pointer
void compareRightClick(lv_point_t point)
{
  for (uint32_t g = 0; g < graphCount; ++g)
  {
    lv_area_t area;

    lv_obj_get_coords(graphs[g].box, &area);

    if ((point.x >= area.x1) && (point.x <= area.x2) && (point.y >= area.y1) && (point.y <= area.y2))
    {
      graphs[g].zoomed = false;
      chartRender();
    }
  }
}

lv_group_t * compareKeypadGroup(void)
{
  return group;
}

#ifdef SERIAL_DASHBOARD_CLI
_Static_assert((COMPARE_CLI_TESTS_MAX == COMPARE_TESTS_MAX) && (COMPARE_CLI_GRAPHS_MAX == COMPARE_GRAPHS_MAX)
               && (COMPARE_CLI_VARS_MAX == COMPARE_VARS_MAX), "same limits");

bool compareCliTestAdd(TestData_t * test, bool named)
{
  if (!entryAdd(test, END_NONE))
  {
    return false;
  }

  entries[entryCount - 1].nameIsCommand = !named && (test->settings[0] != '\0');
  return true;
}

void compareCliGraphCountSet(uint32_t count)
{
  lv_dropdown_set_selected(graphCountDropdown, count - 1);
  graphCountEvent(NULL);
}

void compareCliVarAdd(uint32_t g, const char * name)
{
  selectedAdd(g, name);
}

void compareCliConstantSet(const char * name)
{
  snprintf(constantName, sizeof(constantName), "%s", name);
  selectedAdd(0, constantName);
}

uint32_t compareCliTestCount(void)
{
  return entryCount;
}

void compareCliTestGet(uint32_t i, CompareCliTest_t * test)
{
  static const char * const ends[] = {"file", "condition", "stopped", "full", "not_stable", "plateau"};
  const Entry_t *           entry  = &entries[i];

  test->data         = entry->data;
  test->end          = ((int32_t) i == recording) ? "recording" : ends[entry->end];
  test->settledS     = entry->settledMs / 1000.0;
  test->plateauValue = entry->plateauValue;
}

int32_t compareCliRecordingIndex(void)
{
  return recording;
}

int32_t compareCliChainStep(uint32_t * count, bool * waiting)
{
  *count   = chainRunCount;
  *waiting = waitData != NULL;
  return chainIndex;
}

bool compareCliBusy(void)
{
  return (recording >= 0) || (chainIndex >= 0) || (afterTimer != NULL) || (chainDelayTimer != NULL) ||
         (settingsTimer != NULL) || settingsCapturing;
}

bool compareCliChainSet(const char * const * steps, uint32_t count, uint32_t conn)
{
  if ((chainIndex >= 0) || (count > COMPARE_CHAIN_MAX))
  {
    return false;
  }

  for (uint32_t i = 0; i < count; ++i)
  {
    snprintf(chainSteps[i].text, sizeof(chainSteps[i].text), "%s", steps[i]);
    chainSteps[i].conn = conn;
  }

  chainStepCount = count;
  chainSave();
  chainListRebuild();
  chainNoteUpdate();
  return true;
}

uint32_t compareCliChainGet(uint32_t i, const char ** text, uint32_t * conn)
{
  if (i < chainStepCount)
  {
    *text = chainSteps[i].text;
    *conn = chainSteps[i].conn;
  }

  return chainStepCount;
}

bool compareCliChainRun(void)
{
  if (chainIndex < 0)
  {
    chainRunEvent(NULL);
  }

  return chainIndex >= 0;
}

void compareCliChainStop(void)
{
  // Its Run button stops it
  if (chainIndex >= 0)
  {
    chainRunEvent(NULL);
  }
}

void compareCliTestStop(void)
{
  if (recording >= 0)
  {
    recordFinish(END_STOPPED);
  }
}

bool compareCliTestsClear(void)
{
  if (compareCliBusy())
  {
    return false;
  }

  for (uint32_t i = 0; i < entryCount; ++i)
  {
    testDataFree(entries[i].data);
  }

  entryCount = 0;
  lv_async_call(testsListRebuildAsync, NULL);
  namesRefresh(false);
  compareZoomReset();
  return true;
}

void compareCliStopVarsSet(const char * constant, const char * watch)
{
  if (constant != NULL)
  {
    snprintf(constantName, sizeof(constantName), "%s", constant);
    settingsSetString("compareConstant", constantName);
    selectedAdd(0, constantName);
    selectedSave(0);
  }

  if (watch != NULL)
  {
    snprintf(watchName, sizeof(watchName), "%s", watch);
    settingsSetString("compareWatch", watchName);
  }

  namesRefresh(true);
  conditionUpdate();
  chartDirty = true;
}

void compareCliStopVarsGet(const char ** constant, const char ** watch)
{
  *constant = constantName;
  *watch    = watchName;
}

void compareCliGraphsClear(void)
{
  for (uint32_t g = 0; g < COMPARE_GRAPHS_MAX; ++g)
  {
    graphs[g].selectedCount = 0;
    selectedSave(g);
  }
}

// The same steps as the Picture button
bool compareCliPictureSave(const char * path)
{
  namesRefresh(true);
  compareZoomReset();
  lv_obj_update_layout(lv_screen_active());
  chartRender();
  pictureBegin();

  bool saved = benchCliPictureWrite(graphsBox, path);

  pictureEnd();
  return saved;
}
#endif

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

static lv_obj_t * numberCreate(lv_obj_t * row, const char * key, const char * fallback)
{
  lv_obj_t * input = uiTextareaCreate(row, "");

  lv_obj_set_flex_grow(input, 0);
  lv_obj_set_width(input, COMPARE_NUMBER_WIDTH);
  lv_textarea_set_accepted_chars(input, "0123456789.");
  lv_textarea_set_max_length(input, 12);
  lv_textarea_set_text(input, fallback);
  settingsBindTextarea(input, key);
  lv_obj_add_event_cb(input, conditionEvent, LV_EVENT_VALUE_CHANGED, NULL);
  lv_group_add_obj(group, input);
  return input;
}

// Recording, tests and variables, scrolling when they do not fit
static void settingsColumnCreate(lv_obj_t * parent)
{
  lv_obj_t * header;
  lv_obj_t * box;
  lv_obj_t * row;
  lv_obj_t * column = lv_obj_create(parent);

  lv_obj_remove_style_all(column);
  lv_obj_set_size(column, LV_PCT(COMPARE_PANEL_WIDTH), LV_PCT(100));
  lv_obj_set_style_min_width(column, COMPARE_PANEL_MIN, 0);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(column, uiPx(6), 0);
  lv_obj_set_style_pad_right(column, uiPx(6), 0);
  lv_obj_set_scroll_dir(column, LV_DIR_VER);
  uiScrollbarShow(column);

  // Recording: state, then when it ends
  box = uiBoxCreate(column, tr("Recording"), &header);

  lv_obj_t * recordBtn = uiButtonCreate(header, LV_SYMBOL_VIDEO, recordEvent);
  recordBtnLabel = lv_obj_get_child(recordBtn, 0);
  uiTooltipSet(recordBtn, tr("Start a test now / stop the test. Commands with this symbol checked start one when sent."));

  statusLabel = lv_label_create(box);
  lv_obj_set_width(statusLabel, LV_PCT(100));
  lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(COLOR_INFO), 0);

  // Options set by namesRefresh
  row = uiSettingRowCreate(box, tr("Constant"));
  constantDropdown = uiDropdownCreate(row, "", 0);
  uiTooltipSet(constantDropdown, tr("Same in every test (ex: the setpoint): drawn once. Handle: ref"));
  lv_obj_add_event_cb(constantDropdown, constantEvent, LV_EVENT_VALUE_CHANGED, NULL);

  row = uiSettingRowCreate(box, tr("Watched"));
  watchDropdown = uiDropdownCreate(row, "", 0);
  uiTooltipSet(watchDropdown, tr("Variable that must reach the constant (ex: the temperature). Handle: watch"));
  lv_obj_add_event_cb(watchDropdown, watchEvent, LV_EVENT_VALUE_CHANGED, NULL);

  row = uiSettingRowCreate(box, tr("Stop when"));
  stopDropdown = uiDropdownCreate(row, tr(stopOptions), STOP_SETTLED);
  settingsBindDropdown(stopDropdown, "compareStop");
  lv_obj_add_event_cb(stopDropdown, conditionEvent, LV_EVENT_VALUE_CHANGED, NULL);

  row = uiSettingRowCreate(box, tr("Last samples"));
  samplesInput = numberCreate(row, "compareSamples", "50");
  uiTooltipSet(samplesInput, tr("How many samples in a row must be within the tolerance. Handle: N"));

  row = uiSettingRowCreate(box, tr("Within"));
  toleranceInput = numberCreate(row, "compareTolerance", "2");
  uiTooltipSet(toleranceInput, tr("Tolerance around the constant, in percent"));
  lv_label_set_text(lv_label_create(row), "%");

  row = uiSettingRowCreate(box, tr("Max test time"));
  testMaxInput = numberCreate(row, "compareStableMax", "0");
  uiTooltipSet(testMaxInput, tr("Longest time for a test, in seconds (0: no limit). It then ends as not stable, and a chain goes on."));
  lv_label_set_text(lv_label_create(row), "s");

  // Ends a test that stopped changing, wherever it is
  row = uiSettingRowCreate(box, tr("Auto stable"));
  plateauSwitch = lv_switch_create(row);
  settingsBindSwitch(plateauSwitch, "comparePlateau");
  lv_obj_add_event_cb(plateauSwitch, conditionEvent, LV_EVENT_VALUE_CHANGED, NULL);
  uiTooltipSet(plateauSwitch, tr("Also ends the test when the watched variable stops changing, even far from the constant. An oscillation keeps it going."));
  plateauTimeInput = numberCreate(row, "comparePlateauTime", "60");
  uiTooltipSet(plateauTimeInput, tr("Time over which the 3 highest and 3 lowest peaks must be within the tolerance. Use more than the slowest part of the response."));
  lv_label_set_text(lv_label_create(row), "s");

  row = uiSettingRowCreate(box, tr("Smooth"));
  plateauSmoothInput = numberCreate(row, "comparePlateauSmooth", "3");
  uiTooltipSet(plateauSmoothInput, tr("Averaged over this time for Auto stable: longer than the noise, less than half the period of the oscillations that must keep a test going"));
  lv_label_set_text(lv_label_create(row), "s");

  row = uiSettingRowCreate(box, tr("1 unit ="));
  conversionInput = numberCreate(row, "compareConversion", "1");
  uiTooltipSet(conversionInput, tr("Percent for 1 unit of the variable. Ex: 1 \xC2\xB0" "C = 2.5 %, then 5 % is 2 \xC2\xB0" "C. Handle: band"));
  lv_label_set_text(lv_label_create(row), "%");

  // Custom condition: true = the test ends
  conditionRow = lv_obj_create(box);
  lv_obj_remove_style_all(conditionRow);
  lv_obj_set_size(conditionRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(conditionRow, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(conditionRow, uiPx(4), 0);
  lv_obj_set_scrollable(conditionRow, false);

  conditionInput = uiTextareaCreate(conditionRow, CONDITION_SETTLED);
  lv_obj_set_flex_grow(conditionInput, 0);
  lv_obj_set_width(conditionInput, LV_PCT(100));
  lv_textarea_set_max_length(conditionInput, COMPARE_CONDITION_LEN - 1);
  settingsBindTextarea(conditionInput, "compareCondition");
  lv_obj_add_event_cb(conditionInput, conditionEvent, LV_EVENT_VALUE_CHANGED, NULL);
  lv_group_add_obj(group, conditionInput);

  conditionHelp = lv_label_create(conditionRow);
  lv_obj_set_width(conditionHelp, LV_PCT(100));
  lv_label_set_long_mode(conditionHelp, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(conditionHelp, lv_color_hex(COLOR_INFO), 0);
  lv_label_set_text(conditionHelp, tr("Variables: Temp, \"Var 1\". Handles: ref, watch, band, N, t (s), n.\n"
                                      "Functions: abs, min, max. Last k samples: held(cond, k), avg(x, k), "
                                      "lowest(x, k), highest(x, k), prev(x, k), slope(x, k).\n"
                                      "Ex: held(abs(watch-ref)<0.5,N) || t>600"));

  conditionNote = lv_label_create(box);
  lv_obj_set_width(conditionNote, LV_PCT(100));
  lv_label_set_long_mode(conditionNote, LV_LABEL_LONG_MODE_WRAP);

  // Asked to the device at each test, shown in the legend
  box = uiBoxCreate(column, tr("Device settings"), &header);

  row = uiSettingRowCreate(box, tr("Request"));
  settingsInput = uiTextareaCreate(row, "GET:PID");
  lv_textarea_set_max_length(settingsInput, COMPARE_CHAIN_TEXT_LEN - 1);
  settingsBindTextarea(settingsInput, "compareSettingsCmd");
  uiTooltipSet(settingsInput, tr("Command asking the device for its settings, sent when each test starts (empty: none)"));
  lv_group_add_obj(group, settingsInput);

  row = uiSettingRowCreate(box, tr("Reply starts"));
  replyInput = uiTextareaCreate(row, "CFG:");
  lv_textarea_set_max_length(replyInput, COMPARE_REPLY_LEN - 1);
  settingsBindTextarea(replyInput, "compareSettingsReply");
  uiTooltipSet(replyInput, tr("Text at the start of the device's reply: such lines are the settings of the test, not data. Needed to tell them from the data."));
  lv_group_add_obj(group, replyInput);

  // Chain: each command starts a test, the next one is sent when it ends
  box = uiBoxCreate(column, tr("Chain"), &header);

  lv_obj_t * chainRunBtn = uiButtonCreate(header, LV_SYMBOL_PLAY, chainRunEvent);
  chainRunBtnLabel = lv_obj_get_child(chainRunBtn, 0);
  uiTooltipSet(chainRunBtn, tr("Run the commands one after the other, each starting a test / stop"));
  uiTooltipSet(uiButtonCreate(header, LV_SYMBOL_PLUS, chainAddEvent), tr("Add a command"));

  chainNote = lv_label_create(box);
  lv_obj_set_width(chainNote, LV_PCT(100));
  lv_label_set_long_mode(chainNote, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(chainNote, lv_color_hex(COLOR_INFO), 0);

  row = uiSettingRowCreate(box, tr("Between"));
  chainDelayInput = numberCreate(row, "chainDelay", "100");
  uiTooltipSet(chainDelayInput, tr("Time between the commands of a test"));
  lv_label_set_text(lv_label_create(row), "ms");

  // Before each row: brings the setup to the start point, waits until it is there
  row = uiSettingRowCreate(box, tr("Prepare"));
  prepareInput = uiTextareaCreate(row, "SP:22.25");
  lv_textarea_set_max_length(prepareInput, COMPARE_CHAIN_TEXT_LEN - 1);
  settingsBindTextarea(prepareInput, "chainPrepare");
  uiTooltipSet(prepareInput, tr("Commands bringing the setup to the start point, separated by |. Sent after each test, however it ends, and before the first one (with a Start when condition)."));
  lv_group_add_obj(group, prepareInput);

  row = uiSettingRowCreate(box, tr("Start when"));
  waitInput = uiTextareaCreate(row, "watch>=20");
  lv_textarea_set_max_length(waitInput, COMPARE_CONDITION_LEN - 1);
  settingsBindTextarea(waitInput, "chainWait");
  uiTooltipSet(waitInput, tr("Each row waits for this condition (empty: no wait). Ex: watch>=20, or abs(watch-start)<1 to come back near the value when the last test started."));
  lv_group_add_obj(group, waitInput);

  row = uiSettingRowCreate(box, tr("Held for"));
  waitHoldInput = numberCreate(row, "chainWaitHold", "0");
  uiTooltipSet(waitHoldInput, tr("Seconds the Start when condition must stay true without a break"));
  lv_label_set_text(lv_label_create(row), "s");

  chainList = lv_obj_create(box);
  lv_obj_remove_style_all(chainList);
  lv_obj_set_size(chainList, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chainList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(chainList, uiPx(4), 0);
  lv_obj_set_scrollable(chainList, false);

  benchBoxCreate(column, group);

  // Tests: where they come from, then one row each
  box = uiBoxCreate(column, tr("Tests"), &header);

  uiTooltipSet(uiButtonCreate(header, LV_SYMBOL_DOWNLOAD, captureEvent), tr("Add the data of the plots as a test"));
  openBtn = uiButtonCreate(header, LV_SYMBOL_DIRECTORY, openEvent);
  uiTooltipSet(openBtn, tr("Open tests from CSV files of the export"));

  testsList = lv_obj_create(box);
  lv_obj_remove_style_all(testsList);
  lv_obj_set_size(testsList, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(testsList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(testsList, uiPx(6), 0);
  lv_obj_set_scrollable(testsList, false);

  // Graphs, and the variables each one shows
  box = uiBoxCreate(column, tr("Compared"), &header);

  graphCountDropdown = uiDropdownCreate(header, graphCountOptions, 0);
  uiTooltipSet(graphCountDropdown, tr("Number of graphs. The buttons of a test or a variable choose its graphs."));
  settingsBindDropdown(graphCountDropdown, "compareGraphs");
  graphCount = lv_dropdown_get_selected(graphCountDropdown) + 1;
  lv_obj_add_event_cb(graphCountDropdown, graphCountEvent, LV_EVENT_VALUE_CHANGED, NULL);

  varsList = lv_obj_create(box);
  lv_obj_remove_style_all(varsList);
  lv_obj_set_size(varsList, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(varsList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(varsList, uiPx(4), 0);
  lv_obj_set_scrollable(varsList, false);
}

// Picture and zoom buttons, the graphs one above the other, the note about them
static void chartColumnCreate(lv_obj_t * parent)
{
  lv_obj_t * column = lv_obj_create(parent);

  lv_obj_remove_style_all(column);
  lv_obj_set_height(column, LV_PCT(100));
  lv_obj_set_flex_grow(column, 1);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(column, uiPx(2), 0);
  lv_obj_set_scrollable(column, false);

  lv_obj_t * top = uiRowCreate(column);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  uiTooltipSet(uiButtonCreate(top, LV_SYMBOL_IMAGE, screenshotEvent), tr("Save a picture of the graphs in the test bench folder"));
  uiTooltipSet(uiButtonCreate(top, LV_SYMBOL_HOME, zoomResetEvent),
               tr("Reset the zoom of all graphs (right-click a graph: that graph only). Drag on a graph to zoom."));

  // What the pictures show
  graphsBox = lv_obj_create(column);
  lv_obj_remove_style_all(graphsBox);
  lv_obj_set_width(graphsBox, LV_PCT(100));
  lv_obj_set_flex_grow(graphsBox, 1);
  lv_obj_set_flex_flow(graphsBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(graphsBox, uiPx(4), 0);
  lv_obj_set_scrollable(graphsBox, false);

  for (uint32_t g = 0; g < COMPARE_GRAPHS_MAX; ++g)
  {
    graphCreate(graphsBox, g);
  }

  noteLabel = lv_label_create(graphsBox);
  lv_obj_set_width(noteLabel, LV_PCT(100));
  lv_label_set_long_mode(noteLabel, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(noteLabel, lv_color_hex(COLOR_ERROR), 0);
  lv_obj_set_hidden(noteLabel, true);
}

// Y axis + chart with its legend box, X axis
static void graphCreate(lv_obj_t * parent, uint32_t g)
{
  Graph_t * graph    = &graphs[g];
  void *    userData = (void *) (uintptr_t) g;

  graph->viewEnd = 1.0;
  graph->yMin    = -1.0f;
  graph->yMax    = 1.0f;

  graph->box = lv_obj_create(parent);
  lv_obj_remove_style_all(graph->box);
  lv_obj_set_width(graph->box, LV_PCT(100));
  lv_obj_set_flex_grow(graph->box, 1);
  lv_obj_set_flex_flow(graph->box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(graph->box, uiPx(2), 0);
  lv_obj_set_scrollable(graph->box, false);
  lv_obj_set_hidden(graph->box, g >= graphCount);

  lv_obj_t * row = uiRowCreate(graph->box);
  lv_obj_set_flex_grow(row, 1);
  lv_obj_set_style_pad_column(row, uiPx(2), 0);

  graph->yAxis = lv_obj_create(row);
  lv_obj_remove_style_all(graph->yAxis);
  lv_obj_set_size(graph->yAxis, AXIS_Y_WIDTH, LV_PCT(100));
  lv_obj_set_scrollable(graph->yAxis, false);
  lv_obj_add_event_cb(graph->yAxis, yAxisDrawEvent, LV_EVENT_DRAW_MAIN, userData);

  // The background is drawn with the grid, below the lines (as in the plots)
  lv_obj_t * chart = lv_chart_create(row);

  graph->chart = chart;
  lv_obj_set_height(chart, LV_PCT(100));
  lv_obj_set_flex_grow(chart, 1);
  lv_obj_set_style_pad_all(chart, uiPx(4), 0);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(chart, LV_MAX(1, PICTURE_PX(2)), LV_PART_ITEMS);
  lv_obj_set_scrollable(chart, false);
  lv_obj_set_clickable(chart, true);
  lv_obj_set_send_draw_task_events(chart, true);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
  lv_chart_set_point_count(chart, 2 * COMPARE_COLUMNS_MIN);
  lv_chart_set_div_line_count(chart, 0, 0);
  lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, UI_CHART_RES);

  lv_obj_add_event_cb(chart, gridDrawEvent, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, userData);
  lv_obj_add_event_cb(chart, chartPostEvent, LV_EVENT_DRAW_POST, userData);
  lv_obj_add_event_cb(chart, chartPressEvent, LV_EVENT_PRESSED, userData);
  lv_obj_add_event_cb(chart, chartPressEvent, LV_EVENT_PRESSING, userData);
  lv_obj_add_event_cb(chart, chartPressEvent, LV_EVENT_RELEASED, userData);
  lv_obj_add_event_cb(chart, chartPressEvent, LV_EVENT_PRESS_LOST, userData);
  lv_obj_add_event_cb(chart, chartSizeEvent, LV_EVENT_SIZE_CHANGED, userData);
  lv_obj_add_event_cb(chart, chartDrawTaskEvent, LV_EVENT_DRAW_TASK_ADDED, NULL);

  for (uint32_t c = 0; c < COMPARE_CURVES_MAX; ++c)
  {
    graph->series[c] = lv_chart_add_series(chart, lv_color_hex(COMPARE_COLOR_CONST), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(chart, graph->series[c], LV_CHART_POINT_NONE);
    lv_chart_hide_series(chart, graph->series[c], true);
  }

  graph->hintLabel = lv_label_create(chart);
  lv_obj_set_width(graph->hintLabel, LV_PCT(100));
  lv_label_set_long_mode(graph->hintLabel, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_center(graph->hintLabel);
  lv_obj_set_style_text_color(graph->hintLabel, lv_color_hex(COLOR_INFO), 0);
  lv_obj_set_style_text_align(graph->hintLabel, LV_TEXT_ALIGN_CENTER, 0);

  // Small window over the chart: a line per curve, moved by dragging it (the lines are not clickable)
  graph->legend = lv_obj_create(chart);
  lv_obj_set_size(graph->legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(graph->legend, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(graph->legend, uiPx(4), 0);
  lv_obj_set_style_pad_row(graph->legend, uiPx(2), 0);
  lv_obj_set_style_radius(graph->legend, uiPx(4), 0);
  lv_obj_set_style_bg_opa(graph->legend, LV_OPA_80, 0);
  lv_obj_set_style_border_width(graph->legend, LV_MAX(1, uiPx(1)), 0);
  lv_obj_set_style_border_color(graph->legend, lv_color_hex(COLOR_INFO), 0);
  lv_obj_set_style_border_opa(graph->legend, LV_OPA_50, 0);
  lv_obj_set_scrollable(graph->legend, false);
  lv_obj_align(graph->legend, LV_ALIGN_TOP_RIGHT, -COMPARE_LEGEND_MARGIN, COMPARE_LEGEND_MARGIN);
  lv_obj_set_hidden(graph->legend, true);
  lv_obj_add_event_cb(graph->legend, legendDragEvent, LV_EVENT_PRESSING, NULL);
  uiTooltipSet(graph->legend, tr("Drag to move"));

  for (uint32_t c = 0; c < COMPARE_CURVES_MAX; ++c)
  {
    graph->legendLabels[c] = lv_label_create(graph->legend);
    uiLegendDotAdd(graph->legendLabels[c]);
    lv_obj_set_hidden(graph->legendLabels[c], true);
  }

  graph->xAxis = lv_obj_create(graph->box);
  lv_obj_remove_style_all(graph->xAxis);
  lv_obj_set_size(graph->xAxis, LV_PCT(100), AXIS_X_HEIGHT);
  lv_obj_set_scrollable(graph->xAxis, false);
  lv_obj_add_event_cb(graph->xAxis, xAxisDrawEvent, LV_EVENT_DRAW_MAIN, userData);
}

// One checkable button per graph; color: of the checked ones (COMPARE_NO_COLOR: the theme's)
static lv_obj_t * graphButtonsCreate(lv_obj_t * row, uint32_t mask, uint32_t color, lv_event_cb_t cb, void * index)
{
  lv_obj_t * buttons = lv_buttonmatrix_create(row);

  lv_buttonmatrix_set_map(buttons, graphMaps[graphCount - 1]);
  // CLICK_TRIG: VALUE_CHANGED after the checked state is toggled
  lv_buttonmatrix_set_button_ctrl_all(buttons, LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CLICK_TRIG);

  for (uint32_t g = 0; g < graphCount; ++g)
  {
    if ((mask & (1u << g)) != 0)
    {
      lv_buttonmatrix_set_button_ctrl(buttons, g, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
  }

  lv_obj_set_size(buttons, COMPARE_GRAPH_BTN_W * (int32_t) graphCount, COMPARE_GRAPH_BTN_H);
  lv_obj_set_style_pad_all(buttons, 0, 0);
  lv_obj_set_style_pad_gap(buttons, uiPx(2), 0);
  lv_obj_set_style_border_width(buttons, 0, 0);
  lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, 0);

  if (color != COMPARE_NO_COLOR)
  {
    lv_obj_set_style_bg_color(buttons, lv_color_hex(color), LV_PART_ITEMS | LV_STATE_CHECKED);
  }

  lv_obj_set_user_data(buttons, index);
  lv_obj_add_event_cb(buttons, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return buttons;
}

// Graph of the button just clicked; returns whether it is now checked
static bool graphButtonGet(lv_obj_t * buttons, uint32_t * g)
{
  *g = lv_buttonmatrix_get_selected_button(buttons);
  return lv_buttonmatrix_has_button_ctrl(buttons, *g, LV_BUTTONMATRIX_CTRL_CHECKED);
}

// First color no test has
static uint32_t colorUnused(void)
{
  for (uint32_t c = 0; c < COMPARE_TESTS_MAX; ++c)
  {
    bool used = false;

    for (uint32_t i = 0; (i < entryCount) && !used; ++i)
    {
      used = (entries[i].color == testColors[c]);
    }

    if (!used)
    {
      return testColors[c];
    }
  }

  return testColors[0];
}

// false: all the tests in use (data stays the caller's)
static bool entryAdd(TestData_t * data, TestEnd_t end)
{
  if (entryCount >= COMPARE_TESTS_MAX)
  {
    return false;
  }

  entries[entryCount] = (Entry_t) {data, COMPARE_GRAPHS_ALL, colorUnused(), end, 0, NULL, 0.0f, false};
  entryCount++;
  chartDirty = true;
  return true;
}

static void recordFinish(TestEnd_t end)
{
  Entry_t *    entry = &entries[recording];
  TestData_t * test  = entry->data;
  char         text[TEST_NAME_LEN + 64];
  double       seconds = (test->sampleCount > 0) ? (test->times[test->sampleCount - 1] / 1000.0) : 0.0;

  // Settled: since the first of the N last samples
  entry->settledMs = seconds > 0.0 ? test->times[test->sampleCount - 1] : 0;

  if ((end == END_CONDITION) && (lv_dropdown_get_selected(stopDropdown) == STOP_SETTLED) &&
      (test->sampleCount >= conditionSamples))
  {
    entry->settledMs = test->times[test->sampleCount - conditionSamples];
  }

  if (end == END_PLATEAU)
  {
    entry->settledMs    = plateau.startMs;
    entry->plateauValue = plateau.value;
  }

  entry->end = end;
  recording  = -1;
  chartDirty = true;

  entryInfoUpdate((uint32_t) (entry - entries));
  statusUpdate();
  conditionUpdate();

  if (end == END_CONDITION)
  {
    snprintf(text, sizeof(text), tr("Test ended (condition met) after %.1f s: %s"), seconds, test->name);
  }
  else if (end == END_FULL)
  {
    snprintf(text, sizeof(text), tr("Test ended, its memory is full, after %.1f s: %s"), seconds, test->name);
  }
  else if (end == END_NOT_STABLE)
  {
    snprintf(text, sizeof(text), tr("Test not stable after %.1f s: %s"), seconds, test->name);
  }
  else if (end == END_PLATEAU)
  {
    snprintf(text, sizeof(text), tr("Test stopped changing at %.2f after %.1f s: %s"), (double) entry->plateauValue, seconds, test->name);
  }
  else
  {
    snprintf(text, sizeof(text), tr("Test stopped after %.1f s: %s"), seconds, test->name);
  }

  plotterToast(text);

  chainAdvance = (chainIndex >= 0) && ((end == END_CONDITION) || (end == END_NOT_STABLE) || (end == END_PLATEAU));

  // In a chain, once when all of it ends; stopped by hand, the person is there
  if ((end != END_STOPPED) && !chainAdvance)
  {
    benchNotify(text);
  }

  if ((chainIndex >= 0) && !chainAdvance)
  {
    chainFinish(tr("Chain stopped."));
  }

  // After this event (a recording ends while a line is being received)
  lv_async_call(recordEndAsync, test);
}

// A test ended: saved by the test bench (the picture has this test), then the chain's next command
static void recordEndAsync(void * data)
{
  for (uint32_t i = 0; i < entryCount; ++i)
  {
    if ((entries[i].data == data) && benchAutoSave())
    {
      chartRender();
      pictureBegin();
      benchTestSave(entries[i].data, graphsBox);
      pictureEnd();
    }
  }

  if (chainAdvance)
  {
    chainAdvance = false;
    afterStart(true);
  }
}

// Entry of the test the settings are for; -1 once deleted
static int32_t settingsEntry(void)
{
  for (uint32_t i = 0; i < entryCount; ++i)
  {
    if (entries[i].data == settingsTest)
    {
      return (int32_t) i;
    }
  }

  return -1;
}

// The test just started: its settings are requested after the delay of the commands
static void settingsRequest(uint32_t conn)
{
  if (settingsTimer != NULL)
  {
    lv_timer_delete(settingsTimer);
    settingsTimer = NULL;
  }

  settingsCapturing = false;

  if ((lv_textarea_get_text(settingsInput)[0] != '\0') && (lv_textarea_get_text(replyInput)[0] != '\0'))
  {
    settingsTest  = entries[recording].data;
    settingsConn  = conn;
    settingsTimer = lv_timer_create(settingsTimerCb, chainDelayMs(), NULL);
    lv_timer_set_repeat_count(settingsTimer, 1);
  }
}

// Deleted by LVGL after this call
static void settingsTimerCb(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  settingsTimer = NULL;
  settingsSend();
}

static void settingsSend(void)
{
  if (settingsEntry() < 0)
  {
    return;
  }

  if ((chainSendCb == NULL) || !chainSendCb(settingsConn, lv_textarea_get_text(settingsInput), COLOR_INFO))
  {
    plotterToast(tr("Could not send the settings request."));
    return;
  }

  settingsTick      = lv_tick_get();
  settingsCapturing = true;
}

// The time for the reply is over
static void settingsCaptureEnd(void)
{
  int32_t i = settingsEntry();

  settingsCapturing = false;

  if ((i >= 0) && (entries[i].data->settings[0] == '\0'))
  {
    plotterToast(tr("No reply to the settings request: check the Reply starts text."));
  }
}

// Time for a variable to count as stopped changing; 0: Auto stable is off
static uint32_t plateauMs(void)
{
  float seconds = strtof(lv_textarea_get_text(plateauTimeInput), NULL);

  if (!lv_obj_has_state(plateauSwitch, LV_STATE_CHECKED) || !(seconds > 0.0f))
  {
    return 0;
  }

  return (uint32_t) lroundf(fminf(seconds, (float) (INT32_MAX / 1000)) * 1000.0f);
}

// Averaging of Auto stable; 0: as long as it can be (3 averages)
static uint32_t plateauSmoothMs(void)
{
  float seconds = strtof(lv_textarea_get_text(plateauSmoothInput), NULL);

  return (seconds > 0.0f) ? (uint32_t) lroundf(fminf(seconds, (float) (INT32_MAX / 1000)) * 1000.0f) : 0;
}

// Longest time for a test; 0: no limit
static uint32_t testMaxMs(void)
{
  float seconds = strtof(lv_textarea_get_text(testMaxInput), NULL);

  return (seconds > 0.0f) ? (uint32_t) lroundf(fminf(seconds, (float) (INT32_MAX / 1000)) * 1000.0f) : 0;
}

// Compiles Start when with the settings of the recording; start: its handle
static TestExprError_t waitCompile(const char (*headers)[TEST_HEADER_LEN], uint32_t count, float start, uint32_t * errorAt)
{
  TestExprEnv_t env;

  env.headers     = headers;
  env.columnCount = count;
  env.refColumn   = headerFind(headers, count, constantName);
  env.watchColumn = headerFind(headers, count, watchName);
  env.band        = conditionBand;
  env.samples     = conditionSamples;
  env.start       = start;

  return testExprCompile(&waitCondition, lv_textarea_get_text(waitInput), &env, errorAt);
}

// Before the current step: its commands are sent now without a condition, otherwise when it is met
static void waitStart(void)
{
  static char headers[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  uint32_t    errorAt;

  if (lv_textarea_get_text(waitInput)[0] == '\0')
  {
    chainSend();
    return;
  }

  uint32_t count = plotterRecordStart(recordVars, headers);

  waitData = (count > 0) ? testDataCreate(count) : NULL;

  if (waitData == NULL)
  {
    chainEndNotify(tr("Chain stopped: no variable received yet."));
    return;
  }

  memcpy(waitData->headers, headers, sizeof(headers[0]) * count);
  waitStartTick  = lv_tick_get();
  waitHolding    = false;
  waitNeedsStart = isnan(chainStartValue);

  if (waitCompile((const char (*)[TEST_HEADER_LEN]) headers, count, chainStartValue, &errorAt) != TEST_EXPR_OK)
  {
    chainEndNotify(tr("Chain stopped: the Start when condition is not valid."));
    return;
  }

  chainNoteUpdate();

  // The first test: the later ones had them after the previous test
  if (chainIndex == 0)
  {
    afterStart(false);
  }
}

static uint32_t waitHeldMs(void)
{
  float seconds = strtof(lv_textarea_get_text(waitHoldInput), NULL);

  return (seconds > 0.0f) ? (uint32_t) lroundf(fminf(seconds, (float) (INT32_MAX / 1000)) * 1000.0f) : 0;
}

// New sample during the wait
static void waitSample(const float * row, uint32_t tick)
{
  float   values[TEST_COLUMNS_MAX];
  int32_t age = (int32_t) (tick - waitStartTick);

  if ((waitData == NULL) || (age < 0))
  {
    return;
  }

  for (uint32_t c = 0; c < waitData->columnCount; ++c)
  {
    values[c] = row[recordVars[c]];
  }

  // A long wait: only the last samples matter to the condition
  if (waitData->sampleCount >= TEST_SAMPLES_MAX)
  {
    waitData->sampleCount = 0;
  }

  uint32_t last = (waitData->sampleCount > 0) ? waitData->times[waitData->sampleCount - 1] : 0;
  uint32_t timeMs = LV_MAX((uint32_t) age, last);

  if (!testDataAppend(waitData, timeMs, values))
  {
    return;
  }

  // First row of a chain: no test yet, the wait starts from the current value
  int32_t watch = waitNeedsStart ? testDataColumnFind(waitData, watchName) : -1;

  if ((watch >= 0) && !isnan(values[watch]))
  {
    uint32_t errorAt;

    chainStartValue = values[watch];
    waitNeedsStart  = false;
    waitCompile((const char (*)[TEST_HEADER_LEN]) waitData->headers, waitData->columnCount, chainStartValue, &errorAt);
  }

  // The Prepare commands are all sent before the condition counts
  if ((afterTimer != NULL) || !testExprIsTrue(&waitCondition, waitData))
  {
    waitHolding = false;
    return;
  }

  if (!waitHolding)
  {
    waitHolding = true;
    waitHoldMs  = timeMs;
  }

  if ((timeMs - waitHoldMs) >= waitHeldMs())
  {
    testDataFree(waitData);
    waitData = NULL;

    // After this event (a sample is being received)
    lv_async_call(waitDoneAsync, NULL);
  }
}

static void waitDoneAsync(void * data)
{
  LV_UNUSED(data);

  // Stopped or replaced by a test meanwhile
  if ((chainIndex >= 0) && (recording < 0) && (waitData == NULL))
  {
    chainSend();
  }
}

// Keys "chainCount", "chain1" ... = "connection number<TAB>commands"
static void chainLoad(void)
{
  char key[COMPARE_CHAIN_KEY_LEN];

  chainStepCount = (uint32_t) LV_CLAMP(0, settingsGetInt("chainCount", 0), COMPARE_CHAIN_MAX);

  for (uint32_t i = 0; i < chainStepCount; ++i)
  {
    snprintf(key, sizeof(key), "chain%lu", (unsigned long) (i + 1));

    const char * text = settingsGetString(key, "");
    const char * sep  = strchr(text, COMPARE_CHAIN_SEP);

    chainSteps[i].conn = (uint32_t) LV_CLAMP(1, atoi(text), COMPARE_CHAIN_CONN_MAX) - 1;
    snprintf(chainSteps[i].text, COMPARE_CHAIN_TEXT_LEN, "%s", (sep != NULL) ? (sep + 1) : "");
  }
}

// The entries above the count are left in the file but ignored
static void chainSave(void)
{
  char key[COMPARE_CHAIN_KEY_LEN];
  char text[COMPARE_CHAIN_TEXT_LEN + 8];

  settingsSetInt("chainCount", (int32_t) chainStepCount);

  for (uint32_t i = 0; i < chainStepCount; ++i)
  {
    snprintf(key, sizeof(key), "chain%lu", (unsigned long) (i + 1));
    snprintf(text, sizeof(text), "%lu%c%s", (unsigned long) (chainSteps[i].conn + 1), COMPARE_CHAIN_SEP, chainSteps[i].text);
    settingsSetString(key, text);
  }
}

// [number] [commands] [connection with several] [move up] [delete] for each
static void chainListRebuild(void)
{
  lv_obj_clean(chainList);

  for (uint32_t i = 0; i < chainStepCount; ++i)
  {
    void *     index = (void *) (uintptr_t) i;
    lv_obj_t * row   = uiRowCreate(chainList);

    lv_label_set_text_fmt(lv_label_create(row), "%lu", (unsigned long) (i + 1));

    lv_obj_t * text = uiTextareaCreate(row, tr("Command"));
    lv_textarea_set_max_length(text, COMPARE_CHAIN_TEXT_LEN - 1);
    lv_textarea_set_text(text, chainSteps[i].text);
    lv_obj_set_user_data(text, index);
    lv_obj_add_event_cb(text, chainTextEvent, LV_EVENT_VALUE_CHANGED, NULL);
    uiTooltipSet(text, tr("Commands of this test, separated by | (ex: Kp:2 | SP:5). The last one starts the test."));
    lv_group_add_obj(group, text);

    if (chainConnCount > 1)
    {
      lv_obj_t * conn = lv_dropdown_create(row);

      lv_obj_add_style(conn, uiStyleCompact(), 0);
      lv_obj_set_width(conn, COMPARE_CHAIN_CONN_WIDTH);
      lv_dropdown_set_options_static(conn, chainConnOptions[chainConnCount - 1]);
      lv_dropdown_set_selected(conn, LV_MIN(chainSteps[i].conn, chainConnCount - 1));
      lv_obj_set_user_data(conn, index);
      lv_obj_add_event_cb(conn, chainConnEvent, LV_EVENT_VALUE_CHANGED, NULL);
      uiTooltipSet(conn, tr("Connection the command is sent on"));
    }

    lv_obj_t * upBtn = uiButtonCreate(row, LV_SYMBOL_UP, chainUpEvent);
    lv_obj_set_user_data(upBtn, index);
    lv_obj_set_state(upBtn, LV_STATE_DISABLED, i == 0);
    uiTooltipSet(upBtn, tr("Move up"));

    lv_obj_t * deleteBtn = uiButtonCreate(row, LV_SYMBOL_CLOSE, chainDeleteEvent);
    lv_obj_set_user_data(deleteBtn, index);
    uiTooltipSet(deleteBtn, tr("Delete"));
  }
}

static void chainListRebuildAsync(void * data)
{
  LV_UNUSED(data);
  chainListRebuild();
}

static void chainNoteUpdate(void)
{
  if ((chainIndex >= 0) && (waitData != NULL))
  {
    char     text[COMPARE_CONDITION_LEN + 64];
    uint32_t last = (waitData->sampleCount > 0) ? waitData->times[waitData->sampleCount - 1] : 0;

    snprintf(text, sizeof(text), tr("Waiting before test %lu of %lu: %s, true for %.1f of %.1f s"),
             (unsigned long) (chainIndex + 1), (unsigned long) chainRunCount, lv_textarea_get_text(waitInput),
             waitHolding ? ((last - waitHoldMs) / 1000.0) : 0.0, waitHeldMs() / 1000.0);
    lv_label_set_text(chainNote, text);
    lv_obj_set_style_text_color(chainNote, lv_color_hex(COLOR_ERROR), 0);
  }
  else if (chainIndex >= 0)
  {
    char text[COMPARE_CHAIN_TEXT_LEN + 48];

    snprintf(text, sizeof(text), tr("Running: test %lu of %lu, %s"), (unsigned long) (chainIndex + 1),
             (unsigned long) chainRunCount, chainRun[chainIndex].text);
    lv_label_set_text(chainNote, text);
    lv_obj_set_style_text_color(chainNote, lv_color_hex(COLOR_ERROR), 0);
  }
  else
  {
    lv_label_set_text(chainNote, tr("Each row is a test, its commands separated by |. The next row is sent when it ends."));
    lv_obj_set_style_text_color(chainNote, lv_color_hex(COLOR_INFO), 0);
  }

  lv_label_set_text(chainRunBtnLabel, (chainIndex >= 0) ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
}

// Sends the current step's next command; its last one starts the test. A command that is not sent
// stops the chain: the test would not be the one asked for.
static void chainSend(void)
{
  const ChainStep_t * step = &chainRun[chainIndex];
  char                command[COMPARE_CHAIN_TEXT_LEN];
  char                next[COMPARE_CHAIN_TEXT_LEN];
  char                text[COMPARE_CHAIN_TEXT_LEN + 48];

  testCommandGet(step->text, chainCommand, command, sizeof(command));

  if ((chainSendCb == NULL) || !chainSendCb(step->conn, command, COLOR_SELECTION))
  {
    snprintf(text, sizeof(text), tr("Chain stopped, could not send: %s"), command);
    chainEndNotify(text);
    return;
  }

  chainStepSent = true;

  if (testCommandGet(step->text, chainCommand + 1, next, sizeof(next)))
  {
    chainCommand++;
    chainDelayTimer = lv_timer_create(chainDelayTimerCb, chainDelayMs(), NULL);
    lv_timer_set_repeat_count(chainDelayTimer, 1);
    chainNoteUpdate();
    return;
  }

  compareTestStart(step->text, step->conn);

  // It says why (no variable received, ...)
  if (recording < 0)
  {
    chainFinish(NULL);
    benchNotify(tr("Chain stopped."));
    return;
  }

  chainNoteUpdate();
}

// Deleted by LVGL after this call
static void chainDelayTimerCb(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  chainDelayTimer = NULL;
  chainSend();
}

static uint32_t chainDelayMs(void)
{
  return (uint32_t) LV_CLAMP(1, atol(lv_textarea_get_text(chainDelayInput)), COMPARE_CHAIN_DELAY_MAX);
}

// The current step is over: the Prepare commands are sent, then the next step when goOn
static void afterStart(bool goOn)
{
  const ChainStep_t * step = &chainRun[chainIndex];

  // A test shorter than the delay: the settings are read before Prepare changes them
  if (settingsTimer != NULL)
  {
    lv_timer_delete(settingsTimer);
    settingsTimer = NULL;
    settingsSend();
  }

  // Only one list at a time: the former one has at most a few commands left
  if (afterTimer != NULL)
  {
    lv_timer_delete(afterTimer);
    afterTimer = NULL;
  }

  snprintf(afterText, sizeof(afterText), "%s", lv_textarea_get_text(prepareInput));
  afterConn      = step->conn;
  afterCommand   = 0;
  afterGoOn      = goOn;
  afterFailed[0] = '\0';
  chainStepSent  = false;
  afterSend();
}

// Next Prepare command, the same delay apart as the test's; the last one is also followed by that
// delay, before the next step
static void afterSend(void)
{
  char command[COMPARE_CHAIN_TEXT_LEN];
  char text[COMPARE_CHAIN_TEXT_LEN + 48];

  if (testCommandGet(afterText, afterCommand, command, sizeof(command)))
  {
    afterCommand++;

    if (((chainSendCb == NULL) || !chainSendCb(afterConn, command, COLOR_SELECTION)) && (afterFailed[0] == '\0'))
    {
      snprintf(afterFailed, sizeof(afterFailed), "%s", command);
    }

    snprintf(text, sizeof(text), tr("Sent: %s"), command);
    lv_label_set_text(chainNote, text);

    afterTimer = lv_timer_create(afterTimerCb, chainDelayMs(), NULL);
    lv_timer_set_repeat_count(afterTimer, 1);
    return;
  }

  // The setup may not be back to normal
  if (afterFailed[0] != '\0')
  {
    snprintf(text, sizeof(text), tr("Chain stopped, the setup is not ready, could not send: %s"), afterFailed);
    afterGoOn = false;
    chainFinish(NULL);
    plotterToast(text);
    benchNotify(text);
    return;
  }

  chainNoteUpdate();

  if (afterGoOn)
  {
    afterGoOn = false;
    chainNextAsync(NULL);
  }
}

// Deleted by LVGL after this call
static void afterTimerCb(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  afterTimer = NULL;
  afterSend();
}

static void chainNextAsync(void * data)
{
  char text[64];

  LV_UNUSED(data);

  // Stopped or replaced by another test meanwhile
  if ((chainIndex < 0) || (recording >= 0))
  {
    return;
  }

  chainIndex++;

  chainCommand = 0;

  if ((uint32_t) chainIndex < chainRunCount)
  {
    waitStart();
    return;
  }

  snprintf(text, sizeof(text), tr("Chain finished: %lu tests."), (unsigned long) chainRunCount);
  chainEndNotify(text);
}

// message: toast, NULL = none
// A chain ended by itself: nobody may be there
static void chainEndNotify(const char * message)
{
  chainFinish(message);
  benchNotify(message);
}

static void chainFinish(const char * message)
{
  if (chainDelayTimer != NULL)
  {
    lv_timer_delete(chainDelayTimer);
    chainDelayTimer = NULL;
  }

  // Stopped during the wait: the Prepare commands left are not sent
  if ((waitData != NULL) && (afterTimer != NULL))
  {
    lv_timer_delete(afterTimer);
    afterTimer = NULL;
  }

  // The chain ends with this step: only the Prepare commands are left
  afterGoOn = false;
  testDataFree(waitData);
  waitData = NULL;

  if ((chainIndex >= 0) && ((uint32_t) chainIndex < chainRunCount) && chainStepSent)
  {
    afterStart(false);
  }

  chainStepSent = false;
  chainIndex    = -1;
  chainNoteUpdate();

  if (message != NULL)
  {
    plotterToast(message);
  }
}

// Compiles the condition from the fields, on the recorded test's columns or the variables
// received. Also applies a change made during a recording.
static void conditionUpdate(void)
{
  static char        liveHeaders[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  char               text[COMPARE_CONDITION_LEN + 64];
  uint32_t           liveVars[PLOTTER_MAX_VARS];
  TestExprEnv_t      env;
  const TestData_t * test   = (recording >= 0) ? entries[recording].data : NULL;
  bool               custom = lv_dropdown_get_selected(stopDropdown) == STOP_CUSTOM;

  if (test != NULL)
  {
    env.headers     = test->headers;
    env.columnCount = test->columnCount;
  }
  else
  {
    env.columnCount = plotterVarHeaders(liveVars, liveHeaders);
    env.headers     = liveHeaders;
  }

  float tolerance  = strtof(lv_textarea_get_text(toleranceInput), NULL);
  float conversion = strtof(lv_textarea_get_text(conversionInput), NULL);

  conditionSamples = (uint32_t) LV_CLAMP(0, atol(lv_textarea_get_text(samplesInput)), TEST_EXPR_WINDOW_MAX);
  conditionBand    = testBandUnits(tolerance, conversion);
  env.refColumn    = headerFind(env.headers, env.columnCount, constantName);
  env.watchColumn  = headerFind(env.headers, env.columnCount, watchName);
  env.band         = conditionBand;
  env.start        = chainStartValue;
  env.samples      = conditionSamples;

  lv_obj_set_hidden(conditionRow, !custom);
  lv_obj_set_style_text_color(conditionInput, lv_color_hex(COLOR_ERROR), 0);
  lv_obj_set_style_text_color(conditionNote, lv_color_hex(COLOR_ERROR), 0);
  conditionValid = false;

  const char * source = custom ? lv_textarea_get_text(conditionInput) : CONDITION_SETTLED;
  uint32_t     errorAt = 0;
  TestExprError_t error = testExprCompile(&condition, source, &env, &errorAt);

  if (custom && (source[0] == '\0'))
  {
    lv_label_set_text(conditionNote, tr("Stops with the Stop button: no condition."));
  }
  else if (!custom && ((constantName[0] == '\0') || (watchName[0] == '\0')))
  {
    lv_label_set_text(conditionNote, tr("Choose the Constant and Watched variables to stop automatically."));
  }
  else if (!custom && (conversion <= 0.0f))
  {
    lv_label_set_text(conditionNote, tr("The percent for 1 unit must be above 0."));
  }
  else if (!custom && (conditionSamples == 0))
  {
    lv_label_set_text(conditionNote, tr("At least 1 sample is needed."));
  }
  else if (env.columnCount == 0)
  {
    // Not an error: the names are known once data is received
    lv_label_set_text(conditionNote, tr("Checked when the variables are received."));
    lv_obj_set_style_text_color(conditionNote, lv_color_hex(COLOR_INFO), 0);
  }
  else if (error == TEST_EXPR_ERROR_NAME)
  {
    lv_label_set_text_fmt(conditionNote, tr("Unknown name at character %lu."), (unsigned long) (errorAt + 1));
  }
  else if (error == TEST_EXPR_ERROR_NESTED)
  {
    lv_label_set_text_fmt(conditionNote, tr("A function over samples cannot be inside another (character %lu)."),
                          (unsigned long) (errorAt + 1));
  }
  else if (error == TEST_EXPR_ERROR_LENGTH)
  {
    lv_label_set_text(conditionNote, tr("Condition too long."));
  }
  else if (error != TEST_EXPR_OK)
  {
    lv_label_set_text_fmt(conditionNote, tr("Syntax error at character %lu."), (unsigned long) (errorAt + 1));
  }
  else
  {
    conditionValid = true;
    lv_obj_set_style_text_color(conditionInput, lv_obj_get_style_text_color(samplesInput, LV_PART_MAIN), 0);
    lv_obj_set_style_text_color(conditionNote, lv_color_hex(COLOR_INFO), 0);

    if (custom)
    {
      lv_label_set_text(conditionNote, tr("The test ends when the condition is true."));
    }
    else
    {
      snprintf(text, sizeof(text), tr("Ends when %s stays within %.4g of %s for %lu samples."), watchName,
               (double) env.band, constantName, (unsigned long) conditionSamples);
      lv_label_set_text(conditionNote, text);
    }
  }

  plateauNoteAdd();
}

// Added to the note of the condition
static void plateauNoteAdd(void)
{
  char text[COMPARE_CONDITION_LEN + (2 * TEST_HEADER_LEN) + 128];
  char added[TEST_HEADER_LEN + 64];

  if ((plateauMs() > 0) && (watchName[0] != '\0') && (conditionBand >= 0.0f))
  {
    snprintf(added, sizeof(added), tr("Also when the peaks of %s, averaged over %.4g s, are within %.4g for %.4g s."), watchName,
             plateauSmoothMs() / 1000.0, (double) conditionBand, plateauMs() / 1000.0);
    snprintf(text, sizeof(text), "%s %s", lv_label_get_text(conditionNote), added);
    lv_label_set_text(conditionNote, text);
  }
}

static void statusUpdate(void)
{
  if (recording >= 0)
  {
    const TestData_t * test = entries[recording].data;
    double seconds = (test->sampleCount > 0) ? (test->times[test->sampleCount - 1] / 1000.0) : 0.0;
    char   text[TEST_NAME_LEN + 48];

    snprintf(text, sizeof(text), tr("Recording: %s, %.1f s, %lu samples"), test->name, seconds,
             (unsigned long) test->sampleCount);
    lv_label_set_text(statusLabel, text);
    lv_label_set_text(recordBtnLabel, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(COLOR_ERROR), 0);
  }
  else
  {
    lv_label_set_text(statusLabel, tr("Waiting: send a command with " LV_SYMBOL_VIDEO " checked, or press " LV_SYMBOL_VIDEO "."));
    lv_label_set_text(recordBtnLabel, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(COLOR_INFO), 0);
  }
}

static void entryInfoUpdate(uint32_t i)
{
  const Entry_t *    entry = &entries[i];
  const TestData_t * test  = entry->data;
  char               text[80 + TEST_SETTINGS_LEN];
  double             seconds = (test->sampleCount > 0) ? (test->times[test->sampleCount - 1] / 1000.0) : 0.0;

  if ((int32_t) i == recording)
  {
    snprintf(text, sizeof(text), tr("Recording, %.1f s"), seconds);
  }
  else if (entry->end == END_CONDITION)
  {
    snprintf(text, sizeof(text), tr("Reached at %.1f s, %lu samples"), entry->settledMs / 1000.0, (unsigned long) test->sampleCount);
  }
  else if (entry->end == END_NOT_STABLE)
  {
    snprintf(text, sizeof(text), tr("Not stable after %.1f s, %lu samples"), seconds, (unsigned long) test->sampleCount);
  }
  else if (entry->end == END_PLATEAU)
  {
    snprintf(text, sizeof(text), tr("Stable at %.2f from %.1f s, %lu samples"), (double) entry->plateauValue,
             entry->settledMs / 1000.0, (unsigned long) test->sampleCount);
  }
  else
  {
    snprintf(text, sizeof(text), tr("%.1f s, %lu samples"), seconds, (unsigned long) test->sampleCount);
  }

  // Its settings on the next line
  if (test->settings[0] != '\0')
  {
    size_t used = strlen(text);

    snprintf(&text[used], sizeof(text) - used, "\n%s", test->settings);
  }

  if (entry->infoLabel != NULL)
  {
    lv_label_set_text(entry->infoLabel, text);
  }
}

// [graphs showing it, in the test's color] [name] [save] [delete], then its length and result
static void testsListRebuild(void)
{
  lv_obj_clean(testsList);

  for (uint32_t i = 0; i < entryCount; ++i)
  {
    Entry_t *  entry = &entries[i];
    void *     index = (void *) (uintptr_t) i;
    lv_obj_t * row   = uiRowCreate(testsList);

    lv_obj_t * shown = graphButtonsCreate(row, entry->graphMask, entry->color, testGraphEvent, index);
    uiTooltipSet(shown, tr("Graphs showing this test"));

    lv_obj_t * name = uiTextareaCreate(row, tr("Name"));
    lv_textarea_set_max_length(name, TEST_NAME_LEN - 1);
    lv_textarea_set_text(name, entry->data->name);
    lv_obj_set_user_data(name, index);
    lv_obj_add_event_cb(name, testNameEvent, LV_EVENT_VALUE_CHANGED, NULL);
    uiTooltipSet(name, tr("Name of the test (ex: its PID gains)"));
    lv_group_add_obj(group, name);

    // A test from a file is already saved
    if (entry->data->startMs != 0)
    {
      lv_obj_t * saveBtn = uiButtonCreate(row, LV_SYMBOL_SAVE, testSaveEvent);
      lv_obj_set_user_data(saveBtn, index);
      uiTooltipSet(saveBtn, tr("Save this test to a CSV file"));
    }

    lv_obj_t * deleteBtn = uiButtonCreate(row, LV_SYMBOL_CLOSE, testDeleteEvent);
    lv_obj_set_user_data(deleteBtn, index);
    uiTooltipSet(deleteBtn, tr("Delete"));

    entry->infoLabel = lv_label_create(testsList);
    lv_obj_set_width(entry->infoLabel, LV_PCT(100));
    lv_label_set_long_mode(entry->infoLabel, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(entry->infoLabel, lv_color_hex(COLOR_INFO), 0);
    entryInfoUpdate(i);
  }
}

static void testsListRebuildAsync(void * data)
{
  LV_UNUSED(data);
  testsListRebuild();
}

static void nameAdd(char list[][TEST_HEADER_LEN], uint32_t * count, const char * name)
{
  if ((name[0] == '\0') || (*count >= COMPARE_NAMES_MAX) ||
      (headerFind((const char (*)[TEST_HEADER_LEN]) list, *count, name) >= 0))
  {
    return;
  }

  snprintf(list[(*count)++], TEST_HEADER_LEN, "%s", name);
}

// Names of the variables received, of the tests and the ones chosen (kept in the lists before
// they are received). The lists are rebuilt when these change (an open dropdown would close).
static void namesRefresh(bool force)
{
  static char found[COMPARE_NAMES_MAX][TEST_HEADER_LEN];
  static char live[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  uint32_t    liveVars[PLOTTER_MAX_VARS];
  uint32_t    count = 0;
  uint32_t    liveCount = plotterVarHeaders(liveVars, live);
  static uint32_t conditionLiveCount;

  // The condition's names may be known before they are received
  if (liveCount != conditionLiveCount)
  {
    conditionLiveCount = liveCount;
    conditionUpdate();
  }

  memset(found, 0, sizeof(found));
  nameAdd(found, &count, constantName);
  nameAdd(found, &count, watchName);

  for (uint32_t g = 0; g < COMPARE_GRAPHS_MAX; ++g)
  {
    for (uint32_t s = 0; s < graphs[g].selectedCount; ++s)
    {
      nameAdd(found, &count, graphs[g].selected[s]);
    }
  }

  for (uint32_t c = 0; c < liveCount; ++c)
  {
    nameAdd(found, &count, live[c]);
  }

  for (uint32_t i = 0; i < entryCount; ++i)
  {
    for (uint32_t c = 0; c < entries[i].data->columnCount; ++c)
    {
      nameAdd(found, &count, entries[i].data->headers[c]);
    }
  }

  if (!force && (count == nameCount) && (memcmp(found, names, sizeof(names)) == 0))
  {
    return;
  }

  memcpy(names, found, sizeof(names));
  nameCount = count;

  size_t used = (size_t) snprintf(nameOptions, sizeof(nameOptions), "%s", tr("None"));

  for (uint32_t n = 0; n < nameCount; ++n)
  {
    used += (size_t) snprintf(&nameOptions[used], sizeof(nameOptions) - used, "\n%s", names[n]);
  }

  dropdownNamesSet(constantDropdown, constantName);
  dropdownNamesSet(watchDropdown, watchName);
  varsListRebuild();
  conditionUpdate();
}

static void dropdownNamesSet(lv_obj_t * dropdown, const char * name)
{
  int32_t index = headerFind((const char (*)[TEST_HEADER_LEN]) names, nameCount, name);

  lv_dropdown_set_options(dropdown, nameOptions);
  lv_dropdown_set_selected(dropdown, (uint32_t) (index + 1));
}

// [graphs showing it] [name] for each variable
static void varsListRebuild(void)
{
  char text[TEST_HEADER_LEN + 32];

  lv_obj_clean(varsList);

  for (uint32_t n = 0; n < nameCount; ++n)
  {
    uint32_t mask = 0;

    for (uint32_t g = 0; g < graphCount; ++g)
    {
      mask |= selectedHas(g, names[n]) ? (1u << g) : 0;
    }

    lv_obj_t * row = uiRowCreate(varsList);

    graphButtonsCreate(row, mask, COMPARE_NO_COLOR, varCheckEvent, (void *) (uintptr_t) n);

    lv_obj_t * label = lv_label_create(row);

    snprintf(text, sizeof(text), (strcmp(names[n], constantName) == 0) ? tr("%s (constant)") : "%s", names[n]);
    lv_label_set_text(label, text);
  }
}

// Nothing when it is there, or the graph is full
static void selectedAdd(uint32_t g, const char * name)
{
  Graph_t * graph = &graphs[g];

  if ((name[0] != '\0') && !selectedHas(g, name) && (graph->selectedCount < COMPARE_VARS_MAX))
  {
    snprintf(graph->selected[graph->selectedCount++], TEST_HEADER_LEN, "%s", name);
  }
}

// Keys "compareVars1" ... = names separated by tabs
static void selectedSave(uint32_t g)
{
  const Graph_t * graph = &graphs[g];
  char            key[COMPARE_GRAPHS_KEY_LEN];
  char            text[COMPARE_VARS_MAX * TEST_HEADER_LEN];
  size_t          used = 0;

  text[0] = '\0';

  for (uint32_t s = 0; s < graph->selectedCount; ++s)
  {
    used += (size_t) snprintf(&text[used], sizeof(text) - used, "%s%s", (s > 0) ? "\t" : "", graph->selected[s]);
  }

  snprintf(key, sizeof(key), "compareVars%lu", (unsigned long) (g + 1));
  settingsSetString(key, text);
}

// First graph: by default the variables chosen before there were several graphs, with the
// constant that was then always drawn
static void selectedLoad(uint32_t g)
{
  Graph_t *    graph = &graphs[g];
  char         key[COMPARE_GRAPHS_KEY_LEN];
  const char * former = (g == 0) ? settingsGetString("compareVars", "") : "";

  snprintf(key, sizeof(key), "compareVars%lu", (unsigned long) (g + 1));

  const char * text   = settingsGetString(key, former);
  bool         update = (text == former) && (former[0] != '\0');

  while ((text[0] != '\0') && (graph->selectedCount < COMPARE_VARS_MAX))
  {
    const char * end = strchr(text, COMPARE_SELECTED_SEP);
    size_t       len = (end != NULL) ? (size_t) (end - text) : strlen(text);

    snprintf(graph->selected[graph->selectedCount++], TEST_HEADER_LEN, "%.*s", (int) len, text);
    text = (end != NULL) ? (end + 1) : (text + len);
  }

  if (update)
  {
    selectedAdd(0, constantName);
  }
}

static bool selectedHas(uint32_t g, const char * name)
{
  return headerFind((const char (*)[TEST_HEADER_LEN]) graphs[g].selected, graphs[g].selectedCount, name) >= 0;
}

// First test with nothing selected: the watched and constant variables on the first graph, so it
// is not empty
static void selectionDefault(void)
{
  if ((graphs[0].selectedCount == 0) && (watchName[0] != '\0'))
  {
    selectedAdd(0, watchName);
    selectedAdd(0, constantName);
    selectedSave(0);
    varsListRebuild();
  }
}

static int32_t headerFind(const char (*headers)[TEST_HEADER_LEN], uint32_t count, const char * name)
{
  for (uint32_t c = 0; c < count; ++c)
  {
    if (strcmp(headers[c], name) == 0)
    {
      return (int32_t) c;
    }
  }

  return -1;
}

// What tells the test in the legend: its name, without the command it is named after: nothing
// next to the device's settings, otherwise the time at the end of that name
static const char * legendLabel(const Entry_t * entry)
{
  const char * name   = entry->data->name;
  size_t       length = strlen(name);

  if (!entry->nameIsCommand)
  {
    return name;
  }

  if ((entry->data->settings[0] != '\0') || (length < COMPARE_TIME_TEXT_LEN))
  {
    return "";
  }

  return &name[length - COMPARE_TIME_TEXT_LEN];
}

// The variables of the graph for each of its tests, then the constant once, from the longest test
// having it (so it spans the chart)
static void curvesBuild(Graph_t * graph, uint32_t g)
{
  lv_color_t         chartBg = lv_obj_get_style_bg_color(graph->chart, LV_PART_MAIN);
  bool               constant = selectedHas(g, constantName);
  const TestData_t * longest = NULL;
  int32_t            longestColumn = -1;
  uint32_t           longestMs = 0;

  graph->curveCount = 0;

  for (uint32_t i = 0; i < entryCount; ++i)
  {
    const TestData_t * test = entries[i].data;

    if ((entries[i].graphMask & (1u << g)) == 0)
    {
      continue;
    }

    bool first = true;

    for (uint32_t s = 0; s < graph->selectedCount; ++s)
    {
      int32_t column = testDataColumnFind(test, graph->selected[s]);

      if ((column >= 0) && (strcmp(graph->selected[s], constantName) != 0))
      {
        graph->curves[graph->curveCount++] = (Curve_t) {test, (uint32_t) column,
                                                        lv_color_mix(lv_color_hex(entries[i].color), chartBg, shadeOpa[s]),
                                                        first, legendLabel(&entries[i])};
        first = false;
      }
    }

    int32_t  column = constant ? testDataColumnFind(test, constantName) : -1;
    uint32_t lastMs = (test->sampleCount > 0) ? test->times[test->sampleCount - 1] : 0;

    if ((column >= 0) && ((longest == NULL) || (lastMs > longestMs)))
    {
      longest       = test;
      longestColumn = column;
      longestMs     = lastMs;
    }
  }

  if (longest != NULL)
  {
    graph->curves[graph->curveCount++] = (Curve_t) {longest, (uint32_t) longestColumn, lv_color_hex(COMPARE_COLOR_CONST), false, ""};
  }
}

// The constant is drawn once: tells when the tests do not share it
static void constantNoteUpdate(void)
{
  float low   = INFINITY;
  float high  = -INFINITY;
  float limit = COMPARE_CONST_MARGIN;

  for (uint32_t i = 0; i < entryCount; ++i)
  {
    int32_t column = (constantName[0] != '\0') ? testDataColumnFind(entries[i].data, constantName) : -1;
    float   value;

    if ((entries[i].graphMask != 0) && (column >= 0) && testDataLastValue(entries[i].data, (uint32_t) column, &value))
    {
      low  = fminf(low, value);
      high = fmaxf(high, value);
    }
  }

  // A warning for the screen, not for a report
  bool differs = ((high - low) > limit) && !pictureActive;

  if (differs)
  {
    char text[TEST_HEADER_LEN + 96];

    snprintf(text, sizeof(text), tr("%s differs between the tests (%.2f to %.2f): the gray line is the longest test's."),
             constantName, (double) low, (double) high);
    lv_label_set_text(noteLabel, text);
  }

  lv_obj_set_hidden(noteLabel, !differs);
}

// Whole tests, or the zoomed range
static void viewFit(Graph_t * graph)
{
  if (graph->zoomed)
  {
    return;
  }

  double longest = 0.0;

  for (uint32_t c = 0; c < graph->curveCount; ++c)
  {
    const TestData_t * test = graph->curves[c].data;

    if (test->sampleCount > 0)
    {
      longest = LV_MAX(longest, test->times[test->sampleCount - 1] / 1000.0);
    }
  }

  graph->viewStart = 0.0;
  graph->viewEnd   = LV_MAX(longest, 1.0);
}

// Sizes of the graphs at pictureScale
static void pictureStylesSet(const lv_font_t * font)
{
  lv_obj_set_style_text_font(graphsBox, font, 0);

  for (uint32_t g = 0; g < COMPARE_GRAPHS_MAX; ++g)
  {
    lv_obj_set_width(graphs[g].yAxis, AXIS_Y_WIDTH);
    lv_obj_set_height(graphs[g].xAxis, AXIS_X_HEIGHT);
    lv_obj_set_style_line_width(graphs[g].chart, LV_MAX(1, PICTURE_PX(2)), LV_PART_ITEMS);

    for (uint32_t c = 0; c < COMPARE_CURVES_MAX; ++c)
    {
      lv_obj_set_style_pad_left(graphs[g].legendLabels[c], PICTURE_PX(UI_LEGEND_DOT_PAD), 0);
    }
  }
}

// Lays the graphs out at the size chosen for the pictures, on white. Nothing reaches the screen:
// pictureEnd() puts them back before the next refresh.
static void pictureBegin(void)
{
  BenchPicture_t picture;

  if (!benchPictureGet(&picture))
  {
    return;
  }

  pictureActive = true;
  pictureScale  = picture.scale;

  uiThemeLightSet(true);
  lv_obj_set_flex_grow(graphsBox, 0);
  lv_obj_set_size(graphsBox, picture.width, picture.height);
  pictureStylesSet(picture.font);
  lv_obj_update_layout(graphsBox);
  chartRender();

  // The Y axes fitted their values: the charts have their final width
  lv_obj_update_layout(graphsBox);
  chartRender();
  lv_obj_update_layout(graphsBox);
}

static void pictureEnd(void)
{
  if (!pictureActive)
  {
    return;
  }

  pictureActive = false;
  pictureScale  = 100;

  lv_obj_set_size(graphsBox, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(graphsBox, 1);
  pictureStylesSet(uiFont());
  uiThemeLightSet(false);
  lv_obj_update_layout(graphsBox);
  chartRender();
}

static void chartRender(void)
{
  chartDirty = false;

  for (uint32_t g = 0; g < graphCount; ++g)
  {
    graphRender(&graphs[g], g);
  }

  yAxesWidthUpdate();
  constantNoteUpdate();
}

// As wide as the longest value of the graphs, the same for all so their time axes line up. A width
// that changes sets chartDirty, through the size of the charts.
static void yAxesWidthUpdate(void)
{
  const lv_font_t * font  = lv_obj_get_style_text_font(graphs[0].yAxis, LV_PART_MAIN);
  int32_t           width = 0;

  for (uint32_t g = 0; g < graphCount; ++g)
  {
    const Graph_t * graph = &graphs[g];
    lv_area_t       area;

    lv_obj_get_content_coords(graph->chart, &area);

    Ticks_t ticks = plotterTicksCompute(graph->yMin, graph->yMax, lv_area_get_height(&area), GRID_Y_SPACING_PX);

    for (uint32_t i = 0; i < ticks.count; ++i)
    {
      char       text[16];
      lv_point_t size;

      snprintf(text, sizeof(text), "%.*f", ticks.decimals, (double) plotterTickValue(&ticks, i));
      lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      width = LV_MAX(width, size.x);
    }
  }

  width = LV_MAX(AXIS_Y_WIDTH, width + PICTURE_PX(6));

  for (uint32_t g = 0; g < COMPARE_GRAPHS_MAX; ++g)
  {
    if (lv_obj_get_width(graphs[g].yAxis) != width)
    {
      lv_obj_set_width(graphs[g].yAxis, width);
    }
  }
}

// Each curve resampled on groups of 2 pixel columns (2 points per group: fewer points than columns,
// so lv_chart draws real, anti-aliased lines), the Y range fitted, the legend updated
static void graphRender(Graph_t * graph, uint32_t g)
{
  curvesBuild(graph, g);
  viewFit(graph);

  lv_obj_t * chart   = graph->chart;
  int32_t    width   = lv_obj_get_content_width(chart);
  uint32_t   columns = (uint32_t) LV_CLAMP(COMPARE_COLUMNS_MIN, (width / 2) - 1, COMPARE_COLUMNS_MAX);
  uint32_t   points  = 2 * columns;
  float      low     = INFINITY;
  float      high    = -INFINITY;

  if (lv_chart_get_point_count(chart) != points)
  {
    lv_chart_set_point_count(chart, points);
  }

  for (uint32_t c = 0; c < graph->curveCount; ++c)
  {
    const Curve_t * curve = &graph->curves[c];
    int32_t *       y     = lv_chart_get_series_y_array(chart, graph->series[c]);

    testDataCurve(curve->data, curve->column, graph->viewStart, graph->viewEnd, columns, curveBuffer);

    for (uint32_t i = 0; i < points; ++i)
    {
      if (!isnan(curveBuffer[i]))
      {
        low  = fminf(low, curveBuffer[i]);
        high = fmaxf(high, curveBuffer[i]);
      }
    }

    // Held as float bits until the Y range is known (as in the plots)
    memcpy(y, curveBuffer, points * sizeof(float));
  }

  if (!graph->zoomed && (low <= high))
  {
    float margin = (high > low) ? ((high - low) / 10.0f) : 1.0f;

    graph->yMin = low - margin;
    graph->yMax = high + margin;
  }

  if ((graph->yMax - graph->yMin) < COMPARE_Y_RANGE_MIN)
  {
    float center = (graph->yMax + graph->yMin) / 2.0f;

    graph->yMin = center - (COMPARE_Y_RANGE_MIN / 2.0f);
    graph->yMax = center + (COMPARE_Y_RANGE_MIN / 2.0f);
  }

  for (uint32_t c = 0; c < COMPARE_CURVES_MAX; ++c)
  {
    lv_obj_t * label = graph->legendLabels[c];
    bool       used  = c < graph->curveCount;

    lv_chart_hide_series(chart, graph->series[c], !used);
    lv_obj_set_hidden(label, !used);

    if (!used)
    {
      continue;
    }

    const Curve_t * curve = &graph->curves[c];
    const char *    name  = curve->data->headers[curve->column];
    int32_t *       y     = lv_chart_get_series_y_array(chart, graph->series[c]);

    for (uint32_t i = 0; i < points; ++i)
    {
      float value;

      memcpy(&value, &y[i], sizeof(value));
      y[i] = uiChartPoint(value, graph->yMin, graph->yMax);
    }

    lv_chart_set_series_color(chart, graph->series[c], curve->color);

    char text[TEST_NAME_LEN + TEST_HEADER_LEN + TEST_SETTINGS_LEN + 8];

    // The constant is not a test's: no label, no settings
    bool settings = curve->settings && (curve->data->settings[0] != '\0');
    char sent[TEST_SETTINGS_LEN];

    testSettingsSent(settings ? curve->data->settings : "", curve->data->commands, sent, sizeof(sent));
    snprintf(text, sizeof(text), "%s%s%s%s%s", name, (curve->label[0] != '\0') ? "  " : "", curve->label,
             settings ? "  " : "", sent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, curve->color, 0);
  }

  // Tells what is missing
  if (entryCount == 0)
  {
    lv_label_set_text(graph->hintLabel, tr("No test yet.\nCheck " LV_SYMBOL_VIDEO " on a command in the Commands window, then send it."));
  }
  else if (graph->curveCount == 0)
  {
    lv_label_set_text(graph->hintLabel, tr("Put a test and a variable on this graph with the number buttons at the left."));
  }

  lv_obj_set_hidden(graph->hintLabel, graph->curveCount > 0);
  lv_obj_set_hidden(graph->legend, graph->curveCount == 0);

  lv_chart_refresh(chart);
  lv_obj_invalidate(chart);
  lv_obj_invalidate(graph->yAxis);
  lv_obj_invalidate(graph->xAxis);
}

static int32_t timeToX(const Graph_t * graph, const lv_area_t * area, double time)
{
  return area->x1 + (int32_t) lround(((time - graph->viewStart) / (graph->viewEnd - graph->viewStart)) * (lv_area_get_width(area) - 1));
}

static double xToTime(const Graph_t * graph, const lv_area_t * area, int32_t x)
{
  return graph->viewStart + (((double) (x - area->x1) / LV_MAX(lv_area_get_width(area) - 1, 1)) * (graph->viewEnd - graph->viewStart));
}

static int32_t valueToY(const Graph_t * graph, const lv_area_t * area, float value)
{
  return area->y2 - (int32_t) lround((((double) value - graph->yMin) / ((double) graph->yMax - graph->yMin)) * (lv_area_get_height(area) - 1));
}

static float yToValue(const Graph_t * graph, const lv_area_t * area, int32_t y)
{
  return graph->yMin + (((float) (area->y2 - y) / (float) LV_MAX(lv_area_get_height(area) - 1, 1)) * (graph->yMax - graph->yMin));
}

// Background and gridlines, before the chart draws its lines (PREPROCESS)
static void gridDrawEvent(lv_event_t * e)
{
  const Graph_t *    graph = &graphs[(uintptr_t) lv_event_get_user_data(e)];
  lv_obj_t *         chart = graph->chart;
  lv_layer_t *       layer = lv_event_get_layer(e);
  lv_area_t          coords;
  lv_area_t          area;
  lv_draw_rect_dsc_t bgDsc;
  lv_draw_line_dsc_t lineDsc;

  lv_obj_get_coords(chart, &coords);
  lv_obj_get_content_coords(chart, &area);

  lv_draw_rect_dsc_init(&bgDsc);
  bgDsc.bg_color = lv_obj_get_style_bg_color(chart, LV_PART_MAIN);
  bgDsc.bg_opa   = LV_OPA_COVER;
  bgDsc.radius   = lv_obj_get_style_radius(chart, LV_PART_MAIN);
  lv_draw_rect(layer, &bgDsc, &coords);

  lv_draw_line_dsc_init(&lineDsc);
  lineDsc.width = 1;

  Ticks_t yTicks = plotterTicksCompute(graph->yMin, graph->yMax, lv_area_get_height(&area), GRID_Y_SPACING_PX);

  for (uint32_t i = 0; i < yTicks.count; ++i)
  {
    float value = plotterTickValue(&yTicks, i);

    lineDsc.color = lv_color_hex((value == 0.0f) ? (uiThemeIsDark() ? COLOR_GRID_ZERO_DARK : COLOR_GRID_ZERO)
                                                 : (uiThemeIsDark() ? COLOR_GRID_DARK : COLOR_GRID));
    lineDsc.p1.x  = area.x1;
    lineDsc.p2.x  = area.x2;
    lineDsc.p1.y  = valueToY(graph, &area, value);
    lineDsc.p2.y  = lineDsc.p1.y;
    lv_draw_line(layer, &lineDsc);
  }

  Ticks_t xTicks = plotterTicksCompute((float) graph->viewStart, (float) graph->viewEnd, lv_area_get_width(&area), GRID_X_SPACING_PX);

  lineDsc.color = lv_color_hex(uiThemeIsDark() ? COLOR_GRID_DARK : COLOR_GRID);

  for (uint32_t i = 0; i < xTicks.count; ++i)
  {
    lineDsc.p1.x = timeToX(graph, &area, plotterTickValue(&xTicks, i));
    lineDsc.p2.x = lineDsc.p1.x;
    lineDsc.p1.y = area.y1;
    lineDsc.p2.y = area.y2;
    lv_draw_line(layer, &lineDsc);
  }
}

// Zoom box being dragged
static void chartPostEvent(lv_event_t * e)
{
  uint32_t g = (uintptr_t) lv_event_get_user_data(e);

  if (selGraph == (int32_t) g)
  {
    lv_area_t          area;
    lv_draw_rect_dsc_t selDsc;

    lv_obj_get_content_coords(graphs[g].chart, &area);

    lv_area_t sel =
    {
      LV_CLAMP(area.x1, LV_MIN(selStart.x, selEnd.x), area.x2), LV_CLAMP(area.y1, LV_MIN(selStart.y, selEnd.y), area.y2),
      LV_CLAMP(area.x1, LV_MAX(selStart.x, selEnd.x), area.x2), LV_CLAMP(area.y1, LV_MAX(selStart.y, selEnd.y), area.y2),
    };

    lv_draw_rect_dsc_init(&selDsc);
    selDsc.bg_color     = lv_color_hex(COLOR_SELECTION);
    selDsc.bg_opa       = LV_OPA_20;
    selDsc.border_color = lv_color_hex(COLOR_SELECTION);
    selDsc.border_width = LV_MAX(1, uiPx(1));
    selDsc.border_opa   = LV_OPA_60;
    lv_draw_rect(lv_event_get_layer(e), &selDsc, &sel);
  }
}

// Dragging a box on a graph zooms on it
static void chartPressEvent(lv_event_t * e)
{
  uint32_t        g     = (uintptr_t) lv_event_get_user_data(e);
  Graph_t *       graph = &graphs[g];
  lv_event_code_t code  = lv_event_get_code(e);
  lv_point_t      point;
  lv_area_t       area;

  lv_indev_get_point(lv_indev_active(), &point);
  lv_obj_get_content_coords(graph->chart, &area);

  if (code == LV_EVENT_PRESSED)
  {
    selGraph = (int32_t) g;
    selStart = point;
    selEnd   = point;
    return;
  }

  if (selGraph != (int32_t) g)
  {
    return;
  }

  selEnd = point;
  lv_obj_invalidate(graph->chart);

  if (code == LV_EVENT_PRESS_LOST)
  {
    selGraph = -1;
  }
  else if (code == LV_EVENT_RELEASED)
  {
    selGraph = -1;

    if ((LV_ABS(selEnd.x - selStart.x) >= SEL_MIN_PX) && (LV_ABS(selEnd.y - selStart.y) >= SEL_MIN_PX))
    {
      double start = xToTime(graph, &area, LV_CLAMP(area.x1, LV_MIN(selStart.x, selEnd.x), area.x2));
      double end   = xToTime(graph, &area, LV_CLAMP(area.x1, LV_MAX(selStart.x, selEnd.x), area.x2));
      float  high  = yToValue(graph, &area, LV_CLAMP(area.y1, LV_MIN(selStart.y, selEnd.y), area.y2));
      float  low   = yToValue(graph, &area, LV_CLAMP(area.y1, LV_MAX(selStart.y, selEnd.y), area.y2));

      if ((end - start) >= COMPARE_RANGE_MIN_S)
      {
        graph->viewStart = start;
        graph->viewEnd   = end;
        graph->yMin      = low;
        graph->yMax      = high;
        graph->zoomed    = true;
        chartRender();
      }
    }
  }
}

// Stays in the chart. Its offsets are from the chart's top right corner.
static void legendDragEvent(lv_event_t * e)
{
  lv_obj_t * legend = lv_event_get_target_obj(e);
  lv_obj_t * parent = lv_obj_get_parent(legend);
  lv_point_t move;

  lv_indev_get_vect(lv_indev_active(), &move);

  int32_t minX = lv_obj_get_width(legend) - lv_obj_get_content_width(parent);
  int32_t maxY = lv_obj_get_content_height(parent) - lv_obj_get_height(legend);

  lv_obj_set_pos(legend, LV_CLAMP(minX, lv_obj_get_x_aligned(legend) + move.x, 0),
                 LV_CLAMP(0, lv_obj_get_y_aligned(legend) + move.y, LV_MAX(maxY, 0)));
}

// The curves' line width also in Y
static void chartDrawTaskEvent(lv_event_t * e)
{
  lv_draw_task_t *     task = lv_event_get_draw_task(e);
  lv_draw_dsc_base_t * base = lv_draw_task_get_draw_dsc(task);

  if ((lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_LINE) && (base->part == LV_PART_ITEMS))
  {
    uiChartLineThicken(lv_draw_task_get_line_dsc(task));
  }
}

static void chartSizeEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  chartDirty = true;
}

static void yAxisDrawEvent(lv_event_t * e)
{
  const Graph_t *     graph = &graphs[(uintptr_t) lv_event_get_user_data(e)];
  lv_layer_t *        layer = lv_event_get_layer(e);
  lv_area_t           chartArea;
  lv_area_t           axisArea;
  lv_draw_label_dsc_t labelDsc;

  lv_obj_get_content_coords(graph->chart, &chartArea);
  lv_obj_get_coords(graph->yAxis, &axisArea);

  lv_draw_label_dsc_init(&labelDsc);
  lv_obj_init_draw_label_dsc(graph->yAxis, LV_PART_MAIN, &labelDsc);
  labelDsc.color      = lv_color_hex(COLOR_INFO);
  labelDsc.align      = LV_TEXT_ALIGN_RIGHT;
  labelDsc.text_local = 1;

  Ticks_t ticks      = plotterTicksCompute(graph->yMin, graph->yMax, lv_area_get_height(&chartArea), GRID_Y_SPACING_PX);
  int32_t lineHeight = lv_font_get_line_height(labelDsc.font);

  for (uint32_t i = 0; i < ticks.count; ++i)
  {
    char      text[16];
    float     value = plotterTickValue(&ticks, i);
    int32_t   y = valueToY(graph, &chartArea, value);
    int32_t   top = uiAxisLabelTop(y, lineHeight, &axisArea);
    lv_area_t textArea = {axisArea.x1, top, axisArea.x2 - PICTURE_PX(3), top + lineHeight};

    snprintf(text, sizeof(text), "%.*f", ticks.decimals, (double) value);
    labelDsc.text = text;
    lv_draw_label(layer, &labelDsc, &textArea);
  }
}

// Seconds since the start of the tests
static void xAxisDrawEvent(lv_event_t * e)
{
  const Graph_t *     graph = &graphs[(uintptr_t) lv_event_get_user_data(e)];
  lv_layer_t *        layer = lv_event_get_layer(e);
  lv_area_t           chartArea;
  lv_area_t           axisArea;
  lv_draw_label_dsc_t labelDsc;

  lv_obj_get_content_coords(graph->chart, &chartArea);
  lv_obj_get_coords(graph->xAxis, &axisArea);

  lv_draw_label_dsc_init(&labelDsc);
  lv_obj_init_draw_label_dsc(graph->xAxis, LV_PART_MAIN, &labelDsc);
  labelDsc.color      = lv_color_hex(COLOR_INFO);
  labelDsc.align      = LV_TEXT_ALIGN_CENTER;
  labelDsc.text_local = 1;

  Ticks_t ticks = plotterTicksCompute((float) graph->viewStart, (float) graph->viewEnd, lv_area_get_width(&chartArea), GRID_X_SPACING_PX);

  for (uint32_t i = 0; i < ticks.count; ++i)
  {
    char      text[24];
    float     time = plotterTickValue(&ticks, i);
    int32_t   x = timeToX(graph, &chartArea, time);

    snprintf(text, sizeof(text), "%.*f s", ticks.decimals, (double) time);

    lv_area_t textArea = uiAxisLabelArea(text, labelDsc.font, x, &axisArea);

    labelDsc.text = text;
    lv_draw_label(layer, &labelDsc, &textArea);
  }
}

// The recording's progress, new variable names, and the chart once it is shown
static void refreshTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  namesRefresh(false);

  if (settingsCapturing && (lv_tick_elaps(settingsTick) >= COMPARE_REPLY_MS))
  {
    settingsCaptureEnd();
  }

  // Here and not on each sample: it also ends a test that receives nothing
  if ((recording >= 0) && (testMaxMs() > 0) && (lv_tick_elaps(recordStartTick) >= testMaxMs()))
  {
    recordFinish(END_NOT_STABLE);
  }

  if (recording >= 0)
  {
    statusUpdate();
    entryInfoUpdate((uint32_t) recording);
  }

  if (waitData != NULL)
  {
    chainNoteUpdate();
  }

  if (chartDirty && (selGraph < 0) && lv_obj_is_visible(graphsBox))
  {
    chartRender();
  }
}

static void recordEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (recording >= 0)
  {
    recordFinish(END_STOPPED);
  }
  else
  {
    compareTestStart(tr("Manual"), 0);
  }
}

static void conditionEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  conditionUpdate();
}

static void constantEvent(lv_event_t * e)
{
  uint32_t index = lv_dropdown_get_selected(lv_event_get_target_obj(e));

  snprintf(constantName, sizeof(constantName), "%s", (index > 0) ? names[index - 1] : "");
  settingsSetString("compareConstant", constantName);
  selectedAdd(0, constantName);
  selectedSave(0);
  varsListRebuild();
  conditionUpdate();
  chartDirty = true;
}

static void watchEvent(lv_event_t * e)
{
  uint32_t index = lv_dropdown_get_selected(lv_event_get_target_obj(e));

  snprintf(watchName, sizeof(watchName), "%s", (index > 0) ? names[index - 1] : "");
  settingsSetString("compareWatch", watchName);
  conditionUpdate();
}

// A variable's graph button; a full graph keeps its variables
static void varCheckEvent(lv_event_t * e)
{
  lv_obj_t * buttons = lv_event_get_target_obj(e);
  uint32_t   n       = (uintptr_t) lv_obj_get_user_data(buttons);
  uint32_t   g;
  bool       checked = graphButtonGet(buttons, &g);

  if ((n >= nameCount) || (g >= graphCount))
  {
    return;
  }

  Graph_t * graph = &graphs[g];

  if (!checked)
  {
    int32_t index = headerFind((const char (*)[TEST_HEADER_LEN]) graph->selected, graph->selectedCount, names[n]);

    if (index >= 0)
    {
      memmove(&graph->selected[index], &graph->selected[index + 1], (graph->selectedCount - (uint32_t) index - 1) * TEST_HEADER_LEN);
      graph->selectedCount--;
    }
  }
  else if (graph->selectedCount >= COMPARE_VARS_MAX)
  {
    lv_buttonmatrix_clear_button_ctrl(buttons, g, LV_BUTTONMATRIX_CTRL_CHECKED);
    plotterToast(tr("4 variables at most per graph."));
    return;
  }
  else
  {
    selectedAdd(g, names[n]);
  }

  selectedSave(g);
  chartRender();
}

// A test's graph button
static void testGraphEvent(lv_event_t * e)
{
  lv_obj_t * buttons = lv_event_get_target_obj(e);
  uint32_t   i       = (uintptr_t) lv_obj_get_user_data(buttons);
  uint32_t   g;
  bool       checked = graphButtonGet(buttons, &g);

  if (i < entryCount)
  {
    entries[i].graphMask = checked ? (entries[i].graphMask | (1u << g)) : (entries[i].graphMask & ~(1u << g));
    chartRender();
  }
}

// The rows have one button per graph
static void graphCountEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  graphCount = lv_dropdown_get_selected(graphCountDropdown) + 1;

  for (uint32_t g = 0; g < COMPARE_GRAPHS_MAX; ++g)
  {
    lv_obj_set_hidden(graphs[g].box, g >= graphCount);
  }

  testsListRebuild();
  varsListRebuild();
  lv_obj_update_layout(graphsBox);
  chartRender();
}

static void screenshotEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  chartRender();
  pictureBegin();
  benchScreenshot(graphsBox, tr("Compare"));
  pictureEnd();
}

static void testNameEvent(lv_event_t * e)
{
  lv_obj_t * name = lv_event_get_target_obj(e);
  uint32_t   i    = (uintptr_t) lv_obj_get_user_data(name);

  if (i < entryCount)
  {
    snprintf(entries[i].data->name, TEST_NAME_LEN, "%s", lv_textarea_get_text(name));
    entries[i].nameIsCommand = false;
    chartRender();
  }
}

// Same file as the export of the plots, so it can be opened here later. The copy is taken now.
static void testSaveEvent(lv_event_t * e)
{
  uint32_t i = (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(e));

  if ((i >= entryCount) || ((int32_t) i == recording) || (entries[i].data->sampleCount == 0))
  {
    plotterToast(tr((i < entryCount) && ((int32_t) i == recording) ? "Stop the test before saving it." : "No data to export."));
    return;
  }

  PlotExport_t * data = plotExportFromTest(entries[i].data);

  if (data == NULL)
  {
    plotterToast(tr("Not enough memory to export."));
    return;
  }

  plotterExport(data);
}

// The clicked button is one of the rows: they are rebuilt after this event
static void testDeleteEvent(lv_event_t * e)
{
  uint32_t i = (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(e));

  if (i >= entryCount)
  {
    return;
  }

  if ((int32_t) i == recording)
  {
    recordFinish(END_STOPPED);
  }

  testDataFree(entries[i].data);
  memmove(&entries[i], &entries[i + 1], (entryCount - i - 1) * sizeof(Entry_t));
  entryCount--;

  if (recording > (int32_t) i)
  {
    recording--;
  }

  // Its labels go with the rows
  for (uint32_t n = 0; n < entryCount; ++n)
  {
    entries[n].infoLabel = NULL;
  }

  lv_async_call(testsListRebuildAsync, NULL);
  namesRefresh(false);
  compareZoomReset();
}

static void captureEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (entryCount >= COMPARE_TESTS_MAX)
  {
    plotterToast(tr("16 tests at most: delete one first."));
    return;
  }

  TestData_t * test = plotterCapture();

  if (test == NULL)
  {
    plotterToast(tr("No data to export."));
    return;
  }

  time_t    now   = time(NULL);
  struct tm local = *localtime(&now);

  snprintf(test->name, TEST_NAME_LEN, "%s %02d:%02d:%02d", tr("Plot"), local.tm_hour, local.tm_min, local.tm_sec);
  entryAdd(test, END_NONE);
  namesRefresh(false);
  selectionDefault();
  testsListRebuild();
  compareZoomReset();
}

static void zoomResetEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  compareZoomReset();
}

// Empty is valid: no wait. The names are checked on the variables received.
static bool chainWaitValid(void)
{
  static char headers[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  uint32_t    columns[PLOTTER_MAX_VARS];
  uint32_t    errorAt;
  uint32_t    count = plotterVarHeaders(columns, headers);

  return (lv_textarea_get_text(waitInput)[0] == '\0') ||
         (waitCompile((const char (*)[TEST_HEADER_LEN]) headers, count, 0.0f, &errorAt) == TEST_EXPR_OK);
}

// Starts the chain, or stops it with its test. Rows without a command are skipped.
static void chainRunEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  // The test ends first: the Prepare commands are not part of it
  if (chainIndex >= 0)
  {
    if (recording >= 0)
    {
      recordFinish(END_STOPPED);
    }
    else
    {
      chainFinish(tr("Chain stopped."));
    }

    return;
  }

  chainRunCount = 0;

  for (uint32_t i = 0; i < chainStepCount; ++i)
  {
    char command[COMPARE_CHAIN_TEXT_LEN];

    if (testCommandGet(chainSteps[i].text, 0, command, sizeof(command)))
    {
      chainRun[chainRunCount++] = chainSteps[i];
    }
  }

  conditionUpdate();

  if (chainRunCount == 0)
  {
    plotterToast(tr("Add commands to the chain first."));
  }
  else if (recording >= 0)
  {
    plotterToast(tr("Stop the test before starting the chain."));
  }
  else if (afterTimer != NULL)
  {
    plotterToast(tr("Wait for the Prepare commands to be sent."));
  }
  else if ((entryCount + chainRunCount) > COMPARE_TESTS_MAX)
  {
    plotterToast(tr("Not enough room for the tests of the chain: delete some tests."));
  }
  else if (!conditionValid && (testMaxMs() == 0) && (plateauMs() == 0))
  {
    // Its first test would never end
    plotterToast(tr("A chain needs a valid condition, Auto stable or a max test time."));
  }
  else if (!chainWaitValid())
  {
    plotterToast(tr("The Start when condition is not valid."));
  }
  else
  {
    chainIndex      = 0;
    chainCommand    = 0;
    chainStartValue = NAN;
    waitStart();
  }
}

static void chainAddEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (chainStepCount < COMPARE_CHAIN_MAX)
  {
    chainSteps[chainStepCount++] = (ChainStep_t) {"", 0};
    chainSave();
    chainListRebuild();
  }
}

static void chainTextEvent(lv_event_t * e)
{
  lv_obj_t * text = lv_event_get_target_obj(e);
  uint32_t   i    = (uintptr_t) lv_obj_get_user_data(text);

  if (i < chainStepCount)
  {
    snprintf(chainSteps[i].text, COMPARE_CHAIN_TEXT_LEN, "%s", lv_textarea_get_text(text));
    chainSave();
  }
}

static void chainConnEvent(lv_event_t * e)
{
  lv_obj_t * conn = lv_event_get_target_obj(e);
  uint32_t   i    = (uintptr_t) lv_obj_get_user_data(conn);

  if (i < chainStepCount)
  {
    chainSteps[i].conn = lv_dropdown_get_selected(conn);
    chainSave();
  }
}

// The clicked button is one of the rows: they are rebuilt after this event
static void chainUpEvent(lv_event_t * e)
{
  uint32_t i = (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(e));

  if ((i > 0) && (i < chainStepCount))
  {
    ChainStep_t moved = chainSteps[i];

    chainSteps[i]     = chainSteps[i - 1];
    chainSteps[i - 1] = moved;
    chainSave();
    lv_async_call(chainListRebuildAsync, NULL);
  }
}

static void chainDeleteEvent(lv_event_t * e)
{
  uint32_t i = (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(e));

  if (i < chainStepCount)
  {
    memmove(&chainSteps[i], &chainSteps[i + 1], (chainStepCount - i - 1) * sizeof(ChainStep_t));
    chainStepCount--;
    chainSave();
    lv_async_call(chainListRebuildAsync, NULL);
  }
}

// The dialog runs in its own thread, like the export's
static void openEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (InterlockedCompareExchange(&openState, PLOT_EXPORT_BUSY, PLOT_EXPORT_IDLE) != PLOT_EXPORT_IDLE)
  {
    return;
  }

  openOwner = (HWND) lv_windows_get_display_window_handle(lv_display_get_default());

  uintptr_t thread = _beginthreadex(NULL, 0, openThread, NULL, 0, NULL);

  if (thread == 0)
  {
    InterlockedExchange(&openState, PLOT_EXPORT_IDLE);
    plotterToast(tr("Could not open the files."));
    return;
  }

  CloseHandle((HANDLE) thread);
  lv_obj_set_state(openBtn, LV_STATE_DISABLED, true);
  lv_timer_create(openPollTimer, OPEN_POLL_MS, NULL);
}

static void openPollTimer(lv_timer_t * tim)
{
  char     text[96];
  uint32_t added = 0;

  if (InterlockedCompareExchange(&openState, PLOT_EXPORT_DONE, PLOT_EXPORT_DONE) != PLOT_EXPORT_DONE)
  {
    return;
  }

  lv_timer_delete(tim);
  lv_obj_set_state(openBtn, LV_STATE_DISABLED, false);

  for (uint32_t i = 0; i < openTestCount; ++i)
  {
    if (entryAdd(openTests[i], END_NONE))
    {
      // The file's name holds the command: its settings say more
      entries[entryCount - 1].nameIsCommand = openTests[i]->settings[0] != '\0';
      added++;
    }
    else
    {
      testDataFree(openTests[i]);
      openFailures++;
    }
  }

  if (added > 0)
  {
    namesRefresh(false);
    selectionDefault();
    testsListRebuild();
    compareZoomReset();
  }

  if (openFailures > 0)
  {
    snprintf(text, sizeof(text), tr("%lu tests opened, %lu not (not an export file, or 16 tests at most)."),
             (unsigned long) added, (unsigned long) openFailures);
    plotterToast(text);
  }

  openTestCount = 0;
  openFailures  = 0;
  InterlockedExchange(&openState, PLOT_EXPORT_IDLE);
}

// Multiple selection: the folder, then the file names, each ending with a 0 (one file: its path)
static unsigned __stdcall openThread(void * arg)
{
  LV_UNUSED(arg);

  openPaths[0] = L'\0';

  // "\0" separated pairs: not a single text for tr()
  if (appWindowFileDialog(openOwner, false, tr("Open tests"),
                          (langGet() == LANG_FR) ? L"Fichier CSV (*.csv)\0*.csv\0Tous les fichiers (*.*)\0*.*\0"
                                                 : L"CSV file (*.csv)\0*.csv\0All files (*.*)\0*.*\0",
                          NULL, openPaths, OPEN_PATHS_LEN, OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT))
  {
    const wchar_t * folder = openPaths;
    const wchar_t * name   = folder + wcslen(folder) + 1;

    if (*name == L'\0')
    {
      openFile(folder);
    }

    while (*name != L'\0')
    {
      wchar_t path[MAX_PATH * 2];
      size_t  folderLen = wcslen(folder);
      size_t  nameLen   = wcslen(name);

      if ((folderLen + nameLen + 2) <= (sizeof(path) / sizeof(path[0])))
      {
        wmemcpy(path, folder, folderLen);
        path[folderLen] = L'\\';
        wmemcpy(&path[folderLen + 1], name, nameLen + 1);
        openFile(path);
      }
      else
      {
        openFailures++;
      }

      name += nameLen + 1;
    }
  }

  InterlockedExchange(&openState, PLOT_EXPORT_DONE);
  return 0;
}

// The test takes the file's name
static void openFile(const wchar_t * path)
{
  FILE *       file = _wfopen(path, L"rb");
  TestData_t * test = NULL;

  if ((file != NULL) && (openTestCount < COMPARE_TESTS_MAX))
  {
    test = testDataLoadCsv(file);
  }

  if (file != NULL)
  {
    fclose(file);
  }

  if (test == NULL)
  {
    openFailures++;
    return;
  }

  const wchar_t * base = wcsrchr(path, L'\\');
  char            name[OPEN_NAME_LEN];

  WideCharToMultiByte(CP_UTF8, 0, (base != NULL) ? (base + 1) : path, -1, name, sizeof(name), NULL, NULL);

  char * extension = strrchr(name, '.');

  if (extension != NULL)
  {
    *extension = '\0';
  }

  snprintf(test->name, TEST_NAME_LEN, "%s", name);
  openTests[openTestCount++] = test;
}
