# cluster_rdma
A tool for test the rdma performance in a cluster

# Cluster Model
![Cluster](cluster.png)

This tool can run on all nodes in a cluster. The running mode can be client/server mode or point-to-point mode. Each node sends M Byte data in N batch to X QPs. The tool current supports read/write operation and is mainly used to measure the throughput and latency of rdma in a cluster. 