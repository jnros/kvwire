#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvwire.h"

int rdma_init(struct ctx *c, const char *dev_name)
{
	struct ibv_device **list = ibv_get_device_list(NULL);
	struct ibv_device *dev = NULL;

	if (!list || !*list) { fprintf(stderr, "no RDMA devices\n"); return -1; }
	for (int i = 0; list[i]; i++) {
		if (!dev_name || !strcmp(ibv_get_device_name(list[i]), dev_name))
			{ dev = list[i]; break; }
	}
	if (!dev) { fprintf(stderr, "device %s not found\n", dev_name); return -1; }

	c->ctx = ibv_open_device(dev);
	ibv_free_device_list(list);
	if (!c->ctx) { perror("open_device"); return -1; }

	if (ibv_query_gid(c->ctx, IB_PORT, GID_INDEX, &c->gid)) {
		perror("query_gid"); return -1;
	}
	c->pd = ibv_alloc_pd(c->ctx);
	c->cq = ibv_create_cq(c->ctx, BW_DEPTH + 16, NULL, NULL, 0);

	c->buf = aligned_alloc(4096, MAX_BUF);
	memset(c->buf, 0xAB, MAX_BUF);
	c->mr = ibv_reg_mr(c->pd, c->buf, MAX_BUF,
			   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!c->pd || !c->cq || !c->mr) { perror("pd/cq/mr"); return -1; }

	struct ibv_qp_init_attr qa = {
		.send_cq = c->cq, .recv_cq = c->cq,
		.cap = { .max_send_wr = BW_DEPTH + 4, .max_recv_wr = 4,
			 .max_send_sge = 1, .max_recv_sge = 1 },
		.qp_type = IBV_QPT_RC,
	};
	c->qp = ibv_create_qp(c->pd, &qa);
	if (!c->qp) { perror("create_qp"); return -1; }

	c->local.qpn  = c->qp->qp_num;
	c->local.psn  = lrand48() & 0xffffff;
	c->local.rkey = c->mr->rkey;
	c->local.addr = (uint64_t)(uintptr_t)c->buf;
	c->local.gid  = c->gid;
	return 0;
}

int qp_to_rtr_rts(struct ctx *c)
{
	struct ibv_qp_attr a = { .qp_state = IBV_QPS_INIT,
		.pkey_index = 0, .port_num = IB_PORT,
		.qp_access_flags = IBV_ACCESS_REMOTE_WRITE };
	if (ibv_modify_qp(c->qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
			  IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
		perror("modify INIT"); return -1;
	}

	struct ibv_qp_attr r = {
		.qp_state = IBV_QPS_RTR,
		.path_mtu = IBV_MTU_1024,
		.dest_qp_num = c->remote.qpn,
		.rq_psn = c->remote.psn,
		.max_dest_rd_atomic = 1, .min_rnr_timer = 12,
		.ah_attr = {
			.is_global = 1, .port_num = IB_PORT,
			.grh = { .hop_limit = 1, .sgid_index = GID_INDEX },
		},
	};
	r.ah_attr.grh.dgid = c->remote.gid;
	if (ibv_modify_qp(c->qp, &r, IBV_QP_STATE | IBV_QP_AV |
			  IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
			  IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
		perror("modify RTR"); return -1;
	}

	struct ibv_qp_attr s = {
		.qp_state = IBV_QPS_RTS, .sq_psn = c->local.psn,
		.timeout = 14, .retry_cnt = 7, .rnr_retry = 7,
		.max_rd_atomic = 1,
	};
	if (ibv_modify_qp(c->qp, &s, IBV_QP_STATE | IBV_QP_SQ_PSN |
			  IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
			  IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) {
		perror("modify RTS"); return -1;
	}
	return 0;
}

int post_recv(struct ctx *c)
{
	struct ibv_sge sge = { .addr = (uintptr_t)c->buf, .length = 0,
			       .lkey = c->mr->lkey };
	struct ibv_recv_wr wr = { .wr_id = 1, .sg_list = &sge, .num_sge = 1 };
	struct ibv_recv_wr *bad;
	return ibv_post_recv(c->qp, &wr, &bad);
}

int poll_one(struct ctx *c)
{
	struct ibv_wc wc;
	int n;
	do { n = ibv_poll_cq(c->cq, 1, &wc); } while (n == 0);
	if (n < 0 || wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "wc status %s\n", ibv_wc_status_str(wc.status));
		return -1;
	}
	return 0;
}

int post_write(struct ctx *c, size_t len, int with_imm, int signaled)
{
	struct ibv_sge sge = { .addr = (uintptr_t)c->buf, .length = len,
			       .lkey = c->mr->lkey };
	struct ibv_send_wr wr = {
		.wr_id = 2, .sg_list = &sge, .num_sge = 1,
		.opcode = with_imm ? IBV_WR_RDMA_WRITE_WITH_IMM
				   : IBV_WR_RDMA_WRITE,
		.send_flags = signaled ? IBV_SEND_SIGNALED : 0,
		.wr.rdma = { .remote_addr = c->remote.addr,
			     .rkey = c->remote.rkey },
	};
	struct ibv_send_wr *bad;
	return ibv_post_send(c->qp, &wr, &bad);
}

int post_write_at(struct ctx *c, size_t off, size_t len, int signaled)
{
	struct ibv_sge sge = { .addr = (uintptr_t)c->buf + off, .length = len,
			       .lkey = c->mr->lkey };
	struct ibv_send_wr wr = {
		.wr_id = 3, .sg_list = &sge, .num_sge = 1,
		.opcode = IBV_WR_RDMA_WRITE,
		.send_flags = signaled ? IBV_SEND_SIGNALED : 0,
		.wr.rdma = { .remote_addr = c->remote.addr + off,
			     .rkey = c->remote.rkey },
	};
	struct ibv_send_wr *bad;
	return ibv_post_send(c->qp, &wr, &bad);
}
