/* kplex_mods.h
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 */
struct iftypedef iftypes[] = {
    { GLOBAL, "global" , NULL, NULL },
    { FILEIO, "file", init_file, ifdup_file },
    { SERIAL, "serial", init_serial, ifdup_serial },
    { BCAST, "broadcast", init_bcast, ifdup_bcast },
    { TCP, "tcp", init_tcp, ifdup_tcp },
    { PTY, "pty", init_pty, ifdup_serial },
    { MCAST, "mcast", NULL, NULL },
    { ST, "seatalk", init_seatalk, ifdup_seatalk },
    { END, NULL, NULL, NULL },
};
