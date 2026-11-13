// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Panel of saved commands (up to CMD_PANEL_MAX): each one is sent over the serial port
          with one click. Commands are added, edited and deleted in the panel and saved
          (keys "cmdCount", "cmd1" ... "cmd32" =
          "command<TAB>separator<TAB>value1,value2,...<TAB>connection number<TAB>TR" (T: text
          field, R: starts a test)). The commands are reordered by dragging their handle.

   Sent text: the command alone when it has no value, otherwise "<command><separator><value>";
   with several values, the value is chosen in a dropdown on the command's row; with a text
   field, the value is typed on the row (the values give its starting text). With several
   serial connections, each command goes to its own connection (chosen in the editor).

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "cmd_panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang.h"
#include "settings.h"
#include "ui_widgets.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define CMD_KEY_LEN           16
#define CMD_NONE              -1
#define CMD_FIELD_SEP         '\t'        // between the fields of a saved command
#define CMD_VALUE_SEP         ','         // between the values typed by the user
#define CMD_SENT_LEN          (CMD_NAME_LEN + CMD_SEP_LEN + CMD_VALUES_LEN)
#define CMD_VALUE_WIDTH       uiPx(80)    // values dropdown
#define CMD_BUTTON_MAX_WIDTH  uiPx(140)   // command text in its button
#define CMD_CONN_WIDTH        uiPx(56)    // connection dropdown of the editor
#define CMD_TEXT_MIN_WIDTH    uiPx(60)    // text field of a row
#define CMD_TEXT_FLAG         'T'         // saved: the command has a text field
#define CMD_TEST_FLAG         'R'         // saved: sending the command starts a test
#define CMD_DRAG_EDGE         uiPx(16)    // dragging this close to the list's edge scrolls it
#define CMD_DRAG_SCROLL       uiPx(6)
#define CMD_COLOR_COUNT       16

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  char     name[CMD_NAME_LEN];
  char     sep[CMD_SEP_LEN];
  char     values[CMD_VALUES_LEN];      // "value1,value2,..." (empty: command alone)
  uint32_t conn;                        // serial connection it is sent on (0 = first)
  bool     textInput;                   // value typed in a text field on its row
  bool     startsTest;                  // sending it starts a test (recording)
  char     typed[CMD_VALUES_LEN];       // text of that field (not saved; starts as values)
  uint32_t selected;                    // value chosen in the dropdown (not saved)
  uint32_t color;                       // 0xRRGGBB: its handle and its marks on the plots
} Command_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

// Colors of the commands: the hues of the plot variables 1-16, each new command taking the first
// one no other command has
static const uint32_t cmdColors[CMD_COLOR_COUNT] =
{
  0x2563EB, 0xDC2626, 0x16A34A, 0xD97706, 0x9333EA, 0x0891B2, 0xDB2777, 0x4B5563,
  0x65A30D, 0x0284C7, 0xE11D48, 0xCA8A04, 0x4F46E5, 0x0D9488, 0xC026D3, 0x92400E,
};

static Command_t        commands[CMD_PANEL_MAX];
static uint32_t         commandCount;
static int32_t          editedCommand = CMD_NONE;    // command loaded in the editor
static CmdPanelSendCb_t sendCb;
static uint32_t         connCount = 1;

// Option lists of the connection dropdown, by number of connections
static const char * connOptions[] = {"C1", "C1\nC2", "C1\nC2\nC3", "C1\nC2\nC3\nC4"};

static lv_obj_t *   titleBar;
static lv_obj_t *   list;
static lv_obj_t *   nameInput;
static lv_obj_t *   sepInput;
static lv_obj_t *   valuesInput;
static lv_obj_t *   connDropdown;
static lv_obj_t *   textModeBtn;      // checked: the command gets a text field
static lv_obj_t *   saveBtnLabel;
static lv_group_t * group;

// Row being dragged by its handle, and its place when pressed
static lv_obj_t *   dragRow;
static uint32_t     dragFrom;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void     commandsLoad(void);
static void     commandsSave(void);
static uint32_t colorUnused(void);
static uint32_t valueCount(const Command_t * cmd);
static bool     valueGet(const Command_t * cmd, uint32_t index, char * buffer, uint32_t size);
static void     listRebuild(void);
static void     listRebuildAsync(void * data);
static void     editorReset(void);
static void     editorModeUpdate(void);

static void sendEvent(lv_event_t * e);
static void valueEvent(lv_event_t * e);
static void typedEvent(lv_event_t * e);
static void textModeEvent(lv_event_t * e);
static void testToggleEvent(lv_event_t * e);
static void dragEvent(lv_event_t * e);
static void editEvent(lv_event_t * e);
static void deleteEvent(lv_event_t * e);
static void saveEvent(lv_event_t * e);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

lv_obj_t * cmdPanelCreate(lv_obj_t * parent, lv_event_cb_t closeCb)
{
  // Theme card, like the plot windows
  lv_obj_t * panel = lv_obj_create(parent);
  lv_obj_set_style_pad_all(panel, uiPx(4), 0);
  lv_obj_set_style_pad_row(panel, uiPx(4), 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollable(panel, false);

  // Title and close button
  titleBar = uiRowCreate(panel);
  lv_obj_t * title = lv_label_create(titleBar);
  lv_obj_set_flex_grow(title, 1);
  lv_obj_set_style_pad_left(title, uiPx(4), 0);
  lv_label_set_text(title, tr("Commands"));
  uiTooltipSet(uiButtonCreate(titleBar, LV_SYMBOL_CLOSE, closeCb), tr("Close"));

  lv_obj_t * row;

  // One row per command (send / value / edit / delete), scrolls when they do not fit
  list = lv_obj_create(panel);
  lv_obj_remove_style_all(list);
  lv_obj_set_width(list, LV_PCT(100));
  lv_obj_set_flex_grow(list, 1);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, uiPx(4), 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  uiScrollbarShow(list);

  // Editor: command, then separator + values (Enter in a box or the save button)
  nameInput = uiTextareaCreate(uiRowCreate(panel), tr("Command"));
  lv_textarea_set_max_length(nameInput, CMD_NAME_LEN - 1);
  lv_obj_add_event_cb(nameInput, saveEvent, LV_EVENT_READY, NULL);

  row = uiRowCreate(panel);
  sepInput = uiTextareaCreate(row, tr("Sep"));
  lv_obj_set_flex_grow(sepInput, 0);
  lv_obj_set_width(sepInput, uiPx(52));
  lv_textarea_set_max_length(sepInput, CMD_SEP_LEN - 1);
  lv_obj_add_event_cb(sepInput, saveEvent, LV_EVENT_READY, NULL);

  valuesInput = uiTextareaCreate(row, tr("Values: a,b,c"));
  lv_textarea_set_max_length(valuesInput, CMD_VALUES_LEN - 1);
  lv_obj_add_event_cb(valuesInput, saveEvent, LV_EVENT_READY, NULL);

  // Checked: the value is typed in a text field on the command's row
  textModeBtn = uiButtonCreate(row, LV_SYMBOL_KEYBOARD, textModeEvent);
  lv_obj_set_checkable(textModeBtn, true);
  uiTooltipSet(textModeBtn, tr("Text field: the value is typed on the command's row"));

  // Connection the command is sent on (shown with several connections)
  connDropdown = lv_dropdown_create(row);
  lv_obj_add_style(connDropdown, uiStyleCompact(), 0);
  uiTooltipSet(connDropdown, tr("Connection the command is sent on"));
  lv_obj_set_width(connDropdown, CMD_CONN_WIDTH);
  lv_dropdown_set_options_static(connDropdown, connOptions[0]);
  lv_obj_set_hidden(connDropdown, true);

  lv_obj_t * saveBtn = uiButtonCreate(row, LV_SYMBOL_PLUS, saveEvent);
  saveBtnLabel = lv_obj_get_child(saveBtn, 0);
  uiTooltipSet(saveBtn, tr("Add the command / save the changes"));

  group = lv_group_create();
  lv_group_add_obj(group, nameInput);
  lv_group_add_obj(group, sepInput);
  lv_group_add_obj(group, valuesInput);

  commandsLoad();
  listRebuild();
  editorReset();
  return panel;
}

lv_obj_t * cmdPanelTitleBar(void)
{
  return titleBar;
}

void cmdPanelSetSendCb(CmdPanelSendCb_t cb)
{
  sendCb = cb;
}

// The commands show their connection, and the editor its dropdown, with several connections
void cmdPanelSetConnCount(uint32_t count)
{
  uint32_t selected = lv_dropdown_get_selected(connDropdown);

  connCount = LV_CLAMP(1, count, sizeof(connOptions) / sizeof(connOptions[0]));
  lv_dropdown_set_options_static(connDropdown, connOptions[connCount - 1]);
  lv_dropdown_set_selected(connDropdown, LV_MIN(selected, connCount - 1));
  lv_obj_set_hidden(connDropdown, connCount == 1);
  listRebuild();
}

lv_group_t * cmdPanelGroup(void)
{
  return group;
}

// Closed hand while a row is dragged, open hand over a handle (first child of a command's row)
AppCursor_t cmdPanelCursor(lv_obj_t * hit)
{
  if (dragRow != NULL)
  {
    return APP_CURSOR_GRABBING;
  }

  if ((hit != NULL) && (lv_obj_get_index(hit) == 0) && (lv_obj_get_parent(hit) != NULL) &&
      (lv_obj_get_parent(lv_obj_get_parent(hit)) == list))
  {
    return APP_CURSOR_GRAB;
  }

  return APP_CURSOR_ARROW;
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// "command<TAB>separator<TAB>values<TAB>connection<TAB>T"; former versions: without the text flag
// (no text field), without the connection (first one), or without TAB (command alone)
static void commandsLoad(void)
{
  char key[CMD_KEY_LEN];

  commandCount = 0;

  for (int32_t i = 0; i < LV_MIN(settingsGetInt("cmdCount", 0), CMD_PANEL_MAX); ++i)
  {
    Command_t *  cmd = &commands[commandCount];
    const char * text;
    const char * sep;
    const char * values;
    const char * conn;
    const char * flags;
    const char * color;

    snprintf(key, sizeof(key), "cmd%ld", (long) (i + 1));
    text   = settingsGetString(key, "");
    sep    = strchr(text, CMD_FIELD_SEP);
    values = (sep != NULL) ? strchr(sep + 1, CMD_FIELD_SEP) : NULL;
    conn   = (values != NULL) ? strchr(values + 1, CMD_FIELD_SEP) : NULL;
    flags  = (conn != NULL) ? strchr(conn + 1, CMD_FIELD_SEP) : NULL;
    color  = (flags != NULL) ? strchr(flags + 1, CMD_FIELD_SEP) : NULL;

    snprintf(cmd->name, CMD_NAME_LEN, "%.*s", (int) ((sep != NULL) ? (sep - text) : (int) strlen(text)), text);
    snprintf(cmd->sep, CMD_SEP_LEN, "%.*s", (int) ((values != NULL) ? (values - sep - 1) : 0), (sep != NULL) ? (sep + 1) : "");
    snprintf(cmd->values, CMD_VALUES_LEN, "%.*s",
             (int) ((conn != NULL) ? (conn - values - 1) : ((values != NULL) ? (int) strlen(values + 1) : 0)),
             (values != NULL) ? (values + 1) : "");
    cmd->conn      = (conn != NULL) ? (uint32_t) LV_CLAMP(1, atoi(conn + 1), CMD_CONN_MAX) - 1 : 0;
    // Flags field ends at the color field
    size_t flagsLen = (flags == NULL) ? 0 : ((color != NULL) ? (size_t) (color - flags - 1) : strlen(flags + 1));

    cmd->textInput  = (flagsLen > 0) && (memchr(flags + 1, CMD_TEXT_FLAG, flagsLen) != NULL);
    cmd->startsTest = (flagsLen > 0) && (memchr(flags + 1, CMD_TEST_FLAG, flagsLen) != NULL);
    cmd->selected  = 0;
    snprintf(cmd->typed, CMD_VALUES_LEN, "%s", cmd->values);

    if (cmd->name[0] != '\0')
    {
      // Saved before commands had colors: given one below
      cmd->color = (color != NULL) ? (uint32_t) strtoul(color + 1, NULL, 16) : UINT32_MAX;
      commandCount++;
    }
  }

  bool missing = false;

  for (uint32_t i = 0; i < commandCount; ++i)
  {
    if (commands[i].color == UINT32_MAX)
    {
      commands[i].color = colorUnused();
      missing = true;
    }
  }

  if (missing)
  {
    commandsSave();
  }
}

// The entries above the count are left in the file but ignored
static void commandsSave(void)
{
  char key[CMD_KEY_LEN];
  char text[CMD_SENT_LEN + 12];

  settingsSetInt("cmdCount", (int32_t) commandCount);

  for (uint32_t i = 0; i < commandCount; ++i)
  {
    snprintf(key, sizeof(key), "cmd%lu", (unsigned long) (i + 1));
    snprintf(text, sizeof(text), "%s%c%s%c%s%c%lu%c%s%s%c%06lX", commands[i].name, CMD_FIELD_SEP, commands[i].sep,
             CMD_FIELD_SEP, commands[i].values, CMD_FIELD_SEP, (unsigned long) (commands[i].conn + 1),
             CMD_FIELD_SEP, commands[i].textInput ? "T" : "", commands[i].startsTest ? "R" : "", CMD_FIELD_SEP,
             (unsigned long) commands[i].color);
    settingsSetString(key, text);
  }
}

// First color no command has (all taken: the colors repeat)
static uint32_t colorUnused(void)
{
  for (uint32_t c = 0; c < CMD_COLOR_COUNT; ++c)
  {
    bool used = false;

    for (uint32_t i = 0; (i < commandCount) && !used; ++i)
    {
      used = (commands[i].color == cmdColors[c]);
    }

    if (!used)
    {
      return cmdColors[c];
    }
  }

  return cmdColors[commandCount % CMD_COLOR_COUNT];
}

static uint32_t valueCount(const Command_t * cmd)
{
  uint32_t count = 0;
  char     value[CMD_VALUES_LEN];

  while (valueGet(cmd, count, value, sizeof(value)))
  {
    count++;
  }

  return count;
}

// index-th value, without the spaces around it; false when there is no such (non empty) value
static bool valueGet(const Command_t * cmd, uint32_t index, char * buffer, uint32_t size)
{
  const char * start = cmd->values;

  for (uint32_t i = 0; (i < index) && (start != NULL); ++i)
  {
    start = strchr(start, CMD_VALUE_SEP);
    start = (start != NULL) ? (start + 1) : NULL;
  }

  if (start == NULL)
  {
    return false;
  }

  const char * end = strchr(start, CMD_VALUE_SEP);
  size_t       len = (end != NULL) ? (size_t) (end - start) : strlen(start);

  while ((len > 0) && (*start == ' '))
  {
    start++;
    len--;
  }

  while ((len > 0) && (start[len - 1] == ' '))
  {
    len--;
  }

  snprintf(buffer, size, "%.*s", (int) len, start);
  return len > 0;
}

// [handle (drag: reorder)] [command (click: send)] [values dropdown when several / text field]
// [starts a test] [edit] [delete] for each command. The button shows what is sent ("SP:20"), or "SP:" when the
// value is chosen in the dropdown / typed in the text field.
static void listRebuild(void)
{
  lv_obj_clean(list);
  dragRow = NULL;

  for (uint32_t i = 0; i < commandCount; ++i)
  {
    Command_t * cmd   = &commands[i];
    void *      index = (void *) (uintptr_t) i;
    uint32_t    count = cmd->textInput ? 0 : valueCount(cmd);
    char        text[CMD_SENT_LEN];
    char        value[CMD_VALUES_LEN];

    lv_obj_t * row = uiRowCreate(list);
    lv_obj_set_style_pad_column(row, uiPx(4), 0);
    lv_obj_set_style_radius(row, uiPx(4), 0);

    // Handle (first child: see cmdPanelDragCursor). Dragging it must not scroll the list.
    lv_obj_t * handle = lv_label_create(row);
    lv_label_set_text(handle, LV_SYMBOL_BARS);
    uiTooltipSet(handle, tr("Drag to reorder"));
    lv_obj_set_style_text_color(handle, lv_color_hex(cmd->color), 0);
    lv_obj_set_style_pad_hor(handle, uiPx(3), 0);
    lv_obj_set_style_pad_ver(handle, uiPx(6), 0);
    lv_obj_set_clickable(handle, true);
    lv_obj_set_scroll_chain(handle, false);
    lv_obj_add_event_cb(handle, dragEvent, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(handle, dragEvent, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(handle, dragEvent, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(handle, dragEvent, LV_EVENT_PRESS_LOST, NULL);

    // Connection: shown with several (a connection no longer used is shown in red)
    if (connCount > 1)
    {
      lv_obj_t * connLabel = lv_label_create(row);
      lv_label_set_text_fmt(connLabel, "C%lu", (unsigned long) (cmd->conn + 1));
      lv_obj_set_style_text_color(connLabel, lv_color_hex((cmd->conn < connCount) ? COLOR_INFO : COLOR_ERROR), 0);
    }

    if (cmd->textInput)
    {
      snprintf(text, sizeof(text), "%s%s", cmd->name, cmd->sep);
    }
    else if (count == 0)
    {
      snprintf(text, sizeof(text), "%s", cmd->name);
    }
    else if (count == 1)
    {
      valueGet(cmd, 0, value, sizeof(value));
      snprintf(text, sizeof(text), "%s%s%s", cmd->name, cmd->sep, value);
    }
    else
    {
      snprintf(text, sizeof(text), "%s%s", cmd->name, cmd->sep);
    }

    // As wide as its text (long commands end with "..."), the values dropdown right after it
    lv_obj_t * sendBtn = uiButtonCreate(row, text, sendEvent);
    lv_obj_set_user_data(sendBtn, index);
    uiTooltipSet(sendBtn, tr("Send this command"));

    lv_obj_t * label = lv_obj_get_child(sendBtn, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(label, CMD_BUTTON_MAX_WIDTH, 0);

    if (count > 1)
    {
      char   options[CMD_VALUES_LEN * 2];
      size_t used = 0;

      for (uint32_t v = 0; (v < count) && valueGet(cmd, v, value, sizeof(value)); ++v)
      {
        used += (size_t) snprintf(&options[used], sizeof(options) - used, "%s%s", (v > 0) ? "\n" : "", value);
      }

      lv_obj_t * dropdown = lv_dropdown_create(row);
      lv_obj_add_style(dropdown, uiStyleCompact(), 0);
      lv_obj_set_width(dropdown, CMD_VALUE_WIDTH);
      lv_dropdown_set_options(dropdown, options);
      lv_dropdown_set_selected(dropdown, LV_MIN(cmd->selected, count - 1));
      lv_obj_set_user_data(dropdown, index);
      lv_obj_add_event_cb(dropdown, valueEvent, LV_EVENT_VALUE_CHANGED, NULL);
    }

    if (cmd->textInput)
    {
      // Fills the row up to edit / delete; Enter sends. The PC keyboard types in it (group).
      lv_obj_t * field = uiTextareaCreate(row, tr("Value"));
      lv_obj_set_style_min_width(field, CMD_TEXT_MIN_WIDTH, 0);
      lv_textarea_set_max_length(field, CMD_VALUES_LEN - 1);
      lv_textarea_set_text(field, cmd->typed);
      lv_obj_set_user_data(field, index);
      lv_obj_add_event_cb(field, typedEvent, LV_EVENT_VALUE_CHANGED, NULL);
      lv_obj_add_event_cb(field, sendEvent, LV_EVENT_READY, NULL);
      lv_group_add_obj(group, field);
    }
    else
    {
      // Edit / delete at the right of the row
      lv_obj_t * spacer = lv_obj_create(row);
      lv_obj_remove_style_all(spacer);
      lv_obj_set_height(spacer, 1);
      lv_obj_set_flex_grow(spacer, 1);
    }

    // Checked: sending the command starts a test, shown in the Compare tab
    lv_obj_t * testBtn = uiButtonCreate(row, LV_SYMBOL_VIDEO, testToggleEvent);
    lv_obj_set_checkable(testBtn, true);
    lv_obj_set_state(testBtn, LV_STATE_CHECKED, cmd->startsTest);
    lv_obj_set_user_data(testBtn, index);
    uiTooltipSet(testBtn, tr("Sending this command starts a test (Compare tab)"));

    lv_obj_t * editBtn   = uiButtonCreate(row, LV_SYMBOL_EDIT, editEvent);
    lv_obj_t * deleteBtn = uiButtonCreate(row, LV_SYMBOL_CLOSE, deleteEvent);
    lv_obj_set_user_data(editBtn, index);
    lv_obj_set_user_data(deleteBtn, index);
    uiTooltipSet(editBtn, tr("Edit"));
    uiTooltipSet(deleteBtn, tr("Delete"));
  }
}

static void listRebuildAsync(void * data)
{
  LV_UNUSED(data);
  listRebuild();
}

// Empty editor for a new command; full list: no new command
static void editorReset(void)
{
  editedCommand = CMD_NONE;
  lv_textarea_set_text(nameInput, "");
  lv_textarea_set_text(sepInput, "");
  lv_textarea_set_text(valuesInput, "");
  lv_obj_set_state(textModeBtn, LV_STATE_CHECKED, false);
  editorModeUpdate();
  lv_label_set_text(saveBtnLabel, LV_SYMBOL_PLUS);
  lv_textarea_set_placeholder_text(nameInput, tr((commandCount < CMD_PANEL_MAX) ? "Command" : "Max 32 commands"));
}

// Text field: the values box gives its starting text
static void editorModeUpdate(void)
{
  lv_textarea_set_placeholder_text(valuesInput, tr(lv_obj_has_state(textModeBtn, LV_STATE_CHECKED) ? "Starting text"
                                                                                                     : "Values: a,b,c"));
}

// "<command>" or "<command><separator><value>" (value: the single one, the chosen one, or the
// typed one)
static void sendEvent(lv_event_t * e)
{
  uint32_t  i = (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(e));
  char      text[CMD_SENT_LEN];
  char      value[CMD_VALUES_LEN];

  if ((i >= commandCount) || (sendCb == NULL))
  {
    return;
  }

  if (commands[i].textInput)
  {
    snprintf(text, sizeof(text), "%s%s%s", commands[i].name, commands[i].sep, commands[i].typed);
  }
  else if (valueGet(&commands[i], commands[i].selected, value, sizeof(value)) ||
           valueGet(&commands[i], 0, value, sizeof(value)))
  {
    snprintf(text, sizeof(text), "%s%s%s", commands[i].name, commands[i].sep, value);
  }
  else
  {
    snprintf(text, sizeof(text), "%s", commands[i].name);
  }

  sendCb(commands[i].conn, text, commands[i].color, commands[i].startsTest);
}

static void valueEvent(lv_event_t * e)
{
  lv_obj_t * dropdown = lv_event_get_target_obj(e);
  uint32_t   i = (uintptr_t) lv_obj_get_user_data(dropdown);

  if (i < commandCount)
  {
    commands[i].selected = lv_dropdown_get_selected(dropdown);
  }
}

// Kept while the list is rebuilt (not saved)
static void typedEvent(lv_event_t * e)
{
  lv_obj_t * field = lv_event_get_target_obj(e);
  uint32_t   i = (uintptr_t) lv_obj_get_user_data(field);

  if (i < commandCount)
  {
    snprintf(commands[i].typed, CMD_VALUES_LEN, "%s", lv_textarea_get_text(field));
  }
}

static void textModeEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  editorModeUpdate();
}

static void testToggleEvent(lv_event_t * e)
{
  lv_obj_t * btn = lv_event_get_target_obj(e);
  uint32_t   i   = (uintptr_t) lv_obj_get_user_data(btn);

  if (i < commandCount)
  {
    commands[i].startsTest = lv_obj_has_state(btn, LV_STATE_CHECKED);
    commandsSave();
  }
}

// Dragging a row by its handle: it moves past its neighbors as the pointer passes their middle;
// released, the commands take the rows' new order
static void dragEvent(lv_event_t * e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *      row  = lv_obj_get_parent(lv_event_get_target_obj(e));
  lv_point_t      point;

  lv_indev_get_point(lv_indev_active(), &point);

  if (code == LV_EVENT_PRESSED)
  {
    dragRow  = row;
    dragFrom = (uint32_t) lv_obj_get_index(row);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_INFO), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);

    // Closed hand right away (the cursor is otherwise chosen when the mouse moves)
    appWindowCursorRefresh();
    return;
  }

  if (dragRow != row)
  {
    return;
  }

  if (code == LV_EVENT_PRESSING)
  {
    int32_t    index = lv_obj_get_index(row);
    lv_obj_t * prev  = (index > 0) ? lv_obj_get_child(list, index - 1) : NULL;
    lv_obj_t * next  = lv_obj_get_child(list, index + 1);
    lv_area_t  area;

    if (prev != NULL)
    {
      lv_obj_get_coords(prev, &area);
      if (point.y < ((area.y1 + area.y2) / 2))
      {
        lv_obj_move_to_index(row, index - 1);
      }
    }

    if ((next != NULL) && (lv_obj_get_index(row) == index))
    {
      lv_obj_get_coords(next, &area);
      if (point.y > ((area.y1 + area.y2) / 2))
      {
        lv_obj_move_to_index(row, index + 1);
      }
    }

    // Near the list's top / bottom: scrolls (to reach rows out of view)
    lv_obj_get_coords(list, &area);
    if (point.y < (area.y1 + CMD_DRAG_EDGE))
    {
      lv_obj_scroll_by_bounded(list, 0, CMD_DRAG_SCROLL, LV_ANIM_OFF);
    }
    else if (point.y > (area.y2 - CMD_DRAG_EDGE))
    {
      lv_obj_scroll_by_bounded(list, 0, -CMD_DRAG_SCROLL, LV_ANIM_OFF);
    }

    // New positions for the next comparison
    lv_obj_update_layout(list);
    return;
  }

  // Released / lost: new order
  uint32_t to = (uint32_t) lv_obj_get_index(row);

  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  dragRow = NULL;
  appWindowCursorRefresh();

  if ((to != dragFrom) && (dragFrom < commandCount) && (to < commandCount))
  {
    Command_t moved = commands[dragFrom];

    if (to < dragFrom)
    {
      memmove(&commands[to + 1], &commands[to], (dragFrom - to) * sizeof(Command_t));
    }
    else
    {
      memmove(&commands[dragFrom], &commands[dragFrom + 1], (to - dragFrom) * sizeof(Command_t));
    }

    commands[to] = moved;
    commandsSave();

    // The edited command's place changed
    editorReset();

    // The rows' buttons hold the former places: rebuilt after this event (the handle is one of them)
    lv_async_call(listRebuildAsync, NULL);
  }
}

// The command goes to the editor; saving replaces it
static void editEvent(lv_event_t * e)
{
  uint32_t i = (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(e));

  if (i < commandCount)
  {
    editedCommand = (int32_t) i;
    lv_textarea_set_text(nameInput, commands[i].name);
    lv_textarea_set_text(sepInput, commands[i].sep);
    lv_textarea_set_text(valuesInput, commands[i].values);
    lv_obj_set_state(textModeBtn, LV_STATE_CHECKED, commands[i].textInput);
    editorModeUpdate();
    lv_dropdown_set_selected(connDropdown, LV_MIN(commands[i].conn, connCount - 1));
    lv_label_set_text(saveBtnLabel, LV_SYMBOL_OK);
  }
}

static void deleteEvent(lv_event_t * e)
{
  uint32_t i = (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(e));

  if (i >= commandCount)
  {
    return;
  }

  memmove(&commands[i], &commands[i + 1], (commandCount - i - 1) * sizeof(Command_t));
  commandCount--;
  commandsSave();
  editorReset();

  // The clicked button is one of the rows: they are rebuilt after this event
  lv_async_call(listRebuildAsync, NULL);
}

// Adds the editor's command, or replaces the edited one (no command name: nothing)
static void saveEvent(lv_event_t * e)
{
  Command_t * cmd = NULL;

  LV_UNUSED(e);

  if (lv_textarea_get_text(nameInput)[0] == '\0')
  {
    editorReset();
    return;
  }

  if (editedCommand != CMD_NONE)
  {
    cmd = &commands[editedCommand];
  }
  else if (commandCount < CMD_PANEL_MAX)
  {
    // Color chosen before it is counted (it is not one of the others)
    uint32_t color = colorUnused();

    cmd = &commands[commandCount++];
    cmd->color      = color;
    cmd->startsTest = false;
  }

  if (cmd != NULL)
  {
    snprintf(cmd->name, CMD_NAME_LEN, "%s", lv_textarea_get_text(nameInput));
    snprintf(cmd->sep, CMD_SEP_LEN, "%s", lv_textarea_get_text(sepInput));
    snprintf(cmd->values, CMD_VALUES_LEN, "%s", lv_textarea_get_text(valuesInput));
    // With a single connection (dropdown hidden), an edited command keeps its connection
    if ((connCount > 1) || (editedCommand == CMD_NONE))
    {
      cmd->conn = lv_dropdown_get_selected(connDropdown);
    }
    cmd->textInput = lv_obj_has_state(textModeBtn, LV_STATE_CHECKED);
    snprintf(cmd->typed, CMD_VALUES_LEN, "%s", cmd->values);
    cmd->selected = 0;
  }

  commandsSave();
  listRebuild();
  editorReset();
}
