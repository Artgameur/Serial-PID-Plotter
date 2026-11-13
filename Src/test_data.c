// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Recorded tests, their CSV files, their curves and the conditions ending a recording
          (see test_data.h for the language of the conditions).

   A condition is compiled to a tree of nodes. The functions over the last k samples evaluate
   their argument again on those samples (they are in the test), so a condition keeps no state.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "test_data.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define TEST_CAPACITY_START   1024        // samples; doubled when full
#define CSV_LINE_MAX          8192
#define CSV_FIRST_VALUE       3           // sample number, date and time, seconds, variables
#define CSV_TIME_COLUMN       2
#define CSV_SETTINGS_ROW      "#settings,"
#define CSV_COMMANDS_ROW      "#commands,"
#define NAME_END              ":= \t"     // after a name in a setting or a command

// ================================================================================================
// ================================================================================================
//            PRIVATE ENUM DECLARATION
// ================================================================================================
// ================================================================================================

typedef enum
{
  OP_NUMBER,
  OP_COLUMN,
  OP_TIME,
  OP_COUNT,
  OP_NEG,
  OP_NOT,
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_LT,
  OP_LE,
  OP_GT,
  OP_GE,
  OP_EQ,
  OP_NE,
  OP_AND,
  OP_OR,
  OP_ABS,
  OP_MIN,
  OP_MAX,
  // Over the last k samples (b: k)
  OP_HELD,
  OP_AVG,
  OP_LOWEST,
  OP_HIGHEST,
  OP_PREV,
  OP_SLOPE,
} ExprOp_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  const char *          name;
  int32_t               op;
  uint32_t              args;
} ExprFunction_t;

typedef struct
{
  TestExpr_t *          expr;
  const TestExprEnv_t * env;
  const char *          text;
  const char *          at;
  TestExprError_t       error;
  uint32_t              windowDepth;      // inside a function over samples
} ExprParser_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static const ExprFunction_t functions[] =
{
  {"abs", OP_ABS, 1},
  {"min", OP_MIN, 2},
  {"max", OP_MAX, 2},
  {"held", OP_HELD, 2},
  {"avg", OP_AVG, 2},
  {"lowest", OP_LOWEST, 2},
  {"highest", OP_HIGHEST, 2},
  {"prev", OP_PREV, 2},
  {"slope", OP_SLOPE, 2},
};

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static uint32_t csvSplit(char * line, char ** fields, uint32_t max);
static bool     nameSame(const char * a, size_t aLen, const char * b, size_t bLen);
static void     peakKeep(float * peaks, uint32_t * count, float value);
static float    peaksAverage(const float * peaks, uint32_t count);
static float    plateauGap(const float * means, uint32_t count);

static int32_t exprError(ExprParser_t * parser, TestExprError_t error, const char * at);
static int32_t exprNode(ExprParser_t * parser, int32_t op, int32_t a, int32_t b);
static void    exprSkipSpaces(ExprParser_t * parser);
static bool    exprAccept(ExprParser_t * parser, const char * token);
static int32_t exprOr(ExprParser_t * parser);
static int32_t exprAnd(ExprParser_t * parser);
static int32_t exprCompare(ExprParser_t * parser);
static int32_t exprSum(ExprParser_t * parser);
static int32_t exprProduct(ExprParser_t * parser);
static int32_t exprUnary(ExprParser_t * parser);
static int32_t exprPrimary(ExprParser_t * parser);
static int32_t exprNumber(ExprParser_t * parser, double value);
static int32_t exprColumn(ExprParser_t * parser, int32_t column, const char * at);
static int32_t exprQuoted(ExprParser_t * parser);
static int32_t exprName(ExprParser_t * parser);
static int32_t exprCall(ExprParser_t * parser, const ExprFunction_t * function);

static double exprEval(const TestExpr_t * expr, const TestData_t * test, int32_t index, uint32_t row);
static double exprWindow(const TestExpr_t * expr, const TestData_t * test, const TestExprNode_t * node, uint32_t row);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

TestData_t * testDataCreate(uint32_t columnCount)
{
  TestData_t * test = calloc(1, sizeof(TestData_t));

  if ((test == NULL) || (columnCount == 0) || (columnCount > TEST_COLUMNS_MAX))
  {
    free(test);
    return NULL;
  }

  test->columnCount = columnCount;
  test->capacity    = TEST_CAPACITY_START;
  test->times       = malloc(test->capacity * sizeof(uint32_t));
  test->values      = malloc((size_t) test->capacity * columnCount * sizeof(float));

  if ((test->times == NULL) || (test->values == NULL))
  {
    testDataFree(test);
    return NULL;
  }

  return test;
}

void testDataFree(TestData_t * test)
{
  if (test != NULL)
  {
    free(test->times);
    free(test->values);
    free(test);
  }
}

bool testDataAppend(TestData_t * test, uint32_t timeMs, const float * row)
{
  if (test->sampleCount >= TEST_SAMPLES_MAX)
  {
    return false;
  }

  if (test->sampleCount == test->capacity)
  {
    uint32_t   capacity = (test->capacity > (TEST_SAMPLES_MAX / 2)) ? TEST_SAMPLES_MAX : (test->capacity * 2);
    uint32_t * times    = realloc(test->times, capacity * sizeof(uint32_t));

    if (times == NULL)
    {
      return false;
    }

    test->times = times;

    float * values = realloc(test->values, (size_t) capacity * test->columnCount * sizeof(float));

    if (values == NULL)
    {
      return false;
    }

    test->values   = values;
    test->capacity = capacity;
  }

  test->times[test->sampleCount] = timeMs;
  memcpy(&test->values[(size_t) test->sampleCount * test->columnCount], row, test->columnCount * sizeof(float));
  test->sampleCount++;
  return true;
}

int32_t testDataColumnFind(const TestData_t * test, const char * header)
{
  for (uint32_t c = 0; c < test->columnCount; ++c)
  {
    if (strcmp(test->headers[c], header) == 0)
    {
      return (int32_t) c;
    }
  }

  return -1;
}

TestData_t * testDataLoadCsv(FILE * file)
{
  char *       line = malloc(CSV_LINE_MAX);
  char *       fields[CSV_FIRST_VALUE + TEST_COLUMNS_MAX];
  TestData_t * test = NULL;
  uint32_t     count;

  if ((line == NULL) || (fgets(line, CSV_LINE_MAX, file) == NULL))
  {
    free(line);
    return NULL;
  }

  // Without the BOM
  count = csvSplit((strncmp(line, "\xEF\xBB\xBF", 3) == 0) ? (line + 3) : line, fields,
                   sizeof(fields) / sizeof(fields[0]));

  if (count > CSV_FIRST_VALUE)
  {
    test = testDataCreate(count - CSV_FIRST_VALUE);
  }

  if (test == NULL)
  {
    free(line);
    return NULL;
  }

  for (uint32_t c = 0; c < test->columnCount; ++c)
  {
    snprintf(test->headers[c], TEST_HEADER_LEN, "%s", fields[CSV_FIRST_VALUE + c]);
  }

  double first = 0.0;
  double last  = 0.0;
  bool   ok    = true;

  while (ok && (fgets(line, CSV_LINE_MAX, file) != NULL))
  {
    float  row[TEST_COLUMNS_MAX];
    char * end;
    bool   settings = strncmp(line, CSV_SETTINGS_ROW, strlen(CSV_SETTINGS_ROW)) == 0;
    bool   sent     = strncmp(line, CSV_COMMANDS_ROW, strlen(CSV_COMMANDS_ROW)) == 0;

    count = csvSplit(line, fields, sizeof(fields) / sizeof(fields[0]));

    if (settings && (count >= 2))
    {
      snprintf(test->settings, TEST_SETTINGS_LEN, "%s", fields[1]);
      continue;
    }

    if (sent && (count >= 2))
    {
      snprintf(test->commands, TEST_COMMANDS_LEN, "%s", fields[1]);
      continue;
    }

    if (count <= CSV_TIME_COLUMN)
    {
      continue;
    }

    double time = strtod(fields[CSV_TIME_COLUMN], &end);

    // Rows out of order are not from the export: the curves need the times in order
    if ((end == fields[CSV_TIME_COLUMN]) || ((test->sampleCount > 0) && (time < last)) || (time < 0.0))
    {
      continue;
    }

    if (test->sampleCount == 0)
    {
      first = time;
    }

    last = time;

    for (uint32_t c = 0; c < test->columnCount; ++c)
    {
      const char * field = ((CSV_FIRST_VALUE + c) < count) ? fields[CSV_FIRST_VALUE + c] : "";

      row[c] = strtof(field, &end);
      row[c] = (end == field) ? NAN : row[c];
    }

    ok = testDataAppend(test, (uint32_t) llround((time - first) * 1000.0), row);
  }

  free(line);

  if (test->sampleCount == 0)
  {
    testDataFree(test);
    return NULL;
  }

  return test;
}

bool testDataLastValue(const TestData_t * test, uint32_t column, float * value)
{
  for (uint32_t s = test->sampleCount; s > 0; --s)
  {
    float candidate = test->values[((size_t) (s - 1) * test->columnCount) + column];

    if (!isnan(candidate))
    {
      *value = candidate;
      return true;
    }
  }

  return false;
}

void testDataCurve(const TestData_t * test, uint32_t column, double startS, double endS, uint32_t columns, float * output)
{
  double   startMs = startS * 1000.0;
  double   stepMs  = ((endS - startS) * 1000.0) / columns;
  uint32_t low     = 0;
  uint32_t high    = test->sampleCount;

  // First sample of the view
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
    bool     last   = (b + 1) == columns;     // includes the sample exactly at the end of the view
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

    // Between two samples, the column's middle on the line joining them
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

bool testCommandGet(const char * text, uint32_t index, char * command, uint32_t size)
{
  uint32_t found = 0;

  while (true)
  {
    const char * end = strchr(text, TEST_COMMAND_SEP);
    size_t       len = (end != NULL) ? (size_t) (end - text) : strlen(text);

    while ((len > 0) && isspace((unsigned char) *text))
    {
      text++;
      len--;
    }

    while ((len > 0) && isspace((unsigned char) text[len - 1]))
    {
      len--;
    }

    if ((len > 0) && (found++ == index))
    {
      snprintf(command, size, "%.*s", (int) len, text);
      return true;
    }

    if (end == NULL)
    {
      return false;
    }

    text = end + 1;
  }
}

void testSettingsSent(const char * settings, const char * commands, char * output, uint32_t size)
{
  size_t       used  = 0;
  const char * field = settings;

  output[0] = '\0';

  while (*field != '\0')
  {
    field += strspn(field, " \t,");

    size_t fieldLen = strcspn(field, ",");
    size_t nameLen  = strcspn(field, NAME_END);
    char   command[TEST_COMMANDS_LEN];

    nameLen = (nameLen < fieldLen) ? nameLen : fieldLen;

    for (uint32_t c = 0; testCommandGet(commands, c, command, sizeof(command)); ++c)
    {
      if (nameSame(field, nameLen, command, strcspn(command, NAME_END)) && (nameLen > 0))
      {
        used += (size_t) snprintf(&output[used], size - used, "%s%.*s", (used > 0) ? ", " : "", (int) fieldLen, field);
        break;
      }
    }

    field += fieldLen;
  }

  if (used == 0)
  {
    snprintf(output, size, "%s", settings);
  }
}

float testBandUnits(float tolerancePercent, float percentPerUnit)
{
  return (percentPerUnit > 0.0f) ? (tolerancePercent / percentPerUnit) : -1.0f;
}

bool testPlateauUpdate(TestPlateau_t * plateau, float value, uint32_t timeMs, float band, uint32_t windowMs, uint32_t smoothMs)
{
  uint32_t longest  = (windowMs >= TEST_PLATEAU_BLOCKS_MIN) ? (windowMs / TEST_PLATEAU_BLOCKS_MIN) : 1;
  uint32_t shortest = (windowMs / TEST_PLATEAU_BLOCKS_MAX) + 1;
  uint32_t blockMs  = ((smoothMs > 0) && (smoothMs < longest)) ? ((smoothMs > shortest) ? smoothMs : shortest) : longest;
  uint32_t blocks   = windowMs / blockMs;

  blocks = (blocks < TEST_PLATEAU_BLOCKS_MIN) ? TEST_PLATEAU_BLOCKS_MIN : blocks;

  if (isnan(value))
  {
    return false;
  }

  if (!plateau->active)
  {
    plateau->active       = true;
    plateau->blockStartMs = timeMs;
  }

  bool stable = false;

  // Block over: its average joins the last ones, this sample is in the next block
  if (((timeMs - plateau->blockStartMs) >= blockMs) && (plateau->count > 0))
  {
    // The oldest ones out (more than one if the settings changed)
    if (plateau->meanCount >= blocks)
    {
      uint32_t drop = (plateau->meanCount - blocks) + 1;

      plateau->meanCount -= drop;
      memmove(&plateau->means[0], &plateau->means[drop], sizeof(float) * plateau->meanCount);
      memmove(&plateau->starts[0], &plateau->starts[drop], sizeof(uint32_t) * plateau->meanCount);
    }

    plateau->means[plateau->meanCount]  = (float) (plateau->sum / plateau->count);
    plateau->starts[plateau->meanCount] = plateau->blockStartMs;
    plateau->meanCount++;
    plateau->blockStartMs = timeMs;
    plateau->sum          = 0.0;
    plateau->count        = 0;

    if (plateau->meanCount == blocks)
    {
      double total = 0.0;

      for (uint32_t b = 0; b < blocks; ++b)
      {
        total += plateau->means[b];
      }

      stable         = plateauGap(plateau->means, blocks) <= band;
      plateau->value = (float) (total / blocks);
      plateau->startMs = plateau->starts[0];
    }
  }

  plateau->sum += value;
  plateau->count++;
  return stable;
}

TestExprError_t testExprCompile(TestExpr_t * expr, const char * text, const TestExprEnv_t * env, uint32_t * errorAt)
{
  ExprParser_t parser = {expr, env, text, text, TEST_EXPR_OK, 0};

  expr->nodeCount = 0;
  expr->root      = exprOr(&parser);
  exprSkipSpaces(&parser);

  if ((parser.error == TEST_EXPR_OK) && (*parser.at != '\0'))
  {
    exprError(&parser, TEST_EXPR_ERROR_SYNTAX, parser.at);
  }

  *errorAt = (uint32_t) (parser.at - text);
  return parser.error;
}

bool testExprIsTrue(const TestExpr_t * expr, const TestData_t * test)
{
  if (test->sampleCount == 0)
  {
    return false;
  }

  double value = exprEval(expr, test, expr->root, test->sampleCount - 1);

  return !isnan(value) && (value != 0.0);
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// Keeps the TEST_PLATEAU_PEAKS largest values
static void peakKeep(float * peaks, uint32_t * count, float value)
{
  if (*count < TEST_PLATEAU_PEAKS)
  {
    peaks[(*count)++] = value;
    return;
  }

  uint32_t smallest = 0;

  for (uint32_t p = 1; p < TEST_PLATEAU_PEAKS; ++p)
  {
    smallest = (peaks[p] < peaks[smallest]) ? p : smallest;
  }

  peaks[smallest] = fmaxf(peaks[smallest], value);
}

static float peaksAverage(const float * peaks, uint32_t count)
{
  float total = 0.0f;

  for (uint32_t p = 0; p < count; ++p)
  {
    total += peaks[p];
  }

  return total / (float) count;
}

// Gap between the average of the 3 highest peaks and of the 3 lowest. Low peaks are kept negated,
// so peakKeep keeps the lowest ones.
static float plateauGap(const float * means, uint32_t count)
{
  float    highs[TEST_PLATEAU_PEAKS];
  float    lows[TEST_PLATEAU_PEAKS];
  uint32_t highCount = 0;
  uint32_t lowCount  = 0;

  peakKeep(highs, &highCount, fmaxf(means[0], means[count - 1]));
  peakKeep(lows, &lowCount, -fminf(means[0], means[count - 1]));

  for (uint32_t b = 1; (b + 1) < count; ++b)
  {
    if ((means[b] >= means[b - 1]) && (means[b] >= means[b + 1]))
    {
      peakKeep(highs, &highCount, means[b]);
    }

    if ((means[b] <= means[b - 1]) && (means[b] <= means[b + 1]))
    {
      peakKeep(lows, &lowCount, -means[b]);
    }
  }

  return peaksAverage(highs, highCount) + peaksAverage(lows, lowCount);
}

static bool nameSame(const char * a, size_t aLen, const char * b, size_t bLen)
{
  if (aLen != bLen)
  {
    return false;
  }

  for (size_t i = 0; i < aLen; ++i)
  {
    if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i]))
    {
      return false;
    }
  }

  return true;
}

// Splits line in place on the commas (the CSV export quotes the variable names, "" = a quote);
// returns the number of fields
static uint32_t csvSplit(char * line, char ** fields, uint32_t max)
{
  uint32_t count = 0;
  char *   read  = line;

  while (count < max)
  {
    char * write  = read;
    bool   quoted = false;

    fields[count++] = read;

    while ((*read != '\0') && (*read != '\r') && (*read != '\n') && (quoted || (*read != ',')))
    {
      if (*read != '"')
      {
        *write++ = *read++;
      }
      else if (quoted && (read[1] == '"'))
      {
        *write++ = '"';
        read += 2;
      }
      else
      {
        quoted = !quoted;
        read++;
      }
    }

    bool more = *read == ',';

    *write = '\0';

    if (!more)
    {
      break;
    }

    read++;
  }

  return count;
}

// The first error is the one reported. Returns the (invalid) node index.
static int32_t exprError(ExprParser_t * parser, TestExprError_t error, const char * at)
{
  if (parser->error == TEST_EXPR_OK)
  {
    parser->error = error;
    parser->at    = at;
  }

  return -1;
}

static int32_t exprNode(ExprParser_t * parser, int32_t op, int32_t a, int32_t b)
{
  TestExpr_t * expr = parser->expr;

  if (parser->error != TEST_EXPR_OK)
  {
    return -1;
  }

  if (expr->nodeCount >= TEST_EXPR_NODES_MAX)
  {
    return exprError(parser, TEST_EXPR_ERROR_LENGTH, parser->at);
  }

  expr->nodes[expr->nodeCount] = (TestExprNode_t) {op, a, b, 0.0, -1};
  return (int32_t) expr->nodeCount++;
}

static void exprSkipSpaces(ExprParser_t * parser)
{
  while (isspace((unsigned char) *parser->at))
  {
    parser->at++;
  }
}

static bool exprAccept(ExprParser_t * parser, const char * token)
{
  size_t length = strlen(token);

  exprSkipSpaces(parser);

  if ((parser->error != TEST_EXPR_OK) || (strncmp(parser->at, token, length) != 0))
  {
    return false;
  }

  parser->at += length;
  return true;
}

static int32_t exprOr(ExprParser_t * parser)
{
  int32_t node = exprAnd(parser);

  while (exprAccept(parser, "||"))
  {
    node = exprNode(parser, OP_OR, node, exprAnd(parser));
  }

  return node;
}

static int32_t exprAnd(ExprParser_t * parser)
{
  int32_t node = exprCompare(parser);

  while (exprAccept(parser, "&&"))
  {
    node = exprNode(parser, OP_AND, node, exprCompare(parser));
  }

  return node;
}

static int32_t exprCompare(ExprParser_t * parser)
{
  // "<=" before "<"
  static const struct
  {
    const char * token;
    int32_t      op;
  } compares[] = {{"<=", OP_LE}, {">=", OP_GE}, {"==", OP_EQ}, {"!=", OP_NE}, {"<", OP_LT}, {">", OP_GT}};

  int32_t node = exprSum(parser);

  for (uint32_t i = 0; i < (sizeof(compares) / sizeof(compares[0])); ++i)
  {
    if (exprAccept(parser, compares[i].token))
    {
      return exprNode(parser, compares[i].op, node, exprSum(parser));
    }
  }

  return node;
}

static int32_t exprSum(ExprParser_t * parser)
{
  int32_t node = exprProduct(parser);

  while (true)
  {
    if (exprAccept(parser, "+"))
    {
      node = exprNode(parser, OP_ADD, node, exprProduct(parser));
    }
    else if (exprAccept(parser, "-"))
    {
      node = exprNode(parser, OP_SUB, node, exprProduct(parser));
    }
    else
    {
      return node;
    }
  }
}

static int32_t exprProduct(ExprParser_t * parser)
{
  int32_t node = exprUnary(parser);

  while (true)
  {
    if (exprAccept(parser, "*"))
    {
      node = exprNode(parser, OP_MUL, node, exprUnary(parser));
    }
    else if (exprAccept(parser, "/"))
    {
      node = exprNode(parser, OP_DIV, node, exprUnary(parser));
    }
    else
    {
      return node;
    }
  }
}

static int32_t exprUnary(ExprParser_t * parser)
{
  if (exprAccept(parser, "-"))
  {
    return exprNode(parser, OP_NEG, exprUnary(parser), -1);
  }

  // Not "!=": that one needs a left side
  exprSkipSpaces(parser);

  if ((parser->at[0] == '!') && (parser->at[1] != '='))
  {
    parser->at++;
    return exprNode(parser, OP_NOT, exprUnary(parser), -1);
  }

  return exprPrimary(parser);
}

static int32_t exprPrimary(ExprParser_t * parser)
{
  exprSkipSpaces(parser);

  if (parser->error != TEST_EXPR_OK)
  {
    return -1;
  }

  if (exprAccept(parser, "("))
  {
    int32_t node = exprOr(parser);

    if (!exprAccept(parser, ")"))
    {
      return exprError(parser, TEST_EXPR_ERROR_SYNTAX, parser->at);
    }

    return node;
  }

  if (*parser->at == '"')
  {
    return exprQuoted(parser);
  }

  if (isdigit((unsigned char) *parser->at) || (*parser->at == '.'))
  {
    char * end;
    double value = strtod(parser->at, &end);

    if (end == parser->at)
    {
      return exprError(parser, TEST_EXPR_ERROR_SYNTAX, parser->at);
    }

    parser->at = end;
    return exprNumber(parser, value);
  }

  return exprName(parser);
}

static int32_t exprNumber(ExprParser_t * parser, double value)
{
  int32_t node = exprNode(parser, OP_NUMBER, -1, -1);

  if (node >= 0)
  {
    parser->expr->nodes[node].value = value;
  }

  return node;
}

static int32_t exprColumn(ExprParser_t * parser, int32_t column, const char * at)
{
  if (column < 0)
  {
    return exprError(parser, TEST_EXPR_ERROR_NAME, at);
  }

  int32_t node = exprNode(parser, OP_COLUMN, -1, -1);

  if (node >= 0)
  {
    parser->expr->nodes[node].column = column;
  }

  return node;
}

// "Temp [°C]": the full name of a column
static int32_t exprQuoted(ExprParser_t * parser)
{
  const char * start = parser->at + 1;
  const char * end   = strchr(start, '"');
  size_t       length;

  if (end == NULL)
  {
    return exprError(parser, TEST_EXPR_ERROR_SYNTAX, parser->at);
  }

  length = (size_t) (end - start);

  for (uint32_t c = 0; c < parser->env->columnCount; ++c)
  {
    const char * header = parser->env->headers[c];

    // With or without the unit
    if ((strncmp(header, start, length) == 0) &&
        ((header[length] == '\0') || (strncmp(&header[length], " [", 2) == 0)))
    {
      parser->at = end + 1;
      return exprColumn(parser, (int32_t) c, start);
    }
  }

  return exprError(parser, TEST_EXPR_ERROR_NAME, parser->at);
}

// A handle, a function or a variable (its name without the unit; the bytes above 127 are UTF-8)
static int32_t exprName(ExprParser_t * parser)
{
  const char *      start = parser->at;
  const char *      end   = start;
  const TestExprEnv_t * env = parser->env;
  size_t            length;

  while (isalnum((unsigned char) *end) || (*end == '_') || (((unsigned char) *end) >= 0x80))
  {
    end++;
  }

  length = (size_t) (end - start);

  if (length == 0)
  {
    return exprError(parser, TEST_EXPR_ERROR_SYNTAX, start);
  }

  parser->at = end;

  if (exprAccept(parser, "("))
  {
    for (uint32_t f = 0; f < (sizeof(functions) / sizeof(functions[0])); ++f)
    {
      if ((strlen(functions[f].name) == length) && (strncmp(functions[f].name, start, length) == 0))
      {
        return exprCall(parser, &functions[f]);
      }
    }

    return exprError(parser, TEST_EXPR_ERROR_NAME, start);
  }

  if ((length == 3) && (strncmp(start, "ref", 3) == 0))
  {
    return exprColumn(parser, env->refColumn, start);
  }

  if ((length == 5) && (strncmp(start, "watch", 5) == 0))
  {
    return exprColumn(parser, env->watchColumn, start);
  }

  if ((length == 4) && (strncmp(start, "band", 4) == 0))
  {
    return exprNumber(parser, env->band);
  }

  if ((length == 5) && (strncmp(start, "start", 5) == 0))
  {
    return exprNumber(parser, env->start);
  }

  if ((length == 1) && (*start == 'N'))
  {
    return exprNumber(parser, env->samples);
  }

  if ((length == 1) && (*start == 't'))
  {
    return exprNode(parser, OP_TIME, -1, -1);
  }

  if ((length == 1) && (*start == 'n'))
  {
    return exprNode(parser, OP_COUNT, -1, -1);
  }

  for (uint32_t c = 0; c < env->columnCount; ++c)
  {
    const char * header = env->headers[c];

    if ((strncmp(header, start, length) == 0) &&
        ((header[length] == '\0') || (strncmp(&header[length], " [", 2) == 0)))
    {
      return exprColumn(parser, (int32_t) c, start);
    }
  }

  return exprError(parser, TEST_EXPR_ERROR_NAME, start);
}

// After "name(": the arguments and ")"
static int32_t exprCall(ExprParser_t * parser, const ExprFunction_t * function)
{
  const char * start  = parser->at;
  bool         window = function->op >= OP_HELD;
  int32_t      a;
  int32_t      b = -1;

  // k x k samples at each new sample would freeze the app
  if (window && (parser->windowDepth > 0))
  {
    return exprError(parser, TEST_EXPR_ERROR_NESTED, start);
  }

  parser->windowDepth += window ? 1 : 0;
  a = exprOr(parser);
  parser->windowDepth -= window ? 1 : 0;

  if (function->args == 2)
  {
    if (!exprAccept(parser, ","))
    {
      return exprError(parser, TEST_EXPR_ERROR_SYNTAX, parser->at);
    }

    b = exprOr(parser);
  }

  if (!exprAccept(parser, ")"))
  {
    return exprError(parser, TEST_EXPR_ERROR_SYNTAX, parser->at);
  }

  return exprNode(parser, function->op, a, b);
}

// Value of a node at sample row; NAN = no value
static double exprEval(const TestExpr_t * expr, const TestData_t * test, int32_t index, uint32_t row)
{
  const TestExprNode_t * node = &expr->nodes[index];
  double                 a;
  double                 b;

  switch (node->op)
  {
    case OP_NUMBER:
      return node->value;

    case OP_COLUMN:
      return test->values[((size_t) row * test->columnCount) + (size_t) node->column];

    case OP_TIME:
      return test->times[row] / 1000.0;

    case OP_COUNT:
      return (double) row + 1.0;

    case OP_HELD:
    case OP_AVG:
    case OP_LOWEST:
    case OP_HIGHEST:
    case OP_PREV:
    case OP_SLOPE:
      return exprWindow(expr, test, node, row);

    default:
      break;
  }

  a = exprEval(expr, test, node->a, row);

  switch (node->op)
  {
    case OP_NEG:
      return -a;

    case OP_NOT:
      return isnan(a) ? NAN : ((a == 0.0) ? 1.0 : 0.0);

    case OP_ABS:
      return fabs(a);

    // No value is false
    case OP_AND:
      return (!isnan(a) && (a != 0.0) && (b = exprEval(expr, test, node->b, row), !isnan(b) && (b != 0.0))) ? 1.0 : 0.0;

    case OP_OR:
      return ((!isnan(a) && (a != 0.0)) || (b = exprEval(expr, test, node->b, row), !isnan(b) && (b != 0.0))) ? 1.0 : 0.0;

    default:
      break;
  }

  b = exprEval(expr, test, node->b, row);

  switch (node->op)
  {
    case OP_ADD:
      return a + b;

    case OP_SUB:
      return a - b;

    case OP_MUL:
      return a * b;

    case OP_DIV:
      return (b != 0.0) ? (a / b) : NAN;

    // A comparison with no value is false: the C comparisons are, except !=
    case OP_LT:
      return (a < b) ? 1.0 : 0.0;

    case OP_LE:
      return (a <= b) ? 1.0 : 0.0;

    case OP_GT:
      return (a > b) ? 1.0 : 0.0;

    case OP_GE:
      return (a >= b) ? 1.0 : 0.0;

    case OP_EQ:
      return (a == b) ? 1.0 : 0.0;

    case OP_NE:
      return (!isnan(a) && !isnan(b) && (a != b)) ? 1.0 : 0.0;

    case OP_MIN:
      return (isnan(a) || isnan(b)) ? NAN : fmin(a, b);

    case OP_MAX:
      return (isnan(a) || isnan(b)) ? NAN : fmax(a, b);

    default:
      return NAN;
  }
}

// Functions over the last k samples, row included; no value before there are k of them
static double exprWindow(const TestExpr_t * expr, const TestData_t * test, const TestExprNode_t * node, uint32_t row)
{
  double count = exprEval(expr, test, node->b, row);

  if (isnan(count) || (count < 1.0) || (count > TEST_EXPR_WINDOW_MAX))
  {
    return NAN;
  }

  uint32_t k = (uint32_t) count;

  // prev / slope: k samples back, so k + 1 samples are needed
  if ((node->op == OP_PREV) || (node->op == OP_SLOPE))
  {
    if (row < k)
    {
      return NAN;
    }

    double before = exprEval(expr, test, node->a, row - k);

    if (node->op == OP_PREV)
    {
      return before;
    }

    uint32_t span = test->times[row] - test->times[row - k];

    return (span > 0) ? (((exprEval(expr, test, node->a, row) - before) * 1000.0) / span) : NAN;
  }

  if ((row + 1) < k)
  {
    return (node->op == OP_HELD) ? 0.0 : NAN;
  }

  double total   = 0.0;
  double lowest  = INFINITY;
  double highest = -INFINITY;

  // Latest first: held stops at the first sample that fails
  for (uint32_t i = 0; i < k; ++i)
  {
    double value = exprEval(expr, test, node->a, row - i);

    if (node->op == OP_HELD)
    {
      if (isnan(value) || (value == 0.0))
      {
        return 0.0;
      }

      continue;
    }

    if (isnan(value))
    {
      return NAN;
    }

    total  += value;
    lowest  = fmin(lowest, value);
    highest = fmax(highest, value);
  }

  switch (node->op)
  {
    case OP_HELD:
      return 1.0;

    case OP_AVG:
      return total / k;

    case OP_LOWEST:
      return lowest;

    default:
      return highest;
  }
}
