#ifndef KVWIRE_H
#define KVWIRE_H

#include <stdint.h>
#include <stddef.h>
#include <infiniband/verbs.h>

#define IB_PORT		1
#define GID_INDEX	1
#define MAX_BUF		(64u << 20)
#define WARMUP		64
#define ITERS		2000
#define BW_DEPTH	64

extern const size_t g_sizes[];
extern const size_t n_sizes;

struct qp_info {
	uint32_t qpn;
	uint32_t psn;
	uint32_t rkey;
	uint64_t addr;
	union ibv_gid gid;
} __attribute__((packed));

struct ctx {
	struct ibv_context	*ctx;
	struct ibv_pd		*pd;
	struct ibv_cq		*cq;
	struct ibv_qp		*qp;
	struct ibv_mr		*mr;
	char			*buf;
	union ibv_gid		 gid;
	struct qp_info		 local, remote;
};

int rdma_init(struct ctx *c, const char *dev_name);
int qp_to_rtr_rts(struct ctx *c);
int post_recv(struct ctx *c);
int poll_one(struct ctx *c);
int post_write(struct ctx *c, size_t len, int with_imm, int signaled);
int post_write_at(struct ctx *c, size_t off, size_t len, int signaled);

struct pcie_info {
	int	ok;
	char	link_speed[32];
	int	width;
	int	numa;
	char	vendor[16], device[16];
	char	bdf[32];		/* 0000:08:00.0 */
	char	via[32];		/* backing netdev, "" if direct */
	double	ceiling_gbps;
};
int pcie_query(const char *ibdev, struct pcie_info *out);
void pcie_print(const char *ibdev, const struct pcie_info *pi);
void pcie_oneline(const char *ibdev, const struct pcie_info *pi);

void run_lat(struct ctx *c, int is_client, int oob);
void run_bw(struct ctx *c, int is_client, int oob);
void run_pipe(struct ctx *c, int is_client, int oob);
void load_kv(struct ctx *c, const char *dir);

void sync_peer(int fd);

#endif
