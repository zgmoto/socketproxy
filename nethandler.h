/*
 * nethandler.h
 */
#ifndef __NETHANDLER_H__
#define __NETHANDLER_H__

#include "globals.h"
#include "netlist.h"

struct timespec *get_timespec();

int net_tcp_connect();
int net_tcp_recv(int fd);

void set_listen_fd(int fd);
void register_portmap_listen_fd(int fd, port_map_t *pm);
void time_handler(int index);

#endif  // __NETHANDLER_H__
