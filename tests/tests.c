// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Tests of the engine without the UI: conditions, CSV export / import, plateau detection,
          serial line parser, settings file. Run serial_dashboard_tests.exe (or ctest in
          cmake-build); exit code 0 when all pass.
*/
// ------------------------------------------------------------------------------------------------
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "lvgl.h"
#include "line_parser.h"
#include "plot_export.h"
#include "settings.h"
#include "test_data.h"

static int checks;
static int failures;

#define CHECK(cond)                                                                   \
  do                                                                                  \
  {                                                                                   \
    checks++;                                                                         \
    if (!(cond))                                                                      \
    {                                                                                 \
      failures++;                                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
    }                                                                                 \
  } while (0)

static char tempDir[MAX_PATH];

// ================================================================================================
//            Helpers
// ================================================================================================

// Test with columns "Temp [°C]" and "Power", samples every second (values: NAN = no value)
static TestData_t * testMake(const float * temp, const float * power, uint32_t count)
{
  TestData_t * test = testDataCreate(2);

  snprintf(test->headers[0], TEST_HEADER_LEN, "Temp [\xC2\xB0" "C]");
  snprintf(test->headers[1], TEST_HEADER_LEN, "Power");

  for (uint32_t s = 0; s < count; ++s)
  {
    float row[2] = {temp[s], power[s]};
    testDataAppend(test, s * 1000, row);
  }

  return test;
}

// Compiles text for test; returns the error (errorAt: where)
static TestExprError_t compile(TestExpr_t * expr, const TestData_t * test, const char * text, uint32_t * errorAt)
{
  TestExprEnv_t env = {test->headers, test->columnCount, -1, 0, 0.5f, 3, 10.0f};
  uint32_t      at;

  return testExprCompile(expr, text, &env, (errorAt != NULL) ? errorAt : &at);
}

// Condition true at the latest sample; -1 when it does not compile
static int eval(const TestData_t * test, const char * text)
{
  static TestExpr_t expr;

  if (compile(&expr, test, text, NULL) != TEST_EXPR_OK)
  {
    printf("  (does not compile: %s)\n", text);
    return -1;
  }

  return testExprIsTrue(&expr, test) ? 1 : 0;
}

static bool sameFloat(float a, float b)
{
  return (isnan(a) && isnan(b)) || (fabsf(a - b) <= (1e-6f * fmaxf(1.0f, fabsf(a))));
}

static void pathMake(char * path, size_t size, const char * name)
{
  snprintf(path, size, "%s\\%s", tempDir, name);
}

static void fileWriteText(const char * path, const char * text)
{
  FILE * file = fopen(path, "wb");
  fputs(text, file);
  fclose(file);
}

static TestData_t * csvLoadText(const char * text)
{
  char path[MAX_PATH];

  pathMake(path, sizeof(path), "input.csv");
  fileWriteText(path, text);

  FILE *       file = fopen(path, "rb");
  TestData_t * test = testDataLoadCsv(file);

  fclose(file);
  return test;
}

static bool fileContains(const char * path, const char * text)
{
  static char content[65536];
  FILE *      file = fopen(path, "rb");

  if (file == NULL)
  {
    return false;
  }

  size_t length = fread(content, 1, sizeof(content) - 1, file);
  content[length] = '\0';
  fclose(file);
  return strstr(content, text) != NULL;
}

static bool fileExists(const char * path)
{
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// ================================================================================================
//            Conditions
// ================================================================================================

static void testConditions(void)
{
  const float  temp[]  = {1.0f, 2.0f, 3.0f};
  const float  power[] = {10.0f, 20.0f, 30.0f};
  TestData_t * test    = testMake(temp, power, 3);
  TestExpr_t   expr;
  uint32_t     at;

  // Comparisons, arithmetic, precedence
  CHECK(eval(test, "Temp > 2") == 1);
  CHECK(eval(test, "Temp < 2") == 0);
  CHECK(eval(test, "Temp >= 3 && Temp <= 3") == 1);
  CHECK(eval(test, "Temp == 3") == 1);
  CHECK(eval(test, "Temp != 3") == 0);
  CHECK(eval(test, "1 + 2 * 3 == 7") == 1);
  CHECK(eval(test, "(1 + 2) * 3 == 9") == 1);
  CHECK(eval(test, "-Temp == -3") == 1);
  CHECK(eval(test, "!(Temp > 5)") == 1);
  CHECK(eval(test, "Temp > 5 || Power == 30") == 1);
  CHECK(eval(test, "abs(0 - Temp) == 3") == 1);
  CHECK(eval(test, "min(Temp, Power) == 3 && max(Temp, Power) == 30") == 1);

  // Names: with / without the unit, quoted, handles
  CHECK(eval(test, "\"Temp [\xC2\xB0" "C]\" == 3") == 1);
  CHECK(eval(test, "\"Temp\" == 3") == 1);
  CHECK(eval(test, "watch == 3") == 1);         // watchColumn 0
  CHECK(eval(test, "band == 0.5 && N == 3 && start == 10") == 1);
  CHECK(eval(test, "t == 2 && n == 3") == 1);   // 2 s, 3rd sample

  // Functions over the last k samples
  CHECK(eval(test, "avg(Temp, 3) == 2") == 1);
  CHECK(eval(test, "lowest(Temp, 2) == 2 && highest(Power, 3) == 30") == 1);
  CHECK(eval(test, "prev(Temp, 1) == 2") == 1);
  CHECK(eval(test, "slope(Temp, 2) == 1") == 1);   // 1 per second
  CHECK(eval(test, "held(Temp > 0, 3)") == 1);
  CHECK(eval(test, "held(Temp > 1, 3)") == 0);
  // More samples than there are: no value (false), held: false
  CHECK(eval(test, "avg(Temp, 4) > 0") == 0);
  CHECK(eval(test, "prev(Temp, 3) > 0") == 0);
  CHECK(eval(test, "held(Temp > 0, 4)") == 0);
  CHECK(eval(test, "avg(Temp, 0) > 0") == 0);

  // Division by zero: no value, and a comparison with no value is false (!= too)
  CHECK(eval(test, "Power / 0 > 1") == 0);
  CHECK(eval(test, "Power / 0 < 1") == 0);
  CHECK(eval(test, "Power / 0 != 1") == 0);
  CHECK(eval(test, "!(Power / 0 > 1)") == 1);
  CHECK(eval(test, "Power / 0 > 1 || Temp == 3") == 1);
  CHECK(eval(test, "min(Power / 0, 1) < 5") == 0);

  // Errors and where they are
  CHECK(compile(&expr, test, "", &at) == TEST_EXPR_ERROR_SYNTAX);
  CHECK(compile(&expr, test, "Temp >", &at) == TEST_EXPR_ERROR_SYNTAX);
  CHECK(compile(&expr, test, "(Temp > 1", &at) == TEST_EXPR_ERROR_SYNTAX);
  CHECK(compile(&expr, test, "Temp > 1 2", &at) == TEST_EXPR_ERROR_SYNTAX && (at == 9));
  CHECK(compile(&expr, test, "\"Temp > 1", &at) == TEST_EXPR_ERROR_SYNTAX);
  CHECK(compile(&expr, test, "avg(Temp 3)", &at) == TEST_EXPR_ERROR_SYNTAX);
  CHECK(compile(&expr, test, "Pressure > 1", &at) == TEST_EXPR_ERROR_NAME && (at == 0));
  CHECK(compile(&expr, test, "Temp > 1 && Tem > 1", &at) == TEST_EXPR_ERROR_NAME && (at == 12));
  CHECK(compile(&expr, test, "foo(Temp) > 1", &at) == TEST_EXPR_ERROR_NAME);
  CHECK(compile(&expr, test, "ref > 1", &at) == TEST_EXPR_ERROR_NAME);   // no reference column
  CHECK(compile(&expr, test, "avg(avg(Temp, 2), 2) > 1", &at) == TEST_EXPR_ERROR_NESTED);
  CHECK(compile(&expr, test, "avg(Temp, 2) > 1 && avg(Power, 2) > 1", &at) == TEST_EXPR_OK);

  // Too many terms
  char long_text[1024] = "Temp";
  for (int i = 0; i < 40; ++i)
  {
    strcat(long_text, " + 1");
  }
  CHECK(compile(&expr, test, long_text, &at) == TEST_EXPR_ERROR_LENGTH);

  testDataFree(test);

  // A value missing at the latest sample: comparisons false
  const float  temp2[]  = {1.0f, NAN};
  const float  power2[] = {1.0f, 1.0f};
  TestData_t * gap      = testMake(temp2, power2, 2);

  CHECK(eval(gap, "Temp < 100") == 0);
  CHECK(eval(gap, "Temp != 5") == 0);
  CHECK(eval(gap, "!(Temp > 0)") == 1);         // the comparison is false, so its "not" is true
  CHECK(eval(gap, "avg(Temp, 2) > 0") == 0);
  CHECK(eval(gap, "held(Temp > 0, 2)") == 0);
  CHECK(eval(gap, "Power == 1") == 1);
  testDataFree(gap);

  // No sample: never true
  TestData_t * empty = testDataCreate(1);
  snprintf(empty->headers[0], TEST_HEADER_LEN, "Temp");
  CHECK(compile(&expr, empty, "1 == 1", &at) == TEST_EXPR_OK);
  CHECK(!testExprIsTrue(&expr, empty));
  testDataFree(empty);
}

// ================================================================================================
//            CSV export / import
// ================================================================================================

static void testCsvRoundTrip(void)
{
  const float  temp[]  = {1.5f, NAN, -3.25f, 1234567.0f, 0.000123f};
  const float  power[] = {0.0f, 2.0f, NAN, -0.5f, 1e-7f};
  TestData_t * test    = testMake(temp, power, 5);
  char         path[MAX_PATH];

  // A name with a comma and quotes, settings and commands
  snprintf(test->headers[1], TEST_HEADER_LEN, "Power, \"net\" [W]");
  snprintf(test->settings, TEST_SETTINGS_LEN, "SP:17.00, kP:10.00");
  snprintf(test->commands, TEST_COMMANDS_LEN, "SP:17|KP:10");
  test->startMs = 1700000000000LL;

  PlotExport_t * data = plotExportFromTest(test);

  CHECK(data != NULL);
  pathMake(path, sizeof(path), "roundtrip.csv");
  CHECK(plotExportCliWrite(data, path));
  plotExportFree(data);

  FILE *       file   = fopen(path, "rb");
  TestData_t * loaded = testDataLoadCsv(file);
  fclose(file);

  CHECK(loaded != NULL);

  if (loaded != NULL)
  {
    CHECK(loaded->columnCount == 2);
    CHECK(loaded->sampleCount == 5);
    CHECK(strcmp(loaded->headers[0], test->headers[0]) == 0);
    CHECK(strcmp(loaded->headers[1], test->headers[1]) == 0);
    CHECK(strcmp(loaded->settings, test->settings) == 0);
    CHECK(strcmp(loaded->commands, test->commands) == 0);

    for (uint32_t s = 0; s < 5; ++s)
    {
      CHECK(loaded->times[s] == test->times[s]);

      for (uint32_t c = 0; c < 2; ++c)
      {
        CHECK(sameFloat(loaded->values[(s * 2) + c], test->values[(s * 2) + c]));
      }
    }

    testDataFree(loaded);
  }

  testDataFree(test);
}

static void testCsvMalformed(void)
{
  TestData_t * test;

  // Not export files
  CHECK(csvLoadText("") == NULL);
  CHECK(csvLoadText("Sample,Date time,Time (s)\r\n1,x,0.0\r\n") == NULL);           // no variable
  CHECK(csvLoadText("Sample,Date time,Time (s),Temp\r\n") == NULL);                  // no sample
  CHECK(csvLoadText("Sample,Date time,Time (s),Temp\r\n1,x,abc,1\r\n") == NULL);     // no time

  // BOM, LF only, missing and bad values, rows out of order or without time are skipped
  test = csvLoadText("\xEF\xBB\xBF" "Sample,Date time,Time (s),A,B\n"
                     "1,x,10.0,1,2\n"
                     "2,x,10.5,,abc\n"
                     "3,x,10.2,9,9\n"          // before the previous one: skipped
                     "4,x,,9,9\n"              // no time: skipped
                     "5,x,11.0,3\n"            // B missing
                     "garbage line\n"
                     "\n"
                     "#settings,\"SP:1\"\n");
  CHECK(test != NULL);

  if (test != NULL)
  {
    CHECK(test->columnCount == 2);
    CHECK(strcmp(test->headers[0], "A") == 0);
    CHECK(test->sampleCount == 3);
    CHECK((test->times[0] == 0) && (test->times[1] == 500) && (test->times[2] == 1000));
    CHECK(test->values[0] == 1.0f);
    CHECK(isnan(test->values[2]) && isnan(test->values[3]));
    CHECK(test->values[4] == 3.0f);
    CHECK(isnan(test->values[5]));
    CHECK(strcmp(test->settings, "SP:1") == 0);
    testDataFree(test);
  }

  // More columns than TEST_COLUMNS_MAX: the extra ones are dropped
  char text[4096] = "Sample,Date time,Time (s)";
  for (int c = 0; c < TEST_COLUMNS_MAX + 5; ++c)
  {
    snprintf(&text[strlen(text)], sizeof(text) - strlen(text), ",V%d", c);
  }
  strcat(text, "\r\n1,x,0.0,1\r\n");
  test = csvLoadText(text);
  CHECK((test != NULL) && (test->columnCount <= TEST_COLUMNS_MAX));
  testDataFree(test);
}

static void testCurve(void)
{
  const float  temp[]  = {0.0f, 10.0f, NAN, 30.0f};
  const float  power[] = {0.0f, 0.0f, 0.0f, 0.0f};
  TestData_t * test    = testMake(temp, power, 4);
  float        out[8];

  // 4 columns over 0..3 s: min / max of each one, NAN gaps skipped
  testDataCurve(test, 0, 0.0, 3.0, 4, out);
  CHECK(out[0] == 0.0f);
  CHECK(out[7] == 30.0f);

  // Outside the test: no value
  testDataCurve(test, 0, 10.0, 20.0, 4, out);
  CHECK(isnan(out[0]) && isnan(out[7]));
  testDataFree(test);
}

// testDataCurve as it was before the block summaries (reference for the exact-output tests)
static void curveReference(const TestData_t * test, uint32_t column, double startS, double endS, uint32_t columns, float * output)
{
  double   startMs = startS * 1000.0;
  double   stepMs  = ((endS - startS) * 1000.0) / columns;
  uint32_t low     = 0;
  uint32_t high    = test->sampleCount;

  while (low < high)
  {
    uint32_t middle = low + ((high - low) / 2);

    if ((double) test->times[middle] < startMs)
    {
      low = middle + 1;
    }
    else
    {
      high = middle;
    }
  }

  uint32_t s = low;

  for (uint32_t b = 0; b < columns; ++b)
  {
    double   binEnd = startMs + (stepMs * (b + 1));
    bool     last   = (b + 1) == columns;
    float    minValue = NAN;
    float    maxValue = NAN;
    uint32_t minAt = 0;
    uint32_t maxAt = 0;
    bool     any   = false;

    for (; (s < test->sampleCount) && (last ? ((double) test->times[s] <= binEnd) : ((double) test->times[s] < binEnd)); ++s)
    {
      float value = test->values[((size_t) s * test->columnCount) + column];

      if (isnan(value))
      {
        continue;
      }

      if (!any || (value < minValue))
      {
        minValue = value;
        minAt = s;
      }

      if (!any || (value > maxValue))
      {
        maxValue = value;
        maxAt = s;
      }

      any = true;
    }

    if (any)
    {
      output[2 * b]         = (minAt <= maxAt) ? minValue : maxValue;
      output[(2 * b) + 1]   = (minAt <= maxAt) ? maxValue : minValue;
      continue;
    }

    float value = NAN;

    if ((s > 0) && (s < test->sampleCount))
    {
      float  before     = test->values[((size_t) (s - 1) * test->columnCount) + column];
      float  after      = test->values[((size_t) s * test->columnCount) + column];
      double beforeTime = test->times[s - 1];
      double span       = (double) test->times[s] - beforeTime;
      double position   = (span > 0.0) ? (((binEnd - (stepMs / 2.0)) - beforeTime) / span) : 0.0;

      value = (float) (before + ((double) (after - before) * position));
    }

    output[2 * b]       = value;
    output[(2 * b) + 1] = value;
  }
}

static uint32_t rngState = 12345;

static uint32_t rng(void)
{
  rngState = (rngState * 1103515245u) + 12345u;
  return rngState >> 8;
}

// Value of sample s of a random test: NaN runs, ties (few levels), +0 / -0, spikes
static float randomValue(uint32_t kind)
{
  uint32_t r = rng() % 100;

  if (r < 8)
  {
    return NAN;
  }

  switch (kind)
  {
    case 0:  return (float) (rng() % 5);                        // many ties
    case 1:  return (r < 50) ? 0.0f : -0.0f;                    // only zeros of both signs
    case 2:  return ((float) (rng() % 100000) / 7.0f) - 5000.0f;
    default: return (r < 2) ? 1e6f : (float) (rng() % 3) - 1.0f; // rare spikes
  }
}

static void testCurveExact(void)
{
  static float expected[2 * 2000];
  static float actual[2 * 2000];
  const uint32_t sizes[] = {1, 2, 63, 64, 65, 127, 128, 1000, 4097, 200000};
  uint32_t runs = 0;
  uint32_t mismatches = 0;

  for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
  {
    for (uint32_t kind = 0; kind < 4; ++kind)
    {
      TestData_t * test = testDataCreate(3);
      uint32_t     time = rng() % 1000;

      for (uint32_t s = 0; s < sizes[i]; ++s)
      {
        float row[3] = {randomValue(kind), randomValue((kind + 1) % 4), (s % 50 < 25) ? NAN : randomValue(kind)};

        // Steps of 0 (same time), 1 ms, or gaps
        uint32_t r = rng() % 10;
        time += (r < 2) ? 0 : ((r < 9) ? 1 : (rng() % 5000));
        testDataAppend(test, time, row);
      }

      double total = (test->times[test->sampleCount - 1] + 1) / 1000.0;

      for (uint32_t view = 0; view < 25; ++view)
      {
        // Whole test, zooms, partly or fully outside
        double   start   = (view == 0) ? 0.0 : (((double) (rng() % 1400) / 1000.0) - 0.2) * total;
        double   length  = (view == 0) ? total : (((double) (rng() % 1000) + 1.0) / 1000.0) * total * ((view % 5 == 0) ? 3.0 : 1.0);
        uint32_t columns = (view % 3 == 0) ? 2000 : (1 + (rng() % 2000));

        for (uint32_t c = 0; c < 3; ++c)
        {
          curveReference(test, c, start, start + length, columns, expected);
          testDataCurve(test, c, start, start + length, columns, actual);
          runs++;
          mismatches += (memcmp(expected, actual, (size_t) columns * 2 * sizeof(float)) != 0) ? 1 : 0;
        }
      }

      testDataFree(test);
    }
  }

  // A test cleared to 0 samples and filled again (compare.c's wait data): summaries rebuilt
  TestData_t * test = testDataCreate(1);

  for (uint32_t pass = 0; pass < 2; ++pass)
  {
    test->sampleCount = 0;

    for (uint32_t s = 0; s < 1000; ++s)
    {
      float row[1] = {(pass == 0) ? 100.0f : randomValue(2)};
      testDataAppend(test, s, row);
    }
  }

  curveReference(test, 0, 0.0, 1.0, 50, expected);
  testDataCurve(test, 0, 0.0, 1.0, 50, actual);
  runs++;
  mismatches += (memcmp(expected, actual, 50 * 2 * sizeof(float)) != 0) ? 1 : 0;
  testDataFree(test);

  printf("  curve exact-output runs: %lu, mismatches: %lu\n", (unsigned long) runs, (unsigned long) mismatches);
  CHECK(mismatches == 0);
}

// ================================================================================================
//            Plateau
// ================================================================================================

// Samples every 100 ms for durationMs; true when a plateau was found (at: when)
static bool plateauRun(float (*signal)(uint32_t), uint32_t durationMs, float band, uint32_t windowMs,
                       uint32_t smoothMs, uint32_t * at, float * value)
{
  TestPlateau_t plateau;

  memset(&plateau, 0, sizeof(plateau));

  for (uint32_t t = 0; t <= durationMs; t += 100)
  {
    if (testPlateauUpdate(&plateau, signal(t), t, band, windowMs, smoothMs))
    {
      *at    = t;
      *value = plateau.value;
      return true;
    }
  }

  return false;
}

static float signalConstant(uint32_t t)
{
  (void) t;
  return 5.0f;
}

static float signalRamp(uint32_t t)
{
  return (float) t / 1000.0f;                   // 1 per second
}

static float signalNoisy(uint32_t t)
{
  return 5.0f + (((t / 100) % 2) ? 0.5f : -0.5f);
}

static float signalSettling(uint32_t t)
{
  return (t < 10000) ? ((float) t / 1000.0f) : 10.0f;
}

static float signalGaps(uint32_t t)
{
  return ((t / 100) % 3 == 0) ? NAN : 5.0f;
}

static void testPlateau(void)
{
  uint32_t at = 0;
  float    value = 0.0f;

  CHECK(plateauRun(signalConstant, 10000, 0.1f, 3000, 0, &at, &value));
  CHECK((at >= 3000) && (at <= 4000) && (value == 5.0f));

  CHECK(!plateauRun(signalRamp, 30000, 0.1f, 3000, 0, &at, &value));

  // Noise faster than the smoothing is averaged out (blocks of 500 ms: averages 4.9 / 5.1 instead
  // of 4.5 / 5.5)
  CHECK(plateauRun(signalNoisy, 20000, 0.3f, 3000, 500, &at, &value));
  CHECK(fabsf(value - 5.0f) < 0.1f);

  // Stable only once the ramp ended
  CHECK(plateauRun(signalSettling, 30000, 0.1f, 3000, 0, &at, &value));
  CHECK((at > 10000) && (fabsf(value - 10.0f) < 0.1f));

  // Samples without a value are ignored
  CHECK(plateauRun(signalGaps, 10000, 0.1f, 3000, 0, &at, &value));
  CHECK(value == 5.0f);

  // NAN only: never
  TestPlateau_t plateau;
  memset(&plateau, 0, sizeof(plateau));
  bool any = false;
  for (uint32_t t = 0; t < 10000; t += 100)
  {
    any = any || testPlateauUpdate(&plateau, NAN, t, 1.0f, 1000, 0);
  }
  CHECK(!any && !plateau.active);
}

// ================================================================================================
//            Commands and settings of a test
// ================================================================================================

static void testCommands(void)
{
  char command[TEST_COMMANDS_LEN];
  char sent[TEST_SETTINGS_LEN + 16];

  CHECK(testCommandGet(" Kp:2 | Ki:0.5 || SP:5 ", 0, command, sizeof(command)) && (strcmp(command, "Kp:2") == 0));
  CHECK(testCommandGet(" Kp:2 | Ki:0.5 || SP:5 ", 2, command, sizeof(command)) && (strcmp(command, "SP:5") == 0));
  CHECK(!testCommandGet(" Kp:2 | Ki:0.5 || SP:5 ", 3, command, sizeof(command)));
  CHECK(!testCommandGet("", 0, command, sizeof(command)));

  testSettingsSent("SP:17.00, kP:10.00, kI:0.00", "KP:10|SP:17", sent, TEST_SETTINGS_LEN);
  CHECK(strcmp(sent, "SP:17.00, kP:10.00") == 0);
  testSettingsSent("SP:17.00, kP:10.00", "XX:1", sent, TEST_SETTINGS_LEN);
  CHECK(strcmp(sent, "SP:17.00, kP:10.00") == 0);   // none sent: all of them

  // 160 characters of fields without spaces: the output (", " between them) is longer than the
  // buffer. It must stop at the buffer's end (guard bytes after it untouched).
  char settings[TEST_SETTINGS_LEN];
  settings[0] = '\0';
  while (strlen(settings) + 6 < (TEST_SETTINGS_LEN - 1))
  {
    strcat(settings, "SP:17,");
  }

  memset(sent, 0x5A, sizeof(sent));
  testSettingsSent(settings, "SP:1", sent, TEST_SETTINGS_LEN);
  CHECK(strlen(sent) == (TEST_SETTINGS_LEN - 1));
  bool guardIntact = true;
  for (size_t i = TEST_SETTINGS_LEN; i < sizeof(sent); ++i)
  {
    guardIntact = guardIntact && (sent[i] == 0x5A);
  }
  CHECK(guardIntact);
}

// ================================================================================================
//            Serial line parser
// ================================================================================================

#define LP_MAX 32

static float lpValues[LP_MAX];
static bool  lpValid[LP_MAX];

// Feeds text at now; returns the value count of the last complete line (0: not data), -1 when no
// line was completed. The values are in lpValues / lpValid.
static int feed(LineParser_t * parser, const char * text, uint32_t now, uint32_t maxCount)
{
  int last = -1;

  for (const char * c = text; *c != '\0'; ++c)
  {
    char * line = lineParserByte(parser, *c, now);

    if (line != NULL)
    {
      last = (int) lineParserValues(parser, line, maxCount, lpValues, lpValid);
    }
  }

  return last;
}

static void testLineParser(void)
{
  LineParser_t parser;

  // Values, CR / LF / CRLF ends
  lineParserInit(&parser);
  CHECK(feed(&parser, "1.5,2,-3\r\n", 0, LP_MAX) == 3);
  CHECK((lpValues[0] == 1.5f) && (lpValues[1] == 2.0f) && (lpValues[2] == -3.0f));
  CHECK(feed(&parser, "4;5;6\r", 0, LP_MAX) == 3);
  CHECK(feed(&parser, "7\t8 9\n", 0, LP_MAX) == 3 && (lpValues[2] == 9.0f));

  // Auto count: fewer values than seen is not data; more is; Clear learns again
  CHECK(feed(&parser, "4,5\n", 0, LP_MAX) == 0);
  CHECK(feed(&parser, "1,2,3,4\n", 0, LP_MAX) == 4);
  lineParserClear(&parser);
  CHECK(feed(&parser, "4,5\n", 0, LP_MAX) == 2);

  // At most maxCount values
  lineParserInit(&parser);
  char many[512] = "";
  for (int i = 0; i < 40; ++i)
  {
    snprintf(&many[strlen(many)], sizeof(many) - strlen(many), "%d,", i);
  }
  strcat(many, "\n");
  CHECK(feed(&parser, many, 0, LP_MAX) == LP_MAX);
  lineParserInit(&parser);
  CHECK(feed(&parser, many, 0, 8) == 8);
  CHECK(lpValues[7] == 7.0f);

  // Labels: ':' separator (text fields skipped), or "label:value" without it
  lineParserInit(&parser);
  CHECK(feed(&parser, "Temp:12.5 Hum:40\n", 0, LP_MAX) == 2);
  CHECK((lpValues[0] == 12.5f) && (lpValues[1] == 40.0f));
  lineParserSetFormat(&parser, " ,", true, 0);
  CHECK(feed(&parser, "Temp:12.5 Hum:40\n", 0, LP_MAX) == 2);
  CHECK((lpValues[0] == 12.5f) && (lpValues[1] == 40.0f));

  // Fixed number of values
  lineParserInit(&parser);
  lineParserSetFormat(&parser, " ,", true, 2);
  CHECK(feed(&parser, "1 2 3\n", 0, LP_MAX) == 2);
  CHECK(feed(&parser, "1\n", 0, LP_MAX) == 0);

  // Text fields kept as gaps: the values stay in their column; trailing separator ignored
  lineParserInit(&parser);
  lineParserSetFormat(&parser, ",", false, 0);
  CHECK(feed(&parser, "a,1,2\n", 0, LP_MAX) == 3);
  CHECK(!lpValid[0] && lpValid[1] && (lpValues[1] == 1.0f) && (lpValues[2] == 2.0f));
  lineParserInit(&parser);
  lineParserSetFormat(&parser, ",", false, 0);
  CHECK(feed(&parser, "1,2,\n", 0, LP_MAX) == 2);
  CHECK(feed(&parser, "1,,3\n", 0, LP_MAX) == 3 && !lpValid[1]);

  // Another number of text fields: not data, until it repeats (new line format)
  lineParserInit(&parser);
  CHECK(feed(&parser, "V 1 2\n", 0, LP_MAX) == 2);
  CHECK(feed(&parser, "X Y 1 2\n", 0, LP_MAX) == 0);
  CHECK(feed(&parser, "X Y 1 2\n", 0, LP_MAX) == 0);
  CHECK(feed(&parser, "X Y 1 2\n", 0, LP_MAX) == 2);
  CHECK(feed(&parser, "V 1 2\n", 0, LP_MAX) == 0);

  // Echo of a sent command at the start of the next line, within the timeout
  lineParserInit(&parser);
  lineParserSent(&parser, "SP:17", 1000);
  CHECK(feed(&parser, "SP:1713.07,5\n", 1100, LP_MAX) == 2);
  CHECK(fabsf(lpValues[0] - 13.07f) < 1e-4f);
  lineParserInit(&parser);
  lineParserSent(&parser, "SP:17", 1000);
  CHECK(feed(&parser, "SP:1713.07,5\n", 1000 + LINE_PARSER_ECHO_MS + 1, LP_MAX) == 2);
  CHECK(fabsf(lpValues[0] - 1713.07f) < 1e-2f);  // too late: not removed
  lineParserInit(&parser);
  lineParserSent(&parser, "SP:17", 0xFFFFFF00u);
  CHECK(feed(&parser, "SP:1713.07,5\n", 0x10u, LP_MAX) == 2);   // tick wrapped around
  CHECK(fabsf(lpValues[0] - 13.07f) < 1e-4f);

  // Line too long: dropped up to its end, the next one read
  lineParserInit(&parser);
  char longLine[LINE_PARSER_LINE_MAX + 100];
  memset(longLine, '7', sizeof(longLine) - 6);
  strcpy(&longLine[sizeof(longLine) - 6], ",1,2\n");
  CHECK(feed(&parser, longLine, 0, LP_MAX) == -1);
  CHECK(feed(&parser, "3,4\n", 0, LP_MAX) == 2 && (lpValues[0] == 3.0f));

  // Not numbers: NaN / infinity are text, empty lines are not lines
  lineParserInit(&parser);
  CHECK(feed(&parser, "abc\n", 0, LP_MAX) == 0);
  CHECK(feed(&parser, "\r\n\n\r", 0, LP_MAX) == -1);
  CHECK(feed(&parser, " , ;\n", 0, LP_MAX) == 0);
  lineParserInit(&parser);
  CHECK(feed(&parser, "1,2\n", 0, LP_MAX) == 2);
  CHECK(feed(&parser, "nan,1,2\n", 0, LP_MAX) == 0);    // a text field more than the data lines
  CHECK(feed(&parser, "1e999,1\n", 0, LP_MAX) == 0);
  lineParserInit(&parser);
  CHECK(feed(&parser, "nan,inf,5\n", 0, LP_MAX) == 1 && (lpValues[0] == 5.0f));
}

// ================================================================================================
//            Settings file
// ================================================================================================

static void testSettings(void)
{
  char file[MAX_PATH];
  char temp[MAX_PATH];

  pathMake(file, sizeof(file), "SerialDashboard\\settings.ini");
  pathMake(temp, sizeof(temp), "SerialDashboard\\settings.ini.tmp");

  // In the test folder, not the user's (settingsInit reads APPDATA)
  SetEnvironmentVariableA("APPDATA", tempDir);
  _putenv_s("APPDATA", tempDir);
  settingsInit();

  settingsSetString("alpha", "1");
  settingsSetInt("beta", -42);
  settingsFlush();
  // Text mode: lines end with \r\n
  CHECK(fileContains(file, "alpha=1\r\n") && fileContains(file, "beta=-42\r\n"));
  CHECK(!fileExists(temp));
  CHECK(settingsGetInt("beta", 0) == -42);
  CHECK(settingsGetInt("alpha", 7) == 1);
  CHECK(settingsGetInt("missing", 7) == 7);
  settingsSetString("text", "12abc");
  CHECK(settingsGetInt("text", 7) == 7);

  // The file cannot be replaced (a folder in its place): the change stays to be saved, the file
  // left untouched, no temporary file left
  DeleteFileA(file);
  CreateDirectoryA(file, NULL);
  settingsSetString("gamma", "3");
  settingsFlush();
  CHECK(!fileExists(temp));
  RemoveDirectoryA(file);
  CHECK(!fileExists(file));
  settingsFlush();                              // tried again: written now
  CHECK(fileContains(file, "gamma=3\r\n") && fileContains(file, "alpha=1\r\n"));

  // Profiles: saved without the app settings, loaded back
  char profile[MAX_PATH];
  pathMake(profile, sizeof(profile), "SerialDashboard\\profiles\\Test profile.ini");
  settingsSetString("language", "1");
  CHECK(settingsProfileSave("Test profile"));
  CHECK(fileContains(profile, "gamma=3\r\n") && !fileContains(profile, "language="));
  settingsSetString("gamma", "changed");
  CHECK(settingsProfileLoad("Test profile"));
  CHECK(strcmp(settingsGetString("gamma", ""), "3") == 0);
  CHECK(strcmp(settingsGetString("language", ""), "1") == 0);
  CHECK(fileContains(file, "gamma=3\r\n"));    // settings.ini written at once
  CHECK(!settingsProfileSave("..\\escape"));
  CHECK(!settingsProfileNameValid("bad/name") && !settingsProfileNameValid(" lead") && settingsProfileNameValid("ok-1_2.3"));

  // Room for 1024 settings; more are dropped (last: it fills the store)
  char key[32];
  for (int i = 0; i < 1100; ++i)
  {
    snprintf(key, sizeof(key), "k%d", i);
    settingsSetInt(key, i);
  }
  CHECK(settingsGetInt("k1000", -1) == 1000);   // 7 + 1001 < 1024
  CHECK(settingsGetInt("k1099", -1) == -1);
  settingsFlush();
  CHECK(fileContains(file, "k1000=1000\r\n"));
}

// Child process: a change, then exit() as when the window is closed (LVGL's Windows driver)
static int settingsExitChild(const char * dir)
{
  _putenv_s("APPDATA", dir);
  lv_init();
  settingsInit();
  settingsSetString("written", "at exit");
  exit(0);
}

static void testSettingsAtExit(const char * exe)
{
  char dir[MAX_PATH];
  char file[MAX_PATH];
  char command[MAX_PATH * 3];

  pathMake(dir, sizeof(dir), "exit");
  CreateDirectoryA(dir, NULL);
  snprintf(file, sizeof(file), "%s\\SerialDashboard\\settings.ini", dir);
  snprintf(command, sizeof(command), "\"\"%s\" --settings-exit-child \"%s\"\"", exe, dir);
  CHECK(system(command) == 0);
  CHECK(fileContains(file, "written=at exit\r\n"));
}

// ================================================================================================
//            Benchmark (--bench): large recordings, CSV, Compare curves
// ================================================================================================

static double benchMs(LARGE_INTEGER start)
{
  LARGE_INTEGER now;
  LARGE_INTEGER frequency;

  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  return ((double) (now.QuadPart - start.QuadPart) * 1000.0) / (double) frequency.QuadPart;
}

static int bench(void)
{
  const uint32_t samples = 1000000;
  const uint32_t columns = 8;
  LARGE_INTEGER  start;
  char           path[MAX_PATH];
  float          row[TEST_COLUMNS_MAX];
  static float   curve[2 * 2000];

  TestData_t * test = testDataCreate(columns);

  for (uint32_t c = 0; c < columns; ++c)
  {
    snprintf(test->headers[c], TEST_HEADER_LEN, "V%u", c);
  }

  // 1 M samples at 1 kHz (16 min 40 s), one value missing now and then
  QueryPerformanceCounter(&start);
  double appendMax = 0.0;

  for (uint32_t s = 0; s < samples; ++s)
  {
    LARGE_INTEGER one;

    for (uint32_t c = 0; c < columns; ++c)
    {
      row[c] = ((s % 997) == c) ? NAN : (float) (10.0 * sin((s * 0.001 * (c + 1)) + c) + c);
    }

    QueryPerformanceCounter(&one);
    testDataAppend(test, s, row);
    appendMax = fmax(appendMax, benchMs(one));
  }

  printf("append %u x %u: %.1f ms (longest single append %.2f ms)\n", samples, columns, benchMs(start), appendMax);

  // Compare graph curves: whole test, 2000 pixel columns, each column (as graphRender for 8 curves)
  for (int pass = 0; pass < 3; ++pass)
  {
    QueryPerformanceCounter(&start);

    for (uint32_t c = 0; c < columns; ++c)
    {
      testDataCurve(test, c, 0.0, samples / 1000.0, 2000, curve);
    }

    printf("curves 8 x whole test (2000 columns): %.2f ms\n", benchMs(start));
  }

  QueryPerformanceCounter(&start);
  for (uint32_t c = 0; c < columns; ++c)
  {
    testDataCurve(test, c, 500.0, 510.0, 2000, curve);
  }
  printf("curves 8 x 10 s zoom (2000 columns): %.2f ms\n", benchMs(start));

  // CSV export and import
  pathMake(path, sizeof(path), "bench.csv");
  PlotExport_t * data = plotExportFromTest(test);

  QueryPerformanceCounter(&start);
  bool written = plotExportCliWrite(data, path);
  printf("csv export %u rows: %.1f ms (%s)\n", samples, benchMs(start), written ? "ok" : "FAILED");
  plotExportFree(data);

  QueryPerformanceCounter(&start);
  FILE *       file   = fopen(path, "rb");
  TestData_t * loaded = testDataLoadCsv(file);
  fclose(file);
  printf("csv import %u rows: %.1f ms (%s)\n", samples, benchMs(start),
         ((loaded != NULL) && (loaded->sampleCount == samples)) ? "ok" : "FAILED");

  testDataFree(loaded);
  testDataFree(test);
  DeleteFileA(path);
  return 0;
}

// ================================================================================================
//            Main
// ================================================================================================

static void folderDelete(const char * dir)
{
  char command[MAX_PATH + 32];

  snprintf(command, sizeof(command), "rmdir /s /q \"%s\" 2>nul", dir);
  system(command);
}

int main(int argc, char ** argv)
{
  if ((argc == 3) && (strcmp(argv[1], "--settings-exit-child") == 0))
  {
    return settingsExitChild(argv[2]);
  }

  char base[MAX_PATH];

  GetTempPathA(sizeof(base), base);
  snprintf(tempDir, sizeof(tempDir), "%sserial_dashboard_tests_%lu", base, (unsigned long) GetCurrentProcessId());
  CreateDirectoryA(tempDir, NULL);

  lv_init();

  if ((argc == 2) && (strcmp(argv[1], "--bench") == 0))
  {
    int result = bench();
    folderDelete(tempDir);
    return result;
  }

  testConditions();
  testCsvRoundTrip();
  testCsvMalformed();
  testCurve();
  testCurveExact();
  testPlateau();
  testCommands();
  testLineParser();
  testSettings();
  testSettingsAtExit(argv[0]);

  folderDelete(tempDir);
  printf("%d checks, %d failed\n", checks, failures);
  return (failures == 0) ? 0 : 1;
}
