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
 * Description: the bpf-side prog of thread profiling probe
 ******************************************************************************/
#ifdef BPF_PROG_USER
#undef BPF_PROG_USER
#endif
#define BPF_PROG_KERN
#include "bpf.h"
#include "syscall.bpf.h"

char g_license[] SEC("license") = "GPL";

SET_SYSCALL_PARAMS(read)
{
    sce->ext_info.fd_info.fd = (int)_(PT_REGS_PARM1(regs));
}

SET_SYSCALL_META(read)
{
    scm->nr = SYSCALL_READ_ID;
    scm->flag = SYSCALL_FLAG_FD_STACK;
}

SET_SYSCALL_PARAMS(write)
{
    sce->ext_info.fd_info.fd = (int)_(PT_REGS_PARM1(regs));
}

SET_SYSCALL_META(write)
{
    scm->nr = SYSCALL_WRITE_ID;
    scm->flag = SYSCALL_FLAG_FD_STACK;
}

SET_SYSCALL_PARAMS(readv)
{
    sce->ext_info.fd_info.fd = (int)_(PT_REGS_PARM1(regs));
}

SET_SYSCALL_META(readv)
{
    scm->nr = SYSCALL_READV_ID;
    scm->flag = SYSCALL_FLAG_FD_STACK;
}

SET_SYSCALL_PARAMS(writev)
{
    sce->ext_info.fd_info.fd = (int)_(PT_REGS_PARM1(regs));
}

SET_SYSCALL_META(writev)
{
    scm->nr = SYSCALL_WRITEV_ID;
    scm->flag = SYSCALL_FLAG_FD_STACK;
}

SET_SYSCALL_PARAMS(preadv)
{
    sce->ext_info.fd_info.fd = (int)_(PT_REGS_PARM1(regs));
}

SET_SYSCALL_META(preadv)
{
    scm->nr = SYSCALL_PREADV_ID;
    scm->flag = SYSCALL_FLAG_FD_STACK;
}

SET_SYSCALL_PARAMS(pwritev)
{
    sce->ext_info.fd_info.fd = (int)_(PT_REGS_PARM1(regs));
}

SET_SYSCALL_META(pwritev)
{
    scm->nr = SYSCALL_PWRITEV_ID;
    scm->flag = SYSCALL_FLAG_FD_STACK;
}

SET_SYSCALL_PARAMS(sync) { return; }

SET_SYSCALL_META(sync)
{
    scm->nr = SYSCALL_SYNC_ID;
    scm->flag = SYSCALL_FLAG_STACK;
}

SET_SYSCALL_PARAMS(fsync)
{
    sce->ext_info.fd_info.fd = (int)_(PT_REGS_PARM1(regs));
}

SET_SYSCALL_META(fsync)
{
    scm->nr = SYSCALL_FSYNC_ID;
    scm->flag = SYSCALL_FLAG_FD_STACK;
}

SET_SYSCALL_PARAMS(fdatasync)
{
    sce->ext_info.fd_info.fd = (int)_(PT_REGS_PARM1(regs));
}

SET_SYSCALL_META(fdatasync)
{
    scm->nr = SYSCALL_FDATASYNC_ID;
    scm->flag = SYSCALL_FLAG_FD_STACK;
}

#if defined(__TARGET_ARCH_x86)
KPROBE_SYSCALL(__x64_sys_, read)
KPROBE_SYSCALL(__x64_sys_, readv)
KPROBE_SYSCALL(__x64_sys_, write)
KPROBE_SYSCALL(__x64_sys_, writev)
KPROBE_SYSCALL(__x64_sys_, preadv)
KPROBE_SYSCALL(__x64_sys_, pwritev)
KPROBE_SYSCALL(__x64_sys_, sync)
KPROBE_SYSCALL(__x64_sys_, fsync)
KPROBE_SYSCALL(__x64_sys_, fdatasync)
#elif defined(__TARGET_ARCH_arm64)
KPROBE_SYSCALL(__arm64_sys_, read)
KPROBE_SYSCALL(__arm64_sys_, readv)
KPROBE_SYSCALL(__arm64_sys_, write)
KPROBE_SYSCALL(__arm64_sys_, writev)
KPROBE_SYSCALL(__arm64_sys_, preadv)
KPROBE_SYSCALL(__arm64_sys_, pwritev)
KPROBE_SYSCALL(__arm64_sys_, sync)
KPROBE_SYSCALL(__arm64_sys_, fsync)
KPROBE_SYSCALL(__arm64_sys_, fdatasync)
#endif