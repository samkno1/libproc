#include "polysat.h"

#ifndef TELEMETRY_H_
#define TELEMETRY_H_

struct TelemetryInfo {
  float obc_temp;
  struct adcs_telemetry adcs_info;
  int telemetry_fd;
  ProcessData *proc;
};

// Telemetry for Adcs subsystem. Still under construction.
struct AdcsTelemetry {
  int num_active;
};

// get OBC's one and only telemetry: temperature
void get_obc_temp(float *temp);

// record telemetry for OBC and request telemetry from Adcs
int record_telemetry(ProcessData *proc, void *arg);

// callback for Adcs process
// records Adcs telemetry received
void receive_adcs_telemetry(int socket, unsigned char cmd, void *data,
                            size_t dataLen, struct sockaddr_in *fromAddr);

// used in debugging
int sigint_handler(int signum, void *arg);
#endif