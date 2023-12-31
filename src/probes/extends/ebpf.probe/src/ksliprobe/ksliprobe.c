/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2021. All rights reserved.
 * gala-gopher licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: wo_cow
 * Create: 2022-4-14
 * Description: KSLI probe user prog
 ******************************************************************************/
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

#ifdef BPF_PROG_KERN
#undef BPF_PROG_KERN
#endif

#ifdef BPF_PROG_USER
#undef BPF_PROG_USER
#endif


#include "bpf.h"
#include "args.h"
#include "event.h"
#include "ipc.h"
#include "ksliprobe.skel.h"
#include "tc_loader.h"
#include "ksliprobe.h"

#define OO_NAME "sli"
#define DEFAULT_REDIS_PROC_NAME "redis"
#define SLI_TBL_NAME "redis_sli"
#define MAX_SLI_TBL_NAME "redis_max_sli"

struct ksli_probe_s {
    struct ipc_body_s ipc_body;
    struct bpf_prog_s* ksli_bpf_prog;
    int args_fd;
    int msg_evt_fd;
};

static struct ksli_probe_s g_ksli_probe = {0};
static volatile sig_atomic_t stop;

static void sig_int(int signo)
{
    stop = 1;
}

#define MS2NS(ms)   ((u64)(ms) * 1000000)
#define __ENTITY_ID_LEN 128

static void report_sli_event(struct msg_event_data_t *msg_evt_data)
{
    char entityId[__ENTITY_ID_LEN];
    u64 latency_thr_ns = MS2NS(g_ksli_probe.ipc_body.probe_param.latency_thr);
    unsigned char ser_ip_str[INET6_ADDRSTRLEN];
    unsigned char cli_ip_str[INET6_ADDRSTRLEN];
    struct event_info_s evt = {0};

    if (g_ksli_probe.ipc_body.probe_param.logs == 0) {
        return;
    }

    entityId[0] = 0;
    (void)snprintf(entityId, __ENTITY_ID_LEN, "%d_%d",
        msg_evt_data->conn_id.tgid,
        msg_evt_data->conn_id.fd);

    if ((latency_thr_ns > 0) && (latency_thr_ns < msg_evt_data->latency.rtt_nsec)) {
        ip_str(msg_evt_data->server_ip_info.family, (unsigned char *)&(msg_evt_data->server_ip_info.ipaddr),
            ser_ip_str, INET6_ADDRSTRLEN);
        ip_str(msg_evt_data->client_ip_info.family, (unsigned char *)&(msg_evt_data->client_ip_info.ipaddr),
            cli_ip_str, INET6_ADDRSTRLEN);

        evt.entityName = OO_NAME;
        evt.entityId = entityId;
        evt.metrics = "rtt_nsec";
        evt.pid = (int)msg_evt_data->conn_id.tgid;
        (void)snprintf(evt.ip, EVT_IP_LEN, "CIP(%s:%u), SIP(%s:%u)",
                       cli_ip_str,
                       ntohs(msg_evt_data->client_ip_info.port),
                       ser_ip_str,
                       msg_evt_data->server_ip_info.port);

        report_logs((const struct event_info_s *)&evt,
                    EVT_SEC_WARN,
                    "Process(TID:%d, CIP(%s:%u), SIP(%s:%u)) SLI(%s:%llu) exceed the threshold.",
                    msg_evt_data->conn_id.tgid,
                    cli_ip_str,
                    ntohs(msg_evt_data->client_ip_info.port),
                    ser_ip_str,
                    msg_evt_data->server_ip_info.port,
                    msg_evt_data->latency.command,
                    msg_evt_data->latency.rtt_nsec);
    }
}

static void msg_event_handler(void *ctx, int cpu, void *data, unsigned int size)
{
    struct msg_event_data_t *msg_evt_data = (struct msg_event_data_t *)data;
    unsigned char ser_ip_str[INET6_ADDRSTRLEN];
    unsigned char cli_ip_str[INET6_ADDRSTRLEN];
    //const char *protocol;

    report_sli_event(msg_evt_data);

    ip_str(msg_evt_data->server_ip_info.family, (unsigned char *)&(msg_evt_data->server_ip_info.ipaddr),
        ser_ip_str, INET6_ADDRSTRLEN);
    ip_str(msg_evt_data->client_ip_info.family, (unsigned char *)&(msg_evt_data->client_ip_info.ipaddr),
        cli_ip_str, INET6_ADDRSTRLEN);

    fprintf(stdout,
            "|%s|%d|%d|%s|%s|%s|%u|%s|%u|%llu|\n",
            SLI_TBL_NAME,
            msg_evt_data->conn_id.tgid,
            msg_evt_data->conn_id.fd,
            "REDIS",
            msg_evt_data->latency.command,
            ser_ip_str,
            msg_evt_data->server_ip_info.port,
            cli_ip_str,
            ntohs(msg_evt_data->client_ip_info.port),
            msg_evt_data->latency.rtt_nsec);
    if (g_ksli_probe.ipc_body.probe_param.continuous_sampling_flag) {
        fprintf(stdout,
            "|%s|%d|%d|%s|%s|%s|%u|%s|%u|%llu|\n",
            MAX_SLI_TBL_NAME,
            msg_evt_data->conn_id.tgid,
            msg_evt_data->conn_id.fd,
            "REDIS",
            msg_evt_data->max.command,
            ser_ip_str,
            msg_evt_data->server_ip_info.port,
            cli_ip_str,
            ntohs(msg_evt_data->client_ip_info.port),
            msg_evt_data->max.rtt_nsec);
    }

    (void)fflush(stdout);

    return;
}

static void *msg_event_receiver(void *arg)
{
    struct perf_buffer *pb;

    pb = create_pref_buffer(g_ksli_probe.msg_evt_fd, msg_event_handler);
    if (pb == NULL) {
        fprintf(stderr, "Failed to create perf buffer.\n");
        stop = 1;
        return NULL;
    }

    poll_pb(pb, g_ksli_probe.ipc_body.probe_param.period * 1000);

    stop = 1;
    return NULL;
}

static int init_conn_mgt_process()
{
    int err;
    pthread_t msg_evt_hdl_thd;

    // 启动读写消息事件处理程序
    err = pthread_create(&msg_evt_hdl_thd, NULL, msg_event_receiver, NULL);
    if (err != 0) {
        fprintf(stderr, "Failed to create connection read/write message event handler thread.\n");
        return -1;
    }
    (void)pthread_detach(msg_evt_hdl_thd);
    printf("Connection read/write message event handler thread successfully started!\n");

    return 0;
}

static void load_args(int args_fd, struct ipc_body_s* ipc_body)
{
    __u32 key = 0;
    struct ksli_args_s args = {0};
    args.period = NS(ipc_body->probe_param.period);
    args.continuous_sampling_flag = ipc_body->probe_param.continuous_sampling_flag;

    (void)bpf_map_update_elem(args_fd, &key, &args, BPF_ANY);
}

static void reload_tc_bpf(struct ipc_body_s* ipc_body)
{
#ifdef KERNEL_SUPPORT_TSTAMP
    if (strcmp(g_ksli_probe.ipc_body.probe_param.netcard_list, ipc_body->probe_param.netcard_list) != 0) {
        offload_tc_bpf(TC_TYPE_INGRESS);
        load_tc_bpf(ipc_body->probe_param.netcard_list, TC_PROG, TC_TYPE_INGRESS);
    }
#endif
    return;
}

static int load_ksli_bpf_prog()
{
    struct bpf_prog_s *prog;

    prog = alloc_bpf_prog();
    if (prog == NULL) {
        return -1;
    }

    LOAD(ksliprobe, ksliprobe, err);
    prog->skels[prog->num].skel = ksliprobe_skel;
    prog->skels[prog->num].fn = (skel_destroy_fn)ksliprobe_bpf__destroy;
    prog->num++;

    g_ksli_probe.args_fd = GET_MAP_FD(ksliprobe, args_map);
    if (g_ksli_probe.args_fd <= 0) {
        fprintf(stderr, "ERROR: Failed to get args map fd.\n");
        goto err;
    }
    g_ksli_probe.msg_evt_fd = GET_MAP_FD(ksliprobe, msg_event_map);
    if (g_ksli_probe.msg_evt_fd <= 0) {
        fprintf(stderr, "ERROR: Failed to get msg event map fd.\n");
        goto err;
    }
    g_ksli_probe.ksli_bpf_prog = prog;

    return 0;
err:
    UNLOAD(ksliprobe);
    unload_bpf_prog(&prog);
    return -1;
}

static int init_probe_first_load(bool is_first_load)
{
    int err = 0;

    if (!is_first_load) {
        return 0;
    }

    err = load_ksli_bpf_prog();
    if (err) {
        return err;
    }
    // 初始化连接管理程序
    err = init_conn_mgt_process();
    if (err != 0) {
        fprintf(stderr, "Init connection management process failed.\n");
        return err;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int err = 0;
    struct ipc_body_s ipc_body;
    int msq_id;
    bool is_first_load = true;

    msq_id = create_ipc_msg_queue(IPC_EXCL);
    if (msq_id < 0) {
        return -1;
    }

    INIT_BPF_APP(ksliprobe, EBPF_RLIM_LIMITED);
    
    if (signal(SIGINT, sig_int) == SIG_ERR) {
        ERROR("[KSLIPROBE]: Can't set signal handler: %d\n", errno);
        return -1;
    }

#ifndef KERNEL_SUPPORT_TSTAMP
    INFO("[KSLIPROBE]: The kernel version does not support loading the tc tstamp program\n");
#endif
    INFO("[KSLIPROBE]: SLI probe successfully started!\n");

    while (!stop) {
        err = recv_ipc_msg(msq_id, (long)PROBE_KSLI, &ipc_body);
        if (err == 0) {
            reload_tc_bpf(&ipc_body);

            err = init_probe_first_load(is_first_load);
            if (err) {
                goto err;
            }
            is_first_load = false;

            load_args(g_ksli_probe.args_fd, &ipc_body);
            destroy_ipc_body(&g_ksli_probe.ipc_body);
            (void)memcpy(&g_ksli_probe.ipc_body, &ipc_body, sizeof(struct ipc_body_s));
        }

        sleep(DEFAULT_PERIOD);
    }

err:
    unload_bpf_prog(&g_ksli_probe.ksli_bpf_prog);
#ifdef KERNEL_SUPPORT_TSTAMP
    offload_tc_bpf(TC_TYPE_INGRESS);
#endif
    destroy_ipc_body(&g_ksli_probe.ipc_body);
    return -err;
}
