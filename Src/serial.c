// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Detection of VCOM / COM ports and send / receive (Windows).

   Each open port has a reader thread that waits for received bytes and stamps each chunk with
   its arrival time (lv_tick_get()) as soon as Windows delivers it: the time does not depend on
   how busy the LVGL thread is. An LVGL timer then passes the chunks to the receive callback, in
   the LVGL thread. The port is opened in overlapped mode, so a send is not held by the waiting
   reader.

   Lossless reception: the queue between the two threads holds RX_QUEUE_BYTES / RX_QUEUE_CHUNKS
   (about a minute at 1000 lines per second, minutes at 115200 baud), so the LVGL thread can be
   busy (a picture, a large graph) without losing anything. When it is still full, the reader does
   not drop data: it stops reading until there is room. The data then waits in the driver; a sender
   that honors flow control (RTS/CTS on a real UART, USB flow control on a VCOM device) is held
   back, one that cannot wait loses data in the driver or in the device, which Windows may report
   (driverOverruns). No buffer can be lossless if the sender cannot be paused and the app stays
   slower than it for good: the queue only covers temporary stalls.

   Each LVGL timer tick processes the waiting chunks for at most RX_POLL_BUDGET_MS, so a large
   backlog is caught up over several frames instead of freezing one.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "serial.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <windows.h>
#include <setupapi.h>

#include "profile.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define RX_QUEUE_BYTES        (4u * 1024u * 1024u)   // received bytes waiting for the LVGL timer
#define RX_QUEUE_CHUNKS       65536u      // reads waiting (a USB device: about a line each)
#define RX_FULL_WAIT_MS       5           // queue full: the reader checks for room this often
#define RX_POLL_BUDGET_MS     30          // processing per LVGL timer tick, at most
#define RX_DRIVER_QUEUE       65536       // receive buffer asked of the driver (it may give less)
#define STOP_TIMEOUT_MS       2000        // reader thread end at close
#define READER_RECHECK_MS     100

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

// A read: its bytes in the byte ring, whole (never split at its end)
typedef struct
{
  uint32_t offset;
  uint32_t tick;                          // lv_tick_get() when received
  uint16_t len;
} RxRecord_t;

typedef struct
{
  uint32_t tick;
  uint16_t len;
  uint8_t  data[SERIAL_RX_BUFFER_SIZE];
} RxChunk_t;

// Shared by the reader thread (writes chunks, sets lost) and the LVGL timer (reads chunks). Owned
// by both the open port and the reader thread: the last of them to let go (rxRelease) closes the
// port handle and frees it, so a reader thread stuck in a driver never uses freed memory or a
// closed handle.
typedef struct
{
  HANDLE           port;
  HANDLE           thread;
  HANDLE           stopEvent;
  CRITICAL_SECTION lock;
  // Under lock. Its pages are only committed as they are used (large calloc).
  uint8_t          bytes[RX_QUEUE_BYTES];
  RxRecord_t       records[RX_QUEUE_CHUNKS];
  uint32_t         recordHead;            // next written
  uint32_t         recordTail;            // next read
  uint32_t         byteHead;              // next written
  uint32_t         byteTail;              // oldest waiting (= byteHead when empty)
  SerialStats_t    stats;
  volatile LONG    lost;                  // the port failed (ex: USB unplugged)
  volatile LONG    owners;                // the open port, and the reader thread while it runs
  bool             sim;                   // simulated port (profiling builds only)
} SerialRx_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

// Device class "Ports (COM & LPT)" {4D36E978-E325-11CE-BFC1-08002BE10318}
static const GUID portsClassGuid =
  {0x4D36E978, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void              serialPollCb(lv_timer_t * tim);
static void              serialHandleDisconnect(SerialPort_t * port);
static unsigned __stdcall readerThread(void * arg);
static bool              chunkPush(SerialRx_t * rx, const uint8_t * data, uint32_t len, uint32_t tick);
static bool              chunkPushWait(SerialRx_t * rx, const uint8_t * data, uint32_t len, uint32_t tick);
static bool              chunkPop(SerialRx_t * rx, RxChunk_t * chunk);
static void              rxRelease(SerialRx_t * rx);
static bool              portSetup(HANDLE h, const SerialConfig_t * config, DCB * dcbOut, COMMTIMEOUTS * timeoutsOut);
#ifdef SD_PROFILE
static unsigned __stdcall simThread(void * arg);
#endif

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// Fills ports[] with the COM ports currently present and returns how many were found.
// Ports are not opened, so a port already in use by another program is still listed.
uint32_t serialListPorts(SerialPortInfo_t * ports, uint32_t maxPorts)
{
  uint32_t count = 0;
  HDEVINFO devs = SetupDiGetClassDevsA(&portsClassGuid, NULL, NULL, DIGCF_PRESENT);

  if ((ports == NULL) || (devs == INVALID_HANDLE_VALUE))
  {
    return 0;
  }

  SP_DEVINFO_DATA devInfo = {.cbSize = sizeof(SP_DEVINFO_DATA)};

  for (DWORD i = 0; (count < maxPorts) && SetupDiEnumDeviceInfo(devs, i, &devInfo); ++i)
  {
    char  portName[SERIAL_PORT_NAME_LEN] = {0};
    DWORD size = sizeof(portName) - 1;
    HKEY  key = SetupDiOpenDevRegKey(devs, &devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);

    if (key == INVALID_HANDLE_VALUE)
    {
      continue;
    }

    LONG res = RegQueryValueExA(key, "PortName", NULL, NULL, (LPBYTE) portName, &size);
    RegCloseKey(key);

    // Skip printer ports (LPTx)
    if ((res != ERROR_SUCCESS) || (strncmp(portName, "COM", 3) != 0))
    {
      continue;
    }

    snprintf(ports[count].name, SERIAL_PORT_NAME_LEN, "%s", portName);

    if (!SetupDiGetDeviceRegistryPropertyA(devs, &devInfo, SPDRP_FRIENDLYNAME, NULL,
                                           (PBYTE) ports[count].description,
                                           SERIAL_PORT_DESC_LEN, NULL))
    {
      snprintf(ports[count].description, SERIAL_PORT_DESC_LEN, "%s", portName);
    }

    count++;
  }

  SetupDiDestroyDeviceInfoList(devs);

#ifdef SD_PROFILE
  for (uint32_t s = 0; (s < profSimPorts()) && (count < maxPorts); ++s, ++count)
  {
    snprintf(ports[count].name, SERIAL_PORT_NAME_LEN, "SIM%lu", (unsigned long) (s + 1));
    snprintf(ports[count].description, SERIAL_PORT_DESC_LEN, "Simulated port %lu", (unsigned long) (s + 1));
  }
#endif

  return count;
}

// Opens portName (ex: "COM5") with the given data format and starts its reader thread and LVGL
// timer. rxCb / disconnectCb may be NULL. Call with the LVGL lock held if LVGL runs in another
// thread.
bool serialOpen(SerialPort_t * port, const char * portName, const SerialConfig_t * config,
                SerialRxCb_t rxCb, SerialDisconnectCb_t disconnectCb, void * userData)
{
  char         path[SERIAL_PORT_NAME_LEN + 8];
  DCB          dcb = {.DCBlength = sizeof(DCB)};
  COMMTIMEOUTS timeouts = {0};

  if ((port == NULL) || (portName == NULL) || (config == NULL) ||
      (config->dataBits < 5) || (config->dataBits > 8) ||
      (config->parity > SERIAL_PARITY_SPACE) || (config->stopBits > SERIAL_STOP_BITS_2) ||
      !serialFormatIsValid(config))
  {
    return false;
  }

  bool sim = false;
#ifdef SD_PROFILE
  // Simulated port: a stand-in handle (closed as the real one), no driver
  sim = (profSimPorts() > 0) && (strncmp(portName, "SIM", 3) == 0);
#endif

  // The "\\.\" prefix is required for COM10 and above
  snprintf(path, sizeof(path), "\\\\.\\%s", portName);
  HANDLE h = sim ? CreateEventW(NULL, TRUE, FALSE, NULL)
                 : CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

  if ((h == INVALID_HANDLE_VALUE) || (h == NULL))
  {
    return false;
  }

  if (!sim && !portSetup(h, config, &dcb, &timeouts))
  {
    CloseHandle(h);
    return false;
  }

  SerialRx_t * rx = calloc(1, sizeof(SerialRx_t));
  if (rx == NULL)
  {
    CloseHandle(h);
    return false;
  }

  InitializeCriticalSection(&rx->lock);
  rx->port      = h;
  rx->owners    = 1;
  rx->sim       = sim;
  rx->stats.capacityChunks = RX_QUEUE_CHUNKS - 1;
  rx->stats.capacityBytes  = RX_QUEUE_BYTES;
  rx->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

  // Re-opening an already used struct closes the previous port first
  serialClose(port);

  port->handle       = h;
  port->rx           = rx;
  port->config       = *config;
  port->rxCb         = rxCb;
  port->disconnectCb = disconnectCb;
  port->userData     = userData;
  snprintf(port->name, SERIAL_PORT_NAME_LEN, "%s", portName);

  _beginthreadex_proc_type reader = readerThread;
#ifdef SD_PROFILE
  reader = sim ? simThread : readerThread;
#endif

  // The thread gets rx itself, not port: the port struct may be reused before the thread ends
  rx->owners = 2;
  rx->thread = (rx->stopEvent != NULL) ? (HANDLE) _beginthreadex(NULL, 0, reader, rx, 0, NULL) : NULL;
  if (rx->thread == NULL)
  {
    rx->owners = 1;
    serialClose(port);
    return false;
  }

  port->pollTimer = lv_timer_create(serialPollCb, SERIAL_POLL_PERIOD_MS, port);
  return true;
}

// Data format, no flow control, read timeouts of the reader thread
static bool portSetup(HANDLE h, const SerialConfig_t * config, DCB * dcbOut, COMMTIMEOUTS * timeoutsOut)
{
  DCB          dcb = *dcbOut;
  COMMTIMEOUTS timeouts = *timeoutsOut;

  // A larger driver buffer holds more while the reader waits for room (a request: drivers may
  // keep their own size)
  SetupComm(h, RX_DRIVER_QUEUE, 4096);

  if (!GetCommState(h, &dcb))
  {
    return false;
  }

  dcb.BaudRate     = config->baudRate;
  dcb.ByteSize     = config->dataBits;
  dcb.StopBits     = (BYTE) config->stopBits;
  dcb.Parity       = (BYTE) config->parity;
  dcb.fBinary      = TRUE;
  dcb.fParity      = (config->parity != SERIAL_PARITY_NONE);
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fOutX        = FALSE;
  dcb.fInX         = FALSE;
  // Many USB CDC devices only transmit once DTR is asserted
  dcb.fDtrControl  = DTR_CONTROL_ENABLE;
  dcb.fRtsControl  = RTS_CONTROL_ENABLE;

  // ReadFile returns immediately with whatever is already buffered (the reader thread waits for
  // received bytes with WaitCommEvent)
  timeouts.ReadIntervalTimeout        = MAXDWORD;
  timeouts.ReadTotalTimeoutConstant   = 0;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant  = 100;
  timeouts.WriteTotalTimeoutMultiplier = 1;

  if (!SetCommState(h, &dcb) || !SetCommTimeouts(h, &timeouts) || !SetCommMask(h, EV_RXCHAR))
  {
    return false;
  }

  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
  return true;
}

void serialClose(SerialPort_t * port)
{
  if (port == NULL)
  {
    return;
  }

  if (port->pollTimer != NULL)
  {
    lv_timer_delete(port->pollTimer);
    port->pollTimer = NULL;
  }

  // The port handle is closed with rx (it is rx->port), once the reader thread no longer uses it
  SerialRx_t * rx = (SerialRx_t *) port->rx;
  if (rx != NULL)
  {
    if (rx->thread != NULL)
    {
      SetEvent(rx->stopEvent);
      // Wakes the reader thread if a driver holds it in a read or a wait
      CancelIoEx(rx->port, NULL);
      // Not ended in time (driver stuck): the thread frees rx and closes the port when it ends
      WaitForSingleObject(rx->thread, STOP_TIMEOUT_MS);
      CloseHandle(rx->thread);
    }

    port->rx = NULL;
    rxRelease(rx);
  }

  port->handle = NULL;
}

bool serialIsOpen(const SerialPort_t * port)
{
  return (port != NULL) && (port->handle != NULL) && (port->handle != INVALID_HANDLE_VALUE);
}

// Returns the number of bytes written, or -1 on error (waits until written, at most the write
// timeout)
int32_t serialSend(SerialPort_t * port, const uint8_t * data, uint32_t len)
{
  DWORD      written = 0;
  OVERLAPPED overlapped = {0};

  if (!serialIsOpen(port) || (data == NULL))
  {
    return -1;
  }

  // Simulated port: sent nowhere
  if (((SerialRx_t *) port->rx)->sim)
  {
    return (int32_t) len;
  }

  overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (overlapped.hEvent == NULL)
  {
    return -1;
  }

  bool ok = WriteFile((HANDLE) port->handle, data, len, &written, &overlapped) ||
            ((GetLastError() == ERROR_IO_PENDING) &&
             GetOverlappedResult((HANDLE) port->handle, &overlapped, &written, TRUE));

  CloseHandle(overlapped.hEvent);

  if (!ok)
  {
    serialHandleDisconnect(port);
    return -1;
  }

  return (int32_t) written;
}

bool serialStatsGet(const SerialPort_t * port, SerialStats_t * stats)
{
  SerialRx_t * rx = serialIsOpen(port) ? (SerialRx_t *) port->rx : NULL;

  memset(stats, 0, sizeof(*stats));

  if (rx == NULL)
  {
    return false;
  }

  EnterCriticalSection(&rx->lock);
  *stats = rx->stats;
  LeaveCriticalSection(&rx->lock);
  return true;
}

bool serialSendString(SerialPort_t * port, const char * str)
{
  uint32_t len = (str != NULL) ? (uint32_t) strlen(str) : 0;

  return (str != NULL) && (serialSend(port, (const uint8_t *) str, len) == (int32_t) len);
}

// Windows only accepts 1.5 stop bits with 5 data bits, and 2 stop bits with 6 to 8 data bits
bool serialFormatIsValid(const SerialConfig_t * config)
{
  if (config == NULL)
  {
    return false;
  }

  if (config->stopBits == SERIAL_STOP_BITS_1_5)
  {
    return config->dataBits == 5;
  }

  if (config->stopBits == SERIAL_STOP_BITS_2)
  {
    return config->dataBits != 5;
  }

  return true;
}

// Writes the usual short notation of the data format (ex: "8N1", "7E2", "5N1.5")
void serialFormatToString(const SerialConfig_t * config, char * buffer, uint32_t size)
{
  static const char   parityChars[] = {'N', 'O', 'E', 'M', 'S'};
  static const char * stopBitsText[] = {"1", "1.5", "2"};

  if ((config == NULL) || (buffer == NULL) || (size == 0))
  {
    return;
  }

  if ((config->parity > SERIAL_PARITY_SPACE) || (config->stopBits > SERIAL_STOP_BITS_2))
  {
    snprintf(buffer, size, "?");
    return;
  }

  snprintf(buffer, size, "%u%c%s", (unsigned) config->dataBits, parityChars[config->parity],
           stopBitsText[config->stopBits]);
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// Passes the chunks received since the last tick, each with its arrival time; then reports a lost
// port (after its last data)
static void serialPollCb(lv_timer_t * tim)
{
  SerialPort_t * port  = (SerialPort_t *) lv_timer_get_user_data(tim);
  uint32_t       start = lv_tick_get();
  RxChunk_t      chunk;

  // Taking the chunks out; the callbacks' own stages are timed apart
  PROF_PUSH(PROF_QUEUE);

  // A backlog (the LVGL thread was busy) is caught up over several ticks: the rest stays queued
  while (serialIsOpen(port) && (port->rx != NULL) && (lv_tick_elaps(start) < RX_POLL_BUDGET_MS))
  {
    SerialRx_t * rx = (SerialRx_t *) port->rx;

    if (!chunkPop(rx, &chunk))
    {
      // Typically the USB VCOM device was unplugged
      if (rx->lost != 0)
      {
        serialHandleDisconnect(port);
      }
      break;
    }

    // The callback may close the port (then the loop ends)
    if (port->rxCb != NULL)
    {
      port->rxCb(port, chunk.data, chunk.len, chunk.tick);
    }
  }

  PROF_POP();
}

// Waits for received bytes (or the stop event), reads them and stamps them at once
static unsigned __stdcall readerThread(void * arg)
{
  SerialRx_t *   rx   = (SerialRx_t *) arg;
  HANDLE         h    = rx->port;
  OVERLAPPED     waitOv = {0};
  OVERLAPPED     readOv = {0};
  uint8_t        buffer[SERIAL_RX_BUFFER_SIZE];

  waitOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  readOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

#ifdef SD_PROFILE
  if (profReaderPriority())
  {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  }
#endif

  while ((waitOv.hEvent != NULL) && (readOv.hEvent != NULL))
  {
    DWORD bytesRead = 0;

    PROF_START(acqStart);

    // Everything already received (returns at once: read timeouts)
    ResetEvent(readOv.hEvent);
    if (!ReadFile(h, buffer, sizeof(buffer), &bytesRead, &readOv) &&
        ((GetLastError() != ERROR_IO_PENDING) || !GetOverlappedResult(h, &readOv, &bytesRead, TRUE)))
    {
      InterlockedExchange(&rx->lost, 1);
      break;
    }

    if (bytesRead > 0)
    {
      DWORD   errors = 0;
      COMSTAT status;

      // Data lost or damaged before this read, as Windows tells (not all drivers do)
      if (ClearCommError(h, &errors, &status) && (errors != 0))
      {
        EnterCriticalSection(&rx->lock);
        rx->stats.driverOverruns += ((errors & (CE_RXOVER | CE_OVERRUN)) != 0) ? 1 : 0;
        rx->stats.lineErrors     += ((errors & (CE_FRAME | CE_RXPARITY | CE_BREAK)) != 0) ? 1 : 0;
        LeaveCriticalSection(&rx->lock);
      }

      bool queued = chunkPushWait(rx, buffer, bytesRead, lv_tick_get());

      PROF_END_THREAD(PROF_ACQ, acqStart);

      if (!queued)
      {
        break;
      }

      continue;
    }

    // Nothing left: wait for the next received byte, or the end
    DWORD  mask = 0;
    HANDLE events[2] = {rx->stopEvent, waitOv.hEvent};

    ResetEvent(waitOv.hEvent);
    if (!WaitCommEvent(h, &mask, &waitOv))
    {
      if (GetLastError() != ERROR_IO_PENDING)
      {
        InterlockedExchange(&rx->lost, 1);
        break;
      }

      // Also every READER_RECHECK_MS: read again, in case a driver missed a notification
      DWORD woken = WaitForMultipleObjects(2, events, FALSE, READER_RECHECK_MS);

      if (woken != (WAIT_OBJECT_0 + 1))
      {
        CancelIo(h);
        GetOverlappedResult(h, &waitOv, &bytesRead, TRUE);

        if (woken == WAIT_OBJECT_0)
        {
          break;
        }
        continue;
      }

      if (!GetOverlappedResult(h, &waitOv, &bytesRead, FALSE))
      {
        InterlockedExchange(&rx->lost, 1);
        break;
      }
    }

    if (WaitForSingleObject(rx->stopEvent, 0) == WAIT_OBJECT_0)
    {
      break;
    }
  }

  if (waitOv.hEvent != NULL)
  {
    CloseHandle(waitOv.hEvent);
  }
  if (readOv.hEvent != NULL)
  {
    CloseHandle(readOv.hEvent);
  }

  // Last use of rx by this thread
  rxRelease(rx);
  return 0;
}

// The last owner (the open port or the reader thread) closes the port and frees rx
static void rxRelease(SerialRx_t * rx)
{
  if (InterlockedDecrement(&rx->owners) != 0)
  {
    return;
  }

  CloseHandle(rx->port);

  if (rx->stopEvent != NULL)
  {
    CloseHandle(rx->stopEvent);
  }

  DeleteCriticalSection(&rx->lock);
  free(rx);
}

// Queues a read (len <= SERIAL_RX_BUFFER_SIZE); false when there is no room (nothing queued)
static bool chunkPush(SerialRx_t * rx, const uint8_t * data, uint32_t len, uint32_t tick)
{
  uint32_t offset;
  bool     room;

  EnterCriticalSection(&rx->lock);

  bool empty = rx->recordHead == rx->recordTail;

  if (empty)
  {
    rx->byteHead = 0;
    rx->byteTail = 0;
  }

  // Bytes: after the newest, or from the start of the ring when the end is too short. Never up to
  // the oldest exactly (head == tail means empty).
  if (rx->byteHead >= rx->byteTail)
  {
    room   = ((RX_QUEUE_BYTES - rx->byteHead) >= len) || (rx->byteTail > len);
    offset = ((RX_QUEUE_BYTES - rx->byteHead) >= len) ? rx->byteHead : 0;
  }
  else
  {
    room   = (rx->byteTail - rx->byteHead) > len;
    offset = rx->byteHead;
  }

  room = room && (((rx->recordHead + 1) % RX_QUEUE_CHUNKS) != rx->recordTail);

  if (room)
  {
    RxRecord_t * record = &rx->records[rx->recordHead];

    memcpy(&rx->bytes[offset], data, len);
    *record = (RxRecord_t) {offset, tick, (uint16_t) len};
    rx->recordHead = (rx->recordHead + 1) % RX_QUEUE_CHUNKS;
    rx->byteHead   = offset + len;

    // Ring wasted at the end when the read went to the start: counted as used until read
    rx->stats.chunks++;
    rx->stats.bytes += len;
    rx->stats.queuedChunks++;
    rx->stats.queuedBytes += len;
    rx->stats.highWaterChunks = LV_MAX(rx->stats.highWaterChunks, rx->stats.queuedChunks);
    PROF_DO(profQueueDepth(rx->stats.queuedChunks);)
    rx->stats.highWaterBytes  = LV_MAX(rx->stats.highWaterBytes, rx->stats.queuedBytes);
  }

  LeaveCriticalSection(&rx->lock);
  return room;
}

// Queue full: waits for room instead of dropping the data (the reader pauses; see the file
// comment). false: the port is being closed (the data is then not needed).
static bool chunkPushWait(SerialRx_t * rx, const uint8_t * data, uint32_t len, uint32_t tick)
{
  if (chunkPush(rx, data, len, tick))
  {
    return true;
  }

  uint32_t start = lv_tick_get();
  bool     queued = false;

  EnterCriticalSection(&rx->lock);
  rx->stats.fullWaits++;
  LeaveCriticalSection(&rx->lock);

  while (!queued && (WaitForSingleObject(rx->stopEvent, RX_FULL_WAIT_MS) == WAIT_TIMEOUT))
  {
    // Keeps its arrival time: when the bytes came, not when there was room
    queued = chunkPush(rx, data, len, tick);
  }

  EnterCriticalSection(&rx->lock);
  rx->stats.fullWaitMs += lv_tick_elaps(start);
  LeaveCriticalSection(&rx->lock);
  PROF_DO(profQueueWait(lv_tick_elaps(start));)
  return queued;
}

// Oldest read waiting; false when none
static bool chunkPop(SerialRx_t * rx, RxChunk_t * chunk)
{
  EnterCriticalSection(&rx->lock);

  bool any = rx->recordTail != rx->recordHead;

  if (any)
  {
    const RxRecord_t * record = &rx->records[rx->recordTail];

    chunk->tick = record->tick;
    chunk->len  = record->len;
    memcpy(chunk->data, &rx->bytes[record->offset], record->len);

    rx->recordTail = (rx->recordTail + 1) % RX_QUEUE_CHUNKS;
    rx->byteTail   = (rx->recordTail != rx->recordHead) ? rx->records[rx->recordTail].offset : rx->byteHead;
    rx->stats.queuedChunks--;
    rx->stats.queuedBytes -= record->len;
  }

  LeaveCriticalSection(&rx->lock);
  return any;
}

#ifdef SD_PROFILE
// Simulated port: profSimRate() lines per second, each its own chunk (as a USB device sends a
// line per packet), through the same queue as a real port. Line: "<sequence>,<value>,...\n", the
// sequence counting 0..999999 so lost lines can be counted (profile.c).
static unsigned __stdcall simThread(void * arg)
{
  SerialRx_t *  rx      = (SerialRx_t *) arg;
  // High resolution timer (Windows 10 1803+): 1 ms steps instead of the 15.6 ms of the system tick
  HANDLE        timer   = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  LARGE_INTEGER frequency;
  LARGE_INTEGER start;
  LARGE_INTEGER now;
  uint64_t      sent    = 0;
  uint32_t      rate    = profSimRate();
  uint32_t      values  = profSimValues();
  char          line[SERIAL_RX_BUFFER_SIZE];

  if (timer == NULL)
  {
    timer = CreateWaitableTimerW(NULL, FALSE, NULL);
  }

  if (profReaderPriority())
  {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  }

  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);

  while (timer != NULL)
  {
    LARGE_INTEGER due = {.QuadPart = -10000};    // 1 ms
    HANDLE        events[2] = {rx->stopEvent, timer};

    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);

    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0)
    {
      break;
    }

    QueryPerformanceCounter(&now);

    uint64_t target = (uint64_t) (((double) (now.QuadPart - start.QuadPart) * rate) / (double) frequency.QuadPart);

    // At most one second of lines per wake; behind (ex: this thread paused), it catches up: every
    // line is sent, so a gap in the sequence numbers is a loss in the app
    for (uint32_t n = 0; (sent < target) && (n < rate); ++n, ++sent)
    {
      PROF_START(acqStart);
      // Stamped this late after the line was due (the thread waited for a core)
      profStampDelay((uint64_t) acqStart - (uint64_t) start.QuadPart - (uint64_t) (((double) sent * frequency.QuadPart) / rate));
      double t   = (double) sent / rate;
      int    len = snprintf(line, sizeof(line), "%lu", (unsigned long) (sent % 1000000));

      for (uint32_t v = 1; (v < values) && (len < (int) sizeof(line) - 16); ++v)
      {
        len += snprintf(&line[len], sizeof(line) - (size_t) len, ",%.3f", 10.0 * sin((t * (v + 1)) + v) + (double) v);
      }

      line[len++] = '\n';

      // As a sender that can be held back (flow control): it waits while the queue is full
      bool queued = chunkPushWait(rx, (const uint8_t *) line, (uint32_t) len, lv_tick_get());

      PROF_END_THREAD(PROF_ACQ, acqStart);

      if (!queued)
      {
        break;
      }
    }
  }

  if (timer != NULL)
  {
    CloseHandle(timer);
  }

  rxRelease(rx);
  return 0;
}
#endif

static void serialHandleDisconnect(SerialPort_t * port)
{
  serialClose(port);

  if (port->disconnectCb != NULL)
  {
    port->disconnectCb(port);
  }
}
