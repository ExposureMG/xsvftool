/*
 *  xsvftool.c – Combined FTDI + DirtyJTAG frontend for libxsvf
 *
 *  Copyright (C) 2009  RIEGL Research ForschungsGmbH
 *  Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>
 *  Windows port + DirtyJTAG backend by Pheeeeenom (Mena).
 *
 *  Select backend at runtime:
 *    xsvftool -A ...   FTDI (default)
 *    xsvftool -D ...   DirtyJTAG (VID=0x1209 PID=0xC0CA)
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

/*
 * COMBINED_BUILD suppresses the individual main() functions in both backend
 * files so that only the one below is compiled.  Both backends are #included
 * directly so their structs and statics are visible here without any fragile
 * re-declarations.
 */
#ifndef COMBINED_BUILD
#define COMBINED_BUILD
#endif

#include "xsvfplay_ftd2xx.c"
#include "xsvfplay_dirtyjtag.c"

/* ---- Unified getopt (Windows) ------------------------------------------- */
/* xsvfplay_ftd2xx.c already defines getopt / optind / optarg when _WIN32 is
 * set, and those definitions are now visible here via the #include above.    */

/* ---- Help ---------------------------------------------------------------- */
static void combined_help(const char *progname)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "A JTAG SVF/XSVF Player supporting FTDI and DirtyJTAG probes.\n");
    fprintf(stderr, "Based on libxsvf by Clifford Wolf. For use with J-Runner with Extras!\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: %s [backend] [options] { -s svf | -x xsvf | -c | -l } ...\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "Backend (default: FTDI):\n");
    fprintf(stderr, "   -A          FTDI FT232H / FT2232H / FT4232H (default)\n");
    fprintf(stderr, "   -D          DirtyJTAG USB probe (VID=0x1209 PID=0xC0CA)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "FTDI-only options:\n");
    fprintf(stderr, "   -J name     JTAG port name (default: 'FTDI SPARTAN6 B')\n");
    fprintf(stderr, "   -j index    JTAG port index (overrides -J)\n");
    fprintf(stderr, "   -P PID      USB PID override\n");
    fprintf(stderr, "   -U VID      USB VID override\n");
    fprintf(stderr, "   -l          List attached FTDI devices\n");
    fprintf(stderr, "   -d file     Write MPSSE communication log\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Common options:\n");
    fprintf(stderr, "   -v          Verbose (repeat for more)\n");
    fprintf(stderr, "   -p          Show progress\n");
    fprintf(stderr, "   -f freq     Clock frequency e.g. -f 1M, -f 500k\n");
    fprintf(stderr, "   -s file     Play SVF file\n");
    fprintf(stderr, "   -x file     Play XSVF file\n");
    fprintf(stderr, "   -c          Scan JTAG chain\n");
    fprintf(stderr, "   -L/-B       Print RMASK bits as hex (little/big endian)\n");
    fprintf(stderr, "   -S          Synchronous mode\n");
    fprintf(stderr, "   -F          Force mode (ignore TDO mismatches)\n");
    fprintf(stderr, "\n");
    exit(1);
}

/* ---- main --------------------------------------------------------------- */
int main(int argc, char **argv)
{
    int rc            = 0;
    int gotaction     = 0;
    int hex_mode      = 0;
    int use_dirtyjtag = 0;
    int opt, i, j;
    const char *progname = argc >= 1 ? argv[0] : "xsvftool";

    time_t start = time(NULL);

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /* Pre-scan for -A / -D so we can route all subsequent flags correctly. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0) { use_dirtyjtag = 1; break; }
        if (strcmp(argv[i], "-A") == 0) { use_dirtyjtag = 0; break; }
    }

    while ((opt = getopt(argc, argv, "ADvplLBSFf:x:s:cJ:j:d:P:U:")) != -1)
    {
        switch (opt)
        {
        case 'A': use_dirtyjtag = 0; break;
        case 'D': use_dirtyjtag = 1; break;

        case 'v':
            if (use_dirtyjtag) dj_u.verbose++; else u.verbose++;
            break;
        case 'p':
            if (use_dirtyjtag) dj_u.progress = 1; else u.progress = 1;
            break;
        case 'L': hex_mode = 1; break;
        case 'B': hex_mode = 2; break;

        case 'S':
            if (use_dirtyjtag) {
                if (!dj_u.frequency) dj_u.frequency = 10000;
                dj_u.syncmode = 1;
            } else {
                if (!u.frequency) u.frequency = 10000;
                u.syncmode = 1;
            }
            break;

        case 'F':
            if (use_dirtyjtag) dj_u.forcemode = 1; else u.forcemode = 1;
            break;

        case 'f': {
            int freq = strtol(optarg, &optarg, 10);
            while (*optarg) {
                if      (*optarg == 'k') { freq *= 1000;    optarg++; }
                else if (*optarg == 'M') { freq *= 1000000; optarg++; }
                else if (optarg[0]=='H' && optarg[1]=='z') { optarg += 2; }
                else combined_help(progname);
            }
            if (use_dirtyjtag) dj_u.frequency = freq; else u.frequency = freq;
            break;
        }

        /* FTDI-only */
        case 'J':
            if (use_dirtyjtag) fprintf(stderr, "Warning: -J ignored for DirtyJTAG\n");
            else strncpy(jtag_port_name, optarg, 255);
            break;
        case 'j':
            if (use_dirtyjtag) fprintf(stderr, "Warning: -j ignored for DirtyJTAG\n");
            else jtag_port_pos = atoi(optarg);
            break;
        case 'P':
            if (!use_dirtyjtag) u.PID = strtol(optarg, NULL, 0);
            break;
        case 'U':
            if (!use_dirtyjtag) u.VID = strtol(optarg, NULL, 0);
            break;
        case 'd':
            if (use_dirtyjtag) {
                fprintf(stderr, "Warning: -d ignored for DirtyJTAG\n");
            } else {
                if (!strcmp(optarg, "-"))
                    dumpfile = stdout;
                else
                    dumpfile = fopen(optarg, "w");
                if (!dumpfile) {
                    fprintf(stderr, "Can't open dumpfile '%s': %s\n",
                            optarg, strerror(errno));
                    rc = 1;
                }
            }
            break;
        case 'l':
            if (use_dirtyjtag) fprintf(stderr, "Warning: -l ignored for DirtyJTAG\n");
            else { gotaction = 1; listFTDI(); }
            break;

        /* Actions */
        case 'x':
        case 's': {
            gotaction = 1;
            FILE     **fp  = use_dirtyjtag ? &dj_u.f       : &u.f;
            __int64   *fsz = use_dirtyjtag ? (__int64*)&dj_u.filesize : &u.filesize;

            *fp = strcmp(optarg, "-") ? fopen(optarg, "rb") : stdin;
            if (!*fp) {
                fprintf(stderr, "Can't open %s file '%s': %s\n",
                        opt=='s' ? "SVF" : "XSVF", optarg, strerror(errno));
                rc = 1; break;
            }
            {
                struct _stat64 st; *fsz = 0;
                if (_stat64(optarg, &st) == 0) *fsz = st.st_size;
            }
            {
                struct libxsvf_host *be = use_dirtyjtag ? &dj_h : &h;
                enum libxsvf_mode mode  = (opt=='s') ? LIBXSVF_MODE_SVF
                                                     : LIBXSVF_MODE_XSVF;
                if (libxsvf_play(be, mode) < 0) {
                    fprintf(stderr, "Error while playing %s file '%s'.\n",
                            opt=='s' ? "SVF" : "XSVF", optarg);
                    rc = 1;
                }
            }
            if (strcmp(optarg, "-")) fclose(*fp);
            break;
        }

        case 'c': {
            gotaction = 1;
            struct libxsvf_host *be = use_dirtyjtag ? &dj_h : &h;
            int *freq = use_dirtyjtag ? &dj_u.frequency : &u.frequency;
            int old   = *freq;
            if (!*freq) *freq = 10000;
            if (libxsvf_play(be, LIBXSVF_MODE_SCAN) < 0) {
                fprintf(stderr, "Error while scanning JTAG chain.\n");
                rc = 1;
            }
            *freq = old;
            break;
        }

        default:
            combined_help(progname);
        }
    }

    if (!gotaction)
        combined_help(progname);

    {
        int  rv_i = use_dirtyjtag ? dj_u.retval_i : u.retval_i;
        int *rv   = use_dirtyjtag ? dj_u.retval   : u.retval;
        if (rv_i) {
            if (hex_mode) {
                printf("0x");
                for (i = 0; i < rv_i; i += 4) {
                    int val = 0;
                    for (j = i; j < i+4; j++)
                        val = val<<1 | rv[hex_mode>1 ? j : rv_i-j-1];
                    printf("%x", val);
                }
            } else {
                printf("%d rmask bits:", rv_i);
                for (i = 0; i < rv_i; i++)
                    printf(" %d", rv[i]);
            }
            printf("\n");
        }
    }

    printf("\n\nTime : %ld\n", (long)(time(NULL) - start));
    return rc;
}
