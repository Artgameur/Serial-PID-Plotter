// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Panel of saved commands (up to CMD_PANEL_MAX): each one is sent over the serial port
          with one click. Commands are added, edited and deleted in the panel and saved.
          A command can have values: "<command><separator><value>" is sent, the value being
          chosen in a dropdown when there are several.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef CMD_PANEL_H
#define CMD_PANEL_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "app_window.h"

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define CMD_PANEL_MAX         32
#define CMD_NAME_LEN          41          // 40 characters
#define CMD_SEP_LEN           5           // 4 characters
#define CMD_VALUES_LEN        121         // 120 characters: "value1,value2,..."
#define CMD_CONN_MAX          4           // serial connections a command can be sent on

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

// Sends a command on serial connection conn (0 = first; the line ending is added by the callee).
// color: the command's own color (0xRRGGBB), ex: for its marks on the plots.
// startsTest: the command is flagged to start a test (see compare.h).
typedef void (*CmdPanelSendCb_t)(uint32_t conn, const char * text, uint32_t color, bool startsTest);

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// Creates the panel in parent (sized / positioned by the caller); closeCb: its close button
lv_obj_t * cmdPanelCreate(lv_obj_t * parent, lv_event_cb_t closeCb);

// Title bar of the panel (ex: to move it as a window)
lv_obj_t * cmdPanelTitleBar(void);

void cmdPanelSetSendCb(CmdPanelSendCb_t cb);

// Number of serial connections in use: with several, each command shows / is given its own
void cmdPanelSetConnCount(uint32_t count);

// Text box of the panel, for the PC keyboard
lv_group_t * cmdPanelGroup(void);

// Cursor over hit (object under the pointer): open hand on a command's drag handle, closed hand
// while a command is dragged (anywhere), otherwise APP_CURSOR_ARROW
AppCursor_t cmdPanelCursor(lv_obj_t * hit);

#endif //CMD_PANEL_H
