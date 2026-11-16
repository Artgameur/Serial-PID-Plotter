// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Serial PID Plotter UI: port / data format settings, send / receive terminal, plotter
          (see plotter.c) and its variable settings.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "screens.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "app_window.h"
#include "cmd_panel.h"
#include "lang.h"
#include "compare.h"
#include "platform.h"
#include "plotter.h"
#include "profile.h"
#include "serial.h"
#include "settings.h"
#include "ui_widgets.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define TOP_BAR_HEIGHT        26
#define TAB_BAR_HEIGHT        28
#define TAB_SERIAL            0
#define TAB_PLOT              1
#define TAB_COMPARE           2

// App settings (index in the option lists below)
#define UI_SCALE_DEFAULT      1           // 100 %
#define MAX_FPS_DEFAULT       2           // 60 fps
#define THEME_LIGHT           0
#define THEME_DARK            1

#define DEFAULT_BAUD_RATE     "115200"
#define BAUD_MAX_DIGITS       8
#define PORT_SCAN_PERIOD_MS   1500
#define LOG_MAX_LEN           32768       // terminal text kept
#define LOG_TRIM_LEN          24576       // kept (whole lines) when LOG_MAX_LEN is reached
#define LOG_PENDING_LEN       8192        // received text waiting for logFlushTimer
#define LOG_FLUSH_MS          50
#define PROFILE_MAX           64          // profiles listed

// Serial connections (up to CONN_MAX at once). With several, the terminal shows each received
// line with its connection ("[C2] ..."): text waits for its end of line, at most CONN_LINE_MS.
#define CONN_MAX              PLOTTER_MAX_CONNS

// Help panel: who made the app (same as the sources' headers)
#define APP_NAME              "Serial PID Plotter"
#define APP_AUTHOR            "Author: artgameur"
#define APP_COPYRIGHT         "Copyright (c) 2026 artgameur. MIT License."
#define CONN_LINE_MAX         512
#define CONN_LINE_MS          200
#define CONN_KEY_LEN          32
#define COLOR_WAITING         0xF59E0B

// Data format settings (order matches separatorOptions / lineEndingOptions)
#define SEPARATOR_AUTO        0
#define VAR_COUNT_AUTO        0
#define LINE_ENDING_CRLF      3

// Display mode of the received bytes (order matches rxModeOptions)
#define RX_MODE_TEXT          0
#define RX_MODE_HEX           1
#define RX_MODE_DEC           2

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

// Presets copied into the baud rate text box; any other value can be typed
static const char *   baudPresetOptions = "9600\n19200\n38400\n57600\n115200\n230400\n250000\n256000\n"
                                          "460800\n500000\n921600\n1000000\n2000000";

// Option order matches the values below / the SerialParity_t and SerialStopBits_t enums
static const uint8_t  dataBitsValues[] = {8, 7, 6, 5};
static const char *   dataBitsOptions = "8 bits\n7 bits\n6 bits\n5 bits";
static const char *   parityOptions   = "None\nOdd\nEven\nMark\nSpace";
static const char *   stopBitsOptions = "1 stop\n1.5 stop\n2 stop";
static const char *   rxModeOptions   = "Text\nHex\nDec";

// Separator between the variables of a received line ("Auto" accepts any of them)
static const char *   separatorOptions = "Auto\nSpace\nComma\nSemicolon\nColon\nTab";
static const char *   separatorChars[] = {" ,;:\t", " ", ",", ";", ":", "\t"};

// Index = number of variables plotted (0 = as many as the line contains)
static const char *   varCountOptions = "Auto\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n"
                                        "17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32";

// Appended to each line sent from the terminal
static const char *   lineEndingOptions = "None\nLF (\\n)\nCR (\\r)\nCR+LF (\\r\\n)";
static const char *   lineEndings[]     = {"", "\n", "\r", "\r\n"};

// App settings
static const int32_t      uiScales[]        = {75, 100, 125, 150, 175, 200};
static const char *       uiScaleOptions    = "75 %\n100 %\n125 %\n150 %\n175 %\n200 %";
static const uint32_t     maxFpsValues[]    = {15, 30, 60, 120};
static const char *       maxFpsOptions     = "15 fps\n30 fps\n60 fps\n120 fps";
static const char *       themeOptions      = "Light\nDark";
static const lv_palette_t accentPalettes[]  = {LV_PALETTE_BLUE, LV_PALETTE_TEAL, LV_PALETTE_GREEN, LV_PALETTE_ORANGE,
                                               LV_PALETTE_RED, LV_PALETTE_PURPLE, LV_PALETTE_PINK};
static const char *       accentOptions     = "Blue\nTeal\nGreen\nOrange\nRed\nPurple\nPink";

static const char *       connCountOptions  = "1\n2\n3\n4";

// One serial connection. Its settings are kept in the settings file (connKey()), and shown in the
// Serial tab while it is the selected one (connSel).
typedef struct
{
  SerialPort_t   port;
  char           selectedPort[SERIAL_PORT_NAME_LEN];  // chosen in the port list

  // Auto reconnect: port and format to open again as soon as the port is present
  bool           reconnectPending;
  char           reconnectName[SERIAL_PORT_NAME_LEN];
  SerialConfig_t reconnectConfig;

  // Terminal with several connections: received line waiting for its end
  char           rxLine[CONN_LINE_MAX + 1];
  size_t         rxLineLen;
  uint32_t       rxLineTime;

  // Receive queue problems already reported (serialStatsGet), for the open port
  SerialStats_t  reported;
  bool           backlogWarned;
} Conn_t;

static Conn_t           conns[CONN_MAX];
static uint32_t         connCount = 1;
static uint32_t         connSel;          // connection shown in the Serial tab (settings, sending)
static bool             connLoading;      // its settings are being shown: not saved again
static SerialPortInfo_t portList[SERIAL_MAX_PORTS];
static uint32_t         portCount;

static lv_group_t * serialGroup;

// Top bar
static lv_obj_t * statusLed;
static lv_obj_t * statusLabel;

// Tabs
static lv_obj_t * tabview;

// Serial tab: connection selector (shown with several), connection and its settings panel
static lv_obj_t * connSelBtnm;
static char       connSelLabels[CONN_MAX][40];
static const char * connSelMap[CONN_MAX + 1];
static lv_obj_t * connCountDropdown;
static lv_obj_t * connSplitLabel;
static lv_obj_t * settingsPanel;
static lv_obj_t * panelBtnLabel;
static lv_obj_t * portDropdown;
static lv_obj_t * refreshBtn;
static lv_obj_t * baudInput;
static lv_obj_t * baudPresetDropdown;
static lv_obj_t * dataBitsDropdown;
static lv_obj_t * parityDropdown;
static lv_obj_t * stopBitsDropdown;
static lv_obj_t * connectBtn;
static lv_obj_t * connectBtnLabel;
static lv_obj_t * portInfoLabel;
static lv_obj_t * autoReconnectSwitch;
static lv_obj_t * separatorDropdown;
static lv_obj_t * varCountDropdown;
static lv_obj_t * lineEndingDropdown;

// App settings panel
static lv_obj_t * settingsOverlay;
static lv_obj_t * helpOverlay;
static lv_obj_t * languageDropdown;
static lv_obj_t * languageNoteRow;      // shown while the chosen language is not the UI's
static lv_obj_t * uiScaleDropdown;
static lv_obj_t * scaleNoteLabel;
static lv_obj_t * maxFpsDropdown;
static lv_obj_t * themeDropdown;
static lv_obj_t * accentDropdown;

// Settings profiles (one per project)
static char         profileNames[PROFILE_MAX][SETTINGS_PROFILE_NAME_LEN];
static uint32_t     profileCount;
static char         profileOptions[PROFILE_MAX * SETTINGS_PROFILE_NAME_LEN];
static char         profileDeletePending[SETTINGS_PROFILE_NAME_LEN];   // Delete clicked once
static lv_obj_t *   profileDropdown;
static lv_obj_t *   profileLoadBtn;
static lv_obj_t *   profileDeleteBtn;
static lv_obj_t *   profileNameInput;
static lv_obj_t *   profileNoteLabel;
static lv_obj_t *   fileKindDropdown;
static lv_obj_t *   fileExportBtn;
static lv_obj_t *   fileImportBtn;

// Save / open dialog of the setup files, in its own thread (the data keeps coming meanwhile)
static bool          fileDialogSave;
static char          fileDialogPath[PLATFORM_PATH_LEN];
static bool          fileDialogChosen;
static atomic_bool   fileDialogDone;
static lv_group_t * settingsGroup;

// Serial tab: terminal (received text waits in logPending until logFlushTimer)
static char       logPending[LOG_PENDING_LEN];
static size_t     logPendingLen;

// Terminal text, shown by rxLog without copy (lv_label_set_text_static): a long text kept in the
// LVGL heap and reallocated at each addition could not be reallocated at some point (heap full /
// fragmented by the plots), and LVGL then dropped it (the terminal wiped itself)
static char       logText[LOG_MAX_LEN + 1];
static size_t     logLen;
// Serial tab hidden: the text is added to logText only, the label laid out again when shown
// (laying out the whole text at each addition was the largest cost of the other tabs)
static bool       logStale;
static bool       logFollow = true;   // the view was at the end of the text: kept there
static lv_obj_t * rxLog;
static lv_obj_t * rxModeDropdown;
static lv_obj_t * txInput;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void topBarInit(lv_obj_t * parent);
static void serialTabInit(lv_obj_t * tab);
static void settingsPanelSet(bool open);
static void settingsContentInit(lv_obj_t * tab);
static void helpContentInit(lv_obj_t * content);

static void profilesRefresh(const char * selectedName);
static const char * profileSelected(void);
static void profileNoteSet(const char * text);
static void fileDialogStart(bool save);
static void fileDialogThread(void * arg);
static void fileDialogPathSet(const char * path, void * context);
static void fileDialogPollTimer(lv_timer_t * tim);
static void fileExportEvent(lv_event_t * e);
static void fileImportEvent(lv_event_t * e);
static void portsCloseAll(void);

static const char * connKey(uint32_t conn, const char * name);
static int32_t      connIndexGet(uint32_t conn, const char * name, int32_t fallback, int32_t count);
static bool         connConfigGet(uint32_t conn, SerialConfig_t * config, bool showErrors);
static void         connWidgetsLoad(void);
static void         connSelUpdate(void);
static void         connLog(uint32_t conn, const char * text);
static void         connLineAdd(uint32_t conn, const char * text);
static void         connLineFlush(uint32_t conn);
static int32_t      connUsingPort(const char * name, uint32_t except);
static void         connClose(uint32_t conn);
static void         lineFormatApply(uint32_t conn);

static void portsRefresh(void);
static bool portsChanged(void);
static void portInfoUpdate(void);
static void portErrorShow(const char * text);
static bool portPresent(const char * name);
static bool portConnect(uint32_t conn, const char * name, const SerialConfig_t * config);
static void statusUpdate(void);
static void logAppend(const char * text);
static void logQueue(const char * text);
static void logFlush(void);
static void logInsert(const char * text);
static void logFlushTimer(lv_timer_t * tim);
static void rxQueueCheck(uint32_t conn);
static void rxWarn(uint32_t conn, const char * text, bool toast);

static void portScanTimer(lv_timer_t * tim);
static void tabChangedEvent(lv_event_t * e);
static void settingsPanelBtnEvent(lv_event_t * e);
static void settingsOpenEvent(lv_event_t * e);
static AppCursor_t cursorAt(lv_point_t point);
static void settingsCloseEvent(lv_event_t * e);
static void helpOpenEvent(lv_event_t * e);
static void helpCloseEvent(lv_event_t * e);
static void tabContentEvent(lv_event_t * e);
static void portSelectedEvent(lv_event_t * e);
static void refreshBtnEvent(lv_event_t * e);
static void baudPresetEvent(lv_event_t * e);
static void connectBtnEvent(lv_event_t * e);
static void lineFormatEvent(lv_event_t * e);
static void connSettingEvent(lv_event_t * e);
static void connSelEvent(lv_event_t * e);
static void connCountEvent(lv_event_t * e);
static void sendEvent(lv_event_t * e);
static bool sendLine(uint32_t conn, const char * text, uint32_t markColor);
static void commandSend(uint32_t conn, const char * text, uint32_t color, bool startsTest);
static void rightClick(lv_point_t point);
static void clearBtnEvent(lv_event_t * e);
static void languageEvent(lv_event_t * e);
static void restartEvent(lv_event_t * e);
static void uiScaleEvent(lv_event_t * e);
static void maxFpsEvent(lv_event_t * e);
static void themeEvent(lv_event_t * e);
static void profileSelectedEvent(lv_event_t * e);
static void profileSaveEvent(lv_event_t * e);
static void profileLoadEvent(lv_event_t * e);
static void profileDeleteEvent(lv_event_t * e);

static void serialRx(SerialPort_t * port, const uint8_t * data, uint32_t len, uint32_t tick);
static void serialDisconnect(SerialPort_t * port);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void loadScreen(void)
{
  // Language and UI scale of the last session, before anything is created (texts, sizes and fonts
  // depend on them)
  langSet((Lang_t) settingsGetInt("language", LANG_EN));

  int32_t scaleIndex = settingsGetInt("uiScale", UI_SCALE_DEFAULT);
  if ((scaleIndex < 0) || (scaleIndex >= (int32_t) (sizeof(uiScales) / sizeof(uiScales[0]))))
  {
    scaleIndex = UI_SCALE_DEFAULT;
  }
  uiScaleSet(uiScales[scaleIndex]);
  appWindowSetScale(uiScales[scaleIndex]);

  lv_obj_t * screen = lv_obj_create(NULL);

  uiStylesInit();

  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_pad_row(screen, 0, 0);
  lv_obj_set_scrollable(screen, false);

  topBarInit(screen);

  tabview = lv_tabview_create(screen);
  lv_tabview_set_tab_bar_size(tabview, uiPx(TAB_BAR_HEIGHT));
  lv_obj_set_width(tabview, LV_PCT(100));
  lv_obj_set_flex_grow(tabview, 1);
  lv_obj_add_event_cb(tabview, tabChangedEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // Tabs change with the tab buttons only: dragging on a plot moves its cursors
  lv_obj_t * tabContent = lv_tabview_get_content(tabview);
  lv_obj_set_scrollable(tabContent, false);
  lv_obj_add_event_cb(tabContent, tabContentEvent, LV_EVENT_SCROLL_END | LV_EVENT_PREPROCESS, NULL);
  lv_obj_add_event_cb(tabContent, tabContentEvent, LV_EVENT_SIZE_CHANGED, NULL);

  // Connections of the last session, each with the port it used
  connCount = (uint32_t) LV_CLAMP(1, settingsGetInt("connCount", 1), CONN_MAX);
  for (uint32_t c = 0; c < CONN_MAX; ++c)
  {
    snprintf(conns[c].selectedPort, SERIAL_PORT_NAME_LEN, "%s", settingsGetString(connKey(c, "port"), ""));
  }

  serialTabInit(lv_tabview_add_tab(tabview, tr("Serial")));

  // The variables are split between the connections
  plotterInit(lv_tabview_add_tab(tabview, tr("Plot")));
  plotterSetConnCount(connCount);
  for (uint32_t c = 0; c < CONN_MAX; ++c)
  {
    lineFormatApply(c);
  }

  compareInit(lv_tabview_add_tab(tabview, tr("Compare")));
  compareSetSendCb(sendLine);
  compareSetConnCount(connCount);

  // Right click resets the zoom of the plot / chart under the pointer
  appWindowSetRightClickCb(rightClick);
  appWindowSetCursorCb(cursorAt);
  cmdPanelSetSendCb(commandSend);
  cmdPanelSetConnCount(connCount);

  // App settings: panel over the tabs, opened with the top bar's settings button. Settings of
  // the last session applied.
  lv_obj_t * settingsContent;
  settingsOverlay = uiOverlayCreate(tabview, tr("Settings"), settingsCloseEvent, &settingsContent);
  settingsContentInit(settingsContent);

  // Help: the same kind of panel, opened with the button next to the settings one
  lv_obj_t * helpContent;
  helpOverlay = uiOverlayCreate(tabview, tr("Help"), helpCloseEvent, &helpContent);
  helpContentInit(helpContent);
  languageEvent(NULL);
  uiScaleEvent(NULL);
  maxFpsEvent(NULL);
  themeEvent(NULL);

  // Settings of the first connection shown (also lists the ports)
  connWidgetsLoad();

  // Auto reconnect also reopens the ports of the last session at startup
  for (uint32_t c = 0; c < connCount; ++c)
  {
    Conn_t * conn = &conns[c];

    if ((connIndexGet(c, "autoReconnect", 0, 2) != 0) && (conn->selectedPort[0] != '\0') &&
        connConfigGet(c, &conn->reconnectConfig, false))
    {
      snprintf(conn->reconnectName, sizeof(conn->reconnectName), "%s", conn->selectedPort);
      conn->reconnectPending = true;
    }
  }

  statusUpdate();
  tabChangedEvent(NULL);
  uiTooltipsInit();
  lv_timer_create(portScanTimer, PORT_SCAN_PERIOD_MS, NULL);
  lv_timer_create(logFlushTimer, LOG_FLUSH_MS, NULL);
  portScanTimer(NULL);
  PROF_DO(profScenarioStart(tabview));

  // Closing the window ends the process with exit() (LVGL's Windows driver): the reader threads
  // are stopped first, not left running while the C library shuts down. Before settingsFlush
  // (registered earlier, so called later).
  atexit(portsCloseAll);

  lv_screen_load(screen);
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

static void topBarInit(lv_obj_t * parent)
{
  lv_obj_t * bar = lv_obj_create(parent);
  lv_obj_set_size(bar, LV_PCT(100), uiPx(TOP_BAR_HEIGHT));
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_BAR_BG), 0);
  lv_obj_set_style_text_color(bar, lv_color_hex(COLOR_BAR_TEXT), 0);
  lv_obj_set_style_pad_hor(bar, uiPx(8), 0);
  lv_obj_set_style_pad_ver(bar, 0, 0);
  lv_obj_set_style_pad_column(bar, uiPx(8), 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollable(bar, false);

  statusLed = lv_led_create(bar);
  lv_obj_set_size(statusLed, uiPx(10), uiPx(10));
  lv_led_set_color(statusLed, lv_color_hex(COLOR_CONNECTED));

  lv_obj_t * title = lv_label_create(bar);
  lv_label_set_text(title, APP_NAME);

  statusLabel = lv_label_create(bar);
  lv_obj_set_flex_grow(statusLabel, 1);
  lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_RIGHT, 0);

  // Help: flat button like the settings one (the symbol font has no question mark)
  lv_obj_t * helpBtn = uiButtonCreate(bar, "?", helpOpenEvent);
  lv_obj_set_style_bg_opa(helpBtn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(helpBtn, 0, 0);
  lv_obj_set_style_text_color(helpBtn, lv_color_hex(COLOR_BAR_TEXT), 0);
  lv_obj_set_style_pad_ver(helpBtn, uiPx(2), 0);
  lv_obj_set_style_pad_hor(helpBtn, uiPx(6), 0);
  uiTooltipSet(helpBtn, tr("How to use the app, author"));

  // App settings: flat button, opens the settings panel over the tabs
  lv_obj_t * settingsBtn = uiButtonCreate(bar, LV_SYMBOL_SETTINGS, settingsOpenEvent);
  lv_obj_set_style_bg_opa(settingsBtn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(settingsBtn, 0, 0);
  lv_obj_set_style_text_color(settingsBtn, lv_color_hex(COLOR_BAR_TEXT), 0);
  lv_obj_set_style_pad_ver(settingsBtn, uiPx(2), 0);
  lv_obj_set_style_pad_hor(settingsBtn, uiPx(4), 0);
  uiTooltipSet(settingsBtn, tr("Settings"));
}

// Port connection (with its settings panel, open by default, folded with its button), received
// data and data to send
static void serialTabInit(lv_obj_t * tab)
{
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, uiPx(4), 0);
  lv_obj_set_style_pad_row(tab, uiPx(4), 0);
  lv_obj_set_scrollable(tab, false);

  // Connection shown below (several connections): its port, settings and the sent text. Each
  // button's dot is the connection's state.
  connSelBtnm = lv_buttonmatrix_create(tab);
  lv_obj_set_size(connSelBtnm, LV_PCT(100), uiPx(30));
  lv_obj_set_style_pad_all(connSelBtnm, uiPx(2), 0);
  lv_obj_set_style_pad_gap(connSelBtnm, uiPx(2), 0);
  lv_obj_add_event_cb(connSelBtnm, connSelEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // Port selection, connection, settings panel toggle
  lv_obj_t * row = uiRowCreate(tab);

  portDropdown = lv_dropdown_create(row);
  lv_obj_add_style(portDropdown, uiStyleCompact(), 0);
  lv_obj_set_flex_grow(portDropdown, 1);
  lv_obj_add_event_cb(portDropdown, portSelectedEvent, LV_EVENT_VALUE_CHANGED, NULL);

  refreshBtn = uiButtonCreate(row, LV_SYMBOL_REFRESH, refreshBtnEvent);

  connectBtn = uiButtonCreate(row, tr("Connect"), connectBtnEvent);
  lv_obj_set_width(connectBtn, uiPx(100));
  connectBtnLabel = lv_obj_get_child(connectBtn, 0);

  lv_obj_t * panelBtn = uiButtonCreate(row, LV_SYMBOL_UP, settingsPanelBtnEvent);
  panelBtnLabel = lv_obj_get_child(panelBtn, 0);

  // Full description of the selected port / error messages
  portInfoLabel = lv_label_create(tab);
  lv_obj_set_width(portInfoLabel, LV_PCT(100));
  lv_label_set_long_mode(portInfoLabel, LV_LABEL_LONG_MODE_WRAP);

  // Port and data format settings; scrolls inside when the window is small
  settingsPanel = lv_obj_create(tab);
  lv_obj_remove_style_all(settingsPanel);
  lv_obj_set_size(settingsPanel, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(settingsPanel, LV_PCT(60), 0);
  lv_obj_set_flex_flow(settingsPanel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(settingsPanel, uiPx(6), 0);
  lv_obj_set_style_pad_bottom(settingsPanel, uiPx(4), 0);
  lv_obj_set_scroll_dir(settingsPanel, LV_DIR_VER);
  uiScrollbarShow(settingsPanel);

  lv_obj_t * panel = settingsPanel;

  // Number of connections at once; they share the plotter's variables
  connCountDropdown = uiDropdownCreate(uiSettingRowCreate(panel, tr("Connections")), connCountOptions, connCount - 1);
  lv_obj_add_event_cb(connCountDropdown, connCountEvent, LV_EVENT_VALUE_CHANGED, NULL);

  connSplitLabel = lv_label_create(panel);
  lv_obj_set_width(connSplitLabel, LV_PCT(100));
  lv_label_set_long_mode(connSplitLabel, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(connSplitLabel, lv_color_hex(COLOR_INFO), 0);

  // Settings of the selected connection (saved per connection: connSettingEvent, keys connKey())
  // Baud rate
  row = uiSettingRowCreate(panel, tr("Baud rate"));

  baudInput = uiTextareaCreate(row, tr("Baud rate"));
  lv_textarea_set_accepted_chars(baudInput, "0123456789");
  lv_textarea_set_max_length(baudInput, BAUD_MAX_DIGITS);
  lv_textarea_set_text(baudInput, DEFAULT_BAUD_RATE);
  lv_obj_add_event_cb(baudInput, connSettingEvent, LV_EVENT_VALUE_CHANGED, (void *) "baud");

  // Only shows the arrow; picking a preset writes it in baudInput
  baudPresetDropdown = uiPresetDropdownCreate(row, baudPresetOptions, baudPresetEvent);

  // Frame format (default 8N1)
  row = uiRowCreate(panel);

  dataBitsDropdown = uiDropdownCreate(row, dataBitsOptions, 0);
  parityDropdown   = uiDropdownCreate(row, tr(parityOptions), SERIAL_PARITY_NONE);
  stopBitsDropdown = uiDropdownCreate(row, tr(stopBitsOptions), SERIAL_STOP_BITS_1);
  lv_obj_add_event_cb(dataBitsDropdown, connSettingEvent, LV_EVENT_VALUE_CHANGED, (void *) "dataBits");
  lv_obj_add_event_cb(parityDropdown, connSettingEvent, LV_EVENT_VALUE_CHANGED, (void *) "parity");
  lv_obj_add_event_cb(stopBitsDropdown, connSettingEvent, LV_EVENT_VALUE_CHANGED, (void *) "stopBits");

  // Reopens the port when it comes back after a disconnection (and at startup)
  row = uiSettingRowCreate(panel, tr("Auto reconnect"));
  autoReconnectSwitch = lv_switch_create(row);
  lv_obj_add_event_cb(autoReconnectSwitch, connSettingEvent, LV_EVENT_VALUE_CHANGED, (void *) "autoReconnect");

  // Data format: how received lines are split for the plotter, and what ends sent lines
  uiSectionTitleCreate(panel, tr("Data format"));

  separatorDropdown  = uiDropdownCreate(uiSettingRowCreate(panel, tr("Separator")), tr(separatorOptions),
                                        SEPARATOR_AUTO);
  varCountDropdown   = uiDropdownCreate(uiSettingRowCreate(panel, tr("Variables")), varCountOptions, VAR_COUNT_AUTO);
  lineEndingDropdown = uiDropdownCreate(uiSettingRowCreate(panel, tr("Line ending")), tr(lineEndingOptions),
                                        LINE_ENDING_CRLF);

  // Saved first, then applied (events run in the order they were added)
  lv_obj_add_event_cb(separatorDropdown, connSettingEvent, LV_EVENT_VALUE_CHANGED, (void *) "separator");
  lv_obj_add_event_cb(varCountDropdown, connSettingEvent, LV_EVENT_VALUE_CHANGED, (void *) "varCount");
  lv_obj_add_event_cb(lineEndingDropdown, connSettingEvent, LV_EVENT_VALUE_CHANGED, (void *) "lineEnding");

  lv_obj_add_event_cb(separatorDropdown, lineFormatEvent, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(varCountDropdown, lineFormatEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // Received data (read only), takes the remaining height
  rxLog = lv_textarea_create(tab);
  lv_obj_set_width(rxLog, LV_PCT(100));
  lv_obj_set_flex_grow(rxLog, 1);
  lv_obj_add_style(rxLog, uiStyleCompact(), 0);
  lv_textarea_set_placeholder_text(rxLog, tr("Received data will appear here"));
  lv_label_set_text_static(lv_textarea_get_label(rxLog), logText);
  lv_obj_set_click_focusable(rxLog, false);
  lv_obj_set_style_opa(rxLog, LV_OPA_TRANSP, LV_PART_CURSOR);

  // Data to send: Enter key or Send button
  row = uiRowCreate(tab);

  txInput = uiTextareaCreate(row, tr("Type, then Enter"));
  lv_obj_add_event_cb(txInput, sendEvent, LV_EVENT_READY, NULL);

  rxModeDropdown = uiDropdownCreate(row, tr(rxModeOptions), RX_MODE_TEXT);
  lv_obj_set_flex_grow(rxModeDropdown, 0);
  lv_obj_set_width(rxModeDropdown, uiPx(72));
  settingsBindDropdown(rxModeDropdown, "rxMode");

  lv_obj_t * sendBtn  = uiButtonCreate(row, LV_SYMBOL_RIGHT, sendEvent);
  lv_obj_t * clearBtn = uiButtonCreate(row, LV_SYMBOL_TRASH, clearBtnEvent);

  // The PC keyboard types in these while the tab is shown (txInput by default)
  serialGroup = lv_group_create();
  lv_group_add_obj(serialGroup, baudInput);
  lv_group_add_obj(serialGroup, txInput);

  uiTooltipSet(connSelBtnm, tr("Connection shown below (port, settings, sent text). Green: connected, amber: "
                               "waiting to reconnect."));
  uiTooltipSet(portDropdown, tr("Serial port (VCOM / USB)"));
  uiTooltipSet(refreshBtn, tr("Refresh the port list"));
  uiTooltipSet(connectBtn, tr("Open / close the selected port"));
  uiTooltipSet(panelBtn, tr("Show / hide the connection settings"));
  uiTooltipSet(connCountDropdown, tr("Number of serial ports used at once; they share the 32 variables"));
  uiTooltipSet(baudPresetDropdown, tr("Common baud rates"));
  uiTooltipSet(autoReconnectSwitch, tr("Reopen the port when it comes back (unplugged / at startup)"));
  uiTooltipSet(separatorDropdown, tr("Character between the values of a received line"));
  uiTooltipSet(varCountDropdown, tr("Values plotted per line (Auto: as many as received)"));
  uiTooltipSet(lineEndingDropdown, tr("Added at the end of each sent line"));
  uiTooltipSet(rxModeDropdown, tr("How the received bytes are shown in the terminal"));
  uiTooltipSet(sendBtn, tr("Send (Enter)"));
  uiTooltipSet(clearBtn, tr("Clear the terminal"));
}

// Shows / folds away the port and data format settings
static void settingsPanelSet(bool open)
{
  lv_obj_set_hidden(settingsPanel, !open);
  lv_label_set_text(panelBtnLabel, open ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
}

// Settings profiles, UI scale, max frame rate and color theme of the app (tab: column of the
// settings panel)
static void settingsContentInit(lv_obj_t * tab)
{
  // Profiles: the project settings (port, format, variables, plots, commands) saved under a name
  uiSectionTitleCreate(tab, tr("Profiles"));

  lv_obj_t * row = uiSettingRowCreate(tab, tr("Profile"));
  profileDropdown = uiDropdownCreate(row, "", 0);
  lv_obj_add_event_cb(profileDropdown, profileSelectedEvent, LV_EVENT_VALUE_CHANGED, NULL);
  profileLoadBtn   = uiButtonCreate(row, tr("Load"), profileLoadEvent);
  profileDeleteBtn = uiButtonCreate(row, LV_SYMBOL_TRASH, profileDeleteEvent);
  uiTooltipSet(profileLoadBtn, tr("Load the selected profile (restarts the app)"));
  uiTooltipSet(profileDeleteBtn, tr("Delete the selected profile"));

  row = uiSettingRowCreate(tab, tr("Save as"));
  profileNameInput = uiTextareaCreate(row, tr("Profile name"));
  lv_textarea_set_max_length(profileNameInput, SETTINGS_PROFILE_NAME_LEN - 1);
  lv_textarea_set_accepted_chars(profileNameInput,
                                 "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.");
  lv_obj_add_event_cb(profileNameInput, profileSaveEvent, LV_EVENT_READY, NULL);
  uiTooltipSet(uiButtonCreate(row, tr(LV_SYMBOL_SAVE " Save"), profileSaveEvent),
               tr("Save the current settings under this name"));

  // The whole setup or the plots' layout, in a file anywhere (to share or back up)
  row = uiSettingRowCreate(tab, tr("File"));
  fileKindDropdown = uiDropdownCreate(row, tr("Whole setup\nPlot layout"), SETTINGS_FILE_SETUP);
  uiTooltipSet(fileKindDropdown, tr("Whole setup: as a profile (port, variables, plots, commands, tests). Plot layout: the plots, their windows and which variables they show."));
  fileExportBtn = uiButtonCreate(row, tr("Export"), fileExportEvent);
  uiTooltipSet(fileExportBtn, tr("Save it to a file"));
  fileImportBtn = uiButtonCreate(row, tr("Import"), fileImportEvent);
  uiTooltipSet(fileImportBtn, tr("Open a setup or plot layout file (restarts the app)"));

  profileNoteLabel = lv_label_create(tab);
  lv_obj_set_width(profileNoteLabel, LV_PCT(100));
  lv_obj_set_style_text_color(profileNoteLabel, lv_color_hex(COLOR_INFO), 0);
  lv_label_set_long_mode(profileNoteLabel, LV_LABEL_LONG_MODE_WRAP);

  settingsGroup = lv_group_create();
  lv_group_add_obj(settingsGroup, profileNameInput);

  profilesRefresh(settingsGetString(SETTINGS_PROFILE_KEY, ""));
  lv_textarea_set_text(profileNameInput, profileSelected());
  profileNoteSet(NULL);

  uiSectionTitleCreate(tab, tr("Display"));

  // The texts are set when the UI is created: a new language is used at the next start (or now,
  // with the restart button)
  languageDropdown = uiDropdownCreate(uiSettingRowCreate(tab, tr("Language")), LANG_OPTIONS, langGet());
  settingsBindDropdown(languageDropdown, "language");
  lv_obj_add_event_cb(languageDropdown, languageEvent, LV_EVENT_VALUE_CHANGED, NULL);

  languageNoteRow = uiRowCreate(tab);
  lv_obj_t * languageNote = lv_label_create(languageNoteRow);
  lv_obj_set_flex_grow(languageNote, 1);
  lv_label_set_long_mode(languageNote, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(languageNote, lv_color_hex(COLOR_INFO), 0);
  lv_label_set_text(languageNote, tr("The language changes at the next start."));
  uiButtonCreate(languageNoteRow, tr("Restart now"), restartEvent);

  uiScaleDropdown = uiDropdownCreate(uiSettingRowCreate(tab, tr("UI scale")), uiScaleOptions, UI_SCALE_DEFAULT);
  settingsBindDropdown(uiScaleDropdown, "uiScale");
  lv_obj_add_event_cb(uiScaleDropdown, uiScaleEvent, LV_EVENT_VALUE_CHANGED, NULL);

  // The UI is built at its scale: a new scale is used at the next start
  scaleNoteLabel = lv_label_create(tab);
  lv_obj_set_style_text_color(scaleNoteLabel, lv_color_hex(COLOR_INFO), 0);
  lv_label_set_text(scaleNoteLabel, tr("The new scale applies at the next start."));

  maxFpsDropdown = uiDropdownCreate(uiSettingRowCreate(tab, tr("Max frame rate")), tr(maxFpsOptions), MAX_FPS_DEFAULT);
  settingsBindDropdown(maxFpsDropdown, "maxFps");
  lv_obj_add_event_cb(maxFpsDropdown, maxFpsEvent, LV_EVENT_VALUE_CHANGED, NULL);

  uiSectionTitleCreate(tab, tr("Colors"));

  themeDropdown = uiDropdownCreate(uiSettingRowCreate(tab, tr("Theme")), tr(themeOptions), THEME_LIGHT);
  settingsBindDropdown(themeDropdown, "theme");
  lv_obj_add_event_cb(themeDropdown, themeEvent, LV_EVENT_VALUE_CHANGED, NULL);

  accentDropdown = uiDropdownCreate(uiSettingRowCreate(tab, tr("Accent color")), tr(accentOptions), 0);
  settingsBindDropdown(accentDropdown, "accent");
  lv_obj_add_event_cb(accentDropdown, themeEvent, LV_EVENT_VALUE_CHANGED, NULL);
}

// Rebuilds the profile list, selecting selectedName if it is in it
static void profilesRefresh(const char * selectedName)
{
  size_t   used = 0;
  uint32_t selected = 0;

  profileCount = settingsProfileList(profileNames, PROFILE_MAX);
  profileOptions[0] = '\0';

  for (uint32_t i = 0; i < profileCount; ++i)
  {
    used += (size_t) snprintf(&profileOptions[used], sizeof(profileOptions) - used, "%s%s", (i > 0) ? "\n" : "",
                              profileNames[i]);
    if (strcmp(profileNames[i], selectedName) == 0)
    {
      selected = i;
    }
  }

  lv_dropdown_set_options_static(profileDropdown, (profileCount > 0) ? profileOptions : tr("(none saved)"));
  lv_dropdown_set_selected(profileDropdown, selected);
  lv_obj_set_state(profileLoadBtn, LV_STATE_DISABLED, profileCount == 0);
  lv_obj_set_state(profileDeleteBtn, LV_STATE_DISABLED, profileCount == 0);
  profileDeletePending[0] = '\0';
}

// Name of the profile selected in the list ("" if there is none)
static const char * profileSelected(void)
{
  return (profileCount > 0) ? profileNames[lv_dropdown_get_selected(profileDropdown)] : "";
}

// text: result of the last action; NULL: the current profile
static void profileNoteSet(const char * text)
{
  const char * current = settingsGetString(SETTINGS_PROFILE_KEY, "");

  if (text != NULL)
  {
    lv_label_set_text(profileNoteLabel, text);
  }
  else if (current[0] != '\0')
  {
    lv_label_set_text_fmt(profileNoteLabel, tr("Last profile saved / loaded: %s. Loading a profile restarts the app."),
                          current);
  }
  else
  {
    lv_label_set_text(profileNoteLabel, tr("A profile keeps the port, data format, variables, plots and commands. "
                                           "Loading a profile restarts the app."));
  }
}

// Settings key of a connection's setting: the first connection uses the keys of the former single
// connection ("baud"), the others "c2.baud"... (static buffer: use it before the next call)
static const char * connKey(uint32_t conn, const char * name)
{
  static char key[CONN_KEY_LEN];

  if (conn == 0)
  {
    return name;
  }

  snprintf(key, sizeof(key), "c%lu.%s", (unsigned long) (conn + 1), name);
  return key;
}

// Saved option index of a connection's setting, fallback when missing / out of 0..count-1
static int32_t connIndexGet(uint32_t conn, const char * name, int32_t fallback, int32_t count)
{
  int32_t value = settingsGetInt(connKey(conn, name), fallback);
  return ((value >= 0) && (value < count)) ? value : fallback;
}

// Baud rate and frame format saved for a connection; false when not valid (showErrors: with a
// message, for the selected connection)
static bool connConfigGet(uint32_t conn, SerialConfig_t * config, bool showErrors)
{
  config->baudRate = (uint32_t) strtoul(settingsGetString(connKey(conn, "baud"), DEFAULT_BAUD_RATE), NULL, 10);
  config->dataBits = dataBitsValues[connIndexGet(conn, "dataBits", 0, sizeof(dataBitsValues))];
  config->parity   = (SerialParity_t) connIndexGet(conn, "parity", SERIAL_PARITY_NONE, SERIAL_PARITY_SPACE + 1);
  config->stopBits = (SerialStopBits_t) connIndexGet(conn, "stopBits", SERIAL_STOP_BITS_1, SERIAL_STOP_BITS_2 + 1);

  if (config->baudRate == 0)
  {
    if (showErrors)
    {
      portErrorShow(tr("Enter a baud rate."));
    }
    return false;
  }

  if (!serialFormatIsValid(config))
  {
    if (showErrors)
    {
      portErrorShow(tr("Invalid format: 1.5 stop bits needs 5 data bits,\n2 stop bits needs 6 to 8 data bits."));
    }
    return false;
  }

  return true;
}

// Shows the selected connection's settings, port and state
static void connWidgetsLoad(void)
{
  connLoading = true;
  lv_textarea_set_text(baudInput, settingsGetString(connKey(connSel, "baud"), DEFAULT_BAUD_RATE));
  lv_dropdown_set_selected(dataBitsDropdown, (uint32_t) connIndexGet(connSel, "dataBits", 0, sizeof(dataBitsValues)));
  lv_dropdown_set_selected(parityDropdown,
                           (uint32_t) connIndexGet(connSel, "parity", SERIAL_PARITY_NONE, SERIAL_PARITY_SPACE + 1));
  lv_dropdown_set_selected(stopBitsDropdown,
                           (uint32_t) connIndexGet(connSel, "stopBits", SERIAL_STOP_BITS_1, SERIAL_STOP_BITS_2 + 1));
  lv_obj_set_state(autoReconnectSwitch, LV_STATE_CHECKED, connIndexGet(connSel, "autoReconnect", 0, 2) != 0);
  lv_dropdown_set_selected(separatorDropdown,
                           (uint32_t) connIndexGet(connSel, "separator", SEPARATOR_AUTO,
                                                   sizeof(separatorChars) / sizeof(separatorChars[0])));
  lv_dropdown_set_selected(varCountDropdown,
                           (uint32_t) connIndexGet(connSel, "varCount", VAR_COUNT_AUTO, PLOTTER_MAX_VARS + 1));
  lv_dropdown_set_selected(lineEndingDropdown,
                           (uint32_t) connIndexGet(connSel, "lineEnding", LINE_ENDING_CRLF,
                                                   sizeof(lineEndings) / sizeof(lineEndings[0])));
  connLoading = false;

  // With several connections, the variables plotted per connection
  lv_obj_set_hidden(connSplitLabel, connCount == 1);
  lv_label_set_text_fmt(connSplitLabel, tr("Each connection plots up to %lu variables (C1: Var 1-%lu, C2: Var %lu-%lu...)."),
                        (unsigned long) plotterConnVarCount(), (unsigned long) plotterConnVarCount(),
                        (unsigned long) (plotterConnVarCount() + 1), (unsigned long) (2 * plotterConnVarCount()));

  if (connCount > 1)
  {
    char placeholder[48];
    snprintf(placeholder, sizeof(placeholder), tr("Send to C%lu, Enter"), (unsigned long) (connSel + 1));
    lv_textarea_set_placeholder_text(txInput, placeholder);
  }
  else
  {
    lv_textarea_set_placeholder_text(txInput, tr("Type, then Enter"));
  }

  portsRefresh();
  statusUpdate();
}

// Selector labels: "<dot> C2", the dot green when connected, amber when waiting to reconnect
static void connSelUpdate(void)
{
  lv_obj_set_hidden(connSelBtnm, connCount == 1);

  for (uint32_t c = 0; c < connCount; ++c)
  {
    bool     open  = serialIsOpen(&conns[c].port);
    uint32_t color = open ? COLOR_CONNECTED : (conns[c].reconnectPending ? COLOR_WAITING : COLOR_INFO);

    snprintf(connSelLabels[c], sizeof(connSelLabels[c]), "#%06lX " LV_SYMBOL_BULLET "# C%lu%s%s",
             (unsigned long) color, (unsigned long) (c + 1), open ? " " : "", open ? conns[c].port.name : "");
    connSelMap[c] = connSelLabels[c];
  }

  connSelMap[connCount] = "";

  // A new map clears the buttons' controls: set again
  lv_buttonmatrix_set_map(connSelBtnm, connSelMap);
  lv_buttonmatrix_set_button_ctrl_all(connSelBtnm, LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                   LV_BUTTONMATRIX_CTRL_RECOLOR);
  lv_buttonmatrix_set_one_checked(connSelBtnm, true);
  lv_buttonmatrix_set_button_ctrl(connSelBtnm, connSel, LV_BUTTONMATRIX_CTRL_CHECKED);
}

// Message of a connection in the terminal ("[C2] " first with several connections)
static void connLog(uint32_t conn, const char * text)
{
  if (connCount > 1)
  {
    char prefix[8];
    snprintf(prefix, sizeof(prefix), "[C%lu] ", (unsigned long) (conn + 1));
    logAppend(prefix);
  }

  logAppend(text);
}

// Received text (several connections): shown by whole lines, each with its connection
static void connLineAdd(uint32_t conn, const char * text)
{
  Conn_t * c = &conns[conn];

  for (; *text != '\0'; ++text)
  {
    if (c->rxLineLen == 0)
    {
      c->rxLineTime = lv_tick_get();
    }

    if (*text == '\n')
    {
      connLineFlush(conn);
    }
    else
    {
      c->rxLine[c->rxLineLen++] = *text;

      if (c->rxLineLen >= CONN_LINE_MAX)
      {
        connLineFlush(conn);
      }
    }
  }
}

// The waiting line (complete, too long, or without its end for CONN_LINE_MS) goes to the terminal
static void connLineFlush(uint32_t conn)
{
  Conn_t * c = &conns[conn];
  char     text[CONN_LINE_MAX + 12];

  snprintf(text, sizeof(text), "[C%lu] %.*s\n", (unsigned long) (conn + 1), (int) c->rxLineLen, c->rxLine);
  c->rxLineLen = 0;
  logQueue(text);
}

// Connection (other than except) that has port name open / is waiting for it; -1 if none
static int32_t connUsingPort(const char * name, uint32_t except)
{
  for (uint32_t c = 0; c < connCount; ++c)
  {
    if ((c != except) &&
        ((serialIsOpen(&conns[c].port) && (strcmp(conns[c].port.name, name) == 0)) ||
         (!serialIsOpen(&conns[c].port) && conns[c].reconnectPending && (strcmp(conns[c].reconnectName, name) == 0))))
    {
      return (int32_t) c;
    }
  }

  return -1;
}

// Closes the connection / cancels its reconnection
static void connClose(uint32_t conn)
{
  if (serialIsOpen(&conns[conn].port))
  {
    connLog(conn, tr("[Disconnected]\n"));
    serialClose(&conns[conn].port);
  }

  conns[conn].reconnectPending = false;
  conns[conn].rxLineLen = 0;
  plotterResetLine(conn);
}

// Separator and number of variables of a connection's lines
static void lineFormatApply(uint32_t conn)
{
  uint32_t separator = (uint32_t) connIndexGet(conn, "separator", SEPARATOR_AUTO,
                                               sizeof(separatorChars) / sizeof(separatorChars[0]));

  plotterSetLineFormat(conn, separatorChars[separator], separator == SEPARATOR_AUTO,
                       (uint32_t) connIndexGet(conn, "varCount", VAR_COUNT_AUTO, PLOTTER_MAX_VARS + 1));
}

// Rebuilds the port dropdown, keeping the selected connection's port selected when present. Ports
// used by the other connections are marked ("(C2)").
static void portsRefresh(void)
{
  static char options[SERIAL_MAX_PORTS * (SERIAL_PORT_NAME_LEN + SERIAL_PORT_DESC_LEN + 8)];
  char        selectedName[SERIAL_PORT_NAME_LEN] = {0};
  uint32_t    selected = 0;
  size_t      used = 0;

  if (serialIsOpen(&conns[connSel].port))
  {
    snprintf(selectedName, sizeof(selectedName), "%s", conns[connSel].port.name);
  }
  else
  {
    snprintf(selectedName, sizeof(selectedName), "%s", conns[connSel].selectedPort);
  }

  portCount = serialListPorts(portList, SERIAL_MAX_PORTS);
  options[0] = '\0';

  for (uint32_t i = 0; (i < portCount) && (used < sizeof(options)); ++i)
  {
    // "COM5 - STMicroelectronics Virtual COM Port" (drop the redundant " (COM5)" suffix)
    const char * desc   = portList[i].description;
    const char * suffix = strstr(desc, " (COM");
    int          descLen = (suffix != NULL) ? (int) (suffix - desc) : (int) strlen(desc);
    int32_t      user    = connUsingPort(portList[i].name, connSel);
    char         userText[8] = "";

    if (user >= 0)
    {
      snprintf(userText, sizeof(userText), " (C%ld)", (long) (user + 1));
    }

    used += (size_t) snprintf(&options[used], sizeof(options) - used, "%s%s%s - %.*s",
                              (i > 0) ? "\n" : "", portList[i].name, userText, descLen, desc);

    if (strcmp(portList[i].name, selectedName) == 0)
    {
      selected = i;
    }
  }

  if (portCount == 0)
  {
    lv_dropdown_set_options(portDropdown, tr("No port found"));
  }
  else
  {
    lv_dropdown_set_options(portDropdown, options);
    lv_dropdown_set_selected(portDropdown, selected);
  }

  portInfoUpdate();
}

static bool portsChanged(void)
{
  SerialPortInfo_t current[SERIAL_MAX_PORTS];
  uint32_t         count = serialListPorts(current, SERIAL_MAX_PORTS);

  if (count != portCount)
  {
    return true;
  }

  for (uint32_t i = 0; i < count; ++i)
  {
    if (strcmp(current[i].name, portList[i].name) != 0)
    {
      return true;
    }
  }

  return false;
}

static void portInfoUpdate(void)
{
  lv_obj_set_style_text_color(portInfoLabel, lv_color_hex(COLOR_INFO), 0);

  if (portCount == 0)
  {
    lv_label_set_text(portInfoLabel, tr("Plug in a VCOM / USB serial device."));
  }
  else
  {
    lv_label_set_text(portInfoLabel, portList[lv_dropdown_get_selected(portDropdown)].description);
  }
}

static void portErrorShow(const char * text)
{
  lv_label_set_text(portInfoLabel, text);
  lv_obj_set_style_text_color(portInfoLabel, lv_color_hex(COLOR_ERROR), 0);
}

static bool portPresent(const char * name)
{
  SerialPortInfo_t current[SERIAL_MAX_PORTS];
  uint32_t         count = serialListPorts(current, SERIAL_MAX_PORTS);

  for (uint32_t i = 0; i < count; ++i)
  {
    if (strcmp(current[i].name, name) == 0)
    {
      return true;
    }
  }

  return false;
}

// Opens the port on a connection; on success it becomes that connection's port of the session
// (saved for the next one)
static bool portConnect(uint32_t conn, const char * name, const SerialConfig_t * config)
{
  Conn_t * c = &conns[conn];
  char     msg[64];
  char     format[8];

  if (!serialOpen(&c->port, name, config, serialRx, serialDisconnect, (void *) (uintptr_t) conn))
  {
    return false;
  }

  serialFormatToString(config, format, sizeof(format));
  snprintf(msg, sizeof(msg), tr("[Connected to %s %lu %s]\n"), name, (unsigned long) config->baudRate, format);
  connLog(conn, msg);

  settingsSetString(connKey(conn, "port"), name);
  snprintf(c->selectedPort, sizeof(c->selectedPort), "%s", name);
  c->rxLineLen = 0;
  plotterResetLine(conn);
  c->reconnectPending = false;
  return true;
}

// Top bar: the connection (several: each one's short state); selected connection's buttons
static void statusUpdate(void)
{
  Conn_t * sel     = &conns[connSel];
  bool     isOpen  = serialIsOpen(&sel->port);
  bool     waiting = !isOpen && sel->reconnectPending;
  bool     locked  = isOpen || waiting;
  bool     anyOpen = false;
  char     text[CONN_MAX * 48] = "";
  size_t   used = 0;

  for (uint32_t c = 0; c < connCount; ++c)
  {
    Conn_t * conn  = &conns[c];
    char     state[48];
    char     format[8];

    if (serialIsOpen(&conn->port))
    {
      serialFormatToString(&conn->port.config, format, sizeof(format));
      snprintf(state, sizeof(state), "%s %lu %s", conn->port.name, (unsigned long) conn->port.config.baudRate, format);
      anyOpen = true;
    }
    else if (conn->reconnectPending)
    {
      snprintf(state, sizeof(state), tr("Waiting for %s..."), conn->reconnectName);
    }
    else
    {
      snprintf(state, sizeof(state), "%s", (connCount == 1) ? tr("Disconnected") : "-");
    }

    if (connCount == 1)
    {
      snprintf(text, sizeof(text), "%s", state);
    }
    else
    {
      used += (size_t) snprintf(&text[used], sizeof(text) - used, "%sC%lu %s", (c > 0) ? "  |  " : "",
                                (unsigned long) (c + 1), state);
    }
  }

  lv_label_set_text(statusLabel, text);
  if (anyOpen)
  {
    lv_led_on(statusLed);
  }
  else
  {
    lv_led_off(statusLed);
  }

  connSelUpdate();

  // While waiting to reconnect, the button cancels the reconnection
  lv_label_set_text(connectBtnLabel, tr(isOpen ? "Disconnect" : (waiting ? "Cancel" : "Connect")));

  // Port settings are locked while connected / waiting to reconnect
  lv_obj_set_state(portDropdown, LV_STATE_DISABLED, locked);
  lv_obj_set_state(baudInput, LV_STATE_DISABLED, locked);
  lv_obj_set_state(baudPresetDropdown, LV_STATE_DISABLED, locked);
  lv_obj_set_state(dataBitsDropdown, LV_STATE_DISABLED, locked);
  lv_obj_set_state(parityDropdown, LV_STATE_DISABLED, locked);
  lv_obj_set_state(stopBitsDropdown, LV_STATE_DISABLED, locked);
  lv_obj_set_state(refreshBtn, LV_STATE_DISABLED, locked);
  lv_obj_set_state(connectBtn, LV_STATE_DISABLED, !locked && (portCount == 0));
}

// Messages ("[Connected ...]", "> sent text"): shown right away, after the received text
static void logAppend(const char * text)
{
  logFlush();
  logInsert(text);
}

// Received text: gathered and added by logFlushTimer, so a fast data rate does not lay out the
// terminal text for every received chunk
static void logQueue(const char * text)
{
  size_t len = strlen(text);

  if ((logPendingLen + len) >= LOG_PENDING_LEN)
  {
    logFlush();
  }

  if (len >= LOG_PENDING_LEN)
  {
    logInsert(text);
    return;
  }

  memcpy(&logPending[logPendingLen], text, len);
  logPendingLen += len;
}

static void logFlush(void)
{
  if (logPendingLen == 0)
  {
    return;
  }

  logPending[logPendingLen] = '\0';
  logPendingLen = 0;
  logInsert(logPending);
}

// Adds text at the end; when the log is too long, the oldest lines are removed so that
// LOG_TRIM_LEN characters remain, starting at a line beginning
static void logInsert(const char * text)
{
  size_t len = strlen(text);
  bool   shown = lv_tabview_get_tab_active(tabview) == TAB_SERIAL;

  PROF_PUSH(PROF_TERMINAL_LABEL);

  // Following the new text (view at the bottom): kept at the bottom after the update. Hidden: as
  // it was when last shown.
  bool atBottom = shown ? (lv_obj_get_scroll_bottom(rxLog) <= uiPx(4)) : logFollow;

  // Longer than the whole log: only its end is kept
  if (len > LOG_TRIM_LEN)
  {
    text = &text[len - LOG_TRIM_LEN];
    len  = LOG_TRIM_LEN;
  }

  if ((logLen + len) > LOG_MAX_LEN)
  {
    size_t       keep  = LV_MIN(logLen, LOG_TRIM_LEN - len);
    const char * start = &logText[logLen - keep];
    const char * lineStart = memchr(start, '\n', keep);

    if (lineStart != NULL)
    {
      keep -= (size_t) (lineStart + 1 - start);
      start = lineStart + 1;
    }

    memmove(logText, start, keep);
    logLen = keep;
  }

  memcpy(&logText[logLen], text, len);
  logLen += len;
  logText[logLen] = '\0';
  logFollow = atBottom;

  if (!shown)
  {
    logStale = true;
    PROF_POP();
    return;
  }

  // Same buffer: the label only refreshes (no copy, no LVGL heap)
  lv_label_set_text_static(lv_textarea_get_label(rxLog), logText);

  // The removed lines shorten the text: with the old layout, the view at the bottom would be
  // past the end of the text (blank) until the next update. Scrolled to the real end instead;
  // when reading higher up, the view is left alone.
  if (atBottom)
  {
    lv_obj_update_layout(rxLog);
    lv_obj_scroll_by(rxLog, 0, -lv_obj_get_scroll_bottom(rxLog), LV_ANIM_OFF);
  }

  PROF_POP();
}

// Laying out the terminal text is costly: while the Serial tab is not shown, the received text
// is added in larger batches (half of the waiting buffer)
static void logFlushTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  for (uint32_t c = 0; c < connCount; ++c)
  {
    rxQueueCheck(c);
  }

  // Several connections: a line still without its end is shown after CONN_LINE_MS
  for (uint32_t c = 0; (c < connCount) && (connCount > 1); ++c)
  {
    if ((conns[c].rxLineLen > 0) && (lv_tick_elaps(conns[c].rxLineTime) >= CONN_LINE_MS))
    {
      connLineFlush(c);
    }
  }

  if ((lv_tabview_get_tab_active(tabview) == TAB_SERIAL) || (logPendingLen >= (LOG_PENDING_LEN / 2)))
  {
    logFlush();
  }
}

// Tells in the terminal (and a message over the window) when received data had to wait or was
// lost before the app, and when the app falls behind the data (more than half the queue waiting)
static void rxQueueCheck(uint32_t conn)
{
  Conn_t *      c = &conns[conn];
  SerialStats_t stats;
  char          text[200];

  if (!serialStatsGet(&c->port, &stats))
  {
    memset(&c->reported, 0, sizeof(c->reported));
    c->backlogWarned = false;
    return;
  }

  if (stats.fullWaits != c->reported.fullWaits)
  {
    snprintf(text, sizeof(text), tr("[%s: receive queue full, reading paused %lu ms (data kept; the device may have been held back)]\n"),
             c->port.name, (unsigned long) (stats.fullWaitMs - c->reported.fullWaitMs));
    rxWarn(conn, text, true);
  }

  if (stats.driverOverruns != c->reported.driverOverruns)
  {
    snprintf(text, sizeof(text), tr("[%s: Windows reports received data lost before the app could read it]\n"), c->port.name);
    rxWarn(conn, text, true);
  }

  if (stats.lineErrors != c->reported.lineErrors)
  {
    snprintf(text, sizeof(text), tr("[%s: framing / parity errors reported by Windows]\n"), c->port.name);
    rxWarn(conn, text, false);
  }

  uint32_t percent = (uint32_t) ((100ull * stats.queuedChunks) / LV_MAX(stats.capacityChunks, 1u));

  percent = LV_MAX(percent, (uint32_t) ((100ull * stats.queuedBytes) / LV_MAX(stats.capacityBytes, 1u)));

  // Once, until it drains
  if ((percent >= 50) && !c->backlogWarned)
  {
    snprintf(text, sizeof(text), tr("[%s: %lu%% of the receive queue waiting (the app is behind the data)]\n"), c->port.name,
             (unsigned long) percent);
    rxWarn(conn, text, true);
    c->backlogWarned = true;
  }
  else if (percent < 10)
  {
    c->backlogWarned = false;
  }

  c->reported = stats;
}

// In the terminal, over the window (toast) and to the command line tool's messages
static void rxWarn(uint32_t conn, const char * text, bool toast)
{
  connLog(conn, text);

  if (toast)
  {
    plotterToast(text);
  }

#ifdef SERIAL_DASHBOARD_CLI
  cliMessage(text);
#endif
}

// Detects VCOM devices being plugged / unplugged, and reconnects the connections waiting for
// their port (not while every connection is open)
static void portScanTimer(lv_timer_t * tim)
{
  bool allOpen = true;

  LV_UNUSED(tim);

  for (uint32_t c = 0; c < connCount; ++c)
  {
    allOpen = allOpen && serialIsOpen(&conns[c].port);
  }

  if (allOpen)
  {
    return;
  }

  PROF_PUSH(PROF_PORT_SCAN);

  if (!lv_dropdown_is_open(portDropdown) && portsChanged())
  {
    portsRefresh();
    statusUpdate();
  }

  for (uint32_t c = 0; c < connCount; ++c)
  {
    Conn_t * conn = &conns[c];

    // Not while another connection has that port
    if (conn->reconnectPending && !serialIsOpen(&conn->port) && (connUsingPort(conn->reconnectName, c) < 0) &&
        portPresent(conn->reconnectName) && portConnect(c, conn->reconnectName, &conn->reconnectConfig))
    {
      portsRefresh();
      statusUpdate();
    }
  }

  PROF_POP();
}

// The PC keyboard types in the text boxes of the shown tab
static void tabChangedEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  // Terminal text added while hidden: laid out once now
  if ((lv_tabview_get_tab_active(tabview) == TAB_SERIAL) && logStale)
  {
    logStale = false;
    lv_label_set_text_static(lv_textarea_get_label(rxLog), logText);

    if (logFollow)
    {
      lv_obj_update_layout(rxLog);
      lv_obj_scroll_by(rxLog, 0, -lv_obj_get_scroll_bottom(rxLog), LV_ANIM_OFF);
    }
  }

  // The settings panel covers the tabs; its text box is the profile name
  if (!lv_obj_is_hidden(settingsOverlay))
  {
    uiKeypadGroupSet(settingsGroup);
    return;
  }

  if (lv_tabview_get_tab_active(tabview) == TAB_SERIAL)
  {
    uiKeypadGroupSet(serialGroup);
    lv_group_focus_obj(txInput);
  }
  else if (lv_tabview_get_tab_active(tabview) == TAB_COMPARE)
  {
    uiKeypadGroupSet(compareKeypadGroup());
  }
  else
  {
    uiKeypadGroupSet(plotterKeypadGroup());
  }
}

// Mouse cursor: the plots' resize cursors, unless the settings / help panel covers them
static AppCursor_t cursorAt(lv_point_t point)
{
  if (!lv_obj_is_hidden(settingsOverlay) || !lv_obj_is_hidden(helpOverlay))
  {
    return APP_CURSOR_ARROW;
  }

  return plotterCursorAt(point);
}

static void settingsOpenEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_hidden(helpOverlay, true);
  lv_obj_set_hidden(settingsOverlay, false);
  tabChangedEvent(NULL);
}

static void settingsCloseEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_hidden(settingsOverlay, true);
  tabChangedEvent(NULL);
}

// Only one panel at a time
static void helpOpenEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_hidden(settingsOverlay, true);
  lv_obj_set_hidden(helpOverlay, false);
  tabChangedEvent(NULL);
}

static void helpCloseEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_hidden(helpOverlay, true);
  tabChangedEvent(NULL);
}

// A box per part of the app, then who made it
static void helpContentInit(lv_obj_t * content)
{
  static const char * const sections[][2] =
  {
    {"Serial",
     "Choose the port and the baud rate, then Connect. What the device sends shows in the terminal; type a line at the bottom to send it. Several devices at once: choose the number of Connections, each with its own port."},
    {"Plot",
     "Each line received is split into numbers: one variable per number. Drag along an axis to zoom, right-click a plot to reset it. V1 / V2 / H1 / H2 place cursors to measure. The gear button sets each variable's name, unit, color and gain. Plot windows can be moved and resized; the Commands window keeps the commands you send often."},
    {"Compare",
     "Records tests to compare them. Check the film button of a command in the Commands window: sending it starts a test. A test ends when its condition is met (Settled: the watched variable stays near the constant; or a custom condition), after the max test time, or when Auto stable sees it stopped changing. Tests are overlaid on up to 4 graphs; they can be saved as CSV files and opened again."},
    {"Chain",
     "Runs tests one after the other: each row's commands (separated by |) start a test, the next row is sent when it ends. Prepare commands bring the setup back before each test; Start when waits until a condition holds (ex: the temperature is back) before the next one."},
    {"Test bench",
     "Auto save writes each test that ends to the folder: CSV file and picture of the graphs. Report pictures are on white, at the format and resolution chosen, ready for a lab report. Notify shows a Windows notification when a test or a chain ends by itself."},
    {"Settings",
     "The gear at the top right: language, size of the interface, theme and colors. Profiles keep a whole setup (port, variables, plots, commands, tests) under a name, to switch between projects."},
  };
  lv_obj_t * header;

  for (uint32_t s = 0; s < (sizeof(sections) / sizeof(sections[0])); ++s)
  {
    lv_obj_t * box   = uiBoxCreate(content, tr(sections[s][0]), &header);
    lv_obj_t * label = lv_label_create(box);

    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(label, tr(sections[s][1]));
  }

  lv_obj_t * box = uiBoxCreate(content, tr("About"), &header);

  lv_label_set_text(lv_label_create(box), APP_NAME);
  lv_label_set_text(lv_label_create(box), tr(APP_AUTHOR));

  lv_obj_t * copyright = lv_label_create(box);

  lv_obj_set_width(copyright, LV_PCT(100));
  lv_label_set_long_mode(copyright, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(copyright, lv_color_hex(COLOR_INFO), 0);
  lv_label_set_text(copyright, APP_COPYRIGHT);
}

// The tabview picks the tab from the scroll position when a scroll ends. After a resize (ex: UI
// scale change) the position is the old tab width x index, which can round to the previous tab:
// that handler is blocked (tabs only change with the tab buttons) and the content is scrolled
// back to the active tab instead.
static void tabContentEvent(lv_event_t * e)
{
  if (lv_event_get_code(e) == LV_EVENT_SCROLL_END)
  {
    lv_event_stop_processing(e);
  }
  else
  {
    lv_tabview_set_active(tabview, lv_tabview_get_tab_active(tabview), LV_ANIM_OFF);
  }
}

static void settingsPanelBtnEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  settingsPanelSet(lv_obj_is_hidden(settingsPanel));
}

// Kept for the selected connection (shown again when it is selected again)
static void portSelectedEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (portCount > 0)
  {
    snprintf(conns[connSel].selectedPort, SERIAL_PORT_NAME_LEN, "%s",
             portList[lv_dropdown_get_selected(portDropdown)].name);
  }

  portInfoUpdate();
}

static void refreshBtnEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  portsRefresh();
  statusUpdate();
}

static void baudPresetEvent(lv_event_t * e)
{
  char preset[BAUD_MAX_DIGITS + 1];

  LV_UNUSED(e);
  lv_dropdown_get_selected_str(baudPresetDropdown, preset, sizeof(preset));
  lv_textarea_set_text(baudInput, preset);
}

static void connectBtnEvent(lv_event_t * e)
{
  Conn_t * sel = &conns[connSel];

  LV_UNUSED(e);

  // A manual disconnection / cancel stops the auto reconnection
  if (serialIsOpen(&sel->port) || sel->reconnectPending)
  {
    connClose(connSel);
    portsRefresh();
    statusUpdate();
    return;
  }

  if (portCount == 0)
  {
    return;
  }

  const char *   name = portList[lv_dropdown_get_selected(portDropdown)].name;
  int32_t        user = connUsingPort(name, connSel);
  SerialConfig_t config;

  if (user >= 0)
  {
    char msg[64];
    snprintf(msg, sizeof(msg), tr("%s is already used by C%ld."), name, (long) (user + 1));
    portErrorShow(msg);
    return;
  }

  if (!connConfigGet(connSel, &config, true))
  {
    return;
  }

  if (portConnect(connSel, name, &config))
  {
    lv_group_focus_obj(txInput);
  }
  else
  {
    char msg[96];
    snprintf(msg, sizeof(msg), tr("Cannot open %s.\nIs it used by another program?"), name);
    portErrorShow(msg);
  }

  statusUpdate();
}

// Separator / number of variables: a new split of the lines would mix with the plotted data
static void lineFormatEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  if (!connLoading)
  {
    lineFormatApply(connSel);
    plotterClear();
  }
}

// Setting of the selected connection changed: saved under its key (user data: setting name)
static void connSettingEvent(lv_event_t * e)
{
  lv_obj_t *   obj  = lv_event_get_target_obj(e);
  const char * name = (const char *) lv_event_get_user_data(e);

  if (connLoading)
  {
    return;
  }

  if (obj == baudInput)
  {
    settingsSetString(connKey(connSel, name), lv_textarea_get_text(obj));
  }
  else if (obj == autoReconnectSwitch)
  {
    settingsSetInt(connKey(connSel, name), lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
  }
  else
  {
    settingsSetInt(connKey(connSel, name), (int32_t) lv_dropdown_get_selected(obj));
  }
}

// Another connection shown in the Serial tab
static void connSelEvent(lv_event_t * e)
{
  uint32_t selected = lv_buttonmatrix_get_selected_button(connSelBtnm);

  LV_UNUSED(e);

  if (selected < connCount)
  {
    connSel = selected;
  }

  // Also when the selected button was clicked again (unchecked): checked again
  connWidgetsLoad();
}

// Number of connections: the ones above are closed, the variables are split again (the plotted
// data is cleared)
static void connCountEvent(lv_event_t * e)
{
  uint32_t count = lv_dropdown_get_selected(connCountDropdown) + 1;

  LV_UNUSED(e);

  for (uint32_t c = count; c < connCount; ++c)
  {
    connClose(c);
  }

  // Lines still waiting for their end are shown before the terminal changes mode
  for (uint32_t c = 0; c < LV_MIN(count, connCount); ++c)
  {
    if (conns[c].rxLineLen > 0)
    {
      connLineFlush(c);
    }
  }

  connCount = count;
  connSel   = LV_MIN(connSel, count - 1);
  settingsSetInt("connCount", (int32_t) count);

  plotterSetConnCount(count);
  cmdPanelSetConnCount(count);
  compareSetConnCount(count);
  connWidgetsLoad();
}

static void sendEvent(lv_event_t * e)
{
  LV_UNUSED(e);

  // Typed command: not one of the panel's (no color of its own)
  if (sendLine(connSel, lv_textarea_get_text(txInput), COLOR_INFO))
  {
    lv_textarea_set_text(txInput, "");
  }
}

// Sends text and the connection's line ending, shown in the terminal ("> text"); also used by the
// commands panel. markColor: of its mark on the plots.
static bool sendLine(uint32_t conn, const char * text, uint32_t markColor)
{
  char msg[32];

  if (text[0] == '\0')
  {
    return false;
  }

  if (conn >= connCount)
  {
    snprintf(msg, sizeof(msg), tr("[C%lu is not used]\n"), (unsigned long) (conn + 1));
    logAppend(msg);
    return false;
  }

  SerialPort_t * port    = &conns[conn].port;
  const char *   lineEnd = lineEndings[connIndexGet(conn, "lineEnding", LINE_ENDING_CRLF,
                                                    sizeof(lineEndings) / sizeof(lineEndings[0]))];

  if (!serialIsOpen(port))
  {
    connLog(conn, tr("[Not connected]\n"));
    return false;
  }

  if (!serialSendString(port, text) || ((lineEnd[0] != '\0') && !serialSendString(port, lineEnd)))
  {
    connLog(conn, tr("[Send failed]\n"));
    return false;
  }

  connLog(conn, "> ");
  logAppend(text);
  logAppend("\n");
  plotterSent(conn, text, markColor);
  return true;
}

// A test starts once its command is sent (t = 0)
static void commandSend(uint32_t conn, const char * text, uint32_t color, bool startsTest)
{
  if (sendLine(conn, text, color) && startsTest)
  {
    compareTestStart(text, conn);
  }
}

static void rightClick(lv_point_t point)
{
  if (lv_tabview_get_tab_active(tabview) == TAB_COMPARE)
  {
    compareRightClick(point);
  }
  else
  {
    plotterRightClick(point);
  }
}

static void clearBtnEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  logLen = 0;
  logText[0] = '\0';
  lv_label_set_text_static(lv_textarea_get_label(rxLog), logText);
}

// The choice is saved (settingsBindDropdown); the note and its restart button show while it
// differs from the UI's language
static void languageEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_hidden(languageNoteRow, lv_dropdown_get_selected(languageDropdown) == (uint32_t) langGet());
}

// Settings written now, ports closed (the new instance opens them again), then started again in
// the same window place
static void restartEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  settingsFlush();

  for (uint32_t c = 0; c < CONN_MAX; ++c)
  {
    if (serialIsOpen(&conns[c].port))
    {
      serialClose(&conns[c].port);
    }
  }

  appWindowRestart();
}

// The choice is saved (settingsBindDropdown); the note shows while it differs from the UI's scale
static void uiScaleEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_obj_set_hidden(scaleNoteLabel, uiScales[lv_dropdown_get_selected(uiScaleDropdown)] == uiScaleGet());
}

// The screen refresh and the plot redraw follow the same rate
static void maxFpsEvent(lv_event_t * e)
{
  uint32_t fps = maxFpsValues[lv_dropdown_get_selected(maxFpsDropdown)];

  LV_UNUSED(e);
  appWindowSetMaxFps(fps);
  plotterSetRenderPeriod(1000 / fps);
}

// Also the window: title bar in the accent color (white title), icon with an accent square and
// a white trace (light) or a dark square with an accent trace (dark)
static void themeEvent(lv_event_t * e)
{
  bool       dark   = lv_dropdown_get_selected(themeDropdown) == THEME_DARK;
  lv_color_t accent = lv_palette_main(accentPalettes[lv_dropdown_get_selected(accentDropdown)]);
  uint32_t   accentRgb = lv_color_to_u32(accent) & 0xFFFFFF;

  LV_UNUSED(e);
  uiThemeApply(dark, accent);
  appWindowSetIconColors(dark ? COLOR_BAR_BG : accentRgb, dark ? accentRgb : 0xFFFFFF);
  appWindowSetTitleBarColors(accentRgb, 0xFFFFFF, dark);
}

// The selected profile's name goes in the name box: Save then updates that profile
static void profileSelectedEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_textarea_set_text(profileNameInput, profileSelected());
  profileDeletePending[0] = '\0';
  profileNoteSet(NULL);
}

static void profileSaveEvent(lv_event_t * e)
{
  char name[SETTINGS_PROFILE_NAME_LEN];
  char text[SETTINGS_PROFILE_NAME_LEN + 64];

  LV_UNUSED(e);
  snprintf(name, sizeof(name), "%s", lv_textarea_get_text(profileNameInput));

  if (!settingsProfileNameValid(name))
  {
    profileNoteSet(tr("Enter a name (letters, digits, space, - _ .), without a space at the start or end."));
    return;
  }

  if (!settingsProfileSave(name))
  {
    snprintf(text, sizeof(text), tr("Could not save \"%s\"."), name);
    profileNoteSet(text);
    return;
  }

  profilesRefresh(name);
  snprintf(text, sizeof(text), tr("Saved \"%s\"."), name);
  profileNoteSet(text);
}

// The profile replaces the project settings, and the app restarts to show them
static void profileLoadEvent(lv_event_t * e)
{
  char name[SETTINGS_PROFILE_NAME_LEN];
  char text[SETTINGS_PROFILE_NAME_LEN + 64];

  LV_UNUSED(e);
  snprintf(name, sizeof(name), "%s", profileSelected());

  if (!settingsProfileLoad(name))
  {
    snprintf(text, sizeof(text), tr("Could not load \"%s\"."), name);
    profileNoteSet(text);
    return;
  }

  portsCloseAll();
  appWindowRestart();

  snprintf(text, sizeof(text), tr("\"%s\" loaded: close and start the app again to use it."), name);
  profileNoteSet(text);
}

// Closed first, so a new instance (restart) can open the ports again
static void portsCloseAll(void)
{
  for (uint32_t c = 0; c < CONN_MAX; ++c)
  {
    if (serialIsOpen(&conns[c].port))
    {
      serialClose(&conns[c].port);
    }
  }
}

static void fileExportEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  fileDialogStart(true);
}

static void fileImportEvent(lv_event_t * e)
{
  LV_UNUSED(e);
  fileDialogStart(false);
}

static void fileDialogStart(bool save)
{
  fileDialogSave = save;

  // Default name: what it holds
  snprintf(fileDialogPath, sizeof(fileDialogPath), "%s",
           !save ? "" : ((lv_dropdown_get_selected(fileKindDropdown) == SETTINGS_FILE_LAYOUT) ?
                         "serial_dashboard_layout.ini" : "serial_dashboard_setup.ini"));

  if (platformThreadStart(fileDialogThread, NULL))
  {
    lv_obj_set_state(fileExportBtn, LV_STATE_DISABLED, true);
    lv_obj_set_state(fileImportBtn, LV_STATE_DISABLED, true);
    lv_timer_create(fileDialogPollTimer, 200, NULL);
  }
}

// One path: the settings file chosen to import
static void fileDialogPathSet(const char * path, void * context)
{
  LV_UNUSED(context);
  snprintf(fileDialogPath, sizeof(fileDialogPath), "%s", path);
}

static void fileDialogThread(void * arg)
{
  PlatformFilter_t filter = {tr("Settings"), "ini", tr("All files")};

  LV_UNUSED(arg);

  fileDialogChosen = fileDialogSave ? platformSaveDialog(tr("Export the settings"), &filter, fileDialogPath, sizeof(fileDialogPath))
                                    : platformOpenDialog(tr("Import settings"), &filter, false, fileDialogPathSet, NULL);

  atomic_store(&fileDialogDone, true);
}

// The dialog closed: the file is written / read here, in the LVGL thread
static void fileDialogPollTimer(lv_timer_t * tim)
{
  char               text[PLATFORM_PATH_LEN + 96];
  SettingsFileKind_t kind = (SettingsFileKind_t) lv_dropdown_get_selected(fileKindDropdown);
  const char *       path = fileDialogPath;

  if (!atomic_exchange(&fileDialogDone, false))
  {
    return;
  }

  lv_timer_delete(tim);
  lv_obj_set_state(fileExportBtn, LV_STATE_DISABLED, false);
  lv_obj_set_state(fileImportBtn, LV_STATE_DISABLED, false);

  if (!fileDialogChosen)
  {
    return;
  }

  if (fileDialogSave)
  {
    snprintf(text, sizeof(text), tr(settingsFileExport(path, kind) ? "Saved: %s" : "Could not write %s"), path);
    profileNoteSet(text);
    return;
  }

  if (!settingsFileImport(path, &kind))
  {
    snprintf(text, sizeof(text), tr("%s is not a setup or plot layout file exported by the app."), path);
    profileNoteSet(text);
    return;
  }

  portsCloseAll();
  appWindowRestart();

  profileNoteSet(tr("Imported: close and start the app again to use it."));
}

// Asks for a second click before deleting the file
static void profileDeleteEvent(lv_event_t * e)
{
  char name[SETTINGS_PROFILE_NAME_LEN];
  char text[SETTINGS_PROFILE_NAME_LEN + 64];

  LV_UNUSED(e);
  snprintf(name, sizeof(name), "%s", profileSelected());

  if (strcmp(profileDeletePending, name) != 0)
  {
    snprintf(profileDeletePending, sizeof(profileDeletePending), "%s", name);
    snprintf(text, sizeof(text), tr("Click " LV_SYMBOL_TRASH " again to delete \"%s\"."), name);
    profileNoteSet(text);
    return;
  }

  snprintf(text, sizeof(text), tr(settingsProfileDelete(name) ? "Deleted \"%s\"." : "Could not delete \"%s\"."), name);
  profilesRefresh("");
  lv_textarea_set_text(profileNameInput, profileSelected());
  profileNoteSet(text);
}

// Called by the serial module from an LVGL timer: LVGL objects can be updated directly
static void serialRx(SerialPort_t * port, const uint8_t * data, uint32_t len, uint32_t tick)
{
  // Up to 4 characters per byte in decimal mode ("255 ")
  char     text[(SERIAL_RX_BUFFER_SIZE * 4) + 1];
  uint32_t mode = lv_dropdown_get_selected(rxModeDropdown);
  uint32_t n = 0;
  uint32_t conn = (uint32_t) (uintptr_t) port->userData;

#ifdef SERIAL_DASHBOARD_CLI
  cliReceived(conn, data, len);
#endif
  plotterFeed(conn, data, len, tick);

  PROF_PUSH(PROF_TERMINAL);

  for (uint32_t i = 0; i < len; ++i)
  {
    uint8_t c = data[i];

    if (mode == RX_MODE_HEX)
    {
      n += (uint32_t) snprintf(&text[n], sizeof(text) - n, "%02X ", c);
    }
    else if (mode == RX_MODE_DEC)
    {
      n += (uint32_t) snprintf(&text[n], sizeof(text) - n, "%u ", (unsigned) c);
    }
    else if (c != '\r')
    {
      // Show printable ASCII only; other bytes become '.'
      text[n++] = ((c == '\n') || (c == '\t') || ((c >= 0x20) && (c < 0x7F))) ? (char) c : '.';
    }
  }

  text[n] = '\0';

  // Several connections: each line shown with its connection (text), each received chunk on its
  // own line (hex / dec)
  if (n == 0)
  {
  }
  else if (connCount == 1)
  {
    logQueue(text);
  }
  else if (mode == RX_MODE_TEXT)
  {
    connLineAdd(conn, text);
  }
  else
  {
    char line[sizeof(text) + 8];
    snprintf(line, sizeof(line), "[C%lu] %s\n", (unsigned long) (conn + 1), text);
    logQueue(line);
  }

  PROF_POP();
}

// The port was lost (ex: USB unplugged): with auto reconnect, wait for it to come back
static void serialDisconnect(SerialPort_t * port)
{
  char     msg[80];
  uint32_t index = (uint32_t) (uintptr_t) port->userData;
  Conn_t * conn  = &conns[index];

  if (connIndexGet(index, "autoReconnect", 0, 2) != 0)
  {
    snprintf(conn->reconnectName, sizeof(conn->reconnectName), "%s", port->name);
    conn->reconnectConfig  = port->config;
    conn->reconnectPending = true;
    snprintf(msg, sizeof(msg), tr("[%s disconnected, waiting to reconnect]\n"), port->name);
  }
  else
  {
    snprintf(msg, sizeof(msg), tr("[%s disconnected]\n"), port->name);
  }

  // Its partial line and its values held in the plots end here
  if (conn->rxLineLen > 0)
  {
    connLineFlush(index);
  }
  plotterResetLine(index);

  connLog(index, msg);
#ifdef SERIAL_DASHBOARD_CLI
  cliMessage(msg);
#endif
  portsRefresh();
  statusUpdate();
}

#ifdef SERIAL_DASHBOARD_CLI
bool screensCliConnect(uint32_t conn, const char * port, uint32_t baud, char * error, size_t size)
{
  SerialConfig_t config;
  int32_t        user = connUsingPort(port, conn);

  if (conn >= connCount)
  {
    snprintf(error, size, "C%lu is not used: %lu connection(s) (setting connCount)", (unsigned long) (conn + 1),
             (unsigned long) connCount);
    return false;
  }

  if (user >= 0)
  {
    snprintf(error, size, "%s is already used by C%ld", port, (long) (user + 1));
    return false;
  }

  if (baud > 0)
  {
    char text[16];

    snprintf(text, sizeof(text), "%lu", (unsigned long) baud);
    settingsSetString(connKey(conn, "baud"), text);
  }

  if (!connConfigGet(conn, &config, false))
  {
    snprintf(error, size, "The saved data format of C%lu is not valid", (unsigned long) (conn + 1));
    return false;
  }

  connClose(conn);

  if (!portConnect(conn, port, &config))
  {
    snprintf(error, size, "Could not open %s (missing, or used by another program such as the app)", port);
    return false;
  }

  portsRefresh();
  statusUpdate();
  return true;
}

void screensCliDisconnect(uint32_t conn)
{
  if (conn < CONN_MAX)
  {
    connClose(conn);
    portsRefresh();
    statusUpdate();
  }
}

bool screensCliSend(uint32_t conn, const char * text, bool startsTest)
{
  if (!sendLine(conn, text, COLOR_SELECTION))
  {
    return false;
  }

  if (startsTest)
  {
    compareTestStart(text, conn);
  }

  return true;
}

uint32_t screensCliConnCount(void)
{
  return connCount;
}

const char * screensCliPort(uint32_t conn, bool * open, uint32_t * baud)
{
  *open = serialIsOpen(&conns[conn].port);
  *baud = *open ? conns[conn].port.config.baudRate : 0;
  return *open ? conns[conn].port.name : "";
}
#endif
