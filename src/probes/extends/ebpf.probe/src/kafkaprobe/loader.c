#include <stdio.h>
#include <stdlib.h>
#include <ifaddrs.h>
#include <errno.h>
#include <unistd.h> //F_OK
#include <linux/if_link.h> //XDP_FLAGS_HW_MODE
#include <linux/bpf.h>
#include <arpa/inet.h> // sockaddr_in

#ifdef BPF_PROG_KERN
#undef BPF_PROG_KERN
#endif
#ifdef BPF_PROG_USER
#undef BPF_PROG_USER
#endif
#include "bpf.h"

#include "kafkaprobe.h"

void set_native_mode(__u32 *flag)
{
    *flag = XDP_FLAGS_UPDATE_IF_NOEXIST;
    *flag &= ~XDP_FLAGS_MODES;    /* Clear flags */
    *flag |= XDP_FLAGS_DRV_MODE;  /* Set   flag */
}

void set_socket_mode(__u32 *flag)
{
    *flag = XDP_FLAGS_UPDATE_IF_NOEXIST;
    *flag &= ~XDP_FLAGS_MODES;    /* Clear flags */
    *flag |= XDP_FLAGS_SKB_MODE;  /* Set   flag */
}

void set_offload_mode(__u32 *flag)
{
    *flag = XDP_FLAGS_UPDATE_IF_NOEXIST;
    *flag &= ~XDP_FLAGS_MODES;    /* Clear flags */
    *flag |= XDP_FLAGS_HW_MODE;  /* Set   flag */
}


struct bpf_object *load(struct KafkaConfig *cfg){
    struct bpf_object *obj;
    int ret;
    struct bpf_program *prog;

    obj = bpf_object__open_file(cfg->load_file_name, NULL);
    if (libbpf_get_error(obj)) {
        ERROR("Opening BPF-OBJ file %s fail\n",cfg->load_file_name);
        return NULL;
    }

#if (CURRENT_LIBBPF_VERSION  >= LIBBPF_VERSION(0, 8))
    prog = bpf_object__next_program(obj, NULL);
#else
    prog = bpf_program__next(NULL, obj);
#endif
    if (!prog) {
        bpf_object__close(obj);
        return NULL;
    }

    bpf_program__set_type(prog, BPF_PROG_TYPE_XDP);
    if (cfg->xdp_flag & XDP_FLAGS_HW_MODE)
        bpf_program__set_ifindex(prog, cfg->ifindex);

    ret = bpf_object__load(obj);
    if (ret) {
        fprintf(stderr, "ERROR: loading BPF-OBJ file %s fail\n",cfg->load_file_name);
        bpf_object__close(obj);
        return NULL;
    }

    return obj;
}

void unload(struct bpf_object *obj){
    if (!obj) {
        return;
    }

#if (CURRENT_LIBBPF_VERSION  >= LIBBPF_VERSION(0, 8))
    bpf_object__close(obj);
#else
    (void)bpf_object__unload(obj);
#endif
}

int link_xdp(struct KafkaConfig *cfg, struct bpf_object *obj){

    struct bpf_program *prog;
    int prog_fd = -1;
    int ret;

#if (CURRENT_LIBBPF_VERSION  < LIBBPF_VERSION(0, 7))
    prog = bpf_program__next(NULL, obj);
#else
    prog = bpf_object__next_program(obj, NULL);
#endif
    if(!prog){
        KFK_ERROR("can't find prog in bpf object\n");
        return 1;
    }

    prog_fd = bpf_program__fd(prog);

#if (CURRENT_LIBBPF_VERSION  >= LIBBPF_VERSION(0, 8))
    ret = bpf_xdp_attach(cfg->ifindex, prog_fd, cfg->xdp_flag, NULL);
#else
    ret = bpf_set_link_xdp_fd(cfg->ifindex, prog_fd, cfg->xdp_flag);
#endif
    if(ret){
        switch(ret){
            case -EBUSY:
                KFK_WARN("net interface %s already loaded XDP prog\n", cfg->ifname);
                break;
            case -EOPNOTSUPP:
                KFK_WARN("net interface %s not support flag 0x%x\n", cfg->ifname, cfg->xdp_flag);
                break;
            default:
                KFK_ERROR("net interface %s link prog %s fail\n",cfg->ifname, cfg->load_file_name);
                break;
        }
        return ret;
    }

    return 0;
}

int unlink_xdp(struct KafkaConfig *cfg){
    int ret;
    __u32 prog_fd;

#if (CURRENT_LIBBPF_VERSION  >= LIBBPF_VERSION(0, 8))
    ret = bpf_xdp_query_id(cfg->ifindex, cfg->xdp_flag, &prog_fd);
#else
    ret = bpf_get_link_xdp_id(cfg->ifindex, &prog_fd, cfg->xdp_flag);
#endif
    if (ret) {
        KFK_ERROR("get link xdp prog fd failed \n");
        return 1;
    }

    if (!prog_fd) {
        KFK_INFO("ifname %s has no XDP prog\n", cfg->ifname);
        return 0;
    }

#if (CURRENT_LIBBPF_VERSION  >= LIBBPF_VERSION(0, 8))
    ret = bpf_xdp_detach(cfg->ifindex, cfg->xdp_flag, NULL);
#else
    ret = bpf_set_link_xdp_fd(cfg->ifindex, -1, cfg->xdp_flag);
#endif
    if (ret < 0) {
        KFK_ERROR("unlink xdp prog from net interface %s fail\n", cfg->ifname);
        return 3;
    }

    return 0;
}

int unpin(struct KafkaConfig *cfg, struct bpf_object *obj){
    int ret;

    char dir_path[LARGE_BUF_SIZE];
    snprintf(dir_path, LARGE_BUF_SIZE, "%s/%s", cfg->pin_path, cfg->ifname);

    ret = bpf_object__unpin_maps(obj, dir_path);
    if(ret){
        KFK_ERROR("can't remove map!\n");
        return 1;
    }

    return 0;
}

int pin(struct KafkaConfig *cfg, struct bpf_object *obj){
    int ret;
    char map_path[LARGE_BUF_SIZE];
    char dir_path[LARGE_BUF_SIZE];
    snprintf(dir_path, LARGE_BUF_SIZE, "%s/%s", cfg->pin_path, cfg->ifname);
    for(int i=0;i<MAP_NUM;i++){
        snprintf(map_path, LARGE_BUF_SIZE, "%s/%s/%s",cfg->pin_path, cfg->ifname, get_map_name(i));

        ret = access(map_path, F_OK);
        if(ret != -1){
            unpin(cfg, obj);
            break;
        }
    }

    ret = bpf_object__pin_maps(obj, dir_path);
    if(ret){
        KFK_ERROR("can't pin map in %s\n", dir_path);
        return 1;
    }

    return 0;
}

struct bpf_object *load_link_pin(struct KafkaConfig *cfg){

    int ret;
    struct bpf_object *obj;

    KFK_INFO("load, link, pin kafka probe prog...\n");
    obj = load(cfg);
    if(!obj){
        KFK_ERROR("can't load bpf object!\n");
        return NULL;
    }

    ret = link_xdp(cfg, obj);

    if (ret == -EBUSY) {
        char replication[4];
        KFK_INFO("Do you want to unlink the XDP prog which is runing? please input 'y' or 'n':\n");
        scanf("%s", replication);
        if (replication[0] == 'y') {
            unlink_xdp(cfg);
            ret = link_xdp(cfg, obj);
        } else {
            unload(obj);
            return NULL;
        }
    }

    if (ret == -EOPNOTSUPP || ret == -ENOMEM) {
        KFK_INFO("Change XDP mode to socket mode...\n");
        set_socket_mode(&cfg->xdp_flag);
        ret = link_xdp(cfg, obj);
    }

    if(ret){
        KFK_ERROR("%d: can't link bpf prog!, \n", ret);
        unload(obj);
        return NULL;
    }

    ret = pin(cfg, obj);
    if(ret){
        KFK_ERROR("can't pin bpf map!\n");
        unlink_xdp(cfg);
        unload(obj);
        return NULL;
    }

    KFK_INFO("load, link, pin kafka probe prog success\n");
    return obj;
}

int unpin_unlink_unload(struct KafkaConfig *cfg, struct bpf_object *obj){
    int ret;

    if (obj == NULL) {
        return 0;
    }

    KFK_INFO("unpin, unlink, unload kafka probe prog...\n");
    ret = unpin(cfg, obj);
    if(ret){
        KFK_ERROR("unpin bpf map fail!\n");
        return 1;
    }

    ret = unlink_xdp(cfg);
    if(ret){
        KFK_ERROR("unlink bpf prog fail!\n");
        return 1;
    }

    unload(obj);

    KFK_INFO("unpin, unlink, unload kafka probe prog success\n");
    return 0;
}

int open_bpf_map_file(struct KafkaConfig *cfg, const char *map_name, int *map_fd){
    char map_path[LARGE_BUF_SIZE];
    snprintf(map_path, LARGE_BUF_SIZE, "%s/%s/%s", cfg->pin_path, cfg->ifname, map_name);

    int fd;
    fd = bpf_obj_get(map_path);
    if(fd < 0){
        KFK_ERROR("Failed to open bpf map file:%s\n", map_path);
        return 1;
    }

    *map_fd = fd;
    return 0;
}

static char local_ip[SMALL_BUF_SIZE];

const char* get_local_ip()
{
    return local_ip;
}

int set_local_ip(char * ifname)
{
    struct ifaddrs *if_addr = 0;
    struct ifaddrs *ifa;
    struct sockaddr_in *addr = 0;
    int ret;
    int family;

    ret = getifaddrs(&if_addr);
    if (ret != 0 || if_addr == NULL) {
        return -1;
    }

    for (ifa = if_addr; ifa != NULL; ifa = ifa->ifa_next) {
        family = ifa->ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) {
            if (strcmp(ifa->ifa_name, ifname) ==0) {
                addr = (struct sockaddr_in*)ifa->ifa_addr;
                inet_ntop(family, &addr->sin_addr, local_ip, SMALL_BUF_SIZE);
                return 0;
            }
        }
    }

    KFK_ERROR("can't find ip address the ifname %s attached!\n", ifname);
    freeifaddrs(if_addr);

    return -1;
}