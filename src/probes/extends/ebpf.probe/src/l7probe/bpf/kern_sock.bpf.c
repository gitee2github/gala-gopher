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
 * Author: luzhihao
 * Create: 2023-02-22
 * Description: Socket trace
 ******************************************************************************/

#ifdef BPF_PROG_USER
#undef BPF_PROG_USER
#endif
#define BPF_PROG_KERN

#include "kern_sock.h"

char g_license[] SEC("license") = "GPL";


#define __KPROBE_SYSCALL(arch, func) KPROBE(arch##func, pt_regs)

#if defined(__TARGET_ARCH_x86)
#define KPROBE_SYSCALL(func) __KPROBE_SYSCALL(__x64_sys_, func)
#elif defined(__TARGET_ARCH_arm64)
#define KPROBE_SYSCALL(func)  __KPROBE_SYSCALL(__arm64_sys_, func)
#endif

#define __KRETPROBE_SYSCALL(arch, func) KRETPROBE(arch##func, pt_regs)

#if defined(__TARGET_ARCH_x86)
#define KRETPROBE_SYSCALL(func) __KRETPROBE_SYSCALL(__x64_sys_, func)
#elif defined(__TARGET_ARCH_arm64)
#define KRETPROBE_SYSCALL(func) __KRETPROBE_SYSCALL(__arm64_sys_, func)
#endif

static __always_inline char is_tracing_udp(void)
{
    u32 proto = get_filter_proto();
    return proto & L7PROBE_TRACING_DNS;
}

static __always_inline __maybe_unused void get_sockaddr(struct conn_info_s* conn_info, enum l4_role_t l4_role, const struct socket* socket)
{
    u16 family, port;

#if (CURRENT_LIBBPF_VERSION  >= LIBBPF_VERSION(0, 8))
    family = BPF_CORE_READ(socket, sk, __sk_common.skc_family);
    if (l4_role == L4_CLIENT) {
        port = BPF_CORE_READ(socket, sk, __sk_common.skc_dport);
        port = bpf_ntohs(port);
    } else if (l4_role == L4_SERVER){
        port = BPF_CORE_READ(socket, sk, __sk_common.skc_num);
    } else {
        port = 0;
    }

    conn_info->remote_addr.family = family;
    conn_info->remote_addr.port = port;

    if (family == AF_INET) {
        conn_info->remote_addr.ip = BPF_CORE_READ(socket, sk, __sk_common.skc_daddr);
    } else if (family == AF_INET6) {
        BPF_CORE_READ_INTO(&(conn_info->remote_addr.ip6), socket, sk, __sk_common.skc_v6_daddr);
    }
#else
    struct sock* sk = NULL;
    sk = _(socket->sk);
    family = _(sk->__sk_common.skc_family);
    if (l4_role == L4_CLIENT) {
        port = bpf_ntohs(_(sk->sk_dport));
    } else if (l4_role == L4_SERVER){
        port = _(sk->sk_num);
    } else {
        port = 0;
    }

    conn_info->remote_addr.family = family;
    conn_info->remote_addr.port = port;

    if (family == AF_INET) {
        conn_info->remote_addr.ip = BPF_CORE_READ(socket, sk, __sk_common.skc_daddr);
    } else if (family == AF_INET6) {
      (void)bpf_probe_read(&(conn_info->remote_addr.ip6), IP6_LEN, &(sk->__sk_common.skc_v6_daddr));
    }
#endif
}

static __always_inline __maybe_unused struct sock_conn_s* new_sock_conn(void *ctx, int tgid, int fd, enum l4_role_t l4_role,
                                     const struct sockaddr* addr, const struct socket* socket)
{
    struct conn_id_s id = {0};
    struct sock_conn_s sock_conn = {0};

    id.fd = fd;
    id.tgid = tgid;

    sock_conn.info.id.fd = fd;
    sock_conn.info.id.tgid = tgid;
    sock_conn.info.is_ssl = 0;
    sock_conn.info.protocol = PROTO_UNKNOW;
    sock_conn.info.l4_role = l4_role;
    if (addr != NULL) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)addr;
        const struct sockaddr_in6 *addr_in6 = (const struct sockaddr_in6 *)addr;

        sock_conn.info.remote_addr.family = _(addr->sa_family);
        if (sock_conn.info.remote_addr.family == AF_INET) {
            sock_conn.info.remote_addr.ip = _(addr_in->sin_addr.s_addr);
            sock_conn.info.remote_addr.port = bpf_ntohs(_(addr_in->sin_port));
        } else {
            bpf_probe_read((unsigned char *)&sock_conn.info.remote_addr.ip6, IP6_LEN, &(addr_in6->sin6_addr));
            sock_conn.info.remote_addr.port = bpf_ntohs(_(addr_in6->sin6_port));
        }

        if (l4_role == L4_UNKNOW) {
            sock_conn.info.remote_addr.port = 0; // UDP fix port 0
        }
    } else if (socket != NULL) {
        get_sockaddr(&sock_conn.info, l4_role, socket);
    }

    // new conn obj
    bpf_map_update_elem(&conn_tbl, &id, &sock_conn, BPF_ANY);
    return lkup_sock_conn(tgid, fd);
}

static __always_inline __maybe_unused struct sock_conn_s* get_sock_conn(void *ctx, int tgid, int fd)
{
    int value;
    enum l4_role_t l4_role;
    struct sock_conn_s* sock_conn = NULL;

    struct sock *sk = sock_get_by_fd(fd, (struct task_struct *)bpf_get_current_task());
    if (!sk) {
        return NULL;
    }

#if (CURRENT_KERNEL_VERSION  >= KERNEL_VERSION(5, 6, 0))
    u16 sk_type = BPF_CORE_READ(sk, sk_type);
#else
    u32 sk_type;
    bpf_probe_read(&sk_type, sizeof(u32), (char *)&(sk->sk_txhash) + sizeof(sk->sk_txhash));
    sk_type = sk_type & 0x0000FFFFF;
#endif
    if (sk_type != SOCK_STREAM) {
        l4_role = L4_UNKNOW;
    } else {
        value = lkup_l7_tcp(tgid, fd);
        if (value < 0) {
            return NULL;
        }
        l4_role = (value == 0) ? L4_CLIENT : L4_SERVER;
    }

    struct socket* socket = BPF_CORE_READ(sk, sk_socket);
    if (socket == NULL) {
        return NULL;
    }

    // new sock connection
    sock_conn = new_sock_conn(ctx, tgid, fd, l4_role, NULL, (const struct socket*)socket);
    return sock_conn;
}

static __always_inline __maybe_unused struct sock_conn_s* get_sock_conn_by_addr(void *ctx, int tgid, int fd, const struct sockaddr* addr)
{
    // new UDP sock connection
    return new_sock_conn(ctx, tgid, fd, L4_UNKNOW, addr, NULL);
}

static __always_inline __maybe_unused int submit_conn_open(void *ctx, struct sock_conn_s* sock_conn)
{
    if (sock_conn == NULL) {
        return -1;
    }

    if (sock_conn->info.is_reported != 0) {
        return 0;   // avoid report redundant events
    }

#ifdef __USE_RING_BUF
    struct conn_ctl_s *e = bpf_ringbuf_reserve(&conn_control_events, sizeof(struct conn_ctl_s), 0);
    if (!e) {
        return -1;
    }
#else
    struct conn_ctl_s evt = {0};
    struct conn_ctl_s *e = &evt;
#endif

    e->type = CONN_EVT_OPEN;
    e->timestamp_ns = bpf_ktime_get_ns();
    e->conn_id = sock_conn->info.id;
    e->open.l4_role = sock_conn->info.l4_role;
    e->open.is_ssl = sock_conn->info.is_ssl;

    __builtin_memcpy(&(e->open.addr), &(sock_conn->info.remote_addr), sizeof(struct conn_addr_s));

    // submit conn open event.
#ifdef __USE_RING_BUF
    bpf_ringbuf_submit(e, 0);
#else
    (void)bpf_perf_event_output(ctx, &conn_control_events, BPF_F_CURRENT_CPU, e, sizeof(struct conn_ctl_s));
#endif
    sock_conn->info.is_reported = 1;
    return 0;
}


static __always_inline __maybe_unused int submit_conn_close(void *ctx, conn_ctx_t id, int fd)
{
    int tgid = (int)(id >> INT_LEN);
    struct sock_conn_s* sock_conn = lkup_sock_conn(tgid, fd);
    if (sock_conn == NULL) {
        return 0;
    }

#ifdef __USE_RING_BUF
    struct conn_ctl_s *e = bpf_ringbuf_reserve(&conn_control_events, sizeof(struct conn_ctl_s), 0);
    if (!e) {
        goto end;
    }
#else
    struct conn_ctl_s evt = {0};
    struct conn_ctl_s *e = &evt;
#endif

    e->type = CONN_EVT_CLOSE;
    e->timestamp_ns = bpf_ktime_get_ns();
    e->conn_id = sock_conn->info.id;
    e->close.rd_bytes = sock_conn->rd_bytes;
    e->close.wr_bytes = sock_conn->wr_bytes;

    // submit conn open event.
#ifdef __USE_RING_BUF
    bpf_ringbuf_submit(e, 0);
#else
    (void)bpf_perf_event_output(ctx, &conn_control_events, BPF_F_CURRENT_CPU, e, sizeof(struct conn_ctl_s));
#endif

#ifdef __USE_RING_BUF
end:
#endif
    bpf_map_delete_elem(&conn_tbl, &id);

    return 0;
}

#if 0
static __always_inline __maybe_unused u64 get_cur_cpuacct_cgrp_id(void)
{
    u64 cgroup_id;
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    struct css_set *cgroups = _(task->cgroups);
    struct cgroup_subsys_state *css = _(cgroups->subsys[cpuacct_cgrp_id]);
    struct kernfs_node *kn = BPF_CORE_READ(css, cgroup, kn);

#if (CURRENT_KERNEL_VERSION < KERNEL_VERSION(5, 5, 0))
    cgroup_id = _(kn->id.id);
#else
    cgroup_id = _(kn->id);
#endif
    return cgroup_id;
}
#endif

static __always_inline char is_tracing(int tgid)
{
    return is_filter_id(FILTER_TGID, tgid);
#if 0
    if (is_filter_by_cgrp()) {
        u64 cgrp_id = get_cur_cpuacct_cgrp_id();
        if (is_filter_id(FILTER_CGRPID, cgrp_id)) {
            return 1;
        } else {
            return 0;
        }
    } else {
        if (is_filter_id(FILTER_TGID, tgid)) {
            return 1;
        } else {
            return 0;
        }
    }
#endif
}


// int security_socket_sendmsg(struct socket *sock, struct msghdr *msg, int size)
KPROBE(security_socket_sendmsg, pt_regs)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sock_data_args_s* args = bpf_map_lookup_elem(&sock_data_args, &id);
    if (args) {
        args->is_socket_op = 1;
    }
    return 0;
}

// int security_socket_recvmsg(struct socket *sock, struct msghdr *msg, int size)
KPROBE(security_socket_recvmsg, pt_regs)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sock_data_args_s* args = bpf_map_lookup_elem(&sock_data_args, &id);
    if (args) {
        args->is_socket_op = 1;
    }
    return 0;
}

KRETPROBE(sock_alloc, pt_regs)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sys_accept_args_s* args = bpf_map_lookup_elem(&sys_accept_args, &id);
    if (args == NULL) {
        return 0;
    }

    if (args->newsock == NULL) {
        args->newsock = (struct socket *)PT_REGS_RC(ctx);
    }
    return 0;
}

// int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
KPROBE_SYSCALL(connect)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);

    if (!is_tracing(proc_id)) {
        return 0;
    }

    int fd = (int)PT_REGS_PARM1(ctx);
    const struct sockaddr *addr = (const struct sockaddr *)PT_REGS_PARM2(ctx);

    struct sys_connect_args_s args = {0};
    args.fd = fd;
    args.addr = addr;
    bpf_map_update_elem(&sys_connect_args, &id, &args, BPF_ANY);
    return 0;
}

#define EINPROGRESS 115 // TODO: Varies in different arch
KRETPROBE_SYSCALL(connect)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sys_connect_args_s* args = bpf_map_lookup_elem(&sys_connect_args, &id);
    if (args != NULL) {
        if (args->fd < 0) {
          goto end;
        }

        int ret = (int)PT_REGS_RC(ctx);
        if (ret < 0 && ret != -EINPROGRESS) {
            // EINPROGRESS means NON_BLOCK socket is undergoing handshake.
            goto end;
        }

        struct sock_conn_s* sock_conn = lkup_sock_conn((int)(id >> INT_LEN), args->fd);
        // new sock connection
        if (!sock_conn) {
            sock_conn = new_sock_conn(ctx, (int)(id >> INT_LEN), args->fd, L4_CLIENT, args->addr, NULL);
        }

        submit_conn_open(ctx, sock_conn);
    }
end:
    bpf_map_delete_elem(&sys_connect_args, &id);
    return 0;
}

// ssize_t send(int sockfd, const void *buf, size_t len, int flags);
KPROBE_SYSCALL(send)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);

    if (!is_tracing(proc_id)) {
        return 0;
    }

    int fd = (int)PT_REGS_PARM1(ctx);

    struct sock_data_args_s args = {0};
    args.conn_id.fd = fd;
    args.conn_id.tgid = proc_id;
    args.direct = L7_EGRESS;
    args.buf = (char *)PT_REGS_PARM2(ctx);
    args.is_ssl = 0;
    bpf_map_update_elem(&sock_data_args, &id, &args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(send)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sock_data_args_s* args = bpf_map_lookup_elem(&sock_data_args, &id);
    if (args != NULL) {
        ssize_t bytes_count = (ssize_t)PT_REGS_RC(ctx);
        if (bytes_count <= 0) {
            goto end;
        }

        struct sock_conn_s* sock_conn = lkup_sock_conn(args->conn_id.tgid, args->conn_id.fd);
        // new sock connection
        if (!sock_conn) {
            sock_conn = get_sock_conn(ctx, args->conn_id.tgid, args->conn_id.fd);
            if (sock_conn == NULL) {
                goto end;
            }
            (void)submit_conn_open(ctx, sock_conn);
        }

        submit_sock_data(ctx, sock_conn, id, L7_EGRESS, args, (size_t)bytes_count);
    }

end:
    bpf_map_delete_elem(&sock_data_args, &id);
    return 0;
}

// int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
KPROBE_SYSCALL(accept)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    if (!is_tracing((int)(id >> INT_LEN))) {
        return 0;
    }

    struct sockaddr *addr = (struct sockaddr *)PT_REGS_PARM2(ctx);

    struct sys_accept_args_s args = {0};
    args.addr = addr;
    bpf_map_update_elem(&sys_accept_args, &id, &args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(accept)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sys_accept_args_s* args = bpf_map_lookup_elem(&sys_accept_args, &id);
    if (args != NULL) {
        if (args->addr == NULL) {
          goto end;
        }

        int new_fd = (int)PT_REGS_RC(ctx);
        if (new_fd < 0) {
            goto end;
        }

        struct sock_conn_s* sock_conn = lkup_sock_conn((int)(id >> INT_LEN), new_fd);
        // new sock connection
        if (!sock_conn) {
            sock_conn = new_sock_conn(ctx, (int)(id >> INT_LEN), new_fd, L4_SERVER, args->addr, NULL);
        }

        submit_conn_open(ctx, sock_conn);
    }
end:
    bpf_map_delete_elem(&sys_accept_args, &id);
    return 0;
}

// int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
KPROBE_SYSCALL(accept4)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    if (!is_tracing((int)(id >> INT_LEN))) {
        return 0;
    }

    struct sockaddr *addr = (struct sockaddr *)PT_REGS_PARM2(ctx);

    struct sys_accept_args_s args = {0};
    args.addr = addr;
    bpf_map_update_elem(&sys_accept_args, &id, &args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(accept4)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sys_accept_args_s* args = bpf_map_lookup_elem(&sys_accept_args, &id);
    if (args != NULL) {
        if (args->addr == NULL) {
          goto end;
        }

        int new_fd = (int)PT_REGS_RC(ctx);
        if (new_fd < 0) {
            goto end;
        }

        struct sock_conn_s* sock_conn = lkup_sock_conn((int)(id >> INT_LEN), new_fd);
        // new sock connection
        if (!sock_conn) {
            sock_conn = new_sock_conn(ctx, (int)(id >> INT_LEN), new_fd, L4_SERVER, args->addr, args->newsock);
        }

        submit_conn_open(ctx, sock_conn);
    }
end:
    bpf_map_delete_elem(&sys_accept_args, &id);
    return 0;
}

// ssize_t write(int fd, const void *buf, size_t count);
KPROBE_SYSCALL(write)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);
    int fd = (int)PT_REGS_PARM1(ctx);

    if (!is_tracing(proc_id)) {
        return 0;
    }

    struct sock_data_args_s args = {0};
    args.conn_id.fd = fd;
    args.conn_id.tgid = proc_id;
    args.direct = L7_EGRESS;
    args.buf = (char *)PT_REGS_PARM2(ctx);
    args.is_ssl = 0;
    bpf_map_update_elem(&sock_data_args, &id, &args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(write)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sock_data_args_s* args = bpf_map_lookup_elem(&sock_data_args, &id);
    if (args != NULL && args->is_socket_op) {
        ssize_t bytes_count = (ssize_t)PT_REGS_RC(ctx);
        if (bytes_count <= 0) {
            goto end;
        }

        struct sock_conn_s* sock_conn = lkup_sock_conn(args->conn_id.tgid, args->conn_id.fd);
        // new sock connection
        if (!sock_conn) {
            sock_conn = get_sock_conn(ctx, args->conn_id.tgid, args->conn_id.fd);
            if (sock_conn == NULL) {
                goto end;
            }
            (void)submit_conn_open(ctx, sock_conn);
        }

        submit_sock_data(ctx, sock_conn, id, L7_EGRESS, args, (size_t)bytes_count);
    }

end:
    bpf_map_delete_elem(&sock_data_args, &id);
    return 0;
}

// ssize_t read(int fd, void *buf, size_t count);
KPROBE_SYSCALL(read)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);
    int fd = (int)PT_REGS_PARM1(ctx);

    if (!is_tracing(proc_id)) {
        return 0;
    }

    struct sock_data_args_s args = {0};
    args.conn_id.fd = fd;
    args.conn_id.tgid = proc_id;
    args.direct = L7_INGRESS;
    args.buf = (char *)PT_REGS_PARM2(ctx);
    args.is_ssl = 0;
    bpf_map_update_elem(&sock_data_args, &id, &args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(read)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sock_data_args_s* args = bpf_map_lookup_elem(&sock_data_args, &id);
    if (args != NULL && args->is_socket_op) {
        ssize_t bytes_count = (ssize_t)PT_REGS_RC(ctx);
        if (bytes_count <= 0) {
            goto end;
        }
        struct sock_conn_s* sock_conn = lkup_sock_conn(args->conn_id.tgid, args->conn_id.fd);
        // new sock connection
        if (!sock_conn) {
            sock_conn = get_sock_conn(ctx, args->conn_id.tgid, args->conn_id.fd);
            if (sock_conn == NULL) {
                goto end;
            }
            (void)submit_conn_open(ctx, sock_conn);
        }

        submit_sock_data(ctx, sock_conn, id, L7_INGRESS, args, (size_t)bytes_count);
    }

end:
    bpf_map_delete_elem(&sock_data_args, &id);
    return 0;
}

// ssize_t recv(int sockfd, void *buf, size_t len, int flags);
KPROBE_SYSCALL(recv)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);
    int fd = (int)PT_REGS_PARM1(ctx);

    if (!is_tracing(proc_id)) {
        return 0;
    }

    struct sock_data_args_s args = {0};
    args.conn_id.fd = fd;
    args.conn_id.tgid = proc_id;
    args.direct = L7_INGRESS;
    args.buf = (char *)PT_REGS_PARM2(ctx);
    args.is_ssl = 0;
    bpf_map_update_elem(&sock_data_args, &id, &args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(recv)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();

    struct sock_data_args_s* args = bpf_map_lookup_elem(&sock_data_args, &id);
    if (args != NULL) {
        ssize_t bytes_count = (ssize_t)PT_REGS_RC(ctx);
        if (bytes_count <= 0) {
            goto end;
        }
        struct sock_conn_s* sock_conn = lkup_sock_conn(args->conn_id.tgid, args->conn_id.fd);
        // new sock connection
        if (!sock_conn) {
            sock_conn = get_sock_conn(ctx, args->conn_id.tgid, args->conn_id.fd);
            if (sock_conn == NULL) {
                goto end;
            }
            (void)submit_conn_open(ctx, sock_conn);
        }
        submit_sock_data(ctx, sock_conn, id, L7_INGRESS, args, (size_t)bytes_count);
    }

end:
    bpf_map_delete_elem(&sock_data_args, &id);
    return 0;
}

// ssize_t sendto(int sockfd, const void *buf, size_t len,
//      int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
KPROBE_SYSCALL(sendto)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);

    if (!is_tracing(proc_id)) {
        return 0;
    }

    int sockfd = (int)PT_REGS_PARM1(ctx);
    char *buf = (char *)PT_REGS_PARM2(ctx);
    if (buf == NULL || sockfd < 0) {
        return -1;
    }

    // Filter by UDP tracing-on/off
    if (is_tracing_udp()) {
        struct sockaddr * dest_addr = (struct sockaddr *)PT_REGS_PARM5(ctx);
        if (dest_addr && sockfd > 0) {
            struct sys_connect_args_s args = {0};
            args.fd = sockfd;
            args.addr = dest_addr;
            bpf_map_update_elem(&sys_connect_args, &id, &args, BPF_ANY);
        }
    }

    struct sock_data_args_s data_args = {0};
    data_args.buf = buf;
    data_args.conn_id.fd = sockfd;
    data_args.conn_id.tgid = proc_id;
    data_args.is_ssl = 0;
    bpf_map_update_elem(&sock_data_args, &id, &data_args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(sendto)
{
    int is_udp = 0;
    conn_ctx_t id = bpf_get_current_pid_tgid();
    ssize_t bytes_count = PT_REGS_RC(ctx);
    struct sock_conn_s* sock_conn = NULL;

    // Filter by UDP tracing-on/off
    if (is_tracing_udp()) {
        struct sys_connect_args_s* args = bpf_map_lookup_elem(&sys_connect_args, &id);
        if ((args != NULL) && (bytes_count > 0)) {
            sock_conn = lkup_sock_conn((int)(id >> INT_LEN), args->fd);
            // new sock connection
            if (!sock_conn) {
                sock_conn = get_sock_conn_by_addr(ctx, (int)(id >> INT_LEN), args->fd, args->addr);
            }
            if (sock_conn == NULL) {
                goto end;
            }
            is_udp = 1;
            (void)submit_conn_open(ctx, sock_conn); // UDP socket open event;
        }
    }

    // Unstash arguments, and process syscall.
    struct sock_data_args_s* data_args = bpf_map_lookup_elem(&sock_data_args, &id);
    if ((data_args != NULL) && (bytes_count > 0)) {
        if (!is_udp) {
            sock_conn = lkup_sock_conn((int)(id >> INT_LEN), data_args->conn_id.fd);
            if (!sock_conn) {
                sock_conn = get_sock_conn(ctx, (int)(id >> INT_LEN), data_args->conn_id.fd);
            }
            if (sock_conn == NULL) {
                goto end;
            }
            (void)submit_conn_open(ctx, sock_conn); // TCP socket open event;
        }
        submit_sock_data(ctx, sock_conn, id, L7_EGRESS, data_args, (size_t)bytes_count);
    }
end:
    bpf_map_delete_elem(&sys_connect_args, &id);
    bpf_map_delete_elem(&sock_data_args, &id);
    return 0;
}


// ssize_t recvfrom(int sockfd, void *buf, size_t len,
//      int flags, struct sockaddr *src_addr, socklen_t *addrlen);
KPROBE_SYSCALL(recvfrom)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);

    if (!is_tracing(proc_id)) {
        return 0;
    }

    int sockfd = (int)PT_REGS_PARM1(ctx);
    char *buf = (char *)PT_REGS_PARM2(ctx);
    if (buf == NULL || sockfd < 0) {
        return -1;
    }

    // Filter by UDP tracing-on/off
    if (is_tracing_udp()) {
        struct sockaddr * src_addr = (struct sockaddr *)PT_REGS_PARM5(ctx);
        if (src_addr) {
            struct sys_connect_args_s args = {0};
            args.fd = sockfd;
            args.addr = src_addr;
            bpf_map_update_elem(&sys_connect_args, &id, &args, BPF_ANY);
        }
    }

    struct sock_data_args_s data_args = {0};
    data_args.buf = buf;
    data_args.conn_id.fd = sockfd;
    data_args.conn_id.tgid = proc_id;
    data_args.is_ssl = 0;
    bpf_map_update_elem(&sock_data_args, &id, &data_args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(recvfrom)
{
    int is_udp = 0;
    conn_ctx_t id = bpf_get_current_pid_tgid();
    ssize_t bytes_count = PT_REGS_RC(ctx);
    struct sock_conn_s* sock_conn = NULL;

    // Filter by UDP tracing-on/off
    if (is_tracing_udp()) {
        struct sys_connect_args_s* args = bpf_map_lookup_elem(&sys_connect_args, &id);
        if ((args != NULL) && (bytes_count > 0)) {
            sock_conn = lkup_sock_conn((int)(id >> INT_LEN), args->fd);
            // new sock connection
            if (!sock_conn) {
                sock_conn = get_sock_conn_by_addr(ctx, (int)(id >> INT_LEN), args->fd, args->addr);
            }
            if (sock_conn == NULL) {
                goto end;
            }
            is_udp = 1;
            (void)submit_conn_open(ctx, sock_conn); // UDP socket open event;
        }
    }

    // Unstash arguments, and process syscall.
    struct sock_data_args_s* data_args = bpf_map_lookup_elem(&sock_data_args, &id);
    if ((data_args != NULL) && (bytes_count > 0)) {
        if (!is_udp) {
            sock_conn = lkup_sock_conn((int)(id >> INT_LEN), data_args->conn_id.fd);
            if (!sock_conn) {
                sock_conn = get_sock_conn(ctx, (int)(id >> INT_LEN), data_args->conn_id.fd);
            }
            if (sock_conn == NULL) {
                goto end;
            }
            (void)submit_conn_open(ctx, sock_conn); // TCP socket open event;
        }

        submit_sock_data(ctx, sock_conn, id, L7_INGRESS, data_args, (size_t)bytes_count);
    }

end:
    bpf_map_delete_elem(&sys_connect_args, &id);
    bpf_map_delete_elem(&sock_data_args, &id);
    return 0;
}

// ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
KPROBE_SYSCALL(sendmsg)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);

    if (!is_tracing(proc_id)) {
        return 0;
    }

    int fd = (int)PT_REGS_PARM1(ctx);
    struct user_msghdr *msg = (struct user_msghdr *)PT_REGS_PARM2(ctx);
#if (CURRENT_LIBBPF_VERSION  < LIBBPF_VERSION(0, 8))
    void * msg_name = BPF_CORE_READ(msg, msg_name);
    struct iovec* iov = BPF_CORE_READ(msg, msg_iov);
    size_t iovlen = BPF_CORE_READ(msg, msg_iovlen);
#else
    void * msg_name = BPF_CORE_READ_USER(msg, msg_name);
    struct iovec* iov = BPF_CORE_READ_USER(msg, msg_iov);
    size_t iovlen = BPF_CORE_READ_USER(msg, msg_iovlen);
#endif

    // Filter by UDP tracing-on/off
    if (is_tracing_udp()) {
        if (msg_name) {
            struct sys_connect_args_s args = {0};
            args.fd = fd;
            args.addr = msg_name;
            bpf_map_update_elem(&sys_connect_args, &id, &args, BPF_ANY);
        }
    }

    struct sock_data_args_s data_args = {0};
    data_args.conn_id.fd = fd;
    data_args.conn_id.tgid = proc_id;
    data_args.direct = L7_EGRESS;
    data_args.iov = iov;
    data_args.iovlen = iovlen;
    data_args.is_ssl = 0;
    bpf_map_update_elem(&sock_data_args, &id, &data_args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(sendmsg)
{
    int is_udp = 0;
    conn_ctx_t id = bpf_get_current_pid_tgid();
    ssize_t bytes_count = PT_REGS_RC(ctx);
    struct sock_conn_s* sock_conn = NULL;

    // Filter by UDP tracing-on/off
    if (is_tracing_udp()) {
        struct sys_connect_args_s* args = bpf_map_lookup_elem(&sys_connect_args, &id);
        if (args && bytes_count > 0) {
            sock_conn = lkup_sock_conn((int)(id >> INT_LEN), args->fd);
            // new sock connection
            if (!sock_conn) {
                sock_conn = get_sock_conn_by_addr(ctx, (int)(id >> INT_LEN), args->fd, args->addr);
            }
            if (sock_conn == NULL) {
                goto end;
            }
            is_udp = 1;
            (void)submit_conn_open(ctx, sock_conn); // UDP socket open event;
        }
    }

    struct sock_data_args_s* data_args = bpf_map_lookup_elem(&sock_data_args, &id);
    if ((data_args != NULL) && (bytes_count > 0)) {
        if (!is_udp) {
            sock_conn = lkup_sock_conn((int)(id >> INT_LEN), data_args->conn_id.fd);
            if (!sock_conn) {
                sock_conn = get_sock_conn(ctx, (int)(id >> INT_LEN), data_args->conn_id.fd);
            }
            if (sock_conn == NULL) {
                goto end;
            }
            (void)submit_conn_open(ctx, sock_conn); // TCP socket open event;
        }
        submit_sock_data(ctx, sock_conn, id, L7_EGRESS, data_args, (size_t)bytes_count);
    }

end:
    bpf_map_delete_elem(&sys_connect_args, &id);
    bpf_map_delete_elem(&sock_data_args, &id);
    return 0;
}

// ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);
KPROBE_SYSCALL(recvmsg)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    int proc_id = (int)(id >> INT_LEN);
    if (!is_tracing(proc_id)) {
        return 0;
    }

    int fd = (int)PT_REGS_PARM1(ctx);
    struct user_msghdr *msg = (struct user_msghdr *)PT_REGS_PARM2(ctx);
#if (CURRENT_LIBBPF_VERSION  < LIBBPF_VERSION(0, 8))
    void * msg_name = BPF_CORE_READ(msg, msg_name);
    struct iovec* iov = BPF_CORE_READ(msg, msg_iov);
    size_t iovlen = BPF_CORE_READ(msg, msg_iovlen);
#else
    void * msg_name = BPF_CORE_READ_USER(msg, msg_name);
    struct iovec* iov = BPF_CORE_READ_USER(msg, msg_iov);
    size_t iovlen = BPF_CORE_READ_USER(msg, msg_iovlen);
#endif

    // Filter by UDP tracing-on/off
    if (is_tracing_udp()) {
        if (msg_name) {
            struct sys_connect_args_s args = {0};
            args.fd = fd;
            args.addr = msg_name;
            bpf_map_update_elem(&sys_connect_args, &id, &args, BPF_ANY);
        }
    }

    struct sock_data_args_s data_args = {0};
    data_args.conn_id.fd = fd;
    data_args.conn_id.tgid = proc_id;
    data_args.direct = L7_INGRESS;
    data_args.iov = iov;
    data_args.iovlen = iovlen;
    data_args.is_ssl = 0;
    bpf_map_update_elem(&sock_data_args, &id, &data_args, BPF_ANY);
    return 0;
}

KRETPROBE_SYSCALL(recvmsg)
{
    int is_udp = 0;
    conn_ctx_t id = bpf_get_current_pid_tgid();
    ssize_t bytes_count = PT_REGS_RC(ctx);
    struct sock_conn_s* sock_conn = NULL;

    // Filter by UDP tracing-on/off
    if (is_tracing_udp()) {
        struct sys_connect_args_s* args = bpf_map_lookup_elem(&sys_connect_args, &id);
        if (args && bytes_count > 0) {
            sock_conn = lkup_sock_conn((int)(id >> INT_LEN), args->fd);
            // new sock connection
            if (!sock_conn) {
                sock_conn = get_sock_conn_by_addr(ctx, (int)(id >> INT_LEN), args->fd, args->addr);
            }
            if (sock_conn == NULL) {
                goto end;
            }
            is_udp = 1;
            (void)submit_conn_open(ctx, sock_conn); // UDP socket open event;
        }
    }

    struct sock_data_args_s* data_args = bpf_map_lookup_elem(&sock_data_args, &id);
    if ((data_args != NULL) && (bytes_count > 0)) {
        if (!is_udp) {
            sock_conn = lkup_sock_conn((int)(id >> INT_LEN), data_args->conn_id.fd);
            if (!sock_conn) {
                sock_conn = get_sock_conn(ctx, (int)(id >> INT_LEN), data_args->conn_id.fd);
            }
            if (sock_conn == NULL) {
                goto end;
            }
            (void)submit_conn_open(ctx, sock_conn); // TCP socket open event;
        }
        submit_sock_data(ctx, sock_conn, id, L7_INGRESS, data_args, (size_t)bytes_count);
    }

end:
    bpf_map_delete_elem(&sys_connect_args, &id);
    bpf_map_delete_elem(&sock_data_args, &id);
    return 0;
}

// int close(int fd);
KPROBE_SYSCALL(close)
{
    conn_ctx_t id = bpf_get_current_pid_tgid();
    if (!is_tracing((int)(id >> INT_LEN))) {
        return 0;
    }

    int fd = (int)PT_REGS_PARM1(ctx);
    if (fd < 0) {
        return 0;
    }

    (void)submit_conn_close(ctx, id, fd);
    return 0;
}

