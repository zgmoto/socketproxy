/*
 * globals.c
 */
#include "globals.h"
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>  
#include <sys/ioctl.h> 
#include <netinet/if_ether.h>  
#include <net/if.h>
#include <linux/sockios.h>

#ifdef __cplusplus
extern "C" {
#endif

static char current_time[64];
static int end_flag = 1;
static int test = 0;
static int daemon_run = 0;

static char host_addr[64];
static int host_port = DEFAULT_HOSTPORT;

#ifdef GWLINK_WITH_SOCKS5_PASS
char macaddr[64];
char macdev[64] = DEFAULT_MACDEV;
char auth_user[128] = DEFAULT_USER;
char auth_pass[128] = DEFAULT_PASS;
#endif

static int listen_port = 0;
static int maxconn = SERVER_TCPLINK_NUM;

static port_map_t port_maps[MAX_PORT_MAPS];
static int port_map_count = 0;

static void end_handler(int sig);

int istest() {
	return test;
}

int isdaemon() {
	return daemon_run;
}

void set_end(int end) {
	end_flag = end;
}

int get_end() {
	return end_flag;
}

char *get_host_addr() {
	return host_addr;
}

int get_host_port() {
	return host_port;
}

#ifdef GWLINK_WITH_SOCKS5_PASS
char *get_mac_addr() {
	struct ifreq ifreq;
	int sock;

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		return NULL;
	}

	strcpy(ifreq.ifr_name, macdev);

	if (ioctl(sock, SIOCGIFHWADDR, &ifreq) < 0) {
		return NULL;
	}

	memset(macaddr, 0, sizeof(macaddr));
	sprintf(macaddr, "%02X%02X%02X%02X%02X%02X",
			(uint8) ifreq.ifr_hwaddr.sa_data[0],
			(uint8) ifreq.ifr_hwaddr.sa_data[1],
			(uint8) ifreq.ifr_hwaddr.sa_data[2],
			(uint8) ifreq.ifr_hwaddr.sa_data[3],
			(uint8) ifreq.ifr_hwaddr.sa_data[4],
			(uint8) ifreq.ifr_hwaddr.sa_data[5]);
	if (!strcmp(macaddr, "000000000000")) {
		return NULL;
	}

	return macaddr;
}

int get_rand(int min, int max) {
	srand((unsigned int) time(NULL));
	return (rand() % (max - min + 1)) + min;
}

char *get_auth_user() {
	return auth_user;
}

char *get_auth_pass() {
	return auth_pass;
}
#endif

int get_listen_port() {
	return listen_port;
}

int get_server_mode() {
	return (host_addr[0] == '\0');
}

int add_port_map(int target_port, int listen_port) {
	if (port_map_count >= MAX_PORT_MAPS)
		return -1;
	port_maps[port_map_count].target_port = target_port;
	port_maps[port_map_count].listen_port = listen_port;
	port_maps[port_map_count].listen_fd = -1;
	port_map_count++;
	return 0;
}

int get_port_map_count(void) {
	return port_map_count;
}

port_map_t *get_port_map(int index) {
	if (index < 0 || index >= port_map_count)
		return NULL;
	return &port_maps[index];
}

int get_max_connections_num() {
	return maxconn;
}

void split_host_and_port(char *addrstr) {
	int i = 0;
	int addrlen = strlen(addrstr);

	while (i < addrlen) {
		if (*(addrstr + i) == ':') {
			memset(host_addr, 0, sizeof(host_addr));
			strncpy(host_addr, addrstr, i);
			host_port = atoi(addrstr + i + 1);
			return;
		}
		i++;
	}

	memset(host_addr, 0, sizeof(host_addr));
	strcpy(host_addr, addrstr);
}

int start_params(int argc, char **argv) {
	int ch;
	int isget = 0;
	int ismac_replace = 0;
	opterr = 0;

	const char *optstrs = "a:c:p:e:m:d:t:l:h";
	while ((ch = getopt(argc, argv, optstrs)) != -1) {
		switch (ch) {
		case 'h':
#ifdef GWLINK_WITH_SOCKS5_PASS
			AI_PRINTF("Usage: %s [-t print|daemon] [-c host[:port]] [-l listen_port] [-p target:listen] [-a user:pass] [-e macdev] [-m maxconn]\n", argv[0]);
			AI_PRINTF("  Server: %s -l port [-a user:pass]\n", argv[0]);
			AI_PRINTF("  Client: %s -c host:port -p target:listen [-a auth] [-m maxconn]\n", argv[0]);
#else
			AI_PRINTF("  Server: %s -l port\n", argv[0]);
			AI_PRINTF("  Client: %s -c host:port -p target:listen\n", argv[0]);
#endif
			AI_PRINTF("Default\n");
#ifdef GWLINK_WITH_SOCKS5_PASS
			AI_PRINTF("    user:pass   %s:%s\n", DEFAULT_USER, DEFAULT_PASS);
			AI_PRINTF("    macdev      %s\n", DEFAULT_MACDEV);
#endif
			AI_PRINTF("    port        %d\n", DEFAULT_HOSTPORT);
			AI_PRINTF("    maxconn     %d\n", SERVER_TCPLINK_NUM);
			return 1;

		case 't':
			isget = 1;
			if (!strcmp(optarg, "print")) {
				test = 1;
			} else if (!strcmp(optarg, "daemon")) {
				daemon_run = 1;
				if (fork() > 0) {
					usleep(1000);
					return 1;
				}
				setsid();
			}
			break;

		case 'c':
			isget = 1;
			split_host_and_port(optarg);
			break;

		case 'p':
			isget = 1;
			{
				char *colon = strchr(optarg, ':');
				if (!colon) {
					AI_PRINTF("Invalid -p format, use target:listen\n");
					return -1;
				}
				int tport = atoi(optarg);
				int lport = atoi(colon + 1);
				if (add_port_map(tport, lport) < 0) {
					AI_PRINTF("Too many port mappings (max %d)\n", MAX_PORT_MAPS);
					return -1;
				}
			}
			break;
#ifdef GWLINK_WITH_SOCKS5_PASS
		case 'a':
			isget = 1;
			{
				int oalen = strlen(optarg);
				int oapos = 0;
				while (oapos < oalen) {
					if (*(optarg + oapos) == ':') {
						memset(auth_user, 0, sizeof(auth_user));
						memcpy(auth_user, optarg, oapos);
						memset(auth_pass, 0, sizeof(auth_pass));
						memcpy(auth_pass, optarg + oapos + 1, oalen - oapos);
					}
					oapos++;
				}
			}
			break;

		case 'e':
			isget = 1;
			ismac_replace = 1;
			memset(macdev, 0, sizeof(macdev));
			strcpy(macdev, optarg);
			break;
#endif
		case 'l':
			isget = 1;
			listen_port = atoi(optarg);
			break;

		case 'm':
			isget = 1;
			maxconn = atoi(optarg);
			break;
		}
	}

	if (!isget) {
		AI_PRINTF("Unrecognize arguments.\n");
		AI_PRINTF("\'%s -h\' get more help infomations.\n", argv[0]);
		return -1;
	}
#ifdef GWLINK_WITH_SOCKS5_PASS
	if (ismac_replace) {
		char *mac_addr = get_mac_addr();
		int mac_rand = get_rand(0, 0xFFFE);
		if (mac_addr == NULL || mac_rand == 0) {
			AI_PRINTF("Can't get mac addr from %s\n", macdev);
			AI_PRINTF("\'%s -h\' get more help infomations.\n", argv[0]);
			return -1;
		}

		memset(auth_user, 0, sizeof(auth_user));
		sprintf(auth_user, "%s%04X", mac_addr, (uint32) mac_rand);
	}
#endif
	return 0;
}

void process_signal_register() {
	signal(SIGINT, end_handler);
	signal(SIGTSTP, end_handler);
	signal(SIGPIPE, SIG_IGN); //send error
}

void end_handler(int sig) {
	switch (sig) {
	case SIGINT:
		AI_PRINTF(" SIGINT\t");
		break;

	case SIGTSTP:
		AI_PRINTF(" SIGTSTP\t");
		break;
	}

	AI_PRINTF("\n");
	end_flag = 0;
}

int mach_init() {
#ifdef DLOG_PRINT	
	system("rm -f "TMP_LOG);
#endif
	return 0;
}

char *get_current_time() {
	time_t t;
	time(&t);
	bzero(current_time, sizeof(current_time));
	struct tm *tp = localtime(&t);
	strftime(current_time, 100, "%Y-%m-%d %H:%M:%S", tp);

	return current_time;
}

unsigned long get_system_time() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 * 1000 + tv.tv_usec;
}

#ifdef __cplusplus
}
#endif

