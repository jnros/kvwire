## kvwire

RDMA over Ethernet (RoCE) for disaggregated inference

After the prefill server builds KV cache, it typically moves tensors to the decode server over RDMA. Is it optimal to move tiny chunks, or the entire cache at once? If we paralelize KV compute and RDMA offload, how much can we speed things up?

This project moves a Qwen KV Cache (24 layers, 1024 tokens, 48MB) between two machines with verbs in C.

- Soft-ROCE on two servers over Ethernet
- TCP bootstrap for out-of-band (qpn, rkey, gid) then state machine (INIT->RTR->RTS)
- One-sided RDMA write
- Sweeps chunk size per-token-per-layer (512B) → per-token (12KB) → per-layer (2MB) → whole cache (48MB)
- measures bandwidth + latency (p50/p99/p999), then double-buffered compute/transfer overlap  

Run: Two Linux boxes on same LAN. On each, run:  
modprobe rdma_rxe  
rdma link add rxe0 type rxe netdev eth0  

On each, build with make. Run receiver side (--server) first, note the IP, then run sender (--client).  

Run: Two Linux boxes on same LAN. On each, run:  
modprobe rdma_rxe
rdma link add rxe0 type rxe netdev eth0

On each, build with make. Run receiver side (--server) first, note the IP, then run sender (--client).

Findings: Bandwidth knee at ~12KB. Per-token-per-layer (512B) dominated by overhead.  Per-token (12Kb) streaming moves up to 95% of saturation.   

![BWLat](out_latbw.png)

Overlap peaks at 1.92X of theoretical 2X when Tc = Tx.  

![Overlap](out_overlap.png)

Results are limited to soft RoCE. Real RNIC would lower the floors, shift knee to left, but shape would stay the same.
