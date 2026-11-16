// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Recorded tests (samples of the variables since a test started), their CSV files, the
          curves drawn from them, and the conditions that end a recording. No UI here.

   Condition: an expression evaluated on each new sample; the recording ends when it is true.
     Numbers, + - * /, < <= > >= == !=, && || !, ( )
     Variables: Temp, or "Temp [°C]" (quotes: the full column name, needed with spaces)
     Handles: ref (constant variable), watch (watched variable), band (tolerance, in units),
              N (number of samples), t (seconds since the start), n (samples since the start),
              start (watched variable when the last test started)
     Functions: abs(x), min(a, b), max(a, b)
       Over the last k samples: held(cond, k): true on all of them, avg(x, k), lowest(x, k),
       highest(x, k); prev(x, k): k samples ago; slope(x, k): change per second
     Without enough samples yet, these give no value, and a comparison with no value is false.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef TEST_DATA_H
#define TEST_DATA_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define TEST_NAME_LEN         48
#define TEST_HEADER_LEN       48          // "Name [unit]" of a column
#define TEST_SETTINGS_LEN     161         // device settings of a test, 160 characters
#define TEST_COMMANDS_LEN     121         // commands that started a test, 120 characters
#define TEST_COLUMNS_MAX      32
#define TEST_SAMPLES_MAX      1000000     // a recording ends there
#define TEST_EXPR_NODES_MAX   64
#define TEST_EXPR_WINDOW_MAX  10000       // largest k of the functions over the last k samples
#define TEST_COMMAND_SEP      '|'         // between the commands of a test
#define TEST_PLATEAU_BLOCKS_MIN 3         // averages compared to tell a variable stopped changing
#define TEST_PLATEAU_BLOCKS_MAX 256
#define TEST_PLATEAU_PEAKS    3           // highest and lowest peaks compared
#define TEST_BLOCK_SAMPLES    64          // samples per block summary (curves of long tests)
#define TEST_BLOCK_NONE       UINT32_MAX  // a block summary without any value

// ================================================================================================
// ================================================================================================
//            ENUM DECLARATION
// ================================================================================================
// ================================================================================================

typedef enum
{
  TEST_EXPR_OK,
  TEST_EXPR_ERROR_SYNTAX,
  TEST_EXPR_ERROR_NAME,                   // not a variable, handle or function (of those arguments)
  TEST_EXPR_ERROR_NESTED,                 // function over samples inside another one (too slow)
  TEST_EXPR_ERROR_LENGTH,                 // too many terms
} TestExprError_t;

// How a recorded test ended
typedef enum
{
  TEST_END_NONE,                          // being recorded, or not recorded here
  TEST_END_CONDITION,
  TEST_END_STOPPED,                       // Stop button
  TEST_END_FULL,                          // TEST_SAMPLES_MAX or out of memory
  TEST_END_NOT_STABLE,                    // max test time passed
  TEST_END_PLATEAU,                       // stopped changing (Auto stable)
} TestEnd_t;

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

// Min / max of a column over a block of TEST_BLOCK_SAMPLES samples, with the first sample that has
// each (TEST_BLOCK_NONE: no value in the block). testDataCurve then reads whole blocks instead of
// their samples.
typedef struct
{
  float    min;
  float    max;
  uint32_t minAt;
  uint32_t maxAt;
} TestBlock_t;

typedef struct
{
  char       name[TEST_NAME_LEN];
  uint32_t   columnCount;
  uint32_t   sampleCount;
  uint32_t   capacity;
  char       headers[TEST_COLUMNS_MAX][TEST_HEADER_LEN];
  uint32_t * times;                       // [sampleCount]: ms since the start, not decreasing
  float *    values;                      // [sampleCount][columnCount]: NAN = no value
  TestBlock_t * blocks;                   // [blocks of the samples][columnCount] (testDataAppend)
  int64_t    startMs;                     // Unix time of the start; 0: unknown (from a file)
  char       settings[TEST_SETTINGS_LEN]; // what the device replied, for the legend
  char       commands[TEST_COMMANDS_LEN]; // what was sent: the legend shows those settings
} TestData_t;

// What a condition's names refer to
typedef struct
{
  const char (*headers)[TEST_HEADER_LEN];
  uint32_t    columnCount;
  int32_t     refColumn;                  // -1: none (the handle is then an error)
  int32_t     watchColumn;
  float       band;
  uint32_t    samples;                    // N
  float       start;
} TestExprEnv_t;

// Tells a variable stopped changing, also when it is noisy: averages of consecutive blocks of time,
// the last ones kept. Starts zeroed.
typedef struct
{
  bool     active;
  uint32_t blockStartMs;
  double   sum;                           // block being filled
  uint32_t count;
  float    means[TEST_PLATEAU_BLOCKS_MAX];  // oldest first
  uint32_t starts[TEST_PLATEAU_BLOCKS_MAX]; // their start times
  uint32_t meanCount;
  float    value;                         // average of the blocks when it stopped changing
  uint32_t startMs;                       // start of the first of them
} TestPlateau_t;

typedef struct
{
  int32_t  op;
  int32_t  a;                             // arguments (node indexes)
  int32_t  b;
  double   value;                         // number
  int32_t  column;                        // variable
} TestExprNode_t;

typedef struct
{
  TestExprNode_t nodes[TEST_EXPR_NODES_MAX];
  uint32_t       nodeCount;
  int32_t        root;
} TestExpr_t;

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// Empty test with columnCount columns (headers filled by the caller); NULL if out of memory
TestData_t * testDataCreate(uint32_t columnCount);
void         testDataFree(TestData_t * test);

// Adds a sample (row: columnCount values); false when full (TEST_SAMPLES_MAX) or out of memory
bool testDataAppend(TestData_t * test, uint32_t timeMs, const float * row);

// Column named header, -1 when the test has none
int32_t testDataColumnFind(const TestData_t * test, const char * header);

// Reads a file written by the CSV export: column 3 is the time in seconds, the variables follow.
// t = 0 is its first row. Last rows "#settings,<text>" and "#commands,<text>" give the device
// settings and the commands sent. NULL when it is not such a file or out of memory.
TestData_t * testDataLoadCsv(FILE * file);

// Last value of a column in the test; false when it has none
bool testDataLastValue(const TestData_t * test, uint32_t column, float * value);

// Curve of a column between startS and endS (seconds), for columns pixel columns: output gets
// 2 x columns values in time order, the min and max of each column's samples (keeps the peaks).
// A column without sample gets the line between its neighbors; NAN outside the test or next to a
// sample without value.
void testDataCurve(const TestData_t * test, uint32_t column, double startS, double endS, uint32_t columns, float * output);

// index-th command of text ("Kp:2 | Ki:0.5 | SP:5"), without the spaces around it, empty ones not
// counted; false when there is no such command
bool testCommandGet(const char * text, uint32_t index, char * command, uint32_t size);

// The fields of settings ("SP:17.00, kP:10.00, kI:0.00") named like one of commands ("KP:10|SP:17"),
// upper / lower case aside: "SP:17.00, kP:10.00". All of them when none is, so settings always show.
void testSettingsSent(const char * settings, const char * commands, char * output, uint32_t size);

// Tolerance in percent to units, with percentPerUnit percent for 1 unit; negative when it has no
// meaning (the conversion is not above 0)
float testBandUnits(float tolerancePercent, float percentPerUnit);

// New sample of a variable; true when, in the averages of the blocks of smoothMs in the last
// windowMs, the 3 highest and the 3 lowest peaks average at most band apart. A slow change or an
// oscillation slower than about 2 x smoothMs keeps them apart; noise and cycles shorter than
// smoothMs are averaged out, and a lone spike counts for a third. The higher end of the window is
// also a high peak and the lower end a low one, or a steady drift would have no peaks. At least 3
// blocks (smoothMs 0: exactly 3). Samples without a value are ignored.
bool testPlateauUpdate(TestPlateau_t * plateau, float value, uint32_t timeMs, float band, uint32_t windowMs, uint32_t smoothMs);

// After each sample of a recording (testDataAppend): TEST_END_CONDITION when condition (NULL: none)
// is true, else TEST_END_PLATEAU when column watch stopped changing (testPlateauUpdate; off when
// watch < 0, windowMs 0 or band < 0), else TEST_END_NONE.
TestEnd_t testEndCheck(const TestExpr_t * condition, const TestData_t * test, int32_t watch, TestPlateau_t * plateau,
                       float band, uint32_t windowMs, uint32_t smoothMs);

// Max test time passed (maxMs 0: no limit)
bool testTimeUp(uint32_t elapsedMs, uint32_t maxMs);

// When the test settled, ms from its start: Settled stop (settledSamples N > 0) ended by its
// condition: the first of the N last samples; plateau: the start of its blocks; else the last sample.
uint32_t testSettledMs(const TestData_t * test, TestEnd_t end, uint32_t settledSamples, const TestPlateau_t * plateau);

// A chain goes on to its next test after this end (not after Stop or a full memory)
bool testEndNextInChain(TestEnd_t end);

// Compiles a condition; errorAt receives the place of the error in text
TestExprError_t testExprCompile(TestExpr_t * expr, const char * text, const TestExprEnv_t * env, uint32_t * errorAt);

// Value of the condition at the test's latest sample
bool testExprIsTrue(const TestExpr_t * expr, const TestData_t * test);

#endif //TEST_DATA_H
