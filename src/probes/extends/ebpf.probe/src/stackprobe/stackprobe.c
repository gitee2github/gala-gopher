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
 * Create: 2022-08-22
 * Description: stack probe user prog
 ******************************************************************************/
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#include <linux/perf_event.h>
#include <linux/unistd.h>

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

#ifdef BPF_PROG_KERN
#undef BPF_PROG_KERN
#endif

#ifdef BPF_PROG_USER
#undef BPF_PROG_USER
#endif

#include "bpf.h"
#include "args.h"
#include "logs.h"
#include "syscall.h"
#include "symbol.h"
#include "flame_graph.h"
#include "debug_elf_reader.h"
#include "elf_symb.h"
#include "stackprobe_conf.h"
#include "stackprobe.h"

#define ON_CPU_PROG    "./oncpu.bpf.o"
#define OFF_CPU_PROG   "./offcpu.bpf.o"
#define IO_PROG        "./io.bpf.o"

#define IS_IEG_ADDR(addr)     ((addr) != 0xcccccccccccccccc && (addr) != 0xffffffffffffffff)

#define BPF_GET_MAP_FD(obj, map_name)   \
            ({ \
                int __fd = -1; \
                struct bpf_map *__map = bpf_object__find_map_by_name((obj), (map_name)); \
                if (__map) { \
                    __fd = bpf_map__fd(__map); \
                } \
                __fd; \
            })

#define BPF_PIN_MAP_PATH(obj, map_name, path)   \
            ({ \
                int __ret = -1; \
                struct bpf_map *__map = bpf_object__find_map_by_name((obj), (map_name)); \
                if (__map) { \
                    __ret = bpf_map__set_pin_path(__map, path); \
                } \
                __ret; \
            })

typedef struct {
    u32 sw;
    enum stack_svg_type_e en_type;
    char *flame_name;
    char *prog_name;

} FlameProc;

static struct probe_params params = {.period = DEFAULT_PERIOD};
static volatile sig_atomic_t g_stop;
static struct stack_trace_s *g_st = NULL;

static void sig_int(int signo)
{
    g_stop = 1;
}

#if 1

int stacktrace_create_log_mgr(struct stack_trace_s *st, const char *logDir)
{
    struct log_mgr_s* mgr = create_log_mgr(NULL, 0, 0);
    if (!mgr) {
        return -1;
    }

    (void)strncpy(mgr->debug_path, logDir, PATH_LEN - 1);

    if (init_log_mgr(mgr, 0)) {
        return -1;
    }

    st->log_mgr = (void *)mgr;

    return 0;
}

void stacktrace_destroy_log_mgr(struct stack_trace_s *st)
{
    if (!st->log_mgr) {
        return;
    }

    destroy_log_mgr(st->log_mgr);
    return;
}

#endif

#if 1
static int get_stack_map_fd(struct stack_trace_s *st)
{
    if (st->is_stackmap_a) {
        return st->stackmap_a_fd;
    } else {
        return st->stackmap_b_fd;
    }
}

static struct perf_buffer* get_pb(struct stack_trace_s *st, struct svg_stack_trace_s *svg_st)
{
    if (st->is_stackmap_a) {
        return svg_st->pb_a;
    } else {
        return svg_st->pb_b;
    }
}

#endif

#if 1   // Proc cache

static void __destroy_proc_cache(struct proc_cache_s *proc_cache)
{
    if (!proc_cache || !proc_cache->proc_symbs) {
        return;
    }

    proc_delete_all_symbs(proc_cache->proc_symbs);
    proc_cache->proc_symbs = NULL;
    return;
}

static void destroy_proc_cache_tbl(struct stack_trace_s *st)
{
    if (!st || !st->proc_cache) {
        return;
    }

    struct proc_cache_s *proc_hash_tbl = st->proc_cache;
    struct proc_cache_s *item, *tmp;

    H_ITER(proc_hash_tbl, item, tmp) {
        __destroy_proc_cache(item);
        H_DEL(proc_hash_tbl, item);
        (void)free(item);
    }
    st->proc_cache = NULL;
    (void)memset(st->proc_cache_mirro, 0, sizeof(struct proc_cache_s *) * PROC_CACHE_MAX_COUNT);
    return;
}

static int __aging_proc_cache(struct stack_trace_s *st, struct proc_cache_s *aging_item)
{
    struct proc_cache_s *item = NULL;
    H_FIND(st->proc_cache, &(aging_item->k), sizeof(struct stack_pid_s), item);
    if (item) {
        st->stats.count[STACK_STATS_PCACHE_DEL]++;
        __destroy_proc_cache(item);
        H_DEL(st->proc_cache, item);
        (void)free(item);
        return 0;
    }
    return -1;
}

static int __add_proc_cache_mirro(struct stack_trace_s *st, struct proc_cache_s *new_item)
{
    struct proc_cache_s *aging_item;
    if (st->proc_cache_mirro_count < PROC_CACHE_MAX_COUNT) {
        st->proc_cache_mirro[st->proc_cache_mirro_count] = new_item;
        st->proc_cache_mirro_count++;
        return 0;
    }

    aging_item = st->proc_cache_mirro[0];   // Aging based on the creation timing
    for (int i = 1; i < PROC_CACHE_MAX_COUNT; i++) {
        st->proc_cache_mirro[i - 1] = st->proc_cache_mirro[i];
    }

    st->proc_cache_mirro[PROC_CACHE_MAX_COUNT - 1] = new_item;
    return __aging_proc_cache(st, aging_item);
}

static struct proc_cache_s* __search_proc_cache(struct stack_trace_s *st, struct stack_pid_s *stack_pid)
{
    struct proc_cache_s *item = NULL;
    H_FIND(st->proc_cache, stack_pid, sizeof(struct stack_pid_s), item);
    return item;
}

static struct proc_cache_s* __create_proc_cache(struct stack_trace_s *st, struct stack_pid_s *stack_pid)
{
    struct proc_cache_s *new_item;
    struct proc_symbs_s* proc_symbs;

    proc_symbs = proc_load_all_symbs(st->elf_reader, stack_pid->proc_id, stack_pid->comm);
    if (!proc_symbs) {
        return NULL;
    }

    new_item = (struct proc_cache_s *)malloc(sizeof(struct proc_cache_s));
    if (!new_item) {
        return NULL;
    }

    (void)memcpy(&new_item->k, stack_pid, sizeof(struct stack_pid_s));
    new_item->proc_symbs = proc_symbs;
    H_ADD_KEYPTR(st->proc_cache, &new_item->k, sizeof(struct stack_pid_s), new_item);
    st->stats.count[STACK_STATS_PCACHE_CRT]++;

    if (__add_proc_cache_mirro(st, new_item)) {
        // The program continues.
        ERROR("[STACKPROBE]: Proc cache add failed.\n");
    }
    return new_item;
}

static int search_user_addr_symb(struct stack_trace_s *st,
        struct stack_pid_s *stack_pid, u64 addr, struct addr_symb_s *addr_symb)
{
    struct proc_cache_s* proc_cache;

    proc_cache = __search_proc_cache(st, stack_pid);
    if (!proc_cache) {
        proc_cache = __create_proc_cache(st, stack_pid);
    }

    if (!proc_cache || !proc_cache->proc_symbs) {
        return -1;
    }

    return proc_search_addr_symb(proc_cache->proc_symbs, addr, addr_symb);
}

#endif

#if 1

static void clear_raw_stack_trace(struct stack_trace_s *st)
{
    if (!st || !st->raw_stack_traces) {
        return;
    }

    st->raw_stack_traces->raw_trace_count = 0;
}

static void destroy_raw_stack_trace(struct stack_trace_s *st)
{
    if (!st || !st->raw_stack_traces) {
        return;
    }

    (void)free(st->raw_stack_traces);
    st->raw_stack_traces = NULL;
    return;
}

static struct raw_stack_trace_s *create_raw_stack_trace(struct stack_trace_s *st)
{
    struct raw_stack_trace_s *raw_stack_trace;

    size_t stack_size = st->cpus_num * PERCPU_SAMPLE_COUNT;
    size_t mem_size = sizeof(struct raw_stack_trace_s);
    mem_size += (stack_size * sizeof(struct stack_id_s));

    raw_stack_trace = (struct raw_stack_trace_s *)malloc(mem_size);
    if (!raw_stack_trace) {
        return NULL;
    }
    (void)memset(raw_stack_trace, 0, mem_size);
    raw_stack_trace->stack_size = stack_size;
    return raw_stack_trace;
}

static int add_raw_stack_id(struct stack_trace_s *st, struct stack_id_s *raw_stack_id)
{
    if (!st || !st->raw_stack_traces) {
        return -1;
    }
    struct raw_stack_trace_s *raw_stack_traces = st->raw_stack_traces;

    if (raw_stack_traces->raw_trace_count >= raw_stack_traces->stack_size) {
        return -1;
    }

    (void)memcpy(&(raw_stack_traces->raw_traces[raw_stack_traces->raw_trace_count]),
            raw_stack_id, sizeof(struct stack_id_s));
    raw_stack_traces->raw_trace_count++;
    return 0;
}

#endif

#if 1
static int __stack_addrsymbs2string(struct addr_symb_s *addr_symb, int first, char *p, int size)
{
    int ret;
    char *symb;
    if (size <= 0) {
        return -1;
    }

#if 1
    symb = addr_symb->sym ?: addr_symb->mod;
    if (first) {
        ret = snprintf(p, (size_t)size, "%s", symb);
    } else {
        ret = snprintf(p, (size_t)size, "; %s", symb);
    }

#else
    symb = addr_symb->sym;
    if (symb) {
        if (first) {
            ret = snprintf(p, (size_t)size, "%s", symb);
        } else {
            ret = snprintf(p, (size_t)size, "; %s", symb);
        }
    } else {
        if (first) {
            ret = snprintf(p, (size_t)size, "0x%llx", addr_symb->orign_addr);
        } else {
            ret = snprintf(p, (size_t)size, "; 0x%llx", addr_symb->orign_addr);
        }
    }
#endif
    return (ret > 0 && ret < size) ? (ret) : -1;
}

static int __stack_symbs2string(struct stack_symbs_s *stack_symbs, char symbos_str[], size_t size)
{
    int len;
    int first_flag = 1;
    int remain_len = size;
    char *pos = symbos_str;
    struct addr_symb_s *addr_symb;

    for (int i = 0; i < PERF_MAX_STACK_DEPTH; i++) {
        addr_symb = &(stack_symbs->user_stack_symbs[i]);
        if (addr_symb->orign_addr != 0) {
            len = __stack_addrsymbs2string(addr_symb, first_flag, pos, remain_len);
            if (len < 0) {
                return -1;
            }

            remain_len -= len;
            pos += len;
            first_flag = 0;
        }
    }

    for (int i = 0; i < PERF_MAX_STACK_DEPTH; i++) {
        addr_symb = &(stack_symbs->kern_stack_symbs[i]);
        if (addr_symb->orign_addr != 0) {
            len = __stack_addrsymbs2string(addr_symb, first_flag, pos, remain_len);
            if (len < 0) {
                return -1;
            }

            remain_len -= len;
            pos += len;
            first_flag = 0;
        }
    }

    return 0;
}

static int add_stack_histo(struct stack_trace_s *st, struct stack_symbs_s *stack_symbs)
{
    char str[STACK_SYMBS_LEN];
    struct stack_trace_histo_s *item = NULL, *new_item;

    str[0] = 0;
    if (__stack_symbs2string(stack_symbs, str, STACK_SYMBS_LEN)) {
        // Statistic error, but program continues
        st->stats.count[STACK_STATS_HISTO_ERR]++;
    }

    if (str[0] == 0) {
#ifdef GOPHER_DEBUG
        ERROR("[STACKPROBE]: symbs2str is null(proc = %d, comm = %s).\n",
                stack_symbs->pid.proc_id, stack_symbs->pid.comm);
#endif
        return -1;
    }

    for (int i = 0; i < STACK_SVG_MAX; i++) {
        if (st->svg_stack_traces[i] == NULL) {
            continue;
        }
        H_FIND_S(st->svg_stack_traces[i]->histo_tbl, str, item);
        if (item) {
            st->stats.count[STACK_STATS_HISTO_FOLDED]++;
            item->count++;
            return 0;
        }

        new_item = (struct stack_trace_histo_s *)malloc(sizeof(struct stack_trace_histo_s));
        if (!new_item) {
            return -1;
        }

        new_item->stack_symbs_str[0] = 0;
        (void)strncpy(new_item->stack_symbs_str, str, STACK_SYMBS_LEN - 1);
        new_item->count = 1;
        H_ADD_S(st->svg_stack_traces[i]->histo_tbl, stack_symbs_str, new_item);
    }

    return 0;
}

static void clear_stack_histo(struct svg_stack_trace_s *svg_st)
{
    if (!svg_st || !svg_st->histo_tbl) {
        return;
    }

    struct stack_trace_histo_s *stack_trace_histo_tbl = svg_st->histo_tbl;
    struct stack_trace_histo_s *item, *tmp;

    H_ITER(stack_trace_histo_tbl, item, tmp) {
        H_DEL(stack_trace_histo_tbl, item);
        (void)free(item);
    }
    svg_st->histo_tbl = NULL;
}

#endif

#if 1
static int stack_id2symbs_user(struct stack_trace_s *st, struct stack_id_s *stack_id,
                                struct addr_symb_s usr_stack_symbs[], size_t size)
{
    int index = 0;
    u64 ip[PERF_MAX_STACK_DEPTH] = {0};
    int fd = get_stack_map_fd(st);

    if (bpf_map_lookup_elem(fd, &(stack_id->user_stack_id), ip) != 0) {
#ifdef GOPHER_DEBUG
        ERROR("[STACKPROBE]: Failed to id2symbs user stack(map_lkup).\n");
#endif
        st->stats.count[STACK_STATS_MAP_LKUP_ERR]++;
        return -1;
    }

    for (int i = PERF_MAX_STACK_DEPTH - 1; (i >= 0 && index < size); i--) {
        if (ip[i] != 0 && IS_IEG_ADDR(ip[i])) {
            if (search_user_addr_symb(st, &(stack_id->pid), ip[i], &(usr_stack_symbs[index]))) {
#ifdef GOPHER_DEBUG
                ERROR("[STACKPROBE]: Failed to id2symbs user stack(%s[0x%llx]).\n",
                    stack_id->pid.comm, ip[i]);
#endif
                st->stats.count[STACK_STATS_USR_ADDR_ERR]++;
                usr_stack_symbs[index].mod = stack_id->pid.comm;
            } else {
                st->stats.count[STACK_STATS_USR_ADDR]++;
            }
            index++;
        }
    }
    return 0;
}

#define __CPU_IDLE  "do_idle"
static char __is_cpu_idle(struct addr_symb_s *addr_symb)
{
    if (addr_symb && addr_symb->sym && !strcmp(addr_symb->sym, __CPU_IDLE)) {
        return 1;
    }
    return 0;
}

static int stack_id2symbs_kern(struct stack_trace_s *st, u32 kern_stack_id,
                                struct addr_symb_s kern_stack_symbs[], size_t size)
{
    int index = 0;
    u64 ip[PERF_MAX_STACK_DEPTH] = {0};
    int fd = get_stack_map_fd(st);

    if (bpf_map_lookup_elem(fd, &kern_stack_id, ip) != 0) {
#ifdef GOPHER_DEBUG
        ERROR("[STACKPROBE]: Failed to id2symbs kern stack(stack_id = %u).\n", kern_stack_id);
#endif
        st->stats.count[STACK_STATS_MAP_LKUP_ERR]++;
        return -1;
    }

    for (int i = PERF_MAX_STACK_DEPTH - 1; (i >= 0 && index < size); i--) {
        if (ip[i] != 0 && IS_IEG_ADDR(ip[i])) {
            if (search_kern_addr_symb(st->ksymbs, ip[i], &(kern_stack_symbs[index]))) {
#ifdef GOPHER_DEBUG
                ERROR("[STACKPROBE]: Failed to id2symbs kern stack(0x%llx).\n", ip[i]);
                st->stats.count[STACK_STATS_KERN_ADDR_ERR]++;
#endif
            } else {
                st->stats.count[STACK_STATS_KERN_ADDR]++;
            }

            if (__is_cpu_idle(&kern_stack_symbs[index])) {
                return 1;   // ignore cpu idle
            }

            index++;
        }
    }
    return 0;
}

static int stack_id2symbs(struct stack_trace_s *st, struct stack_id_s *stack_id, struct stack_symbs_s *stack_symbs)
{
    int ret;
    (void)memcpy(&(stack_symbs->pid), &(stack_id->pid), sizeof(struct stack_pid_s));

    if (stack_id->kern_stack_id >= 0) {
        ret = stack_id2symbs_kern(st, stack_id->kern_stack_id,
                    stack_symbs->kern_stack_symbs, PERF_MAX_STACK_DEPTH);
        if (ret) {
            return ret;
        }
    }

    if (stack_id->user_stack_id >= 0) {
        if (stack_id2symbs_user(st, stack_id,
                stack_symbs->user_stack_symbs, PERF_MAX_STACK_DEPTH)) {
            return -1;
        }
    }

    if ((stack_id->user_stack_id >= 0) && (stack_id->kern_stack_id >= 0)) {
        st->stats.count[STACK_STATS_USR_KERN_ADDR]++;
    }

    return 0;
}

static u64 __stack_count_symb(struct stack_trace_s *st)
{
    int i;
    u64 count = 0;
    struct mod_s* mod;
    struct proc_cache_s *item, *tmp;

    H_ITER(st->proc_cache, item, tmp) {
        if (!item->proc_symbs) {
            continue;
        }

        for (i = 0; i < item->proc_symbs->mods_count; i++) {
            mod = item->proc_symbs->mods[i];
            if (mod && mod->mod_symbs) {
                count += (u64)mod->mod_symbs->symbs_count;
            }

            if (mod && mod->debug_symbs) {
                count += (u64)mod->debug_symbs->symbs_count;
            }
        }
    }
    return count;
}

static int stack_id2histogram(struct stack_trace_s *st)
{
    int ret;
    struct stack_id_s *stack_id;
    struct stack_symbs_s stack_symbs;
    if (!st->raw_stack_traces) {
        return -1;
    }

    for (int i = 0; i < st->raw_stack_traces->raw_trace_count; i++) {
        stack_id = &(st->raw_stack_traces->raw_traces[i]);
        (void)memset(&stack_symbs, 0, sizeof(stack_symbs));
        ret = stack_id2symbs(st, stack_id, &stack_symbs);
        if (ret > 0) {
            continue;
        }
        if (ret < 0) {
            return -1;
        }
        st->stats.count[STACK_STATS_ID2SYMBS]++;
        (void)add_stack_histo(st, &stack_symbs);
    }

    st->stats.count[STACK_STATS_P_CACHE] = H_COUNT(st->proc_cache);
    st->stats.count[STACK_STATS_SYMB_CACHE] = __stack_count_symb(st);
    return 0;
}

#endif

static char is_tmout(struct stack_trace_s *st)
{
    time_t current = (time_t)time(NULL);
    time_t secs;

    if (current > st->running_times) {
        secs = current - st->running_times;
        if (secs >= TMOUT_PERIOD) {
            st->running_times = current;
            return 1;
        }
    }
    return 0;
}

static void process_loss_data(void *ctx, int cpu, u64 cnt)
{
    if (!g_st || !g_st->raw_stack_traces) {
        return;
    }
    g_st->stats.count[STACK_STATS_LOSS] += cnt;
}

static void process_raw_stack_trace(void *ctx, int cpu, void *data, u32 size)
{
    if (!g_st || !g_st->raw_stack_traces || !data) {
        return;
    }

    if (add_raw_stack_id(g_st, (struct stack_id_s *)data)) {
        g_st->stats.count[STACK_STATS_LOSS]++;
    } else {
        g_st->stats.count[STACK_STATS_RAW]++;
    }

    return;
}

static void destroy_svg_stack_trace(struct svg_stack_trace_s **ptr_svg_st)
{
    struct svg_stack_trace_s *svg_st = *ptr_svg_st;

    *ptr_svg_st = NULL;
    if (!svg_st) {
        return;
    }

    if (svg_st->obj) {
        bpf_object__close(svg_st->obj);
    }

    if (svg_st->pb_a) {
        perf_buffer__free(svg_st->pb_a);
    }
    if (svg_st->pb_b) {
        perf_buffer__free(svg_st->pb_b);
    }
    if (svg_st->svg_mng) {
        destroy_svg_mng(svg_st->svg_mng);
    }

    clear_stack_histo(svg_st);

    (void)free(svg_st);
    return;
}

static void destroy_stack_trace(struct stack_trace_s **ptr_st)
{
    struct stack_trace_s *st = *ptr_st;
    *ptr_st = NULL;
    if (!st) {
        return;
    }

    for (int cpu = 0; cpu < st->cpus_num; cpu++) {
        if (st->pmu_fd[cpu] > 0) {
            ioctl(st->pmu_fd[cpu], PERF_EVENT_IOC_DISABLE);
            close(st->pmu_fd[cpu]);
        }
    }

    if (st->ksymbs) {
        destroy_ksymbs_tbl(st->ksymbs);
        (void)free(st->ksymbs);
    }

    destroy_raw_stack_trace(st);
    destroy_proc_cache_tbl(st);

    if (st->elf_reader) {
        destroy_elf_reader(st->elf_reader);
    }

    deinit_elf_symbs();

    stacktrace_destroy_log_mgr(st);

    (void)free(st);
    return;
}

static struct svg_stack_trace_s *create_svg_stack_trace(StackprobeConfig *conf, const char *flame_name)
{
    struct svg_stack_trace_s *svg_st = (struct svg_stack_trace_s *)malloc(sizeof(struct svg_stack_trace_s));
    if (!svg_st) {
        return NULL;
    }
    memset(svg_st, 0, sizeof(struct svg_stack_trace_s));

    svg_st->svg_mng = create_svg_mng(conf->generalConfig->period);
    if (!svg_st->svg_mng) {
        goto err;
    }

    if (set_svg_dir(&svg_st->svg_mng->svg, conf->generalConfig->svgDir, flame_name)) {
        goto err;
    }

    if (set_flame_graph_path(svg_st->svg_mng, conf->generalConfig->flameDir, flame_name)) {
        goto err;
    }

    INFO("[STACKPROBE]: create %s svg stack trace succeed.\n", flame_name);
    return svg_st;

err:
    destroy_svg_stack_trace(&svg_st);
    return NULL;
}


static struct stack_trace_s *create_stack_trace(StackprobeConfig *conf)
{
    int cpus_num = NR_CPUS;
    size_t size = sizeof(struct stack_trace_s) + cpus_num * sizeof(int);
    struct stack_trace_s *st = (struct stack_trace_s *)malloc(size);
    if (!st) {
        return NULL;
    }

    (void)memset(st, 0, size);
    st->cpus_num = cpus_num;

#if 0
    if (stacktrace_create_log_mgr(st, conf->generalConfig->logDir)) {
        goto err;
    }
#endif

    st->elf_reader = create_elf_reader(conf->generalConfig->debugDir);
    if (!st->elf_reader) {
        goto err;
    }

    st->raw_stack_traces = create_raw_stack_trace(st);
    if (!st->raw_stack_traces) {
        goto err;
    }

    st->ksymbs = create_ksymbs_tbl();
    if (!st->ksymbs) {
        goto err;
    }

    if (load_kern_syms(st->ksymbs)) {
        ERROR("[STACKPROBE]: Failed to load kern symbols.\n");
        goto err;
    }

    (void)sort_kern_syms(st->ksymbs);

    st->running_times = (time_t)time(NULL);
    st->is_stackmap_a = ((st->convert_stack_count % 2) == 0);
    INFO("[STACKPROBE]: create stack trace succeed(cpus_num = %d, kern_symbols = %u).\n",
        st->cpus_num, st->ksymbs->ksym_size);
    return st;

err:
    destroy_stack_trace(&st);
    return NULL;
}

static int load_bpf_prog(struct svg_stack_trace_s *svg_st, const char *prog_name)
{
    int ret;
    struct bpf_program *prog;

    svg_st->obj = bpf_object__open_file(prog_name, NULL);
    ret = libbpf_get_error(svg_st->obj);
    if (ret) {
        ERROR("[STACKPROBE]: opening BPF object file failed(err = %d).\n", ret);
        goto err;
    }

    ret = BPF_PIN_MAP_PATH(svg_st->obj, "proc_obj_map", PROC_MAP_PATH);
    if (ret) {
        ERROR("[STACKPROBE]: Failed to pin bpf map(err = %d).\n", ret);
        goto err;
    }

    ret = bpf_object__load(svg_st->obj);
    if (ret) {
        ERROR("[STACKPROBE]: Failed to load bpf prog(err = %d).\n", ret);
        goto err;
    }

    prog = bpf_program__next(NULL, svg_st->obj);
    if (prog == NULL) {
        ERROR("[STACKPROBE]: Cannot find bpf_prog.\n");
        goto err;
    }
    svg_st->bpf_prog_fd = bpf_program__fd(prog);
    g_st->convert_map_fd = BPF_GET_MAP_FD(svg_st->obj, "convert_map");
    g_st->stackmap_a_fd = BPF_GET_MAP_FD(svg_st->obj, "stackmap_a");
    g_st->stackmap_b_fd = BPF_GET_MAP_FD(svg_st->obj, "stackmap_b");
    svg_st->stackmap_perf_a_fd = BPF_GET_MAP_FD(svg_st->obj, "stackmap_perf_a");
    svg_st->stackmap_perf_b_fd = BPF_GET_MAP_FD(svg_st->obj, "stackmap_perf_b");

    INFO("[STACKPROBE]: load bpf prog succeed(%s).\n", ON_CPU_PROG);
    return 0;

err:
    return -1;
}

static int perf_and_attach_bpf_prog(struct svg_stack_trace_s *svg_st)
{
    int ret;

    struct perf_event_attr attr_type_sw = {
        .sample_freq = SAMPLE_PERIOD,
        .freq = 1,
        .type = PERF_TYPE_SOFTWARE,
        .config = PERF_COUNT_SW_CPU_CLOCK,
    };

    svg_st->pb_a = create_pref_buffer2(svg_st->stackmap_perf_a_fd, process_raw_stack_trace, process_loss_data);
    if (!svg_st->pb_a) {
        goto err;
    }

    svg_st->pb_b = create_pref_buffer2(svg_st->stackmap_perf_b_fd, process_raw_stack_trace, process_loss_data);
    if (!svg_st->pb_b) {
        goto err;
    }

    for (int cpu = 0; cpu < g_st->cpus_num; cpu++) {
        g_st->pmu_fd[cpu] = perf_event_open(&attr_type_sw, -1, cpu, -1, 0);
        if (g_st->pmu_fd[cpu] < 0) {
            ERROR("[STACKPROBE]: Failed open perf event.\n");
            goto err;
        }

        ret = ioctl(g_st->pmu_fd[cpu], PERF_EVENT_IOC_ENABLE, 0);
        if (ret) {
            ERROR("[STACKPROBE]: Failed to PERF_EVENT_IOC_ENABLE(err = %d).\n", ret);
            goto err;
        }

        ret = ioctl(g_st->pmu_fd[cpu], PERF_EVENT_IOC_SET_BPF, svg_st->bpf_prog_fd);
        if (ret) {
            ERROR("[STACKPROBE]: Failed to PERF_EVENT_IOC_SET_BPF(err = %d).\n", ret);
            goto err;
        }

        INFO("[STACKPROBE]: perf open and attach bpf succeed(cpu = %d).\n", cpu);
    }

    return 0;

err:
    return -1;
}

static void update_convert_counter()
{
    u32 key = 0;
    (void)bpf_map_update_elem(g_st->convert_map_fd, &key, &(g_st->convert_stack_count), BPF_ANY);
}

static void clear_stackmap(int stackmap_fd)
{
    u32 stackid = 0, next_id;
    while (bpf_map_get_next_key(stackmap_fd, &stackid, &next_id) == 0) {
        bpf_map_delete_elem(stackmap_fd, &next_id);
        stackid = next_id;
    }
}

static void clear_running_ctx(struct stack_trace_s *st)
{
    u64 pcache_crt, pcache_del;
    clear_raw_stack_trace(st);
    clear_stackmap(get_stack_map_fd(st));
    for (int i = 0; i < STACK_SVG_MAX; i++) {
        if (st->svg_stack_traces[i] == NULL) {
            continue;
        }
        clear_stack_histo(st->svg_stack_traces[i]);
    }

    pcache_del = st->stats.count[STACK_STATS_PCACHE_DEL];
    pcache_crt = st->stats.count[STACK_STATS_PCACHE_CRT];
    (void)memset(&(st->stats), 0, sizeof(st->stats));
    st->stats.count[STACK_STATS_PCACHE_DEL] = pcache_del;
    st->stats.count[STACK_STATS_PCACHE_CRT] = pcache_crt;
}

static void record_running_ctx(struct stack_trace_s *st)
{
#if 1 //GOPHER_DEBUG
    int i, len, ret;
    char *pos;
    char buf[LINE_BUF_LEN];

    const char *col[STACK_STATS_MAX] = {"RAW", "LOSS", "HISTO_ERR", "HISTO_FOLD", "ID2SYMBS",
        "PCACHE_DEL", "PCACHE_CRT", "KERN_ERR", "USER_ERR", "MAP_LKUP_ERR",
        "KERN_OK", "USER_OK", "KERN_USER", "P_CACHE", "SYMB_CACHE"};
    const int offset[STACK_STATS_MAX] = {-8, -8, -10, -12, -10, -12, -12, -10, -10, -14, -9, -9, -11, -9, 12};

    printf("\n========================================================================================\n");

    buf[0] = 0;
    pos = buf;
    len = LINE_BUF_LEN;
    for (i = 0; i < STACK_STATS_MAX - 1; i++) {
        ret = snprintf(pos, len, "%*s", offset[i], col[i]);
        len -= ret;
        pos += ret;
    }
    (void)snprintf(pos, len, "%*s\n", offset[i], col[i]);

    printf(buf);

    buf[0] = 0;
    pos = buf;
    len = LINE_BUF_LEN;
    for (i = 0; i < STACK_STATS_MAX - 1; i++) {
        ret = snprintf(pos, len, "%*llu", offset[i], st->stats.count[i]);
        len -= ret;
        pos += ret;
    }
    (void)snprintf(pos, len, "%*llu\n", offset[i], st->stats.count[i]);
    printf(buf);
#endif
    return;
}

static void *__running(void *arg)
{
    int ret;
    struct svg_stack_trace_s *svg_st = arg;
    struct perf_buffer *pb = get_pb(g_st, svg_st);

    // Read raw stack-trace data from current data channel.
    while ((ret = perf_buffer__poll(pb, 0)) >= 0) {
        if (g_stop) {
            break;
        }
        pb = get_pb(g_st, svg_st);
        sleep(1);
    }
    printf("fuck\n");
    return NULL;
}

static void switch_stackmap()
{
    struct stack_trace_s *st = g_st;
    st->is_stackmap_a = ((st->convert_stack_count % 2) == 0);

    if (!is_tmout(st)) {
        return;
    }

    // Notify BPF to switch to another channel
    st->convert_stack_count++;
    update_convert_counter();
    (void)stack_id2histogram(st);
    // Histogram format to flame graph
    for (int i = 0; i < STACK_SVG_MAX; i++) {
        if (st->svg_stack_traces[i] == NULL) {
            continue;
        }
        wr_flamegraph(st->svg_stack_traces[i]->svg_mng, st->svg_stack_traces[i]->histo_tbl, i);
    }
    record_running_ctx(st);
    // Clear the context information of the running environment.
    clear_running_ctx(st);
    sleep(1);
}

static void init_wr_flame_pthreads(struct svg_stack_trace_s *svg_st, const char *flame_name)
{
    int err;
    pthread_t wr_flame_thd;

    err = pthread_create(&wr_flame_thd, NULL, __running, (void *)svg_st);
    if (err != 0) {
        fprintf(stderr, "Failed to create %s wr_flame_pthread.\n", flame_name);
        g_stop = 1;
        return;
    }
    printf("%s wr_flame_pthread successfully started!\n", flame_name);

    return;
}

static void init_enabled_svg_stack_traces(StackprobeConfig *conf)
{
    struct svg_stack_trace_s *svg_st;
    FlameProc flameProcs[] = {
        // This array order must be the same as the order of enum stack_svg_type_e
        { conf->flameTypesConfig->oncpu, STACK_SVG_ONCPU, "oncpu", ON_CPU_PROG},
        { conf->flameTypesConfig->offcpu, STACK_SVG_OFFCPU, "offcpu", OFF_CPU_PROG},
        { conf->flameTypesConfig->io, STACK_SVG_IO, "io", IO_PROG}
    };
    
    for (int i = 0; i < STACK_SVG_MAX; i++) {
        if (!flameProcs[i].sw) {
            continue;
        }
        
        svg_st = create_svg_stack_trace(conf, flameProcs[i].flame_name);
        if (!svg_st) {
            continue;
        }
        g_st->svg_stack_traces[i] = svg_st;

        if (load_bpf_prog(svg_st, flameProcs[i].prog_name)) {
            destroy_svg_stack_trace(&svg_st);
            continue;
        }

        if (perf_and_attach_bpf_prog(svg_st)) {
            destroy_svg_stack_trace(&svg_st);
            continue;
        }
        // Initializing the BPF Data Channel
        update_convert_counter();
        init_wr_flame_pthreads(svg_st, flameProcs[i].flame_name);
    }
}

#ifdef EBPF_RLIM_LIMITED
#undef EBPF_RLIM_LIMITED
#endif
#define EBPF_RLIM_LIMITED  500*1024*1024 // 500M
int main(int argc, char **argv)
{
    int err = -1;
    StackprobeConfig *conf;


    if (signal(SIGINT, sig_int) == SIG_ERR) {
        fprintf(stderr, "can't set signal handler: %d\n", errno);
        return errno;
    }

    err = configInit(&conf, STACKPROBE_CONF_PATH_DEFAULT);
    if (err != 0) {
        return -1;
    }

    err = args_parse(argc, argv, &params);
    if (err != 0) {
        return -1;
    }

    INIT_BPF_APP(stackprobe, EBPF_RLIM_LIMITED);

    g_st = create_stack_trace(conf);
    if (!g_st) {
        return -1;
    }
    init_enabled_svg_stack_traces(conf);
    INFO("[STACKPROBE]: Started successfully.\n");

    while (!g_stop) {
        switch_stackmap();
        sleep(1);
    }

    destroy_stack_trace(&g_st);

    return -err;
}
