// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Receive queue of a serial port, between its reader thread and the LVGL thread; the same
          on every system (serial_win.c, serial_posix.c, both using serial_common.c).

   It holds SERIAL_QUEUE_BYTES / SERIAL_QUEUE_CHUNKS (about a minute at 1000 lines per second,
   minutes at 115200 baud), so the LVGL thread can be busy (a picture, a large graph) without
   losing anything. When it is still full, the reader does not drop data: it waits for room
   (serialQueuePushWait). The data then waits in the driver; a sender that honors flow control
   (RTS/CTS on a real UART, USB flow control on a VCOM device) is held back, one that cannot wait
   loses data in the driver or in the device, which the system may report. No buffer can be
   lossless if the sender cannot be paused and the app stays slower than it for good: the queue
   only covers temporary stalls.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef SERIAL_COMMON_H
#define SERIAL_COMMON_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "serial.h"

// ================================================================================================
// ================================================================================================
//            DEFINE / STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

#define SERIAL_QUEUE_BYTES    (4u * 1024u * 1024u)   // received bytes waiting for the LVGL timer
#define SERIAL_QUEUE_CHUNKS   65536u      // reads waiting (a USB device: about a line each)

typedef struct SerialQueue SerialQueue_t;

// A read taken out of the queue, with the time it arrived (lv_tick_get())
typedef struct
{
  uint32_t tick;
  uint16_t len;
  uint8_t  data[SERIAL_RX_BUFFER_SIZE];
} SerialChunk_t;

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// NULL: out of memory. Its pages are only committed as they are used (large calloc).
SerialQueue_t * serialQueueCreate(void);
void            serialQueueDestroy(SerialQueue_t * queue);

// Reader thread: queues a read (len <= SERIAL_RX_BUFFER_SIZE); false: no room (nothing queued)
bool serialQueuePush(SerialQueue_t * queue, const uint8_t * data, uint32_t len, uint32_t tick);

// Reader thread: queue full, waits for room instead of dropping the data. false: *stop was set
// (the port is being closed: the data is then not needed).
bool serialQueuePushWait(SerialQueue_t * queue, const uint8_t * data, uint32_t len, uint32_t tick, atomic_bool * stop);

// LVGL thread: the oldest read waiting; false: none
bool serialQueuePop(SerialQueue_t * queue, SerialChunk_t * chunk);

// Counters (any thread)
void serialQueueStats(SerialQueue_t * queue, SerialStats_t * stats);
void serialQueueErrors(SerialQueue_t * queue, bool driverOverrun, bool lineError);

#endif //SERIAL_COMMON_H
