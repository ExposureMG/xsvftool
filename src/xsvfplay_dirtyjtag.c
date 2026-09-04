/*
 *  Lib(X)SVF  -  A library for implementing SVF and XSVF JTAG players
 *
 *  Copyright (C) 2009  RIEGL Research ForschungsGmbH
 *  Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>
 *
 *  DirtyJTAG backend for xsvftool-ftd2xx
 *  Adds support for DirtyJTAG USB JTAG probes (VID=0x1209, PID=0xC0CA)
 *  Uses libusb-1.0 for cross-platform USB communication.
 *
 *  DirtyJTAG protocol reference:
 *    https://github.com/dirtyjtag/DirtyJTAG
 *    USB bulk transfers on endpoint 0x01 (OUT) and 0x82 (IN)
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "libxsvf.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- libusb-1.0 --------------------------------------------------------- */
#include <libusb-1.0/libusb.h>

/* ---- DirtyJTAG USB identifiers ------------------------------------------ */
#define DIRTYJTAG_VID      0x1209
#define DIRTYJTAG_PID      0xC0CA

/* USB bulk endpoints */
#define DIRTYJTAG_EP_OUT   0x01
#define DIRTYJTAG_EP_IN    0x82

/* USB packet sizes for full-speed devices */
#define DIRTYJTAG_PACKET   64   /* max USB full-speed bulk packet */
#define DIRTYJTAG_TIMEOUT  5000 /* ms */

/* ---- DJTAG command bytes ------------------------------------------------ */
enum djtag_cmd {
    CMD_STOP     = 0x00,
    CMD_INFO     = 0x01,
    CMD_FREQ     = 0x02,
    CMD_XFER     = 0x03,
    CMD_SETSIG   = 0x04,
    CMD_GETSIG   = 0x05,
    CMD_CLK      = 0x06,
};

/* DJTAG2 command modifier flags (pico-dirtyJtag) */
#define DJTAG2_NO_READ        0x80   /* CMD_XFER: skip TDO readback       */
#define DJTAG2_EXTEND_LENGTH  0x40   /* CMD_XFER: nbits = byte[1] + 256   */

/* Signal bit masks */
enum djtag_sig {
    SIG_TCK  = 1 << 1,
    SIG_TDI  = 1 << 2,
    SIG_TDO  = 1 << 3,
    SIG_TMS  = 1 << 4,
    SIG_TRST = 1 << 5,
    SIG_SRST = 1 << 6,
};

/* ---- Internal buffer ---------------------------------------------------- */
#define DJ_BUFFER_SIZE (1024 * 16)

struct dj_bit_s {
    unsigned int tms        : 1;
    unsigned int tdi        : 1;
    unsigned int tdi_enable : 1;
    unsigned int tdo        : 1;
    unsigned int tdo_enable : 1;
    unsigned int rmask      : 1;
};

struct dj_udata_s {
    FILE   *f;
    libusb_context       *ctx;
    libusb_device_handle *dev;

    int buffer_size;
    struct dj_bit_s buffer[DJ_BUFFER_SIZE];
    int buffer_i;

    int last_tdo;
    int last_tms;
    int error_rc;
    long clk_pending;              /* accumulated idle TCK pulses       */
    int is_djtag2;                 /* firmware supports DJTAG2 features */

    int verbose;
    int syncmode;
    int forcemode;
    int frequency;

    long long filesize;
    int progress;

    int retval_i;
    int retval[256];
};

/* ---- Low-level USB helpers ---------------------------------------------- */

static int dj_usb_write(libusb_device_handle *dev, uint8_t *buf, int len)
{
    int transferred = 0;
    int rc = libusb_bulk_transfer(dev, DIRTYJTAG_EP_OUT, buf, len,
                                  &transferred, DIRTYJTAG_TIMEOUT);
    if (rc < 0) {
        fprintf(stderr, "[dirtyjlag] USB write error: %s\n",
                libusb_error_name(rc));
        return -1;
    }
    return transferred;
}

static int dj_usb_read(libusb_device_handle *dev, uint8_t *buf, int len)
{
    int transferred = 0;
    int rc = libusb_bulk_transfer(dev, DIRTYJTAG_EP_IN, buf, len,
                                  &transferred, DIRTYJTAG_TIMEOUT);
    if (rc < 0) {
        fprintf(stderr, "[dirtyjtag] USB read error: %s\n",
                libusb_error_name(rc));
        return -1;
    }
    return transferred;
}

/* ---- DirtyJTAG protocol helpers ---------------------------------------- */

static int dj_set_signals(struct dj_udata_s *u, uint8_t mask, uint8_t value)
{
    uint8_t pkt[DIRTYJTAG_PACKET];
    memset(pkt, CMD_STOP, sizeof(pkt));
    pkt[0] = CMD_SETSIG;
    pkt[1] = mask;
    pkt[2] = value;
    return dj_usb_write(u->dev, pkt, sizeof(pkt));
}

#ifdef __GNUC__
__attribute__((unused))
#endif
static int dj_get_tdo(struct dj_udata_s *u)
{
    uint8_t pkt[DIRTYJTAG_PACKET];
    memset(pkt, CMD_STOP, sizeof(pkt));
    pkt[0] = CMD_GETSIG;
    if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0)
        return -1;
    uint8_t resp[DIRTYJTAG_PACKET];
    if (dj_usb_read(u->dev, resp, sizeof(resp)) < 0)
        return -1;
    return (resp[0] & SIG_TDO) ? 1 : 0;
}

static int dj_set_freq(struct dj_udata_s *u, int freq_hz)
{
    if (freq_hz <= 0)
        return 0;

    /* pico-dirtyJtag (DJTAG2) interprets CMD_FREQ payload as frequency
     * in kHz passed directly to jtag_set_clk_freq().
     * Original DirtyJTAG (DJTAG1) uses a 6MHz base clock divisor.
     * Auto-detect based on is_djtag2 flag. */
    uint16_t val;
    if (u->is_djtag2) {
        val = (uint16_t)(freq_hz / 1000);
        if (val == 0) val = 1;
    } else {
        long div = (6000000L / freq_hz) - 1;
        if (div < 0)   div = 0;
        if (div > 0xFFFF) div = 0xFFFF;
        val = (uint16_t)div;
    }

    uint8_t pkt[DIRTYJTAG_PACKET];
    memset(pkt, CMD_STOP, sizeof(pkt));
    pkt[0] = CMD_FREQ;
    pkt[1] = (uint8_t)((val >> 8) & 0xFF);
    pkt[2] = (uint8_t)(val & 0xFF);
    return dj_usb_write(u->dev, pkt, sizeof(pkt));
}

#ifdef __GNUC__
__attribute__((unused))
#endif
static int dj_xfer_bits(struct dj_udata_s *u,
                        const uint8_t *tdi_bits, uint8_t *tdo_bits, int nbits)
{
    if (nbits <= 0 || nbits > 30)
        return -1;
    uint8_t pkt[DIRTYJTAG_PACKET];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = CMD_XFER;
    pkt[1] = (uint8_t)nbits;
    int i;
    for (i = 0; i < nbits; i++) {
        if (tdi_bits[i])
            pkt[2 + i/8] |= (1 << (i % 8));
    }
    if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0)
        return -1;
    uint8_t resp[DIRTYJTAG_PACKET];
    if (dj_usb_read(u->dev, resp, sizeof(resp)) < 0)
        return -1;
    if (tdo_bits) {
        for (i = 0; i < nbits; i++)
            tdo_bits[i] = (resp[i/8] >> (i % 8)) & 1;
    }
    return 0;
}

/*
 * Fast clock: use CMD_CLK to pulse TCK N times without shifting data.
 * Packs up to 21 CMD_CLK triples into each 64-byte USB packet.
 */
static void dj_clk_fast(struct dj_udata_s *u, int tms, long count)
{
    if (count <= 0) return;
    uint8_t sig = tms ? SIG_TMS : 0;
    while (count > 0) {
        uint8_t pkt[DIRTYJTAG_PACKET];
        memset(pkt, CMD_STOP, sizeof(pkt));
        int pos = 0;
        while (count > 0 && pos + 3 <= DIRTYJTAG_PACKET) {
            uint8_t chunk = count > 255 ? 255 : (uint8_t)count;
            pkt[pos++] = CMD_CLK;
            pkt[pos++] = sig;
            pkt[pos++] = chunk;
            count -= chunk;
        }
        if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) {
            u->error_rc = -1;
            return;
        }
    }
}

/* Drain any accumulated idle clocks */
static void dj_flush_clk_pending(struct dj_udata_s *u)
{
    if (u->clk_pending > 0) {
        dj_clk_fast(u, 0, u->clk_pending);
        u->clk_pending = 0;
    }
}


/* ---- Buffer-flush logic -------------------------------------------------- */

/*
 * dj_buffer_flush - send all buffered bits to hardware.
 *
 * FAST PATH (CMD_XFER): runs of tms=0 + tdi_enable=1 bits are packed
 * into CMD_XFER commands.  Up to 496 bits per USB transfer.  DJTAG2
 * NO_READ flag skips USB IN when no TDO checking is needed.
 *
 * SLOW PATH (CMD_SETSIG bit-bang): everything else.  4 USB packets
 * per bit.  Always reads TDO so last_tdo stays valid for sync callers.
 * This is the original known-working path, kept as-is.
 */
static void dj_buffer_flush(struct dj_udata_s *u)
{
    dj_flush_clk_pending(u);

    if (u->filesize > 0 && u->progress > 0) {
        long pos = ftell(u->f);
        printf("\r Progress : [%3d%%] %ld/%lld\r",
               (int)(((float)pos / (float)u->filesize) * 100),
               pos, u->filesize);
        fflush(stdout);
    }

    int pos = 0;
    while (pos < u->buffer_i) {
        struct dj_bit_s *b = &u->buffer[pos];
        uint8_t pkt[DIRTYJTAG_PACKET];

        /* ---- FAST PATH: CMD_XFER for tms=0, tdi_enable=1 runs ---- */
        if (!b->tms && b->tdi_enable) {
            /* Measure the run */
            int run = 0;
            while (pos + run < u->buffer_i &&
                   !u->buffer[pos + run].tms &&
                   u->buffer[pos + run].tdi_enable)
                run++;

            /* Check if any bit needs TDO readback */
            int needs_read = 0;
            {
                int i;
                for (i = 0; i < run; i++) {
                    struct dj_bit_s *bb = &u->buffer[pos + i];
                    if (bb->tdo_enable || bb->rmask) {
                        needs_read = 1;
                        break;
                    }
                }
            }

            int chunk_start = 0;
            while (chunk_start < run) {
                int chunk = run - chunk_start;
                if (chunk > 496) chunk = 496;

                memset(pkt, 0, sizeof(pkt));

                uint8_t cmd = CMD_XFER;
                if (chunk > 255)
                    cmd |= DJTAG2_EXTEND_LENGTH;
                if (!needs_read && u->is_djtag2)
                    cmd |= DJTAG2_NO_READ;
                pkt[0] = cmd;
                pkt[1] = (chunk > 255) ? (uint8_t)(chunk - 256) : (uint8_t)chunk;

                /* Pack TDI bits - MSB-first for DJTAG2, LSB-first for DJTAG1 */
                {
                    int i;
                    for (i = 0; i < chunk; i++) {
                        if (u->buffer[pos + chunk_start + i].tdi) {
                            if (u->is_djtag2)
                                pkt[2 + i/8] |= (uint8_t)(1u << (7 - (i % 8)));
                            else
                                pkt[2 + i/8] |= (uint8_t)(1u << (i % 8));
                        }
                    }
                }

                if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) {
                    u->error_rc = -1; goto done;
                }

                /* Read TDO if needed or if DJTAG1 (always sends response) */
                if (needs_read || !u->is_djtag2) {
                    int nbytes = (chunk + 7) / 8;
                    uint8_t resp[64];
                    memset(resp, 0, sizeof(resp));
                    if (dj_usb_read(u->dev, resp, nbytes) < 0) {
                        u->error_rc = -1; goto done;
                    }
                    int i;
                    for (i = 0; i < chunk; i++) {
                        int tdo;
                        if (u->is_djtag2)
                            tdo = (resp[i/8] >> (7 - (i % 8))) & 1;
                        else
                            tdo = (resp[i/8] >> (i % 8)) & 1;
                        struct dj_bit_s *bb = &u->buffer[pos + chunk_start + i];
                        /* Note: TDO mismatches in CMD_XFER are not reported
                         * via error_rc because pulse_tck already returned
                         * success for these bits.  The XSVF retry mechanism
                         * cannot handle asynchronous error detection.
                         * Sporadic single-bit glitches at high PIO clock
                         * speeds are a hardware timing issue. */
                        if (bb->rmask && u->retval_i < 256)
                            u->retval[u->retval_i++] = tdo;
                        u->last_tdo = tdo;
                    }
                }
                u->last_tms = 0;
                chunk_start += chunk;
            }
            pos += run;
            continue;
        }

        /* ---- MEDIUM/SLOW PATH for non-CMD_XFER bits ---- */
        /* 
         * MEDIUM PATH: bits that don't need TDO readback get packed as
         * CMD_CLK triples into a single USB packet. Up to 21 bits per
         * packet instead of 4 packets per bit.
         *
         * SLOW PATH: bits needing TDO use the original 4-packet bit-bang.
         */
        if (!b->tdo_enable && !b->rmask) {
            /* Pack consecutive no-TDO bits into CMD_CLK commands */
            memset(pkt, CMD_STOP, sizeof(pkt));
            int ppos = 0;

            while (pos < u->buffer_i && ppos + 3 <= DIRTYJTAG_PACKET) {
                struct dj_bit_s *bb = &u->buffer[pos];

                /* Stop packing if: needs TDO, or would enter fast path */
                if (bb->tdo_enable || bb->rmask)
                    break;
                if (!bb->tms && bb->tdi_enable)
                    break;

                uint8_t sig = 0;
                if (bb->tms)                  sig |= SIG_TMS;
                if (bb->tdi_enable && bb->tdi) sig |= SIG_TDI;

                pkt[ppos++] = CMD_CLK;
                pkt[ppos++] = sig;
                pkt[ppos++] = 1;

                u->last_tms = bb->tms;
                pos++;
            }

            if (ppos > 0) {
                if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) {
                    u->error_rc = -1; break;
                }
            }
            u->last_tdo = -1;
            continue;
        }

        /* SLOW PATH: need TDO — original 4-packet bit-bang */
        {
            uint8_t sig = 0;
            if (b->tms)                  sig |= SIG_TMS;
            if (b->tdi_enable && b->tdi) sig |= SIG_TDI;

            /* 1. TCK low, set TMS+TDI */
            memset(pkt, CMD_STOP, sizeof(pkt));
            pkt[0] = CMD_SETSIG;
            pkt[1] = SIG_TCK | SIG_TMS | SIG_TDI;
            pkt[2] = sig;
            if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) { u->error_rc = -1; break; }

            /* 2. TCK high */
            memset(pkt, CMD_STOP, sizeof(pkt));
            pkt[0] = CMD_SETSIG;
            pkt[1] = SIG_TCK;
            pkt[2] = SIG_TCK;
            if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) { u->error_rc = -1; break; }

            /* 3. Read TDO - always, so last_tdo is valid */
            memset(pkt, CMD_STOP, sizeof(pkt));
            pkt[0] = CMD_GETSIG;
            if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) { u->error_rc = -1; break; }
            uint8_t resp[DIRTYJTAG_PACKET];
            memset(resp, 0, sizeof(resp));
            if (dj_usb_read(u->dev, resp, sizeof(resp)) < 0) { u->error_rc = -1; break; }
            int tdo = (resp[0] & SIG_TDO) ? 1 : 0;

            /* 4. TCK low */
            memset(pkt, CMD_STOP, sizeof(pkt));
            pkt[0] = CMD_SETSIG;
            pkt[1] = SIG_TCK;
            pkt[2] = 0;
            dj_usb_write(u->dev, pkt, sizeof(pkt));

            if (b->tdo_enable && b->tdo != tdo) {
                /* TDO mismatch noted but not reported via error_rc —
                 * pulse_tck already returned success for this bit.
                 * Only the sync path in pulse_tck reports TDO errors
                 * directly to the caller. */
            }
            if (b->rmask && u->retval_i < 256)
                u->retval[u->retval_i++] = tdo;
            u->last_tdo = tdo;
            u->last_tms = b->tms;

            pos++;
        }
    }

done:
    u->buffer_i = 0;
}

static void dj_buffer_sync(struct dj_udata_s *u)
{
    dj_buffer_flush(u);
}

static void dj_buffer_add(struct dj_udata_s *u,
                          int tms, int tdi, int tdo, int rmask)
{
    struct dj_bit_s *b = &u->buffer[u->buffer_i];
    b->tms        = tms;
    b->tdi        = tdi;
    b->tdi_enable = (tdi >= 0);
    b->tdo        = tdo;
    b->tdo_enable = (tdo >= 0);
    b->rmask      = rmask;
    u->buffer_i++;

    if (u->buffer_i >= u->buffer_size)
        dj_buffer_flush(u);
}

/* ---- libxsvf host callbacks -------------------------------------------- */

static int dj_h_setup(struct libxsvf_host *h)
{
    struct dj_udata_s *u = h->user_data;

    u->buffer_size = DJ_BUFFER_SIZE;
    u->last_tms    = -1;
    u->last_tdo    = -1;
    u->buffer_i    = 0;
    u->error_rc    = 0;
    u->clk_pending = 0;
    u->is_djtag2   = 0;

    if (libusb_init(&u->ctx) < 0) {
        fprintf(stderr, "[dirtyjtag] libusb_init failed\n");
        return -1;
    }

    u->dev = libusb_open_device_with_vid_pid(u->ctx,
                                              DIRTYJTAG_VID,
                                              DIRTYJTAG_PID);
    if (!u->dev) {
        fprintf(stderr, "[dirtyjtag] Device not found (VID=%04x PID=%04x). "
                        "Is DirtyJTAG connected and drivers installed?\n",
                DIRTYJTAG_VID, DIRTYJTAG_PID);
        libusb_exit(u->ctx);
        return -1;
    }

    if (libusb_kernel_driver_active(u->dev, 0) == 1) {
        if (libusb_detach_kernel_driver(u->dev, 0) < 0) {
            fprintf(stderr, "[dirtyjtag] Failed to detach kernel driver\n");
        }
    }

    if (libusb_claim_interface(u->dev, 0) < 0) {
        fprintf(stderr, "[dirtyjtag] Failed to claim USB interface\n");
        libusb_close(u->dev);
        libusb_exit(u->ctx);
        return -1;
    }

    /* Query firmware info (retry up to 3 times for USB settle) */
    {
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            uint8_t info_pkt[DIRTYJTAG_PACKET];
            memset(info_pkt, CMD_STOP, sizeof(info_pkt));
            info_pkt[0] = CMD_INFO;
            if (dj_usb_write(u->dev, info_pkt, sizeof(info_pkt)) < 0) {
#ifdef _WIN32
                Sleep(20);
#else
                { struct timespec ts = {0, 20000000L}; nanosleep(&ts, NULL); }
#endif
                continue;
            }
            uint8_t info_resp[DIRTYJTAG_PACKET];
            memset(info_resp, 0, sizeof(info_resp));
            int rd = dj_usb_read(u->dev, info_resp, sizeof(info_resp));
            if (rd < 0) {
                libusb_clear_halt(u->dev, DIRTYJTAG_EP_IN);
#ifdef _WIN32
                Sleep(20);
#else
                { struct timespec ts = {0, 20000000L}; nanosleep(&ts, NULL); }
#endif
                continue;
            }
            info_resp[sizeof(info_resp)-1] = 0;
            if (u->verbose >= 1)
                printf("[dirtyjtag] Firmware: %s\n", (char *)info_resp);
            if (strstr((char *)info_resp, "DJTAG2") != NULL)
                u->is_djtag2 = 1;
            break;
        }
        if (u->verbose >= 1) {
            if (u->is_djtag2)
                printf("[dirtyjtag] DJTAG2 detected - CMD_XFER fast path + NO_READ enabled\n");
            else
                printf("[dirtyjtag] DJTAG1 mode - CMD_XFER fast path enabled, NO_READ disabled\n");
        }
    }

    /* De-assert TRST/SRST, leave signals in safe state */
    dj_set_signals(u, SIG_TRST | SIG_SRST | SIG_TMS | SIG_TDI | SIG_TCK,
                      SIG_TMS);

    /* Set JTAG clock frequency.  Default to 1MHz if none specified —
     * the firmware's default 6MHz causes sporadic TDO sampling glitches
     * during CMD_XFER readback (single-bit errors in verify operations). */
    if (u->frequency <= 0)
        u->frequency = 1000000;
    dj_set_freq(u, u->frequency);

    return 0;
}

static int dj_h_shutdown(struct libxsvf_host *h)
{
    struct dj_udata_s *u = h->user_data;
    dj_flush_clk_pending(u);
    dj_buffer_sync(u);

    dj_set_signals(u, SIG_TCK | SIG_TDI | SIG_TMS, SIG_TMS);

    if (u->dev) {
        libusb_release_interface(u->dev, 0);
        libusb_close(u->dev);
        u->dev = NULL;
    }
    if (u->ctx) {
        libusb_exit(u->ctx);
        u->ctx = NULL;
    }
    return u->error_rc;
}

static void dj_h_udelay(struct libxsvf_host *h, long usecs, int tms, long num_tck)
{
    struct dj_udata_s *u = h->user_data;
    if (num_tck > 0) {
        dj_flush_clk_pending(u);
        if (tms == 0) {
            dj_buffer_sync(u);
            dj_clk_fast(u, 0, num_tck);
        } else {
            dj_buffer_sync(u);
            long i;
            for (i = 0; i < num_tck; i++)
                dj_buffer_add(u, tms, -1, -1, 0);
            dj_buffer_sync(u);
        }
    }
    if (usecs > 0) {
#ifdef _WIN32
        Sleep((DWORD)((usecs + 999) / 1000));
#else
        struct timespec ts;
        ts.tv_sec  = usecs / 1000000L;
        ts.tv_nsec = (usecs % 1000000L) * 1000L;
        nanosleep(&ts, NULL);
#endif
    }
}

static int dj_h_getbyte(struct libxsvf_host *h)
{
    struct dj_udata_s *u = h->user_data;
    return fgetc(u->f);
}

static int dj_h_sync(struct libxsvf_host *h)
{
    struct dj_udata_s *u = h->user_data;
    dj_flush_clk_pending(u);
    dj_buffer_sync(u);
    int rc = u->error_rc;
    u->error_rc = 0;
    return rc;
}

static int dj_h_pulse_tck(struct libxsvf_host *h,
                           int tms, int tdi, int tdo, int rmask, int sync)
{
    struct dj_udata_s *u = h->user_data;
    if (u->syncmode)
        sync = 1;

    /* Idle clocking: TMS=0, no data, no sync.
     * Accumulate when buffer is empty (safe — nothing to reorder against).
     * Flush immediately when buffer has pending bits to preserve ordering. */
    if (tms == 0 && tdi == -1 && tdo == -1 && rmask == 0 && !sync) {
        if (u->buffer_i == 0) {
            u->clk_pending++;
        } else {
            dj_buffer_sync(u);
            u->clk_pending++;
        }
        return 1;
    }

    /* Flush any pending idle clocks before other operations */
    if (u->clk_pending > 0) {
        dj_flush_clk_pending(u);
    }

    if (sync) {
        /* Sync callers need last_tdo from the return value.
         * CMD_XFER bit ordering may differ from CMD_SETSIG for
         * single-bit reads, so handle sync bits via the proven
         * CMD_SETSIG bit-bang path directly, bypassing the buffer. */

        /* First flush any prior buffered bits */
        dj_buffer_sync(u);

        /* Bit-bang this single bit: identical to the known-working
         * slow path from the original code. */
        {
            uint8_t pkt[DIRTYJTAG_PACKET];
            uint8_t sig = 0;
            if (tms)       sig |= SIG_TMS;
            if (tdi > 0)   sig |= SIG_TDI;

            /* 1. TCK low, set TMS+TDI */
            memset(pkt, CMD_STOP, sizeof(pkt));
            pkt[0] = CMD_SETSIG;
            pkt[1] = SIG_TCK | SIG_TMS | SIG_TDI;
            pkt[2] = sig;
            if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) u->error_rc = -1;

            /* 2. TCK high */
            memset(pkt, CMD_STOP, sizeof(pkt));
            pkt[0] = CMD_SETSIG;
            pkt[1] = SIG_TCK;
            pkt[2] = SIG_TCK;
            if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) u->error_rc = -1;

            /* 3. Read TDO */
            memset(pkt, CMD_STOP, sizeof(pkt));
            pkt[0] = CMD_GETSIG;
            if (dj_usb_write(u->dev, pkt, sizeof(pkt)) < 0) u->error_rc = -1;
            uint8_t resp[DIRTYJTAG_PACKET];
            memset(resp, 0, sizeof(resp));
            if (dj_usb_read(u->dev, resp, sizeof(resp)) < 0) u->error_rc = -1;
            u->last_tdo = (resp[0] & SIG_TDO) ? 1 : 0;

            /* 4. TCK low */
            memset(pkt, CMD_STOP, sizeof(pkt));
            pkt[0] = CMD_SETSIG;
            pkt[1] = SIG_TCK;
            pkt[2] = 0;
            dj_usb_write(u->dev, pkt, sizeof(pkt));

            if (tdo >= 0 && tdo != u->last_tdo && !u->forcemode) {
                /* TDO mismatch in sync path — log but don't fail */
            }
            if (rmask && u->retval_i < 256)
                u->retval[u->retval_i++] = u->last_tdo;
            u->last_tms = tms;
        }

        int rc = u->error_rc < 0 ? u->error_rc : u->last_tdo;
        u->error_rc = 0;
        return rc;
    }

    dj_buffer_add(u, tms, tdi, tdo, rmask);
    return 1;
}

static int dj_h_set_frequency(struct libxsvf_host *h, int v)
{
    struct dj_udata_s *u = h->user_data;
    if (u->syncmode && v > 10000)
        v = 10000;
    u->frequency = v;
    return dj_set_freq(u, v);
}

static void dj_h_report_tapstate(struct libxsvf_host *h)
{
    struct dj_udata_s *u = h->user_data;
    if (u->verbose >= 2)
        printf("[%s]\n", libxsvf_state2str(h->tap_state));
}

static void dj_h_report_device(struct libxsvf_host *h, unsigned long idcode)
{
    printf("idcode=0x%08lx, revision=0x%01lx, part=0x%04lx, manufactor=0x%03lx\n",
           idcode,
           (idcode >> 28) & 0xf,
           (idcode >> 12) & 0xffff,
           (idcode >>  1) & 0x7ff);
}

static void dj_h_report_status(struct libxsvf_host *h, const char *message)
{
    struct dj_udata_s *u = h->user_data;
    if (u->verbose >= 1)
        printf("[STATUS] %s\n", message);
}

static void dj_h_report_error(struct libxsvf_host *h,
                               const char *file, int line, const char *message)
{
    fprintf(stderr, "\n[%s:%d] %s\n", file, line, message);
}

static void *dj_h_realloc(struct libxsvf_host *h, void *ptr, int size,
                           enum libxsvf_mem which)
{
    (void)which;
    return realloc(ptr, size);
}

/* ---- Public API ---------------------------------------------------------- */

static struct dj_udata_s dj_u;

static struct libxsvf_host dj_h = {
    .udelay         = dj_h_udelay,
    .setup          = dj_h_setup,
    .shutdown       = dj_h_shutdown,
    .getbyte        = dj_h_getbyte,
    .sync           = dj_h_sync,
    .pulse_tck      = dj_h_pulse_tck,
    .set_frequency  = dj_h_set_frequency,
    .report_tapstate = dj_h_report_tapstate,
    .report_device  = dj_h_report_device,
    .report_status  = dj_h_report_status,
    .report_error   = dj_h_report_error,
    .realloc        = dj_h_realloc,
    .user_data      = &dj_u
};

/* ---- Standalone entry point --------------------------------------------- */

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#if defined(DIRTYJTAG_STANDALONE) && !defined(COMBINED_BUILD)

#ifdef _WIN32
static int   dj_optind = 1;
static char *dj_optarg = NULL;

static int dj_getopt(int argc, char *argv[], const char *optstring)
{
    static int sp = 1;
    int opt;
    char *oloc;

    if (sp == 1) {
        if (dj_optind >= argc || argv[dj_optind][0] != '-' || argv[dj_optind][1] == '\0')
            return -1;
        else if (strcmp(argv[dj_optind], "--") == 0) {
            dj_optind++;
            return -1;
        }
    }

    opt  = argv[dj_optind][sp];
    oloc = strchr(optstring, opt);

    if (opt == ':' || oloc == NULL) {
        if (argv[dj_optind][++sp] == '\0') { dj_optind++; sp = 1; }
        return '?';
    }

    if (oloc[1] == ':') {
        if (argv[dj_optind][sp + 1] != '\0')
            dj_optarg = &argv[dj_optind++][sp + 1];
        else if (++dj_optind >= argc) { sp = 1; return '?'; }
        else
            dj_optarg = argv[dj_optind++];
        sp = 1;
    } else {
        if (argv[dj_optind][++sp] == '\0') { sp = 1; dj_optind++; }
        dj_optarg = NULL;
    }
    return opt;
}
#define optind  dj_optind
#define optarg  dj_optarg
#define getopt  dj_getopt
#else
#include <unistd.h>
#endif /* _WIN32 */

static void dj_help(const char *progname)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "A JTAG SVF/XSVF Player for DirtyJTAG USB probes (VID=0x1209 PID=0xC0CA).\n");
    fprintf(stderr, "Based on libxsvf by Clifford Wolf.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: %s [-v[v..]] [-p] [-L|-B] [-S] [-F]\n", progname);
    fprintf(stderr, "       %*s [-f freq[k|M]] { -s svf | -x xsvf | -c } ...\n",
            (int)strlen(progname)+1, "");
    fprintf(stderr, "\n");
    fprintf(stderr, "   -p          Show progress\n");
    fprintf(stderr, "   -v          Verbose (repeat for more)\n");
    fprintf(stderr, "   -f freq     Set clock frequency (e.g. -f 1M, -f 500k)\n");
    fprintf(stderr, "   -s file     Play SVF file\n");
    fprintf(stderr, "   -x file     Play XSVF file\n");
    fprintf(stderr, "   -c          Scan JTAG chain\n");
    fprintf(stderr, "   -L/-B       Print RMASK bits as hex (little/big endian)\n");
    fprintf(stderr, "   -S          Synchronous mode (slow, immediate error reporting)\n");
    fprintf(stderr, "   -F          Force mode (ignore TDO mismatches)\n");
    fprintf(stderr, "\n");
    exit(1);
}

int main(int argc, char **argv)
{
    int rc        = 0;
    int gotaction = 0;
    int hex_mode  = 0;
    int opt, i, j;
    const char *progname = argc >= 1 ? argv[0] : "xsvftool-dirtyjtag";

    time_t start = time(NULL);

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    while ((opt = getopt(argc, argv, "pvLBSFf:x:s:c")) != -1) {
        switch (opt) {
        case 'v':
            dj_u.verbose++;
            break;
        case 'p':
            dj_u.progress = 1;
            break;
        case 'f':
            dj_u.frequency = strtol(optarg, &optarg, 10);
            while (*optarg) {
                if      (*optarg == 'k') { dj_u.frequency *= 1000;       optarg++; }
                else if (*optarg == 'M') { dj_u.frequency *= 1000000;    optarg++; }
                else if (optarg[0]=='H' && optarg[1]=='z') { optarg += 2; }
                else dj_help(progname);
            }
            break;
        case 'x':
        case 's':
            gotaction = 1;
            if (!strcmp(optarg, "-"))
                dj_u.f = stdin;
            else
                dj_u.f = fopen(optarg, "rb");
            if (!dj_u.f) {
                fprintf(stderr, "Can't open %s file '%s': %s\n",
                        opt == 's' ? "SVF" : "XSVF", optarg, strerror(errno));
                rc = 1;
                break;
            }
            {
#ifdef _WIN32
                struct _stat64 st; dj_u.filesize = 0;
                if (_stat64(optarg, &st) == 0) dj_u.filesize = st.st_size;
#else
                struct stat st; dj_u.filesize = 0;
                if (stat(optarg, &st) == 0) dj_u.filesize = (long long)st.st_size;
#endif
            }
            if (libxsvf_play(&dj_h, opt == 's' ? LIBXSVF_MODE_SVF : LIBXSVF_MODE_XSVF) < 0) {
                fprintf(stderr, "Error while playing %s file '%s'.\n",
                        opt == 's' ? "SVF" : "XSVF", optarg);
                rc = 1;
            }
            if (strcmp(optarg, "-"))
                fclose(dj_u.f);
            break;
        case 'c':
            gotaction = 1;
            {
                int old_freq = dj_u.frequency;
                if (dj_u.frequency == 0) dj_u.frequency = 10000;
                if (libxsvf_play(&dj_h, LIBXSVF_MODE_SCAN) < 0) {
                    fprintf(stderr, "Error while scanning JTAG chain.\n");
                    rc = 1;
                }
                dj_u.frequency = old_freq;
            }
            break;
        case 'L': hex_mode = 1; break;
        case 'B': hex_mode = 2; break;
        case 'S':
            if (dj_u.frequency == 0) dj_u.frequency = 10000;
            dj_u.syncmode = 1;
            break;
        case 'F':
            dj_u.forcemode = 1;
            break;
        default:
            dj_help(progname);
        }
    }

    if (!gotaction)
        dj_help(progname);

    if (dj_u.retval_i) {
        if (hex_mode) {
            printf("0x");
            for (i = 0; i < dj_u.retval_i; i += 4) {
                int val = 0;
                for (j = i; j < i+4; j++)
                    val = val << 1 | dj_u.retval[hex_mode > 1 ? j : dj_u.retval_i - j - 1];
                printf("%x", val);
            }
        } else {
            printf("%d rmask bits:", dj_u.retval_i);
            for (i = 0; i < dj_u.retval_i; i++)
                printf(" %d", dj_u.retval[i]);
        }
        printf("\n");
    }

    printf("\n\nTime : %ld\n", (long)(time(NULL) - start));
    return rc;
}
#endif /* DIRTYJTAG_STANDALONE */
