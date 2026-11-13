// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Plotter of the numbers received on the serial port: up to 4 plots, per variable
          name / gain / offset / line thickness, cursors and side bars.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef PLOTTER_H
#define PLOTTER_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "app_window.h"
#include "test_data.h"

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define PLOTTER_MAX_VARS      32
#define PLOTTER_MAX_CONNS     4           // serial connections; they share the variables

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

// Gridline values: first, first + step, ... (count lines)
typedef struct
{
  float    first;
  float    step;
  uint32_t count;
  int      decimals;
} Ticks_t;

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// plotTab receives the plots and the panel of the plot / variable settings
void plotterInit(lv_obj_t * plotTab);

// Number of serial connections (1 to PLOTTER_MAX_CONNS): each one gets PLOTTER_MAX_VARS / count
// variables (connection c: from c x that number). Clears the plotted data.
void     plotterSetConnCount(uint32_t count);
uint32_t plotterConnVarCount(void);

// Raw bytes received on connection conn at tick (lv_tick_get() when they arrived); complete lines
// are parsed for numbers, each sample taking the arrival time of its line's end
void plotterFeed(uint32_t conn, const uint8_t * data, uint32_t len, uint32_t tick);

// Connection opened / closed: partial line dropped, its last values no longer held
void plotterResetLine(uint32_t conn);

// Text just sent on connection conn: when the device echoes it at the start of a line (ex:
// without a line break, merged with the next data line), it is not read as data. It is also
// marked on the plots in markColor (0xRRGGBB, the command's color).
void plotterSent(uint32_t conn, const char * text, uint32_t markColor);
void plotterClear(void);

// Line format of connection conn. separators: characters splitting a line. skipText: empty / text
// fields are skipped (otherwise they leave a gap to keep the columns). varCount: values per line,
// 0 = as many as found (at most plotterConnVarCount()).
void plotterSetLineFormat(uint32_t conn, const char * separators, bool skipText, uint32_t varCount);

// Right click at point (screen coordinates): resets the zoom of the plot under it
void plotterRightClick(lv_point_t point);

// Mouse cursor at point (screen coordinates): resize cursors on the windows' corner handles and
// on the seams between them (also while dragging them)
AppCursor_t plotterCursorAt(lv_point_t point);

// New samples are drawn at most once per period (follow the screen frame rate)
void plotterSetRenderPeriod(uint32_t ms);

// Text boxes the PC keyboard types in on the plot tab (NULL: none)
lv_group_t * plotterKeypadGroup(void);

// Gridline values between min and max, about one every spacing pixels, on 1 / 2 / 5 steps; i-th one
Ticks_t plotterTicksCompute(float min, float max, int32_t pixels, int32_t spacing);
float   plotterTickValue(const Ticks_t * ticks, uint32_t i);

// Variables received: their number in columns, their column name ("Name [unit]") in headers
// (room for PLOTTER_MAX_VARS); returns how many
uint32_t plotterVarHeaders(uint32_t * columns, char headers[][TEST_HEADER_LEN]);

// A recording starts (as plotterVarHeaders); it then gets the data lines (compareSampleAdd)
uint32_t plotterRecordStart(uint32_t * columns, char headers[][TEST_HEADER_LEN]);

// New test from the data kept by the plots (name not set); NULL when there is none / out of memory
TestData_t * plotterCapture(void);

// Message shown for a few seconds over the window, in any tab
void plotterToast(const char * text);

#ifdef SERIAL_DASHBOARD_CLI               // only in the command line tool (agent/), not in the app
// Variables received (names as in the tests) and their latest displayed value (NAN: none yet)
uint32_t plotterCliLatest(char headers[][TEST_HEADER_LEN], float * values);

// Implemented by the command line tool: bytes received on a connection; a message to the user
void cliReceived(uint32_t conn, const uint8_t * data, uint32_t len);
void cliMessage(const char * text);
#endif

#endif //PLOTTER_H
