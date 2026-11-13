// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Detection of VCOM / COM ports and send / receive (Windows).

   Each open port has a reader thread that waits for received bytes and stamps each chunk with
   its arrival time (lv_tick_get()) as soon as Windows delivers it: the time does not depend on
   how busy the LVGL thread is. An LVGL timer then passes the chunks to the receive callback, in
   the LVGL thread. The port is opened in overlapped mode, so a send is not held by the waiting
   reader.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "Serial_Reception.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <windows.h>
#include <setupapi.h>

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define RX_CHUNKS             512         // chunks waiting for the LVGL timer (~2 s at 115200 baud
                                          // in small chunks, more in large ones)
#define STOP_TIMEOUT_MS       2000        // reader thread end at close
#define READER_RECHECK_MS     100

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  uint32_t tick;                          // lv_tick_get() when received
  uint16_t len;
  uint8_t  data[SERIAL_RX_BUFFER_SIZE];
} RxChunk_t;

// Shared by the reader thread (writes chunks, sets lost) and the LVGL timer (reads chunks)
typedef struct
{
  HANDLE           thread;
  HANDLE           stopEvent;
  CRITICAL_SECTION lock;
  RxChunk_t        chunks[RX_CHUNKS];
  uint32_t         head;                  // next written
  uint32_t         tail;                  // next read
  volatile LONG    lost;                  // the port failed (ex: USB unplugged)
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
static void              chunkPush(SerialRx_t * rx, const uint8_t * data, uint32_t len, uint32_t tick);

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

  // The "\\.\" prefix is required for COM10 and above
  snprintf(path, sizeof(path), "\\\\.\\%s", portName);
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

  if (h == INVALID_HANDLE_VALUE)
  {
    return false;
  }

  if (!GetCommState(h, &dcb))
  {
    CloseHandle(h);
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
    CloseHandle(h);
    return false;
  }

  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

  SerialRx_t * rx = calloc(1, sizeof(SerialRx_t));
  if (rx == NULL)
  {
    CloseHandle(h);
    return false;
  }

  InitializeCriticalSection(&rx->lock);
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

  rx->thread = (rx->stopEvent != NULL) ? (HANDLE) _beginthreadex(NULL, 0, readerThread, port, 0, NULL) : NULL;
  if (rx->thread == NULL)
  {
    serialClose(port);
    return false;
  }

  port->pollTimer = lv_timer_create(serialPollCb, SERIAL_POLL_PERIOD_MS, port);
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

  // The reader thread ends first (it uses the handle and the chunks)
  SerialRx_t * rx = (SerialRx_t *) port->rx;
  if (rx != NULL)
  {
    if (rx->thread != NULL)
    {
      SetEvent(rx->stopEvent);
      WaitForSingleObject(rx->thread, STOP_TIMEOUT_MS);
      CloseHandle(rx->thread);
    }

    if (rx->stopEvent != NULL)
    {
      CloseHandle(rx->stopEvent);
    }

    DeleteCriticalSection(&rx->lock);
    free(rx);
    port->rx = NULL;
  }

  if ((port->handle != NULL) && (port->handle != INVALID_HANDLE_VALUE))
  {
    CloseHandle((HANDLE) port->handle);
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
  SerialPort_t * port = (SerialPort_t *) lv_timer_get_user_data(tim);
  RxChunk_t      chunk;

  while (serialIsOpen(port) && (port->rx != NULL))
  {
    SerialRx_t * rx = (SerialRx_t *) port->rx;
    bool         any;

    EnterCriticalSection(&rx->lock);
    any = rx->tail != rx->head;
    if (any)
    {
      chunk    = rx->chunks[rx->tail];
      rx->tail = (rx->tail + 1) % RX_CHUNKS;
    }
    LeaveCriticalSection(&rx->lock);

    if (!any)
    {
      // Typically the USB VCOM device was unplugged
      if (rx->lost != 0)
      {
        serialHandleDisconnect(port);
      }
      return;
    }

    // The callback may close the port (then the loop ends)
    if (port->rxCb != NULL)
    {
      port->rxCb(port, chunk.data, chunk.len, chunk.tick);
    }
  }
}

// Waits for received bytes (or the stop event), reads them and stamps them at once
static unsigned __stdcall readerThread(void * arg)
{
  SerialPort_t * port = (SerialPort_t *) arg;
  SerialRx_t *   rx   = (SerialRx_t *) port->rx;
  HANDLE         h    = (HANDLE) port->handle;
  OVERLAPPED     waitOv = {0};
  OVERLAPPED     readOv = {0};
  uint8_t        buffer[SERIAL_RX_BUFFER_SIZE];

  waitOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  readOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

  while ((waitOv.hEvent != NULL) && (readOv.hEvent != NULL))
  {
    DWORD bytesRead = 0;

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
      chunkPush(rx, buffer, bytesRead, lv_tick_get());
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

  return 0;
}

// Full (the LVGL thread stalled for seconds): the chunk is dropped
static void chunkPush(SerialRx_t * rx, const uint8_t * data, uint32_t len, uint32_t tick)
{
  EnterCriticalSection(&rx->lock);

  uint32_t next = (rx->head + 1) % RX_CHUNKS;

  if (next != rx->tail)
  {
    RxChunk_t * chunk = &rx->chunks[rx->head];

    chunk->tick = tick;
    chunk->len  = (uint16_t) len;
    memcpy(chunk->data, data, len);
    rx->head = next;
  }

  LeaveCriticalSection(&rx->lock);
}

static void serialHandleDisconnect(SerialPort_t * port)
{
  serialClose(port);

  if (port->disconnectCb != NULL)
  {
    port->disconnectCb(port);
  }
}
