// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Serial line parser of one connection: received bytes to lines, lines to numbers. No UI
          (plotter.c owns one per connection).

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef LINE_PARSER_H
#define LINE_PARSER_H

#include <stdbool.h>
#include <stdint.h>

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define LINE_PARSER_LINE_MAX  1024        // room for 32 values per line
#define LINE_PARSER_ECHO_MS   2000        // a command's echo is expected within this time
#define LINE_PARSER_NEW_RUNS  3           // lines with a new text field count before accepted

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  char         line[LINE_PARSER_LINE_MAX]; // line being received
  uint32_t     lineLen;
  bool         lineDiscard;               // too long: ignored up to the next end of line

  // Echo of a sent command: a device echoing it without a line break merges it with the next
  // data line ("SP:17" + "13.07:..." = "SP:1713.07:...", a spike). Removed from the start of the
  // lines received within LINE_PARSER_ECHO_MS.
  char         echoText[LINE_PARSER_LINE_MAX];
  uint32_t     echoTime;
  bool         echoPending;

  // Number of text (non numeric) fields of the data lines: a line with another number is not
  // data (ex: a reply merged into a data line), unless the new number repeats (new line format)
  int32_t      textFieldsRef;
  int32_t      textFieldsNew;
  uint32_t     textFieldsNewRuns;

  const char * separators;                // kept by the caller (static text)
  bool         skipText;
  uint32_t     varCount;                  // values per line, 0 = as many as found
  uint32_t     countSeen;                 // most values found in a line (since the last clear)
} LineParser_t;

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// Separators " ,;:\t", text fields skipped, as many values as found
void lineParserInit(LineParser_t * parser);

// separators: characters splitting a line (kept, not copied). skipText: empty / text fields are
// skipped (otherwise they leave a gap to keep the columns). varCount: values per line, 0 = as many
// as found.
void lineParserSetFormat(LineParser_t * parser, const char * separators, bool skipText, uint32_t varCount);

// Connection opened / closed: the partial line is dropped and the text fields learned again
void lineParserReset(LineParser_t * parser);

// Plot cleared: the number of values and the text fields are learned again
void lineParserClear(LineParser_t * parser);

// text was sent at now (ms tick): removed from the start of the next line if it comes within
// LINE_PARSER_ECHO_MS
void lineParserSent(LineParser_t * parser, const char * text, uint32_t now);

// Adds a received byte at now (ms tick). At the end of a line (\r or \n), returns the line without
// the echo (valid until the next call); otherwise NULL. Empty and too long lines are not returned.
char * lineParserByte(LineParser_t * parser, char c, uint32_t now);

// The numbers of a line (text is modified): returns how many values (values[i] read when valid[i]),
// or 0 when the line is not data. maxCount: at most this many (room in values and valid).
// Lines with fewer values than expected are not data (ex: the device's answer to a command): with
// a fixed varCount, fewer than that; with 0, fewer than the most seen since the last clear.
uint32_t lineParserValues(LineParser_t * parser, char * text, uint32_t maxCount, float * values, bool * valid);

#endif //LINE_PARSER_H
