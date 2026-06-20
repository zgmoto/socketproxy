/*
 * nethandler.c
 */

#include "nethandler.h"
#include "netlist.h"
#include <errno.h>

#ifdef GWLINK_WITH_SOCKS5_PASS
const char socks5_frame[] = { 0x05, 0x01, 0x02 };
char userpass_frame[256];
#endif

static int nbytes;
static uint8 buf[MAXSIZE];
static char rate_print[64];

#define RECONNECT_BACKOFF_BASE   1
#define RECONNECT_BACKOFF_MAX   60

static int reconnect_fail_count = 0;

/* forward declaration */
static tcp_conn_t *try_connect(int index, char *host, int port, enum ConnWay way);

static int pool_listen_fd = -1;
void set_listen_fd(int fd) { pool_listen_fd = fd; }

/* multiple port-map listen fds */
#define MAX_PM_LISTEN_FDS 16
static struct { int fd; port_map_t *pm; } pm_listen_fds[MAX_PM_LISTEN_FDS];
static int pm_listen_count = 0;

void register_portmap_listen_fd(int fd, port_map_t *pm) {
	if (pm_listen_count < MAX_PM_LISTEN_FDS) {
		pm_listen_fds[pm_listen_count].fd = fd;
		pm_listen_fds[pm_listen_count].pm = pm;
		pm_listen_count++;
	}
}

static port_map_t *lookup_portmap_listen_fd(int fd) {
	int i;
	for (i = 0; i < pm_listen_count; i++)
		if (pm_listen_fds[i].fd == fd) return pm_listen_fds[i].pm;
	return NULL;
}

/* 在池中找一条空闲且已认证的服务器连接 */
static tcp_conn_t *find_idle_pool_conn() {
	tcp_conn_list_t *plist = get_tcp_conn_list();
	tcp_conn_t *t;
	for (t = plist->p_head; t != NULL; t = t->next) {
		ext_conn_t *ex = (ext_conn_t *) t->extdata;
		if (ex && ex->way == CONN_WITH_SERVER
				&& t->gwlink_status == GWLINK_START && !ex->isuse) {
			return t;
		}
	}
	return NULL;
}

/* server mode: pair two unpaired GWLINK_START connections */
static void try_pair_server(tcp_conn_t *self) {
	if (!get_server_mode()) return;
	ext_conn_t *sx = (ext_conn_t *) self->extdata;
	if (sx->toconn) return;

	tcp_conn_list_t *plist = get_tcp_conn_list();
	tcp_conn_t *t;
	for (t = plist->p_head; t != NULL; t = t->next) {
		if (t == self) continue;
		ext_conn_t *ex = (ext_conn_t *) t->extdata;
		if (ex && t->gwlink_status == GWLINK_START
				&& !ex->toconn && sx->way != ex->way) {
			sx->toconn = t;
			ex->toconn = self;
			sx->isuse = 1;
			ex->isuse = 1;
			AO_PRINTF("[%s] paired fd=%d <-> fd=%d\n",
					get_current_time(), self->fd, t->fd);
			return;
		}
	}
}

/* accept 客户端连接，立即配对池中空闲连接 */
static void handle_accept(int listen_fd) {
	struct sockaddr_in cin;
	socklen_t alen = sizeof(cin);
	int cfd = accept(listen_fd, (struct sockaddr *) &cin, &alen);
	if (cfd < 0) return;

	ext_conn_t *ext = calloc(1, sizeof(ext_conn_t));
	ext->way = CONN_WITH_CLIENT;
	ext->isuse = 0;
	ext->target_port = -1;
	ext->head_len = 0;

	tcp_conn_t *cli = new_tcpconn(cfd,
			get_server_mode() ? GWLINK_AUTH : GWLINK_START,
			ntohs(cin.sin_port), inet_ntoa(cin.sin_addr),
			get_listen_port(), ext);
	if (cli == NULL) {
		free(ext);
		close(cfd);
		return;
	}

	addto_tcpconn_list(cli);
	cli->pt_pos = select_set(cfd);

	if (!get_server_mode()) {
		/* 查找是否是 port-map 监听 */
		port_map_t *pm = lookup_portmap_listen_fd(listen_fd);
		tcp_conn_t *srv = find_idle_pool_conn();
		if (srv) {
			ext->toconn = srv;
			((ext_conn_t *) srv->extdata)->toconn = cli;
			((ext_conn_t *) srv->extdata)->isuse = 1;

			if (pm) {
				/* 在池连接上发送 2 字节目标端口头 */
				uint8 header[2];
				header[0] = (pm->target_port >> 8) & 0xFF;
				header[1] = pm->target_port & 0xFF;
				int sent = 0;
				while (sent < 2) {
					int n = send(srv->fd, header + sent, 2 - sent, 0);
					if (n < 0) {
						if (errno == EAGAIN) { usleep(1000); continue; }
						break;
					}
					sent += n;
				}
				AI_PRINTF("[%s] sent header target=%d on pool fd=%d\n",
						get_current_time(), pm->target_port, srv->fd);
			}

			AI_PRINTF("[%s] client %s:%d paired with pool fd=%d\n",
					get_current_time(), cli->host_addr, cli->port, srv->fd);
		} else {
			AI_PRINTF("[%s] client %s:%d waiting (no free pool conn)\n",
					get_current_time(), cli->host_addr, cli->port);
		}
	}
}

struct timespec *select_time = NULL;
struct timespec local_time;

int serlink_count[PTHREAD_SELECT_NUM] = { 0 };

int before_channel;
int iswork = 0;
int istrigger = 0;

int get_total_serlink_count() {
	int i;
	int total = 0;
	for (i = 0; i < PTHREAD_SELECT_NUM; i++) {
		total += serlink_count[i];
	}

	return total;
}

void set_timespec(time_t s) {
	if (s == 0) {
		select_time = NULL;
	} else {
		local_time.tv_sec = s;
		local_time.tv_nsec = 0;
		select_time = &local_time;
	}
}

struct timespec *get_timespec() {
	return select_time;
}

char *get_rate_print(uint32 rate) {
	bzero(rate_print, sizeof(rate_print));
	if (rate < 1024) {
		sprintf(rate_print, "%u B/s", rate);
	} else if (rate < 1024 * 1024) {
		sprintf(rate_print, "%.1f KB/s", (float) (rate * 10 / 1024) / 10);
	} else if (rate < 1024 * 1024 * 1024) {
		sprintf(rate_print, "%.2f MB/s",
				(float) (rate * 100 / (1024 * 1024)) / 100);
	} else {
		sprintf(rate_print, "%u.2f G/s",
				(float) (rate * 100 / (1024 * 1024 * 1024)) / 100);
	}

	return rate_print;
}

void send_with_rate_callback(tcp_conn_t *src_conn, tcp_conn_t *dst_conn,
		uint8 *data, int len,
		void(*rate_call)(float, tcp_conn_t *, tcp_conn_t *)) {
	if (data == NULL || len <= 0 || src_conn == NULL || dst_conn == NULL) {
		return;
	}

	if (dst_conn->gwlink_status != GWLINK_START) {
		if (dst_conn->data == NULL) {
			dst_conn->data = calloc(1, len);
			if (dst_conn->data) {
				memcpy(dst_conn->data, data, len);
				dst_conn->len = len;
			} else {
				dst_conn->len = 0;
			}
		} else {
			dst_conn->data = realloc(dst_conn->data, dst_conn->len + len);
			if (dst_conn->data) {
				memcpy(dst_conn->data + dst_conn->len, data, len);
				dst_conn->len += len;
			} else {
				dst_conn->len = 0;
			}
		}
		return;
	}

	int stm = 100;
	unsigned long sbtime = get_system_time();

	uint8 *pdata = data;
	int plen = len;
	if (dst_conn->data != NULL && dst_conn->data != data && dst_conn->len > 0) {
		dst_conn->data = realloc(dst_conn->data, dst_conn->len + len);
		if (dst_conn->data) {
			memcpy(dst_conn->data + dst_conn->len, data, len);
			dst_conn->len += len;
		} else {
			dst_conn->len = 0;
		}

		pdata = dst_conn->data;
		plen = dst_conn->len;
	}

	while (send(dst_conn->fd, pdata, plen, 0) < 0) {
		if (errno == EAGAIN) {
			usleep(stm);
			if (stm < 1000)
				stm += 100;
			else if (stm < 50000)
				stm += 1000;
			else
				stm += 100000;

			//printf("send() again: errno==EAGAIN\n");
			continue;
		} else {
			if (errno != EPIPE && errno != ECONNRESET) {
				char ebuf[128] = { 0 };
				sprintf(ebuf, "send() ERROR errno=%d, strerror=%s", errno,
						strerror(errno));
				perror(ebuf);
			}
			return;
		}
	}

	if (dst_conn->data != NULL || dst_conn->len > 0) {
		free(dst_conn->data);
		dst_conn->data = NULL;
		dst_conn->len = 0;
	}

	rate_call(((float) len * 1000) / (get_system_time() - sbtime), src_conn,
			dst_conn);
}

void close_connection(int index, int fd) {
	close(fd);
	select_clr(index, fd);
}

void detect_link(int index) {
	if (iswork && serlink_count[index] <= 5) {
		set_timespec(get_rand(5, 10));
	} else if (istrigger && serlink_count[index] == 20) {
		time_handler(index);
		set_timespec(get_rand(58, 118));
		istrigger = 0;
	} else if (iswork && serlink_count[index] < (get_max_connections_num()
			/ PTHREAD_SELECT_NUM)) {
		set_timespec(get_rand(58, 118));
	} else {
		iswork = 1;
		set_timespec(0);
	}

	if (serlink_count[index] == get_max_connections_num()) {
		istrigger = 1;
	}
}

static void try_reconnect(int index) {
	tcp_conn_t *conn = try_connect(index, get_host_addr(),
			get_host_port(), CONN_WITH_SERVER);
	if (conn == NULL) {
		reconnect_fail_count++;
		int delay = RECONNECT_BACKOFF_BASE << reconnect_fail_count;
		if (delay > RECONNECT_BACKOFF_MAX)
			delay = RECONNECT_BACKOFF_MAX;
		set_timespec(delay);
		AO_PRINTF("[%s] reconnect fail, retry in %ds (fail_count=%d)\n",
				get_current_time(), delay, reconnect_fail_count);
	} else {
		reconnect_fail_count = 0;
		set_timespec(0);
	}
}

void release_connection_with_fd(int index, int fd) {
	tcp_conn_t *tconn = queryfrom_tcpconn_list(fd);
	int is_server = 0;
	if (tconn && tconn->extdata) {
		is_server = (((ext_conn_t *) tconn->extdata)->way
				== CONN_WITH_SERVER);
	}

	detect_link(index);
	close_connection(index, fd);
	delfrom_tcpconn_list(fd);
	AO_PRINTF("[%s] close, user=%s, fd=%d, total=%d\n",
			get_current_time(), get_auth_user(), fd, get_total_serlink_count());

	if (is_server && !get_server_mode()) {
		try_reconnect(index);
	}
}

uint32 get_socket_local_port(int fd) {
	struct sockaddr_in loc_addr;
	int len = sizeof(sizeof(loc_addr));
	memset(&loc_addr, 0, len);
	if (getsockname(fd, (struct sockaddr *) &loc_addr, &len) == 0
			&& (loc_addr.sin_family == AF_INET)) {
		return ntohs(loc_addr.sin_port);
	}

	return 0;
}

tcp_conn_t *try_connect(int index, char *host, int port, enum ConnWay way) {
	int fd;
	tcp_conn_t *tconn;
	if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		perror("client tcp socket fail");
		return NULL;
	}

	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	ext_conn_t *extdata = calloc(1, sizeof(ext_conn_t));
	extdata->toconn = NULL;
	extdata->way = way;
	extdata->isuse = 0;

	tconn = new_tcpconn(fd, GWLINK_INIT, 0, host, port, extdata);
	if (tconn == NULL) {
		free(extdata);
		return NULL;
	}

	int res = connect(fd, (struct sockaddr *) &tconn->host_in,
			sizeof(tconn->host_in));
	if (0 != res && errno != EINPROGRESS) {
		perror("client tcp socket connect server fail");
		AO_PRINTF("[%s] connect %s fail\n", get_current_time(), host);
		free(extdata);
		if (tconn) {
			free(tconn);
		}
		return NULL;
	}

	if (res == 0) {
		tconn->port = get_socket_local_port(fd);
		if (way != CONN_WITH_CLIENT) {
			tconn->pt_pos = select_set(fd);
			serlink_count[tconn->pt_pos]++;
			detect_link(tconn->pt_pos);
#ifdef GWLINK_WITH_SOCKS5_PASS
			tconn->gwlink_status = GWLINK_AUTH;
#else
			tconn->gwlink_status = GWLINK_START;
#endif
		} else {
			tconn->gwlink_status = GWLINK_START;
		}

		AO_PRINTF("[%s] line %d connect %s:%d, current port=%d, fd=%d, total=%d\n",
				get_current_time(), __LINE__, tconn->host_addr,
				tconn->host_port, tconn->port, fd, get_total_serlink_count());
	} else {
		if (index < 0)
			tconn->pt_pos = select_wtset(fd);
		else
			tconn->pt_pos = select_wtset_with_index(index, fd);
	}

	addto_tcpconn_list(tconn);
	return tconn;
}

int net_tcp_connect(char *host, int port) {
	int i;
	for (i = 0; i < get_max_connections_num(); i++) {
		try_connect(-1, get_host_addr(), get_host_port(), CONN_WITH_SERVER);
	}

	return 0;
}

void send_to_stream_call(float rate, tcp_conn_t *src_conn, tcp_conn_t *dst_conn) {
	((ext_conn_t *) src_conn->extdata)->isuse = 1;

	if (before_channel == src_conn->fd) {
		AT_PRINTF("\033[1A");
	}

	AT_PRINTF("[%s] %s:%d ==> %s:%d, fd=%d, %s\n",
			get_current_time(), src_conn->host_addr, src_conn->host_port,
			dst_conn->host_addr, dst_conn->host_port, src_conn->fd, get_rate_print(rate));

	before_channel = src_conn->fd;
}

void send_back_stream_call(float rate, tcp_conn_t *src_conn,
		tcp_conn_t *dst_conn) {
	((ext_conn_t *) dst_conn->extdata)->isuse = 1;

	if (before_channel == src_conn->fd) {
		AT_PRINTF("\033[1A");
	}

	AT_PRINTF("[%s] %s:%d <== %s:%d, fd=%d, %s\n",
			get_current_time(), dst_conn->host_addr, dst_conn->host_port,
			src_conn->host_addr, src_conn->host_port, src_conn->fd, get_rate_print(rate));

	before_channel = src_conn->fd;
}

int net_tcp_recv(int fd) {
	if ((pool_listen_fd > 0 && fd == pool_listen_fd)
			|| lookup_portmap_listen_fd(fd) != NULL) {
		handle_accept(fd);
		return 0;
	}

	tcp_conn_t *t_conn = queryfrom_tcpconn_list(fd);
	if (t_conn == NULL) {
		return 0;
	}

	if (t_conn->gwlink_status > GWLINK_INIT) {
		int stm = 100;
		while ((nbytes = recv(fd, buf, sizeof(buf), 0)) <= 0
				|| t_conn->gwlink_status == GWLINK_RELEASE) {
			if (errno == EAGAIN) {
				usleep(stm);
				if (stm < 1000)
					stm += 100;
				else if (stm < 50000)
					stm += 1000;
				else
					stm += 100000;

				//printf("send() again: errno==EAGAIN\n");
				continue;
			}

			ext_conn_t *extdata = (ext_conn_t *) (t_conn->extdata);
			tcp_conn_t *toconn = extdata->toconn;
			if (toconn) {
				if (toconn->gwlink_status > GWLINK_INIT) {
					if (extdata->way == CONN_WITH_SERVER)
						AO_PRINTF("[%s] line:%d to target\n", get_current_time(), __LINE__);
					else if (extdata->way == CONN_WITH_CLIENT) {
						serlink_count[toconn->pt_pos]--;
						AO_PRINTF("[%s] line:%d to server\n", get_current_time(), __LINE__);
					}

					release_connection_with_fd(toconn->pt_pos, toconn->fd);
				} else {
					((ext_conn_t *) toconn->extdata)->toconn = NULL;
				}
			}

			if (extdata->way == CONN_WITH_SERVER) {
					serlink_count[t_conn->pt_pos]--;
				AO_PRINTF("[%s] line:%d to server\n", get_current_time(), __LINE__);
			} else
				AO_PRINTF("[%s] line:%d to target\n", get_current_time(), __LINE__);

			release_connection_with_fd(t_conn->pt_pos, fd);
			return 0;
		}

		if (1) {
			//PRINT_HEX(buf, nbytes);
#ifdef GWLINK_WITH_SOCKS5_PASS
			if (t_conn->gwlink_status == GWLINK_AUTH) {
				int is_pool = 0;
				if (nbytes >= 3 && buf[0] == 0x05 && buf[1] + 2 <= nbytes) {
					int i;
					for (i = 0; i < buf[1]; i++) {
						if (buf[2 + i] == 0x02) { is_pool = 1; break; }
					}
				}
				if (is_pool) {
					uint8 resp[2] = { 0x05, 0x02 };
					while (send(fd, resp, 2, 0) < 0) {
						if (errno == EAGAIN) { usleep(100000); continue; }
						else { return 0; }
					}
					((ext_conn_t *) t_conn->extdata)->way = CONN_WITH_SERVER;
					t_conn->gwlink_status = GWLINK_PASS;
				} else if (get_server_mode()) {
					t_conn->gwlink_status = GWLINK_START;
				} else if (nbytes >= 2 && buf[0] == 0x05 && buf[1] == 0x02) {
					memset(userpass_frame, 0, sizeof(userpass_frame));
					userpass_frame[0] = 0x1;
					userpass_frame[1] = strlen(get_auth_user());
					memcpy(userpass_frame + 2, get_auth_user(),
							userpass_frame[1]);
					userpass_frame[2 + userpass_frame[1]] = strlen(
							get_auth_pass());
					memcpy(userpass_frame + 3 + userpass_frame[1],
							get_auth_pass(),
							userpass_frame[2 + userpass_frame[1]]);

					while (send(fd, userpass_frame, strlen(userpass_frame), 0)
							< 0) {
						if (errno == EAGAIN) {
							usleep(100000); continue;
						} else { return 0; }
					}
					t_conn->gwlink_status = GWLINK_PASS;
				}
				if (t_conn->gwlink_status == GWLINK_AUTH) return 0;
			}
			if (t_conn->gwlink_status == GWLINK_PASS) {
				ext_conn_t *ex = (ext_conn_t *) t_conn->extdata;
				if (get_server_mode() && ex->way == CONN_WITH_SERVER) {
					if (nbytes > 2 && buf[0] == 0x01) {
						int ulen = (uint8) buf[1];
						int plen = (uint8) buf[2 + ulen];
						if (3 + ulen + plen <= nbytes) {
							char user[128] = {0}, pass[128] = {0};
							memcpy(user, buf + 2, ulen);
							memcpy(pass, buf + 3 + ulen, plen);
							if (!strcmp(user, get_auth_user())
									&& !strcmp(pass, get_auth_pass())) {
								uint8 ok[2] = { 0x01, 0 };
								send(fd, ok, 2, 0);
								t_conn->gwlink_status = GWLINK_START;
								AO_PRINTF("[%s] auth ok fd=%d\n",
										get_current_time(), fd);
							} else {
								uint8 no[2] = { 0x01, 0x01 };
								send(fd, no, 2, 0);
							}
						}
					}
					return 0;
				}
				if (nbytes >= 2 && buf[0] == 0x01 && buf[1] == 0) {
					t_conn->gwlink_status = GWLINK_START;
					AO_PRINTF("[%s] line %d: user=%s auth success, fd=%d, total=%d\n", get_current_time(),
							__LINE__, get_auth_user(), t_conn->fd, get_total_serlink_count());
				} else if (nbytes >= 2 && buf[0] == 0x01 && buf[1] == 0x01) {
					t_conn->gwlink_status = GWLINK_AUTH;
					AO_PRINTF("[%s] line %d: user=%s pass=%s, auth fail, fd=%d\n", get_current_time(),
							__LINE__, get_auth_user(), get_auth_pass(), t_conn->fd);
				}

				if (nbytes > 2) {
					char tbuf[nbytes - 2];
					memcpy(tbuf, buf + 2, nbytes - 2);
					memcpy(buf, tbuf, nbytes - 2);
					nbytes = nbytes - 2;
				} else {
					return 0;
				}
			}
#endif
			try_pair_server(t_conn);
			tcp_conn_t *toconn = ((ext_conn_t *) (t_conn->extdata))->toconn;
			if (toconn) {
				if (((ext_conn_t *) (t_conn->extdata))->way == CONN_WITH_SERVER) {
					send_with_rate_callback(t_conn, toconn, buf, nbytes,
							send_to_stream_call);
				} else {
					send_with_rate_callback(t_conn, toconn, buf, nbytes,
							send_back_stream_call);
				}
			} else if (((ext_conn_t *) (t_conn->extdata))->way
					== CONN_WITH_SERVER) {
				ext_conn_t *ex = (ext_conn_t *) t_conn->extdata;
				/* 累积 2 字节目标端口头 */
				int needed = 2 - ex->head_len;
				int take = (nbytes < needed) ? nbytes : needed;
				memcpy(ex->head_buf + ex->head_len, buf, take);
				ex->head_len += take;

				if (ex->head_len < 2) {
					/* 头部还不完整，等更多数据 */
					return 0;
				}

				/* 头部完整，提取目标端口 */
				uint16 target_port = ((uint16) ex->head_buf[0] << 8)
						| ex->head_buf[1];
				ex->target_port = target_port;

				AO_PRINTF("[%s] header target_port=%d, fd=%d\n",
						get_current_time(), target_port, t_conn->fd);

				tcp_conn_t *cliconn = try_connect(t_conn->pt_pos, "127.0.0.1",
						target_port, CONN_WITH_CLIENT);
				if (cliconn) {
					ex->toconn = cliconn;
					((ext_conn_t *) cliconn->extdata)->toconn = t_conn;

					/* 转发头部之后的负载数据 */
					int payload_off = take;
					int payload_len = nbytes - take;
					if (payload_len > 0) {
						send_with_rate_callback(t_conn, cliconn,
								buf + payload_off, payload_len,
								send_to_stream_call);
					}
				} else {
					AO_PRINTF("[%s] connect to 127.0.0.1:%d failed\n",
							get_current_time(), target_port);
					/* 无法连接目标，释放池连接通知 client 端断开 */
					serlink_count[t_conn->pt_pos]--;
					release_connection_with_fd(t_conn->pt_pos, fd);
					return 0;
				}
			} else {
				AO_PRINTF("[%s] No object to transfer data\n", get_current_time());
			}
		}
	} else {
		int errinfo, errlen;
		int pt_pos = t_conn->pt_pos;
		errlen = sizeof(errlen);

		if (0 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &errinfo, &errlen)) {
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
			t_conn->port = get_socket_local_port(fd);
			if (t_conn->pt_pos < 0) {
				t_conn->pt_pos = select_set(fd);
				pt_pos = t_conn->pt_pos;
			} else {
				select_set_with_index(t_conn->pt_pos, fd);
			}

			if (((ext_conn_t *) (t_conn->extdata))->way == CONN_WITH_SERVER) {
#ifdef GWLINK_WITH_SOCKS5_PASS
				t_conn->gwlink_status = GWLINK_AUTH;

				while (send(fd, socks5_frame, sizeof(socks5_frame), 0) < 0) {
					if (errno == EAGAIN) {
						usleep(100000);
						continue;
					} else {
						t_conn->gwlink_status = GWLINK_RELEASE;
						break;
					}
				}
#endif
					serlink_count[t_conn->pt_pos]++;
				detect_link(t_conn->pt_pos);
			}

			AO_PRINTF("[%s] line %d connect %s:%d, current port=%d, fd=%d, total=%d\n",
					get_current_time(), __LINE__, t_conn->host_addr,
					t_conn->host_port, t_conn->port, fd, get_total_serlink_count());

			if (((ext_conn_t *) (t_conn->extdata))->way == CONN_WITH_CLIENT) {
				t_conn->gwlink_status = GWLINK_START;
				tcp_conn_t *toconn = ((ext_conn_t *) (t_conn->extdata))->toconn;
				if (toconn == NULL) {
					AO_PRINTF("[%s] line:%d to target\n", get_current_time(), __LINE__);
					release_connection_with_fd(t_conn->pt_pos, fd);
				} else {
					send_with_rate_callback(toconn, t_conn, t_conn->data,
							t_conn->len, send_to_stream_call);
				}
			}
		} else {
			AO_PRINTF("[%s] connect %s fail\n",
					get_current_time(), t_conn->host_addr, fd);
		}

		select_wtclr(pt_pos, fd);
	}

	return 0;
}

void time_handler(int index) {
	if (get_server_mode())
		return;

	AO_PRINTF("[%s] time handle, %d\n", get_current_time(),
			get_timespec() ? get_timespec()->tv_sec : 0);
	int need = (get_max_connections_num() / PTHREAD_SELECT_NUM)
			- serlink_count[index];
	if (need <= 0)
		return;

	int ok = 0;
	int i;
	for (i = 0; i < need; i++) {
		if (try_connect(index, get_host_addr(), get_host_port(),
				CONN_WITH_SERVER) != NULL) {
			ok++;
		}
	}

	if (ok > 0) {
		reconnect_fail_count = 0;
		set_timespec(0);
	} else {
		reconnect_fail_count++;
		int delay = RECONNECT_BACKOFF_BASE << reconnect_fail_count;
		if (delay > RECONNECT_BACKOFF_MAX)
			delay = RECONNECT_BACKOFF_MAX;
		set_timespec(delay);
		AO_PRINTF("[%s] batch reconnect fail, retry in %ds (fail_count=%d)\n",
				get_current_time(), delay, reconnect_fail_count);
	}
}

