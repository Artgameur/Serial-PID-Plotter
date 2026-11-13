// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Command line tool for AI agents: lists test CSV files, and writes the picture of the
          Compare graphs of several of them. No window, no serial port, and settings.ini is neither
          read nor written (settingsInit() is not called): each run starts from the defaults.

          The real Compare tab is created, on a display nothing is shown on, so the pictures are
          those of the app's Report setting. Results are a JSON object on stdout; see AGENT.md.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#include <stdarg.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shellapi.h>

#include "lvgl.h"

#include "cli.h"
#include "compare.h"
#include "test_bench.h"
#include "test_data.h"
#include "ui_widgets.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define EXIT_OK               0
#define EXIT_USAGE            2           // bad arguments, unknown variable
#define EXIT_CSV              3           // a file could not be read as a test
#define EXIT_WRITE            4           // the picture could not be drawn or written

#define CLI_WARNINGS_MAX      32
#define CLI_TEXT_LEN          320
#define CLI_DISPLAY_WIDTH     1280        // nothing is shown: only the picture's size counts
#define CLI_DISPLAY_HEIGHT    720
#define CLI_BUFFER_LINES      40
#define CLI_BYTES_PER_PIXEL   4
#define CLI_UNIT_START        " ["        // "Temp [C]": the variable's name is before it

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  const char * path;
  const char * label;                     // NULL: the name of the file
  TestData_t * data;
} CliTest_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static const char usageText[] =
  "serial_dashboard_cli: Serial Dashboard for AI agents. See AGENT.md.\n"
  "\n"
  "Session (the app's engine in the background, holding the serial port):\n"
  "  start [--connect COM5] [--baud N] [--profile NAME | --settings FILE | --clean]\n"
  "        [--set KEY=VALUE]... [--idle MINUTES]\n"
  "  status | stop | connect PORT [--baud N] | disconnect\n"
  "  send TEXT [--test] [--wait S] [--until TEXT] | read [--wait S] [--until TEXT] [--max N]\n"
  "  messages [--all] | test-start [TEXT] | test-stop | wait [--timeout S]\n"
  "  chain-set STEP... | chain-run | chain-stop | tests | tests-clear\n"
  "  export --out FILE.csv [--test N] | picture --out FILE.png [--graph VARS]... [--constant VAR]\n"
  "  set KEY VALUE | get [KEY]          (--conn N picks the connection, default 1)\n"
  "  ports                              (serial ports of this PC, no session needed)\n"
  "\n"
  "Pictures of CSV files, no session needed:\n"
  "  serial_dashboard_cli --list FILE.csv...\n"
  "      Prints each file's variables, samples, duration and device settings as JSON.\n"
  "\n"
  "  serial_dashboard_cli --out PICTURE.png --graph VARS [options] [--label NAME] FILE.csv...\n"
  "      Overlays the files (16 at most) and writes one PNG.\n"
  "\n"
  "  --graph VARS     Variables of a graph, separated by commas (\"Temp,PWM\"), 4 at most.\n"
  "                   Repeat it for up to 4 graphs, stacked in the picture.\n"
  "  --constant VAR   Variable shared by the tests (ex: the setpoint): drawn once, on graph 1 too.\n"
  "  --format W:H     Shape of the picture. Default 16:8.\n"
  "  --width INCHES   Width on the page. Default 6.5.\n"
  "  --ppi N          Resolution. Default 300. Pixels = width x ppi, 100 to 6000 per side.\n"
  "  --label NAME     Legend name of the next file, instead of its device settings / file name.\n"
  "\n"
  "A variable is its full column name (\"Temp [C]\") or its name alone (\"Temp\").\n"
  "Results and errors are one JSON object on stdout. Exit code: 0 done, 2 bad arguments,\n"
  "3 a CSV could not be read, 4 the picture could not be written.\n";

static CliTest_t tests[COMPARE_CLI_TESTS_MAX];
static uint32_t  testCount;
static char      graphVars[COMPARE_CLI_GRAPHS_MAX][COMPARE_CLI_VARS_MAX][TEST_HEADER_LEN];
static uint32_t  graphVarCount[COMPARE_CLI_GRAPHS_MAX];
static uint32_t  graphCount;
static char      constant[TEST_HEADER_LEN];
static char      warnings[CLI_WARNINGS_MAX][CLI_TEXT_LEN];
static uint32_t  warningCount;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void          jsonString(const char * text);
static void          fail(int code, const char * format, ...);
static void          warningAdd(const char * format, ...);
static char *        utf8FromWide(const wchar_t * text);
static const char *  fileNameOf(const char * path);
static void          testsLoad(void);
static double        durationS(const TestData_t * test);
static void          testsPrint(bool columns);
static void          listPrint(void);
static const char *  variableResolve(const char * name);
static void          graphAdd(char * list);
static void          graphsResolve(void);
static uint32_t      tickGet(void);
static void          flushDiscard(lv_display_t * display, const lv_area_t * area, uint8_t * pixels);
static void          uiStart(void);
static void          picturePrint(const char * path);
static void          pictureMake(const char * path, const char * format, const char * width, const char * ppi);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

int main(void)
{
  int         argc;
  wchar_t **  wide   = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool        list   = false;
  const char * out   = NULL;
  const char * format = "16:8";
  const char * width = "6.5";
  const char * ppi   = "300";
  const char * label = NULL;

  // Arguments and output in UTF-8: the column names have units like °C
  SetConsoleOutputCP(CP_UTF8);

  if (wide == NULL)
  {
    fail(EXIT_USAGE, "Could not read the command line.");
  }

  if (argc <= 1)
  {
    fputs(usageText, stdout);
    return EXIT_USAGE;
  }

  // A command: the session's, or ports; options: pictures of CSV files (below)
  if (wide[1][0] != L'-')
  {
    char ** args = malloc(sizeof(char *) * (size_t) argc);

    for (int a = 0; (args != NULL) && (a < argc); ++a)
    {
      args[a] = utf8FromWide(wide[a]);
    }

    if (args == NULL)
    {
      fail(EXIT_USAGE, "Out of memory.");
    }

    if (strcmp(args[1], "ports") == 0)
    {
      return cliPorts();
    }

    if (strcmp(args[1], "serve") == 0)
    {
      return cliServe(argc - 1, &args[1]);
    }

    if (strcmp(args[1], "start") == 0)
    {
      return cliStart(argc - 1, &args[1]);
    }

    if (cliIsSessionCommand(args[1]))
    {
      return cliClient(argc - 1, &args[1]);
    }

    fail(EXIT_USAGE, "Unknown command %s. Use --help.", args[1]);
  }

  for (int a = 1; a < argc; ++a)
  {
    const char * arg     = utf8FromWide(wide[a]);
    bool         takes   = (strcmp(arg, "--out") == 0) || (strcmp(arg, "--graph") == 0) || (strcmp(arg, "--constant") == 0)
                           || (strcmp(arg, "--format") == 0) || (strcmp(arg, "--width") == 0) || (strcmp(arg, "--ppi") == 0)
                           || (strcmp(arg, "--label") == 0);
    char *       value   = NULL;

    if (takes)
    {
      if ((a + 1) >= argc)
      {
        fail(EXIT_USAGE, "%s needs a value.", arg);
      }

      value = utf8FromWide(wide[++a]);
    }

    if ((strcmp(arg, "--help") == 0) || (strcmp(arg, "-h") == 0))
    {
      fputs(usageText, stdout);
      return EXIT_OK;
    }
    else if (strcmp(arg, "--list") == 0)
    {
      list = true;
    }
    else if (strcmp(arg, "--out") == 0)
    {
      out = value;
    }
    else if (strcmp(arg, "--graph") == 0)
    {
      graphAdd(value);
    }
    else if (strcmp(arg, "--constant") == 0)
    {
      snprintf(constant, sizeof(constant), "%s", value);
    }
    else if (strcmp(arg, "--format") == 0)
    {
      format = value;
    }
    else if (strcmp(arg, "--width") == 0)
    {
      width = value;
    }
    else if (strcmp(arg, "--ppi") == 0)
    {
      ppi = value;
    }
    else if (strcmp(arg, "--label") == 0)
    {
      label = value;
    }
    else if (strncmp(arg, "--", 2) == 0)
    {
      fail(EXIT_USAGE, "Unknown option %s. Use --help.", arg);
    }
    else if (testCount >= COMPARE_CLI_TESTS_MAX)
    {
      fail(EXIT_USAGE, "%d CSV files at most.", COMPARE_CLI_TESTS_MAX);
    }
    else
    {
      tests[testCount++] = (CliTest_t) {arg, label, NULL};
      label = NULL;
    }
  }

  if (testCount == 0)
  {
    fail(EXIT_USAGE, "No CSV file given.");
  }

  if (label != NULL)
  {
    fail(EXIT_USAGE, "--label comes before the file it names.");
  }

  if (!list && (out == NULL))
  {
    fail(EXIT_USAGE, "Give --list, or --out with the picture's file.");
  }

  testsLoad();

  if (list)
  {
    listPrint();
    return EXIT_OK;
  }

  if (graphCount == 0)
  {
    fail(EXIT_USAGE, "Give at least one --graph. See the variables with --list.");
  }

  // Numbers only: the bench's fields would read the start of anything else
  char * end;

  if (!(strtof(width, &end) > 0.0f) || (*end != '\0'))
  {
    fail(EXIT_USAGE, "--width: a number of inches above 0, not \"%s\".", width);
  }

  if ((strtol(ppi, &end, 10) <= 0) || (*end != '\0'))
  {
    fail(EXIT_USAGE, "--ppi: a whole number above 0, not \"%s\".", ppi);
  }

  graphsResolve();
  pictureMake(out, format, width, ppi);
  fflush(stdout);

  // Without stopping LVGL's drawing threads first: nothing is left to save
  ExitProcess(EXIT_OK);
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

static void jsonString(const char * text)
{
  putchar('"');

  for (const unsigned char * c = (const unsigned char *) text; *c != '\0'; ++c)
  {
    if ((*c == '"') || (*c == '\\'))
    {
      printf("\\%c", *c);
    }
    else if (*c < 0x20)
    {
      printf("\\u%04x", *c);
    }
    else
    {
      putchar(*c);
    }
  }

  putchar('"');
}

// Does not return
static void fail(int code, const char * format, ...)
{
  char    text[CLI_TEXT_LEN * 2];
  va_list args;

  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);

  fputs("{\"ok\":false,\"error\":", stdout);
  jsonString(text);
  fputs("}\n", stdout);
  fflush(stdout);
  ExitProcess((UINT) code);
}

static void warningAdd(const char * format, ...)
{
  va_list args;

  if (warningCount >= CLI_WARNINGS_MAX)
  {
    return;
  }

  va_start(args, format);
  vsnprintf(warnings[warningCount++], CLI_TEXT_LEN, format, args);
  va_end(args);
}

static char * utf8FromWide(const wchar_t * text)
{
  int    size   = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
  char * result = malloc((size_t) LV_MAX(size, 1));

  if ((result == NULL) || (size == 0))
  {
    fail(EXIT_USAGE, "Could not read the command line.");
  }

  WideCharToMultiByte(CP_UTF8, 0, text, -1, result, size, NULL, NULL);
  return result;
}

static const char * fileNameOf(const char * path)
{
  const char * name = path;

  for (const char * c = path; *c != '\0'; ++c)
  {
    if ((*c == '\\') || (*c == '/'))
    {
      name = c + 1;
    }
  }

  return name;
}

// Each test takes the file's name, as when opened in the app
static void testsLoad(void)
{
  for (uint32_t t = 0; t < testCount; ++t)
  {
    wchar_t path[MAX_PATH * 2];
    FILE *  file = NULL;

    if (MultiByteToWideChar(CP_UTF8, 0, tests[t].path, -1, path, sizeof(path) / sizeof(path[0])) != 0)
    {
      file = _wfopen(path, L"rb");
    }

    if (file == NULL)
    {
      fail(EXIT_CSV, "Could not open %s.", tests[t].path);
    }

    tests[t].data = testDataLoadCsv(file);
    fclose(file);

    if (tests[t].data == NULL)
    {
      fail(EXIT_CSV, "%s is not a CSV file exported by Serial Dashboard (or there is not enough memory).", tests[t].path);
    }

    if (tests[t].label != NULL)
    {
      snprintf(tests[t].data->name, TEST_NAME_LEN, "%s", tests[t].label);
    }
    else
    {
      char   name[TEST_NAME_LEN + 4];
      char * extension;

      snprintf(name, sizeof(name), "%s", fileNameOf(tests[t].path));
      extension = strrchr(name, '.');

      if (extension != NULL)
      {
        *extension = '\0';
      }

      snprintf(tests[t].data->name, TEST_NAME_LEN, "%s", name);
    }
  }
}

static double durationS(const TestData_t * test)
{
  return (test->sampleCount > 0) ? (test->times[test->sampleCount - 1] / 1000.0) : 0.0;
}

static void testsPrint(bool columns)
{
  fputs("\"tests\":[", stdout);

  for (uint32_t t = 0; t < testCount; ++t)
  {
    const TestData_t * test = tests[t].data;

    fputs((t > 0) ? ",{\"file\":" : "{\"file\":", stdout);
    jsonString(tests[t].path);
    fputs(",\"name\":", stdout);
    jsonString(test->name);
    printf(",\"samples\":%lu,\"duration_s\":%.3f,\"settings\":", (unsigned long) test->sampleCount, durationS(test));
    jsonString(test->settings);
    fputs(",\"commands\":", stdout);
    jsonString(test->commands);

    if (columns)
    {
      fputs(",\"variables\":[", stdout);

      for (uint32_t c = 0; c < test->columnCount; ++c)
      {
        if (c > 0)
        {
          putchar(',');
        }

        jsonString(test->headers[c]);
      }

      putchar(']');
    }

    putchar('}');
  }

  putchar(']');
}

static void listPrint(void)
{
  fputs("{\"ok\":true,", stdout);
  testsPrint(true);
  fputs("}\n", stdout);
}

// The column name that name stands for, in any test: exactly it, otherwise the one with that name
// before its unit, upper / lower case aside. Fails when there is none, or several.
static const char * variableResolve(const char * name)
{
  const char * found = NULL;

  for (uint32_t t = 0; t < testCount; ++t)
  {
    int32_t column = testDataColumnFind(tests[t].data, name);

    if (column >= 0)
    {
      return tests[t].data->headers[column];
    }
  }

  size_t length = strlen(name);

  for (uint32_t t = 0; t < testCount; ++t)
  {
    for (uint32_t c = 0; c < tests[t].data->columnCount; ++c)
    {
      const char * header = tests[t].data->headers[c];
      const char * unit   = strstr(header, CLI_UNIT_START);
      size_t       size   = (unit != NULL) ? (size_t) (unit - header) : strlen(header);

      if ((size != length) || (_strnicmp(header, name, length) != 0))
      {
        continue;
      }

      if ((found != NULL) && (strcmp(found, header) != 0))
      {
        fail(EXIT_USAGE, "\"%s\" could be \"%s\" or \"%s\": give the full column name.", name, found, header);
      }

      found = header;
    }
  }

  if (found == NULL)
  {
    fail(EXIT_USAGE, "No test has a variable \"%s\". See the variables with --list.", name);
  }

  return found;
}

static void graphAdd(char * list)
{
  if (graphCount >= COMPARE_CLI_GRAPHS_MAX)
  {
    fail(EXIT_USAGE, "%d graphs at most.", COMPARE_CLI_GRAPHS_MAX);
  }

  for (char * name = strtok(list, ","); name != NULL; name = strtok(NULL, ","))
  {
    while (*name == ' ')
    {
      ++name;
    }

    if (*name == '\0')
    {
      continue;
    }

    if (graphVarCount[graphCount] >= COMPARE_CLI_VARS_MAX)
    {
      fail(EXIT_USAGE, "%d variables at most in a graph.", COMPARE_CLI_VARS_MAX);
    }

    snprintf(graphVars[graphCount][graphVarCount[graphCount]++], TEST_HEADER_LEN, "%s", name);
  }

  if (graphVarCount[graphCount] == 0)
  {
    fail(EXIT_USAGE, "--graph: no variable given.");
  }

  graphCount++;
}

// Names replaced by the columns' full names; tests that will be missing a curve are told
static void graphsResolve(void)
{
  if (constant[0] != '\0')
  {
    snprintf(constant, sizeof(constant), "%s", variableResolve(constant));

    bool shown = false;

    for (uint32_t s = 0; s < graphVarCount[0]; ++s)
    {
      shown = shown || (strcmp(variableResolve(graphVars[0][s]), constant) == 0);
    }

    if (!shown && (graphVarCount[0] >= COMPARE_CLI_VARS_MAX))
    {
      fail(EXIT_USAGE, "Graph 1 has %d variables: no room for the constant, which is drawn on it.", COMPARE_CLI_VARS_MAX);
    }
  }

  for (uint32_t g = 0; g < graphCount; ++g)
  {
    for (uint32_t s = 0; s < graphVarCount[g]; ++s)
    {
      snprintf(graphVars[g][s], TEST_HEADER_LEN, "%s", variableResolve(graphVars[g][s]));

      for (uint32_t t = 0; t < testCount; ++t)
      {
        if (testDataColumnFind(tests[t].data, graphVars[g][s]) < 0)
        {
          warningAdd("%s has no column \"%s\": that curve is not drawn.", tests[t].data->name, graphVars[g][s]);
        }
      }
    }
  }

  for (uint32_t t = 0; t < testCount; ++t)
  {
    if (tests[t].data->sampleCount == 0)
    {
      warningAdd("%s has no samples.", tests[t].data->name);
    }
  }
}

static uint32_t tickGet(void)
{
  return GetTickCount();
}

static void flushDiscard(lv_display_t * display, const lv_area_t * area, uint8_t * pixels)
{
  LV_UNUSED(area);
  LV_UNUSED(pixels);
  lv_display_flush_ready(display);
}

// A display without a window, with the light theme, holding the Compare tab
static void uiStart(void)
{
  static uint8_t buffer[CLI_DISPLAY_WIDTH * CLI_BUFFER_LINES * CLI_BYTES_PER_PIXEL];

  lv_init();
  lv_tick_set_cb(tickGet);

  lv_display_t * display = lv_display_create(CLI_DISPLAY_WIDTH, CLI_DISPLAY_HEIGHT);

  lv_display_set_flush_cb(display, flushDiscard);
  lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_lock();
  uiStylesInit();
  uiThemeApply(false, lv_palette_main(LV_PALETTE_BLUE));
  compareInit(lv_screen_active());
}

static void picturePrint(const char * path)
{
  BenchPicture_t picture;

  benchPictureGet(&picture);

  fputs("{\"ok\":true,\"output\":", stdout);
  jsonString(path);
  printf(",\"width_px\":%ld,\"height_px\":%ld,\"ppi\":%ld,\"constant\":", (long) picture.width, (long) picture.height, (long) picture.ppi);
  jsonString(constant);
  fputs(",\"graphs\":[", stdout);

  for (uint32_t g = 0; g < graphCount; ++g)
  {
    fputs((g > 0) ? ",[" : "[", stdout);

    for (uint32_t s = 0; s < graphVarCount[g]; ++s)
    {
      if (s > 0)
      {
        putchar(',');
      }

      jsonString(graphVars[g][s]);
    }

    putchar(']');
  }

  fputs("],", stdout);
  testsPrint(false);
  fputs(",\"warnings\":[", stdout);

  for (uint32_t w = 0; w < warningCount; ++w)
  {
    if (w > 0)
    {
      putchar(',');
    }

    jsonString(warnings[w]);
  }

  fputs("]}\n", stdout);
}

static void pictureMake(const char * path, const char * format, const char * width, const char * ppi)
{
  BenchPicture_t picture;

  uiStart();

  if (!benchCliPictureSet(format, width, ppi))
  {
    fail(EXIT_USAGE, "--format: two numbers above 0, like 16:8, not \"%s\".", format);
  }

  // The bench keeps each side within its limits
  benchPictureGet(&picture);

  if (lroundf(strtof(width, NULL) * strtof(ppi, NULL)) != picture.width)
  {
    warningAdd("Width limited to %ld pixels (100 to 6000): lower --width or --ppi.", (long) picture.width);
  }

  for (uint32_t t = 0; t < testCount; ++t)
  {
    compareCliTestAdd(tests[t].data, tests[t].label != NULL);
  }

  compareCliGraphCountSet(graphCount);

  for (uint32_t g = 0; g < graphCount; ++g)
  {
    for (uint32_t s = 0; s < graphVarCount[g]; ++s)
    {
      compareCliVarAdd(g, graphVars[g][s]);
    }
  }

  if (constant[0] != '\0')
  {
    compareCliConstantSet(constant);
  }

  if (!compareCliPictureSave(path))
  {
    fail(EXIT_WRITE, "Could not write %s. Does its folder exist?", path);
  }

  picturePrint(path);
}
