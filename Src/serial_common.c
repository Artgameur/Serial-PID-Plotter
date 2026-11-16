// ------------------------------------------------------------------------------------------------
/*!@file
   @brief What serial_win.c and serial_posix.c share: the receive queue (see serial_common.h:
          reads in a byte ring, each whole, never split at the ring's end, with their arrival
          time) and the functions of serial.h that do not depend on the system.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "serial_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "profile.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE / STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

#define FULL_WAIT_MS          5           // queue full: the reader checks for room this often

typedef struct
{
  uint32_t offset;
  uint32_t tick;                          // lv_tick_get() when received
  uint16_t len;
} SerialRecord_t;

// Under lock
struct SerialQueue
{
  PlatformMutex_t * lock;
  uint8_t           bytes[SERIAL_QUEUE_BYTES];
  SerialRecord_t    records[SERIAL_QUEUE_CHUNKS];
  uint32_t          recordHead;           // next written
  uint32_t          recordTail;           // next read
  uint32_t          byteHead;             // next written
  uint32_t          byteTail;             // oldest waiting (= byteHead when empty)
  SerialStats_t     stats;
};

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTIONS
// ================================================================================================
// ================================================================================================

SerialQueue_t * serialQueueCreate(void)
{
  SerialQueue_t * queue = calloc(1, sizeof(SerialQueue_t));

  if (queue == NULL)
  {
    return NULL;
  }

  queue->lock = platformMutexCreate();

  if (queue->lock == NULL)
  {
    free(queue);
    return NULL;
  }

  queue->stats.capacityChunks = SERIAL_QUEUE_CHUNKS - 1;
  queue->stats.capacityBytes  = SERIAL_QUEUE_BYTES;
  return queue;
}

void serialQueueDestroy(SerialQueue_t * queue)
{
  if (queue != NULL)
  {
    platformMutexDestroy(queue->lock);
    free(queue);
  }
}

bool serialQueuePush(SerialQueue_t * queue, const uint8_t * data, uint32_t len, uint32_t tick)
{
  uint32_t offset;
  bool     room;

  platformMutexLock(queue->lock);

  if (queue->recordHead == queue->recordTail)
  {
    queue->byteHead = 0;
    queue->byteTail = 0;
  }

  // Bytes: after the newest, or from the start of the ring when the end is too short. Never up to
  // the oldest exactly (head == tail means empty).
  if (queue->byteHead >= queue->byteTail)
  {
    room   = ((SERIAL_QUEUE_BYTES - queue->byteHead) >= len) || (queue->byteTail > len);
    offset = ((SERIAL_QUEUE_BYTES - queue->byteHead) >= len) ? queue->byteHead : 0;
  }
  else
  {
    room   = (queue->byteTail - queue->byteHead) > len;
    offset = queue->byteHead;
  }

  room = room && (((queue->recordHead + 1) % SERIAL_QUEUE_CHUNKS) != queue->recordTail);

  if (room)
  {
    SerialRecord_t * record = &queue->records[queue->recordHead];

    memcpy(&queue->bytes[offset], data, len);
    *record = (SerialRecord_t) {offset, tick, (uint16_t) len};
    queue->recordHead = (queue->recordHead + 1) % SERIAL_QUEUE_CHUNKS;
    queue->byteHead   = offset + len;

    // Ring wasted at the end when the read went to the start: counted as used until read
    queue->stats.chunks++;
    queue->stats.bytes += len;
    queue->stats.queuedChunks++;
    queue->stats.queuedBytes += len;
    queue->stats.highWaterChunks = (queue->stats.queuedChunks > queue->stats.highWaterChunks) ? queue->stats.queuedChunks
                                                                                              : queue->stats.highWaterChunks;
    queue->stats.highWaterBytes  = (queue->stats.queuedBytes > queue->stats.highWaterBytes) ? queue->stats.queuedBytes
                                                                                            : queue->stats.highWaterBytes;
    PROF_DO(profQueueDepth(queue->stats.queuedChunks);)
  }

  platformMutexUnlock(queue->lock);
  return room;
}

bool serialQueuePushWait(SerialQueue_t * queue, const uint8_t * data, uint32_t len, uint32_t tick, atomic_bool * stop)
{
  if (serialQueuePush(queue, data, len, tick))
  {
    return true;
  }

  int64_t start  = platformUnixMs();
  bool    queued = false;

  platformMutexLock(queue->lock);
  queue->stats.fullWaits++;
  platformMutexUnlock(queue->lock);

  while (!queued && !atomic_load(stop))
  {
    platformSleepMs(FULL_WAIT_MS);
    // Keeps its arrival time: when the bytes came, not when there was room
    queued = serialQueuePush(queue, data, len, tick);
  }

  uint32_t waited = (uint32_t) (platformUnixMs() - start);

  platformMutexLock(queue->lock);
  queue->stats.fullWaitMs += waited;
  platformMutexUnlock(queue->lock);
  PROF_DO(profQueueWait(waited);)
  return queued;
}

bool serialQueuePop(SerialQueue_t * queue, SerialChunk_t * chunk)
{
  platformMutexLock(queue->lock);

  bool any = queue->recordTail != queue->recordHead;

  if (any)
  {
    const SerialRecord_t * record = &queue->records[queue->recordTail];

    chunk->tick = record->tick;
    chunk->len  = record->len;
    memcpy(chunk->data, &queue->bytes[record->offset], record->len);

    queue->recordTail = (queue->recordTail + 1) % SERIAL_QUEUE_CHUNKS;
    queue->byteTail   = (queue->recordTail != queue->recordHead) ? queue->records[queue->recordTail].offset : queue->byteHead;
    queue->stats.queuedChunks--;
    queue->stats.queuedBytes -= record->len;
  }

  platformMutexUnlock(queue->lock);
  return any;
}

void serialQueueStats(SerialQueue_t * queue, SerialStats_t * stats)
{
  platformMutexLock(queue->lock);
  *stats = queue->stats;
  platformMutexUnlock(queue->lock);
}

void serialQueueErrors(SerialQueue_t * queue, bool driverOverrun, bool lineError)
{
  platformMutexLock(queue->lock);
  queue->stats.driverOverruns += driverOverrun ? 1 : 0;
  queue->stats.lineErrors     += lineError ? 1 : 0;
  platformMutexUnlock(queue->lock);
}

// ================================================================================================
// ================================================================================================
//            serial.h FUNCTIONS THE SAME ON EVERY SYSTEM
// ================================================================================================
// ================================================================================================

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
