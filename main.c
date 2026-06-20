/*
 * main.c
 */
#include <globals.h>
#include <nethandler.h>

#ifdef __cplusplus
extern "C" {
#endif

int main(int argc, char **argv) {
	if (start_params(argc, argv) != 0) {
		return 1;
	}

	process_signal_register();

	if (mach_init() < 0) {
		return 1;
	}

	AI_PRINTF("[%s] %s start, getpid=%d\n",
			get_current_time(), TARGET_NAME, getpid());

	if (select_init() < 0) {
		return 1;
	}

	if (!get_server_mode()) {
		if (net_tcp_connect() < 0) {
			return 1;
		}
	}

	if (get_listen_port() > 0) {
		int lfd;
		int opt = 1;
		struct sockaddr_in lin;

		if ((lfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			perror("listen socket");
			return 1;
		}

		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

		bzero(&lin, sizeof(lin));
		lin.sin_family = AF_INET;
		lin.sin_addr.s_addr = htonl(INADDR_ANY);
		lin.sin_port = htons(get_listen_port());

		if (bind(lfd, (struct sockaddr *) &lin, sizeof(lin)) < 0) {
			perror("listen bind");
			return 1;
		}

		if (listen(lfd, 256) < 0) {
			perror("listen");
			return 1;
		}

		set_listen_fd(lfd);
		select_set(lfd);

		AI_PRINTF("[%s] listen on :%d for clients\n",
				get_current_time(), get_listen_port());
	}

	/* Create listen sockets for each -p target:listen mapping */
	{
		int i;
		for (i = 0; i < get_port_map_count(); i++) {
			port_map_t *pm = get_port_map(i);
			int lfd;
			int opt = 1;
			struct sockaddr_in lin;

			if ((lfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
				perror("portmap listen socket");
				return 1;
			}

			setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

			bzero(&lin, sizeof(lin));
			lin.sin_family = AF_INET;
			lin.sin_addr.s_addr = htonl(INADDR_ANY);
			lin.sin_port = htons(pm->listen_port);

			if (bind(lfd, (struct sockaddr *) &lin, sizeof(lin)) < 0) {
				perror("portmap listen bind");
				return 1;
			}

			if (listen(lfd, 256) < 0) {
				perror("portmap listen");
				return 1;
			}

			pm->listen_fd = lfd;
			register_portmap_listen_fd(lfd, pm);
			select_set(lfd);

			AI_PRINTF("[%s] portmap listen on :%d -> target :%d\n",
					get_current_time(), pm->listen_port, pm->target_port);
		}
	}

	pthread_with_select();

	AI_PRINTF("[%s] %s exit, %d\n", get_current_time(), TARGET_NAME, getpid());
	return 0;
}

#ifdef __cplusplus
}
#endif
