// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Compare tab: records tests started by a command until a condition is met, and overlays
          the selected variables of several tests on one chart, the constant variable drawn once.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef COMPARE_H
#define COMPARE_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

// Sends a command of a chain on serial connection conn; false when it could not be sent
typedef bool (*CompareSendCb_t)(uint32_t conn, const char * text, uint32_t color);

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// tab receives the settings, the tests and the chart; call after plotterInit()
void compareInit(lv_obj_t * tab);

void compareSetSendCb(CompareSendCb_t cb);

// Number of serial connections in use: with several, each command of a chain is given its own
void compareSetConnCount(uint32_t count);

// Starts a test now (t = 0), named after command. A test being recorded ends first. conn: serial
// connection the device's settings are requested on.
void compareTestStart(const char * command, uint32_t conn);

bool compareRecording(void);

// Line received on connection conn: true when it is the device's settings (not data)
bool compareLineCapture(uint32_t conn, const char * line);

// New sample during a recording: row has the displayed values of all the variables, NAN = none
void compareSampleAdd(const float * row, uint32_t tick);

// Whole tests shown again (X and Y) in all the graphs / in the graph under point
void compareZoomReset(void);
void compareRightClick(lv_point_t point);

// Text boxes the PC keyboard types in on the tab
lv_group_t * compareKeypadGroup(void);

#ifdef SERIAL_DASHBOARD_CLI               // only in the command line tool (agent/), not in the app
#include "test_data.h"

#define COMPARE_CLI_TESTS_MAX 16
#define COMPARE_CLI_GRAPHS_MAX 4
#define COMPARE_CLI_VARS_MAX  4           // variables of a graph

// As a test opened from a file; it is kept. named: test->name is shown in the legend even with the
// device's settings. false when there are COMPARE_CLI_TESTS_MAX tests.
bool compareCliTestAdd(TestData_t * test, bool named);
void compareCliGraphCountSet(uint32_t count);
void compareCliVarAdd(uint32_t g, const char * name);
void compareCliConstantSet(const char * name);

// Picture of the graphs, at the size set with benchCliPictureSet()
bool compareCliPictureSave(const char * path);

typedef struct
{
  const TestData_t * data;
  const char *       end;                 // "recording", "condition", "stopped", "full",
                                          // "not_stable", "plateau" or "file"
  double             settledS;            // condition / plateau: when it was reached
  float              plateauValue;
} CompareCliTest_t;

uint32_t compareCliTestCount(void);
void     compareCliTestGet(uint32_t i, CompareCliTest_t * test);
int32_t  compareCliRecordingIndex(void);  // -1: none
// Step of the chain being run (-1: none) and its number of steps; waiting: for Start when
int32_t  compareCliChainStep(uint32_t * count, bool * waiting);
// A test, chain, Prepare command or settings request is under way
bool     compareCliBusy(void);
// Replaces the chain's steps (each "cmd|cmd"), all sent on connection conn; false when running
bool     compareCliChainSet(const char * const * steps, uint32_t count, uint32_t conn);
uint32_t compareCliChainGet(uint32_t i, const char ** text, uint32_t * conn);
// Starts the chain as its Run button; false when it did not start (a message says why)
bool     compareCliChainRun(void);
void     compareCliChainStop(void);
void     compareCliTestStop(void);
// All the tests deleted; false when busy
bool     compareCliTestsClear(void);
void     compareCliGraphsClear(void);
// The constant / watched variable of the stop conditions, as chosen in their lists (NULL: kept,
// "": none)
void     compareCliStopVarsSet(const char * constant, const char * watch);
void     compareCliStopVarsGet(const char ** constant, const char ** watch);
#endif

#endif //COMPARE_H
