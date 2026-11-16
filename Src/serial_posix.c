// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Serial ports on macOS and Linux (POSIX termios), as serial_win.c does on Windows.

   Ports: macOS /dev/cu.* (the "call-up" devices, which do not wait for a carrier), Linux
   /dev/ttyUSB*, ttyACM*, ttyAMA* and the real ttyS* (see serialListPorts). A port's name is its
   device name without "/dev/" ("cu.usbmodem14201", "ttyACM0").

   Each open port has a reader thread that waits for received bytes (poll()) and stamps each read
   with its arrival time (lv_tick_get()); an LVGL timer passes them to the receive callback in the
   LVGL thread, through the queue of serial_common.c (lossless: when it is full the reader waits).
   The port and the reader thread own the reader's data together (rxRelease), as on Windows.

   Limits: the baud rate must be a standard one on Linux (non-standard rates need termios2, not
   done yet); 1.5 stop bits are not supported; mark / space parity: Linux only (CMSPAR).

   NOT YET BUILT OR TESTED: written on Windows, to be built and checked on macOS / Linux.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#ifndef _WIN32

#define _DARWIN_C_SOURCE
#include "serial.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

#include "platform.h"
#include "profile.h"
#include "serial_common.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE / STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

#define RX_POLL_BUDGET_MS     30          // processing per LVGL timer tick, at most
#define STOP_TIMEOUT_MS       2000        // reader thread end at close
#define READ_WAIT_MS          100         // the reader checks for the end this often
#define WRITE_TIMEOUT_MS      100         // and 1 ms per byte, as on Windows
#define DEVICE_PATH_LEN       (SERIAL_PORT_NAME_LEN + 8)

// Shared by the reader thread and the LVGL timer; owned by both the open port and the reader
// thread: the last of them to let go (rxRelease) closes the port and frees it
typedef struct
{
  int             fd;
  int             wake[2];                // a byte written here ends the reader's wait (close)
  atomic_bool     stop;
  atomic_bool     lost;                   // the port failed (ex: USB unplugged)
  atomic_bool     done;                   // the reader thread no longer uses the port
  atomic_int      owners;
  bool            threadStarted;
  SerialQueue_t * queue;
} SerialRx_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void serialPollCb(lv_timer_t * tim);
static void serialHandleDisconnect(SerialPort_t * port);
static void readerThread(void * arg);
static void rxRelease(SerialRx_t * rx);
static bool portSetup(int fd, const SerialConfig_t * config);
static bool portWanted(const char * name);
static void portDescription(const char * name, char * description, size_t size);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// The ports present now, by name, in no particular order (ports already in use are listed too)
uint32_t serialListPorts(SerialPortInfo_t * ports, uint32_t maxPorts)
{
  uint32_t        count = 0;
  struct dirent * entry;
#ifdef __APPLE__
  DIR *           dir = opendir("/dev");
#else
  DIR *           dir = opendir("/sys/class/tty");
#endif

  if ((ports == NULL) || (dir == NULL))
  {
    if (dir != NULL)
    {
      closedir(dir);
    }

    return 0;
  }

  while (((entry = readdir(dir)) != NULL) && (count < maxPorts))
  {
    if (!portWanted(entry->d_name) || (strlen(entry->d_name) >= SERIAL_PORT_NAME_LEN))
    {
      continue;
    }

    snprintf(ports[count].name, SERIAL_PORT_NAME_LEN, "%s", entry->d_name);
    portDescription(entry->d_name, ports[count].description, SERIAL_PORT_DESC_LEN);
    count++;
  }

  closedir(dir);
  return count;
}

// Opens portName (ex: "ttyACM0") with the given data format and starts its reader thread and
// LVGL timer. rxCb / disconnectCb may be NULL.
bool serialOpen(SerialPort_t * port, const char * portName, const SerialConfig_t * config,
                SerialRxCb_t rxCb, SerialDisconnectCb_t disconnectCb, void * userData)
{
  char path[DEVICE_PATH_LEN];

  if ((port == NULL) || (portName == NULL) || (config == NULL) || (config->dataBits < 5) || (config->dataBits > 8) ||
      (config->parity > SERIAL_PARITY_SPACE) || (config->stopBits > SERIAL_STOP_BITS_2) || !serialFormatIsValid(config) ||
      (strchr(portName, '/') != NULL))
  {
    return false;
  }

  snprintf(path, sizeof(path), "/dev/%s", portName);

  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

  if (fd < 0)
  {
    return false;
  }

  // Exclusive, as on Windows: another program cannot open it meanwhile
  ioctl(fd, TIOCEXCL);

  if (!portSetup(fd, config))
  {
    close(fd);
    return false;
  }

  SerialRx_t *    rx    = calloc(1, sizeof(SerialRx_t));
  SerialQueue_t * queue = (rx != NULL) ? serialQueueCreate() : NULL;

  if ((queue == NULL) || (pipe(rx->wake) != 0))
  {
    serialQueueDestroy(queue);
    free(rx);
    close(fd);
    return false;
  }

  rx->fd     = fd;
  rx->queue  = queue;
  rx->owners = 1;
  fcntl(rx->wake[0], F_SETFD, FD_CLOEXEC);
  fcntl(rx->wake[1], F_SETFD, FD_CLOEXEC);

  // Re-opening an already used struct closes the previous port first
  serialClose(port);

  port->handle       = rx;                // not NULL: open
  port->rx           = rx;
  port->config       = *config;
  port->rxCb         = rxCb;
  port->disconnectCb = disconnectCb;
  port->userData     = userData;
  snprintf(port->name, SERIAL_PORT_NAME_LEN, "%s", portName);

  rx->owners = 2;
  rx->threadStarted = platformThreadStart(readerThread, rx);

  if (!rx->threadStarted)
  {
    rx->owners = 1;
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

  SerialRx_t * rx = (SerialRx_t *) port->rx;

  if (rx != NULL)
  {
    if (rx->threadStarted)
    {
      atomic_store(&rx->stop, true);

      // Ends the reader's wait at once
      if (write(rx->wake[1], "x", 1) < 0)
      {
        // The reader still sees stop within READ_WAIT_MS
      }

      // Not ended in time (driver stuck): the thread frees rx and closes the port when it ends
      for (uint32_t waited = 0; !atomic_load(&rx->done) && (waited < STOP_TIMEOUT_MS); ++waited)
      {
        platformSleepMs(1);
      }
    }

    port->rx = NULL;
    rxRelease(rx);
  }

  port->handle = NULL;
}

bool serialIsOpen(const SerialPort_t * port)
{
  return (port != NULL) && (port->handle != NULL);
}

// Returns the number of bytes written, or -1 on error (waits until written, at most the write
// timeout)
int32_t serialSend(SerialPort_t * port, const uint8_t * data, uint32_t len)
{
  if (!serialIsOpen(port) || (data == NULL))
  {
    return -1;
  }

  SerialRx_t * rx      = (SerialRx_t *) port->rx;
  uint32_t     written = 0;
  int64_t      end     = platformUnixMs() + WRITE_TIMEOUT_MS + len;

  while (written < len)
  {
    ssize_t done = write(rx->fd, &data[written], len - written);

    if (done > 0)
    {
      written += (uint32_t) done;
      continue;
    }

    if ((done < 0) && (errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR))
    {
      serialHandleDisconnect(port);
      return -1;
    }

    // Driver buffer full: wait until it can take more, within the timeout
    int64_t left = end - platformUnixMs();
    struct pollfd out = {rx->fd, POLLOUT, 0};

    if ((left <= 0) || (poll(&out, 1, (int) left) <= 0))
    {
      break;
    }
  }

  tcdrain(rx->fd);
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

  serialQueueStats(rx->queue, stats);
  return true;
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// As serial_win.c: the reads received since the last tick (at most RX_POLL_BUDGET_MS of work),
// then a lost port is reported (after its last data)
static void serialPollCb(lv_timer_t * tim)
{
  SerialPort_t * port  = (SerialPort_t *) lv_timer_get_user_data(tim);
  uint32_t       start = lv_tick_get();
  SerialChunk_t  chunk;

  PROF_PUSH(PROF_QUEUE);

  while (serialIsOpen(port) && (port->rx != NULL) && (lv_tick_elaps(start) < RX_POLL_BUDGET_MS))
  {
    SerialRx_t * rx = (SerialRx_t *) port->rx;

    if (!serialQueuePop(rx->queue, &chunk))
    {
      if (atomic_load(&rx->lost))
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

// Waits for received bytes (or the end), reads them and stamps them at once
static void readerThread(void * arg)
{
  SerialRx_t * rx = (SerialRx_t *) arg;
  uint8_t      buffer[SERIAL_RX_BUFFER_SIZE];

  while (!atomic_load(&rx->stop))
  {
    struct pollfd fds[2] = {{rx->fd, POLLIN, 0}, {rx->wake[0], POLLIN, 0}};
    int           ready  = poll(fds, 2, READ_WAIT_MS);

    if (ready < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      atomic_store(&rx->lost, true);
      break;
    }

    if ((ready == 0) || (fds[1].revents != 0))
    {
      continue;   // timeout (stop checked) or woken to end
    }

    if (fds[0].revents & POLLIN)
    {
      ssize_t got = read(rx->fd, buffer, sizeof(buffer));

      if (got > 0)
      {
        if (!serialQueuePushWait(rx->queue, buffer, (uint32_t) got, lv_tick_get(), &rx->stop))
        {
          break;
        }

        continue;
      }

      // 0 bytes although ready, or an error: the device is gone (USB unplugged)
      if ((got == 0) || ((errno != EAGAIN) && (errno != EINTR)))
      {
        atomic_store(&rx->lost, true);
        break;
      }
    }
    else if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
    {
      atomic_store(&rx->lost, true);
      break;
    }
  }

  atomic_store(&rx->done, true);
  rxRelease(rx);
}

// The last owner (the open port or the reader thread) closes the port and frees rx
static void rxRelease(SerialRx_t * rx)
{
  if (atomic_fetch_sub(&rx->owners, 1) != 1)
  {
    return;
  }

  close(rx->fd);
  close(rx->wake[0]);
  close(rx->wake[1]);
  serialQueueDestroy(rx->queue);
  free(rx);
}

static void serialHandleDisconnect(SerialPort_t * port)
{
  serialClose(port);

  if (port->disconnectCb != NULL)
  {
    port->disconnectCb(port);
  }
}

// termios speed constant of a standard rate; 0: not standard
static speed_t standardSpeed(uint32_t rate)
{
  static const struct
  {
    uint32_t rate;
    speed_t  speed;
  } speeds[] =
  {
    {50, B50}, {75, B75}, {110, B110}, {134, B134}, {150, B150}, {200, B200}, {300, B300}, {600, B600},
    {1200, B1200}, {1800, B1800}, {2400, B2400}, {4800, B4800}, {9600, B9600}, {19200, B19200},
    {38400, B38400}, {57600, B57600}, {115200, B115200}, {230400, B230400},
#ifdef B460800
    {460800, B460800},
#endif
#ifdef B500000
    {500000, B500000},
#endif
#ifdef B576000
    {576000, B576000},
#endif
#ifdef B921600
    {921600, B921600},
#endif
#ifdef B1000000
    {1000000, B1000000},
#endif
#ifdef B1500000
    {1500000, B1500000},
#endif
#ifdef B2000000
    {2000000, B2000000},
#endif
#ifdef B3000000
    {3000000, B3000000},
#endif
#ifdef B4000000
    {4000000, B4000000},
#endif
  };

  for (size_t i = 0; i < (sizeof(speeds) / sizeof(speeds[0])); ++i)
  {
    if (speeds[i].rate == rate)
    {
      return speeds[i].speed;
    }
  }

  return 0;
}

// Raw 8-bit data, no flow control, the data format; DTR and RTS asserted (many USB CDC devices
// only send once DTR is)
static bool portSetup(int fd, const SerialConfig_t * config)
{
  struct termios tio;
  static const tcflag_t sizes[] = {CS5, CS6, CS7, CS8};

  if (tcgetattr(fd, &tio) != 0)
  {
    return false;
  }

  cfmakeraw(&tio);
  tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
  tio.c_cflag |= CLOCAL | CREAD | sizes[config->dataBits - 5];
  tio.c_iflag &= ~(IXON | IXOFF | IXANY);
  tio.c_cc[VMIN]  = 0;
  tio.c_cc[VTIME] = 0;

  switch (config->parity)
  {
    case SERIAL_PARITY_NONE:
      break;

    case SERIAL_PARITY_ODD:
      tio.c_cflag |= PARENB | PARODD;
      break;

    case SERIAL_PARITY_EVEN:
      tio.c_cflag |= PARENB;
      break;

#ifdef CMSPAR
    case SERIAL_PARITY_MARK:
      tio.c_cflag |= PARENB | PARODD | CMSPAR;
      break;

    case SERIAL_PARITY_SPACE:
      tio.c_cflag |= PARENB | CMSPAR;
      break;
#endif

    default:
      return false;
  }

  if (config->stopBits == SERIAL_STOP_BITS_2)
  {
    tio.c_cflag |= CSTOPB;
  }
  else if (config->stopBits != SERIAL_STOP_BITS_1)
  {
    return false;
  }

  speed_t speed = standardSpeed(config->baudRate);

#ifdef __APPLE__
  // Any rate: set after tcsetattr (IOSSIOSPEED)
  if ((cfsetispeed(&tio, (speed != 0) ? speed : B9600) != 0) || (cfsetospeed(&tio, (speed != 0) ? speed : B9600) != 0) ||
      (tcsetattr(fd, TCSANOW, &tio) != 0))
  {
    return false;
  }

  if (speed == 0)
  {
    speed_t custom = (speed_t) config->baudRate;

    if (ioctl(fd, IOSSIOSPEED, &custom) != 0)
    {
      return false;
    }
  }
#else
  if ((speed == 0) || (cfsetispeed(&tio, speed) != 0) || (cfsetospeed(&tio, speed) != 0) || (tcsetattr(fd, TCSANOW, &tio) != 0))
  {
    return false;
  }
#endif

  int lines = TIOCM_DTR | TIOCM_RTS;

  ioctl(fd, TIOCMBIS, &lines);
  tcflush(fd, TCIOFLUSH);
  return true;
}

#ifdef __APPLE__

// /dev/cu.*: the call-up devices (tty.* wait for a carrier)
static bool portWanted(const char * name)
{
  return (strncmp(name, "cu.", 3) == 0) && (strcmp(name, "cu.Bluetooth-Incoming-Port") != 0);
}

static void portDescription(const char * name, char * description, size_t size)
{
  snprintf(description, size, "%s", name);
}

#else

// Text of a file of /sys (first line); false: none
static bool sysRead(const char * path, char * text, size_t size)
{
  FILE * file = fopen(path, "r");
  bool   ok   = (file != NULL) && (fgets(text, (int) size, file) != NULL);

  if (file != NULL)
  {
    fclose(file);
  }

  if (ok)
  {
    text[strcspn(text, "\r\n")] = '\0';
  }

  return ok && (text[0] != '\0');
}

// USB serial (ttyUSB, ttyACM), on-board UART (ttyAMA), and the ttyS* that are real: the legacy
// 8250 ports the kernel lists without hardware are left out
static bool portWanted(const char * name)
{
  char path[PLATFORM_PATH_LEN];
  char link[PLATFORM_PATH_LEN];

  if ((strncmp(name, "ttyUSB", 6) == 0) || (strncmp(name, "ttyACM", 6) == 0) || (strncmp(name, "ttyAMA", 6) == 0))
  {
    return true;
  }

  if (strncmp(name, "ttyS", 4) != 0)
  {
    return false;
  }

  snprintf(path, sizeof(path), "/sys/class/tty/%s/device/driver", name);

  ssize_t len = readlink(path, link, sizeof(link) - 1);

  if (len <= 0)
  {
    return false;
  }

  link[len] = '\0';

  const char * driver = strrchr(link, '/');

  return strcmp((driver != NULL) ? (driver + 1) : link, "serial8250") != 0;
}

// "STM32 Virtual ComPort (ttyACM0)": the USB product name when there is one
static void portDescription(const char * name, char * description, size_t size)
{
  static const char * const levels[] = {"../product", "../../product"};
  char                      path[PLATFORM_PATH_LEN];
  char                      product[SERIAL_PORT_DESC_LEN];

  for (size_t i = 0; i < (sizeof(levels) / sizeof(levels[0])); ++i)
  {
    snprintf(path, sizeof(path), "/sys/class/tty/%s/device/%s", name, levels[i]);

    if (sysRead(path, product, sizeof(product)))
    {
      snprintf(description, size, "%.*s (%s)", (int) (size / 2), product, name);
      return;
    }
  }

  snprintf(description, size, "%s", name);
}

#endif

#endif
