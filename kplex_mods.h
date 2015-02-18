/* kplex_mods.h
 * This file is part of kplex
 * Copyright Keith Young 2012 - 2015
 * For copying information see the file COPYING distributed with this software
 */
struct iftypedef iftypes[] = {
    { GLOBAL, "global" , NULL, NULL },
    { FILEIO, "file", init_file, ifdup_file },
    { SERIAL, "serial", init_serial, ifdup_serial },
    { PTY, "pty", init_pty, ifdup_serial },
    { TCP, "tcp", init_tcp, ifdup_tcp },
    { UDP, "udp", init_udp, ifdup_udp },
    { GOFREE, "gofree", init_gofree, ifdup_gofree },
    { BCAST, "broadcast", init_bcast, ifdup_bcast },
    { MCAST, "multicast", init_mcast, ifdup_mcast },
    { ST, "seatalk", NULL, NULL },
    { END, NULL, NULL, NULL },
};
