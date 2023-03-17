#include "telemetry_atom.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define TEMP_PATH "/sys/bus/i2c/devices/4-004c/temp1_input"
#define TELEMETRY_PATH "/root/telemetry"

#define POST_INTERVAL 21700000
#define RECORD_INTERVAL 60000

enum TelemetryType {
  kTemp = 0,
  kAdcs = 1,
};

// used only for getting adcs telemetry
static struct TelemetryInfo *telem;

void get_obc_temp(float *temp) {
  int temp_fd;
  int bytes;

  if (!(temp_fd = open(TEMP_PATH, O_RDONLY))) {
    DBG_print(DBG_LEVEL_WARN, "Unable to open temperature file\n");
  }

  if (!(bytes = read(temp_fd, &temp, 1))) {
    DBG_print(DBG_LEVEL_WARN, "Unable to read temperature file\n");
  } else {
    DBG_print(DBG_LEVEL_INFO, "Read %d bytes\n", bytes);
  }

  if (!close(temp_fd)) {
    DBG_print(DBG_LEVEL_WARN, "Unable to close file\n");
  }

  return;
}

void receive_adcs_telemetry(int socket, unsigned char cmd, void *data,
                            size_t dataLen, struct sockaddr_in *fromAddr) {
  int res = 0;

  struct AdcsTelemetry *adcs;
  adcs = (struct AdcsTelemetry *)data;

  if (_write_telemetry(telem->proc, telem, kAdcs) < 0) {
    DBG_print(DBG_LEVEL_WARN, "Unable to write telemetry\n");
  }
}

// internal function for writing telemetry to file
int _write_telemetry(ProcessData *proc, struct TelemetryInfo *telemetry,
                     enum TelemetryType typ) {
  // write temperature
  int res, buff_size;
  char buff[256];

  if (typ == kTemp) {
    buff_size =
        snprintf(buff, 50, "OBC Temperature: %f\n", telemetry->obc_temp);
  } else if (typ = kAdcs) {
    buff_size = snprintf(buff, 50, "ADCS active devices #: %d\n",
                         telemetry->adcs_info.num_active);
  }

  res = PROC_nonblocking_write(proc, telemetry->telemetry_fd, buff, buff_size,
                               FREE_DATA_AFTER_WRITE);

  return res;
}

int record_telemetry(ProcessData *proc, void *arg) {
  struct TelemetryInfo *telemetry = (struct TelemetryInfo *)arg;

  get_obc_temp(telemetry->obc_temp);

  if (!PROC_cmd(proc, "telemetry?", *proc, sizeof(*proc), "adcs")) {
    DBG_print(DBG_LEVEL_WARN, "Unable to send command to adcs\n");
  }

  if (_write_telemetry(proc, telemetry, kTemp) < 0) {
    DBG_print(DBG_LEVEL_WARN, "Unable to write telemetry\n");
  }
  return EVENT_KEEP;
}

int sigint_handler(int signum, void *arg) {
  DBG("SIGINT handler!\n");
  EVT_exit_loop(PROC_evt(arg));
  return EVENT_KEEP;
}

int main(void) {
  // telemetry parameters
  struct TelemetryInfo telemetry;

  // initialize structure
  // memset(telemetry, 0, sizeof(telemetry));
  telemetry.obc_temp = -1;

  // Initialize the process
  telem = &telemetry;
  telemetry.proc = PROC_init("telemetry", WD_ENABLED);
  if (!proc) {
    DBG_print(DBG_LEVEL_FATAL, "FAILED TO INITIALIZE PROCESS\n");
    return -1;
  }

  DBG_init();

  // initialize file descriptor
  telemetry.telemetry_fd =
      open(TELEMETRY_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND,
           S_IRUSR | S_IWUSR);

  // Setup a scheduled and signal event
  EVT_sched_add(PROC_evt(telemetry.proc), EVT_ms2tv(RECORD_INTERVAL),
                &record_telemetry, &telemetry);
  PROC_signal(telemetry.proc, SIGINT, &sigint_handler, telemetry.proc);

  // Start the event loop
  DBG_print(DBG_LEVEL_INFO, "Collecting telemetry...\n");
  EVT_start_loop(PROC_evt(telemetry.proc));

  // Cleanup liproc
  DBG_print(DBG_LEVEL_INFO, "Cleaning up process...\n");
  PROC_cleanup(telemetry.proc);

  return 0;
}