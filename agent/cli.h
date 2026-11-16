// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Command line tool: the session (the app's engine in the background, holding the serial
          ports) and the commands that talk to it through a named pipe.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef CLI_H
#define CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define CLI_EXIT_OK           0
#define CLI_EXIT_FAILED       1           // the session answered with an error
#define CLI_EXIT_USAGE        2
#define CLI_EXIT_NO_SESSION   5

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

// Text that grows (JSON answers)
typedef struct
{
  char * text;
  size_t length;
  size_t size;
} CliOut_t;

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// LVGL tick of the tools: milliseconds, high resolution counter
uint32_t cliTickGet(void);

// Display flush of the tools: nothing is shown (pictures draw themselves)
void cliFlushDiscard(lv_display_t * display, const lv_area_t * area, uint8_t * pixels);

// header ("Temp [C]") names name without its unit, upper / lower case aside ("temp")
bool cliNameIs(const char * header, const char * name);

void cliOutf(CliOut_t * out, const char * format, ...);
void cliOutString(CliOut_t * out, const char * text);   // as a JSON string, quoted
void cliOutNumber(CliOut_t * out, double value);         // NAN: null

// A session command (status, send, ...); false when name is not one
bool cliIsSessionCommand(const char * name);

// serve: runs the session in this process. start: runs it in the background, returns when ready.
// Other session commands: sent to the session. argv[0] is the command.
int cliServe(int argc, char ** argv);
int cliStart(int argc, char ** argv);
int cliClient(int argc, char ** argv);

// Detected serial ports, without a session
int cliPorts(void);

#endif //CLI_H
