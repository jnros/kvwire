#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

#include "kvwire.h"

static int tcp_connect(const char *host, int port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_port = htons(port) };
	int fd, on = 1;

	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		struct hostent *he = gethostbyname(host);
		if (!he) { fprintf(stderr, "resolve %s failed\n", host); return -1; }
		memcpy(&sa.sin_addr, he->h_addr, he->h_length);
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	while (connect(fd, (void *)&sa, sizeof sa) < 0) {
		if (errno != ECONNREFUSED) { perror("connect"); return -1; }
		usleep(200000);
	}
	setsockopt(fd, IPPROTO_TCP, 1, &on, sizeof on);
	return fd;
}

static int tcp_listen(int port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_addr.s_addr = INADDR_ANY,
				  .sin_port = htons(port) };
	int lfd, fd, on = 1;

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	if (bind(lfd, (void *)&sa, sizeof sa) < 0) { perror("bind"); return -1; }
	listen(lfd, 1);
	fd = accept(lfd, NULL, NULL);
	close(lfd);
	setsockopt(fd, IPPROTO_TCP, 1, &on, sizeof on);
	return fd;
}

static int xchg(int fd, const struct qp_info *out, struct qp_info *in)
{
	if (write(fd, out, sizeof *out) != (ssize_t)sizeof *out) return -1;
	if (read(fd, in, sizeof *in) != (ssize_t)sizeof *in) return -1;
	return 0;
}

void sync_peer(int fd)
{
	char c = 'x';
	write(fd, &c, 1);
	read(fd, &c, 1);
}

int main(int argc, char **argv)
{
	const char *role = NULL, *peer = NULL, *mode = "lat";
	const char *dev = "rxe0", *kvdir = NULL;
	int port = 18515, probe = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--server")) role = "server";
		else if (!strcmp(argv[i], "--client")) { role = "client"; peer = argv[++i]; }
		else if (!strcmp(argv[i], "--mode")) mode = argv[++i];
		else if (!strcmp(argv[i], "--dev")) dev = argv[++i];
		else if (!strcmp(argv[i], "--kv")) kvdir = argv[++i];
		else if (!strcmp(argv[i], "--port")) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--probe")) probe = 1;
	}

	if (probe) {
		struct pcie_info pi;
		pcie_query(dev, &pi);
		pcie_print(dev, &pi);
		return pi.ok ? 0 : 1;
	}

	if (!role) {
		fprintf(stderr,
		  "usage:\n"
		  "  probe:  %s --probe [--dev rxe0]\n"
		  "  server: %s --server [--mode lat|bw|pipe] [--dev rxe0]\n"
		  "  client: %s --client <server-ip> [--mode lat|bw|pipe] [--kv DIR]\n",
		  argv[0], argv[0], argv[0]);
		return 1;
	}
	srand48(getpid() ^ time(NULL));

	struct ctx c = {0};
	if (rdma_init(&c, dev)) return 1;

	struct pcie_info pi;
	pcie_query(dev, &pi);
	if (!strcmp(role, "server"))
		fprintf(stderr, "kvwire: server  mode %s  dev %s  port %d\n",
			mode, dev, port);
	else
		fprintf(stderr, "kvwire: client -> %s  mode %s  dev %s\n",
			peer, mode, dev);
	pcie_oneline(dev, &pi);

	int oob = !strcmp(role, "server") ? tcp_listen(port)
					  : tcp_connect(peer, port);
	if (oob < 0) return 1;
	if (xchg(oob, &c.local, &c.remote)) { perror("xchg"); return 1; }
	if (qp_to_rtr_rts(&c)) return 1;

	int is_client = !strcmp(role, "client");
	if (is_client && kvdir) load_kv(&c, kvdir);
	post_recv(&c);

	fprintf(stderr, "connected: local qpn %u <-> remote qpn %u, mode %s\n",
		c.local.qpn, c.remote.qpn, mode);

	if (!strcmp(mode, "lat"))       run_lat(&c, is_client, oob);
	else if (!strcmp(mode, "bw"))   run_bw(&c, is_client, oob);
	else if (!strcmp(mode, "pipe")) run_pipe(&c, is_client, oob);
	else { fprintf(stderr, "bad mode %s\n", mode); return 1; }

	sync_peer(oob);
	close(oob);
	return 0;
}
