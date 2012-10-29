/* kplex_mods.h
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 */
iface_t *(*init_func[])(char *, iface_t *) = {
    init_file,
    init_serial,
    init_bcast,
    init_tcp,
    init_pty,
    init_bcast,
    init_seatalk
    };
