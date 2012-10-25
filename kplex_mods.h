iface_t *(*init_func[])(char *, iface_t *) = {
    init_file,
    init_serial,
    init_bcast,
    init_tcp,
    init_pty
    };
