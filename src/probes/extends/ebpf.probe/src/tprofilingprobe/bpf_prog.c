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
 * Author: algorithmofdish
 * Create: 2023-04-03
 * Description: the user-side program of thread profiling probe
 ******************************************************************************/
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#ifdef BPF_PROG_KERN
#undef BPF_PROG_KERN
#endif

#ifdef BPF_PROG_USER
#undef BPF_PROG_USER
#endif

#include "bpf.h"
#include "args.h"
#include "profiling_event.h"
#include "tprofiling.h"
#include "bpf_prog.h"
#include "syscall_file.skel.h"
#include "syscall_net.skel.h"
#include "syscall_lock.skel.h"
#include "syscall_sched.skel.h"
#include "oncpu.skel.h"

#define __LOAD_SYSCALL_PROBE(probe_name, end, load) \
    OPEN(probe_name, end, load); \
    MAP_SET_PIN_PATH(probe_name, event_map, SYSCALL_EVENT_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, setting_map, SETTING_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, proc_filter_map, PROC_FILTER_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, thrd_bl_map, THRD_BL_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, stack_map, STACK_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, syscall_enter_map, SYSCALL_ENTER_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, syscall_stash_map, SYSCALL_STASH_MAP_PATH, load); \
    LOAD_ATTACH(probe_name, end, load)

#define __LOAD_ONCPU_PROBE(probe_name, end, load) \
    OPEN(probe_name, end, load); \
    MAP_SET_PIN_PATH(probe_name, event_map, ONCPU_EVENT_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, setting_map, SETTING_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, proc_filter_map, PROC_FILTER_MAP_PATH, load); \
    MAP_SET_PIN_PATH(probe_name, thrd_bl_map, THRD_BL_MAP_PATH, load); \
    LOAD_ATTACH(probe_name, end, load)

static char is_load_probe(struct probe_params *args, u32 probe)
{
    if (args->load_probe & probe) {
        return 1;
    }
    return 0;
}

static void perf_event_handler(void *ctx, int cpu, void *data, __u32 size)
{
    output_profiling_event((trace_event_data_t *)data);
}

static int load_syscall_create_pb(struct bpf_prog_s *prog, int fd)
{
    struct perf_buffer *pb;

    if (prog->pb == NULL) {
        pb = create_pref_buffer(fd, perf_event_handler);
        if (pb == NULL) {
            fprintf(stderr, "ERROR: create perf buffer failed\n");
            return -1;
        }
        prog->pb = pb;
        printf("INFO: Success to create syscall pb buffer.\n");
    }

    return 0;
}

static int __load_syscall_file_bpf_prog(struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_SYSCALL_PROBE(syscall_file, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_file_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_file_bpf__destroy;
        prog->num++;

        ret = load_syscall_create_pb(prog, GET_MAP_FD(syscall_file, event_map));
    }

    return ret;
err:
    UNLOAD(syscall_file);
    return -1;
}

static int __load_syscall_net_bpf_prog(struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_SYSCALL_PROBE(syscall_net, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_net_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_net_bpf__destroy;
        prog->num++;

        ret = load_syscall_create_pb(prog, GET_MAP_FD(syscall_net, event_map));
    }

    return ret;
err:
    UNLOAD(syscall_net);
    return -1;
}

static int __load_syscall_lock_bpf_prog(struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_SYSCALL_PROBE(syscall_lock, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_lock_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_lock_bpf__destroy;
        prog->num++;

        ret = load_syscall_create_pb(prog, GET_MAP_FD(syscall_lock, event_map));
    }

    return ret;
err:
    UNLOAD(syscall_lock);
    return -1;
}

static int __load_syscall_sched_bpf_prog(struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_SYSCALL_PROBE(syscall_sched, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = syscall_sched_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)syscall_sched_bpf__destroy;
        prog->num++;

        ret = load_syscall_create_pb(prog, GET_MAP_FD(syscall_sched, event_map));
    }

    return ret;
err:
    UNLOAD(syscall_sched);
    return -1;
}

struct bpf_prog_s *load_syscall_bpf_prog(struct probe_params *params)
{
    struct bpf_prog_s *prog;
    char is_load_syscall_file, is_load_syscall_net;
    char is_load_syscall_lock, is_load_syscall_sched;

    is_load_syscall_file = is_load_probe(params, TPROFILING_PROBE_SYSCALL_FILE);
    is_load_syscall_net = is_load_probe(params, TPROFILING_PROBE_SYSCALL_NET);
    is_load_syscall_lock = is_load_probe(params, TPROFILING_PROBE_SYSCALL_LOCK);
    is_load_syscall_sched = is_load_probe(params, TPROFILING_PROBE_SYSCALL_SCHED);

    prog = alloc_bpf_prog();
    if (prog == NULL) {
        return NULL;
    }

    if (__load_syscall_file_bpf_prog(prog, is_load_syscall_file)) {
        goto err;
    }

    if (__load_syscall_net_bpf_prog(prog, is_load_syscall_net)) {
        goto err;
    }

    if (__load_syscall_lock_bpf_prog(prog, is_load_syscall_lock)) {
        goto err;
    }

    if (__load_syscall_sched_bpf_prog(prog, is_load_syscall_sched)) {
        goto err;
    }

    return prog;

err:
    unload_bpf_prog(&prog);
    return NULL;
}

static int load_oncpu_create_pb(struct bpf_prog_s *prog, int fd)
{
    struct perf_buffer *pb;

    if (prog->pb == NULL) {
        pb = create_pref_buffer(fd, perf_event_handler);
        if (pb == NULL) {
            fprintf(stderr, "ERROR: create perf buffer failed\n");
            return -1;
        }
        prog->pb = pb;
        printf("INFO: Success to create oncpu pb buffer.\n");
    }

    return 0;
}

static int __load_oncpu_bpf_prog(struct bpf_prog_s *prog, char is_load)
{
    int ret = 0;

    __LOAD_ONCPU_PROBE(oncpu, err, is_load);
    if (is_load) {
        prog->skels[prog->num].skel = oncpu_skel;
        prog->skels[prog->num].fn = (skel_destroy_fn)oncpu_bpf__destroy;
        prog->num++;

        ret = load_oncpu_create_pb(prog, GET_MAP_FD(oncpu, event_map));
    }

    return ret;
err:
    UNLOAD(oncpu);
    return -1;
}

struct bpf_prog_s *load_oncpu_bpf_prog(struct probe_params *params)
{

    struct bpf_prog_s *prog;
    char is_load_oncpu;

    is_load_oncpu = is_load_probe(params, TPROFILING_PROBE_ONCPU);

    prog = alloc_bpf_prog();
    if (prog == NULL) {
        return NULL;
    }

    if (__load_oncpu_bpf_prog(prog, is_load_oncpu)) {
        goto err;
    }

    return prog;

err:
    unload_bpf_prog(&prog);
    return NULL;
}