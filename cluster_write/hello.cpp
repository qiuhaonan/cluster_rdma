#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <endian.h>
#include <inttypes.h>
#include <assert.h>
#include <byteswap.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <fstream>
#include <vector>
#include <thread>
#include <string>
#include <iostream>
#include <mutex>
#include "sock.h"
#include "head.h"

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t ntohll(uint64_t x)
{
    return bswap_64(x);
}
static inline uint64_t htonll(uint64_t x)
{
    return bswap_64(x);
}
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t ntohll(uint64_t x)
{
    return x;
}
static inline uint64_t htonll(uint64_t x)
{
    return x;
}
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

using namespace std;

long start_usec = 0;
long end_usec = 0;
bool debug = false;

struct config_t
{
    const char *dev_name;
    char *server_name;
    u_int32_t tcp_port;
    int ib_port;
};

struct config_t config = {
    "mlx5_0",
    NULL,
    19875,
    1};

struct mem_data
{
    uint64_t addr;
    uint32_t rkey;
    char gid[33];
};

struct resources
{
    vector<struct mem_data> remote_props_vec;
    struct ibv_port_attr port_attr;
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *cc;
    std::mutex lock;
    vector<struct ibv_qp *> qp_vec;
    struct ibv_mr *mr;
    char *buf;
    char *normal_buf;
    vector<int> server_sock_vec;
    vector<int> client_sock_vec;
    union ibv_gid dgid;
    union ibv_gid sgid;
    int max_cq_size;
    int mem_size;
    int io_size;
    int batch_size;
    int test_num;
    int traffic_class;
    int sgid_index;
    int verbose;
    int run_infinitely;
    int iteration;
    int perform_warmup;
    int event_or_poll; // 1 for event, 0 for poll default
    int cluster_size;
    int qp_num_per_node;
    char *remote_ip_config;
    vector<string> remote_ip_vec;
    shared_ptr<thread> server_thread;
    bool stop;
    long wait_time_before_start;
    long poll_time_out;
    bool server_client;
};

static inline uint64_t random_offset(struct resources *res)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    srand(now.tv_sec + now.tv_usec);
    uint64_t offset = rand();
    offset = (offset % (res->mem_size - res->io_size));
    return offset;
}

static inline long tick(long start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    long usec = now.tv_sec * 1000000 + now.tv_usec;
    usec = usec - start;
    return usec;
}

static int post_send(struct resources *res, int qp_index)
{
    struct ibv_send_wr *bad_wr;
    int rc;
    struct ibv_send_wr sr[res->batch_size];
    struct ibv_sge sge[res->batch_size];
    int i = 0;
    int offset = 0;
    for (; i < res->batch_size; ++i)
    {
        memset(&sge[i], 0, sizeof(struct ibv_sge));
        offset = random_offset(res);
        memcpy(res->buf + offset, res->normal_buf, res->io_size);
        sge[i].addr = (uintptr_t)res->buf + offset;
        sge[i].length = res->io_size;
        sge[i].lkey = res->mr->lkey;

        memset(&sr[i], 0, sizeof(struct ibv_send_wr));
        sr[i].next = (i < res->batch_size - 1) ? &sr[i + 1] : NULL;
        sr[i].wr_id = 0;
        sr[i].sg_list = &sge[i];
        sr[i].num_sge = 1;
        sr[i].opcode = IBV_WR_RDMA_WRITE;
        sr[i].wr.rdma.remote_addr = res->remote_props_vec[qp_index].addr + offset;
        sr[i].wr.rdma.rkey = res->remote_props_vec[qp_index].rkey;
    }
    sr[i - 1].send_flags = IBV_SEND_SIGNALED;
    rc = ibv_post_send(res->qp_vec[qp_index], &sr[0], &bad_wr);
    if (rc)
    {
        fprintf(stderr, "failed to post SR\n");
        return 1;
    }

    return 0;
}

static int modify_qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr qp_attr;
    int mask;
    int rc;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = config.ib_port;
    qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;

    mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

    rc = ibv_modify_qp(qp, &qp_attr, mask);
    if (rc)
    {
        fprintf(stderr, "failed to modify qp state to INIT\n");
        fprintf(stderr, "return %d\n", rc);
        return rc;
    }
    return 0;
}

static int modify_qp_to_rtr(struct resources *res, struct ibv_qp *qp, uint32_t remote_qpn)
{
    struct ibv_qp_attr qp_attr;
    int mask;
    int rc;

    memset(&qp_attr, 0, sizeof(qp_attr));

    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_1024;
    qp_attr.dest_qp_num = remote_qpn;
    qp_attr.max_dest_rd_atomic = 16;
    qp_attr.min_rnr_timer = 0x12;
    qp_attr.rq_psn = 0;
    qp_attr.ah_attr.is_global = 1;
    qp_attr.ah_attr.grh.sgid_index = res->sgid_index;
    qp_attr.ah_attr.grh.traffic_class = res->traffic_class;
    qp_attr.ah_attr.grh.dgid = res->dgid;
    qp_attr.ah_attr.port_num = config.ib_port;

    mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_AV | IBV_QP_MIN_RNR_TIMER | IBV_QP_MAX_DEST_RD_ATOMIC;

    rc = ibv_modify_qp(qp, &qp_attr, mask);
    if (rc)
    {
        fprintf(stderr, "failed to modify qp to rtr\n");
        return 1;
    }
    return 0;
}

static int modify_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr qp_attr;
    int mask;
    int rc;

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.timeout = 0x12;
    qp_attr.retry_cnt = 6;
    qp_attr.rnr_retry = 0;
    qp_attr.sq_psn = 0;
    qp_attr.max_rd_atomic = 16;

    mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    rc = ibv_modify_qp(qp, &qp_attr, mask);
    if (rc)
    {
        fprintf(stderr, "failed to modify qp state to RTS\n");
        return rc;
    }
    return 0;
}

static void resources_init(struct resources *res)
{
    res->ib_ctx = NULL;
    res->cq = NULL;
    res->cc = NULL;
    res->pd = NULL;
    res->mr = NULL;
    res->buf = NULL;
    res->normal_buf = NULL;
    res->max_cq_size = 10;
    res->mem_size = 0;
    res->io_size = 0;
    res->traffic_class = 0;
    res->sgid_index = 0;
    res->batch_size = 0;
    res->test_num = 0;
    res->verbose = 0;
    res->run_infinitely = 0;
    res->iteration = 0;
    res->perform_warmup = 0;
    res->event_or_poll = 0;
    res->cluster_size = 2;
    res->qp_num_per_node = 1;
    res->stop = false;
    res->wait_time_before_start = 1000000; // default 1s
    res->poll_time_out = 1000000;          // default 1s
    res->server_client = false;            // default client
}

static int resources_create(struct resources *res)
{
    struct ibv_device **dev_list = NULL;
    int dev_num;
    int mr_flags = 0;
    struct ibv_device *ib_dev = NULL;
    size_t size;
    int cq_size = 0;

    dev_list = ibv_get_device_list(&dev_num);

    int i = 0;
    for (; i < dev_num; ++i)
    {
        if (!config.dev_name)
        {
            fprintf(stdout, "unspecified device name,use device found first\n");
            res->ib_ctx = ibv_open_device(dev_list[i]);
            break;
        }
        if (!strcmp(config.dev_name, ibv_get_device_name(dev_list[i])))
        {
            ib_dev = dev_list[i];
            break;
        }
    }
    if (!ib_dev)
    {
        fprintf(stderr, "IB device %s wasn't found \n", config.dev_name);
        return 1;
    }

    res->ib_ctx = ibv_open_device(ib_dev);
    if (!res->ib_ctx)
    {
        fprintf(stdout, "couldn't get device\n");
        return -1;
    }
    ib_dev = NULL;

    if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr))
    {
        fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
        return 1;
    }
    res->pd = ibv_alloc_pd(res->ib_ctx);
    if (!res->pd)
    {
        fprintf(stderr, "failed to alloc pd\n");
        return -1;
    }
    res->cc = ibv_create_comp_channel(res->ib_ctx);
    cq_size = res->max_cq_size;
    res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, res->cc, 0);
    if (!res->cq)
    {
        fprintf(stderr, "failed to create cq\n");
        return -1;
    }

    size = res->mem_size;
    res->buf = (char *)malloc(size);
    size_t io_size = res->io_size;
    res->normal_buf = (char *)malloc(io_size);
    if (!res->buf)
    {
        fprintf(stderr, "failed to malloc buf\n");
        return -1;
    }

    memset(res->buf, 'a', size);
    memset(res->normal_buf, 'b', io_size);

    mr_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;

    res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
    if (!res->mr)
    {
        fprintf(stderr, "failed to register memory region\n");
        return -1;
    }

    if (ibv_query_gid(res->ib_ctx, config.ib_port, res->sgid_index, &(res->sgid)))
    {
        fprintf(stderr, "failed to query gid\n");
        return -1;
    }

    return 0;
}

static int connect_qp(struct resources *res, struct ibv_qp *qp, int sock, bool server)
{
    int rc;
    uint32_t local_qp_num, remote_qp_num;

    if (modify_qp_to_init(qp))
    {
        fprintf(stderr, "failed to modify qp to init\n");
        return -1;
    }

    local_qp_num = htonl(qp->qp_num);

    rc = sock_sync_data(sock, server, sizeof(uint32_t), &local_qp_num, &remote_qp_num);

    if (rc < 0)
    {
        fprintf(stderr, "sock_sync_data failed\n");
        return -1;
    }

    remote_qp_num = ntohl(remote_qp_num);

    rc = modify_qp_to_rtr(res, qp, remote_qp_num);
    if (rc)
    {
        fprintf(stderr, "failed to modify qp to rtr\n");
        return -1;
    }

    rc = modify_qp_to_rts(qp);
    if (rc)
    {
        fprintf(stderr, "failed to modify qp state from reset to rts\n");
        return rc;
    }

    if (sock_sync_ready(sock, server))
    {
        fprintf(stderr, "sync after qps are moved to RTS\n");
        return 1;
    }
    return 0;
}

static void establish_qp_in_batch(struct resources *res, int sock, bool server)
{
    struct mem_data local_data, remote_data;

    gid_to_wire_gid(&(res->sgid), local_data.gid);
    local_data.addr = htonll((uintptr_t)res->buf);
    local_data.rkey = htonl(res->mr->rkey);

    if (sock_sync_data(sock, server, sizeof(struct mem_data), &local_data, &remote_data))
    {
        assert(0);
    }

    wire_gid_to_gid(remote_data.gid, &(res->dgid));
    remote_data.addr = ntohll(remote_data.addr);
    remote_data.rkey = ntohl(remote_data.rkey);

    for (int i = 0; i < res->qp_num_per_node; ++i)
    {
        struct ibv_qp_init_attr qp_init_attr;
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));

        qp_init_attr.send_cq = res->cq;
        qp_init_attr.recv_cq = res->cq;
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.sq_sig_all = 0;
        qp_init_attr.cap.max_send_wr = 32;
        qp_init_attr.cap.max_recv_wr = 32;
        qp_init_attr.cap.max_send_sge = 1;
        qp_init_attr.cap.max_recv_sge = 1;

        struct ibv_qp *qp = ibv_create_qp(res->pd, &qp_init_attr);
        if (!qp)
        {
            fprintf(stderr, "failed to create qp\n");
            exit(-1);
        }
        if (connect_qp(res, qp, sock, server))
        {
            fprintf(stderr, "failed to establish qp\n");
            exit(-1);
        }
        std::lock_guard<std::mutex> guard(res->lock);
        res->qp_vec.push_back(qp);
        res->remote_props_vec.push_back(remote_data);
    }
}

static void establish_server_connection(struct resources *res)
{
    int sock = sock_daemon_connect(config.tcp_port);
    if (sock < 0)
    {
        fprintf(stderr, "failed to establish TCP connection on port %d\n", config.tcp_port);
        exit(-1);
    }
    int queue_size = 100;
    listen(sock, queue_size);
    while (!res->stop)
    {
        int server_sock = accept(sock, NULL, 0);
        if (server_sock > 0)
        {
            res->server_sock_vec.push_back(server_sock);
            establish_qp_in_batch(res, server_sock, true);
        }
    }
    close(sock);
}

static int establish_client_connection(struct resources *res)
{
    if (res->remote_ip_vec.size() != 0)
    {
        for (auto ip : res->remote_ip_vec)
        {
            int client_sock = sock_client_connect(ip.c_str(), config.tcp_port);
            if (client_sock < 0)
            {
                fprintf(stderr, "failed to establish TCP connection to server %s,port %d\n", ip.c_str(), config.tcp_port);
                return -1;
            }
            else
            {
                res->client_sock_vec.push_back(client_sock);
                establish_qp_in_batch(res, client_sock, false);
            }
        }
    }
    return 0;
}

static void establish_server_and_client_connection(struct resources *res)
{
    res->server_thread = std::make_shared<std::thread>(establish_server_connection, res);
    usleep(res->wait_time_before_start);
    establish_client_connection(res);
}

static int event_one_completion(struct resources *res)
{
    struct ibv_wc wc;
    int rc = 0;
    rc = ibv_req_notify_cq(res->cq, 0);
    if (rc)
    {
        printf("Couldn't request CQ notification");
        return -1;
    }
    void *ev_ctx = NULL;
    rc = ibv_get_cq_event(res->cc, &(res->cq), &ev_ctx);
    if (rc)
    {
        printf("Failed to get CQ event, errno = %d\n", errno);
        return -1;
    }
    ibv_ack_cq_events(res->cq, 1);
    ibv_poll_cq(res->cq, 1, &wc);
    if (wc.status != IBV_WC_SUCCESS)
    {
        fprintf(stderr, "got bad compeltion with status:0x%x\n", wc.status);
        return -1;
    }
    return 0;
}

static int poll_one_completion(struct resources *res)
{
    struct ibv_wc wc;
    unsigned long start_time_msec;
    unsigned long cur_time_msec;
    struct timeval cur_time;
    int poll_result;
    int rc = 0;
    gettimeofday(&cur_time, NULL);
    start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    do
    {
        poll_result = ibv_poll_cq(res->cq, 1, &wc);
        gettimeofday(&cur_time, NULL);
        cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    } while ((poll_result == 0) && ((cur_time_msec - start_time_msec) < res->poll_time_out));
    if (poll_result < 0)
    {
        fprintf(stderr, "poll cq failed\n");
        rc = 1;
    }
    else if (poll_result == 0)
    {
        fprintf(stderr, "completion wasn't found in the cq after timeout\n");
        rc = 1;
    }
    else
    {
        if (wc.status != IBV_WC_SUCCESS)
        {
            fprintf(stderr, "got bad compeltion with status:0x%x,vendor syndrome:0x%x\n", wc.status, wc.vendor_err);
            rc = 1;
        }
    }
    return rc;
}

static int poll_completion(struct resources *res)
{
    if (res->event_or_poll == 0)
        return poll_one_completion(res);
    else
        return event_one_completion(res);
}

static int warm_up(struct resources *res)
{
    int i = 0;
    struct timeval now;
    for (; i < 1000; ++i)
    {
        gettimeofday(&now, NULL);
        srand(now.tv_sec + now.tv_usec);
        uint32_t qp_index = rand() % res->qp_vec.size();
        if (post_send(res, qp_index))
        {
            fprintf(stderr, "failed to post send in warm_up\n");
            assert(0);
        }
        if (poll_one_completion(res))
        {
            fprintf(stderr, "poll completion failed\n");
            assert(0);
        }
    }
}

static void cluster_task(struct resources *res)
{
    while (res->qp_vec.size() != ((res->cluster_size - 1) * res->qp_num_per_node))
        ;
    int circle = 0;
    ofstream file;
    file.open(std::to_string(res->qp_num_per_node), ios::out);
    if (res->perform_warmup == 1)
        warm_up(res);
    struct timeval now;
    for (;;)
    {
        start_usec = tick(0);
        for (int i = 0; i < res->test_num; ++i)
        {
            gettimeofday(&now, NULL);
            srand(now.tv_sec + now.tv_usec);
            uint32_t qp_index = rand() % res->qp_vec.size();
            if (post_send(res, qp_index))
            {
                fprintf(stderr, "failed to post sr\n");
                return;
            }
            if (poll_completion(res))
            {
                fprintf(stderr, "poll completion failed\n");
                return;
            }
        }
        if (res->verbose == 1)
        {
            end_usec = tick(start_usec);
            double th = res->test_num;
            th = (th * res->batch_size * 1000000) / end_usec;
            printf("%dth %d times %dB x %d write on %dB memory over %d QP, cost time %d us, throughput = %f ops\n", circle, res->test_num, res->io_size, res->batch_size, res->mem_size, res->qp_vec.size(), end_usec, th);
            file << circle << "th " << res->test_num << " times " << res->io_size << "B x " << res->batch_size << " write on " << res->mem_size << "B memory over " << res->qp_vec.size() << " QP, throughput = " << th << " ops\n";
        }
        ++circle;
        if (res->run_infinitely != 1 && res->iteration <= circle)
        {
            break;
        }
    }
    file.close();
}

static void cluster_barrier(struct resources *res)
{
    if (res->server_client == false) // client
    {
        if (res->server_sock_vec.size() != 0)
            for (auto fd : res->server_sock_vec)
                if (fd > 0)
                    sock_sync_ready(fd, res->server_client);
        if (res->client_sock_vec.size() != 0)
            for (auto fd : res->client_sock_vec)
                if (fd > 0)
                    sock_sync_ready(fd, res->server_client);
    }
    else
    {
        if (res->client_sock_vec.size() != 0)
            for (auto fd : res->client_sock_vec)
                if (fd > 0)
                    sock_sync_ready(fd, res->server_client);
        if (res->server_sock_vec.size() != 0)
            for (auto fd : res->server_sock_vec)
                if (fd > 0)
                    sock_sync_ready(fd, res->server_client);
    }
}

static int resources_destroy(struct resources *res)
{
    int rc = 0;
    res->stop = true;
    res->server_thread->join();
    if (res->cc)
    {
        if (ibv_destroy_comp_channel(res->cc))
        {
            fprintf(stderr, "failed to destroy comp channel\n");
            rc = 1;
        }
    }
    if (res->cq)
    {
        if (ibv_destroy_cq(res->cq))
        {
            fprintf(stderr, "failed to destroy cq\n");
            rc = 1;
        }
    }
    for (auto qp : res->qp_vec)
    {
        if (ibv_destroy_qp(qp))
        {
            fprintf(stderr, "failed to destroy qp\n");
            rc = 1;
        }
    }
    if (res->mr)
    {
        if (ibv_dereg_mr(res->mr))
        {
            fprintf(stderr, "failed to dereg mr\n");
            rc = 1;
        }
    }
    if (res->pd)
    {
        if (ibv_dealloc_pd(res->pd))
        {
            fprintf(stderr, "failed to dealloc pd\n");
            rc = 1;
        }
    }
    if (res->ib_ctx)
    {
        if (ibv_close_device(res->ib_ctx))
        {
            fprintf(stderr, "failed to close device\n");
            rc = 1;
        }
    }
    if (res->buf)
        free(res->buf);
    if (res->normal_buf)
        free(res->normal_buf);
    for (auto sock : res->server_sock_vec)
        if (close(sock))
        {
            fprintf(stderr, "failed to close socket\n");
            rc = 1;
        }
    for (auto sock : res->client_sock_vec)
        if (close(sock))
        {
            fprintf(stderr, "failed to close socket\n");
            rc = 1;
        }

    return rc;
}

static void usage(const char *argv0)
{
    fprintf(stdout, "Usage:0\n");
    fprintf(stdout, "Options:\n");
    fprintf(stdout, " -p, --port=<port> listen on /connect to port <post> (default 19875)");
}

static void resolve_remote_ip(struct resources *res)
{
    ifstream in;
    in.open(res->remote_ip_config, ios::in);
    if (in.is_open() == false)
    {
        fprintf(stderr, "No remote ip configuration file found! Make sure that file is done\n");
        exit(-1);
    }
    string line;
    while (std::getline(in, line))
    {
        res->remote_ip_vec.push_back(line);
    }
    in.close();

    return;
}

static void print_para(struct resources *res)
{
    std::cout << "tcp port:" << config.tcp_port << std::endl;
    std::cout << "mem size:" << res->mem_size << std::endl;
    std::cout << "io  size:" << res->io_size << std::endl;
    std::cout << "batch size:" << res->batch_size << std::endl;
    std::cout << "traffic class:" << res->traffic_class << std::endl;
    std::cout << "gid index:" << res->sgid_index << std::endl;
    std::cout << "verbose:" << res->verbose << std::endl;
    std::cout << "iteration:" << res->iteration << std::endl;
    std::cout << "device:" << config.dev_name << std::endl;
    std::cout << "ib port:" << config.ib_port << std::endl;
    std::cout << "cluster:" << res->cluster_size << std::endl;
    std::cout << "qp/node:" << res->qp_num_per_node << std::endl;
    std::cout << "ip configure file:" << res->remote_ip_config << std::endl;
    std::cout << "test num:" << res->test_num << std::endl;
    std::cout << "wait time before start:" << res->wait_time_before_start << std::endl;
    std::cout << "poll cq timeout:" << res->poll_time_out << std::endl;
    std::cout << "cq size:" << res->max_cq_size << std::endl;
    std::cout << "server or client:" << res->server_client << std::endl;
}

int main(int argc, char *argv[])
{

    struct resources res;
    int rc = 1;

    resources_init(&res);

    while (1)
    {
        int c;
        static struct option long_options[] = {
            {"tcp-port", 1, NULL, 'p'},
            {"mem-size", 1, NULL, 'm'},
            {"io-size", 1, NULL, 'o'},
            {"batch-size", 1, NULL, 'b'},
            {"traffic-class", 1, NULL, 't'},
            {"gid-index", 1, NULL, 'g'},
            {"verbose", 0, NULL, 'v'},
            {"iteration", 1, NULL, 'I'},
            {"ib-dev", 1, NULL, 'd'},
            {"ib-port", 1, NULL, 'i'},
            {"run-infinitely", 0, NULL, 'r'},
            {"warm-up", 0, NULL, 'w'},
            {"event", 0, NULL, 'e'},
            {"cluster", 1, NULL, 'c'},
            {"qp-per-node", 1, NULL, 'q'},
            {"ip-config", 1, NULL, 'f'},
            {"test-num", 1, NULL, 'T'},
            {"wait-time", 1, NULL, 'W'},
            {"server-client", 0, NULL, 's'},
            {NULL, 0, NULL, '\0'}};
        c = getopt_long(argc, argv, "p:m:o:b:t:g:vI:d:i:rwec:q:f:T:", long_options, NULL);
        if (c == -1)
            break;
        switch (c)
        {
        case 'p':
            config.tcp_port = strtoul(optarg, NULL, 0);
            break;
        case 'm':
            res.mem_size = strtoul(optarg, NULL, 0);
            break;
        case 'o':
            res.io_size = strtoul(optarg, NULL, 0);
            break;
        case 'b':
            res.batch_size = strtoul(optarg, NULL, 0);
            break;
        case 't':
            res.traffic_class = strtoul(optarg, NULL, 0);
            break;
        case 'g':
            res.sgid_index = strtoul(optarg, NULL, 0);
            break;
        case 'v':
            res.verbose = 1;
            break;
        case 'r':
            res.run_infinitely = 1;
            break;
        case 'I':
            res.iteration = strtoul(optarg, NULL, 0);
            break;
        case 'd':
            config.dev_name = strdup(optarg);
            break;
        case 'w':
            res.perform_warmup = 1;
            break;
        case 'e':
            res.event_or_poll = 1;
            break;
        case 'c':
            res.cluster_size = strtoul(optarg, NULL, 0);
            break;
        case 'q':
            res.qp_num_per_node = strtoul(optarg, NULL, 0);
            break;
        case 'f':
            res.remote_ip_config = strdup(optarg);
            break;
        case 'T':
            res.test_num = strtoul(optarg, NULL, 0);
            break;
        case 'W':
            res.wait_time_before_start = strtoul(optarg, NULL, 0);
            break;
        case 's':
            res.server_client = true;
            break;
        case 'i':
            config.ib_port = strtoul(optarg, NULL, 0);
            if (config.ib_port < 0)
            {
                usage(argv[0]);
                return 1;
            };
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    print_para(&res);
    resolve_remote_ip(&res);

    if (resources_create(&res))
    {
        fprintf(stderr, "failed to create resources\n");
        goto cleanup;
    }

    establish_server_and_client_connection(&res);

    cluster_task(&res);

    cluster_barrier(&res);
    rc = 0;
cleanup:
    if (resources_destroy(&res))
    {
        fprintf(stderr, "failed to destroy resources\n");
        rc = 1;
    }
    return rc;
}
