#include <fcntl.h>
#include <polysat/polysat.h>
#include <stdio.h>
#include <time.h>

#define PAYLOAD_PATH

struct PayloadInfo {
  struct ProcessData *proc;
  struct ProcChild *child;
};

static struct PayloadInfo *payload_;

// TODO: create function for GPIO power on/off of cameras

/* Take picture with optical camera */
void optical_capture(int socket, unsigned char cmd, void *data, size_t dataLen,
                     struct sockaddr_in *fromAddr) {
  /* Create a child process to execute payload written code */
  payload_->child = PROC_fork_child(payload_->proc, PAYLOAD_PATH, NULL);

  /* TODO: Need to find a way of monitoring when child process dies (picture
   * taken = child process death) */
  if (!payload_->child) {
    DBG_print(DBG_LEVEL_WARN, "Unable to load optical program\n");
    PROC_cmd(proc, "", NULL, 0, "atom-comms");
    return;
  }

  /* Send message to comms process, so picture can be sent to comms
   * microcontroller*/
  PROC_cmd(payload_->proc, "picture", NULL, 0, "atom-comms");
}

/* Take picture with thermal camera */
void thermal_capture(int socket, unsigned char cmd, void *data, size_t dataLen,
                     struct sockaddr_in *fromAddr) {
  payload_->child = PROC_fork_child(payload_->proc, PAYLOAD_PATH, NULL);

  /* TODO: Need to find a way of monitoring when child process dies (picture
   * taken = child process death) */
  if (!payload_->child) {
    DBG_print(DBG_LEVEL_WARN, "Unable to load thermal program\n");
    PROC_cmd(proc, "", NULL, 0, "atom-comms");
    return;
  }

  /* Send message to comms process, so picture can be sent to comms
   * microcontroller*/
  PROC_cmd(payload_->proc, "picture", NULL, 0, "atom-comms");
}

void power_off(int socket, unsigned char cmd, void *data, size_t dataLen,
               struct sockaddr_in *fromAddr) {}

int signal_handler_end(int signal, void *arg) {
  struct ProcessData *proc = (struct ProcessData *)arg;

  printf("\n\nSignal recieved! Stopping...\n\n");
  EVT_exit_loop(PROC_evt(proc));

  return 0;
}

int main(void) {
  struct PayloadInfo payload;

  // Initialize the process
  proc = PROC_init("atom-payload", WD_DISABLED);
  if (!proc) {
    DBG_print(DBG_LEVEL_FATAL, "FAILED TO INITIALIZE PROCESS\n");
    return -1;
  }

  // Setup a scheduled and signal event
  PROC_signal(proc, SIGINT, &signal_handler_end, proc);

  // Start the event loop
  printf("Capturing");
  EVT_start_loop(PROC_evt(proc));

  // Cleanup liproc
  printf("Cleaning up process...\n");
  PROC_cleanup(proc);
  printf("Done.\n");

  return 0;
}
