/*
 * globals.h
 */
#ifndef __GLOBALS_H__
#define __GLOBALS_H__

#include "mtypes.h"
#include "netlist.h"
#include "dlog.h"
#include <time.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>

#ifndef PTHREAD_SELECT_NUM
#define PTHREAD_SELECT_NUM	1
#endif

#define GWLINK_WITH_SOCKS5_PASS

#ifdef __cplusplus
extern "C" {
#endif

enum ConnWay {
	CONN_WITH_SERVER = 0, CONN_WITH_CLIENT
};

typedef struct ExtConnData {
	tcp_conn_t *toconn;
	enum ConnWay way;
	int isuse;
	int target_port;       /* -1 unset; server reads 2-byte header from pool */
	uint8 head_buf[2];     /* partial header accumulator (server side) */
	int head_len;          /* 0..2 bytes accumulated */
} ext_conn_t;

#define MAX_PORT_MAPS	16

typedef struct {
	uint16 target_port;   /* server connects to 127.0.0.1:target_port */
	uint16 listen_port;   /* client listens on this port */
	int listen_fd;        /* listening socket fd, -1 until created */
} port_map_t;

#define MAXSIZE	8192
#define DEFAULT_USER	"admin"
#define DEFAULT_PASS	"admin"
#define DEFAULT_HOSTPORT	40000
#define SERVER_TCPLINK_NUM	200
#define DEFAULT_MACDEV	"eth0"

int istest();
int isdaemon();
void set_end(int end);
int get_end();
char *get_host_addr();
int get_host_port();
#ifdef GWLINK_WITH_SOCKS5_PASS
char *get_auth_user();
char *get_auth_pass();
#endif
int get_max_connections_num();
int get_listen_port();
int get_server_mode();

int add_port_map(int target_port, int listen_port);
int get_port_map_count(void);
port_map_t *get_port_map(int index);

int start_params(int argc, char **argv);

int mach_init();

void process_signal_register();

char *get_current_time();
unsigned long get_system_time();

#ifdef __cplusplus
}
#endif

#endif	//__GLOBALS_H__
