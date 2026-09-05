/*
 *  Lib(X)SVF  -  A library for implementing SVF and XSVF JTAG players
 *  libftdi1 Port
 *
 *  Copyright (C) 2009  RIEGL Research ForschungsGmbH
 *  Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>
 */

#include "libxsvf.h"

#define BUFFER_SIZE (1024 * 16)
#define BLOCK_WRITE

#include <assert.h>
#include <errno.h>
#include <ftdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <math.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

char jtag_port_name[256] = "FTDI SPARTAN6 B";
int jtag_port_pos = -1;

struct read_job_s;
struct udata_s;
struct buffer_s;

typedef void job_handler_t(struct udata_s *u, struct read_job_s *job,
                           unsigned char *data);

struct read_job_s {
  struct read_job_s *next;
  int data_len, bits_len;
  struct buffer_s *buffer;
  job_handler_t *handler;
  unsigned int command_id;
};

struct buffer_s {
  unsigned int tms : 1;
  unsigned int tdi : 1;
  unsigned int tdi_enable : 1;
  unsigned int tdo : 1;
  unsigned int tdo_enable : 1;
  unsigned int rmask : 1;
};

struct udata_s {
  FILE *f;
  struct ftdi_context *ftdic;
  int buffer_size;
  struct buffer_s buffer[BUFFER_SIZE];
  struct read_job_s *job_fifo_out, *job_fifo_in;
  int last_tms;
  int last_tdo;
  int buffer_i;
  int retval_i;
  int retval[256];
  int error_rc;
  int verbose;
  int syncmode;
  int forcemode;
  int frequency;
#ifdef BLOCK_WRITE
  int ftdibuf_len;
  unsigned char ftdibuf[4096];
#endif
  int64_t filesize;
  int progress;
  unsigned PID;
  unsigned VID;
};

static FILE *dumpfile = NULL;

static void write_dumpfile(int wr, unsigned char *buf, int size,
                           unsigned int command_id) {
  int i;
  if (!dumpfile)
    return;
  fprintf(dumpfile, "%s[%u] %04x:", wr ? "SEND" : "RECV", command_id, size);
  for (i = 0; i < size; i++)
    fprintf(dumpfile, " %02x", buf[i]);
  fprintf(dumpfile, "\n");
}

static int my_ftdi_read_data(struct ftdi_context *ftdi, unsigned char *buf,
                             int size, unsigned int command_id) {
  int pos = 0;
  int poll_count = 0;
  while (pos < size) {
    int r = ftdi_read_data(ftdi, buf + pos, size - pos);
    if (r < 0) {
      fprintf(stderr, "[***] ftdi_read_data returned error (rc=%d).\n", r);
      break;
    }
    if (r == 0) {
      if (++poll_count > 8) {
        fprintf(stderr,
                "[***] my_ftdi_read_data gives up polling <id=%u, pos=%u, "
                "size=%u>.\n",
                command_id, pos, size);
        break;
      }
      usleep(4096 << poll_count);
    } else {
      pos += r;
    }
  }
  write_dumpfile(0, buf, pos, command_id);
  return pos;
}

static int my_ftdi_write_data(struct udata_s *u, unsigned char *buf, int size,
                              int sync) {
#ifdef BLOCK_WRITE
  int total_queued = 0;
  sync = 1;

  while (size > 0) {
    if (u->ftdibuf_len == 4096) {
      if (dumpfile)
        fprintf(dumpfile, "WRITE %d BYTES (buffer full)\n", u->ftdibuf_len);
      int w = ftdi_write_data(u->ftdic, u->ftdibuf, u->ftdibuf_len);
      if (w != u->ftdibuf_len)
        return -1;
      u->ftdibuf_len = 0;
    }

    int chunksize = 4096 - u->ftdibuf_len;
    if (chunksize > size)
      chunksize = size;

    memcpy(u->ftdibuf + u->ftdibuf_len, buf, chunksize);
    u->ftdibuf_len += chunksize;
    total_queued += chunksize;
    size -= chunksize;
    buf += chunksize;
  }

  if (sync && u->ftdibuf_len > 0) {
    if (dumpfile)
      fprintf(dumpfile, "WRITE %d BYTES (sync)\n", u->ftdibuf_len);
    int w = ftdi_write_data(u->ftdic, u->ftdibuf, u->ftdibuf_len);
    if (w != u->ftdibuf_len)
      return -1;
    u->ftdibuf_len = 0;
  }

  return total_queued;
#else
  int w = ftdi_write_data(u->ftdic, buf, size);
  return w;
#endif
}

static struct read_job_s *new_read_job(struct udata_s *u, int data_len,
                                       int bits_len, struct buffer_s *buffer,
                                       job_handler_t *handler) {
  struct read_job_s *job = calloc(1, sizeof(struct read_job_s));
  static unsigned int command_count = 0;

  job->data_len = data_len;
  job->bits_len = bits_len;
  job->buffer = calloc(bits_len, sizeof(struct buffer_s));
  memcpy(job->buffer, buffer, bits_len * sizeof(struct buffer_s));
  job->handler = handler;
  job->command_id = command_count++;

  if (u->job_fifo_in)
    u->job_fifo_in->next = job;
  if (!u->job_fifo_out)
    u->job_fifo_out = job;
  u->job_fifo_in = job;

  return job;
}

static void transfer_tms_job_handler(struct udata_s *u, struct read_job_s *job,
                                     unsigned char *data) {
  int i;
  for (i = 0; i < job->bits_len; i++) {
    int bit = (data[i / 8] & (1 << (i % 8))) != 0;
    if (job->buffer[i].rmask) {
      if (u->retval_i < 256)
        u->retval[u->retval_i++] = bit;
    }
    if (job->buffer[i].tdo_enable && bit != job->buffer[i].tdo) {
      if (u->verbose >= 1)
        printf("[***] TDO mismatch: read %d, expected %d at bit %d in command "
               "%d.\n",
               bit, job->buffer[i].tdo, i, job->command_id);
      if (!u->forcemode)
        u->error_rc = -1;
    }
    u->last_tdo = bit;
  }
}

static void transfer_tdi_job_handler(struct udata_s *u, struct read_job_s *job,
                                     unsigned char *data) {
  int i;
  for (i = 0; i < job->bits_len; i++) {
    int bit = (data[i / 8] & (1 << (i % 8))) != 0;
    if (job->buffer[i].rmask) {
      if (u->retval_i < 256)
        u->retval[u->retval_i++] = bit;
    }
    if (job->buffer[i].tdo_enable && bit != job->buffer[i].tdo) {
      if (u->verbose >= 1)
        printf("[***] TDO mismatch: read %d, expected %d at bit %d in command "
               "%d.\n",
               bit, job->buffer[i].tdo, i, job->command_id);
      if (!u->forcemode)
        u->error_rc = -1;
    }
    u->last_tdo = bit;
  }
}

static void buffer_flush(struct udata_s *u) {
  int i, j, data_len, bits_len;
  unsigned char cmd[4096];
  unsigned char *rxdata;
  int cmd_i = 0;
  struct buffer_s *b;
  struct read_job_s *job;

  for (i = 0; i < u->buffer_i; i = j) {
    for (j = i; j < u->buffer_i; j++) {
      if (u->buffer[i].tdi_enable != u->buffer[j].tdi_enable)
        break;
      if (u->buffer[i].tdo_enable != u->buffer[j].tdo_enable)
        break;
      if (u->buffer[i].rmask != u->buffer[j].rmask)
        break;
    }

    b = u->buffer + i;
    bits_len = j - i;
    data_len = (bits_len + 7) / 8;

    if (b->tdi_enable) {
      cmd[cmd_i++] = (b->tdo_enable || b->rmask) ? 0x3b : 0x1b;
      cmd[cmd_i++] = bits_len - 1;
      cmd[cmd_i] = 0;

      for (j = 0; j < bits_len; j++) {
        if (u->buffer[i + j].tdi)
          cmd[cmd_i] |= 1 << (j % 8);
        if (j % 8 == 7 || j == bits_len - 1)
          cmd_i++;
      }

      if (b->tdo_enable || b->rmask)
        new_read_job(u, data_len, bits_len, u->buffer + i,
                     transfer_tdi_job_handler);
    } else {
      cmd[cmd_i++] = (b->tdo_enable || b->rmask) ? 0x6e : 0x4b;
      cmd[cmd_i++] = bits_len - 1;
      cmd[cmd_i] = 0;

      for (j = 0; j < bits_len; j++) {
        if (u->buffer[i + j].tms)
          cmd[cmd_i] |= 1 << (j % 8);
        if (j % 8 == 7 || j == bits_len - 1)
          cmd_i++;
      }

      if (b->tdo_enable || b->rmask)
        new_read_job(u, data_len, bits_len, u->buffer + i,
                     transfer_tms_job_handler);
    }

    u->last_tms = u->buffer[j - 1].tms;
  }

  u->buffer_i = 0;

  write_dumpfile(1, cmd, cmd_i, 0);
  if (my_ftdi_write_data(u, cmd, cmd_i, 0) != cmd_i) {
    fprintf(stderr, "IO Error: Short write in buffer_flush.\n");
    u->error_rc = -1;
  }

  if (u->progress && u->filesize > 0) {
    long long pos = ftell(u->f);
    int prog = (int)((pos * 100) / u->filesize);
    if (prog != u->progress) {
      u->progress = prog;
      fprintf(stderr, "\rProgress: [%3d%%]", u->progress);
      fflush(stderr);
    }
  }
}

static void buffer_sync(struct udata_s *u) {
  buffer_flush(u);

  if (!u->job_fifo_out)
    return;

  my_ftdi_write_data(u, NULL, 0, 1);

  while (u->job_fifo_out) {
    struct read_job_s *job = u->job_fifo_out;
    u->job_fifo_out = job->next;
    if (!u->job_fifo_out)
      u->job_fifo_in = NULL;

    unsigned char *rxdata = malloc(job->data_len);
    if (my_ftdi_read_data(u->ftdic, rxdata, job->data_len, job->command_id) !=
        job->data_len) {
      fprintf(stderr, "IO Error: Short read in buffer_sync.\n");
      u->error_rc = -1;
    } else {
      job->handler(u, job, rxdata);
    }

    free(rxdata);
    free(job->buffer);
    free(job);
  }
}

static void buffer_add(struct udata_s *u, int tms, int tdi, int tdo,
                       int rmask) {
  u->buffer[u->buffer_i].tms = tms;
  u->buffer[u->buffer_i].tdi = tdi;
  u->buffer[u->buffer_i].tdi_enable = tdi >= 0;
  u->buffer[u->buffer_i].tdo = tdo;
  u->buffer[u->buffer_i].tdo_enable = tdo >= 0;
  u->buffer[u->buffer_i].rmask = rmask;
  u->buffer_i++;

  if (u->buffer_i >= u->buffer_size)
    buffer_flush(u);
}

#define MAX_DEVICES 32

void listFTDI(void) {
  struct ftdi_context *ftdi = ftdi_new();
  if (!ftdi)
    return;

  struct ftdi_device_list *devlist = NULL;
  int count = ftdi_usb_find_all(ftdi, &devlist, 0x0403, 0);
  if (count < 0) {
    fprintf(stderr, "Error finding FTDI devices: %s\n",
            ftdi_get_error_string(ftdi));
    ftdi_free(ftdi);
    return;
  }

  printf("%d devices found\n", count);
  struct ftdi_device_list *curdev = devlist;
  int i = 0;
  while (curdev) {
    char manufacturer[128] = {0};
    char description[128] = {0};
    char serial[128] = {0};
    ftdi_usb_get_strings2(ftdi, curdev->dev, manufacturer, 128, description,
                          128, serial, 128);
    printf("Device %d - Description: '%s', Serial: '%s'\n", i++, description,
           serial);
    curdev = curdev->next;
  }

  ftdi_list_free(&devlist);
  ftdi_free(ftdi);
}

static int h_setup(struct libxsvf_host *h) {
  struct udata_s *u = h->user_data;
  u->buffer_size = BUFFER_SIZE;
#ifdef BLOCK_WRITE
  u->ftdibuf_len = 0;
#endif

  u->ftdic = ftdi_new();
  if (!u->ftdic) {
    fprintf(stderr, "Failed to allocate libftdi context\n");
    return -1;
  }

  enum ftdi_interface interface = INTERFACE_A;
  if (strstr(jtag_port_name, " B") || strstr(jtag_port_name, " Channel B")) {
    interface = INTERFACE_B;
  }
  ftdi_set_interface(u->ftdic, interface);
  u->ftdic->module_detach_mode = AUTO_DETACH_SIO_MODULE;

  int vid = u->VID ? u->VID : 0x0403;
  int pid = u->PID ? u->PID : 0x6010;
  int ret = ftdi_usb_open(u->ftdic, vid, pid);
  if (ret < 0) {
    // Fallback to 0x6011 (FT4232H) or 0x6001 (FT232R)
    if (ftdi_usb_open(u->ftdic, vid, 0x6011) < 0 &&
        ftdi_usb_open(u->ftdic, vid, 0x6001) < 0) {
      fprintf(stderr, "Failed to Open FTDI JTAG Interface (%s): %s (%d)\n",
              jtag_port_name, ftdi_get_error_string(u->ftdic), ret);
      ftdi_free(u->ftdic);
      u->ftdic = NULL;
      return -1;
    }
  }

  if (ftdi_usb_reset(u->ftdic) < 0 ||
      ftdi_set_bitmode(u->ftdic, 0x00, BITMODE_RESET) < 0 ||
      ftdi_set_latency_timer(u->ftdic, 2) < 0 ||
      ftdi_set_bitmode(u->ftdic, 0x0B, BITMODE_MPSSE) < 0) {
    fprintf(stderr, "Failed to configure FTDI MPSSE bitmode: %s\n",
            ftdi_get_error_string(u->ftdic));
    ftdi_usb_close(u->ftdic);
    ftdi_free(u->ftdic);
    u->ftdic = NULL;
    return -1;
  }

  unsigned char init_commands[] = {
      0x8B, 0x97, 0x8D, // 12Mhz internal clk
      0x86, 0x02, 0x00, // initial clk freq (2 MHz)
      0x80, 0x08, 0x1b, // initial line states
      0x85,             // disable loopback
  };

  write_dumpfile(1, init_commands, sizeof(init_commands), 0);
  if (ftdi_write_data(u->ftdic, init_commands, sizeof(init_commands)) !=
      sizeof(init_commands)) {
    fprintf(stderr, "IO Error: Interface setup failed (init commands)\n");
    return -1;
  }

  if (u->frequency > 0)
    h->set_frequency(h, u->frequency);

  u->job_fifo_out = NULL;
  u->job_fifo_in = NULL;
  u->last_tms = -1;
  u->last_tdo = -1;
  u->buffer_i = 0;
  u->error_rc = 0;

  return 0;
}

static int h_shutdown(struct libxsvf_host *h) {
  struct udata_s *u = h->user_data;
  buffer_sync(u);
  if (u->ftdic) {
    ftdi_set_bitmode(u->ftdic, 0x00, BITMODE_RESET);
    ftdi_usb_close(u->ftdic);
    ftdi_free(u->ftdic);
    u->ftdic = NULL;
  }
  return u->error_rc;
}

static void h_udelay(struct libxsvf_host *h, long usecs, int tms,
                     long num_tck) {
  struct udata_s *u = h->user_data;
  buffer_sync(u);
  if (num_tck > 0) {
    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);
    while (num_tck > 0) {
      buffer_add(u, tms, -1, -1, 0);
      num_tck--;
    }
    buffer_sync(u);
    gettimeofday(&tv2, NULL);
    if (tv2.tv_sec > tv1.tv_sec) {
      usecs -=
          (1000000 - tv1.tv_usec) + (tv2.tv_sec - tv1.tv_sec - 1) * 1000000;
      tv1.tv_usec = 0;
    }
    usecs -= tv2.tv_usec - tv1.tv_usec;
  }
  if (usecs > 0) {
    usleep(usecs);
  }
}

static int h_getbyte(struct libxsvf_host *h) {
  struct udata_s *u = h->user_data;
  return fgetc(u->f);
}

static int h_sync(struct libxsvf_host *h) {
  struct udata_s *u = h->user_data;
  buffer_sync(u);
  int rc = u->error_rc;
  u->error_rc = 0;
  return rc;
}

static int h_pulse_tck(struct libxsvf_host *h, int tms, int tdi, int tdo,
                       int rmask, int sync) {
  struct udata_s *u = h->user_data;
  if (u->syncmode)
    sync = 1;
  buffer_add(u, tms, tdi, tdo, rmask);
  if (sync) {
    buffer_sync(u);
    int rc = u->error_rc < 0 ? u->error_rc : u->last_tdo;
    u->error_rc = 0;
    return rc;
  }
  return u->error_rc < 0 ? u->error_rc : 1;
}

static int h_set_frequency(struct libxsvf_host *h, int v) {
  int rc;
  struct udata_s *u = h->user_data;
  if (u->syncmode && v > 10000)
    v = 10000;
  unsigned char sethighspeed_command[] = {0x8A, 0x97, 0x8D};
  unsigned char setfreq_command[] = {0x86, 0x02, 0x00};
  int div;
  if (v > 12e6) {
    fprintf(
        stderr,
        "Warning : Using High-Speed config, only for FT2232H and FT4232H\n");
    write_dumpfile(1, sethighspeed_command, sizeof(sethighspeed_command), 0);
    rc = my_ftdi_write_data(u, sethighspeed_command,
                            sizeof(sethighspeed_command), 1);
    if (rc != sizeof(sethighspeed_command)) {
      fprintf(stderr, "IO Error: Set frequency write failed: (rc=%d/%d)\n", rc,
              (int)sizeof(sethighspeed_command));
      u->error_rc = -1;
    }
    div = (int)fmax(ceil(60e6 / (2 * v) - 1), 0);
  } else
    div = (int)fmax(ceil(12e6 / (2 * v) - 1), 0);
  setfreq_command[1] = div >> 0;
  setfreq_command[2] = div >> 8;
  write_dumpfile(1, setfreq_command, sizeof(setfreq_command), 0);
  rc = my_ftdi_write_data(u, setfreq_command, sizeof(setfreq_command), 1);
  if (rc != sizeof(setfreq_command)) {
    fprintf(stderr, "IO Error: Set frequency write failed: (rc=%d/%d)\n", rc,
            (int)sizeof(setfreq_command));
    u->error_rc = -1;
  }
  return 0;
}

static void h_report_tapstate(struct libxsvf_host *h) {
  struct udata_s *u = h->user_data;
  if (u->verbose >= 2)
    printf("[%s]\n", libxsvf_state2str(h->tap_state));
}

static void h_report_device(struct libxsvf_host *h, unsigned long idcode) {
  printf("idcode=0x%08lx, revision=0x%01lx, part=0x%04lx, manufactor=0x%03lx\n",
         idcode, (idcode >> 28) & 0xf, (idcode >> 12) & 0xffff,
         (idcode >> 1) & 0x7ff);
}

static void h_report_status(struct libxsvf_host *h, const char *message) {
  struct udata_s *u = h->user_data;
  if (u->verbose >= 1)
    printf("[STATUS] %s\n", message);
}

static void h_report_error(struct libxsvf_host *h, const char *file, int line,
                           const char *message) {
  fprintf(stderr, "\n[%s:%d] %s\n", file, line, message);
}

static void *h_realloc(struct libxsvf_host *h, void *ptr, int size,
                       enum libxsvf_mem which) {
  return realloc(ptr, size);
}

static struct udata_s u = {};

static struct libxsvf_host h = {.udelay = h_udelay,
                                .setup = h_setup,
                                .shutdown = h_shutdown,
                                .getbyte = h_getbyte,
                                .sync = h_sync,
                                .pulse_tck = h_pulse_tck,
                                .set_frequency = h_set_frequency,
                                .report_tapstate = h_report_tapstate,
                                .report_device = h_report_device,
                                .report_status = h_report_status,
                                .report_error = h_report_error,
                                .realloc = h_realloc,
                                .user_data = &u};

#ifndef COMBINED_BUILD

const char *progname;

static void help(void) {
  fprintf(stderr, "\n");
  fprintf(stderr, "A JTAG SVF/XSVF Player based on libxsvf for FTDI FT232H, "
                  "FT2232H, FT4232H\n");
  fprintf(stderr, "libftdi1 Port\n\n");
  fprintf(stderr,
          "Usage: %s [ -v[v..] ] [ -d dumpfile ] [ -p ] [ -L | -B ] [ -S ] [ "
          "-F ] \\\n",
          progname);
  fprintf(
      stderr,
      "      %*s [ -f freq[k|M] ] { -s svf-file | -x xsvf-file | -c } ...\n",
      (int)(strlen(progname) + 1), "");
  fprintf(stderr, "\n");
  fprintf(stderr, "   -p\n          Show progress\n\n");
  fprintf(stderr, "   -v\n          Enable verbose output\n\n");
  fprintf(stderr, "   -P PID\n        PID value for USB FTDI device\n\n");
  fprintf(stderr, "   -U VID\n        VID value for USB FTDI device\n\n");
  fprintf(stderr, "   -J jtagport\n   JTAG Port Name, by default '%s'\n\n",
          jtag_port_name);
  fprintf(stderr, "   -l\n          List FTDI devices\n\n");
  fprintf(stderr,
          "   -d dumpfile\n Write a logfile of all MPSSE communication\n\n");
  fprintf(stderr, "   -L, -B\n      Print RMASK bits as hex value (little or "
                  "big endian)\n\n");
  fprintf(stderr, "   -S\n          Run in synchronous mode\n\n");
  fprintf(stderr,
          "   -F\n          Force mode (ignore all TDO mismatches)\n\n");
  fprintf(stderr, "   -f freq[k|M]\n Set maximum frequency\n\n");
  fprintf(stderr, "   -s svf-file\n  Play the specified SVF file\n\n");
  fprintf(stderr, "   -x xsvf-file\n Play the specified XSVF file\n\n");
  fprintf(stderr, "   -c\n          List devices in JTAG chain\n\n");
  exit(1);
}

int main(int argc, char **argv) {
  int rc = 0;
  int gotaction = 0;
  int hex_mode = 0;
  int opt, i, j;
  time_t start = time(NULL);

  progname = argc >= 1 ? argv[0] : "xsvftool-play";
  while ((opt = getopt(argc, argv, "pvlP:U:J:j:d:LBSFf:x:s:c")) != -1) {
    switch (opt) {
    case 'v':
      u.verbose++;
      break;
    case 'l':
      gotaction = 1;
      listFTDI();
      break;
    case 'd':
      if (!strcmp(optarg, "-"))
        dumpfile = stdout;
      else
        dumpfile = fopen(optarg, "w");
      if (!dumpfile) {
        fprintf(stderr, "Can't open dumpfile `%s': %s\n", optarg,
                strerror(errno));
        rc = 1;
      }
      break;
    case 'J':
      strncpy(jtag_port_name, optarg, 255);
      break;
    case 'j':
      jtag_port_pos = atoi(optarg);
      break;
    case 'p':
      u.progress = 1;
      break;
    case 'f':
      u.frequency = strtol(optarg, &optarg, 10);
      while (*optarg != 0) {
        if (*optarg == 'k') {
          u.frequency *= 1000;
          optarg++;
          continue;
        }
        if (*optarg == 'M') {
          u.frequency *= 1000000;
          optarg++;
          continue;
        }
        if (optarg[0] == 'H' && optarg[1] == 'z') {
          optarg += 2;
          continue;
        }
        help();
      }
      break;
    case 'x':
    case 's':
      gotaction = 1;
      if (!strcmp(optarg, "-"))
        u.f = stdin;
      else
        u.f = fopen(optarg, "rb");
      if (u.f == NULL) {
        fprintf(stderr, "Can't open %s file `%s': %s\n",
                opt == 's' ? "SVF" : "XSVF", optarg, strerror(errno));
        rc = 1;
        break;
      }
      {
        struct stat _s;
        u.filesize = 0;
        if (stat(optarg, &_s) < 0)
          fprintf(stderr, "Failed to stat file\n");
        else
          u.filesize = _s.st_size;
      }
      if (libxsvf_play(&h, opt == 's' ? LIBXSVF_MODE_SVF : LIBXSVF_MODE_XSVF) <
          0) {
        fprintf(stderr, "Error while playing %s file `%s'.\n",
                opt == 's' ? "SVF" : "XSVF", optarg);
        rc = 1;
      }
      if (strcmp(optarg, "-"))
        fclose(u.f);
      break;
    case 'c':
      gotaction = 1;
      int old_frequency = u.frequency;
      if (u.frequency == 0)
        u.frequency = 10000;
      if (libxsvf_play(&h, LIBXSVF_MODE_SCAN) < 0) {
        fprintf(stderr, "Error while scanning JTAG chain.\n");
        rc = 1;
      }
      u.frequency = old_frequency;
      break;
    case 'L':
      hex_mode = 1;
      break;
    case 'B':
      hex_mode = 2;
      break;
    case 'S':
      if (u.frequency == 0)
        u.frequency = 10000;
      u.syncmode = 1;
      break;
    case 'F':
      u.forcemode = 1;
      break;
    case 'P':
      u.PID = strtol(optarg, NULL, 0);
      break;
    case 'U':
      u.VID = strtol(optarg, NULL, 0);
      break;
    default:
      help();
      break;
    }
  }

  if (!gotaction)
    help();

  if (u.retval_i) {
    if (hex_mode) {
      printf("0x");
      for (i = 0; i < u.retval_i; i += 4) {
        int val = 0;
        for (j = i; j < i + 4; j++)
          val = val << 1 | u.retval[hex_mode > 1 ? j : u.retval_i - j - 1];
        printf("%x", val);
      }
    } else {
      printf("%d rmask bits:", u.retval_i);
      for (i = 0; i < u.retval_i; i++)
        printf(" %d", u.retval[i]);
    }
    printf("\n");
  }

  printf("\n\nTime : %ld\n", (long)(time(NULL) - start));

  return rc;
}

#endif /* COMBINED_BUILD */
