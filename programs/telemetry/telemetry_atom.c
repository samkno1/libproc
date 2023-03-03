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

int record_telemetry(struct ProcessData *proc, void *arg) {
  int flags, bytes;
  char buff[1024];

  struct TelemetryInfo *telemetry = (struct TelemetryInfo *)arg;

  // temp
  get_obc_temp(telemetry->obc_temp);

  // create telemetry file
  flags = O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK;
  telemetry->telemetry_fd = open(TELEMETRY_PATH, flags);

  // write temperature
  snprintf(buff, 50, "OBC Temperature: %f\n", telemetry->obc_temp);
  if (!(bytes = write(telemetry->telemetry_fd, buff, sizeof(buff)))) {
    DBG_print(DBG_LEVEL_INFO, "Wrote %d bytes\n", bytes);
  }

  if (!close(telemetry->telemetry_fd)) {
    DBG_print(DBG_LEVEL_WARN, "Unable to close file\n");
  }

  return EVENT_KEEP;
}

int main(void) {
  // Where libproc stores its state
  struct ProcessData *proc;

  // telemetry parameters
  struct TelemetryInfo telemetry;

  // Initialize the process
  proc = PROC_init("telemetry", WD_ENABLED);
  if (!proc) {
    DBG_print(DBG_LEVEL_FATAL, "FAILED TO INITIALIZE PROCESS\n");
    return -1;
  }
  DBG_init(proc->name);

  // Setup a scheduled and signal event
  EVT_sched_add(PROC_evt(proc), EVT_ms2tv(RECORD_INTERVAL), &record_telemetry,
                &telemetry);

  // Start the event loop
  DBG_print(DBG_LEVEL_INFO, "Collecting telemetry...\n");
  EVT_start_loop(PROC_evt(proc));

  // Cleanup liproc
  DBG_print(DBG_LEVEL_INFO, "Cleaning up process...\n");
  PROC_cleanup(proc);

  return 0;
}