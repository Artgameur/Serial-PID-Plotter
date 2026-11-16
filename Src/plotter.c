// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Plotter of the numbers received on the serial port: up to 4 plots, per variable
          name / gain / offset / line thickness, cursors, zoom, grid and side bars.

   The raw received values are kept in a history buffer of the last PLOT_POINTS_MAX samples. The
   plots show the last plotLength of them ("positions", 0 = oldest), and each chart the positions
   viewStart..viewEnd (X zoom, per plot), computed as value * gain + offset, so any
   setting change also updates what is already plotted. With more shown samples than the chart can
   show, each group of samples (about one per 2 pixel columns) is drawn as its min and max (keeps
   the peaks). lv_chart gets fewer points than pixel columns, so it draws real lines, anti-aliased,
   not its bars of one pixel column.

   With several serial connections, the variables are split between them (connection c owns
   PLOTTER_MAX_VARS / count variables, from c x that number). Each line received on any connection
   adds a sample: its connection's variables get the line's values, the other connections' keep
   their last value (until that connection is reset / cleared).

   A test being recorded (compare.c) gets every data line, also while the plots are paused.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "plotter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cmd_panel.h"
#include "compare.h"
#include "lang.h"
#include "line_parser.h"
#include "plot_export.h"
#include "profile.h"
#include "settings.h"
#include "test_bench.h"
#include "ui_widgets.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define PLOT_COUNT_MAX        8
#define PLOT_POINTS_MAX       10000       // history kept, whatever the plot length
#define PLOT_LENGTH_DEFAULT   1           // index in plotLengths (200)
#define PLOT_DISPLAY_BUCKETS  500         // groups before the chart has a size
#define PLOT_BUCKETS_MIN      100         // groups: one per 2 pixel columns of the chart, within
#define PLOT_BUCKETS_MAX      4000        // these limits (above: min / max per group)
#define PLOT_COLUMNS_MARGIN   2           // groups less than half the columns: fewer points than columns
#define PLOT_RENDER_MS        30          // charts are redrawn at most this often
#define PLOT_RENDER_MIN_MS    16          // even at higher frame rates: new data at most ~60 x / s
#define PLOT_VIEW_MIN         3           // smallest X zoom, in samples
#define PLOT_VALUE_MAX        10000000.0f // larger displayed values are not drawn
#define PLOT_Y_RANGE_MIN      0.0005f     // smallest Y range: gridlines every 0.0001
#define PLOT_Y_RANGE_REL      0.000002f   // ... and x the values (float precision: ~7 digits)
#define VALUE_DECIMALS_MIN    2           // values shown with 2 decimals, up to 4 when zoomed in
#define VALUE_DECIMALS_MAX    4
#define PLOT_NAME_LEN         16
#define PLOT_UNIT_LEN         12
#define PLOT_NUMBER_LEN       16
#define PLOT_KEY_LEN          24

// The chart is drawn with the largest width; each variable's line is then thinned to its own
// width while being drawn (a draw task area can shrink safely, not grow)
#define LINE_WIDTH_MIN        1
#define LINE_WIDTH_MAX        6
#define LINE_WIDTH_DEFAULT    2

// Bar graph: one slot per variable (bar + value above + name below). Next to the chart the slots
// have a fixed width; without the chart they share the plot's width.
#define BAR_GAP               uiPx(4)

// Chart and bar graph side by side: the divider between them sets the bars' share of the width
#define DIVIDER_WIDTH         uiPx(8)
#define BARS_SHARE_DEFAULT    250         // 1/GEOM_FULL
#define BARS_SHARE_MIN        50
#define BARS_SHARE_MAX        950
#define CHART_MIN_WIDTH       uiPx(60)
#define BARS_MIN_WIDTH        uiPx(30)

// Plot windows: moved with their title bar, resized with their corner handle; edges snap to the
// plot area and to the other windows within SNAP_PX. Geometry in 1/1000 of the plot area.
#define WINDOW_GAP            uiPx(4)
#define WINDOW_MIN_W          uiPx(140)
#define WINDOW_MIN_H          uiPx(100)
#define WINDOW_HANDLE         uiPx(14)
#define SNAP_PX               uiPx(8)
#define GEOM_FULL             1000
// Windows in the plot area: the plots (window w = plot w), then the commands window
#define WIN_CMD               PLOT_COUNT_MAX
#define WIN_COUNT             (PLOT_COUNT_MAX + 1)
#define CMD_TILE_WIDTH        250         // commands window column when tiled (1/GEOM_FULL)
#define SNAP_TARGETS_MAX      (2 + (4 * WIN_COUNT))

// Seams: the gaps between windows whose edges are on the same line. Dragging one moves all these
// edges (resizes the windows on both sides). Grabbed in the gap or on a window's border.
#define SEAM_GRAB_PX          ((WINDOW_GAP / 2) + uiPx(4))
#define SEAM_ALIGN_PX         uiPx(2)

// Axes and grid: spacing between gridlines adapts to the zoom (1, 2, 5 x 10^n steps)
#define AXIS_Y_WIDTH          uiPx(28)
#define PICTURE_PX(px)        ((uiPx(px) * pictureScale) / 100)   // follows the size of a picture
#define AXIS_X_HEIGHT         PICTURE_PX(16)
#define GRID_Y_SPACING_PX     PICTURE_PX(24)
#define GRID_X_SPACING_PX     PICTURE_PX(50)
#define GRID_X_TIME_SPACING_PX PICTURE_PX(80)   // time labels are wider ("12:03:15.2")

// X axis labels (order of xAxisOptions): sample number, seconds before the latest sample, time of
// day the sample was received. The gridlines stay on samples: with time, the label is the time of
// the sample under the gridline.
#define X_AXIS_SAMPLES        0
#define X_AXIS_SECONDS        1
#define X_AXIS_CLOCK          2
#define GRID_MAX_LINES        64

// Tools of cursorToolBtnm: 2 vertical cursors (position), 2 horizontal (value), zoom
#define TOOL_NONE             -1
#define TOOL_V1               0
#define TOOL_V2               1
#define TOOL_H1               2
#define TOOL_H2               3
#define TOOL_ZOOM             4
#define CURSOR_NONE           -1
#define CURSOR_COLOR_1        0x111827
#define CURSOR_COLOR_1_DARK   0xF9FAFB
#define CURSOR_COLOR_2        0x9333EA

// Command marks: vertical line where a command was sent (between the samples before / after it)
#define CMD_MARKS_MAX         64          // latest ones kept
#define CMD_MARK_TEXT_LEN     32          // label: the command (its start when longer)
#define CMD_MARK_PAD_X        uiPx(4)     // label's rounded background around the text
#define CMD_MARK_RADIUS       uiPx(4)
#define CMD_MARK_DOT          LV_MAX(1, uiPx(2))   // dotted line: dot length, gap
#define CMD_MARK_DOT_GAP      LV_MAX(2, uiPx(3))

// Zoom selection being dragged
#define SEL_NONE              0
#define SEL_BOX               1           // in a chart (Zoom tool): X and Y
#define SEL_X                 2           // on the X axis: X only
#define SEL_Y                 3           // on a Y axis: Y only
#define SEL_MIN_PX            uiPx(4)

#define ZOOM_ALL              -1          // zoomReset(): every plot

// Settings panel: fields as wide as their content needs, not the whole row
#define SETTINGS_NUMBER_WIDTH   uiPx(64)    // plot / variable number, plot count
#define SETTINGS_LENGTH_WIDTH   uiPx(120)   // "10000 points", "6 px"
#define SETTINGS_VALUE_WIDTH    uiPx(80)    // Y min / max, gain, offset
#define SETTINGS_UNIT_WIDTH     uiPx(64)
#define SETTINGS_NAME_WIDTH     uiPx(120)   // plot / variable name
#define SHOW_IN_BUTTON_W        uiPx(38)    // "Show in" buttons: up to 4 per row
#define SHOW_IN_BUTTON_H        uiPx(28)
#define SHOW_IN_PER_ROW         4

#define CONN_NONE             UINT32_MAX  // varConn(): variable of no connection

#define EXPORT_POLL_MS        200         // export result checked this often while it runs
#define TOAST_MS              6000        // export result shown this long

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  char    name[PLOT_NAME_LEN];
  char    unit[PLOT_UNIT_LEN];          // shown after the value, ex: "°C" (optional)
  uint32_t color;                       // 0xRRGGBB: line, legend, bar and name
  char    gainText[PLOT_NUMBER_LEN];    // as typed, ex: "2.5" or "/4"
  char    offsetText[PLOT_NUMBER_LEN];
  float   gain;
  float   offset;
  uint8_t width;
  uint8_t plotMask;                     // bit p set: shown in plot p
} PlotVar_t;

typedef struct
{
  lv_obj_t *          cell;             // window (winObj): title bar, row, X axis
  lv_obj_t *          header;           // title bar (drag to move): name + legend
  lv_obj_t *          legend;           // "Name value" of the variables shown in this plot
  lv_obj_t *          legendLabels[PLOTTER_MAX_VARS];
  lv_obj_t *          row;              // Y axis + chart + bars
  lv_obj_t *          yAxis;
  lv_obj_t *          chart;
  lv_obj_t *          divider;          // between chart and bars: drag to share the width
  lv_obj_t *          bars;
  lv_obj_t *          xAxis;
  lv_obj_t *          hint;             // shown while the plot has nothing to show
  lv_chart_series_t * series[PLOTTER_MAX_VARS];
  uint32_t            viewStart;        // shown positions (X zoom)
  uint32_t            viewEnd;
  float               yMin;             // displayed values at the bottom / top of the chart
  float               yMax;
  bool                yZoomed;          // zoomed in: yMin / yMax kept until the zoom is reset
  bool                chartVisible;     // line chart (off: bar graph only)
  bool                barsVisible;      // bar graph (at the right of the chart, or alone)
  bool                cmdMarks;         // lines where the commands were sent
  int32_t             barsShare;        // bar graph's part of the width next to the chart (1/GEOM_FULL)
  bool                yManual;          // Y scale set by the user (otherwise: follows the data)
  float               yManualMin;
  float               yManualMax;
  char                yMinText[PLOT_NUMBER_LEN];   // as typed
  char                yMaxText[PLOT_NUMBER_LEN];
  char                name[PLOT_NAME_LEN];         // optional, after "P1" in its title
  uint32_t            xAxisMode;                   // X_AXIS_xxx: X axis labels
  lv_obj_t *          title;            // "P1" / "P1  name" in the title bar
} Plot_t;

// Command sent: number of the first sample received after it, label, color (the command's)
typedef struct
{
  uint64_t sample;
  char     text[CMD_MARK_TEXT_LEN];
  uint32_t color;
} CmdMark_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

// Variables 1-8: 8 hues, 9-16: 8 other hues, 17-32: darker shades of 1-16 (neighbor numbers
// never look alike)
static const uint32_t varDefaultColors[PLOTTER_MAX_VARS] =
{
  0x2563EB, 0xDC2626, 0x16A34A, 0xD97706, 0x9333EA, 0x0891B2, 0xDB2777, 0x4B5563,
  0x65A30D, 0x0284C7, 0xE11D48, 0xCA8A04, 0x4F46E5, 0x0D9488, 0xC026D3, 0x92400E,
  0x1E40AF, 0x991B1B, 0x166534, 0xB45309, 0x6B21A8, 0x155E75, 0x9D174D, 0x1F2937,
  0x3F6212, 0x075985, 0x9F1239, 0x854D0E, 0x3730A3, 0x115E59, 0x86198F, 0x78350F,
};

// Color presets: "Default" (the variable's color from varDefaultColors), then the hues of
// variables 1-16
static const char * colorPresetOptions = "Default\nBlue\nRed\nGreen\nOrange\nPurple\nCyan\nPink\nGray\n"
                                         "Lime\nSky\nRose\nYellow\nIndigo\nTeal\nFuchsia\nBrown";
#define COLOR_PRESET_DEFAULT  0

static const char * cursorNames[]  = {"V1", "V2", "H1", "H2"};
static const char * toolMap[]      = {"V1", "V2", "H1", "H2", "Zoom", ""};
static const char * plotNames[]    = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
static const char * plotCountOptions = "1\n2\n3\n4\n5\n6\n7\n8";
static const char * xAxisOptions     = "Samples\nSeconds\nClock time";

// Unit presets copied into the Unit text box ("None" clears it); any other unit can be typed
static const char * unitPresetOptions = "None\n\xC2\xB0" "C\n\xC2\xB0" "F\n%\nV\nmV\nA\nmA\nW\nHz\nrpm\ns\nms\n"
                                        "m\nmm\nkg\ng\nN\nPa\nbar";
#define UNIT_PRESET_NONE      0

// Number of samples shown by the plots
static const uint32_t plotLengths[]     = {100, 200, 500, 1000, 2000, 5000, 10000};
static const char *   plotLengthOptions = "100 points\n200 points\n500 points\n1000 points\n"
                                          "2000 points\n5000 points\n10000 points";

static const char * widthOptions     = "1 px\n2 px\n3 px\n4 px\n5 px\n6 px";

// Settings and data
static PlotVar_t vars[PLOTTER_MAX_VARS];
static Plot_t    plots[PLOT_COUNT_MAX];
static uint32_t  plotCount = 1;
static bool      paused;
static float     history[PLOTTER_MAX_VARS][PLOT_POINTS_MAX];   // raw values, NAN = no value
static uint32_t  historyTime[PLOT_POINTS_MAX];                  // lv_tick_get() of each sample
static uint32_t  historyHead;                                   // next written = oldest sample
static uint32_t  historyCount;                                  // samples kept since the last clear
static uint64_t  sampleTotal;                                   // samples received since start
static uint32_t  varSeenMask;                                   // bit v: variable v received
static uint32_t  plotLength = 200;                              // positions shown by the plots
static int32_t   pictureScale = 100;                            // percent, while laid out for a picture
static float     recordRow[PLOTTER_MAX_VARS];                   // displayed values of the test recorded
static int64_t   clockBaseMs;                                   // wall clock (Unix ms) at clockBaseTick:
static uint32_t  clockBaseTick;                                 // a sample's time of day from its tick
static bool      renderPending;                                 // new samples to draw
static lv_timer_t * renderTimerHandle;

// Serial connections: received lines of each one, number in use (splits the variables)
static LineParser_t parsers[PLOTTER_MAX_CONNS];
static bool         connLive[PLOTTER_MAX_CONNS];  // has sent data: its values are held in the other samples
static uint32_t     connCount = 1;

// Cursors (vertical: position, horizontal: displayed value) and zoom selection
static int32_t    vCursor[2] = {CURSOR_NONE, CURSOR_NONE};
static float      hCursor[2];
static bool       hCursorSet[2];
static int32_t    tool = TOOL_NONE;
static int32_t    selMode = SEL_NONE;
static uint32_t   selPlot;
static lv_point_t selStart;
static lv_point_t selEnd;

// Command marks (ring: cmdMarkHead is the next written), shown in the plots with cmdMarks on
static CmdMark_t  cmdMarks[CMD_MARKS_MAX];
static uint32_t   cmdMarkHead;
static uint32_t   cmdMarkCount;

// Window being moved / resized: its rectangle and the pointer when pressed
static lv_area_t  dragStartRect;
static lv_point_t dragStartPoint;

// Seam being dragged: windows whose right / left / bottom / top edge is on it (bit p: plot p),
// and the windows' rectangles when pressed
static uint16_t   seamLeftMask;
static uint16_t   seamRightMask;
static uint16_t   seamAboveMask;
static uint16_t   seamBelowMask;
static lv_area_t  seamStartRects[WIN_COUNT];
static bool       windowResizing;                 // corner handle being dragged
static bool       dividerDragging;                // chart / bars divider being dragged
static int32_t    dividerStartBars;               // bars' width when pressed
static int32_t    dividerStartTotal;              // chart + bars width when pressed
static bool       windowDragging;                 // window moved / resized or seam dragged
static bool       windowMoving;                   // window moved by its title bar

// Plot tab
static lv_obj_t * toolBtnm;
static lv_obj_t * pauseBtnLabel;
static lv_obj_t * exportBtn;
static lv_obj_t * toastLabel;                     // export result, over the bottom of the plots
static lv_timer_t * toastTimer;
static lv_obj_t * plotsBox;
static lv_obj_t * cmdPanel;

// Windows of the plot area (plot cells, commands panel): object, corner handle, geometry
// (x, y, w, h in 1/GEOM_FULL of the plot area)
static lv_obj_t * winObj[WIN_COUNT];
static lv_obj_t * winHandle[WIN_COUNT];
static int32_t    winGeom[WIN_COUNT][4];
static lv_obj_t * readoutLabel;

// Variable settings panel
static lv_obj_t *   varsPanel;
static lv_obj_t *   plotCountDropdown;
static lv_obj_t *   cmdPanelSwitch;
static lv_obj_t *   cmdMarksSwitch;
static lv_obj_t *   plotLengthDropdown;
static lv_obj_t *   chartSwitch;
static lv_obj_t *   barsSwitch;
static lv_obj_t *   plotSelDropdown;
static lv_obj_t *   yAutoSwitch;
static lv_obj_t *   yMinInput;
static lv_obj_t *   yMaxInput;
static uint32_t     editedPlot;
static lv_obj_t *   varDropdown;
static lv_obj_t *   nameInput;
static lv_obj_t *   unitInput;
static lv_obj_t *   unitPresetDropdown;
static uint32_t     varSelMap[PLOTTER_MAX_VARS];   // variable of each varDropdown option
static uint32_t     varSelCount;
static lv_obj_t *   plotNameInput;
static lv_obj_t *   xAxisDropdown;
static const char * showInDynMap[PLOT_COUNT_MAX + 2];   // "Show in" buttons of the shown plots
static lv_obj_t *   colorPresetDropdown;
static lv_obj_t *   gainInput;
static lv_obj_t *   offsetInput;
static lv_obj_t *   widthDropdown;
static lv_obj_t *   showInBtnm;
static lv_group_t * varsGroup;
static uint32_t     editedVar;
static bool         editorLoading;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void plotTabCreate(lv_obj_t * tab);
static void plotCreate(uint32_t p);
static lv_obj_t * switchCreate(lv_obj_t * row, const char * text);
static void widthFix(lv_obj_t * obj, int32_t width);
static void colorButtonUpdate(uint32_t color);
static void showInChecksSet(void);
static void varsTabCreate(lv_obj_t * tab);
static void varLoad(uint32_t v);
static void varSave(uint32_t v);
static void plotSettingsLoad(uint32_t p);
static void plotSettingsSave(uint32_t p);
static void plotTitleUpdate(uint32_t p);
static void plotSelRefresh(void);
static void plotEditorLoad(uint32_t p);

static void    lineParse(uint32_t conn, char * text, uint32_t tick);
static void    sampleAdd(uint32_t conn, const float * values, const bool * valid, uint32_t count, uint32_t tick);
static void    recordFeed(uint32_t conn, const float * values, const bool * valid, uint32_t count, uint32_t tick);
static uint32_t connVarCount(void);
static uint32_t varConn(uint32_t v);
static bool    varSeen(uint32_t v);
static float   displayValue(uint32_t v, float raw);
#ifdef SERIAL_DASHBOARD_CLI
static void    cliLatestUpdate(uint32_t conn, const float * values, const bool * valid, uint32_t count);
#endif
static bool    valueDrawable(float value);
static void    pointStore(int32_t * point, float value);
static float   pointLoad(const int32_t * point);
static void    yRangeLimit(Plot_t * plot);
static int     valueDecimals(const Plot_t * plot);
static void    yAxisWidthUpdate(Plot_t * plot);
static void    varNameGet(uint32_t v, char * buffer, uint32_t size);
static bool    varInPlot(uint32_t v, uint32_t p);
static uint32_t historyIndex(uint32_t position);
static uint32_t shownPosition(void);

static void plotsRender(void);
static void plotLengthApply(void);
static void visibilityUpdate(void);
static void barsLayoutUpdate(void);
static void plotsLayoutUpdate(void);
static void legendUpdate(void);
static void readoutUpdate(void);
static void plotsInvalidate(void);

static uint32_t barsCount(uint32_t p);
static int32_t  barSlotWidth(const Plot_t * plot, const lv_area_t * barsArea, uint32_t count);
static void     plotValueArea(const Plot_t * plot, lv_area_t * area);

static void     plotsTile(void);
static bool     windowShown(uint32_t w);
static void     geomKeyGet(uint32_t w, char * key, uint32_t size);
static bool     geomLoad(uint32_t w);
static void     geomSave(uint32_t w);
static void     windowRectGet(uint32_t w, lv_area_t * rect);
static void     windowRectSet(uint32_t w, const lv_area_t * rect);
static void     windowHandleCreate(uint32_t w);
static void     windowEventsAdd(uint32_t w, lv_obj_t * titleBar);
static void     windowsApply(void);
static bool     snapDelta(int32_t value, const int32_t * targets, uint32_t count, int32_t * delta);
static int32_t  snapPair(int32_t low, int32_t high, const int32_t * targets, uint32_t count);
static uint32_t snapTargetsGet(uint32_t w, bool horizontal, int32_t * targets);
static void     windowRaise(uint32_t w);
static bool     seamFind(lv_point_t point, bool vertical, uint16_t * lowMask, uint16_t * highMask);
static int32_t  seamShiftLimit(int32_t shift, bool vertical);
static Ticks_t yTicksGet(const Plot_t * plot, const lv_area_t * area);
static Ticks_t xTicksGet(const Plot_t * plot, const lv_area_t * area);
static int32_t positionToX(const Plot_t * plot, const lv_area_t * area, float position);
static int32_t xToPosition(const Plot_t * plot, const lv_area_t * area, int32_t x);
static int32_t valueToY(const Plot_t * plot, const lv_area_t * area, float value);
static float   yToValue(const Plot_t * plot, const lv_area_t * area, int32_t y);

static float   tickSnap(const Ticks_t * ticks, float value, float min, float max);
static float   xToPositionF(const Plot_t * plot, const lv_area_t * area, int32_t x);
static bool    axisSelectionGet(float * low, float * high);

static void selectionHandle(lv_event_t * e, int32_t mode, uint32_t p);
static void selectionApply(void);
static void zoomReset(int32_t plotIndex);

static void editorLoad(uint32_t v);
static void varDropdownRefresh(void);
static bool gainParse(const char * text, float * gain);
static bool numberParse(const char * text, float * value);
static void inputValidSet(lv_obj_t * input, bool valid);

static void chartGridDrawEvent(lv_event_t * e);
static void chartDrawTaskEvent(lv_event_t * e);
static void chartDrawPostEvent(lv_event_t * e);
static void chartPressEvent(lv_event_t * e);
static void yAxisDrawEvent(lv_event_t * e);
static void yAxisPressEvent(lv_event_t * e);
static void xAxisDrawEvent(lv_event_t * e);
static void xAxisPressEvent(lv_event_t * e);
static bool positionTick(uint32_t position, uint32_t * tick);
static int  positionDecimals(const Plot_t * plot);
static void positionLabel(uint32_t mode, uint32_t position, int decimals, char * text, size_t size);
static void xAxisModeEvent(lv_event_t * e);
static void barsDrawEvent(lv_event_t * e);
static void plotsBoxSizeEvent(lv_event_t * e);
static void windowMoveEvent(lv_event_t * e);
static void windowResizeEvent(lv_event_t * e);
static void seamEvent(lv_event_t * e);
static void dividerEvent(lv_event_t * e);
static void dividerDrawEvent(lv_event_t * e);
static void resizeHandleDrawEvent(lv_event_t * e);
static void tileEvent(lv_event_t * e);
static void chartSwitchEvent(lv_event_t * e);
static void cmdPanelSwitchEvent(lv_event_t * e);
static void cmdMarksSwitchEvent(lv_event_t * e);
static void cmdPanelCloseEvent(lv_event_t * e);
static void toolEvent(lv_event_t * e);
static void cursorsClearEvent(lv_event_t * e);
static void zoomResetEvent(lv_event_t * e);
static void varsOpenEvent(lv_event_t * e);
static void varsCloseEvent(lv_event_t * e);
static void pauseEvent(lv_event_t * e);
static void clearEvent(lv_event_t * e);
static void exportEvent(lv_event_t * e);
static void screenshotEvent(lv_event_t * e);
static void pictureStylesSet(const lv_font_t * font);
static void pictureLayoutSet(bool picture);
static void exportPollTimer(lv_timer_t * tim);
static void toastShow(const char * text);
static void toastHideTimer(lv_timer_t * tim);
static void toastClickEvent(lv_event_t * e);
static void plotCountEvent(lv_event_t * e);
static void plotLengthEvent(lv_event_t * e);
static void renderTimer(lv_timer_t * tim);
static void barsSwitchEvent(lv_event_t * e);
static void plotSelEvent(lv_event_t * e);
static void plotNameEvent(lv_event_t * e);
static void yScaleEvent(lv_event_t * e);
static void varSelectedEvent(lv_event_t * e);
static void nameEvent(lv_event_t * e);
static void unitEvent(lv_event_t * e);
static void unitPresetEvent(lv_event_t * e);
static void colorPresetEvent(lv_event_t * e);
static bool colorParse(const char * text, uint32_t * color);
static void colorApply(uint32_t v);
static void gainEvent(lv_event_t * e);
static void offsetEvent(lv_event_t * e);
static void widthEvent(lv_event_t * e);
static void showInEvent(lv_event_t * e);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void plotterInit(lv_obj_t * plotTab)
{
  lv_obj_t * varsContent;

  // To the ms (time() is to the second)
  clockBaseMs   = appWindowClockMs();
  clockBaseTick = lv_tick_get();

  for (uint32_t c = 0; c < PLOTTER_MAX_CONNS; ++c)
  {
    lineParserInit(&parsers[c]);
  }

  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    varLoad(v);

    for (uint32_t i = 0; i < PLOT_POINTS_MAX; ++i)
    {
      history[v][i] = NAN;
    }
  }

  plotTabCreate(plotTab);

  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    plotSettingsLoad(p);
  }

  // Variable settings: window over the plots (about half of the plot area, centered), opened
  // with the toolbar's settings button. Its content scrolls when it does not fit.
  varsPanel = uiOverlayCreate(plotTab, "Variables", varsCloseEvent, &varsContent);
  lv_obj_set_size(varsPanel, LV_PCT(50), LV_PCT(50));
  // Minimum readable size; fits in the plot area of the smallest window (320 x 240 x scale)
  lv_obj_set_style_min_width(varsPanel, uiPx(280), 0);
  lv_obj_set_style_min_height(varsPanel, uiPx(160), 0);
  lv_obj_center(varsPanel);
  lv_obj_set_style_radius(varsPanel, uiPx(8), 0);
  lv_obj_set_style_border_width(varsPanel, LV_MAX(1, uiPx(1)), 0);
  lv_obj_set_style_border_color(varsPanel, lv_color_hex(COLOR_INFO), 0);
  lv_obj_set_style_shadow_width(varsPanel, uiPx(20), 0);
  lv_obj_set_style_shadow_opa(varsPanel, LV_OPA_30, 0);
  lv_obj_set_style_clip_corner(varsPanel, true, 0);
  varsTabCreate(varsContent);

  // Windows where they were in the last session; tiled when a shown one has no saved geometry
  bool geomValid = true;

  for (uint32_t w = 0; w < WIN_COUNT; ++w)
  {
    // Commands window by default: column at the right
    winGeom[w][0] = (w == WIN_CMD) ? (GEOM_FULL - CMD_TILE_WIDTH) : 0;
    winGeom[w][1] = 0;
    winGeom[w][2] = (w == WIN_CMD) ? CMD_TILE_WIDTH : GEOM_FULL;
    winGeom[w][3] = GEOM_FULL;
    geomValid = (geomLoad(w) || !windowShown(w)) && geomValid;
  }

  if (!geomValid)
  {
    plotsTile();
  }

  plotsLayoutUpdate();
  renderTimerHandle = lv_timer_create(renderTimer, PLOT_RENDER_MS, NULL);
}

void plotterSetRenderPeriod(uint32_t ms)
{
  // Redrawing every plot for new data is the heaviest drawing: limited even when the UI runs faster
  if (renderTimerHandle != NULL)
  {
    lv_timer_set_period(renderTimerHandle, LV_MAX(ms, PLOT_RENDER_MIN_MS));
  }
}

// Splits the received bytes into lines; each complete line is parsed for numbers
void plotterFeed(uint32_t conn, const uint8_t * data, uint32_t len, uint32_t tick)
{
  if (conn >= connCount)
  {
    return;
  }

  uint32_t now = lv_tick_get();

  PROF_PUSH(PROF_PARSE);

  for (uint32_t i = 0; i < len; ++i)
  {
    char * text = lineParserByte(&parsers[conn], (char) data[i], now);

    // A reply with the device's settings is not data, even with numbers in it
    if ((text != NULL) && !compareLineCapture(conn, text))
    {
      lineParse(conn, text, tick);
    }
  }

  PROF_POP();
}

// Connection opened / closed: its partial line is dropped, and its values are no longer held in
// the samples of the other connections until it sends data
void plotterResetLine(uint32_t conn)
{
  if (conn < PLOTTER_MAX_CONNS)
  {
    lineParserReset(&parsers[conn]);
    connLive[conn] = false;
  }
}

// A command was sent: its echo is removed from the next lines (line_parser.c), and it is
// marked on the plots (with several connections, its label starts with the connection)
void plotterSent(uint32_t conn, const char * text, uint32_t markColor)
{
  if (conn < PLOTTER_MAX_CONNS)
  {
    lineParserSent(&parsers[conn], text, lv_tick_get());

    CmdMark_t * mark = &cmdMarks[cmdMarkHead];

    mark->sample = sampleTotal;
    mark->color  = markColor;

    if (connCount > 1)
    {
      snprintf(mark->text, sizeof(mark->text), "C%u %s", (unsigned) (conn + 1), text);
    }
    else
    {
      snprintf(mark->text, sizeof(mark->text), "%s", text);
    }

    cmdMarkHead  = (cmdMarkHead + 1) % CMD_MARKS_MAX;
    cmdMarkCount = LV_MIN(cmdMarkCount + 1, CMD_MARKS_MAX);

    for (uint32_t p = 0; p < plotCount; ++p)
    {
      if (plots[p].cmdMarks)
      {
        lv_obj_invalidate(plots[p].chart);
      }
    }
  }
}

// The variables are split again: the plotted data is cleared (it belonged to the former split)
void plotterSetConnCount(uint32_t count)
{
  connCount = LV_CLAMP(1, count, PLOTTER_MAX_CONNS);

  for (uint32_t c = 0; c < PLOTTER_MAX_CONNS; ++c)
  {
    plotterResetLine(c);
  }

  // Default names ("C2 V1") follow the split (plotterClear refreshes the variable list)
  plotterClear();
}

uint32_t plotterConnVarCount(void)
{
  return connVarCount();
}

// Removes the plotted data; the variable settings, cursors and zoom are kept
void plotterClear(void)
{
  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    for (uint32_t i = 0; i < PLOT_POINTS_MAX; ++i)
    {
      history[v][i] = NAN;
    }
  }

  historyCount = 0;
  varSeenMask  = 0;
  cmdMarkHead  = 0;
  cmdMarkCount = 0;

  for (uint32_t c = 0; c < PLOTTER_MAX_CONNS; ++c)
  {
    lineParserClear(&parsers[c]);
    connLive[c] = false;
  }

  // No variable received: the settings list the connections' variables (and their default names)
  visibilityUpdate();
  barsLayoutUpdate();
  varDropdownRefresh();
  plotsRender();
}

void plotterSetLineFormat(uint32_t conn, const char * separators, bool skipText, uint32_t varCount)
{
  if (conn < PLOTTER_MAX_CONNS)
  {
    lineParserSetFormat(&parsers[conn], separators, skipText, LV_MIN(varCount, PLOTTER_MAX_VARS));
  }
}

// Text boxes of the variable settings while their panel is open, otherwise the commands panel's
lv_group_t * plotterKeypadGroup(void)
{
  if (!lv_obj_is_hidden(varsPanel))
  {
    return varsGroup;
  }

  return lv_obj_is_hidden(cmdPanel) ? NULL : cmdPanelGroup();
}

// Right click at point (screen coordinates): resets the zoom of the plot window under it (the
// top one where windows overlap)
void plotterRightClick(lv_point_t point)
{
  int32_t found = ZOOM_ALL;
  int32_t foundIndex = -1;

  if (!lv_obj_is_hidden(varsPanel))
  {
    return;
  }

  for (uint32_t w = 0; w < WIN_COUNT; ++w)
  {
    lv_area_t area;

    if (!windowShown(w))
    {
      continue;
    }

    lv_obj_get_coords(winObj[w], &area);

    if ((point.x >= area.x1) && (point.x <= area.x2) && (point.y >= area.y1) && (point.y <= area.y2) &&
        (lv_obj_get_index(winObj[w]) > foundIndex))
    {
      found = (int32_t) w;
      foundIndex = lv_obj_get_index(winObj[w]);
    }
  }

  // The commands window above: no plot clicked
  if ((found != ZOOM_ALL) && (found != WIN_CMD))
  {
    zoomReset(found);
  }
}

// Variables received, with their column names ("Name [unit]")
uint32_t plotterVarHeaders(uint32_t * columns, char headers[][TEST_HEADER_LEN])
{
  uint32_t count = 0;

  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    if (varSeen(v))
    {
      char name[PLOT_NAME_LEN + 8];

      varNameGet(v, name, sizeof(name));

      if (vars[v].unit[0] != '\0')
      {
        snprintf(headers[count], TEST_HEADER_LEN, "%s [%s]", name, vars[v].unit);
      }
      else
      {
        snprintf(headers[count], TEST_HEADER_LEN, "%s", name);
      }

      columns[count++] = v;
    }
  }

  return count;
}

uint32_t plotterRecordStart(uint32_t * columns, char headers[][TEST_HEADER_LEN])
{
  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    recordRow[v] = NAN;
  }

  return plotterVarHeaders(columns, headers);
}

// The kept samples (up to PLOT_POINTS_MAX, since the last clear) of the variables received, as
// displayed; t = 0 is the first one
TestData_t * plotterCapture(void)
{
  static char headers[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  uint32_t    columns[PLOTTER_MAX_VARS];
  uint32_t    columnCount = plotterVarHeaders(columns, headers);
  TestData_t * test = ((historyCount > 0) && (columnCount > 0)) ? testDataCreate(columnCount) : NULL;

  if (test == NULL)
  {
    return NULL;
  }

  uint32_t oldest    = (historyHead + PLOT_POINTS_MAX - historyCount) % PLOT_POINTS_MAX;
  uint32_t firstTick = historyTime[oldest];

  memcpy(test->headers, headers, sizeof(headers[0]) * columnCount);
  test->startMs = clockBaseMs + (int32_t) (firstTick - clockBaseTick);

  for (uint32_t s = 0; s < historyCount; ++s)
  {
    uint32_t index = (oldest + s) % PLOT_POINTS_MAX;
    float    row[PLOTTER_MAX_VARS];

    for (uint32_t c = 0; c < columnCount; ++c)
    {
      float raw = history[columns[c]][index];
      row[c] = isnan(raw) ? NAN : displayValue(columns[c], raw);
    }

    testDataAppend(test, historyTime[index] - firstTick, row);
  }

  return test;
}

// Same targets as the presses: a corner handle, or a seam where the press lands on the plot area
// (gap) or on a window's border. While dragging, the drag's cursor until released.
AppCursor_t plotterCursorAt(lv_point_t point)
{
  bool    vertical;
  bool    horizontal;
  uint16_t lowMask;
  uint16_t highMask;

  if (windowResizing)
  {
    return APP_CURSOR_SIZE_NWSE;
  }

  if (dividerDragging)
  {
    return APP_CURSOR_SIZE_WE;
  }

  // A window moved by its title bar / a command row dragged: closed hand wherever the pointer goes
  if (windowMoving || (cmdPanelCursor(NULL) == APP_CURSOR_GRABBING))
  {
    return APP_CURSOR_GRABBING;
  }

  vertical   = (seamLeftMask | seamRightMask) != 0;
  horizontal = (seamAboveMask | seamBelowMask) != 0;

  if (!vertical && !horizontal)
  {
    lv_obj_t * hit = lv_indev_search_obj(lv_screen_active(), &point);
    bool       onSeamArea = (hit == plotsBox);

    if (!lv_obj_is_hidden(varsPanel) || (hit == NULL))
    {
      return APP_CURSOR_ARROW;
    }

    // Command rows are reordered by dragging their handle, windows moved by their title bar
    // (legend / title labels are not clickable: the hit is the bar itself)
    if ((cmdPanelCursor(hit) == APP_CURSOR_GRAB) || (hit == cmdPanelTitleBar()))
    {
      return APP_CURSOR_GRAB;
    }

    for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
    {
      if (hit == plots[p].header)
      {
        return APP_CURSOR_GRAB;
      }
    }

    for (uint32_t w = 0; w < WIN_COUNT; ++w)
    {
      if (hit == winHandle[w])
      {
        return APP_CURSOR_SIZE_NWSE;
      }

      if ((w < PLOT_COUNT_MAX) && (hit == plots[w].divider))
      {
        return APP_CURSOR_SIZE_WE;
      }

      onSeamArea = onSeamArea || (hit == winObj[w]);
    }

    if (!onSeamArea)
    {
      return APP_CURSOR_ARROW;
    }

    lv_area_t box;
    lv_obj_get_content_coords(plotsBox, &box);
    lv_point_t local = {point.x - box.x1, point.y - box.y1};

    vertical   = seamFind(local, true, &lowMask, &highMask);
    horizontal = seamFind(local, false, &lowMask, &highMask);
  }

  if (vertical && horizontal)
  {
    return APP_CURSOR_SIZE_ALL;
  }

  return vertical ? APP_CURSOR_SIZE_WE : (horizontal ? APP_CURSOR_SIZE_NS : APP_CURSOR_ARROW);
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

static void plotTabCreate(lv_obj_t * tab)
{
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, uiPx(4), 0);
  lv_obj_set_style_pad_row(tab, uiPx(4), 0);
  lv_obj_set_scrollable(tab, false);

  // Tool used when dragging on a plot, clear cursors, reset zoom, pause, clear data
  lv_obj_t * row = uiRowCreate(tab);

  toolBtnm = lv_buttonmatrix_create(row);
  lv_buttonmatrix_set_map(toolBtnm, toolMap);
  // CLICK_TRIG: VALUE_CHANGED on release, after the checked state is toggled (otherwise it
  // is sent on press and the handler would read the state before the click)
  lv_buttonmatrix_set_button_ctrl_all(toolBtnm, LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
  lv_buttonmatrix_set_button_width(toolBtnm, TOOL_ZOOM, 2);
  lv_buttonmatrix_set_one_checked(toolBtnm, true);
  lv_obj_set_flex_grow(toolBtnm, 1);
  lv_obj_set_height(toolBtnm, uiPx(30));
  lv_obj_set_style_pad_all(toolBtnm, uiPx(2), 0);
  lv_obj_set_style_pad_gap(toolBtnm, uiPx(2), 0);
  lv_obj_add_event_cb(toolBtnm, toolEvent, LV_EVENT_VALUE_CHANGED, NULL);

  uiTooltipSet(toolBtnm, tr("Drag on a plot: V1 / V2 vertical cursors, H1 / H2 horizontal cursors, Zoom: zoom box"));
  uiTooltipSet(uiButtonCreate(row, LV_SYMBOL_CLOSE, cursorsClearEvent), tr("Remove the cursors"));
  uiTooltipSet(uiButtonCreate(row, LV_SYMBOL_HOME, zoomResetEvent),
               tr("Reset the zoom of all plots (right-click a plot: that plot only)"));
  lv_obj_t * pauseBtn = uiButtonCreate(row, LV_SYMBOL_PAUSE, pauseEvent);
  pauseBtnLabel = lv_obj_get_child(pauseBtn, 0);
  uiTooltipSet(pauseBtn, tr("Pause / resume the plots"));
  uiTooltipSet(uiButtonCreate(row, LV_SYMBOL_TRASH, clearEvent), tr("Clear the plotted data"));
  exportBtn = uiButtonCreate(row, LV_SYMBOL_DOWNLOAD, exportEvent);
  uiTooltipSet(exportBtn, tr("Export the data to a CSV file"));
  uiTooltipSet(uiButtonCreate(row, LV_SYMBOL_IMAGE, screenshotEvent), tr("Save a picture of the plots in the test bench folder"));
  uiTooltipSet(uiButtonCreate(row, LV_SYMBOL_SETTINGS, varsOpenEvent), tr("Plot and variable settings"));

  // Area of the windows: plots and commands (placed from their geometry, also when the area is
  // resized)
  plotsBox = lv_obj_create(tab);
  lv_obj_remove_style_all(plotsBox);
  lv_obj_set_width(plotsBox, LV_PCT(100));
  lv_obj_set_flex_grow(plotsBox, 1);
  lv_obj_set_scrollable(plotsBox, false);
  lv_obj_add_event_cb(plotsBox, plotsBoxSizeEvent, LV_EVENT_SIZE_CHANGED, NULL);

  // Pressing in the gaps between windows: seams
  lv_obj_set_clickable(plotsBox, true);
  lv_obj_add_event_cb(plotsBox, seamEvent, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(plotsBox, seamEvent, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(plotsBox, seamEvent, LV_EVENT_RELEASED, NULL);

  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    plotCreate(p);
  }

  // Commands window (shown with the Commands switch): moved, resized and snapped like the plots
  cmdPanel = cmdPanelCreate(plotsBox, cmdPanelCloseEvent);
  lv_obj_set_hidden(cmdPanel, true);
  winObj[WIN_CMD] = cmdPanel;
  windowEventsAdd(WIN_CMD, cmdPanelTitleBar());
  windowHandleCreate(WIN_CMD);
  uiTooltipSet(cmdPanelTitleBar(), tr("Drag to move"));

  // Cursor positions and differences
  readoutLabel = lv_label_create(tab);
  lv_obj_set_width(readoutLabel, LV_PCT(100));
  lv_label_set_long_mode(readoutLabel, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_color(readoutLabel, lv_color_hex(COLOR_INFO), 0);
  lv_obj_set_hidden(readoutLabel, true);
}

// One plot window in plotsBox: title bar (name + legend), Y axis + chart + bars, X axis, and the
// resize handle in the bottom right corner
static void plotCreate(uint32_t p)
{
  Plot_t * plot = &plots[p];
  void *   userData = (void *) (uintptr_t) p;

  // Window: theme card (background, border, radius), positioned by windowsApply
  plot->cell = lv_obj_create(plotsBox);
  lv_obj_set_style_pad_all(plot->cell, uiPx(3), 0);
  lv_obj_set_style_pad_row(plot->cell, uiPx(2), 0);
  lv_obj_set_flex_flow(plot->cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollable(plot->cell, false);
  winObj[p] = plot->cell;

  // Title bar: drag to move the window (its border: seams)
  plot->header = uiRowCreate(plot->cell);
  lv_obj_set_flex_align(plot->header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_hor(plot->header, uiPx(4), 0);
  windowEventsAdd(p, plot->header);

  // "P1", with the plot's name once its settings are loaded (plotTitleUpdate)
  plot->title = lv_label_create(plot->header);
  lv_label_set_text_fmt(plot->title, "P%u", (unsigned) (p + 1));

  // Legend of this plot (labels shown / filled by legendUpdate); not clickable, so pressing it
  // drags the title bar
  plot->legend = lv_obj_create(plot->header);
  lv_obj_remove_style_all(plot->legend);
  lv_obj_set_height(plot->legend, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(plot->legend, 1);
  lv_obj_set_flex_flow(plot->legend, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(plot->legend, uiPx(10), 0);
  lv_obj_set_style_pad_row(plot->legend, uiPx(2), 0);
  lv_obj_set_scrollable(plot->legend, false);
  lv_obj_set_clickable(plot->legend, false);

  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    plot->legendLabels[v] = lv_label_create(plot->legend);
    uiLegendDotAdd(plot->legendLabels[v]);
    lv_obj_set_style_text_color(plot->legendLabels[v], lv_color_hex(vars[v].color), 0);
    lv_obj_set_hidden(plot->legendLabels[v], true);
  }

  plot->row = uiRowCreate(plot->cell);
  lv_obj_set_flex_grow(plot->row, 1);
  lv_obj_set_style_pad_column(plot->row, uiPx(2), 0);

  // Labels of the gridlines; dragging on it zooms Y
  plot->yAxis = lv_obj_create(plot->row);
  lv_obj_remove_style_all(plot->yAxis);
  lv_obj_set_size(plot->yAxis, AXIS_Y_WIDTH, LV_PCT(100));
  lv_obj_set_scrollable(plot->yAxis, false);
  lv_obj_set_clickable(plot->yAxis, true);
  lv_obj_add_event_cb(plot->yAxis, yAxisDrawEvent, LV_EVENT_DRAW_MAIN, userData);
  lv_obj_add_event_cb(plot->yAxis, yAxisPressEvent, LV_EVENT_PRESSED, userData);
  lv_obj_add_event_cb(plot->yAxis, yAxisPressEvent, LV_EVENT_PRESSING, userData);
  lv_obj_add_event_cb(plot->yAxis, yAxisPressEvent, LV_EVENT_RELEASED, userData);

  // The background is drawn with the grid (chartGridDrawEvent), below the lines
  plot->chart = lv_chart_create(plot->row);
  lv_obj_set_height(plot->chart, LV_PCT(100));
  lv_obj_set_flex_grow(plot->chart, 1);
  lv_obj_set_style_pad_all(plot->chart, uiPx(4), 0);
  lv_obj_set_style_bg_opa(plot->chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_size(plot->chart, 0, 0, LV_PART_INDICATOR);   // lines only, no point dots
  lv_obj_set_style_line_width(plot->chart, uiPx(LINE_WIDTH_MAX), LV_PART_ITEMS);
  lv_obj_set_scrollable(plot->chart, false);
  lv_obj_set_clickable(plot->chart, true);
  lv_obj_set_send_draw_task_events(plot->chart, true);
  lv_chart_set_type(plot->chart, LV_CHART_TYPE_LINE);
  lv_chart_set_update_mode(plot->chart, LV_CHART_UPDATE_MODE_CIRCULAR);
  lv_chart_set_point_count(plot->chart, plotLength);
  lv_chart_set_div_line_count(plot->chart, 0, 0);

  plot->yMin = -1.0f;
  plot->yMax = 1.0f;
  lv_chart_set_axis_range(plot->chart, LV_CHART_AXIS_PRIMARY_Y, 0, UI_CHART_RES);

  lv_obj_add_event_cb(plot->chart, chartGridDrawEvent, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, userData);
  lv_obj_add_event_cb(plot->chart, chartDrawTaskEvent, LV_EVENT_DRAW_TASK_ADDED, userData);
  lv_obj_add_event_cb(plot->chart, chartDrawPostEvent, LV_EVENT_DRAW_POST, userData);
  lv_obj_add_event_cb(plot->chart, chartPressEvent, LV_EVENT_PRESSED, userData);
  lv_obj_add_event_cb(plot->chart, chartPressEvent, LV_EVENT_PRESSING, userData);
  lv_obj_add_event_cb(plot->chart, chartPressEvent, LV_EVENT_RELEASED, userData);

  // Text set by visibilityUpdate, centered in the window (also without chart)
  plot->hint = lv_label_create(plot->cell);
  lv_obj_set_floating(plot->hint, true);
  lv_obj_set_width(plot->hint, LV_PCT(100));
  lv_label_set_long_mode(plot->hint, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_center(plot->hint);
  lv_obj_set_style_text_color(plot->hint, lv_color_hex(COLOR_INFO), 0);
  lv_obj_set_style_text_align(plot->hint, LV_TEXT_ALIGN_CENTER, 0);

  // Series index = variable index (the draw task events rely on it)
  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    plot->series[v] = lv_chart_add_series(plot->chart, lv_color_hex(vars[v].color), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(plot->chart, plot->series[v], LV_CHART_POINT_NONE);
  }

  // Between chart and bars: drag to change their widths
  plot->divider = lv_obj_create(plot->row);
  lv_obj_remove_style_all(plot->divider);
  lv_obj_set_size(plot->divider, DIVIDER_WIDTH, LV_PCT(100));
  lv_obj_set_clickable(plot->divider, true);
  lv_obj_add_event_cb(plot->divider, dividerDrawEvent, LV_EVENT_DRAW_MAIN, NULL);
  lv_obj_add_event_cb(plot->divider, dividerEvent, LV_EVENT_PRESSED, userData);
  lv_obj_add_event_cb(plot->divider, dividerEvent, LV_EVENT_PRESSING, userData);
  lv_obj_add_event_cb(plot->divider, dividerEvent, LV_EVENT_RELEASED, userData);

  // Bars are drawn in barsDrawEvent, on the plot's Y scale (see plotValueArea)
  plot->bars = lv_obj_create(plot->row);
  lv_obj_remove_style_all(plot->bars);
  lv_obj_set_height(plot->bars, LV_PCT(100));
  lv_obj_add_event_cb(plot->bars, barsDrawEvent, LV_EVENT_DRAW_MAIN, userData);

  // Sample positions below the chart (dragging on it zooms X) and names below the bars
  plot->xAxis = lv_obj_create(plot->cell);
  lv_obj_remove_style_all(plot->xAxis);
  lv_obj_set_size(plot->xAxis, LV_PCT(100), AXIS_X_HEIGHT);
  lv_obj_set_scrollable(plot->xAxis, false);
  lv_obj_set_clickable(plot->xAxis, true);
  lv_obj_add_event_cb(plot->xAxis, xAxisDrawEvent, LV_EVENT_DRAW_MAIN, userData);
  lv_obj_add_event_cb(plot->xAxis, xAxisPressEvent, LV_EVENT_PRESSED, userData);
  lv_obj_add_event_cb(plot->xAxis, xAxisPressEvent, LV_EVENT_PRESSING, userData);
  lv_obj_add_event_cb(plot->xAxis, xAxisPressEvent, LV_EVENT_RELEASED, userData);

  // Resize handle, above the rest of the window
  windowHandleCreate(p);

  uiTooltipSet(plot->header, tr("Drag to move. Right-click the plot to reset its zoom."));
  uiTooltipSet(plot->yAxis, tr("Drag along the axis to zoom"));
  uiTooltipSet(plot->xAxis, tr("Drag along the axis to zoom"));
  uiTooltipSet(plot->divider, tr("Drag to share the width between the chart and the bars"));
}

// Fixed width instead of filling the row
static void widthFix(lv_obj_t * obj, int32_t width)
{
  lv_obj_set_flex_grow(obj, 0);
  lv_obj_set_width(obj, width);
}

// "[switch] text" in a row
static lv_obj_t * switchCreate(lv_obj_t * row, const char * text)
{
  lv_obj_t * sw = lv_switch_create(row);
  lv_obj_t * label = lv_label_create(row);
  lv_label_set_text(label, text);
  lv_obj_set_style_pad_right(label, uiPx(10), 0);
  return sw;
}

// Three boxes, each saying what it applies to: all the plots, the plot chosen in its header,
// the variable chosen in its header
static void varsTabCreate(lv_obj_t * tab)
{
  lv_obj_t * header;
  lv_obj_t * box;
  lv_obj_t * row;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, uiPx(6), 0);
  lv_obj_set_style_pad_row(tab, uiPx(8), 0);

  // All plots: how many, samples shown, commands window
  box = uiBoxCreate(tab, tr("All plots"), &header);

  row = uiSettingRowCreate(box, tr("Plots"));
  plotCountDropdown = uiDropdownCreate(row, plotCountOptions, 0);
  widthFix(plotCountDropdown, SETTINGS_NUMBER_WIDTH);
  settingsBindDropdown(plotCountDropdown, "plotCount");
  plotCount = lv_dropdown_get_selected(plotCountDropdown) + 1;
  lv_obj_add_event_cb(plotCountDropdown, plotCountEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // Back to the default arrangement of the plot windows
  uiTooltipSet(uiButtonCreate(row, tr("Tile"), tileEvent), tr("Arrange the windows side by side"));

  row = uiSettingRowCreate(box, tr("Length"));
  plotLengthDropdown = uiDropdownCreate(row, plotLengthOptions, PLOT_LENGTH_DEFAULT);
  widthFix(plotLengthDropdown, SETTINGS_LENGTH_WIDTH);
  uiTooltipSet(plotLengthDropdown, tr("Samples shown by the plots"));
  settingsBindDropdown(plotLengthDropdown, "plotLength");
  plotLengthApply();
  lv_obj_add_event_cb(plotLengthDropdown, plotLengthEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // Panel of saved commands at the right of the plots
  row = uiSettingRowCreate(box, tr("Commands"));
  cmdPanelSwitch = switchCreate(row, tr("Commands window"));
  uiTooltipSet(cmdPanelSwitch, tr("Show the Commands window"));
  settingsBindSwitch(cmdPanelSwitch, "cmdPanel");
  lv_obj_set_hidden(cmdPanel, !lv_obj_has_state(cmdPanelSwitch, LV_STATE_CHECKED));
  lv_obj_add_event_cb(cmdPanelSwitch, cmdPanelSwitchEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // The plot chosen in the header: what it shows, its Y scale (auto: follows the data)
  box = uiBoxCreate(tab, tr("Plot"), &header);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(header, uiPx(6), 0);

  // Options set by plotSelRefresh; the button shows the number only
  plotSelDropdown = uiDropdownCreate(header, "", 0);
  widthFix(plotSelDropdown, SETTINGS_NUMBER_WIDTH);
  lv_obj_add_event_cb(plotSelDropdown, plotSelEvent, LV_EVENT_VALUE_CHANGED, NULL);

  plotNameInput = uiTextareaCreate(header, tr("Name"));
  widthFix(plotNameInput, SETTINGS_NAME_WIDTH);
  lv_textarea_set_max_length(plotNameInput, PLOT_NAME_LEN - 1);
  lv_obj_add_event_cb(plotNameInput, plotNameEvent, LV_EVENT_VALUE_CHANGED, NULL);
  uiTooltipSet(plotNameInput, tr("Name"));

  row = uiSettingRowCreate(box, tr("Show"));
  chartSwitch = switchCreate(row, tr("Chart"));
  uiTooltipSet(chartSwitch, tr("Line chart (off: bar graph only)"));
  lv_obj_add_event_cb(chartSwitch, chartSwitchEvent, LV_EVENT_VALUE_CHANGED, NULL);
  barsSwitch = switchCreate(row, tr("Bars"));
  uiTooltipSet(barsSwitch, tr("Bar graph of the current values"));
  lv_obj_add_event_cb(barsSwitch, barsSwitchEvent, LV_EVENT_VALUE_CHANGED, NULL);
  // Vertical line where each command was sent, labeled with the command
  cmdMarksSwitch = switchCreate(row, tr("Marks"));
  uiTooltipSet(cmdMarksSwitch, tr("Line on the plot when a command is sent, with the command"));
  lv_obj_add_event_cb(cmdMarksSwitch, cmdMarksSwitchEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // X axis labels of this plot: samples, seconds or time of day
  row = uiSettingRowCreate(box, tr("X axis"));
  xAxisDropdown = uiDropdownCreate(row, tr(xAxisOptions), X_AXIS_SAMPLES);
  widthFix(xAxisDropdown, SETTINGS_LENGTH_WIDTH);
  uiTooltipSet(xAxisDropdown, tr("Sample number, seconds before the latest sample, or time of day"));
  lv_obj_add_event_cb(xAxisDropdown, xAxisModeEvent, LV_EVENT_VALUE_CHANGED, NULL);

  row = uiSettingRowCreate(box, tr("Y scale"));
  yAutoSwitch = switchCreate(row, tr("Auto"));
  uiTooltipSet(yAutoSwitch, tr("Y scale follows the data (off: Y min / Y max)"));
  lv_obj_add_event_cb(yAutoSwitch, yScaleEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // Used when Auto is off (greyed out otherwise)
  row = uiSettingRowCreate(box, tr("Min / max"));
  yMinInput = uiTextareaCreate(row, tr("ex: -10"));
  widthFix(yMinInput, SETTINGS_VALUE_WIDTH);
  lv_textarea_set_accepted_chars(yMinInput, "0123456789.-");
  lv_textarea_set_max_length(yMinInput, PLOT_NUMBER_LEN - 1);
  lv_obj_add_event_cb(yMinInput, yScaleEvent, LV_EVENT_VALUE_CHANGED, NULL);

  lv_label_set_text(lv_label_create(row), "-");

  yMaxInput = uiTextareaCreate(row, tr("ex: 100"));
  widthFix(yMaxInput, SETTINGS_VALUE_WIDTH);
  lv_textarea_set_accepted_chars(yMaxInput, "0123456789.-");
  lv_textarea_set_max_length(yMaxInput, PLOT_NUMBER_LEN - 1);
  lv_obj_add_event_cb(yMaxInput, yScaleEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // The variable chosen in the header (variables are numbered by column of the received lines),
  // with its name, unit and color on the same line (wrapped when the panel is narrow). The
  // number button shows the number only; its list shows the names.
  box = uiBoxCreate(tab, tr("Variable"), &header);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(header, uiPx(6), 0);

  varDropdown = lv_dropdown_create(header);
  lv_obj_add_style(varDropdown, uiStyleCompact(), 0);
  widthFix(varDropdown, SETTINGS_NUMBER_WIDTH);
  lv_obj_add_event_cb(varDropdown, varSelectedEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // Placeholder: the default name (editorLoad)
  nameInput = uiTextareaCreate(header, "");
  widthFix(nameInput, SETTINGS_NAME_WIDTH);
  lv_textarea_set_max_length(nameInput, PLOT_NAME_LEN - 1);
  lv_obj_add_event_cb(nameInput, nameEvent, LV_EVENT_VALUE_CHANGED, NULL);
  uiTooltipSet(nameInput, tr("Name"));

  unitInput = uiTextareaCreate(header, tr("Unit"));
  widthFix(unitInput, SETTINGS_UNIT_WIDTH);
  lv_textarea_set_max_length(unitInput, PLOT_UNIT_LEN - 1);
  lv_obj_add_event_cb(unitInput, unitEvent, LV_EVENT_VALUE_CHANGED, NULL);
  uiTooltipSet(unitInput, tr("Unit"));

  // Only shows the arrow; picking a preset writes it in unitInput
  unitPresetDropdown = uiPresetDropdownCreate(header, tr(unitPresetOptions), unitPresetEvent);
  uiTooltipSet(unitPresetDropdown, tr("Common units"));

  // Color: button filled with the variable's color, its list gives the preset colors
  colorPresetDropdown = uiPresetDropdownCreate(header, tr(colorPresetOptions), colorPresetEvent);
  lv_obj_set_style_bg_opa(colorPresetDropdown, LV_OPA_COVER, 0);
  uiTooltipSet(colorPresetDropdown, tr("Color"));

  row = uiSettingRowCreate(box, tr("Line width"));
  widthDropdown = uiDropdownCreate(row, widthOptions, LINE_WIDTH_DEFAULT - LINE_WIDTH_MIN);
  widthFix(widthDropdown, SETTINGS_LENGTH_WIDTH);
  lv_obj_add_event_cb(widthDropdown, widthEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // Scaling of the received value on one line: "x gain + offset", the formula below
  row = uiSettingRowCreate(box, tr("Scaling"));
  lv_label_set_text(lv_label_create(row), "x");
  gainInput = uiTextareaCreate(row, tr("Gain"));
  widthFix(gainInput, SETTINGS_VALUE_WIDTH);
  uiTooltipSet(gainInput, tr("Plotted value = received value x gain (\"/4\": divided by 4)"));
  lv_textarea_set_accepted_chars(gainInput, "0123456789.-/*");
  lv_textarea_set_max_length(gainInput, PLOT_NUMBER_LEN - 1);
  lv_obj_add_event_cb(gainInput, gainEvent, LV_EVENT_VALUE_CHANGED, NULL);

  lv_label_set_text(lv_label_create(row), "+");
  offsetInput = uiTextareaCreate(row, tr("Offset"));
  widthFix(offsetInput, SETTINGS_VALUE_WIDTH);
  uiTooltipSet(offsetInput, tr("Offset"));
  lv_textarea_set_accepted_chars(offsetInput, "0123456789.-");
  lv_textarea_set_max_length(offsetInput, PLOT_NUMBER_LEN - 1);
  lv_obj_add_event_cb(offsetInput, offsetEvent, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t * help = lv_label_create(box);
  lv_obj_set_width(help, LV_PCT(100));
  lv_label_set_long_mode(help, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(help, lv_color_hex(COLOR_INFO), 0);
  lv_label_set_text(help, tr("Plotted value = received value x gain + offset."));

  // Only the shown plots' buttons (plotSelRefresh)
  row = uiSettingRowCreate(box, tr("Show in"));
  showInBtnm = lv_buttonmatrix_create(row);
  uiTooltipSet(showInBtnm, tr("Plots where this variable is shown"));
  lv_obj_set_style_pad_all(showInBtnm, uiPx(2), 0);
  lv_obj_set_style_pad_gap(showInBtnm, uiPx(2), 0);
  lv_obj_add_event_cb(showInBtnm, showInEvent, LV_EVENT_VALUE_CHANGED, NULL);

  varsGroup = lv_group_create();
  lv_group_add_obj(varsGroup, plotNameInput);
  lv_group_add_obj(varsGroup, yMinInput);
  lv_group_add_obj(varsGroup, yMaxInput);
  lv_group_add_obj(varsGroup, nameInput);
  lv_group_add_obj(varsGroup, unitInput);
  lv_group_add_obj(varsGroup, gainInput);
  lv_group_add_obj(varsGroup, offsetInput);

  varDropdownRefresh();
  plotSelRefresh();
  editorLoad(0);
  plotEditorLoad(0);
}

// Settings of plot p from the last session (keys "plot1.bars", "plot1.yAuto", "plot1.yMin",
// "plot1.yMax"). Bars default to the former setting shared by all plots ("bars").
static void plotSettingsLoad(uint32_t p)
{
  Plot_t * plot = &plots[p];
  char     key[PLOT_KEY_LEN];

  snprintf(key, sizeof(key), "plot%u.chart", (unsigned) (p + 1));
  plot->chartVisible = settingsGetInt(key, 1) != 0;
  snprintf(key, sizeof(key), "plot%u.bars", (unsigned) (p + 1));
  plot->barsVisible = settingsGetInt(key, settingsGetInt("bars", 1)) != 0;
  snprintf(key, sizeof(key), "plot%u.barsShare", (unsigned) (p + 1));
  plot->barsShare = LV_CLAMP(BARS_SHARE_MIN, settingsGetInt(key, BARS_SHARE_DEFAULT), BARS_SHARE_MAX);
  // Default: the former setting shared by all plots ("cmdMarks")
  snprintf(key, sizeof(key), "plot%u.cmdMarks", (unsigned) (p + 1));
  plot->cmdMarks = settingsGetInt(key, settingsGetInt("cmdMarks", 0)) != 0;

  snprintf(key, sizeof(key), "plot%u.yMin", (unsigned) (p + 1));
  snprintf(plot->yMinText, PLOT_NUMBER_LEN, "%s", settingsGetString(key, "0"));
  snprintf(key, sizeof(key), "plot%u.yMax", (unsigned) (p + 1));
  snprintf(plot->yMaxText, PLOT_NUMBER_LEN, "%s", settingsGetString(key, "100"));
  snprintf(key, sizeof(key), "plot%u.yAuto", (unsigned) (p + 1));

  // A manual scale needs a valid range
  plot->yManual = (settingsGetInt(key, 1) == 0) && numberParse(plot->yMinText, &plot->yManualMin) &&
                  numberParse(plot->yMaxText, &plot->yManualMax) && (plot->yManualMax > plot->yManualMin) &&
                  (fabsf(plot->yManualMin) <= PLOT_VALUE_MAX) && (fabsf(plot->yManualMax) <= PLOT_VALUE_MAX);

  snprintf(key, sizeof(key), "plot%u.name", (unsigned) (p + 1));
  snprintf(plot->name, PLOT_NAME_LEN, "%s", settingsGetString(key, ""));
  plotTitleUpdate(p);

  // Default: the former setting shared by all plots ("xAxis")
  snprintf(key, sizeof(key), "plot%u.xAxis", (unsigned) (p + 1));
  plot->xAxisMode = (uint32_t) LV_CLAMP(X_AXIS_SAMPLES, settingsGetInt(key, settingsGetInt("xAxis", X_AXIS_SAMPLES)),
                                        X_AXIS_CLOCK);
}

// "P1" or "P1  name"
static void plotTitleUpdate(uint32_t p)
{
  if (plots[p].name[0] != '\0')
  {
    lv_label_set_text_fmt(plots[p].title, "P%u  %s", (unsigned) (p + 1), plots[p].name);
  }
  else
  {
    lv_label_set_text_fmt(plots[p].title, "P%u", (unsigned) (p + 1));
  }
}

// Plot list of the settings: "P1  name" per shown plot (plotCount), the button showing "P1" (like
// the variables); the "Show in" buttons of the plots not shown are hidden (their choice is kept)
static void plotSelRefresh(void)
{
  char   options[PLOT_COUNT_MAX * (PLOT_NAME_LEN + 8)];
  size_t used = 0;

  for (uint32_t p = 0; p < plotCount; ++p)
  {
    used += (size_t) snprintf(&options[used], sizeof(options) - used, "%sP%u%s%s", (p > 0) ? "\n" : "",
                              (unsigned) (p + 1), (plots[p].name[0] != '\0') ? "  " : "", plots[p].name);
  }

  // "Show in": one button per shown plot, SHOW_IN_PER_ROW per row, each of a fixed size (a new map
  // clears the buttons' controls: set again)
  uint32_t n = 0;

  for (uint32_t p = 0; p < plotCount; ++p)
  {
    if ((p > 0) && ((p % SHOW_IN_PER_ROW) == 0))
    {
      showInDynMap[n++] = "\n";
    }
    showInDynMap[n++] = plotNames[p];
  }

  showInDynMap[n] = "";

  uint32_t columns = LV_MIN(plotCount, SHOW_IN_PER_ROW);
  uint32_t rows    = (plotCount + SHOW_IN_PER_ROW - 1) / SHOW_IN_PER_ROW;

  lv_buttonmatrix_set_map(showInBtnm, showInDynMap);
  lv_buttonmatrix_set_button_ctrl_all(showInBtnm, LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
  lv_obj_set_size(showInBtnm, (int32_t) columns * SHOW_IN_BUTTON_W + uiPx(4),
                  (int32_t) rows * SHOW_IN_BUTTON_H + uiPx(4));
  showInChecksSet();

  lv_dropdown_set_options(plotSelDropdown, options);

  // The edited plot is no longer shown: the first one
  if (editedPlot >= plotCount)
  {
    plotEditorLoad(0);
  }

  lv_dropdown_set_selected(plotSelDropdown, editedPlot);
}

static void plotSettingsSave(uint32_t p)
{
  const Plot_t * plot = &plots[p];
  char           key[PLOT_KEY_LEN];

  snprintf(key, sizeof(key), "plot%u.chart", (unsigned) (p + 1));
  settingsSetInt(key, plot->chartVisible ? 1 : 0);
  snprintf(key, sizeof(key), "plot%u.bars", (unsigned) (p + 1));
  settingsSetInt(key, plot->barsVisible ? 1 : 0);
  snprintf(key, sizeof(key), "plot%u.barsShare", (unsigned) (p + 1));
  settingsSetInt(key, plot->barsShare);
  snprintf(key, sizeof(key), "plot%u.cmdMarks", (unsigned) (p + 1));
  settingsSetInt(key, plot->cmdMarks ? 1 : 0);
  snprintf(key, sizeof(key), "plot%u.yAuto", (unsigned) (p + 1));
  settingsSetInt(key, plot->yManual ? 0 : 1);
  snprintf(key, sizeof(key), "plot%u.yMin", (unsigned) (p + 1));
  settingsSetString(key, plot->yMinText);
  snprintf(key, sizeof(key), "plot%u.yMax", (unsigned) (p + 1));
  settingsSetString(key, plot->yMaxText);
  snprintf(key, sizeof(key), "plot%u.name", (unsigned) (p + 1));
  settingsSetString(key, plot->name);
  snprintf(key, sizeof(key), "plot%u.xAxis", (unsigned) (p + 1));
  settingsSetInt(key, (int32_t) plot->xAxisMode);
}

// Fills the plot settings fields with the settings of plot p; Y min / max only editable when the
// Y scale is not auto
static void plotEditorLoad(uint32_t p)
{
  Plot_t * plot = &plots[p];

  editorLoading = true;
  editedPlot = p;

  // The button shows the number only (the list: "P1  name"); set_text keeps a copy
  lv_dropdown_set_text(plotSelDropdown, plotNames[p]);
  lv_textarea_set_text(plotNameInput, plot->name);
  lv_dropdown_set_selected(xAxisDropdown, plot->xAxisMode);

  lv_obj_set_state(chartSwitch, LV_STATE_CHECKED, plot->chartVisible);
  lv_obj_set_state(barsSwitch, LV_STATE_CHECKED, plot->barsVisible);
  lv_obj_set_state(cmdMarksSwitch, LV_STATE_CHECKED, plot->cmdMarks);
  lv_obj_set_state(yAutoSwitch, LV_STATE_CHECKED, !plot->yManual);
  lv_textarea_set_text(yMinInput, plot->yMinText);
  lv_textarea_set_text(yMaxInput, plot->yMaxText);
  inputValidSet(yMinInput, true);
  inputValidSet(yMaxInput, true);
  lv_obj_set_state(yMinInput, LV_STATE_DISABLED, !plot->yManual);
  lv_obj_set_state(yMaxInput, LV_STATE_DISABLED, !plot->yManual);

  editorLoading = false;
}

// Settings of variable v from the last session (keys "var1.name", "var1.gain", ...)
static void varLoad(uint32_t v)
{
  PlotVar_t * var = &vars[v];
  char        key[PLOT_KEY_LEN];

  snprintf(key, sizeof(key), "var%u.name", (unsigned) (v + 1));
  snprintf(var->name, PLOT_NAME_LEN, "%s", settingsGetString(key, ""));

  snprintf(key, sizeof(key), "var%u.unit", (unsigned) (v + 1));
  snprintf(var->unit, PLOT_UNIT_LEN, "%s", settingsGetString(key, ""));

  // "RRGGBB"; missing / invalid: the default color of the variable
  snprintf(key, sizeof(key), "var%u.color", (unsigned) (v + 1));
  if (!colorParse(settingsGetString(key, ""), &var->color))
  {
    var->color = varDefaultColors[v];
  }

  snprintf(key, sizeof(key), "var%u.gain", (unsigned) (v + 1));
  snprintf(var->gainText, PLOT_NUMBER_LEN, "%s", settingsGetString(key, "1"));
  if (!gainParse(var->gainText, &var->gain))
  {
    var->gain = 1.0f;
  }

  snprintf(key, sizeof(key), "var%u.offset", (unsigned) (v + 1));
  snprintf(var->offsetText, PLOT_NUMBER_LEN, "%s", settingsGetString(key, "0"));
  if (!numberParse(var->offsetText, &var->offset))
  {
    var->offset = 0.0f;
  }

  snprintf(key, sizeof(key), "var%u.width", (unsigned) (v + 1));
  var->width = (uint8_t) LV_CLAMP(LINE_WIDTH_MIN, settingsGetInt(key, LINE_WIDTH_DEFAULT), LINE_WIDTH_MAX);

  snprintf(key, sizeof(key), "var%u.plots", (unsigned) (v + 1));
  var->plotMask = (uint8_t) (settingsGetInt(key, 0x01) & ((1 << PLOT_COUNT_MAX) - 1));
}

static void varSave(uint32_t v)
{
  const PlotVar_t * var = &vars[v];
  char              key[PLOT_KEY_LEN];

  snprintf(key, sizeof(key), "var%u.name", (unsigned) (v + 1));
  settingsSetString(key, var->name);
  snprintf(key, sizeof(key), "var%u.unit", (unsigned) (v + 1));
  settingsSetString(key, var->unit);

  char color[8];
  snprintf(color, sizeof(color), "%06lX", (unsigned long) var->color);
  snprintf(key, sizeof(key), "var%u.color", (unsigned) (v + 1));
  settingsSetString(key, color);
  snprintf(key, sizeof(key), "var%u.gain", (unsigned) (v + 1));
  settingsSetString(key, var->gainText);
  snprintf(key, sizeof(key), "var%u.offset", (unsigned) (v + 1));
  settingsSetString(key, var->offsetText);
  snprintf(key, sizeof(key), "var%u.width", (unsigned) (v + 1));
  settingsSetInt(key, var->width);
  snprintf(key, sizeof(key), "var%u.plots", (unsigned) (v + 1));
  settingsSetInt(key, var->plotMask);
}

// Splits a line with the line format (ex: "1.5 2 -3", "1.5,2,-3", "1.5:2:-3", "a:1.5;b:2").
// skipText: empty / text fields are skipped (so "a:1.5 b:2" keeps 1.5 and 2).
// Otherwise an empty / text field leaves a gap so the next values stay in their column.
static void lineParse(uint32_t conn, char * text, uint32_t tick)
{
  float    values[PLOTTER_MAX_VARS];
  bool     valid[PLOTTER_MAX_VARS];
  // At most the connection's share of the variables; 0: not a data line
  uint32_t count = lineParserValues(&parsers[conn], text, connVarCount(), values, valid);

  if (count == 0)
  {
    return;
  }

  PROF_DO(profLine(conn, values, count));

#ifdef SERIAL_DASHBOARD_CLI
  cliLatestUpdate(conn, values, valid, count);
#endif

  // Pausing the plots does not lose any of the test
  if (compareRecording())
  {
    PROF_PUSH(PROF_RECORD);
    recordFeed(conn, values, valid, count, tick);
    PROF_POP();
  }

  if (!paused)
  {
    PROF_PUSH(PROF_PLOT_STORE);
    sampleAdd(conn, values, valid, count, tick);
    PROF_DO(profSamplesPlotted(1));
    PROF_POP();
  }
}

#ifdef SERIAL_DASHBOARD_CLI
static float cliLatest[PLOTTER_MAX_VARS];
static bool  cliLatestSet[PLOTTER_MAX_VARS];

// As recordFeed()
static void cliLatestUpdate(uint32_t conn, const float * values, const bool * valid, uint32_t count)
{
  uint32_t first = conn * connVarCount();

  for (uint32_t v = first; (v < PLOTTER_MAX_VARS) && (varConn(v) == conn); ++v)
  {
    uint32_t i = v - first;

    cliLatest[v]    = ((i < count) && valid[i]) ? displayValue(v, values[i]) : NAN;
    cliLatestSet[v] = true;
  }
}

uint32_t plotterCliLatest(char headers[][TEST_HEADER_LEN], float * values)
{
  uint32_t columns[PLOTTER_MAX_VARS];
  uint32_t count = plotterVarHeaders(columns, headers);

  for (uint32_t c = 0; c < count; ++c)
  {
    values[c] = cliLatestSet[columns[c]] ? cliLatest[columns[c]] : NAN;
  }

  return count;
}
#endif

// New sample of the test recorded: the line's values for the connection's variables, the last
// value of the other connections' (held in recordRow)
static void recordFeed(uint32_t conn, const float * values, const bool * valid, uint32_t count, uint32_t tick)
{
  uint32_t first = conn * connVarCount();

  for (uint32_t v = first; (v < PLOTTER_MAX_VARS) && (varConn(v) == conn); ++v)
  {
    uint32_t i = v - first;

    recordRow[v] = ((i < count) && valid[i]) ? displayValue(v, values[i]) : NAN;
  }

  compareSampleAdd(recordRow, tick);
}

// New sample: the line's values for the connection's variables; the other connections' variables
// keep their last value while those connections are live (otherwise: no value)
// tick: when the line arrived (stamped by the serial reader thread)
static void sampleAdd(uint32_t conn, const float * values, const bool * valid, uint32_t count, uint32_t tick)
{
  uint32_t first    = conn * connVarCount();
  uint32_t previous = (historyHead + PLOT_POINTS_MAX - 1) % PLOT_POINTS_MAX;
  uint32_t seen     = varSeenMask;

  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    uint32_t owner = varConn(v);

    if (owner == conn)
    {
      uint32_t i = v - first;

      history[v][historyHead] = ((i < count) && valid[i]) ? values[i] : NAN;
      seen |= (i < count) ? (1u << v) : 0;
    }
    else if ((owner != CONN_NONE) && connLive[owner] && (historyCount > 0))
    {
      history[v][historyHead] = history[v][previous];
    }
    else
    {
      history[v][historyHead] = NAN;
    }
  }

  connLive[conn] = true;

  if (seen != varSeenMask)
  {
    varSeenMask = seen;
    visibilityUpdate();
    barsLayoutUpdate();
    varDropdownRefresh();
  }

  historyTime[historyHead] = tick;

  // Drawn by renderTimer, so a fast data rate does not redraw for every line
  historyHead  = (historyHead + 1) % PLOT_POINTS_MAX;
  historyCount = LV_MIN(historyCount + 1, PLOT_POINTS_MAX);
  sampleTotal++;
  renderPending = true;
}

static float displayValue(uint32_t v, float raw)
{
  return (raw * vars[v].gain) + vars[v].offset;
}

// Displayed value that can be drawn (the comparison also rejects NaN)
static bool valueDrawable(float value)
{
  return (value >= -PLOT_VALUE_MAX) && (value <= PLOT_VALUE_MAX);
}

// While a plot is rendered, its chart points first hold the displayed values (float bits, NaN = no
// point): the Y range is known only once all of them are computed. They are then converted to
// chart points (chartPoint).
static void pointStore(int32_t * point, float value)
{
  memcpy(point, &value, sizeof(value));
}

static float pointLoad(const int32_t * point)
{
  float value;
  memcpy(&value, point, sizeof(value));
  return value;
}

// The Y range is at least PLOT_Y_RANGE_MIN (and a few float steps of the values), around its center
static void yRangeLimit(Plot_t * plot)
{
  float center = (plot->yMin + plot->yMax) / 2.0f;
  float range  = LV_MAX(PLOT_Y_RANGE_MIN, fabsf(center) * PLOT_Y_RANGE_REL);

  if (!((plot->yMax - plot->yMin) >= range))
  {
    plot->yMin = center - (range / 2.0f);
    plot->yMax = center + (range / 2.0f);
  }
}

// Decimals of the values shown for a plot: one more than its gridlines, 2 to 4 (4: zoomed in to
// gridlines every 0.001 or less)
static int valueDecimals(const Plot_t * plot)
{
  lv_area_t area;

  plotValueArea(plot, &area);
  return LV_CLAMP(VALUE_DECIMALS_MIN, yTicksGet(plot, &area).decimals + 1, VALUE_DECIMALS_MAX);
}

// The Y axis is as wide as its longest gridline value (more decimals when zoomed in)
static void yAxisWidthUpdate(Plot_t * plot)
{
  const lv_font_t * font = lv_obj_get_style_text_font(plot->yAxis, LV_PART_MAIN);
  lv_area_t         area;
  int32_t           width = 0;
  char              text[24];

  plotValueArea(plot, &area);
  Ticks_t ticks = yTicksGet(plot, &area);

  // The widest are at the ends (most digits / minus sign)
  for (uint32_t i = 0; i < ticks.count; i += LV_MAX(1, ticks.count - 1))
  {
    lv_point_t size;

    snprintf(text, sizeof(text), "%.*f", ticks.decimals, (double) plotterTickValue(&ticks, i));
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    width = LV_MAX(width, size.x);
  }

  width = LV_MAX(AXIS_Y_WIDTH, width + uiPx(6));

  if (lv_obj_get_width(plot->yAxis) != width)
  {
    lv_obj_set_width(plot->yAxis, width);
  }
}

// Default name: "Var 3", or with several connections "C2 V3" (3rd value of connection 2's lines)
static void varNameGet(uint32_t v, char * buffer, uint32_t size)
{
  uint32_t conn = varConn(v);

  if (vars[v].name[0] != '\0')
  {
    snprintf(buffer, size, "%s", vars[v].name);
  }
  else if ((connCount > 1) && (conn != CONN_NONE))
  {
    snprintf(buffer, size, "C%u V%u", (unsigned) (conn + 1), (unsigned) (v - (conn * connVarCount()) + 1));
  }
  else
  {
    snprintf(buffer, size, "Var %u", (unsigned) (v + 1));
  }
}

// Variables of each connection (the last ones are unused when the split is not exact, ex: 3 x 10)
static uint32_t connVarCount(void)
{
  return PLOTTER_MAX_VARS / connCount;
}

// Connection whose lines give variable v
static uint32_t varConn(uint32_t v)
{
  uint32_t conn = v / connVarCount();
  return (conn < connCount) ? conn : CONN_NONE;
}

static bool varSeen(uint32_t v)
{
  return (varSeenMask & (1u << v)) != 0;
}

static bool varInPlot(uint32_t v, uint32_t p)
{
  return varSeen(v) && ((vars[v].plotMask & (1u << p)) != 0);
}

// Position 0 is the oldest shown sample, plotLength - 1 the latest
static uint32_t historyIndex(uint32_t position)
{
  return (historyHead + (PLOT_POINTS_MAX - plotLength) + position) % PLOT_POINTS_MAX;
}

// Position shown in the legend / bars: at V1 when placed, otherwise the latest sample
static uint32_t shownPosition(void)
{
  return (vCursor[0] != CURSOR_NONE) ? (uint32_t) vCursor[0] : (plotLength - 1);
}

// Fills the charts with the shown positions of the history and fits the Y axis of each plot
// that is not zoomed (10 % margin), then updates what depends on them.
// Few enough samples: one chart point per sample. Above: the samples are split in groups of about
// 2 columns, each drawn as 2 points, its min and max in time order.
static void plotsRender(void)
{
  renderPending = false;
  PROF_PUSH(PROF_PLOT_PREP);

  for (uint32_t p = 0; p < plotCount; ++p)
  {
    Plot_t * plot     = &plots[p];
    uint32_t viewStart = plot->viewStart;
    uint32_t count    = (plot->viewEnd - viewStart) + 1;
    float    minValue = INFINITY;
    float    maxValue = -INFINITY;

    // About one group per 2 pixel columns of the chart: 2 points per group stay below one point
    // per column, the limit where lv_chart stops drawing lines
    int32_t  columns  = lv_obj_get_content_width(plot->chart);
    uint32_t target   = (columns > 0) ? (uint32_t) LV_CLAMP(PLOT_BUCKETS_MIN, (columns / 2) - PLOT_COLUMNS_MARGIN, PLOT_BUCKETS_MAX)
                                      : PLOT_DISPLAY_BUCKETS;
    // Rounded up: at most target groups. Aligned on the absolute sample numbers, not on the view,
    // so a sample stays in the same group while the data scrolls (otherwise the groups' min / max
    // moved within them at each new sample and the curve seemed to stretch and compress).
    uint32_t size     = (count + target - 1) / target;
    bool     decimate = size > 1;
    int64_t  base     = (int64_t) sampleTotal - (int64_t) plotLength;   // sample number at position 0
    int64_t  absStart = base + viewStart;
    int64_t  absFirst = ((absStart >= 0) ? (absStart / size) : -(((-absStart) + size - 1) / size)) * (int64_t) size;
    uint32_t buckets  = (uint32_t) (((absStart + count) - absFirst + size - 1) / size);

    // Aligned groups: buckets is maxBuckets or one less while scrolling. The chart keeps
    // maxBuckets slots (same horizontal scale, no reallocation); an unused last slot stays empty.
    uint32_t maxBuckets = decimate ? (((count + size - 1) / size) + 1) : count;

    lv_chart_set_point_count(plot->chart, decimate ? (maxBuckets * 2) : maxBuckets);
    PROF_DO(uint32_t shownSeries = 0;)

    for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
    {
      // Hidden series are not drawn: filled when shown (visibility changes render again)
      if (!varInPlot(v, p))
      {
        continue;
      }

      PROF_DO(shownSeries++;)

      int32_t *     y     = lv_chart_get_series_y_array(plot->chart, plot->series[v]);
      const float * raw   = history[v];
      uint32_t      index = historyIndex(viewStart);   // follows pos through the ring buffer

      for (uint32_t b = 0; b < buckets; ++b)
      {
        // Group b: samples absFirst + b * size ... (the first / last group may be partly outside
        // the view), as positions
        uint32_t first = (uint32_t) (LV_MAX(absFirst + ((int64_t) b * size), absStart) - base);
        uint32_t last  = (uint32_t) (LV_MIN(absFirst + ((int64_t) (b + 1) * size), absStart + count) - base);
        float    rawLow = 0.0f;
        float    rawHigh = 0.0f;
        uint32_t rawLowPos = 0;
        uint32_t rawHighPos = 0;
        bool     any = false;

        // Extremes of the raw values (value * gain + offset keeps the order, or reverses it when
        // gain < 0): only those two are converted, not every sample
        for (uint32_t pos = first; pos < last; ++pos)
        {
          float value = raw[index];

          index = ((index + 1) < PLOT_POINTS_MAX) ? (index + 1) : 0;

          // NaN: no value
          if (value != value)
          {
            continue;
          }

          if (!any || (value < rawLow))
          {
            rawLow = value;
            rawLowPos = pos;
          }

          if (!any || (value > rawHigh))
          {
            rawHigh = value;
            rawHighPos = pos;
          }

          any = true;
        }

        bool     reversed = vars[v].gain < 0.0f;
        float    low      = displayValue(v, reversed ? rawHigh : rawLow);
        float    high     = displayValue(v, reversed ? rawLow : rawHigh);
        uint32_t lowPos   = reversed ? rawHighPos : rawLowPos;
        uint32_t highPos  = reversed ? rawLowPos : rawHighPos;

        // Too large after gain / offset: no point
        any = any && valueDrawable(low) && valueDrawable(high);

        if (any)
        {
          minValue = fminf(minValue, low);
          maxValue = fmaxf(maxValue, high);
        }

        // Displayed values for now (see pointStore)
        if (!decimate)
        {
          pointStore(&y[b], any ? low : NAN);
        }
        else
        {
          pointStore(&y[2 * b], !any ? NAN : ((lowPos <= highPos) ? low : high));
          pointStore(&y[(2 * b) + 1], !any ? NAN : ((lowPos <= highPos) ? high : low));
        }
      }

      // Unused last slot (see maxBuckets)
      for (uint32_t b = buckets; b < maxBuckets; ++b)
      {
        pointStore(&y[decimate ? (2 * b) : b], NAN);

        if (decimate)
        {
          pointStore(&y[(2 * b) + 1], NAN);
        }
      }
    }

    // Zoomed: kept. Manual scale: as set. Auto: fitted to the shown points.
    if (plot->yZoomed)
    {
    }
    else if (plot->yManual)
    {
      plot->yMin = plot->yManualMin;
      plot->yMax = plot->yManualMax;
    }
    else if (minValue <= maxValue)
    {
      float margin = (maxValue - minValue) / 10.0f;
      if (margin == 0.0f)
      {
        margin = 1.0f;
      }

      plot->yMin = minValue - margin;
      plot->yMax = maxValue + margin;
    }

    yRangeLimit(plot);

    // Displayed values to chart points, now that the Y range is known
    uint32_t pointCount = lv_chart_get_point_count(plot->chart);

    for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
    {
      if (varInPlot(v, p))
      {
        int32_t * y = lv_chart_get_series_y_array(plot->chart, plot->series[v]);

        for (uint32_t i = 0; i < pointCount; ++i)
        {
          y[i] = uiChartPoint(pointLoad(&y[i]), plot->yMin, plot->yMax);
        }
      }
    }

    yAxisWidthUpdate(plot);
    lv_chart_refresh(plot->chart);
    PROF_DO(profChartPoints(count * shownSeries, pointCount * shownSeries));
  }

  legendUpdate();
  readoutUpdate();
  plotsInvalidate();
  PROF_POP();
}

// New plot length: shows all of it, cursors outside it are removed
static void plotLengthApply(void)
{
  plotLength = plotLengths[lv_dropdown_get_selected(plotLengthDropdown)];

  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    plots[p].viewStart = 0;
    plots[p].viewEnd   = plotLength - 1;
  }

  for (uint32_t c = 0; c < 2; ++c)
  {
    if (vCursor[c] >= (int32_t) plotLength)
    {
      vCursor[c] = CURSOR_NONE;
    }
  }
}

// Shows each series only in the plots selected for its variable; an empty plot tells why
static void visibilityUpdate(void)
{
  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    bool empty = true;

    for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
    {
      lv_chart_hide_series(plots[p].chart, plots[p].series[v], !varInPlot(v, p));
      empty = empty && !varInPlot(v, p);
    }

    if (varSeenMask == 0)
    {
      lv_label_set_text(plots[p].hint, tr("Waiting for lines of numbers\nex: 17.08 17.50 -2.38"));
    }
    else
    {
      lv_label_set_text_fmt(plots[p].hint, tr("No variable in P%u\n" LV_SYMBOL_SETTINGS " > Show in"), (unsigned) (p + 1));
    }

    lv_obj_set_hidden(plots[p].hint, !empty);
  }

  legendUpdate();
}

// Chart and / or bar graph of each plot. Both shown: they share the width (barsShare, set with the
// divider between them); alone, each fills the window.
static void barsLayoutUpdate(void)
{
  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    Plot_t * plot = &plots[p];
    uint32_t count = barsCount(p);
    bool     barsShown = plot->barsVisible && (count > 0);
    bool     both = plot->chartVisible && barsShown;

    lv_obj_set_hidden(plot->chart, !plot->chartVisible);
    lv_obj_set_hidden(plot->yAxis, !plot->chartVisible && !barsShown);
    lv_obj_set_hidden(plot->bars, !barsShown);
    lv_obj_set_hidden(plot->divider, !both);

    // flex_grow is 1..255: the share in percent
    int32_t barsGrow = LV_CLAMP(1, (plot->barsShare * 100) / GEOM_FULL, 99);

    lv_obj_set_flex_grow(plot->chart, both ? (uint8_t) (100 - barsGrow) : 1);
    lv_obj_set_flex_grow(plot->bars, both ? (uint8_t) barsGrow : 1);

    lv_obj_invalidate(plot->bars);
    lv_obj_invalidate(plot->xAxis);
  }
}

// Shows the plot windows in use; a new number of plots tiles them
static void plotsLayoutUpdate(void)
{
  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    lv_obj_set_hidden(plots[p].cell, p >= plotCount);
  }

  windowsApply();
  visibilityUpdate();
  barsLayoutUpdate();
  plotsRender();
}

static uint32_t barsCount(uint32_t p)
{
  uint32_t count = 0;

  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    count += varInPlot(v, p) ? 1 : 0;
  }

  return count;
}

// The bars share the bar graph's width
static int32_t barSlotWidth(const Plot_t * plot, const lv_area_t * barsArea, uint32_t count)
{
  LV_UNUSED(plot);
  return lv_area_get_width(barsArea) / (int32_t) LV_MAX(count, 1);
}

// Screen area of the plot's Y scale: the chart's content, or without the chart the bars' area
// (room kept above for the value of the highest bar)
static void plotValueArea(const Plot_t * plot, lv_area_t * area)
{
  if (plot->chartVisible)
  {
    lv_obj_get_content_coords(plot->chart, area);
    return;
  }

  lv_obj_get_coords(plot->bars, area);
  area->y1 += lv_font_get_line_height(uiFont()) + uiPx(2);
  area->y2 -= uiPx(2);
}

// Default layout, saved: the plots in rows (1-2 plots: 1 row, 3-4: 2 columns, 5-6: 3, 7-8: 4),
// the windows of the last row stretched to fill it (3 plots: 2 on top, 1 across the bottom).
// With the commands window shown, it takes a column at the right and the plots the rest.
static void plotsTile(void)
{
  int32_t width = windowShown(WIN_CMD) ? (GEOM_FULL - CMD_TILE_WIDTH) : GEOM_FULL;
  int32_t cols  = (plotCount <= 2) ? (int32_t) plotCount : (int32_t) ((plotCount + 1) / 2);
  int32_t rows  = ((int32_t) plotCount + cols - 1) / cols;

  for (uint32_t p = 0; p < plotCount; ++p)
  {
    int32_t row   = (int32_t) p / cols;
    int32_t col   = (int32_t) p % cols;
    int32_t inRow = (row == (rows - 1)) ? ((int32_t) plotCount - (cols * row)) : cols;

    winGeom[p][0] = (col * width) / inRow;
    winGeom[p][1] = (row * GEOM_FULL) / rows;
    winGeom[p][2] = (((col + 1) * width) / inRow) - winGeom[p][0];
    winGeom[p][3] = (((row + 1) * GEOM_FULL) / rows) - winGeom[p][1];
    geomSave(p);
  }

  if (windowShown(WIN_CMD))
  {
    winGeom[WIN_CMD][0] = width;
    winGeom[WIN_CMD][1] = 0;
    winGeom[WIN_CMD][2] = GEOM_FULL - width;
    winGeom[WIN_CMD][3] = GEOM_FULL;
    geomSave(WIN_CMD);
  }

  windowsApply();
}

static bool windowShown(uint32_t w)
{
  return (w == WIN_CMD) ? !lv_obj_is_hidden(winObj[WIN_CMD]) : (w < plotCount);
}

// Key of window w's geometry: "plot1.geom" ... "plot4.geom", "cmdWindow.geom"
static void geomKeyGet(uint32_t w, char * key, uint32_t size)
{
  if (w == WIN_CMD)
  {
    snprintf(key, size, "cmdWindow.geom");
  }
  else
  {
    snprintf(key, size, "plot%u.geom", (unsigned) (w + 1));
  }
}

// "x,y,w,h" in 1/GEOM_FULL of the plot area; false when missing / invalid
static bool geomLoad(uint32_t w)
{
  char key[PLOT_KEY_LEN];
  int  g[4];

  geomKeyGet(w, key, sizeof(key));

  if ((sscanf(settingsGetString(key, ""), "%d,%d,%d,%d", &g[0], &g[1], &g[2], &g[3]) != 4) ||
      (g[0] < 0) || (g[1] < 0) || (g[2] <= 0) || (g[3] <= 0) || ((g[0] + g[2]) > GEOM_FULL) ||
      ((g[1] + g[3]) > GEOM_FULL))
  {
    return false;
  }

  for (uint32_t i = 0; i < 4; ++i)
  {
    winGeom[w][i] = g[i];
  }

  return true;
}

static void geomSave(uint32_t w)
{
  char key[PLOT_KEY_LEN];
  char value[32];

  geomKeyGet(w, key, sizeof(key));
  snprintf(value, sizeof(value), "%ld,%ld,%ld,%ld", (long) winGeom[w][0], (long) winGeom[w][1],
           (long) winGeom[w][2], (long) winGeom[w][3]);
  settingsSetString(key, value);
}

// Window rectangle in pixels, relative to plotsBox. The geometry covers the window and half of
// the gap around it, so neighbor windows are WINDOW_GAP apart.
static void windowRectGet(uint32_t w, lv_area_t * rect)
{
  int32_t width  = lv_obj_get_content_width(plotsBox);
  int32_t height = lv_obj_get_content_height(plotsBox);
  int32_t half   = WINDOW_GAP / 2;

  rect->x1 = ((winGeom[w][0] * width) / GEOM_FULL) + half;
  rect->y1 = ((winGeom[w][1] * height) / GEOM_FULL) + half;
  rect->x2 = (((winGeom[w][0] + winGeom[w][2]) * width) / GEOM_FULL) - half - 1;
  rect->y2 = (((winGeom[w][1] + winGeom[w][3]) * height) / GEOM_FULL) - half - 1;
}

// Back to the geometry (kept inside the plot area) and to the window's position / size
static void windowRectSet(uint32_t w, const lv_area_t * rect)
{
  int32_t width  = LV_MAX(lv_obj_get_content_width(plotsBox), 1);
  int32_t height = LV_MAX(lv_obj_get_content_height(plotsBox), 1);
  int32_t half   = WINDOW_GAP / 2;

  winGeom[w][0] = LV_CLAMP(0, (((rect->x1 - half) * GEOM_FULL) + (width / 2)) / width, GEOM_FULL - 1);
  winGeom[w][1] = LV_CLAMP(0, (((rect->y1 - half) * GEOM_FULL) + (height / 2)) / height, GEOM_FULL - 1);
  winGeom[w][2] = LV_CLAMP(1, ((((rect->x2 + 1 + half) * GEOM_FULL) + (width / 2)) / width) - winGeom[w][0],
                           GEOM_FULL - winGeom[w][0]);
  winGeom[w][3] = LV_CLAMP(1, ((((rect->y2 + 1 + half) * GEOM_FULL) + (height / 2)) / height) - winGeom[w][1],
                           GEOM_FULL - winGeom[w][1]);

  lv_obj_set_pos(winObj[w], rect->x1, rect->y1);
  lv_obj_set_size(winObj[w], lv_area_get_width(rect), lv_area_get_height(rect));
  renderPending = true;   // groups follow the charts' widths
}

// Positions / sizes the windows from their geometry (also when the plot area is resized). Before
// the first layout the area has no size yet: done by plotsBoxSizeEvent once it has.
static void windowsApply(void)
{
  if ((lv_obj_get_content_width(plotsBox) <= 0) || (lv_obj_get_content_height(plotsBox) <= 0))
  {
    return;
  }

  for (uint32_t w = 0; w < WIN_COUNT; ++w)
  {
    lv_area_t rect;

    windowRectGet(w, &rect);
    lv_obj_set_pos(winObj[w], rect.x1, rect.y1);
    lv_obj_set_size(winObj[w], lv_area_get_width(&rect), lv_area_get_height(&rect));
  }
}

// Corner handle of window w (drag to resize), above its content
static void windowHandleCreate(uint32_t w)
{
  void *     userData = (void *) (uintptr_t) w;
  lv_obj_t * handle = lv_obj_create(winObj[w]);

  lv_obj_remove_style_all(handle);
  lv_obj_set_floating(handle, true);
  lv_obj_set_size(handle, WINDOW_HANDLE, WINDOW_HANDLE);
  lv_obj_align(handle, LV_ALIGN_BOTTOM_RIGHT, uiPx(3), uiPx(3));
  lv_obj_set_clickable(handle, true);
  lv_obj_add_event_cb(handle, resizeHandleDrawEvent, LV_EVENT_DRAW_MAIN, NULL);
  lv_obj_add_event_cb(handle, windowResizeEvent, LV_EVENT_PRESSED, userData);
  lv_obj_add_event_cb(handle, windowResizeEvent, LV_EVENT_PRESSING, userData);
  lv_obj_add_event_cb(handle, windowResizeEvent, LV_EVENT_RELEASED, userData);
  uiTooltipSet(handle, tr("Drag to resize"));
  winHandle[w] = handle;
}

// Window w: pressing its border drags the seams, its title bar moves it
static void windowEventsAdd(uint32_t w, lv_obj_t * titleBar)
{
  void * userData = (void *) (uintptr_t) w;

  lv_obj_add_event_cb(winObj[w], seamEvent, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(winObj[w], seamEvent, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(winObj[w], seamEvent, LV_EVENT_RELEASED, NULL);

  lv_obj_set_clickable(titleBar, true);
  lv_obj_add_event_cb(titleBar, windowMoveEvent, LV_EVENT_PRESSED, userData);
  lv_obj_add_event_cb(titleBar, windowMoveEvent, LV_EVENT_PRESSING, userData);
  lv_obj_add_event_cb(titleBar, windowMoveEvent, LV_EVENT_RELEASED, userData);
}

// *delta: shift that brings value onto the nearest target; false when none is within SNAP_PX
static bool snapDelta(int32_t value, const int32_t * targets, uint32_t count, int32_t * delta)
{
  int32_t bestDistance = SNAP_PX + 1;

  for (uint32_t i = 0; i < count; ++i)
  {
    int32_t distance = LV_ABS(targets[i] - value);

    if (distance < bestDistance)
    {
      bestDistance = distance;
      *delta = targets[i] - value;
    }
  }

  return bestDistance <= SNAP_PX;
}

// Moving window: shift snapping its low or its high edge, whichever is closer to a target
static int32_t snapPair(int32_t low, int32_t high, const int32_t * targets, uint32_t count)
{
  int32_t lowDelta = 0;
  int32_t highDelta = 0;
  bool    lowSnap  = snapDelta(low, targets, count, &lowDelta);
  bool    highSnap = snapDelta(high, targets, count, &highDelta);

  if (lowSnap && (!highSnap || (LV_ABS(lowDelta) <= LV_ABS(highDelta))))
  {
    return lowDelta;
  }

  return highSnap ? highDelta : 0;
}

// Edges a window edge can snap to (horizontal: x, otherwise y): the plot area's edges, the edges
// of the other shown windows (aligned) and next to them (WINDOW_GAP apart)
static uint32_t snapTargetsGet(uint32_t w, bool horizontal, int32_t * targets)
{
  int32_t  half  = WINDOW_GAP / 2;
  int32_t  size  = horizontal ? lv_obj_get_content_width(plotsBox) : lv_obj_get_content_height(plotsBox);
  uint32_t count = 0;

  targets[count++] = half;
  targets[count++] = size - half - 1;

  for (uint32_t q = 0; q < WIN_COUNT; ++q)
  {
    lv_area_t other;

    if ((q == w) || !windowShown(q))
    {
      continue;
    }

    windowRectGet(q, &other);
    int32_t low  = horizontal ? other.x1 : other.y1;
    int32_t high = horizontal ? other.x2 : other.y2;

    targets[count++] = low;
    targets[count++] = high;
    targets[count++] = high + WINDOW_GAP + 1;
    targets[count++] = low - WINDOW_GAP - 1;
  }

  return count;
}

// Above the other windows
static void windowRaise(uint32_t w)
{
  lv_obj_move_to_index(winObj[w], -1);
}

// Seam at point (relative to plotsBox; vertical: a seam between windows side by side): an edge of
// a shown window within SEAM_GRAB_PX, with windows on both sides. lowMask: windows whose right
// (bottom) edge is on the seam, highMask: windows whose left (top) edge is on it.
static bool seamFind(lv_point_t point, bool vertical, uint16_t * lowMask, uint16_t * highMask)
{
  int32_t half = WINDOW_GAP / 2;

  for (uint32_t w = 0; w < WIN_COUNT; ++w)
  {
    lv_area_t rect;

    if (!windowShown(w))
    {
      continue;
    }

    windowRectGet(w, &rect);

    int32_t along     = vertical ? point.y : point.x;
    int32_t alongLow  = (vertical ? rect.y1 : rect.x1) - SEAM_GRAB_PX;
    int32_t alongHigh = (vertical ? rect.y2 : rect.x2) + SEAM_GRAB_PX;
    int32_t across    = vertical ? point.x : point.y;

    if ((along < alongLow) || (along > alongHigh))
    {
      continue;
    }

    // Seam line candidates: just after this window's right edge, just before its left edge
    int32_t lines[2] = {(vertical ? rect.x2 : rect.y2) + half, (vertical ? rect.x1 : rect.y1) - half};

    for (uint32_t i = 0; i < 2; ++i)
    {
      if (LV_ABS(across - lines[i]) > SEAM_GRAB_PX)
      {
        continue;
      }

      *lowMask = 0;
      *highMask = 0;

      for (uint32_t q = 0; q < WIN_COUNT; ++q)
      {
        lv_area_t other;

        if (!windowShown(q))
        {
          continue;
        }

        windowRectGet(q, &other);

        if (LV_ABS(((vertical ? other.x2 : other.y2) + half) - lines[i]) <= SEAM_ALIGN_PX)
        {
          *lowMask |= (uint16_t) (1u << q);
        }

        if (LV_ABS(((vertical ? other.x1 : other.y1) - half) - lines[i]) <= SEAM_ALIGN_PX)
        {
          *highMask |= (uint16_t) (1u << q);
        }
      }

      // A seam has windows on both sides (otherwise: the plot area's edge)
      if ((*lowMask != 0) && (*highMask != 0))
      {
        return true;
      }
    }
  }

  *lowMask = 0;
  *highMask = 0;
  return false;
}

// Seam shift limited so every window on it keeps its minimum size
static int32_t seamShiftLimit(int32_t shift, bool vertical)
{
  uint16_t lowMask  = vertical ? seamLeftMask : seamAboveMask;
  uint16_t highMask = vertical ? seamRightMask : seamBelowMask;
  int32_t minSize  = vertical ? WINDOW_MIN_W : WINDOW_MIN_H;

  for (uint32_t w = 0; w < WIN_COUNT; ++w)
  {
    const lv_area_t * rect = &seamStartRects[w];
    int32_t           size = vertical ? lv_area_get_width(rect) : lv_area_get_height(rect);

    // Low side windows shrink when shift < 0, high side windows when shift > 0
    if ((lowMask & (1u << w)) != 0)
    {
      shift = LV_MAX(shift, minSize - size);
    }

    if ((highMask & (1u << w)) != 0)
    {
      shift = LV_MIN(shift, size - minSize);
    }
  }

  return shift;
}

// "Name value" of each variable (value at V1 when that cursor is placed), only above the plots
// that show the variable
// Each plot shows the values with its own decimals (more when zoomed in)
static void legendUpdate(void)
{
  uint32_t index = historyIndex(shownPosition());
  int      decimals[PLOT_COUNT_MAX];

  for (uint32_t p = 0; p < plotCount; ++p)
  {
    decimals[p] = valueDecimals(&plots[p]);
  }

  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    char  name[PLOT_NAME_LEN];
    char  text[PLOT_NAME_LEN + PLOT_UNIT_LEN + 24];
    float value = displayValue(v, history[v][index]);

    varNameGet(v, name, sizeof(name));

    // Hidden plots are updated when shown (plotsLayoutUpdate)
    for (uint32_t p = 0; p < plotCount; ++p)
    {
      lv_obj_t * label = plots[p].legendLabels[v];
      bool       shown = varInPlot(v, p);

      lv_obj_set_hidden(label, !shown);

      if (!shown)
      {
        continue;
      }

      // "Temp 17.70 °C" (no unit: "Temp 17.70")
      if (isnan(value))
      {
        snprintf(text, sizeof(text), "%s -", name);
      }
      else
      {
        snprintf(text, sizeof(text), "%s %.*f%s%s", name, decimals[p], (double) value,
                 (vars[v].unit[0] != '\0') ? " " : "", vars[v].unit);
      }

      // Called at every redraw: only changed texts are set (each set redraws the label)
      if (strcmp(lv_label_get_text(label), text) != 0)
      {
        lv_label_set_text(label, text);
      }
    }
  }
}

// "V1 #120  V2 #150  dX 30  H1 17.50  H2 2.00  dY 15.50"
static void readoutUpdate(void)
{
  char   text[160];
  size_t used = 0;
  int    decimals = VALUE_DECIMALS_MIN;

  text[0] = '\0';

  // Horizontal cursors are in every plot: decimals of the most zoomed in one
  for (uint32_t p = 0; p < plotCount; ++p)
  {
    decimals = LV_MAX(decimals, valueDecimals(&plots[p]));
  }

  // Vertical cursors (shared by the plots): sample numbers / times as on P1's X axis, to the ms
  uint32_t xMode = plots[0].xAxisMode;

  for (uint32_t c = 0; c < 2; ++c)
  {
    if (vCursor[c] != CURSOR_NONE)
    {
      char position[24];

      positionLabel(xMode, (uint32_t) vCursor[c], 3, position, sizeof(position));
      used += (size_t) snprintf(&text[used], sizeof(text) - used, (xMode == X_AXIS_SAMPLES) ? "%s #%s  " : "%s %s  ",
                                cursorNames[TOOL_V1 + c], position);
    }
  }

  if ((vCursor[0] != CURSOR_NONE) && (vCursor[1] != CURSOR_NONE))
  {
    uint32_t tick0;
    uint32_t tick1;

    if (xMode == X_AXIS_SAMPLES)
    {
      used += (size_t) snprintf(&text[used], sizeof(text) - used, "dX %ld  ", (long) labs(vCursor[1] - vCursor[0]));
    }
    else if (positionTick((uint32_t) vCursor[0], &tick0) && positionTick((uint32_t) vCursor[1], &tick1))
    {
      used += (size_t) snprintf(&text[used], sizeof(text) - used, "dt %.3f s  ",
                                (double) labs((long) (int32_t) (tick1 - tick0)) / 1000.0);
    }
  }

  for (uint32_t c = 0; c < 2; ++c)
  {
    if (hCursorSet[c])
    {
      used += (size_t) snprintf(&text[used], sizeof(text) - used, "%s %.*f  ", cursorNames[TOOL_H1 + c],
                                decimals, (double) hCursor[c]);
    }
  }

  if (hCursorSet[0] && hCursorSet[1])
  {
    snprintf(&text[used], sizeof(text) - used, "dY %.*f", decimals, (double) fabsf(hCursor[1] - hCursor[0]));
  }

  // Called at every redraw: set only when changed (each set lays the label out again)
  if (strcmp(lv_label_get_text(readoutLabel), text) != 0)
  {
    lv_label_set_text(readoutLabel, text);
  }

  lv_obj_set_hidden(readoutLabel, text[0] == '\0');
}

static void plotsInvalidate(void)
{
  for (uint32_t p = 0; p < plotCount; ++p)
  {
    lv_obj_invalidate(plots[p].chart);
    lv_obj_invalidate(plots[p].yAxis);
    lv_obj_invalidate(plots[p].xAxis);
    lv_obj_invalidate(plots[p].bars);
  }
}

// Gridline values between min and max, about one every "spacing" pixels, on 1 / 2 / 5 steps
Ticks_t plotterTicksCompute(float min, float max, int32_t pixels, int32_t spacing)
{
  Ticks_t ticks = {0.0f, 0.0f, 0, 0};
  float   range = max - min;

  if (!(range > 0.0f) || (pixels <= 0))
  {
    return ticks;
  }

  float raw  = range / (float) LV_MAX(pixels / spacing, 1);
  float mag  = powf(10.0f, floorf(log10f(raw)));
  float norm = raw / mag;

  ticks.step  = ((norm <= 1.0f) ? 1.0f : (norm <= 2.0f) ? 2.0f : (norm <= 5.0f) ? 5.0f : 10.0f) * mag;
  ticks.first = ceilf(min / ticks.step) * ticks.step;
  ticks.count = (uint32_t) LV_CLAMP(0.0f, floorf((max - ticks.first) / ticks.step) + 1.0f, (float) GRID_MAX_LINES);
  ticks.decimals = (ticks.step >= 1.0f) ? 0 : (ticks.step >= 0.1f) ? 1 : (ticks.step >= 0.01f) ? 2 :
                   (ticks.step >= 0.001f) ? 3 : 4;
  return ticks;
}

static Ticks_t yTicksGet(const Plot_t * plot, const lv_area_t * area)
{
  return plotterTicksCompute(plot->yMin, plot->yMax, lv_area_get_height(area), GRID_Y_SPACING_PX);
}

// X gridlines are on whole samples
static Ticks_t xTicksGet(const Plot_t * plot, const lv_area_t * area)
{
  Ticks_t ticks = plotterTicksCompute((float) plot->viewStart, (float) plot->viewEnd, lv_area_get_width(area),
                               (plot->xAxisMode == X_AXIS_SAMPLES) ? GRID_X_SPACING_PX : GRID_X_TIME_SPACING_PX);

  if ((ticks.count > 0) && (ticks.step < 1.0f))
  {
    ticks = (Ticks_t) {(float) plot->viewStart, 1.0f, (plot->viewEnd - plot->viewStart) + 1, 0};
  }

  return ticks;
}

// i-th gridline value; tiny rounding errors near 0 would print "-0"
float plotterTickValue(const Ticks_t * ticks, uint32_t i)
{
  float value = ticks->first + (ticks->step * (float) i);
  return (fabsf(value) < (ticks->step * 0.001f)) ? 0.0f : value;
}

// Same mapping as lv_chart's line drawing
static int32_t positionToX(const Plot_t * plot, const lv_area_t * area, float position)
{
  return area->x1 + (int32_t) (((float) lv_area_get_width(area) * (position - (float) plot->viewStart)) /
                               (float) (plot->viewEnd - plot->viewStart));
}

static int32_t xToPosition(const Plot_t * plot, const lv_area_t * area, int32_t x)
{
  int32_t width = lv_area_get_width(area);
  int32_t span  = (int32_t) (plot->viewEnd - plot->viewStart);
  int32_t dx    = LV_CLAMP(0, x - area->x1, width);

  return (int32_t) plot->viewStart + (((dx * span) + (width / 2)) / LV_MAX(width, 1));
}

// Nearest gridline, kept inside [min, max]
static float tickSnap(const Ticks_t * ticks, float value, float min, float max)
{
  if (ticks->count > 0)
  {
    value = ticks->first + (roundf((value - ticks->first) / ticks->step) * ticks->step);
  }

  return LV_CLAMP(min, value, max);
}

static float   xToPositionF(const Plot_t * plot, const lv_area_t * area, int32_t x)
{
  int32_t width = LV_MAX(lv_area_get_width(area), 1);
  int32_t dx    = LV_CLAMP(0, x - area->x1, width);

  return (float) plot->viewStart + (((float) dx * (float) (plot->viewEnd - plot->viewStart)) / (float) width);
}

// Range of the X / Y axis selection being dragged, snapped to the gridlines (X: positions,
// Y: displayed values). The range is at least one gridline step.
static bool axisSelectionGet(float * low, float * high)
{
  Plot_t *  plot = &plots[selPlot];
  lv_area_t area;
  Ticks_t   ticks;
  float     min;
  float     max;

  plotValueArea(plot, &area);

  if (selMode == SEL_X)
  {
    ticks = xTicksGet(plot, &area);
    min   = (float) plot->viewStart;
    max   = (float) plot->viewEnd;
    *low  = xToPositionF(plot, &area, LV_MIN(selStart.x, selEnd.x));
    *high = xToPositionF(plot, &area, LV_MAX(selStart.x, selEnd.x));
  }
  else if (selMode == SEL_Y)
  {
    ticks = yTicksGet(plot, &area);
    min   = plot->yMin;
    max   = plot->yMax;
    *low  = yToValue(plot, &area, LV_MAX(selStart.y, selEnd.y));
    *high = yToValue(plot, &area, LV_MIN(selStart.y, selEnd.y));
  }
  else
  {
    return false;
  }

  *low  = tickSnap(&ticks, *low, min, max);
  *high = tickSnap(&ticks, *high, min, max);

  // Both edges on the same gridline: take the next one (or the previous one at the end)
  if ((*high <= *low) && (ticks.count > 0))
  {
    if ((*low + ticks.step) <= max)
    {
      *high = *low + ticks.step;
    }
    else
    {
      *low = LV_MAX(min, *high - ticks.step);
    }
  }

  return *high > *low;
}

// Same mapping as lv_chart's line drawing (values out of the range: at the edge)
static int32_t valueToY(const Plot_t * plot, const lv_area_t * area, float value)
{
  int32_t height = lv_area_get_height(area);
  float   part   = (value - plot->yMin) / (plot->yMax - plot->yMin);

  if (!(part >= 0.0f))
  {
    part = 0.0f;
  }

  return (area->y1 + height) - (int32_t) (LV_MIN(part, 1.0f) * (float) height);
}

static float yToValue(const Plot_t * plot, const lv_area_t * area, int32_t y)
{
  int32_t height = lv_area_get_height(area);
  int32_t dy     = LV_CLAMP(0, (area->y1 + height) - y, height);

  return plot->yMin + (((float) dy * (plot->yMax - plot->yMin)) / (float) LV_MAX(height, 1));
}

// Press / drag / release of a zoom selection (box in a chart, or section of an axis)
static void selectionHandle(lv_event_t * e, int32_t mode, uint32_t p)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_point_t      point;

  lv_indev_get_point(lv_indev_active(), &point);

  if (code == LV_EVENT_PRESSED)
  {
    selMode  = mode;
    selPlot  = p;
    selStart = point;
    selEnd   = point;
  }
  else if (selMode == mode)
  {
    selEnd = point;

    if (code == LV_EVENT_RELEASED)
    {
      selectionApply();
      selMode = SEL_NONE;
    }
  }

  plotsInvalidate();
}

// Box: free selection. Axis sections: snapped to the gridlines (see axisSelectionGet).
static void selectionApply(void)
{
  Plot_t *  plot = &plots[selPlot];
  lv_area_t area;
  float     low;
  float     high;

  plotValueArea(plot, &area);

  if ((selMode == SEL_BOX) && (LV_ABS(selEnd.x - selStart.x) >= SEL_MIN_PX))
  {
    int32_t start = xToPosition(plot, &area, LV_MIN(selStart.x, selEnd.x));
    int32_t end   = xToPosition(plot, &area, LV_MAX(selStart.x, selEnd.x));

    if ((end - start) >= (PLOT_VIEW_MIN - 1))
    {
      plot->viewStart = (uint32_t) start;
      plot->viewEnd   = (uint32_t) end;
    }
  }

  if ((selMode == SEL_BOX) && (LV_ABS(selEnd.y - selStart.y) >= SEL_MIN_PX))
  {
    float yMin = yToValue(plot, &area, LV_MAX(selStart.y, selEnd.y));
    float yMax = yToValue(plot, &area, LV_MIN(selStart.y, selEnd.y));

    // Not below the smallest range (yRangeLimit)
    if (yMax > yMin)
    {
      plot->yMin    = yMin;
      plot->yMax    = yMax;
      plot->yZoomed = true;
      yRangeLimit(plot);
    }
  }

  if ((selMode == SEL_X) && (LV_ABS(selEnd.x - selStart.x) >= SEL_MIN_PX) && axisSelectionGet(&low, &high))
  {
    int32_t start = (int32_t) lroundf(low);
    int32_t end   = (int32_t) lroundf(high);

    if ((end - start) >= (PLOT_VIEW_MIN - 1))
    {
      plot->viewStart = (uint32_t) start;
      plot->viewEnd   = (uint32_t) end;
    }
  }

  if ((selMode == SEL_Y) && (LV_ABS(selEnd.y - selStart.y) >= SEL_MIN_PX) && axisSelectionGet(&low, &high))
  {
    plot->yMin    = low;
    plot->yMax    = high;
    plot->yZoomed = true;
    yRangeLimit(plot);
  }

  plotsRender();
}

// All samples, Y ranges following the data
static void zoomReset(int32_t plotIndex)
{
  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    if ((plotIndex == ZOOM_ALL) || ((int32_t) p == plotIndex))
    {
      plots[p].viewStart = 0;
      plots[p].viewEnd   = plotLength - 1;
      plots[p].yZoomed   = false;
    }
  }

  plotsRender();
}

// Fills the editor with the settings of variable v
static void editorLoad(uint32_t v)
{
  char placeholder[PLOT_NAME_LEN];

  editorLoading = true;
  editedVar = v;

  // The button shows the number only (the list: "1  name"); set_text keeps a copy
  char number[8];
  snprintf(number, sizeof(number), "%u", (unsigned) (v + 1));
  lv_dropdown_set_text(varDropdown, number);

  snprintf(placeholder, sizeof(placeholder), "Var %u", (unsigned) (v + 1));
  lv_textarea_set_placeholder_text(nameInput, placeholder);
  lv_textarea_set_text(nameInput, vars[v].name);
  lv_textarea_set_text(unitInput, vars[v].unit);
  colorButtonUpdate(vars[v].color);
  lv_textarea_set_text(gainInput, vars[v].gainText);
  lv_textarea_set_text(offsetInput, vars[v].offsetText);
  inputValidSet(gainInput, true);
  inputValidSet(offsetInput, true);
  lv_dropdown_set_selected(widthDropdown, vars[v].width - LINE_WIDTH_MIN);
  showInChecksSet();

  editorLoading = false;
}

// "Show in" buttons checked for the edited variable's plots (button p = plot p)
static void showInChecksSet(void)
{
  for (uint32_t p = 0; p < plotCount; ++p)
  {
    if ((vars[editedVar].plotMask & (1u << p)) != 0)
    {
      lv_buttonmatrix_set_button_ctrl(showInBtnm, p, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    else
    {
      lv_buttonmatrix_clear_button_ctrl(showInBtnm, p, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
  }
}

// "1  Temp\n2  Var 2\n..." keeping the selection.
// Only the variables received (before any data: those of the connections in use, to set them up
// in advance). The edited variable, when no longer listed, becomes the first one.
static void varDropdownRefresh(void)
{
  char     options[PLOTTER_MAX_VARS * (PLOT_NAME_LEN + 6)] = "";
  size_t   used = 0;
  uint32_t selected = 0;
  bool     found = false;

  if (varDropdown == NULL)
  {
    return;
  }

  varSelCount = 0;

  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    if ((varSeenMask != 0) ? varSeen(v) : (varConn(v) != CONN_NONE))
    {
      varSelMap[varSelCount++] = v;
    }
  }

  for (uint32_t i = 0; (i < varSelCount) && (used < sizeof(options)); ++i)
  {
    char name[PLOT_NAME_LEN];

    varNameGet(varSelMap[i], name, sizeof(name));
    used += (size_t) snprintf(&options[used], sizeof(options) - used, "%s%u  %s", (i > 0) ? "\n" : "",
                              (unsigned) (varSelMap[i] + 1), name);

    if (varSelMap[i] == editedVar)
    {
      selected = i;
      found = true;
    }
  }

  lv_dropdown_set_options(varDropdown, options);
  lv_dropdown_set_selected(varDropdown, selected);

  if (!found && (varSelCount > 0))
  {
    editorLoad(varSelMap[0]);
  }
}

// "2.5" multiplies, "/4" divides, "*3" multiplies
static bool gainParse(const char * text, float * gain)
{
  bool         divide = (text[0] == '/');
  const char * start  = (divide || (text[0] == '*')) ? &text[1] : text;
  float        value;

  if (!numberParse(start, &value) || (divide && (value == 0.0f)))
  {
    return false;
  }

  *gain = divide ? (1.0f / value) : value;
  return true;
}

static bool numberParse(const char * text, float * value)
{
  char * end;

  *value = strtof(text, &end);
  return (end != text) && (*end == '\0') && isfinite(*value);
}

// Red text while the typed number is not valid (the last valid value stays in use)
static void inputValidSet(lv_obj_t * input, bool valid)
{
  if (valid)
  {
    lv_obj_remove_local_style_prop(input, LV_STYLE_TEXT_COLOR, 0);
  }
  else
  {
    lv_obj_set_style_text_color(input, lv_color_hex(COLOR_ERROR), 0);
  }
}

// Background and gridlines, drawn before the chart draws its border and lines (PREPROCESS)
static void chartGridDrawEvent(lv_event_t * e)
{
  Plot_t *           plot  = &plots[(uintptr_t) lv_event_get_user_data(e)];
  lv_layer_t *       layer = lv_event_get_layer(e);
  lv_area_t          coords;
  lv_area_t          area;
  lv_draw_rect_dsc_t bgDsc;
  lv_draw_line_dsc_t lineDsc;

  lv_obj_get_coords(plot->chart, &coords);
  lv_obj_get_content_coords(plot->chart, &area);

  lv_draw_rect_dsc_init(&bgDsc);
  bgDsc.bg_color = lv_obj_get_style_bg_color(plot->chart, LV_PART_MAIN);
  bgDsc.bg_opa   = LV_OPA_COVER;
  bgDsc.radius   = lv_obj_get_style_radius(plot->chart, LV_PART_MAIN);
  lv_draw_rect(layer, &bgDsc, &coords);

  lv_draw_line_dsc_init(&lineDsc);
  lineDsc.width = 1;

  Ticks_t yTicks = yTicksGet(plot, &area);
  for (uint32_t i = 0; i < yTicks.count; ++i)
  {
    float value = plotterTickValue(&yTicks, i);

    lineDsc.color = lv_color_hex((value == 0.0f) ? (uiThemeIsDark() ? COLOR_GRID_ZERO_DARK : COLOR_GRID_ZERO)
                                                 : (uiThemeIsDark() ? COLOR_GRID_DARK : COLOR_GRID));
    lineDsc.p1.x  = area.x1;
    lineDsc.p2.x  = area.x2;
    lineDsc.p1.y  = valueToY(plot, &area, value);
    lineDsc.p2.y  = lineDsc.p1.y;
    lv_draw_line(layer, &lineDsc);
  }

  Ticks_t xTicks = xTicksGet(plot, &area);
  lineDsc.color = lv_color_hex(uiThemeIsDark() ? COLOR_GRID_DARK : COLOR_GRID);
  for (uint32_t i = 0; i < xTicks.count; ++i)
  {
    lineDsc.p1.x = positionToX(plot, &area, plotterTickValue(&xTicks, i));
    lineDsc.p2.x = lineDsc.p1.x;
    lineDsc.p1.y = area.y1;
    lineDsc.p2.y = area.y2;
    lv_draw_line(layer, &lineDsc);
  }
}

// Gives each variable's line its own thickness, in X and Y
static void chartDrawTaskEvent(lv_event_t * e)
{
  lv_draw_task_t *     task = lv_event_get_draw_task(e);
  lv_draw_dsc_base_t * base = lv_draw_task_get_draw_dsc(task);

  if ((lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_LINE) && (base->part == LV_PART_ITEMS) &&
      (base->id1 < PLOTTER_MAX_VARS))
  {
    lv_draw_line_dsc_t * line = lv_draw_task_get_line_dsc(task);

    line->width = LV_MAX(1, PICTURE_PX(vars[base->id1].width));
    uiChartLineThicken(line);
  }
}

// Draws the cursors and the zoom selection over the chart
static void chartDrawPostEvent(lv_event_t * e)
{
  uint32_t            p     = (uintptr_t) lv_event_get_user_data(e);
  Plot_t *            plot  = &plots[p];
  lv_layer_t *        layer = lv_event_get_layer(e);
  lv_area_t           area;
  lv_draw_line_dsc_t  lineDsc;
  lv_draw_label_dsc_t labelDsc;

  lv_obj_get_content_coords(plot->chart, &area);

  lv_draw_line_dsc_init(&lineDsc);
  lineDsc.width      = LV_MAX(1, uiPx(1));
  lineDsc.dash_width = uiPx(4);
  lineDsc.dash_gap   = uiPx(3);

  lv_draw_label_dsc_init(&labelDsc);
  lv_obj_init_draw_label_dsc(plot->chart, LV_PART_MAIN, &labelDsc);
  int32_t lineHeight = lv_font_get_line_height(labelDsc.font);

  for (uint32_t c = 0; c < 2; ++c)
  {
    lv_color_t color = lv_color_hex((c == 1) ? CURSOR_COLOR_2 : (uiThemeIsDark() ? CURSOR_COLOR_1_DARK : CURSOR_COLOR_1));

    lineDsc.color  = color;
    labelDsc.color = color;

    if ((vCursor[c] >= (int32_t) plot->viewStart) && (vCursor[c] <= (int32_t) plot->viewEnd))
    {
      int32_t   x = positionToX(plot, &area, (float) vCursor[c]);
      lv_area_t textArea = {x + uiPx(2), area.y1, x + uiPx(30), area.y1 + lineHeight};

      lineDsc.p1.x = x;
      lineDsc.p1.y = area.y1;
      lineDsc.p2.x = x;
      lineDsc.p2.y = area.y2;
      lv_draw_line(layer, &lineDsc);

      labelDsc.text = cursorNames[TOOL_V1 + c];
      lv_draw_label(layer, &labelDsc, &textArea);
    }

    // Horizontal cursors are values: drawn in every plot whose range contains them
    if (hCursorSet[c])
    {
      if ((hCursor[c] >= plot->yMin) && (hCursor[c] <= plot->yMax))
      {
        int32_t   y = valueToY(plot, &area, hCursor[c]);
        lv_area_t textArea = {area.x2 - uiPx(24), y - lineHeight, area.x2, y};

        lineDsc.p1.x = area.x1;
        lineDsc.p1.y = y;
        lineDsc.p2.x = area.x2;
        lineDsc.p2.y = y;
        lv_draw_line(layer, &lineDsc);

        labelDsc.text = cursorNames[TOOL_H1 + c];
        lv_draw_label(layer, &labelDsc, &textArea);
      }
    }
  }

  // Command marks: dotted, between the samples before / after the command (at the latest sample
  // until one is received after it). The command is written at the bottom (the cursors' names
  // are at the top), centered on the line in a rounded box of its color.
  if (plot->cmdMarks)
  {
    int64_t            base = (int64_t) sampleTotal - (int64_t) plotLength;   // sample number at position 0
    lv_draw_rect_dsc_t textBgDsc;

    lv_draw_rect_dsc_init(&textBgDsc);
    textBgDsc.radius = CMD_MARK_RADIUS;

    // Dotted (the vertical cursors are dashed)
    lineDsc.dash_width = CMD_MARK_DOT;
    lineDsc.dash_gap   = CMD_MARK_DOT_GAP;
    labelDsc.color     = lv_color_white();

    // Oldest first: the latest command's label is drawn over the others
    for (uint32_t i = 0; i < cmdMarkCount; ++i)
    {
      const CmdMark_t * mark     = &cmdMarks[(cmdMarkHead + CMD_MARKS_MAX - cmdMarkCount + i) % CMD_MARKS_MAX];
      float             position = (float) ((int64_t) mark->sample - base) - 0.5f;

      position = LV_MIN(position, (float) (plotLength - 1));

      if ((position < (float) plot->viewStart) || (position > (float) plot->viewEnd))
      {
        continue;
      }

      int32_t    x = positionToX(plot, &area, position);
      lv_point_t size;

      lineDsc.color = lv_color_hex(mark->color);
      lineDsc.p1.x  = x;
      lineDsc.p1.y  = area.y1;
      lineDsc.p2.x  = x;
      lineDsc.p2.y  = area.y2;
      lv_draw_line(layer, &lineDsc);

      // Centered on the line, moved in when it would go past the chart's sides
      lv_text_get_size(&size, mark->text, labelDsc.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      int32_t   boxWidth = size.x + (2 * CMD_MARK_PAD_X);
      int32_t   left     = LV_CLAMP(area.x1, x - (boxWidth / 2), LV_MAX(area.x1, area.x2 + 1 - boxWidth));
      lv_area_t bgArea   = {left, area.y2 - lineHeight, left + boxWidth - 1, area.y2};
      lv_area_t textArea = {left + CMD_MARK_PAD_X, bgArea.y1, left + CMD_MARK_PAD_X + size.x, bgArea.y2};

      textBgDsc.bg_color = lineDsc.color;
      lv_draw_rect(layer, &textBgDsc, &bgArea);
      labelDsc.text = mark->text;
      lv_draw_label(layer, &labelDsc, &textArea);
    }
  }

  // Zoom selection, on the plot it is made on (each plot has its own zoom)
  if ((selMode != SEL_NONE) && (selPlot == p))
  {
    lv_area_t          sel = area;
    lv_draw_rect_dsc_t selDsc;
    float              low;
    float              high;

    if (selMode == SEL_BOX)
    {
      sel.x1 = LV_CLAMP(area.x1, LV_MIN(selStart.x, selEnd.x), area.x2);
      sel.x2 = LV_CLAMP(area.x1, LV_MAX(selStart.x, selEnd.x), area.x2);
      sel.y1 = LV_CLAMP(area.y1, LV_MIN(selStart.y, selEnd.y), area.y2);
      sel.y2 = LV_CLAMP(area.y1, LV_MAX(selStart.y, selEnd.y), area.y2);
    }
    else if (axisSelectionGet(&low, &high))
    {
      // Snapped range (positions / values) back to pixels
      if (selMode == SEL_X)
      {
        sel.x1 = positionToX(plot, &area, low);
        sel.x2 = positionToX(plot, &area, high);
      }
      else
      {
        sel.y1 = valueToY(plot, &area, high);
        sel.y2 = valueToY(plot, &area, low);
      }
    }

    lv_draw_rect_dsc_init(&selDsc);
    selDsc.bg_color     = lv_color_hex(COLOR_SELECTION);
    selDsc.bg_opa       = LV_OPA_20;
    selDsc.border_color = lv_color_hex(COLOR_SELECTION);
    selDsc.border_width = LV_MAX(1, uiPx(1));
    selDsc.border_opa   = LV_OPA_60;
    lv_draw_rect(layer, &selDsc, &sel);
  }
}

// Clicking / dragging on a plot: moves the selected cursor, or draws a zoom box
static void chartPressEvent(lv_event_t * e)
{
  uint32_t   p    = (uintptr_t) lv_event_get_user_data(e);
  Plot_t *   plot = &plots[p];
  lv_area_t  area;
  lv_point_t point;

  if (tool == TOOL_ZOOM)
  {
    selectionHandle(e, SEL_BOX, p);
    return;
  }

  if ((tool == TOOL_NONE) || (lv_event_get_code(e) == LV_EVENT_RELEASED))
  {
    return;
  }

  lv_indev_get_point(lv_indev_active(), &point);
  lv_obj_get_content_coords(plot->chart, &area);

  if ((lv_area_get_width(&area) <= 0) || (lv_area_get_height(&area) <= 0))
  {
    return;
  }

  if (tool <= TOOL_V2)
  {
    vCursor[tool - TOOL_V1] = xToPosition(plot, &area, point.x);
  }
  else
  {
    hCursor[tool - TOOL_H1]    = yToValue(plot, &area, point.y);
    hCursorSet[tool - TOOL_H1] = true;
  }

  legendUpdate();
  readoutUpdate();
  plotsInvalidate();
}

// Gridline values, right aligned next to the chart
static void yAxisDrawEvent(lv_event_t * e)
{
  Plot_t *            plot  = &plots[(uintptr_t) lv_event_get_user_data(e)];
  lv_layer_t *        layer = lv_event_get_layer(e);
  lv_area_t           chartArea;
  lv_area_t           axisArea;
  lv_draw_label_dsc_t labelDsc;

  plotValueArea(plot, &chartArea);
  lv_obj_get_coords(plot->yAxis, &axisArea);

  lv_draw_label_dsc_init(&labelDsc);
  lv_obj_init_draw_label_dsc(plot->yAxis, LV_PART_MAIN, &labelDsc);
  labelDsc.color      = lv_color_hex(COLOR_INFO);
  labelDsc.align      = LV_TEXT_ALIGN_RIGHT;
  labelDsc.text_local = 1;

  Ticks_t ticks = yTicksGet(plot, &chartArea);
  int32_t lineHeight = lv_font_get_line_height(labelDsc.font);

  for (uint32_t i = 0; i < ticks.count; ++i)
  {
    char      text[16];
    float     value = plotterTickValue(&ticks, i);
    int32_t   y = valueToY(plot, &chartArea, value);
    int32_t   top = uiAxisLabelTop(y, lineHeight, &axisArea);
    lv_area_t textArea = {axisArea.x1, top, axisArea.x2 - uiPx(3), top + lineHeight};

    snprintf(text, sizeof(text), "%.*f", ticks.decimals, (double) value);
    labelDsc.text = text;
    lv_draw_label(layer, &labelDsc, &textArea);
  }
}

static void yAxisPressEvent(lv_event_t * e)
{
  selectionHandle(e, SEL_Y, (uintptr_t) lv_event_get_user_data(e));
}

// Sample positions below the chart, and variable names below the bars (one per bar slot)
static void xAxisDrawEvent(lv_event_t * e)
{
  uint32_t            p     = (uintptr_t) lv_event_get_user_data(e);
  Plot_t *            plot  = &plots[p];
  lv_layer_t *        layer = lv_event_get_layer(e);
  lv_area_t           chartArea;
  lv_area_t           axisArea;
  lv_draw_label_dsc_t labelDsc;

  lv_obj_get_content_coords(plot->chart, &chartArea);
  lv_obj_get_coords(plot->xAxis, &axisArea);

  lv_draw_label_dsc_init(&labelDsc);
  lv_obj_init_draw_label_dsc(plot->xAxis, LV_PART_MAIN, &labelDsc);
  labelDsc.color      = lv_color_hex(COLOR_INFO);
  labelDsc.align      = LV_TEXT_ALIGN_CENTER;
  labelDsc.text_local = 1;

  if (!lv_obj_is_hidden(plot->bars))
  {
    lv_area_t barsArea;
    uint32_t  count = barsCount(p);

    lv_obj_get_coords(plot->bars, &barsArea);
    int32_t slot = barSlotWidth(plot, &barsArea, count);
    int32_t x    = barsArea.x1;

    for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
    {
      if (varInPlot(v, p))
      {
        char      name[PLOT_NAME_LEN];
        lv_area_t textArea = {x, axisArea.y1, x + slot - 1, axisArea.y2};

        // In the variable's color, like its bar
        varNameGet(v, name, sizeof(name));
        labelDsc.text  = name;
        labelDsc.color = lv_color_hex(vars[v].color);
        lv_draw_label(layer, &labelDsc, &textArea);
        x += slot;
      }
    }
  }

  if (!plot->chartVisible)
  {
    return;
  }

  labelDsc.color = lv_color_hex(COLOR_INFO);

  Ticks_t ticks = xTicksGet(plot, &chartArea);
  int     decimals = positionDecimals(plot);

  for (uint32_t i = 0; i < ticks.count; ++i)
  {
    char      text[24];
    float     position = plotterTickValue(&ticks, i);
    int32_t   x = positionToX(plot, &chartArea, position);

    positionLabel(plot->xAxisMode, (uint32_t) lroundf(position), decimals, text, sizeof(text));

    lv_area_t textArea = uiAxisLabelArea(text, labelDsc.font, x, &axisArea);

    labelDsc.text = text;
    lv_draw_label(layer, &labelDsc, &textArea);
  }
}

// Tick (time received) of the sample at position; false where the history has no sample yet
static bool positionTick(uint32_t position, uint32_t * tick)
{
  uint32_t kept = LV_MIN(historyCount, plotLength);

  if ((position >= plotLength) || (position < (plotLength - kept)))
  {
    return false;
  }

  *tick = historyTime[historyIndex(position)];
  return true;
}

// Decimals of the time labels of a plot: finer when its view covers less time
static int positionDecimals(const Plot_t * plot)
{
  uint32_t first;
  uint32_t last;
  uint32_t start = plot->viewStart;

  // First position with a sample (the view may start before the history)
  while ((start < plot->viewEnd) && !positionTick(start, &first))
  {
    start++;
  }

  if (!positionTick(start, &first) || !positionTick(plot->viewEnd, &last))
  {
    return 0;
  }

  uint32_t span = last - first;
  return (span < 2000) ? 2 : ((span < 20000) ? 1 : 0);
}

// Label of position on the X axis / in the readout: sample number, seconds before the latest
// sample ("-1.5 s"), or time of day ("12:03:15.2"); "" for time where there is no sample
static void positionLabel(uint32_t mode, uint32_t position, int decimals, char * text, size_t size)
{
  uint32_t tick;
  uint32_t latest = historyTime[(historyHead + PLOT_POINTS_MAX - 1) % PLOT_POINTS_MAX];

  if (mode == X_AXIS_SAMPLES)
  {
    snprintf(text, size, "%lu", (unsigned long) position);
  }
  else if (!positionTick(position, &tick))
  {
    text[0] = '\0';
  }
  else if (mode == X_AXIS_SECONDS)
  {
    snprintf(text, size, "%.*f s", decimals, (double) (int32_t) (tick - latest) / 1000.0);
  }
  else
  {
    // Wall clock: base time + time since the base (ticks wrap after 49 days)
    int64_t   ms = clockBaseMs + (int32_t) (tick - clockBaseTick);
    time_t    seconds = (time_t) (ms / 1000);
    struct tm local;

    local = *localtime(&seconds);

    if (decimals > 0)
    {
      int32_t fraction = (int32_t) (ms % 1000);
      for (int d = decimals; d < 3; ++d)
      {
        fraction /= 10;
      }
      snprintf(text, size, "%02d:%02d:%02d.%0*ld", local.tm_hour, local.tm_min, local.tm_sec, decimals, (long) fraction);
    }
    else
    {
      snprintf(text, size, "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    }
  }
}

// X axis labels of the edited plot (saved); its axis redrawn, the gridlines' spacing following the
// label width (and the cursor readout, which follows P1)
static void xAxisModeEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  plots[editedPlot].xAxisMode = lv_dropdown_get_selected(xAxisDropdown);
  plotSettingsSave(editedPlot);
  readoutUpdate();
  plotsInvalidate();
}

// X zoom only applies to the chart
static void xAxisPressEvent(lv_event_t * e)
{
  uint32_t p = (uintptr_t) lv_event_get_user_data(e);

  if (plots[p].chartVisible)
  {
    selectionHandle(e, SEL_X, p);
  }
}

// Bar graph: one slot per variable with a bar from 0 to the value (plot's Y scale) and the value
// above it (below it for a negative value), or inside the bar when there is no room outside.
// The names are drawn below, in the X axis (xAxisDrawEvent).
static void barsDrawEvent(lv_event_t * e)
{
  uint32_t            p     = (uintptr_t) lv_event_get_user_data(e);
  Plot_t *            plot  = &plots[p];
  lv_layer_t *        layer = lv_event_get_layer(e);
  uint32_t            index = historyIndex(shownPosition());
  uint32_t            count = barsCount(p);
  lv_area_t           valueArea;
  lv_area_t           barsArea;
  lv_draw_rect_dsc_t  rectDsc;
  lv_draw_label_dsc_t labelDsc;

  if (count == 0)
  {
    return;
  }

  plotValueArea(plot, &valueArea);
  lv_obj_get_coords(plot->bars, &barsArea);

  lv_draw_rect_dsc_init(&rectDsc);
  rectDsc.bg_opa = LV_OPA_COVER;
  rectDsc.radius = uiPx(2);

  lv_draw_label_dsc_init(&labelDsc);
  lv_obj_init_draw_label_dsc(plot->bars, LV_PART_MAIN, &labelDsc);
  labelDsc.align      = LV_TEXT_ALIGN_CENTER;
  labelDsc.text_local = 1;

  int32_t lineHeight = lv_font_get_line_height(labelDsc.font);
  int32_t slot  = barSlotWidth(plot, &barsArea, count);
  int32_t width = LV_MAX(uiPx(4), slot - (2 * BAR_GAP));
  int32_t zeroY = valueToY(plot, &valueArea, 0.0f);
  int32_t x     = barsArea.x1;
  int     decimals = valueDecimals(plot);

  for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
  {
    if (!varInPlot(v, p))
    {
      continue;
    }

    float value = displayValue(v, history[v][index]);

    if (valueDrawable(value))
    {
      int32_t   y = valueToY(plot, &valueArea, value);
      lv_area_t bar = {x + ((slot - width) / 2), LV_MIN(y, zeroY), x + ((slot + width) / 2) - 1, LV_MAX(y, zeroY)};
      lv_area_t text = {x, 0, x + slot - 1, 0};
      bool      inside = false;
      char      valueText[24];

      rectDsc.bg_color = lv_color_hex(vars[v].color);
      lv_draw_rect(layer, &rectDsc, &bar);

      if (value >= 0.0f)
      {
        text.y2 = bar.y1 - 1;
        text.y1 = text.y2 - lineHeight;
        inside  = text.y1 < barsArea.y1;
        if (inside)
        {
          text.y1 = bar.y1 + 1;
          text.y2 = text.y1 + lineHeight;
        }
      }
      else
      {
        text.y1 = bar.y2 + 1;
        text.y2 = text.y1 + lineHeight;
        inside  = text.y2 > barsArea.y2;
        if (inside)
        {
          text.y2 = bar.y2 - 1;
          text.y1 = text.y2 - lineHeight;
        }
      }

      snprintf(valueText, sizeof(valueText), "%.*f", decimals, (double) value);
      labelDsc.text  = valueText;
      labelDsc.color = inside ? lv_color_white() : lv_color_hex(vars[v].color);
      lv_draw_label(layer, &labelDsc, &text);
    }

    x += slot;
  }
}

static void plotsBoxSizeEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  windowsApply();
  renderPending = true;   // groups follow the charts' widths
}

// Title bar dragged: the window follows the pointer, its edges snapping to the plot area and to
// the other windows; kept inside the plot area
static void windowMoveEvent(lv_event_t * e)
{
  uint32_t   w = (uintptr_t) lv_event_get_user_data(e);
  lv_point_t point;
  int32_t    targets[SNAP_TARGETS_MAX];

  lv_indev_get_point(lv_indev_active(), &point);

  if (lv_event_get_code(e) == LV_EVENT_PRESSED)
  {
    windowRaise(w);
    windowRectGet(w, &dragStartRect);
    dragStartPoint = point;
    windowDragging = true;
    windowMoving   = true;

    // Closed hand right away (the cursor is otherwise chosen when the mouse moves)
    appWindowCursorRefresh();
    return;
  }

  if (lv_event_get_code(e) == LV_EVENT_RELEASED)
  {
    windowDragging = false;
    windowMoving   = false;
    appWindowCursorRefresh();
    geomSave(w);
    return;
  }

  int32_t   half   = WINDOW_GAP / 2;
  int32_t   width  = lv_obj_get_content_width(plotsBox);
  int32_t   height = lv_obj_get_content_height(plotsBox);
  lv_area_t rect   = dragStartRect;
  int32_t   dx     = point.x - dragStartPoint.x;
  int32_t   dy     = point.y - dragStartPoint.y;

  rect.x1 += dx;
  rect.x2 += dx;
  rect.y1 += dy;
  rect.y2 += dy;

  dx = snapPair(rect.x1, rect.x2, targets, snapTargetsGet(w, true, targets));
  dy = snapPair(rect.y1, rect.y2, targets, snapTargetsGet(w, false, targets));

  // Inside the plot area
  dx += LV_MAX(0, half - (rect.x1 + dx)) - LV_MAX(0, (rect.x2 + dx) - (width - half - 1));
  dy += LV_MAX(0, half - (rect.y1 + dy)) - LV_MAX(0, (rect.y2 + dy) - (height - half - 1));

  rect.x1 += dx;
  rect.x2 += dx;
  rect.y1 += dy;
  rect.y2 += dy;
  windowRectSet(w, &rect);
}

// Corner handle dragged: the right / bottom edges follow the pointer and snap, down to the
// minimum window size and inside the plot area
static void windowResizeEvent(lv_event_t * e)
{
  uint32_t   w = (uintptr_t) lv_event_get_user_data(e);
  lv_point_t point;
  int32_t    targets[SNAP_TARGETS_MAX];
  int32_t    delta = 0;

  lv_indev_get_point(lv_indev_active(), &point);

  if (lv_event_get_code(e) == LV_EVENT_PRESSED)
  {
    windowRaise(w);
    windowRectGet(w, &dragStartRect);
    dragStartPoint = point;
    windowResizing = true;
    windowDragging = true;
    return;
  }

  if (lv_event_get_code(e) == LV_EVENT_RELEASED)
  {
    windowResizing = false;
    windowDragging = false;
    geomSave(w);
    return;
  }

  int32_t   half   = WINDOW_GAP / 2;
  int32_t   width  = lv_obj_get_content_width(plotsBox);
  int32_t   height = lv_obj_get_content_height(plotsBox);
  lv_area_t rect   = dragStartRect;

  rect.x2 += point.x - dragStartPoint.x;
  rect.y2 += point.y - dragStartPoint.y;

  if (snapDelta(rect.x2, targets, snapTargetsGet(w, true, targets), &delta))
  {
    rect.x2 += delta;
  }

  if (snapDelta(rect.y2, targets, snapTargetsGet(w, false, targets), &delta))
  {
    rect.y2 += delta;
  }

  rect.x2 = LV_CLAMP(rect.x1 + WINDOW_MIN_W - 1, rect.x2, width - half - 1);
  rect.y2 = LV_CLAMP(rect.y1 + WINDOW_MIN_H - 1, rect.y2, height - half - 1);
  windowRectSet(w, &rect);
}

// Seam dragged (in a gap or on a window border): the edges on the seam follow the pointer; at a
// crossing both the vertical and the horizontal seam move
static void seamEvent(lv_event_t * e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_point_t      point;
  lv_area_t       box;

  lv_indev_get_point(lv_indev_active(), &point);
  lv_obj_get_content_coords(plotsBox, &box);
  point.x -= box.x1;
  point.y -= box.y1;

  if (code == LV_EVENT_PRESSED)
  {
    bool vertical   = seamFind(point, true, &seamLeftMask, &seamRightMask);
    bool horizontal = seamFind(point, false, &seamAboveMask, &seamBelowMask);

    for (uint32_t w = 0; w < WIN_COUNT; ++w)
    {
      windowRectGet(w, &seamStartRects[w]);
    }

    dragStartPoint = point;
    windowDragging = vertical || horizontal;
    return;
  }

  uint16_t moved = seamLeftMask | seamRightMask | seamAboveMask | seamBelowMask;

  if (moved == 0)
  {
    return;
  }

  if (code == LV_EVENT_RELEASED)
  {
    for (uint32_t w = 0; w < WIN_COUNT; ++w)
    {
      if ((moved & (1u << w)) != 0)
      {
        geomSave(w);
      }
    }

    seamLeftMask = seamRightMask = seamAboveMask = seamBelowMask = 0;
    windowDragging = false;
    return;
  }

  int32_t dx = seamShiftLimit(point.x - dragStartPoint.x, true);
  int32_t dy = seamShiftLimit(point.y - dragStartPoint.y, false);

  for (uint32_t w = 0; w < WIN_COUNT; ++w)
  {
    lv_area_t rect = seamStartRects[w];
    uint16_t  bit  = (uint16_t) (1u << w);

    if ((moved & bit) == 0)
    {
      continue;
    }

    rect.x2 += ((seamLeftMask & bit) != 0) ? dx : 0;
    rect.x1 += ((seamRightMask & bit) != 0) ? dx : 0;
    rect.y2 += ((seamAboveMask & bit) != 0) ? dy : 0;
    rect.y1 += ((seamBelowMask & bit) != 0) ? dy : 0;
    windowRectSet(w, &rect);
  }
}

// Divider dragged: the chart / bars border follows the pointer, both keep a minimum width and
// together still fill the window
static void dividerEvent(lv_event_t * e)
{
  uint32_t   p    = (uintptr_t) lv_event_get_user_data(e);
  Plot_t *   plot = &plots[p];
  lv_point_t point;

  lv_indev_get_point(lv_indev_active(), &point);

  if (lv_event_get_code(e) == LV_EVENT_PRESSED)
  {
    dividerStartBars  = lv_obj_get_width(plot->bars);
    dividerStartTotal = lv_obj_get_width(plot->chart) + dividerStartBars;
    dragStartPoint    = point;
    dividerDragging   = true;
    windowDragging    = true;
    return;
  }

  if (lv_event_get_code(e) == LV_EVENT_RELEASED)
  {
    dividerDragging = false;
    windowDragging  = false;
    plotSettingsSave(p);
    return;
  }

  int32_t bars = dividerStartBars - (point.x - dragStartPoint.x);

  bars = LV_CLAMP(BARS_MIN_WIDTH, bars, dividerStartTotal - CHART_MIN_WIDTH);
  plot->barsShare = LV_CLAMP(BARS_SHARE_MIN, (bars * GEOM_FULL) / LV_MAX(dividerStartTotal, 1), BARS_SHARE_MAX);
  barsLayoutUpdate();
}

// Short vertical line in the middle of the divider
static void dividerDrawEvent(lv_event_t * e)
{
  lv_obj_t *         divider = lv_event_get_target_obj(e);
  lv_layer_t *       layer   = lv_event_get_layer(e);
  lv_area_t          area;
  lv_draw_line_dsc_t lineDsc;

  lv_obj_get_coords(divider, &area);
  lv_draw_line_dsc_init(&lineDsc);
  lineDsc.color = lv_color_hex(COLOR_INFO);
  lineDsc.width = LV_MAX(1, uiPx(2));
  lineDsc.opa   = LV_OPA_60;
  lineDsc.round_start = 1;
  lineDsc.round_end   = 1;

  int32_t x = (area.x1 + area.x2) / 2;
  int32_t middle = (area.y1 + area.y2) / 2;
  int32_t half = LV_MIN(uiPx(16), lv_area_get_height(&area) / 4);

  lineDsc.p1.x = x;
  lineDsc.p1.y = middle - half;
  lineDsc.p2.x = x;
  lineDsc.p2.y = middle + half;
  lv_draw_line(layer, &lineDsc);
}

// Two diagonal lines in the corner
static void resizeHandleDrawEvent(lv_event_t * e)
{
  lv_obj_t *         handle = lv_event_get_target_obj(e);
  lv_layer_t *       layer  = lv_event_get_layer(e);
  lv_area_t          area;
  lv_draw_line_dsc_t lineDsc;

  lv_obj_get_coords(handle, &area);
  lv_draw_line_dsc_init(&lineDsc);
  lineDsc.color = lv_color_hex(COLOR_INFO);
  lineDsc.width = LV_MAX(1, uiPx(1));

  for (int32_t i = 1; i <= 2; ++i)
  {
    int32_t offset = (lv_area_get_width(&area) * i) / 3;

    lineDsc.p1.x = area.x2 - offset;
    lineDsc.p1.y = area.y2;
    lineDsc.p2.x = area.x2;
    lineDsc.p2.y = area.y2 - offset;
    lv_draw_line(layer, &lineDsc);
  }
}

static void tileEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  plotsTile();
}

static void chartSwitchEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  plots[editedPlot].chartVisible = lv_obj_has_state(chartSwitch, LV_STATE_CHECKED);
  plotSettingsSave(editedPlot);
  barsLayoutUpdate();
  plotsRender();
}

static void toolEvent(lv_event_t * e)
{
  uint32_t id = lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e));

  if ((id != LV_BUTTONMATRIX_BUTTON_NONE) && lv_buttonmatrix_has_button_ctrl(toolBtnm, id, LV_BUTTONMATRIX_CTRL_CHECKED))
  {
    tool = (int32_t) id;
  }
  else
  {
    tool = TOOL_NONE;
  }
}

static void cursorsClearEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  vCursor[0] = CURSOR_NONE;
  vCursor[1] = CURSOR_NONE;
  hCursorSet[0] = false;
  hCursorSet[1] = false;
  tool = TOOL_NONE;
  lv_buttonmatrix_clear_button_ctrl_all(toolBtnm, LV_BUTTONMATRIX_CTRL_CHECKED);

  legendUpdate();
  readoutUpdate();
  plotsInvalidate();
}

static void zoomResetEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  zoomReset(ZOOM_ALL);
}

static void varsOpenEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_hidden(varsPanel, false);
  uiKeypadGroupSet(varsGroup);
}

static void varsCloseEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_hidden(varsPanel, true);
  uiKeypadGroupSet(plotterKeypadGroup());
}

// Shown where it was (its saved geometry), above the plots
static void cmdPanelSwitchEvent(lv_event_t * e)
{
  bool shown = lv_obj_has_state(cmdPanelSwitch, LV_STATE_CHECKED);

  LV_UNUSED(e);
  lv_obj_set_hidden(cmdPanel, !shown);

  if (shown)
  {
    windowRaise(WIN_CMD);
  }
}

static void cmdMarksSwitchEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  plots[editedPlot].cmdMarks = lv_obj_has_state(cmdMarksSwitch, LV_STATE_CHECKED);
  plotSettingsSave(editedPlot);
  lv_obj_invalidate(plots[editedPlot].chart);
}

// Closing the panel turns its switch off (saved)
static void cmdPanelCloseEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_state(cmdPanelSwitch, LV_STATE_CHECKED, false);
  lv_obj_send_event(cmdPanelSwitch, LV_EVENT_VALUE_CHANGED, NULL);
  uiKeypadGroupSet(plotterKeypadGroup());
}

static void pauseEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  paused = !paused;
  lv_label_set_text(pauseBtnLabel, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void clearEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  plotterClear();
}

// Exports the kept samples (up to PLOT_POINTS_MAX, since the last clear) of the variables received,
// as displayed (gain and offset applied). The copy is taken now: data keeps coming meanwhile.
static void exportEvent(lv_event_t * e)
{
  static char headers[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  uint32_t    columns[PLOTTER_MAX_VARS];      // variable of each column
  uint32_t    columnCount = plotterVarHeaders(columns, headers);

  LV_UNUSED(e);

  if ((historyCount == 0) || (columnCount == 0))
  {
    toastShow(tr("No data to export."));
    return;
  }

  PlotExport_t * data = plotExportAlloc(historyCount, columnCount);

  if (data == NULL)
  {
    toastShow(tr("Not enough memory to export."));
    return;
  }

  data->firstSample = sampleTotal - historyCount + 1;
  memcpy(data->headers, headers, sizeof(headers[0]) * columnCount);

  // Oldest first
  for (uint32_t s = 0; s < historyCount; ++s)
  {
    uint32_t index = (historyHead + PLOT_POINTS_MAX - historyCount + s) % PLOT_POINTS_MAX;
    float *  row   = &data->values[(size_t) s * columnCount];

    data->ticks[s] = historyTime[index];
    for (uint32_t c = 0; c < columnCount; ++c)
    {
      float raw = history[columns[c]][index];
      row[c] = isnan(raw) ? NAN : displayValue(columns[c], raw);
    }
  }

  plotterExport(data);
}

void plotterExport(PlotExport_t * data)
{
  if (!plotExportStart(data))
  {
    toastShow(tr("Could not start the export."));
    return;
  }

  // One export at a time: the button is back when the result is in
  lv_obj_set_state(exportBtn, LV_STATE_DISABLED, true);
  lv_timer_create(exportPollTimer, EXPORT_POLL_MS, NULL);
}

void plotterToast(const char * text)
{
#ifdef SERIAL_DASHBOARD_CLI
  cliMessage(text);
#endif
  toastShow(text);
}

// Sizes of the plots at pictureScale
static void pictureStylesSet(const lv_font_t * font)
{
  lv_obj_set_style_text_font(plotsBox, font, 0);

  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    lv_obj_set_height(plots[p].xAxis, AXIS_X_HEIGHT);
    lv_obj_set_style_line_width(plots[p].chart, PICTURE_PX(LINE_WIDTH_MAX), LV_PART_ITEMS);

    for (uint32_t v = 0; v < PLOTTER_MAX_VARS; ++v)
    {
      lv_obj_set_style_pad_left(plots[p].legendLabels[v], PICTURE_PX(UI_LEGEND_DOT_PAD), 0);
    }
  }
}

// For a picture, only the charts: without the commands window, the bars and the windows' handles,
// the plots stretched to fill the picture as they are arranged. false: as they were.
static void pictureLayoutSet(bool picture)
{
  static int32_t savedGeom[WIN_COUNT][4];
  static bool    savedBars[PLOT_COUNT_MAX];
  static bool    savedCmdHidden;

  if (picture)
  {
    int32_t left   = GEOM_FULL;
    int32_t top    = GEOM_FULL;
    int32_t right  = 0;
    int32_t bottom = 0;

    memcpy(savedGeom, winGeom, sizeof(winGeom));
    savedCmdHidden = lv_obj_is_hidden(cmdPanel);
    lv_obj_set_hidden(cmdPanel, true);

    for (uint32_t p = 0; p < plotCount; ++p)
    {
      left   = LV_MIN(left, winGeom[p][0]);
      top    = LV_MIN(top, winGeom[p][1]);
      right  = LV_MAX(right, winGeom[p][0] + winGeom[p][2]);
      bottom = LV_MAX(bottom, winGeom[p][1] + winGeom[p][3]);
    }

    int32_t width  = LV_MAX(right - left, 1);
    int32_t height = LV_MAX(bottom - top, 1);

    for (uint32_t p = 0; p < plotCount; ++p)
    {
      winGeom[p][0] = ((winGeom[p][0] - left) * GEOM_FULL) / width;
      winGeom[p][1] = ((winGeom[p][1] - top) * GEOM_FULL) / height;
      winGeom[p][2] = (winGeom[p][2] * GEOM_FULL) / width;
      winGeom[p][3] = (winGeom[p][3] * GEOM_FULL) / height;
    }

    for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
    {
      savedBars[p] = plots[p].barsVisible;

      // A plot with bars only keeps them
      plots[p].barsVisible = plots[p].barsVisible && !plots[p].chartVisible;
    }
  }
  else
  {
    memcpy(winGeom, savedGeom, sizeof(winGeom));
    lv_obj_set_hidden(cmdPanel, savedCmdHidden);

    for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
    {
      plots[p].barsVisible = savedBars[p];
    }
  }

  for (uint32_t w = 0; w < WIN_COUNT; ++w)
  {
    lv_obj_set_hidden(winHandle[w], picture);
  }

  barsLayoutUpdate();
  windowsApply();
}

// All the plot windows, as they are arranged. For a report they are laid out at the picture's size,
// on white, with only the charts; nothing of it reaches the screen.
static void screenshotEvent(lv_event_t * e)
{
  BenchPicture_t picture;
  bool           sized = benchPictureGet(&picture);

  LV_UNUSED(e);

  if (sized)
  {
    pictureScale = picture.scale;
    uiThemeLightSet(true);
    lv_obj_set_flex_grow(plotsBox, 0);
    lv_obj_set_size(plotsBox, picture.width, picture.height);
    pictureStylesSet(picture.font);
    lv_obj_update_layout(plotsBox);
    pictureLayoutSet(true);
    lv_obj_update_layout(plotsBox);
  }

  plotsRender();
  lv_obj_update_layout(plotsBox);
  benchScreenshot(plotsBox, tr("Plot"));

  if (sized)
  {
    pictureScale = 100;
    lv_obj_set_size(plotsBox, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(plotsBox, 1);
    pictureStylesSet(uiFont());
    uiThemeLightSet(false);
    pictureLayoutSet(false);
    lv_obj_update_layout(plotsBox);
    plotsRender();
  }
}

static void exportPollTimer(lv_timer_t * tim)
{
  char message[PLOT_EXPORT_MESSAGE_LEN];

  if (plotExportPoll(message, sizeof(message)) != PLOT_EXPORT_DONE)
  {
    return;
  }

  lv_timer_delete(tim);
  lv_obj_set_state(exportBtn, LV_STATE_DISABLED, false);

  // Empty: the dialog was canceled
  if (message[0] != '\0')
  {
    toastShow(message);
  }
}

// Message over the bottom of the window, whatever the tab, for TOAST_MS (or until clicked)
static void toastShow(const char * text)
{
  if (toastLabel == NULL)
  {
    toastLabel = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(toastLabel, uiFont(), 0);
    lv_obj_set_style_max_width(toastLabel, LV_PCT(90), 0);
    lv_label_set_long_mode(toastLabel, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_bg_color(toastLabel, lv_color_hex(COLOR_BAR_BG), 0);
    lv_obj_set_style_bg_opa(toastLabel, LV_OPA_90, 0);
    lv_obj_set_style_text_color(toastLabel, lv_color_hex(COLOR_BAR_TEXT), 0);
    lv_obj_set_style_pad_hor(toastLabel, uiPx(10), 0);
    lv_obj_set_style_pad_ver(toastLabel, uiPx(6), 0);
    lv_obj_set_style_radius(toastLabel, uiPx(6), 0);
    lv_obj_set_clickable(toastLabel, true);
    lv_obj_add_event_cb(toastLabel, toastClickEvent, LV_EVENT_CLICKED, NULL);

    toastTimer = lv_timer_create(toastHideTimer, TOAST_MS, NULL);
    lv_timer_pause(toastTimer);
  }

  lv_obj_set_size(toastLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_label_set_text(toastLabel, text);
  lv_obj_update_layout(toastLabel);

  // Long text: wrapped at 90 % of the window, its height measured (the label keeps one line)
  int32_t maxWidth = (lv_obj_get_width(lv_layer_top()) * 9) / 10;

  if (lv_obj_get_width(toastLabel) >= maxWidth)
  {
    lv_point_t size;

    lv_text_get_size(&size, text, uiFont(), 0, 0, maxWidth - (2 * uiPx(10)), LV_TEXT_FLAG_NONE);
    lv_obj_set_size(toastLabel, maxWidth, size.y + (2 * uiPx(6)));
    lv_obj_update_layout(toastLabel);
  }

  lv_obj_align(toastLabel, LV_ALIGN_BOTTOM_MID, 0, -uiPx(12));
  lv_obj_set_hidden(toastLabel, false);
  lv_obj_move_foreground(toastLabel);
  lv_timer_reset(toastTimer);
  lv_timer_resume(toastTimer);
}

static void toastHideTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);
  lv_obj_set_hidden(toastLabel, true);
  lv_timer_pause(toastTimer);
}

static void toastClickEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  toastHideTimer(toastTimer);
}

static void plotCountEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  plotCount = lv_dropdown_get_selected(plotCountDropdown) + 1;
  plotsTile();
  plotsLayoutUpdate();
  plotSelRefresh();
}

static void plotLengthEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  plotLengthApply();
  plotsRender();
}

// New samples are drawn after a window move / resize, which then follows the pointer smoothly
static void renderTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  // The plots are not on screen (other tab): drawn when they are (the samples are kept)
  if (renderPending && !windowDragging && lv_obj_is_visible(plotsBox))
  {
    plotsRender();
  }
}

static void plotSelEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  plotEditorLoad(lv_dropdown_get_selected(plotSelDropdown));
}

// Saved, shown in the plot's title bar and in the plot list
static void plotNameEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  snprintf(plots[editedPlot].name, PLOT_NAME_LEN, "%s", lv_textarea_get_text(plotNameInput));
  plotSettingsSave(editedPlot);
  plotTitleUpdate(editedPlot);
  plotSelRefresh();
}

// Auto switch, Min or Max changed: the manual scale is used when Auto is off and the range is
// valid (min < max); an invalid number is shown in red and the plot keeps its last scale
static void yScaleEvent(lv_event_t * e)
{
  Plot_t * plot = &plots[editedPlot];
  bool     manual = !lv_obj_has_state(yAutoSwitch, LV_STATE_CHECKED);
  float    min;
  float    max;

  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  snprintf(plot->yMinText, PLOT_NUMBER_LEN, "%s", lv_textarea_get_text(yMinInput));
  snprintf(plot->yMaxText, PLOT_NUMBER_LEN, "%s", lv_textarea_get_text(yMaxInput));

  bool minValid = numberParse(plot->yMinText, &min) && (fabsf(min) <= PLOT_VALUE_MAX);
  bool maxValid = numberParse(plot->yMaxText, &max) && (fabsf(max) <= PLOT_VALUE_MAX);
  bool rangeValid = minValid && maxValid && (max > min);

  inputValidSet(yMinInput, !manual || (minValid && rangeValid));
  inputValidSet(yMaxInput, !manual || (maxValid && rangeValid));
  lv_obj_set_state(yMinInput, LV_STATE_DISABLED, !manual);
  lv_obj_set_state(yMaxInput, LV_STATE_DISABLED, !manual);

  if (!manual)
  {
    plot->yManual = false;
  }
  else if (rangeValid)
  {
    plot->yManual    = true;
    plot->yManualMin = min;
    plot->yManualMax = max;
  }

  // The new scale replaces a zoom of this plot
  plot->yZoomed = false;
  plotSettingsSave(editedPlot);
  plotsRender();
}

static void barsSwitchEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  plots[editedPlot].barsVisible = lv_obj_has_state(barsSwitch, LV_STATE_CHECKED);
  plotSettingsSave(editedPlot);
  barsLayoutUpdate();
}

static void varSelectedEvent(lv_event_t * e)
{
  uint32_t selected = lv_dropdown_get_selected(varDropdown);

  LV_UNUSED(e);

  if (selected < varSelCount)
  {
    editorLoad(varSelMap[selected]);
  }
}

static void nameEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  snprintf(vars[editedVar].name, PLOT_NAME_LEN, "%s", lv_textarea_get_text(nameInput));
  varSave(editedVar);
  varDropdownRefresh();
  legendUpdate();
}

static void unitEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  snprintf(vars[editedVar].unit, PLOT_UNIT_LEN, "%s", lv_textarea_get_text(unitInput));
  varSave(editedVar);
  legendUpdate();
}

// The chosen preset becomes the variable's color (saved)
static void colorPresetEvent(lv_event_t * e)
{
  uint32_t preset = lv_dropdown_get_selected(colorPresetDropdown);

  LV_UNUSED(e);
  vars[editedVar].color = (preset == COLOR_PRESET_DEFAULT) ? varDefaultColors[editedVar] : varDefaultColors[preset - 1];
  varSave(editedVar);
  colorApply(editedVar);
}

// Color button filled with color, its arrow in black or white (the one readable on it)
static void colorButtonUpdate(uint32_t color)
{
  uint32_t luma = ((((color >> 16) & 0xFF) * 299) + (((color >> 8) & 0xFF) * 587) + ((color & 0xFF) * 114)) / 1000;

  lv_color_t arrow = (luma > 150) ? lv_color_black() : lv_color_white();

  lv_obj_set_style_bg_color(colorPresetDropdown, lv_color_hex(color), 0);
  lv_obj_set_style_text_color(colorPresetDropdown, arrow, 0);
  lv_obj_set_style_text_color(colorPresetDropdown, arrow, LV_PART_INDICATOR);
}

// 6 hex digits, optionally after '#'
static bool colorParse(const char * text, uint32_t * color)
{
  char * end;

  text = (text[0] == '#') ? &text[1] : text;

  if (strlen(text) != 6)
  {
    return false;
  }

  *color = (uint32_t) strtoul(text, &end, 16);
  return *end == '\0';
}

// New color of variable v: its lines, legend labels and the color button (bars / names are drawn
// with it)
static void colorApply(uint32_t v)
{
  lv_color_t color = lv_color_hex(vars[v].color);

  for (uint32_t p = 0; p < PLOT_COUNT_MAX; ++p)
  {
    lv_chart_set_series_color(plots[p].chart, plots[p].series[v], color);
    lv_obj_set_style_text_color(plots[p].legendLabels[v], color, 0);
  }

  if (v == editedVar)
  {
    colorButtonUpdate(vars[v].color);
  }

  plotsInvalidate();
}

// The text box change saves the unit (unitEvent)
static void unitPresetEvent(lv_event_t * e)
{
  char unit[PLOT_UNIT_LEN];

  LV_UNUSED(e);

  if (lv_dropdown_get_selected(unitPresetDropdown) == UNIT_PRESET_NONE)
  {
    unit[0] = '\0';
  }
  else
  {
    lv_dropdown_get_selected_str(unitPresetDropdown, unit, sizeof(unit));
  }

  lv_textarea_set_text(unitInput, unit);
}

static void gainEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  const char * text = lv_textarea_get_text(gainInput);
  float        gain;
  bool         valid = gainParse(text, &gain);

  inputValidSet(gainInput, valid);
  snprintf(vars[editedVar].gainText, PLOT_NUMBER_LEN, "%s", text);

  if (valid)
  {
    vars[editedVar].gain = gain;
    varSave(editedVar);
    plotsRender();
  }
}

static void offsetEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  const char * text = lv_textarea_get_text(offsetInput);
  float        offset;
  bool         valid = numberParse(text, &offset);

  inputValidSet(offsetInput, valid);
  snprintf(vars[editedVar].offsetText, PLOT_NUMBER_LEN, "%s", text);

  if (valid)
  {
    vars[editedVar].offset = offset;
    varSave(editedVar);
    plotsRender();
  }
}

static void widthEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  vars[editedVar].width = (uint8_t) (lv_dropdown_get_selected(widthDropdown) + LINE_WIDTH_MIN);
  varSave(editedVar);
  plotsInvalidate();
}

static void showInEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (editorLoading)
  {
    return;
  }

  // Only the shown plots have a button: the choice for the others is kept
  vars[editedVar].plotMask &= (uint8_t) ~((1u << plotCount) - 1);

  for (uint32_t p = 0; p < plotCount; ++p)
  {
    if (lv_buttonmatrix_has_button_ctrl(showInBtnm, p, LV_BUTTONMATRIX_CTRL_CHECKED))
    {
      vars[editedVar].plotMask |= (uint8_t) (1u << p);
    }
  }

  varSave(editedVar);
  visibilityUpdate();
  barsLayoutUpdate();
  plotsRender();
}
