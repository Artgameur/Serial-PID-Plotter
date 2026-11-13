// ------------------------------------------------------------------------------------------------
/*!@file
   @brief The session: the whole app (Serial, Plot and Compare tabs) created on a display that is
          never shown, with the user's settings read (never written), so tests, chains, stop
          conditions and the test bench work as in the app. It holds the serial ports between
          commands. Each command comes through the named pipe as its arguments, one per line,
          then an empty line; the answer is one JSON object.

          Received lines and the app's messages are kept in rings; read / messages return the
          new ones. wait / read --wait / send --wait answer later, from the main loop.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#include "cli.h"

#include <math.h>
#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "lvgl.h"

#include "compare.h"
#include "plot_export.h"
#include "plotter.h"
#include "screens.h"
#include "serial.h"
#include "settings.h"
#include "test_bench.h"
#include "test_data.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define PIPE_NAME             L"\\\\.\\pipe\\SerialDashboardCli"
#define PIPE_BUFFER           65536
#define REQUEST_LEN           16384
#define REQUEST_ARGS_MAX      64
#define CONNECT_TRIES_MS      5000
#define START_READY_MS        20000
#define START_POLL_MS         200

#define LINES_MAX             4096        // received lines kept
#define LINE_LEN              200
#define MESSAGES_MAX          512
#define MESSAGE_LEN           256
#define READ_MAX_DEFAULT      200         // lines returned by read at most (the latest)
#define WAIT_DEFAULT_S        120
#define WAIT_MAX_S            3600
#define WAIT_GRACE_MS         1500        // after a test: its CSV and picture are being written
#define LAYOUT_PERIOD_MS      250         // the screen is not drawn: its layout is updated this often
#define LOOP_SLEEP_MAX_MS     5
#define IDLE_DEFAULT_MIN      60

#define DISPLAY_WIDTH         1280
#define DISPLAY_HEIGHT        800
#define BUFFER_LINES          40
#define BYTES_PER_PIXEL       4

#define CONN_MAX              PLOTTER_MAX_CONNS
#define ERROR_LEN             320
#define SETTINGS_DIR          "\\SerialDashboard"
#define SETTINGS_FILE         "\\settings.ini"
#define PROFILES_DIR          "\\profiles\\"
#define LOG_DIR               L"SerialDashboardCli"
#define LOG_FILE              L"\\session.log"
#define UNIT_START            " ["        // "Temp [C]": the variable's name is before it

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  uint64_t seq;
  uint32_t conn;
  uint32_t ms;                            // since the session started
  char     text[LINE_LEN];
} Line_t;

typedef struct
{
  uint64_t seq;
  uint32_t ms;
  char     text[MESSAGE_LEN];
} Message_t;

typedef enum
{
  PENDING_NONE,
  PENDING_LINES,                          // read --wait, send --wait
  PENDING_WAIT,                           // wait: the test / chain ends
} PendingKind_t;

typedef struct
{
  PendingKind_t kind;
  uint32_t      startTick;
  uint32_t      timeoutMs;
  uint64_t      lineFrom;                 // lines after this one
  uint64_t      messageFrom;
  int32_t       conn;                     // -1: all
  uint32_t      max;
  char          until[LINE_LEN];
  uint32_t      idleSince;                // wait: not busy since (0: busy)
  bool          sent;                     // send --wait
} Pending_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static const char * const sessionCommands[] =
{
  "status", "stop", "connect", "disconnect", "send", "read", "messages", "test-start", "test-stop",
  "chain-set", "chain-run", "chain-stop", "wait", "tests", "tests-clear", "export", "picture", "set", "get",
};

// Lines and messages (rings), written by the main loop only
static Line_t    lines[LINES_MAX];
static uint64_t  lineSeq;                 // of the last one; 0: none yet
static uint64_t  lineCursor;              // last one returned by read
static char      partial[CONN_MAX][LINE_LEN];
static uint32_t  partialLen[CONN_MAX];
static Message_t messages[MESSAGES_MAX];
static uint64_t  messageSeq;
static uint64_t  messageCursor;
static uint32_t  startTick;
static bool      serving;

// Request from the pipe thread, answer from the main loop
static CRITICAL_SECTION requestLock;
static HANDLE           requestEvent;
static HANDLE           answerEvent;
static char             request[REQUEST_LEN];
static CliOut_t         answer;
static Pending_t        pending;
static bool             stopping;
static uint32_t         lastRequestTick;
static uint32_t         idleMinutes = IDLE_DEFAULT_MIN;
static char             settingsUsed[MAX_PATH * 2];

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static uint32_t     nowMs(void);
static uint32_t     tickGet(void);
static const char * optionGet(int argc, char ** argv, const char * name);
static bool         flagGet(int argc, char ** argv, const char * name);
static uint32_t     positionals(int argc, char ** argv, char ** out, uint32_t max);
static bool         connGet(int argc, char ** argv, uint32_t * conn, CliOut_t * out);
static void         errorOut(CliOut_t * out, const char * format, ...);
static void         eventPrint(const char * kind, const char * text);
static void         lineAdd(uint32_t conn, const char * text, uint32_t length);
static void         linesOut(CliOut_t * out, uint64_t from, int32_t conn, uint32_t max);
static bool         linesMatch(uint64_t from, int32_t conn, const char * text);
static void         messagesOut(CliOut_t * out, uint64_t from);
static void         testOut(CliOut_t * out, uint32_t i);
static void         testsOut(CliOut_t * out);
static void         statusOut(CliOut_t * out);
static const char * variableResolve(const char * name, char * error, size_t size);
static void         requestRun(char * text, CliOut_t * out);
static void         pendingCheck(void);
static unsigned __stdcall pipeThread(void * arg);
static void         sessionEnd(void);
static HANDLE       pipeOpen(void);
static bool         pipeExchange(int argc, char ** argv, CliOut_t * reply);
static void         argQuote(wchar_t * line, size_t size, const char * arg);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void cliFlushDiscard(lv_display_t * display, const lv_area_t * area, uint8_t * pixels)
{
  LV_UNUSED(area);
  LV_UNUSED(pixels);
  lv_display_flush_ready(display);
}

bool cliNameIs(const char * header, const char * name)
{
  const char * unit   = strstr(header, UNIT_START);
  size_t       base   = (unit != NULL) ? (size_t) (unit - header) : strlen(header);
  size_t       length = strlen(name);

  return (base == length) && (_strnicmp(header, name, length) == 0);
}

void cliOutf(CliOut_t * out, const char * format, ...)
{
  va_list args;

  for (;;)
  {
    size_t room = out->size - out->length;
    int    n;

    va_start(args, format);
    n = (out->text != NULL) ? vsnprintf(&out->text[out->length], room, format, args) : -1;
    va_end(args);

    if ((n >= 0) && ((size_t) n < room))
    {
      out->length += (size_t) n;
      return;
    }

    size_t size = LV_MAX(out->size * 2, out->length + ((n > 0) ? (size_t) n : 0) + 4096);
    char * text = realloc(out->text, size);

    if (text == NULL)
    {
      return;
    }

    out->text = text;
    out->size = size;
  }
}

void cliOutString(CliOut_t * out, const char * text)
{
  cliOutf(out, "\"");

  for (const unsigned char * c = (const unsigned char *) text; *c != '\0'; ++c)
  {
    if ((*c == '"') || (*c == '\\'))
    {
      cliOutf(out, "\\%c", *c);
    }
    else if (*c < 0x20)
    {
      cliOutf(out, "\\u%04x", *c);
    }
    else
    {
      cliOutf(out, "%c", *c);
    }
  }

  cliOutf(out, "\"");
}

void cliOutNumber(CliOut_t * out, double value)
{
  if (isnan(value) || isinf(value))
  {
    cliOutf(out, "null");
  }
  else
  {
    cliOutf(out, "%.6g", value);
  }
}

bool cliIsSessionCommand(const char * name)
{
  for (uint32_t c = 0; c < (sizeof(sessionCommands) / sizeof(sessionCommands[0])); ++c)
  {
    if (strcmp(name, sessionCommands[c]) == 0)
    {
      return true;
    }
  }

  return false;
}

int cliPorts(void)
{
  SerialPortInfo_t ports[SERIAL_MAX_PORTS];
  uint32_t         count = serialListPorts(ports, SERIAL_MAX_PORTS);
  CliOut_t         out = {0};

  cliOutf(&out, "{\"ok\":true,\"ports\":[");

  for (uint32_t p = 0; p < count; ++p)
  {
    cliOutf(&out, (p > 0) ? ",{\"name\":" : "{\"name\":");
    cliOutString(&out, ports[p].name);
    cliOutf(&out, ",\"description\":");
    cliOutString(&out, ports[p].description);
    cliOutf(&out, "}");
  }

  cliOutf(&out, "]}\n");
  fputs(out.text, stdout);
  return CLI_EXIT_OK;
}

// Implemented for the app's sources (plotter.h): received bytes, by whole lines
void cliReceived(uint32_t conn, const uint8_t * data, uint32_t len)
{
  if (conn >= CONN_MAX)
  {
    return;
  }

  for (uint32_t i = 0; i < len; ++i)
  {
    char c = (char) data[i];

    if (c == '\n')
    {
      lineAdd(conn, partial[conn], partialLen[conn]);
      partialLen[conn] = 0;
    }
    else if (c != '\r')
    {
      partial[conn][partialLen[conn]++] = c;

      if (partialLen[conn] >= (LINE_LEN - 1))
      {
        lineAdd(conn, partial[conn], partialLen[conn]);
        partialLen[conn] = 0;
      }
    }
  }
}

// The app's messages (toasts) and connection events
void cliMessage(const char * text)
{
  Message_t * message = &messages[(messageSeq + 1) % MESSAGES_MAX];
  size_t      length  = strcspn(text, "\n");

  message->seq = ++messageSeq;
  message->ms  = nowMs();
  snprintf(message->text, sizeof(message->text), "%.*s", (int) length, text);
  eventPrint("message", message->text);
}

int cliServe(int argc, char ** argv)
{
  static uint8_t buffer[DISPLAY_WIDTH * BUFFER_LINES * BYTES_PER_PIXEL];
  const char *   settingsPath = optionGet(argc, argv, "--settings");
  const char *   profile      = optionGet(argc, argv, "--profile");
  const char *   port         = optionGet(argc, argv, "--connect");
  const char *   baud         = optionGet(argc, argv, "--baud");
  const char *   idle         = optionGet(argc, argv, "--idle");
  const char *   appData      = getenv("APPDATA");
  char           path[MAX_PATH * 2];
  CliOut_t       out = {0};

  HANDLE pipe = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 1, PIPE_BUFFER, PIPE_BUFFER, 0, NULL);

  if (pipe == INVALID_HANDLE_VALUE)
  {
    eventPrint("error", "A session is already running: stop it first (serial_dashboard_cli stop).");
    return CLI_EXIT_USAGE;
  }

  if (idle != NULL)
  {
    idleMinutes = (uint32_t) strtoul(idle, NULL, 10);
  }

  startTick = GetTickCount();
  lv_init();
  lv_tick_set_cb(tickGet);

  lv_display_t * display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);

  lv_display_set_flush_cb(display, cliFlushDiscard);
  lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

  // The user's settings, read only: settingsInit() (which saves them) is not called
  if (flagGet(argc, argv, "--clean"))
  {
    snprintf(settingsUsed, sizeof(settingsUsed), "(none: --clean)");
  }
  else
  {
    if (settingsPath != NULL)
    {
      snprintf(path, sizeof(path), "%s", settingsPath);
    }
    else
    {
      snprintf(path, sizeof(path), "%s%s%s", (appData != NULL) ? appData : ".", SETTINGS_DIR, SETTINGS_FILE);
    }

    settingsCliLoad(path);
    snprintf(settingsUsed, sizeof(settingsUsed), "%s", path);

    if (profile != NULL)
    {
      snprintf(path, sizeof(path), "%s%s%s%s.ini", (appData != NULL) ? appData : ".", SETTINGS_DIR, PROFILES_DIR, profile);

      if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
      {
        snprintf(path, sizeof(path), "No profile named %s.", profile);
        eventPrint("error", path);
        return CLI_EXIT_USAGE;
      }

      settingsCliLoad(path);
      snprintf(&settingsUsed[strlen(settingsUsed)], sizeof(settingsUsed) - strlen(settingsUsed), " + %s", path);
    }
  }

  // Messages in English; the session opens only the ports it is told to
  settingsSetString("language", "0");

  for (uint32_t c = 0; c < CONN_MAX; ++c)
  {
    char key[24];

    snprintf(key, sizeof(key), (c == 0) ? "autoReconnect" : "c%lu.autoReconnect", (unsigned long) (c + 1));
    settingsSetString(key, "0");
  }

  for (int a = 0; a < argc; ++a)
  {
    if ((strcmp(argv[a], "--set") == 0) && ((a + 1) < argc))
    {
      char * equal = strchr(argv[++a], '=');

      if (equal == NULL)
      {
        eventPrint("error", "--set needs KEY=VALUE.");
        return CLI_EXIT_USAGE;
      }

      *equal = '\0';
      settingsSetString(argv[a], equal + 1);
    }
  }

  lv_lock();
  loadScreen();

  // Not drawn (nothing shows it): only laid out, below; pictures draw themselves
  lv_timer_pause(lv_display_get_refr_timer(display));
  lv_obj_update_layout(lv_screen_active());
  lv_unlock();

  if (port != NULL)
  {
    char error[ERROR_LEN];

    if (!screensCliConnect(0, port, (baud != NULL) ? (uint32_t) strtoul(baud, NULL, 10) : 0, error, sizeof(error)))
    {
      cliMessage(error);
    }
  }

  InitializeCriticalSection(&requestLock);
  requestEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
  answerEvent  = CreateEventW(NULL, FALSE, FALSE, NULL);
  serving      = true;
  lastRequestTick = GetTickCount();

  uintptr_t thread = _beginthreadex(NULL, 0, pipeThread, pipe, 0, NULL);

  if (thread == 0)
  {
    eventPrint("error", "Could not start the session.");
    return CLI_EXIT_FAILED;
  }

  CloseHandle((HANDLE) thread);

  cliOutf(&out, "settings: %s", settingsUsed);
  eventPrint("ready", out.text);
  free(out.text);

  uint32_t layoutTick = GetTickCount();

  for (;;)
  {
    uint32_t next = lv_timer_handler();

    if (lv_tick_elaps(layoutTick) >= LAYOUT_PERIOD_MS)
    {
      layoutTick = lv_tick_get();
      lv_obj_update_layout(lv_screen_active());
    }

    if ((pending.kind == PENDING_NONE) && (WaitForSingleObject(requestEvent, 0) == WAIT_OBJECT_0))
    {
      EnterCriticalSection(&requestLock);
      answer.length = 0;
      requestRun(request, &answer);
      LeaveCriticalSection(&requestLock);

      if (pending.kind == PENDING_NONE)
      {
        SetEvent(answerEvent);
      }
    }

    pendingCheck();

    if (stopping && (pending.kind == PENDING_NONE))
    {
      // The answer is written by the pipe thread first
      Sleep(200);
      sessionEnd();
    }

    if ((idleMinutes > 0) && !compareCliBusy() && (pending.kind == PENDING_NONE) &&
        ((GetTickCount() - lastRequestTick) >= (idleMinutes * 60000u)))
    {
      cliMessage("No command for the idle time: the session ends.");
      sessionEnd();
    }

    Sleep(LV_MIN(next, LOOP_SLEEP_MAX_MS));
  }
}

int cliStart(int argc, char ** argv)
{
  wchar_t             exe[MAX_PATH];
  wchar_t             line[8192];
  wchar_t             logPath[MAX_PATH];
  wchar_t             temp[MAX_PATH];
  CliOut_t            reply = {0};
  char *              ping[] = {"status"};
  STARTUPINFOW        startup = {.cb = sizeof(startup)};
  PROCESS_INFORMATION process;
  SECURITY_ATTRIBUTES inherit = {.nLength = sizeof(inherit), .bInheritHandle = TRUE};

  if (pipeExchange(1, ping, &reply))
  {
    printf("{\"ok\":false,\"error\":\"A session is already running. Use it, or stop it first (serial_dashboard_cli stop).\"}\n");
    return CLI_EXIT_FAILED;
  }

  GetModuleFileNameW(NULL, exe, MAX_PATH);
  GetTempPathW(MAX_PATH, temp);
  _snwprintf(logPath, MAX_PATH, L"%ls%ls", temp, LOG_DIR);
  CreateDirectoryW(logPath, NULL);
  wcsncat(logPath, LOG_FILE, MAX_PATH - wcslen(logPath) - 1);

  HANDLE log = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);

  _snwprintf(line, sizeof(line) / sizeof(line[0]), L"\"%ls\" serve", exe);

  for (int a = 1; a < argc; ++a)
  {
    argQuote(line, sizeof(line) / sizeof(line[0]), argv[a]);
  }

  // Only the log is inherited: with the caller's handles (ex: the pipe a shell reads this
  // command's output from), the caller would wait for the session to end
  STARTUPINFOEXW                extended = {0};
  SIZE_T                        listSize = 0;
  LPPROC_THREAD_ATTRIBUTE_LIST  attributes;

  InitializeProcThreadAttributeList(NULL, 1, 0, &listSize);
  attributes = malloc(listSize);

  if ((log == INVALID_HANDLE_VALUE) || (attributes == NULL) || !InitializeProcThreadAttributeList(attributes, 1, 0, &listSize) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof(log), NULL, NULL))
  {
    printf("{\"ok\":false,\"error\":\"Could not start the session (its log file).\"}\n");
    return CLI_EXIT_FAILED;
  }

  startup.dwFlags    = STARTF_USESTDHANDLES;
  startup.hStdInput  = NULL;
  startup.hStdOutput = log;
  startup.hStdError  = log;
  extended.StartupInfo         = startup;
  extended.StartupInfo.cb      = sizeof(extended);
  extended.lpAttributeList     = attributes;

  if (!CreateProcessW(exe, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                      NULL, NULL, &extended.StartupInfo, &process))
  {
    printf("{\"ok\":false,\"error\":\"Could not start the session.\"}\n");
    return CLI_EXIT_FAILED;
  }

  DeleteProcThreadAttributeList(attributes);
  free(attributes);
  CloseHandle(log);
  CloseHandle(process.hThread);

  char logUtf8[MAX_PATH * 3];

  WideCharToMultiByte(CP_UTF8, 0, logPath, -1, logUtf8, sizeof(logUtf8), NULL, NULL);

  for (uint32_t waited = 0; waited < START_READY_MS; waited += START_POLL_MS)
  {
    if (WaitForSingleObject(process.hProcess, START_POLL_MS) == WAIT_OBJECT_0)
    {
      char  text[2048] = "";
      FILE * file = _wfopen(logPath, L"rb");

      if (file != NULL)
      {
        text[fread(text, 1, sizeof(text) - 1, file)] = '\0';
        fclose(file);
      }

      CliOut_t out = {0};

      cliOutf(&out, "{\"ok\":false,\"error\":\"The session stopped at once.\",\"log\":");
      cliOutString(&out, text);
      cliOutf(&out, "}\n");
      fputs(out.text, stdout);
      return CLI_EXIT_FAILED;
    }

    reply.length = 0;

    if (pipeExchange(1, ping, &reply))
    {
      // The status, with where the session's own log is
      if ((reply.length > 0) && (reply.text[reply.length - 1] == '\n'))
      {
        reply.text[--reply.length] = '\0';
      }

      if ((reply.length > 0) && (reply.text[reply.length - 1] == '}'))
      {
        reply.text[--reply.length] = '\0';
      }

      CliOut_t out = {0};

      cliOutf(&out, "%s,\"pid\":%lu,\"log\":", reply.text, (unsigned long) process.dwProcessId);
      cliOutString(&out, logUtf8);
      cliOutf(&out, "}\n");
      fputs(out.text, stdout);
      CloseHandle(process.hProcess);
      return CLI_EXIT_OK;
    }
  }

  CloseHandle(process.hProcess);
  printf("{\"ok\":false,\"error\":\"The session did not answer in time.\"}\n");
  return CLI_EXIT_FAILED;
}

int cliClient(int argc, char ** argv)
{
  CliOut_t reply = {0};

  for (int a = 0; a < argc; ++a)
  {
    if (strpbrk(argv[a], "\r\n") != NULL)
    {
      printf("{\"ok\":false,\"error\":\"Arguments cannot contain line breaks.\"}\n");
      return CLI_EXIT_USAGE;
    }
  }

  if (!pipeExchange(argc, argv, &reply))
  {
    printf("{\"ok\":false,\"error\":\"No session running. Start one: serial_dashboard_cli start --connect COM5\"}\n");
    return CLI_EXIT_NO_SESSION;
  }

  fputs(reply.text, stdout);
  return (strncmp(reply.text, "{\"ok\":true", 10) == 0) ? CLI_EXIT_OK : CLI_EXIT_FAILED;
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

static uint32_t nowMs(void)
{
  return GetTickCount() - startTick;
}

static uint32_t tickGet(void)
{
  return GetTickCount();
}


static const char * optionGet(int argc, char ** argv, const char * name)
{
  for (int a = 0; (a + 1) < argc; ++a)
  {
    if (strcmp(argv[a], name) == 0)
    {
      return argv[a + 1];
    }
  }

  return NULL;
}

static bool flagGet(int argc, char ** argv, const char * name)
{
  for (int a = 0; a < argc; ++a)
  {
    if (strcmp(argv[a], name) == 0)
    {
      return true;
    }
  }

  return false;
}

// Arguments that are not options (after the command); flags without value: --test, --all
static uint32_t positionals(int argc, char ** argv, char ** out, uint32_t max)
{
  uint32_t count = 0;

  for (int a = 1; a < argc; ++a)
  {
    if (strncmp(argv[a], "--", 2) == 0)
    {
      a += ((strcmp(argv[a], "--test") != 0) && (strcmp(argv[a], "--all") != 0)) ? 1 : 0;
    }
    else if (count < max)
    {
      out[count++] = argv[a];
    }
  }

  return count;
}

// --conn N (1 = C1); C1 without it
static bool connGet(int argc, char ** argv, uint32_t * conn, CliOut_t * out)
{
  const char * text = optionGet(argc, argv, "--conn");

  *conn = (text != NULL) ? (uint32_t) (strtoul(text, NULL, 10) - 1) : 0;

  if (*conn >= screensCliConnCount())
  {
    errorOut(out, "--conn: 1 to %lu (setting connCount).", (unsigned long) screensCliConnCount());
    return false;
  }

  return true;
}

static void errorOut(CliOut_t * out, const char * format, ...)
{
  char    text[ERROR_LEN * 2];
  va_list args;

  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);

  out->length = 0;
  cliOutf(out, "{\"ok\":false,\"error\":");
  cliOutString(out, text);
  cliOutf(out, "}\n");
}

// The session's own output (its log): one JSON object per line
static void eventPrint(const char * kind, const char * text)
{
  CliOut_t out = {0};

  cliOutf(&out, "{\"event\":\"%s\",\"ms\":%lu,\"text\":", kind, (unsigned long) nowMs());
  cliOutString(&out, text);
  cliOutf(&out, "}\n");
  fputs(out.text, stdout);
  fflush(stdout);
  free(out.text);
}

static void lineAdd(uint32_t conn, const char * text, uint32_t length)
{
  Line_t * line = &lines[(lineSeq + 1) % LINES_MAX];

  line->seq  = ++lineSeq;
  line->conn = conn;
  line->ms   = nowMs();
  snprintf(line->text, sizeof(line->text), "%.*s", (int) length, text);
}

// Lines after from (of conn, -1: all), the last max of them
static void linesOut(CliOut_t * out, uint64_t from, int32_t conn, uint32_t max)
{
  uint64_t oldest = (lineSeq > LINES_MAX) ? (lineSeq - LINES_MAX + 1) : 1;
  uint64_t first  = LV_MAX(from + 1, oldest);
  uint32_t count  = 0;
  bool     comma  = false;

  for (uint64_t s = first; s <= lineSeq; ++s)
  {
    count += ((conn < 0) || (lines[s % LINES_MAX].conn == (uint32_t) conn)) ? 1 : 0;
  }

  uint32_t skip = (count > max) ? (count - max) : 0;

  cliOutf(out, "\"lines\":[");

  for (uint64_t s = first; s <= lineSeq; ++s)
  {
    const Line_t * line = &lines[s % LINES_MAX];

    if ((conn >= 0) && (line->conn != (uint32_t) conn))
    {
      continue;
    }

    if (skip > 0)
    {
      skip--;
      continue;
    }

    cliOutf(out, "%s{\"conn\":%lu,\"ms\":%lu,\"text\":", comma ? "," : "", (unsigned long) (line->conn + 1),
            (unsigned long) line->ms);
    cliOutString(out, line->text);
    cliOutf(out, "}");
    comma = true;
  }

  cliOutf(out, "],\"lines_left_out\":%lu,\"lines_lost\":%lu", (unsigned long) ((count > max) ? (count - max) : 0),
          (unsigned long) ((first > (from + 1)) ? (first - from - 1) : 0));
}

static bool linesMatch(uint64_t from, int32_t conn, const char * text)
{
  uint64_t oldest = (lineSeq > LINES_MAX) ? (lineSeq - LINES_MAX + 1) : 1;

  for (uint64_t s = LV_MAX(from + 1, oldest); s <= lineSeq; ++s)
  {
    const Line_t * line = &lines[s % LINES_MAX];

    if (((conn < 0) || (line->conn == (uint32_t) conn)) && (strstr(line->text, text) != NULL))
    {
      return true;
    }
  }

  return false;
}

static void messagesOut(CliOut_t * out, uint64_t from)
{
  uint64_t oldest = (messageSeq > MESSAGES_MAX) ? (messageSeq - MESSAGES_MAX + 1) : 1;

  cliOutf(out, "\"messages\":[");

  for (uint64_t s = LV_MAX(from + 1, oldest); s <= messageSeq; ++s)
  {
    const Message_t * message = &messages[s % MESSAGES_MAX];

    cliOutf(out, "%s{\"ms\":%lu,\"text\":", (s > LV_MAX(from + 1, oldest)) ? "," : "", (unsigned long) message->ms);
    cliOutString(out, message->text);
    cliOutf(out, "}");
  }

  cliOutf(out, "]");
}

static void testOut(CliOut_t * out, uint32_t i)
{
  CompareCliTest_t   test;
  const TestData_t * data;

  compareCliTestGet(i, &test);
  data = test.data;

  cliOutf(out, "{\"test\":%lu,\"name\":", (unsigned long) (i + 1));
  cliOutString(out, data->name);
  cliOutf(out, ",\"end\":\"%s\",\"samples\":%lu,\"duration_s\":", test.end, (unsigned long) data->sampleCount);
  cliOutNumber(out, (data->sampleCount > 0) ? (data->times[data->sampleCount - 1] / 1000.0) : 0.0);

  if ((strcmp(test.end, "condition") == 0) || (strcmp(test.end, "plateau") == 0))
  {
    cliOutf(out, ",\"reached_s\":");
    cliOutNumber(out, test.settledS);
  }

  if (strcmp(test.end, "plateau") == 0)
  {
    cliOutf(out, ",\"plateau_value\":");
    cliOutNumber(out, test.plateauValue);
  }

  cliOutf(out, ",\"settings\":");
  cliOutString(out, data->settings);
  cliOutf(out, ",\"commands\":");
  cliOutString(out, data->commands);
  cliOutf(out, ",\"variables\":[");

  for (uint32_t c = 0; c < data->columnCount; ++c)
  {
    cliOutf(out, (c > 0) ? "," : "");
    cliOutString(out, data->headers[c]);
  }

  cliOutf(out, "]}");
}

static void testsOut(CliOut_t * out)
{
  cliOutf(out, "\"tests\":[");

  for (uint32_t i = 0; i < compareCliTestCount(); ++i)
  {
    cliOutf(out, (i > 0) ? "," : "");
    testOut(out, i);
  }

  cliOutf(out, "]");
}

static void statusOut(CliOut_t * out)
{
  char     headers[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  float    values[PLOTTER_MAX_VARS];
  uint32_t count = plotterCliLatest(headers, values);
  uint32_t steps;
  bool     waiting;
  int32_t  step = compareCliChainStep(&steps, &waiting);
  int32_t  recording = compareCliRecordingIndex();

  cliOutf(out, "{\"ok\":true,\"settings_file\":");
  cliOutString(out, settingsUsed);
  cliOutf(out, ",\"connections\":[");

  for (uint32_t c = 0; c < screensCliConnCount(); ++c)
  {
    bool         open;
    uint32_t     baud;
    const char * port = screensCliPort(c, &open, &baud);

    cliOutf(out, "%s{\"conn\":%lu,\"open\":%s,\"port\":", (c > 0) ? "," : "", (unsigned long) (c + 1), open ? "true" : "false");
    cliOutString(out, port);
    cliOutf(out, ",\"baud\":%lu}", (unsigned long) baud);
  }

  cliOutf(out, "],\"variables\":[");

  for (uint32_t v = 0; v < count; ++v)
  {
    cliOutf(out, "%s{\"name\":", (v > 0) ? "," : "");
    cliOutString(out, headers[v]);
    cliOutf(out, ",\"value\":");
    cliOutNumber(out, values[v]);
    cliOutf(out, "}");
  }

  cliOutf(out, "],\"busy\":%s,\"recording\":", compareCliBusy() ? "true" : "false");

  if (recording >= 0)
  {
    testOut(out, (uint32_t) recording);
  }
  else
  {
    cliOutf(out, "null");
  }

  cliOutf(out, ",\"chain\":{\"running\":%s,\"step\":%ld,\"waiting_for_start\":%s,\"steps\":[", (step >= 0) ? "true" : "false",
          (long) (step + 1), waiting ? "true" : "false");

  const char * text;
  uint32_t     conn;
  uint32_t     total = compareCliChainGet(0, &text, &conn);

  for (uint32_t i = 0; i < total; ++i)
  {
    compareCliChainGet(i, &text, &conn);
    cliOutf(out, "%s{\"conn\":%lu,\"commands\":", (i > 0) ? "," : "", (unsigned long) (conn + 1));
    cliOutString(out, text);
    cliOutf(out, "}");
  }

  cliOutf(out, "]},\"tests\":%lu,\"new_lines\":%lu,\"new_messages\":%lu}\n", (unsigned long) compareCliTestCount(),
          (unsigned long) (lineSeq - lineCursor), (unsigned long) (messageSeq - messageCursor));
}

// Full column name of name ("Temp" or "Temp [°C]"), among the tests' and received variables
static const char * variableResolve(const char * name, char * error, size_t size)
{
  static char found[TEST_HEADER_LEN];
  char        headers[PLOTTER_MAX_VARS][TEST_HEADER_LEN];
  float       values[PLOTTER_MAX_VARS];
  uint32_t    count = plotterCliLatest(headers, values);

  found[0] = '\0';

  for (int32_t pass = 0; pass < 2; ++pass)
  {
    for (uint32_t i = 0; i <= compareCliTestCount(); ++i)
    {
      const char (*list)[TEST_HEADER_LEN] = (const char (*)[TEST_HEADER_LEN]) headers;
      uint32_t   listCount = count;

      if (i < compareCliTestCount())
      {
        CompareCliTest_t test;

        compareCliTestGet(i, &test);
        list      = (const char (*)[TEST_HEADER_LEN]) test.data->headers;
        listCount = test.data->columnCount;
      }

      for (uint32_t c = 0; c < listCount; ++c)
      {
        if ((pass == 0) ? (strcmp(list[c], name) == 0) : cliNameIs(list[c], name))
        {
          if ((found[0] != '\0') && (strcmp(found, list[c]) != 0))
          {
            snprintf(error, size, "\"%s\" could be \"%s\" or \"%s\": give the full name.", name, found, list[c]);
            return NULL;
          }

          snprintf(found, sizeof(found), "%s", list[c]);
        }
      }
    }

    if (found[0] != '\0')
    {
      return found;
    }
  }

  snprintf(error, size, "No variable \"%s\" (see status and tests).", name);
  return NULL;
}

// One command, from the main loop. Commands that wait set pending instead of answering.
static void requestRun(char * text, CliOut_t * out)
{
  char *   argv[REQUEST_ARGS_MAX];
  int      argc = 0;
  char     error[ERROR_LEN];
  uint32_t conn;
  char *   args[REQUEST_ARGS_MAX];
  uint32_t count;

  for (char * line = text; (*line != '\0') && (argc < REQUEST_ARGS_MAX);)
  {
    char * end = strchr(line, '\n');

    if (end == NULL)
    {
      break;
    }

    *end = '\0';
    argv[argc++] = line;
    line = end + 1;
  }

  if (argc == 0)
  {
    errorOut(out, "Empty command.");
    return;
  }

  const char * command = argv[0];

  count = positionals(argc, argv, args, REQUEST_ARGS_MAX);

  if (strcmp(command, "status") == 0)
  {
    statusOut(out);
  }
  else if (strcmp(command, "stop") == 0)
  {
    stopping = true;
    cliOutf(out, "{\"ok\":true,\"stopped\":true}\n");
  }
  else if (strcmp(command, "connect") == 0)
  {
    const char * baud = optionGet(argc, argv, "--baud");

    if (count < 1)
    {
      errorOut(out, "connect PORT [--baud N] [--conn N]");
    }
    else if (connGet(argc, argv, &conn, out))
    {
      if (!screensCliConnect(conn, args[0], (baud != NULL) ? (uint32_t) strtoul(baud, NULL, 10) : 0, error, sizeof(error)))
      {
        errorOut(out, "%s", error);
      }
      else
      {
        statusOut(out);
      }
    }
  }
  else if (strcmp(command, "disconnect") == 0)
  {
    if (connGet(argc, argv, &conn, out))
    {
      screensCliDisconnect(conn);
      statusOut(out);
    }
  }
  else if (strcmp(command, "send") == 0)
  {
    const char * wait  = optionGet(argc, argv, "--wait");
    const char * until = optionGet(argc, argv, "--until");

    if (count != 1)
    {
      errorOut(out, "send TEXT [--conn N] [--test] [--wait SECONDS] [--until TEXT] (TEXT quoted if it has spaces)");
    }
    else if (connGet(argc, argv, &conn, out))
    {
      uint64_t from = lineSeq;

      if (!screensCliSend(conn, args[0], flagGet(argc, argv, "--test")))
      {
        bool     open;
        uint32_t baud;

        screensCliPort(conn, &open, &baud);
        errorOut(out, open ? "Could not send on C%lu." : "C%lu is not connected (connect PORT).", (unsigned long) (conn + 1));
      }
      else if ((wait != NULL) || (until != NULL))
      {
        pending = (Pending_t) {PENDING_LINES, lv_tick_get(), (uint32_t) (LV_MIN(atof((wait != NULL) ? wait : "2"), WAIT_MAX_S) * 1000.0),
                               from, messageSeq, (int32_t) conn, READ_MAX_DEFAULT, "", 0, true};
        snprintf(pending.until, sizeof(pending.until), "%s", (until != NULL) ? until : "");
      }
      else
      {
        cliOutf(out, "{\"ok\":true,\"sent\":");
        cliOutString(out, args[0]);
        cliOutf(out, ",\"test_started\":%s}\n", (flagGet(argc, argv, "--test") && (compareCliRecordingIndex() >= 0)) ? "true" : "false");
      }
    }
  }
  else if (strcmp(command, "read") == 0)
  {
    const char * wait  = optionGet(argc, argv, "--wait");
    const char * until = optionGet(argc, argv, "--until");
    const char * max   = optionGet(argc, argv, "--max");
    int32_t      only  = -1;

    if (optionGet(argc, argv, "--conn") != NULL)
    {
      if (!connGet(argc, argv, &conn, out))
      {
        return;
      }

      only = (int32_t) conn;
    }

    pending = (Pending_t) {PENDING_LINES, lv_tick_get(), (uint32_t) (LV_MIN(atof((wait != NULL) ? wait : "0"), WAIT_MAX_S) * 1000.0),
                           lineCursor, messageSeq, only, (max != NULL) ? (uint32_t) strtoul(max, NULL, 10) : READ_MAX_DEFAULT,
                           "", 0, false};
    snprintf(pending.until, sizeof(pending.until), "%s", (until != NULL) ? until : "");
  }
  else if (strcmp(command, "messages") == 0)
  {
    cliOutf(out, "{\"ok\":true,");
    messagesOut(out, flagGet(argc, argv, "--all") ? 0 : messageCursor);
    cliOutf(out, "}\n");
    messageCursor = messageSeq;
  }
  else if (strcmp(command, "test-start") == 0)
  {
    if (!connGet(argc, argv, &conn, out))
    {
      return;
    }

    uint64_t from = messageSeq;

    if (count >= 1)
    {
      if (!screensCliSend(conn, args[0], true))
      {
        errorOut(out, "C%lu is not connected (connect PORT).", (unsigned long) (conn + 1));
        return;
      }
    }
    else
    {
      compareTestStart("Manual", conn);
    }

    cliOutf(out, "{\"ok\":%s,", (compareCliRecordingIndex() >= 0) ? "true" : "false");
    messagesOut(out, from);
    cliOutf(out, "}\n");
  }
  else if (strcmp(command, "test-stop") == 0)
  {
    compareCliTestStop();
    statusOut(out);
  }
  else if (strcmp(command, "chain-set") == 0)
  {
    if (!connGet(argc, argv, &conn, out))
    {
      return;
    }

    if (!compareCliChainSet((const char * const *) args, count, conn))
    {
      errorOut(out, (count > COMPARE_CLI_TESTS_MAX) ? "16 steps at most." : "Stop the chain first (chain-stop).");
    }
    else
    {
      statusOut(out);
    }
  }
  else if (strcmp(command, "chain-run") == 0)
  {
    uint64_t from    = messageSeq;
    bool     started = compareCliChainRun();

    cliOutf(out, "{\"ok\":%s,", started ? "true" : "false");
    messagesOut(out, from);
    cliOutf(out, "}\n");
  }
  else if (strcmp(command, "chain-stop") == 0)
  {
    compareCliChainStop();
    statusOut(out);
  }
  else if (strcmp(command, "wait") == 0)
  {
    const char * timeout = optionGet(argc, argv, "--timeout");

    pending = (Pending_t) {PENDING_WAIT, lv_tick_get(),
                           (uint32_t) (LV_MIN(atof((timeout != NULL) ? timeout : "0"), WAIT_MAX_S) * 1000.0),
                           lineSeq, messageCursor, -1, 0, "", 0, false};

    if (timeout == NULL)
    {
      pending.timeoutMs = WAIT_DEFAULT_S * 1000u;
    }
  }
  else if (strcmp(command, "tests") == 0)
  {
    cliOutf(out, "{\"ok\":true,");
    testsOut(out);
    cliOutf(out, "}\n");
  }
  else if (strcmp(command, "tests-clear") == 0)
  {
    if (compareCliTestsClear())
    {
      cliOutf(out, "{\"ok\":true,\"tests\":0}\n");
    }
    else
    {
      errorOut(out, "A test or chain is under way: stop it first.");
    }
  }
  else if (strcmp(command, "export") == 0)
  {
    const char * path  = optionGet(argc, argv, "--out");
    const char * which = optionGet(argc, argv, "--test");
    uint32_t     total = compareCliTestCount();
    uint32_t     i     = (which != NULL) ? (uint32_t) (strtoul(which, NULL, 10) - 1) : (total - 1);

    if ((path == NULL) || (total == 0) || (i >= total))
    {
      errorOut(out, (total == 0) ? "No test yet." : "export --out FILE.csv [--test N] (N: 1 to %lu, default the last)", (unsigned long) total);
      return;
    }

    CompareCliTest_t test;

    compareCliTestGet(i, &test);

    PlotExport_t * data = plotExportFromTest(test.data);
    bool           ok   = (data != NULL) && plotExportCliWrite(data, path);

    plotExportFree(data);

    if (!ok)
    {
      errorOut(out, "Could not write %s (empty test, or its folder does not exist).", path);
    }
    else
    {
      cliOutf(out, "{\"ok\":true,\"output\":");
      cliOutString(out, path);
      cliOutf(out, ",\"test\":");
      testOut(out, i);
      cliOutf(out, "}\n");
    }
  }
  else if (strcmp(command, "picture") == 0)
  {
    const char * path     = optionGet(argc, argv, "--out");
    const char * constant = optionGet(argc, argv, "--constant");
    const char * format   = optionGet(argc, argv, "--format");
    const char * width    = optionGet(argc, argv, "--width");
    const char * ppi      = optionGet(argc, argv, "--ppi");
    uint32_t     graphs   = 0;

    if (path == NULL)
    {
      errorOut(out, "picture --out FILE.png [--graph VARS]... [--constant VAR] [--format W:H] [--width IN] [--ppi N]");
      return;
    }

    if ((format != NULL) || (width != NULL) || (ppi != NULL))
    {
      if (!benchCliPictureSet((format != NULL) ? format : "16:8", (width != NULL) ? width : "6.5", (ppi != NULL) ? ppi : "300"))
      {
        errorOut(out, "--format: two numbers above 0, like 16:8.");
        return;
      }
    }

    for (int a = 0; (a + 1) < argc; ++a)
    {
      graphs += (strcmp(argv[a], "--graph") == 0) ? 1 : 0;
    }

    if (graphs > COMPARE_CLI_GRAPHS_MAX)
    {
      errorOut(out, "%d graphs at most.", COMPARE_CLI_GRAPHS_MAX);
      return;
    }

    // Names checked first: nothing changes on an error
    for (int a = 0; (a + 1) < argc; ++a)
    {
      if (strcmp(argv[a], "--graph") == 0)
      {
        char     list[REQUEST_LEN];
        uint32_t vars = 0;

        snprintf(list, sizeof(list), "%s", argv[a + 1]);

        for (char * name = strtok(list, ","); name != NULL; name = strtok(NULL, ","))
        {
          name += strspn(name, " ");

          if (*name == '\0')
          {
            continue;
          }

          if (++vars > COMPARE_CLI_VARS_MAX)
          {
            errorOut(out, "%d variables at most in a graph.", COMPARE_CLI_VARS_MAX);
            return;
          }

          if (variableResolve(name, error, sizeof(error)) == NULL)
          {
            errorOut(out, "%s", error);
            return;
          }
        }
      }
    }

    if ((constant != NULL) && (variableResolve(constant, error, sizeof(error)) == NULL))
    {
      errorOut(out, "%s", error);
      return;
    }

    if (graphs > 0)
    {
      uint32_t g = 0;

      compareCliGraphsClear();
      compareCliGraphCountSet(graphs);

      for (int a = 0; (a + 1) < argc; ++a)
      {
        if (strcmp(argv[a], "--graph") == 0)
        {
          char list[REQUEST_LEN];

          snprintf(list, sizeof(list), "%s", argv[a + 1]);

          for (char * name = strtok(list, ","); name != NULL; name = strtok(NULL, ","))
          {
            name += strspn(name, " ");

            if (*name != '\0')
            {
              compareCliVarAdd(g, variableResolve(name, error, sizeof(error)));
            }
          }

          g++;
        }
      }
    }

    if (constant != NULL)
    {
      compareCliConstantSet(variableResolve(constant, error, sizeof(error)));
    }

    if (!compareCliPictureSave(path))
    {
      errorOut(out, "Could not write %s. Does its folder exist?", path);
      return;
    }

    BenchPicture_t picture;
    bool           report = benchPictureGet(&picture);

    cliOutf(out, "{\"ok\":true,\"output\":");
    cliOutString(out, path);

    if (report)
    {
      cliOutf(out, ",\"width_px\":%ld,\"height_px\":%ld,\"ppi\":%ld", (long) picture.width, (long) picture.height, (long) picture.ppi);
    }

    cliOutf(out, ",\"tests\":%lu}\n", (unsigned long) compareCliTestCount());
  }
  else if (strcmp(command, "set") == 0)
  {
    bool live;
    bool variable = (count == 2) && ((strcmp(args[0], "compareConstant") == 0) || (strcmp(args[0], "compareWatch") == 0));

    if (count != 2)
    {
      errorOut(out, "set KEY VALUE (see the keys with get)");
    }
    else if (variable)
    {
      // Lists of the variable names in the app: by name ("" for none)
      const char * name = (args[1][0] != '\0') ? variableResolve(args[1], error, sizeof(error)) : "";

      if (name == NULL)
      {
        errorOut(out, "%s", error);
        return;
      }

      bool constant = strcmp(args[0], "compareConstant") == 0;

      compareCliStopVarsSet(constant ? name : NULL, constant ? NULL : name);
      cliOutf(out, "{\"ok\":true,\"key\":");
      cliOutString(out, args[0]);
      cliOutf(out, ",\"value\":");
      cliOutString(out, name);
      cliOutf(out, ",\"live\":true}\n");
    }
    else if (!settingsCliApply(args[0], args[1], &live, error, sizeof(error)))
    {
      errorOut(out, "%s", error);
    }
    else
    {
      cliOutf(out, "{\"ok\":true,\"key\":");
      cliOutString(out, args[0]);
      cliOutf(out, ",\"value\":");
      cliOutString(out, settingsGetString(args[0], ""));
      cliOutf(out, ",\"live\":%s}\n", live ? "true" : "false");
    }
  }
  else if (strcmp(command, "get") == 0)
  {
    const SettingsCliBinding_t * bindings;
    uint32_t                     total = settingsCliBindings(&bindings);
    bool                         comma = false;

    if (count == 1)
    {
      cliOutf(out, "{\"ok\":true,\"key\":");
      cliOutString(out, args[0]);
      cliOutf(out, ",\"value\":");
      cliOutString(out, settingsGetString(args[0], ""));
      cliOutf(out, "}\n");
      return;
    }

    const char * constant;
    const char * watch;

    compareCliStopVarsGet(&constant, &watch);
    cliOutf(out, "{\"ok\":true,\"settings\":[{\"key\":\"compareConstant\",\"kind\":\"variable\",\"value\":");
    cliOutString(out, constant);
    cliOutf(out, "},{\"key\":\"compareWatch\",\"kind\":\"variable\",\"value\":");
    cliOutString(out, watch);
    cliOutf(out, "}");
    comma = true;

    for (uint32_t b = 0; b < total; ++b)
    {
      const SettingsCliBinding_t * binding = &bindings[b];
      char                         value[LINE_LEN];

      // Hidden duplicates of a key are listed once
      bool seen = false;

      for (uint32_t p = 0; p < b; ++p)
      {
        seen = seen || (strcmp(bindings[p].key, binding->key) == 0);
      }

      if (seen)
      {
        continue;
      }

      cliOutf(out, "%s{\"key\":", comma ? "," : "");
      cliOutString(out, binding->key);

      if (binding->kind == SETTINGS_CLI_TEXT)
      {
        cliOutf(out, ",\"kind\":\"text\",\"value\":");
        cliOutString(out, lv_textarea_get_text(binding->obj));
      }
      else if (binding->kind == SETTINGS_CLI_DROPDOWN)
      {
        lv_dropdown_get_selected_str(binding->obj, value, sizeof(value));
        cliOutf(out, ",\"kind\":\"choice\",\"value\":");
        cliOutString(out, value);
        cliOutf(out, ",\"options\":");
        cliOutString(out, lv_dropdown_get_options(binding->obj));
      }
      else
      {
        cliOutf(out, ",\"kind\":\"switch\",\"value\":\"%s\"", lv_obj_has_state(binding->obj, LV_STATE_CHECKED) ? "on" : "off");
      }

      cliOutf(out, "}");
      comma = true;
    }

    cliOutf(out, "]}\n");
  }
  else
  {
    errorOut(out, "Unknown command %s. Use --help.", command);
  }
}

// Answers a waiting command when it is done
static void pendingCheck(void)
{
  if (pending.kind == PENDING_NONE)
  {
    return;
  }

  bool timedOut = lv_tick_elaps(pending.startTick) >= pending.timeoutMs;

  if (pending.kind == PENDING_LINES)
  {
    bool matched = (pending.until[0] != '\0') && linesMatch(pending.lineFrom, pending.conn, pending.until);

    if (!timedOut && !matched)
    {
      return;
    }

    EnterCriticalSection(&requestLock);
    answer.length = 0;
    cliOutf(&answer, "{\"ok\":true,");

    if (pending.until[0] != '\0')
    {
      cliOutf(&answer, "\"matched\":%s,", matched ? "true" : "false");
    }

    linesOut(&answer, pending.lineFrom, pending.conn, pending.max);
    cliOutf(&answer, ",");
    messagesOut(&answer, pending.messageFrom);
    cliOutf(&answer, "}\n");
    LeaveCriticalSection(&requestLock);

    if (!pending.sent)
    {
      lineCursor = lineSeq;
    }
  }
  else
  {
    if (compareCliBusy())
    {
      pending.idleSince = 0;
    }
    else if (pending.idleSince == 0)
    {
      pending.idleSince = LV_MAX(lv_tick_get(), 1);
    }

    bool done = (pending.idleSince != 0) && (lv_tick_elaps(pending.idleSince) >= WAIT_GRACE_MS);

    if (!done && !timedOut)
    {
      return;
    }

    EnterCriticalSection(&requestLock);
    answer.length = 0;
    cliOutf(&answer, "{\"ok\":true,\"done\":%s,", done ? "true" : "false");
    messagesOut(&answer, pending.messageFrom);
    cliOutf(&answer, ",");
    testsOut(&answer);
    cliOutf(&answer, "}\n");
    LeaveCriticalSection(&requestLock);
    messageCursor = messageSeq;
  }

  pending.kind = PENDING_NONE;
  SetEvent(answerEvent);
}

// One client at a time: reads its arguments, waits for the answer, writes it
static unsigned __stdcall pipeThread(void * arg)
{
  HANDLE pipe = (HANDLE) arg;

  for (;;)
  {
    bool  connected = ConnectNamedPipe(pipe, NULL) || (GetLastError() == ERROR_PIPE_CONNECTED);
    DWORD total = 0;

    if (connected)
    {
      // Arguments, one per line, then an empty line
      while (total < (REQUEST_LEN - 1))
      {
        DWORD got = 0;

        if (!ReadFile(pipe, &request[total], REQUEST_LEN - 1 - total, &got, NULL) || (got == 0))
        {
          break;
        }

        total += got;
        request[total] = '\0';

        if ((total >= 2) && (strcmp(&request[total - 2], "\n\n") == 0))
        {
          break;
        }
      }
    }

    if (connected && (total >= 2) && (strcmp(&request[total - 2], "\n\n") == 0))
    {
      DWORD written;

      request[total - 1] = '\0';
      lastRequestTick = GetTickCount();
      SetEvent(requestEvent);
      WaitForSingleObject(answerEvent, INFINITE);

      EnterCriticalSection(&requestLock);
      WriteFile(pipe, answer.text, (DWORD) answer.length, &written, NULL);
      LeaveCriticalSection(&requestLock);
      FlushFileBuffers(pipe);
      lastRequestTick = GetTickCount();
    }

    DisconnectNamedPipe(pipe);
  }

  return 0;
}

// Ports closed, then the process ends
static void sessionEnd(void)
{
  for (uint32_t c = 0; c < CONN_MAX; ++c)
  {
    screensCliDisconnect(c);
  }

  eventPrint("stopped", "Session ended, ports closed.");
  ExitProcess(CLI_EXIT_OK);
}

static HANDLE pipeOpen(void)
{
  for (uint32_t waited = 0; waited < CONNECT_TRIES_MS; waited += 100)
  {
    HANDLE pipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (pipe != INVALID_HANDLE_VALUE)
    {
      return pipe;
    }

    // No session; busy: another command is being answered
    if (GetLastError() != ERROR_PIPE_BUSY)
    {
      return INVALID_HANDLE_VALUE;
    }

    WaitNamedPipeW(PIPE_NAME, 100);
  }

  return INVALID_HANDLE_VALUE;
}

static bool pipeExchange(int argc, char ** argv, CliOut_t * reply)
{
  HANDLE   pipe = pipeOpen();
  CliOut_t text = {0};
  DWORD    written;

  if (pipe == INVALID_HANDLE_VALUE)
  {
    return false;
  }

  for (int a = 0; a < argc; ++a)
  {
    cliOutf(&text, "%s\n", argv[a]);
  }

  cliOutf(&text, "\n");

  bool ok = WriteFile(pipe, text.text, (DWORD) text.length, &written, NULL);

  free(text.text);
  reply->length = 0;

  while (ok)
  {
    char  chunk[4096];
    DWORD got = 0;

    if (!ReadFile(pipe, chunk, sizeof(chunk), &got, NULL) || (got == 0))
    {
      break;
    }

    cliOutf(reply, "%.*s", (int) got, chunk);
  }

  CloseHandle(pipe);
  return ok && (reply->length > 0);
}

// Appends arg to a command line, quoted as CommandLineToArgvW reads it back
static void argQuote(wchar_t * line, size_t size, const char * arg)
{
  wchar_t wide[4096];
  size_t  used = wcslen(line);

  MultiByteToWideChar(CP_UTF8, 0, arg, -1, wide, sizeof(wide) / sizeof(wide[0]));

  if ((used + 4) >= size)
  {
    return;
  }

  line[used++] = L' ';
  line[used++] = L'"';

  for (const wchar_t * c = wide; (*c != L'\0') && ((used + 4) < size); ++c)
  {
    size_t slashes = 0;

    while (c[slashes] == L'\\')
    {
      slashes++;
    }

    if ((c[slashes] == L'"') || (c[slashes] == L'\0'))
    {
      // Backslashes before a quote (or the closing one) are doubled
      for (size_t s = 0; (s < (2 * slashes)) && ((used + 4) < size); ++s)
      {
        line[used++] = L'\\';
      }

      if (c[slashes] == L'\0')
      {
        break;
      }

      line[used++] = L'\\';
      line[used++] = L'"';
      c += slashes;
    }
    else
    {
      for (size_t s = 0; s < slashes; ++s)
      {
        line[used++] = L'\\';
      }

      c += slashes;
      line[used++] = *c;
    }
  }

  line[used++] = L'"';
  line[used]   = L'\0';
}
