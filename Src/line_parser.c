// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Serial line parser of one connection: received bytes to lines, lines to numbers.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "line_parser.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static bool   textFieldsCheck(LineParser_t * parser, int32_t textFields);
static char * echoRemove(LineParser_t * parser, char * text, uint32_t now);
static bool   fieldParse(char * field, float * value);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void lineParserInit(LineParser_t * parser)
{
  memset(parser, 0, sizeof(*parser));
  parser->separators    = " ,;:\t";
  parser->skipText      = true;
  parser->textFieldsRef = -1;
}

void lineParserSetFormat(LineParser_t * parser, const char * separators, bool skipText, uint32_t varCount)
{
  parser->separators    = separators;
  parser->skipText      = skipText;
  parser->varCount      = varCount;
  parser->textFieldsRef = -1;
}

void lineParserReset(LineParser_t * parser)
{
  parser->lineLen       = 0;
  parser->lineDiscard   = false;
  parser->textFieldsRef = -1;
}

void lineParserClear(LineParser_t * parser)
{
  parser->countSeen     = 0;
  parser->textFieldsRef = -1;
}

void lineParserSent(LineParser_t * parser, const char * text, uint32_t now)
{
  snprintf(parser->echoText, sizeof(parser->echoText), "%s", text);
  parser->echoTime    = now;
  parser->echoPending = (text[0] != '\0');
}

char * lineParserByte(LineParser_t * parser, char c, uint32_t now)
{
  if ((c == '\n') || (c == '\r'))
  {
    char * text = NULL;

    if ((parser->lineLen > 0) && !parser->lineDiscard)
    {
      parser->line[parser->lineLen] = '\0';
      text = echoRemove(parser, parser->line, now);
    }

    parser->lineLen = 0;
    parser->lineDiscard = false;
    return text;
  }

  if (parser->lineLen < (LINE_PARSER_LINE_MAX - 1))
  {
    parser->line[parser->lineLen++] = c;
  }
  else
  {
    // Line too long: ignore it up to the next end of line
    parser->lineDiscard = true;
  }

  return NULL;
}

uint32_t lineParserValues(LineParser_t * parser, char * text, uint32_t maxCount, float * values, bool * valid)
{
  uint32_t count = 0;
  uint32_t validCount = 0;
  int32_t  textFields = 0;
  char *   field = text;

  if ((parser->varCount != 0) && (parser->varCount < maxCount))
  {
    maxCount = parser->varCount;
  }

  while ((field != NULL) && (count < maxCount))
  {
    char * next = strpbrk(field, parser->separators);

    if (next != NULL)
    {
      *next++ = '\0';
    }

    valid[count] = fieldParse(field, &values[count]);

    if (valid[count])
    {
      validCount++;
      count++;
    }
    else
    {
      textFields += (field[strspn(field, " \t")] != '\0') ? 1 : 0;
      count += parser->skipText ? 0 : 1;
    }

    field = next;
  }

  // Ignore empty fields at the end of the line (ex: trailing separator)
  while ((count > 0) && !valid[count - 1])
  {
    count--;
  }

  // Lines with fewer values than expected are not data and would draw spikes and gaps
  uint32_t expected = (parser->varCount != 0) ? maxCount : parser->countSeen;

  if ((validCount == 0) || (count < expected) || !textFieldsCheck(parser, textFields))
  {
    return 0;
  }

  parser->countSeen = (count > parser->countSeen) ? count : parser->countSeen;
  return count;
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// A line with as many text fields as the data lines is data. One with another number is not
// (ex: "SP:1713.07:..." has the extra text field "SP"), unless that number repeats for
// LINE_PARSER_NEW_RUNS lines: the device's new line format.
static bool textFieldsCheck(LineParser_t * parser, int32_t textFields)
{
  if ((parser->textFieldsRef < 0) || (textFields == parser->textFieldsRef))
  {
    parser->textFieldsRef = textFields;
    parser->textFieldsNewRuns = 0;
    return true;
  }

  parser->textFieldsNewRuns = (textFields == parser->textFieldsNew) ? (parser->textFieldsNewRuns + 1) : 1;
  parser->textFieldsNew = textFields;

  if (parser->textFieldsNewRuns < LINE_PARSER_NEW_RUNS)
  {
    return false;
  }

  parser->textFieldsRef = textFields;
  parser->textFieldsNewRuns = 0;
  return true;
}

// Line starting with the text just sent (within LINE_PARSER_ECHO_MS): the rest of the line
static char * echoRemove(LineParser_t * parser, char * text, uint32_t now)
{
  if (!parser->echoPending)
  {
    return text;
  }

  size_t len = strlen(parser->echoText);

  if ((uint32_t) (now - parser->echoTime) > LINE_PARSER_ECHO_MS)
  {
    parser->echoPending = false;
  }
  else if (strncmp(text, parser->echoText, len) == 0)
  {
    parser->echoPending = false;
    return &text[len];
  }

  return text;
}

// Reads one number; surrounding blanks are allowed. When ':' is not a separator,
// "label:value" keeps the value part.
static bool fieldParse(char * field, float * value)
{
  char * colon = strrchr(field, ':');
  char * start = (colon != NULL) ? (colon + 1) : field;
  char * end;

  *value = strtof(start, &end);

  if (end == start)
  {
    return false;
  }

  while ((*end == ' ') || (*end == '\t'))
  {
    end++;
  }

  return (*end == '\0') && isfinite(*value);
}
