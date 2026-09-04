/*
 *  xsvftool.h – C Library Interface for xsvftool
 *
 *  Copyright (C) 2009  RIEGL Research ForschungsGmbH
 *  Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>
 *  Windows port + DirtyJTAG backend by Pheeeeenom (Mena).
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

#ifndef XSVFTOOL_H
#define XSVFTOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run xsvftool using command-line formatted argument vector (e.g. argc, argv).
 * Returns 0 on success, non-zero on error.
 */
int xsvftool_run(int argc, char **argv);

/*
 * High-level helper functions for playing SVF/XSVF files or scanning JTAG chain.
 *
 * backend: "FTDI" (or NULL for default FTDI) or "DirtyJTAG" / "D"
 * frequency_hz: Clock frequency in Hz (e.g. 1000000 for 1MHz, 0 for default)
 */
int xsvftool_play_xsvf(const char *filename, const char *backend, int frequency_hz);
int xsvftool_play_svf(const char *filename, const char *backend, int frequency_hz);
int xsvftool_scan_chain(const char *backend, int frequency_hz);

#ifdef __cplusplus
}
#endif

#endif /* XSVFTOOL_H */
