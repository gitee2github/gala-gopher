/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * gala-gopher licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: luzhihao
 * Create: 2022-07-13
 * Description: process probe
 ******************************************************************************/
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/resource.h>

#ifdef BPF_PROG_KERN
#undef BPF_PROG_KERN
#endif

#ifdef BPF_PROG_USER
#undef BPF_PROG_USER
#endif

#include "bpf.h"
#include "args.h"
#include "proc.h"
#include "task.h"
#include "event.h"
#include "syscall.skel.h"
#include "syscall_io.skel.h"
#include "syscall_net.skel.h"
#include "syscall_fork.skel.h"
#include "syscall_sched.skel.h"
#include "ex4.skel.h"
#include "overlay.skel.h"
#include "tmpfs.skel.h"
#include "page.skel.h"
#include "proc_io.skel.h"
#include "bpf_prog.h"

#ifdef OO_NAME
#undef OO_NAME
#endif
#define OO_NAME  "proc"

static struct ipc_body_s *__ipc_body = NULL;

#define PROC_TBL_SYSCALL        "proc_syscall"
#define PROC_TBL_SYSCALL_IO     "proc_syscall_io"
#define PROC_TBL_SYSCALL_NET    "proc_syscall_net"
#define PROC_TBL_SYSCALL_SCHED  "proc_syscall_sched"
#define PROC_TBL_SYSCALL_FORK   "proc_syscall_fork"

#define PROC_TBL_EXT4           "proc_ext4"
#define PROC_TBL_OVERLAY        "proc_overlay"
#define PROC_TBL_TMPFS          "proc_tmpfs"

#define PROC_TBL_PAGE           "proc_page"
#define PROC_TBL_DNS            "proc_dns"

#define PROC_TBL_IO             "proc_io"

#define OVERLAY_MOD  "overlay"
#define EXT4_MOD  "ext4"

#define __LOAD_PROBE(probe_name, end, load) \
    OPEN(probe_name, end, load); \
    MAP_SET_PIN_PATH(probe_name, args_map, ARGS_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, g_proc_map, PROC_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, g_proc_output, PROC_OUTPUT_PATH, load); \
    LOAD_ATTACH(taskprobe, probe_name, end, load)

static void report_proc_metrics(struct proc_data_s *proc)
{
    char entityId[INT_LEN];
    u64 latency_thr_us = US(__ipc_body->probe_param.latency_thr);
    struct event_info_s evt = {0};

    if (__ipc_body->probe_param.logs == 0) {
        return;
    }

    entityId[0] = 0;
    (void)snprintf(entityId, INT_LEN, "%d", proc->proc_id);

    evt.entityName = OO_NAME;
    evt.entityId = entityId;
    evt.pid = (int)proc->proc_id;

    if (proc->syscall.failed > 0) {
        evt.metrics = "syscall_failed";
        report_logs((const struct event_info_s *)&evt,
                    EVT_SEC_WARN,
                    "Process(COMM:%s PID:%u) syscall failed(SysCall-ID:%d RET:%d COUNT:%u).",
                    proc->comm,
                    proc->proc_id,
                    proc->syscall.last_syscall_id,
                    proc->syscall.last_ret_code,
                    proc->syscall.failed);
    }

    if (proc->dns_op.gethostname_failed > 0) {
        evt.metrics = "gethostname_failed";
        report_logs((const struct event_info_s *)&evt,
                    EVT_SEC_WARN,
                    "Process(COMM:%s PID:%u) gethostname failed(COUNT:%u).",
                    proc->comm,
                    proc->proc_id,
                    proc->dns_op.gethostname_failed);
    }

    if (latency_thr_us > 0 && proc->proc_io.iowait_us > latency_thr_us) {
        evt.metrics = "iowait_us";
        report_logs((const struct event_info_s *)&evt,
                    EVT_SEC_WARN,
                    "Process(COMM:%s PID:%u) iowait %llu us.",
                    proc->comm,
                    proc->proc_id,
                    proc->proc_io.iowait_us);
    }

    if (proc->proc_io.hang_count > 0) {
        evt.metrics = "hang_count";
        report_logs((const struct event_info_s *)&evt,
                    EVT_SEC_WARN,
                    "Process(COMM:%s PID:%u) hang count %u.",
                    proc->comm,
                    proc->proc_id,
                    proc->proc_io.hang_count);
    }

    if (proc->proc_io.bio_err_count > 0) {
        evt.metrics = "bio_err_count";
        report_logs((const struct event_info_s *)&evt,
                    EVT_SEC_WARN,
                    "Process(COMM:%s PID:%u) bio error %u.",
                    proc->comm,
                    proc->proc_id,
                    proc->proc_io.bio_err_count);
    }
}

static void output_proc_metrics_syscall(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|%u|\n",
        PROC_TBL_SYSCALL,
        proc->proc_id,

        proc->syscall.failed);
}

static void output_proc_metrics_syscall_io(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|%llu|%llu|%llu|\n",
        PROC_TBL_SYSCALL_IO,
        proc->proc_id,

        proc->syscall.ns_mount,
        proc->syscall.ns_umount,
        proc->syscall.ns_read,
        proc->syscall.ns_write,
        proc->syscall.ns_fsync);
}

static void output_proc_metrics_syscall_net(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|\n",
        PROC_TBL_SYSCALL_NET,
        proc->proc_id,

        proc->syscall.ns_sendmsg,
        proc->syscall.ns_recvmsg);
}

static void output_proc_metrics_syscall_sched(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|%llu|%llu|\n",
        PROC_TBL_SYSCALL_SCHED,
        proc->proc_id,

        proc->syscall.ns_sched_yield,
        proc->syscall.ns_futex,
        proc->syscall.ns_epoll_wait,
        proc->syscall.ns_epoll_pwait);
}

static void output_proc_metrics_syscall_fork(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|%llu|\n",

        PROC_TBL_SYSCALL_FORK,
        proc->proc_id,

        proc->syscall.ns_fork,
        proc->syscall.ns_vfork,
        proc->syscall.ns_clone);
}

static void output_proc_metrics_ext4(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|%llu|%llu|\n",

        PROC_TBL_EXT4,
        proc->proc_id,

        proc->op_ext4.ns_read,
        proc->op_ext4.ns_write,
        proc->op_ext4.ns_open,
        proc->op_ext4.ns_flush);
}

static void output_proc_metrics_overlay(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|%llu|%llu|\n",

        PROC_TBL_OVERLAY,
        proc->proc_id,

        proc->op_overlay.ns_read,
        proc->op_overlay.ns_write,
        proc->op_overlay.ns_open,
        proc->op_overlay.ns_flush);
}

static void output_proc_metrics_tmpfs(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|%llu|\n",

        PROC_TBL_TMPFS,
        proc->proc_id,

        proc->op_tmpfs.ns_read,
        proc->op_tmpfs.ns_write,
        proc->op_tmpfs.ns_flush);
}

static void output_proc_metrics_page(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|%llu|%llu|%llu|\n",
        PROC_TBL_PAGE,
        proc->proc_id,

        proc->page_op.reclaim_ns,
        proc->page_op.count_access_pagecache,
        proc->page_op.count_mark_buffer_dirty,
        proc->page_op.count_load_page_cache,
        proc->page_op.count_mark_page_dirty);
}

static void output_proc_metrics_dns(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%llu|%llu|\n",
        PROC_TBL_DNS,
        proc->proc_id,

        proc->dns_op.gethostname_failed,
        proc->dns_op.gethostname_ns);
}

static void output_proc_io_stats(struct proc_data_s *proc)
{
    (void)fprintf(stdout,
        "|%s|%u|"
        "%u|%u|%u|%u|"
        "%u|%u|%u|%llu|\n",
        PROC_TBL_IO,
        proc->proc_id,

        proc->proc_io.less_4k_io_read,
        proc->proc_io.less_4k_io_write,
        proc->proc_io.greater_4k_io_read,
        proc->proc_io.greater_4k_io_write,

        proc->proc_io.bio_latency,
        proc->proc_io.bio_err_count,
        proc->proc_io.hang_count,
        proc->proc_io.iowait_us);
}

void output_proc_metrics(void *ctx, int cpu, void *data, __u32 size)
{
    struct proc_data_s *proc = (struct proc_data_s *)data;
    u32 flags = proc->flags;

    report_proc_metrics(proc);

    if (flags & TASK_PROBE_SYSCALL) {
        output_proc_metrics_syscall(proc);
    } else if (flags & TASK_PROBE_IO_SYSCALL) {
        output_proc_metrics_syscall_io(proc);
    } else if (flags & TASK_PROBE_NET_SYSCALL) {
        output_proc_metrics_syscall_net(proc);
    } else if (flags & TASK_PROBE_SCHED_SYSCALL) {
        output_proc_metrics_syscall_sched(proc);
    } else if (flags & TASK_PROBE_FORK_SYSCALL) {
        output_proc_metrics_syscall_fork(proc);
    } else if (flags & TASK_PROBE_EXT4_OP) {
        output_proc_metrics_ext4(proc);
    } else if (flags & TASK_PROBE_OVERLAY_OP) {
        output_proc_metrics_overlay(proc);
    } else if (flags & TASK_PROBE_TMPFS_OP) {
        output_proc_metrics_tmpfs(proc);
    } else if (flags & TASK_PROBE_PAGE_OP) {
        output_proc_metrics_page(proc);
    } else if (flags & TASK_PROBE_DNS_OP) {
        output_proc_metrics_dns(proc);
    } else if (flags & TASK_PROBE_IO) {
        output_proc_io_stats(proc);
    }

    (void)fflush(stdout);
    return;
}

static int load_proc_create_pb(struct bpf_prog_s* prog, int fd)
{
    struct perf_buffer *pb;

    if (prog->pb == NULL) {
        pb = create_pref_buffer(fd, output_proc_metrics);
        if (pb == NULL) {
            fprintf(stderr, "ERROR: crate perf buffer failed\n");
            return -1;
        }
        prog->pb = pb;
        INFO("Success to create proc pb buffer.\n");
    }
    return 0;
}

static int load_proc_syscall_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(syscall, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(syscall, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(syscall, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(syscall, g_proc_output));
    }

    return ret;
err:
    UNLOAD(syscall);
    return -1;
}

static int load_proc_syscall_io_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(syscall_io, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_io_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_io_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(syscall_io, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(syscall_io, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(syscall_io, g_proc_output));
    }

    return ret;
err:
    UNLOAD(syscall_io);
    return -1;
}

static int load_proc_syscall_net_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(syscall_net, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_net_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_net_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(syscall_net, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(syscall_net, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(syscall_net, g_proc_output));
    }

    return ret;
err:
    UNLOAD(syscall_net);
    return -1;
}

static int load_proc_syscall_fork_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(syscall_fork, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_fork_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_fork_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(syscall_fork, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(syscall_fork, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(syscall_fork, g_proc_output));
    }

    return ret;
err:
    UNLOAD(syscall_fork);
    return -1;
}

static int load_proc_syscall_sched_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(syscall_sched, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_sched_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_sched_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(syscall_sched, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(syscall_sched, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(syscall_sched, g_proc_output));
    }

    return ret;
err:
    UNLOAD(syscall_sched);
    return -1;
}

static int load_proc_ext4_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(ex4, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = ex4_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)ex4_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(ex4, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(ex4, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(ex4, g_proc_output));
    }

    return ret;
err:
    UNLOAD(ex4);
    return -1;
}

static int load_proc_overlay_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(overlay, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = overlay_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)overlay_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(overlay, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(overlay, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(overlay, g_proc_output));
    }

    return ret;
err:
    UNLOAD(overlay);
    return -1;
}

static int load_proc_tmpfs_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(tmpfs, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = tmpfs_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)tmpfs_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(tmpfs, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(tmpfs, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(tmpfs, g_proc_output));
    }

    return ret;
err:
    UNLOAD(tmpfs);
    return -1;
}

static int load_proc_page_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(page, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = page_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)page_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(page, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(page, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(page, g_proc_output));
    }

    return ret;
err:
    UNLOAD(page);
    return -1;
}

static int load_proc_io_prog(struct task_probe_s *task_probe, struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_PROBE(proc_io, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = proc_io_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)proc_io_bpf__destroy;
        prog->num++;

        task_probe->proc_map_fd = GET_MAP_FD(proc_io, g_proc_map);
        task_probe->args_fd = GET_MAP_FD(proc_io, args_map);

        ret = load_proc_create_pb(prog, GET_MAP_FD(proc_io, g_proc_output));
    }

    return ret;
err:
    UNLOAD(proc_io);
    return -1;
}

int load_proc_bpf_prog(struct task_probe_s *task_probe, struct ipc_body_s *ipc_body, struct bpf_prog_s **new_prog)
{
    struct bpf_prog_s *prog;
    char is_load = 0;
    char is_load_syscall, is_load_syscall_io, is_load_syscall_net;
    char is_load_syscall_fork, is_load_syscall_sched, is_load_proc_io;
    char is_load_overlay, is_load_ext4, is_load_tmpfs, is_load_page;

    *new_prog = NULL;
    __ipc_body = ipc_body;

    is_load_overlay = (ipc_body->probe_range_flags & PROBE_RANGE_PROC_FS) & is_exist_mod(OVERLAY_MOD);
    is_load_ext4 = (ipc_body->probe_range_flags & PROBE_RANGE_PROC_FS) & is_exist_mod(EXT4_MOD);

    is_load_syscall = ipc_body->probe_range_flags & PROBE_RANGE_PROC_SYSCALL;
    is_load_syscall_io = ipc_body->probe_range_flags & PROBE_RANGE_PROC_IO;
    is_load_syscall_net = ipc_body->probe_range_flags & PROBE_RANGE_PROC_NET;
    is_load_syscall_fork = ipc_body->probe_range_flags & PROBE_RANGE_PROC_SYSCALL;
    is_load_syscall_sched = ipc_body->probe_range_flags & PROBE_RANGE_PROC_SYSCALL;

    is_load_tmpfs = ipc_body->probe_range_flags & PROBE_RANGE_PROC_FS;
    is_load_page = ipc_body->probe_range_flags & PROBE_RANGE_PROC_PAGECACHE;

    is_load_proc_io = ipc_body->probe_range_flags & PROBE_RANGE_PROC_IO;

    is_load = is_load_overlay | is_load_ext4 | is_load_syscall | is_load_syscall_io | is_load_syscall_net;
    is_load |= is_load_syscall_fork | is_load_syscall_sched | is_load_tmpfs | is_load_page | is_load_proc_io;
    if (!is_load) {
        return 0;
    }

    prog = alloc_bpf_prog();
    if (prog == NULL) {
        return -1;
    }

    if (load_proc_syscall_prog(task_probe, prog, is_load_syscall)) {
        goto err;
    }

    if (load_proc_syscall_io_prog(task_probe, prog, is_load_syscall_io)) {
        goto err;
    }

    if (load_proc_syscall_net_prog(task_probe, prog, is_load_syscall_net)) {
        goto err;
    }

    if (load_proc_syscall_sched_prog(task_probe, prog, is_load_syscall_sched)) {
        goto err;
    }

    if (load_proc_syscall_fork_prog(task_probe, prog, is_load_syscall_fork)) {
        goto err;
    }

    if (load_proc_ext4_prog(task_probe, prog, is_load_ext4)) {
        goto err;
    }

    if (load_proc_overlay_prog(task_probe, prog, is_load_overlay)) {
        goto err;
    }

    if (load_proc_tmpfs_prog(task_probe, prog, is_load_tmpfs)) {
        goto err;
    }

    if (load_proc_page_prog(task_probe, prog, is_load_page)) {
        goto err;
    }

    if (load_proc_io_prog(task_probe, prog, is_load_proc_io)) {
        goto err;
    }

    *new_prog = prog;
    return 0;

err:
    unload_bpf_prog(&prog);
    __ipc_body = NULL;
    return -1;
}

