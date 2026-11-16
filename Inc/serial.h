// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Detection of VCOM / COM ports and send / receive through LVGL timers (Windows).

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define SERIAL_PORT_NAME_LEN    64          // macOS: "cu.usbmodem14201", Linux: "ttyACM0"
#define SERIAL_PORT_DESC_LEN    128
#define SERIAL_MAX_PORTS        32
#define SERIAL_RX_BUFFER_SIZE   256
#define SERIAL_POLL_PERIOD_MS   10

// ================================================================================================
// ================================================================================================
//            ENUM DECLARATION
// ================================================================================================
// ================================================================================================

// Values match the Windows DCB constants (NOPARITY, ODDPARITY, ...)
typedef enum
{
  SERIAL_PARITY_NONE  = 0,
  SERIAL_PARITY_ODD   = 1,
  SERIAL_PARITY_EVEN  = 2,
  SERIAL_PARITY_MARK  = 3,
  SERIAL_PARITY_SPACE = 4,
} SerialParity_t;

// Values match the Windows DCB constants (ONESTOPBIT, ONE5STOPBITS, TWOSTOPBITS)
typedef enum
{
  SERIAL_STOP_BITS_1   = 0,
  SERIAL_STOP_BITS_1_5 = 1,
  SERIAL_STOP_BITS_2   = 2,
} SerialStopBits_t;

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct SerialPort SerialPort_t;

// Data format; 1.5 stop bits is only valid with 5 data bits, 2 stop bits only with 6 to 8
typedef struct
{
  uint32_t         baudRate;
  uint8_t          dataBits;   // 5 to 8
  SerialParity_t   parity;
  SerialStopBits_t stopBits;
} SerialConfig_t;

#define SERIAL_CONFIG_DEFAULT {115200, 8, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_1}

// Called from the LVGL timer: safe to update LVGL objects inside. tick: lv_tick_get() when the
// data arrived (stamped by the port's reader thread, not when the timer runs)
typedef void (*SerialRxCb_t)(SerialPort_t * port, const uint8_t * data, uint32_t len, uint32_t tick);
typedef void (*SerialDisconnectCb_t)(SerialPort_t * port);

// Detected port (ex: name = "COM5", description = "STMicroelectronics Virtual COM Port (COM5)")
typedef struct
{
  char name[SERIAL_PORT_NAME_LEN];
  char description[SERIAL_PORT_DESC_LEN];
} SerialPortInfo_t;

// Receive queue of an open port (reader thread to LVGL thread), since it was opened. The app never
// drops received data: with the queue full, the reader stops reading until there is room (the data
// waits in the driver, which can then hold the sender back: see serial_common.h).
typedef struct
{
  uint64_t chunks;                        // queued
  uint64_t bytes;
  uint32_t queuedChunks;                  // waiting now
  uint32_t queuedBytes;
  uint32_t highWaterChunks;               // most waiting at once
  uint32_t highWaterBytes;
  uint32_t capacityChunks;
  uint32_t capacityBytes;
  uint32_t fullWaits;                     // times the queue was full: reading paused
  uint32_t fullWaitMs;                    // how long in all
  uint32_t driverOverruns;                // Windows lost received data before the app read it
  uint32_t lineErrors;                    // framing / parity errors and breaks reported by Windows
} SerialStats_t;

// Owned by the caller; must stay alive while open (declare it static or global)
struct SerialPort
{
  void *               handle;
  void *               rx;                // reader thread and received chunks (serial_win.c / serial_posix.c)
  char                 name[SERIAL_PORT_NAME_LEN];
  SerialConfig_t       config;
  lv_timer_t *         pollTimer;
  SerialRxCb_t         rxCb;
  SerialDisconnectCb_t disconnectCb;
  void *               userData;
};

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

uint32_t serialListPorts(SerialPortInfo_t * ports, uint32_t maxPorts);

bool serialOpen(SerialPort_t * port, const char * portName, const SerialConfig_t * config,
                SerialRxCb_t rxCb, SerialDisconnectCb_t disconnectCb, void * userData);
void serialClose(SerialPort_t * port);
bool serialIsOpen(const SerialPort_t * port);

int32_t serialSend(SerialPort_t * port, const uint8_t * data, uint32_t len);
bool    serialSendString(SerialPort_t * port, const char * str);

// Receive queue counters of an open port; false (all 0) when it is not open
bool serialStatsGet(const SerialPort_t * port, SerialStats_t * stats);

bool serialFormatIsValid(const SerialConfig_t * config);
void serialFormatToString(const SerialConfig_t * config, char * buffer, uint32_t size);

#endif //SERIAL_H
