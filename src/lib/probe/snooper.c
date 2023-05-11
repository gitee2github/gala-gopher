/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2023. All rights reserved.
 * gala-gopher licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: luzhihao
 * Create: 2023-04-06
 * Description: snooper
 ******************************************************************************/
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <regex.h>
#include <cjson/cJSON.h>

#include "bpf.h"
#include "container.h"
#include "probe_mng.h"

#include "snooper.h"
#include "snooper.skel.h"
#include "snooper_bpf.h"

// Snooper obj name define
#define SNOOPER_OBJNAME_PROBE       "probe"
#define SNOOPER_OBJNAME_PROCID      "proc_id"
#define SNOOPER_OBJNAME_PROCNAME    "proc_name"
#define SNOOPER_OBJNAME_POD         "pod"
#define SNOOPER_OBJNAME_CONTAINERID "container_id"
#define SNOOPER_OBJNAME_GAUSSDB     "gaussdb"

// 'proc_name' snooper subobj name define'
/*
"proc_name": [
                {
                    "comm": "app1",
                    "cmdline": "",
                    "debuing_dir": ""
                },
                {
                    "comm": "app2",
                    "cmdline": "",
                    "debuing_dir": ""
                }
            ],
*/
#define SNOOPER_OBJNAME_COMM        "comm"
#define SNOOPER_OBJNAME_CMDLINE     "cmdline"
#define SNOOPER_OBJNAME_DBGDIR      "debugging_dir"

// 'gaussdb' snooper subobj name define
/*
"gaussdb": [
                {
                    "dbip": "192.168.1.1",
                    "dbport": 8080,
                    "dbname": "",
                    "dbuser": "",
                    "dbpass": ""
                },
                {
                    "dbip": "192.168.1.1",
                    "dbport": 8081,
                    "dbname": "",
                    "dbuser": "",
                    "dbpass": ""
                }
            ],
*/
#define SNOOPER_OBJNAME_DBIP        "dbip"
#define SNOOPER_OBJNAME_DBPORT      "dbport"
#define SNOOPER_OBJNAME_DBNAME      "dbname"
#define SNOOPER_OBJNAME_DBUSER      "dbuser"
#define SNOOPER_OBJNAME_DBPASS      "dbpass"

static struct probe_mng_s *__probe_mng_snooper = NULL;

struct probe_range_define_s {
    enum probe_type_e probe_type;
    char *desc;
    u32 flags;                      /* Refer to [PROBE] subprobe define. */
};

struct probe_range_define_s probe_range_define[] = {
    {PROBE_FG,     "oncpu",               PROBE_RANGE_ONCPU},
    {PROBE_FG,     "offcpu",              PROBE_RANGE_OFFCPU},
    {PROBE_FG,     "mem",                 PROBE_RANGE_MEM},

    {PROBE_L7,     "l7_bytes_metrics",    PROBE_RANGE_L7BYTES_METRICS},
    {PROBE_L7,     "l7_rpc_metrics",      PROBE_RANGE_L7RPC_METRICS},
    {PROBE_L7,     "l7_rpc_trace",        PROBE_RANGE_L7RPC_TRACE},

    {PROBE_TCP,    "tcp_abnormal",        PROBE_RANGE_TCP_ABNORMAL},
    {PROBE_TCP,    "tcp_rtt",             PROBE_RANGE_TCP_RTT},
    {PROBE_TCP,    "tcp_windows",         PROBE_RANGE_TCP_WINDOWS},
    {PROBE_TCP,    "tcp_srtt",            PROBE_RANGE_TCP_SRTT},
    {PROBE_TCP,    "tcp_rate",            PROBE_RANGE_TCP_RATE},
    {PROBE_TCP,    "tcp_sockbuf",         PROBE_RANGE_TCP_SOCKBUF},
    {PROBE_TCP,    "tcp_stats",           PROBE_RANGE_TCP_STATS},

    {PROBE_SOCKET, "tcp_socket",          PROBE_RANGE_SOCKET_TCP},
    {PROBE_SOCKET, "udp_socket",          PROBE_RANGE_SOCKET_UDP},

    {PROBE_IO,     "io_trace",            PROBE_RANGE_IO_TRACE},
    {PROBE_IO,     "io_err",              PROBE_RANGE_IO_ERR},
    {PROBE_IO,     "io_count",            PROBE_RANGE_IO_COUNT},
    {PROBE_IO,     "page_cache",          PROBE_RANGE_IO_PAGECACHE},

    {PROBE_PROC,   "base_metrics",        PROBE_RANGE_PROC_BASIC},
    {PROBE_PROC,   "proc_syscall",        PROBE_RANGE_PROC_SYSCALL},
    {PROBE_PROC,   "proc_fs",             PROBE_RANGE_PROC_FS},
    {PROBE_PROC,   "proc_io",             PROBE_RANGE_PROC_IO},
    {PROBE_PROC,   "proc_dns",            PROBE_RANGE_PROC_DNS},
    {PROBE_PROC,   "proc_pagecache",      PROBE_RANGE_PROC_PAGECACHE}
};

static void refresh_snooper_obj(struct probe_s *probe);

static int get_probe_range(const char *range)
{

    size_t size = sizeof(probe_range_define) / sizeof(struct probe_range_define_s);

    for (int i = 0; i < size; i++) {
        if (!strcasecmp(probe_range_define[i].desc, range)) {
            return probe_range_define[i].flags;
        }
    }

    return 0;
}

void free_snooper_conf(struct snooper_conf_s* snooper_conf)
{
    if (snooper_conf == NULL) {
        return;
    }

    if (snooper_conf->type = SNOOPER_CONF_APP) {
        if (snooper_conf->conf.app.cmdline) {
            (void)free(snooper_conf->conf.app.cmdline);
        }
        if (snooper_conf->conf.app.debuging_dir) {
            (void)free(snooper_conf->conf.app.debuging_dir);
        }
    }

    if (snooper_conf->type = SNOOPER_CONF_GAUSSDB) {
        if (snooper_conf->conf.gaussdb.dbname) {
            (void)free(snooper_conf->conf.gaussdb.dbname);
        }
        if (snooper_conf->conf.gaussdb.usr) {
            (void)free(snooper_conf->conf.gaussdb.usr);
        }
        if (snooper_conf->conf.gaussdb.pass) {
            (void)free(snooper_conf->conf.gaussdb.pass);
        }
        if (snooper_conf->conf.gaussdb.ip) {
            (void)free(snooper_conf->conf.gaussdb.ip);
        }
    }

    if (snooper_conf->type = SNOOPER_CONF_POD) {
        if (snooper_conf->conf.pod) {
            (void)free(snooper_conf->conf.pod);
        }
    }
    (void)free(snooper_conf);
    snooper_conf = NULL;
}


static struct snooper_conf_s* new_snooper_conf(void)
{
    struct snooper_conf_s* snooper_conf = (struct snooper_conf_s *)malloc(sizeof(struct snooper_conf_s));
    if (snooper_conf == NULL) {
        return NULL;
    }

    (void)memset(snooper_conf, 0, sizeof(struct snooper_conf_s));

    return snooper_conf;
}

static int add_snooper_conf_procid(struct probe_s *probe, u32 proc_id)
{
    if (probe->snooper_conf_num >= SNOOPER_MAX) {
        return -1;
    }

    struct snooper_conf_s* snooper_conf = new_snooper_conf();
    if (snooper_conf == NULL) {
        return -1;
    }
    snooper_conf->type = SNOOPER_CONF_PROC_ID;
    snooper_conf->conf.proc_id = proc_id;

    if (probe->snooper_confs[probe->snooper_conf_num] != NULL) {
        free_snooper_conf(probe->snooper_confs[probe->snooper_conf_num]);
        probe->snooper_confs[probe->snooper_conf_num] = NULL;
    }

    probe->snooper_confs[probe->snooper_conf_num] = snooper_conf;
    probe->snooper_conf_num++;
    return 0;
}

static int add_snooper_conf_procname(struct probe_s *probe,
                            const char* comm, const char *cmdline, const char *dbgdir)
{
    if (probe->snooper_conf_num >= SNOOPER_MAX) {
        return -1;
    }

    if (comm[0] == 0) {
        return 0;
    }

    struct snooper_conf_s* snooper_conf = new_snooper_conf();
    if (snooper_conf == NULL) {
        return -1;
    }

    (void)strncpy(snooper_conf->conf.app.comm, comm, TASK_COMM_LEN);
    if (cmdline && !(comm[0] != 0)) {
        snooper_conf->conf.app.cmdline = strdup(cmdline);
    }
    if (dbgdir && !(dbgdir[0] != 0)) {
        snooper_conf->conf.app.debuging_dir = strdup(dbgdir);
    }
    snooper_conf->type = SNOOPER_CONF_APP;

    if (probe->snooper_confs[probe->snooper_conf_num] != NULL) {
        free_snooper_conf(probe->snooper_confs[probe->snooper_conf_num]);
        probe->snooper_confs[probe->snooper_conf_num] = NULL;
    }

    probe->snooper_confs[probe->snooper_conf_num] = snooper_conf;
    probe->snooper_conf_num++;
    return 0;
}

static int add_snooper_conf_pod(struct probe_s *probe, const char* pod)
{
    if (probe->snooper_conf_num >= SNOOPER_MAX) {
        return -1;
    }
    if (pod[0] == 0) {
        return 0;
    }

    struct snooper_conf_s* snooper_conf = new_snooper_conf();
    if (snooper_conf == NULL) {
        return -1;
    }

    snooper_conf->conf.pod = strdup(pod);
    snooper_conf->type = SNOOPER_CONF_POD;

    if (probe->snooper_confs[probe->snooper_conf_num] != NULL) {
        free_snooper_conf(probe->snooper_confs[probe->snooper_conf_num]);
        probe->snooper_confs[probe->snooper_conf_num] = NULL;
    }

    probe->snooper_confs[probe->snooper_conf_num] = snooper_conf;
    probe->snooper_conf_num++;
    return 0;
}

static int add_snooper_conf_container(struct probe_s *probe, const char* container_id)
{
    if (probe->snooper_conf_num >= SNOOPER_MAX) {
        return -1;
    }

    if (container_id[0] == 0) {
        return 0;
    }

    struct snooper_conf_s* snooper_conf = new_snooper_conf();
    if (snooper_conf == NULL) {
        return -1;
    }

    (void)strncpy(snooper_conf->conf.container_id, container_id, CONTAINER_ABBR_ID_LEN);
    snooper_conf->type = SNOOPER_CONF_CONTAINER_ID;

    if (probe->snooper_confs[probe->snooper_conf_num] != NULL) {
        free_snooper_conf(probe->snooper_confs[probe->snooper_conf_num]);
        probe->snooper_confs[probe->snooper_conf_num] = NULL;
    }

    probe->snooper_confs[probe->snooper_conf_num] = snooper_conf;
    probe->snooper_conf_num++;
    return 0;
}

static int add_snooper_conf_gaussdb(struct probe_s *probe, char *ip, char *dbname,
                                                char *usr, char *pass, u32 port)
{
    if (probe->snooper_conf_num >= SNOOPER_MAX) {
        return -1;
    }

    struct snooper_conf_s* snooper_conf = new_snooper_conf();
    if (snooper_conf == NULL) {
        return -1;
    }

    if (ip && !(ip[0] != 0)) {
        snooper_conf->conf.gaussdb.ip = strdup(ip);
    }
    if (dbname && !(dbname[0] != 0)) {
        snooper_conf->conf.gaussdb.dbname = strdup(dbname);
    }
    if (usr && !(usr[0] != 0)) {
        snooper_conf->conf.gaussdb.usr = strdup(usr);
    }
    if (pass && !(pass[0] != 0)) {
        snooper_conf->conf.gaussdb.pass = strdup(pass);
    }
    snooper_conf->conf.gaussdb.port = port;
    snooper_conf->type = SNOOPER_CONF_GAUSSDB;

    if (probe->snooper_confs[probe->snooper_conf_num] != NULL) {
        free_snooper_conf(probe->snooper_confs[probe->snooper_conf_num]);
        probe->snooper_confs[probe->snooper_conf_num] = NULL;
    }

    probe->snooper_confs[probe->snooper_conf_num] = snooper_conf;
    probe->snooper_conf_num++;
    return 0;
}

static void print_snooper_procid(struct probe_s *probe, cJSON *json)
{
    cJSON *procid_item;
    struct snooper_conf_s *snooper_conf;

    procid_item = cJSON_CreateArray();
    for (int i = 0; i < probe->snooper_conf_num; i++) {
        snooper_conf = probe->snooper_confs[i];
        if (snooper_conf->type != SNOOPER_CONF_PROC_ID) {
            continue;
        }

        cJSON_AddItemToArray(procid_item, cJSON_CreateNumber(snooper_conf->conf.proc_id));
    }
    cJSON_AddItemToObject(json, SNOOPER_OBJNAME_PROCID, procid_item);
}

static int parse_snooper_procid(struct probe_s *probe, const cJSON *json)
{
    int ret;
    cJSON *procid_item, *object;

    procid_item = cJSON_GetObjectItem(json, SNOOPER_OBJNAME_PROCID);
    if (procid_item == NULL) {
        return 0;
    }

    size_t size = cJSON_GetArraySize(procid_item);
    for (int i = 0; i < size; i++) {
        object = cJSON_GetArrayItem(procid_item, i);
        if (object->type != cJSON_Number) {
            return -1;
        }

        ret = add_snooper_conf_procid(probe, (u32)object->valueint);
        if (ret != 0) {
            return -1;
        }
    }

    return 0;
}

/* {"probe":["XX","YY"]} , XX must be string but unsupported probe range will be ignored */
static int parse_snooper_probe(struct probe_s *probe, const cJSON *json)
{
    int ret;
    int range;
    cJSON *probe_item, *object;

    probe_item = cJSON_GetObjectItem(json, SNOOPER_OBJNAME_PROBE);
    if (probe_item == NULL) {
        return 0;
    }

    size_t size = cJSON_GetArraySize(probe_item);
    for (int i = 0; i < size; i++) {
        object = cJSON_GetArrayItem(probe_item, i);
        if (object->type != cJSON_String) {
            return -1;
        }

        range = get_probe_range((const char*)object->valuestring);
        probe->probe_range_flags |= range;
    }

    return 0;
}

static void print_snooper_procname(struct probe_s *probe, cJSON *json)
{
    cJSON *procname_item, *object;
    struct snooper_conf_s *snooper_conf;

    procname_item = cJSON_CreateArray();
    for (int i = 0; i < probe->snooper_conf_num; i++) {
        snooper_conf = probe->snooper_confs[i];
        if (snooper_conf->type != SNOOPER_CONF_APP) {
            continue;
        }

        object = cJSON_CreateObject();
        cJSON_AddStringToObject(object, SNOOPER_OBJNAME_COMM, snooper_conf->conf.app.comm);
        cJSON_AddStringToObject(object, SNOOPER_OBJNAME_CMDLINE, snooper_conf->conf.app.cmdline?:"");
        cJSON_AddStringToObject(object, SNOOPER_OBJNAME_DBGDIR, snooper_conf->conf.app.debuging_dir?:"");
        cJSON_AddItemToArray(procname_item, object);
    }
    cJSON_AddItemToObject(json, SNOOPER_OBJNAME_PROCNAME, procname_item);
}

static int parse_snooper_procname(struct probe_s *probe, const cJSON *json)
{
    int ret;
    cJSON *procname_item, *comm_item, *cmdline_item, *dbgdir_item, *object;
    char *comm, *cmdline, *dbgdir;

    procname_item = cJSON_GetObjectItem(json, SNOOPER_OBJNAME_PROCNAME);
    if (procname_item == NULL) {
        return 0;
    }

    size_t size = cJSON_GetArraySize(procname_item);
    for (int i = 0; i < size; i++) {
        object = cJSON_GetArrayItem(procname_item, i);

        comm_item = cJSON_GetObjectItem(object, SNOOPER_OBJNAME_COMM);
        cmdline_item = cJSON_GetObjectItem(object, SNOOPER_OBJNAME_CMDLINE);
        dbgdir_item = cJSON_GetObjectItem(object, SNOOPER_OBJNAME_DBGDIR);

        if ((comm_item == NULL) || (comm_item->type != cJSON_String)) {
            return -1;
        }

        if (cmdline_item && (cmdline_item->type != cJSON_String)) {
            return -1;
        }

        if (dbgdir_item && (dbgdir_item->type != cJSON_String)) {
            return -1;
        }
        comm = (char *)comm_item->valuestring;
        cmdline = (cmdline_item != NULL) ? (char *)comm_item->valuestring : NULL;
        dbgdir = (dbgdir_item != NULL) ? (char *)dbgdir_item->valuestring : NULL;
        ret = add_snooper_conf_procname(probe, (const char *)comm, (const char *)cmdline, (const char *)dbgdir);
        if (ret != 0) {
            return -1;
        }
    }

    return 0;
}


static void print_snooper_pod_container(struct probe_s *probe, cJSON *json)
{
    cJSON *pod_item, *cntr_item;
    struct snooper_conf_s *snooper_conf;

    pod_item = cJSON_CreateArray();
    cntr_item = cJSON_CreateArray();
    for (int i = 0; i < probe->snooper_conf_num; i++) {
        snooper_conf = probe->snooper_confs[i];
        if (snooper_conf->type == SNOOPER_CONF_POD) {
            cJSON_AddItemToArray(pod_item, cJSON_CreateString(snooper_conf->conf.pod));
            continue;
        }

        if (snooper_conf->type == SNOOPER_CONF_CONTAINER_ID) {
            cJSON_AddItemToArray(cntr_item, cJSON_CreateString(snooper_conf->conf.container_id));
            continue;
        }
    }
    cJSON_AddItemToObject(json, SNOOPER_OBJNAME_POD, pod_item);
    cJSON_AddItemToObject(json, SNOOPER_OBJNAME_CONTAINERID, cntr_item);
}

static int parse_snooper_pod_container(struct probe_s *probe, const cJSON *json, const char *item_name)
{
    int ret;
    cJSON *item, *object;
    int pod_flag = 0;

    if (!strcasecmp(item_name, SNOOPER_OBJNAME_POD)) {
        pod_flag = 1;
    }

    item = cJSON_GetObjectItem(json, item_name);
    if (item == NULL) {
        return 0;
    }

    size_t size = cJSON_GetArraySize(item);
    for (int i = 0; i < size; i++) {
        object = cJSON_GetArrayItem(item, i);
        if (object->type != cJSON_String) {
            return -1;
        }
        if (pod_flag) {
            ret = add_snooper_conf_pod(probe, (const char *)object->valuestring);
        } else {
            ret = add_snooper_conf_container(probe, (const char *)object->valuestring);
        }
        if (ret != 0) {
            return -1;
        }
    }

    return 0;
}

static void print_snooper_gaussdb(struct probe_s *probe, cJSON *json)
{
    cJSON *gaussdb_item, *object;
    struct snooper_conf_s *snooper_conf;

    gaussdb_item = cJSON_CreateArray();
    for (int i = 0; i < probe->snooper_conf_num; i++) {
        snooper_conf = probe->snooper_confs[i];
        if (snooper_conf->type != SNOOPER_CONF_GAUSSDB) {
            continue;
        }

        object = cJSON_CreateObject();
        cJSON_AddStringToObject(object, SNOOPER_OBJNAME_DBIP, snooper_conf->conf.gaussdb.ip?:"");
        cJSON_AddNumberToObject(object, SNOOPER_OBJNAME_DBPORT, snooper_conf->conf.gaussdb.port);
        cJSON_AddStringToObject(object, SNOOPER_OBJNAME_DBNAME, snooper_conf->conf.gaussdb.dbname?:"");
        cJSON_AddStringToObject(object, SNOOPER_OBJNAME_DBUSER, snooper_conf->conf.gaussdb.usr?:"");
        cJSON_AddStringToObject(object, SNOOPER_OBJNAME_DBPASS, snooper_conf->conf.gaussdb.pass?:"");
        cJSON_AddItemToArray(gaussdb_item, object);
    }
    cJSON_AddItemToObject(json, SNOOPER_OBJNAME_GAUSSDB, gaussdb_item);
}

static int parse_snooper_gaussdb(struct probe_s *probe, const cJSON *json)
{
    int ret;
    cJSON *gaussdb_item, *ip_item, *dbname_item, *usr_item, *pass_item, *port_item, *object;
    char *ip, *dbname, *usr, *pass;

    gaussdb_item = cJSON_GetObjectItem(json, SNOOPER_OBJNAME_GAUSSDB);
    if (gaussdb_item == NULL) {
        return 0;
    }

    size_t size = cJSON_GetArraySize(gaussdb_item);
    for (int i = 0; i < size; i++) {
        object = cJSON_GetArrayItem(gaussdb_item, i);

        ip_item = cJSON_GetObjectItem(object, SNOOPER_OBJNAME_DBIP);
        dbname_item = cJSON_GetObjectItem(object, SNOOPER_OBJNAME_DBNAME);
        usr_item = cJSON_GetObjectItem(object, SNOOPER_OBJNAME_DBUSER);
        pass_item = cJSON_GetObjectItem(object, SNOOPER_OBJNAME_DBPASS);
        port_item = cJSON_GetObjectItem(object, SNOOPER_OBJNAME_DBPORT);

        if ((ip_item == NULL) || (ip_item->type != cJSON_String)) {
            return -1;
        }
        if ((dbname_item == NULL) || (dbname_item->type != cJSON_String)) {
            return -1;
        }
        if ((usr_item == NULL) || (usr_item->type != cJSON_String)) {
            return -1;
        }
        if ((pass_item == NULL) || (pass_item->type != cJSON_String)) {
            return -1;
        }
        if ((port_item == NULL) || (port_item->type != cJSON_Number)) {
            return -1;
        }

        ip = (char *)ip_item->valuestring;
        dbname = (char *)dbname_item->valuestring;
        usr = (char *)usr_item->valuestring;
        pass = (char *)pass_item->valuestring;
        ret = add_snooper_conf_gaussdb(probe, ip, dbname, usr, pass, (u32)port_item->valueint);
        if (ret != 0) {
            return -1;
        }
    }

    return 0;
}

void print_snooper(struct probe_s *probe, cJSON *json)
{
    cJSON *range;
    size_t size = sizeof(probe_range_define) / sizeof(struct probe_range_define_s);

    range = cJSON_CreateArray();
    for (int i = 0; i < size; i++) {
        if (probe->probe_type == probe_range_define[i].probe_type) {
            if (probe->probe_range_flags & probe_range_define[i].flags) {
                cJSON_AddItemToArray(range, cJSON_CreateString(probe_range_define[i].desc));
            }
        }
    }
    cJSON_AddItemToObject(json, SNOOPER_OBJNAME_PROBE, range);

    print_snooper_procid(probe, json);
    print_snooper_procname(probe, json);
    print_snooper_pod_container(probe, json);
    print_snooper_gaussdb(probe, json);
}

static int send_snooper_obj(struct probe_s *probe)
{
    //TODO: send snooper obj to probe by ipc msg
    return 0;
}

//TODO: refactor this func
int parse_snooper(struct probe_s *probe, const cJSON *json)
{
    int i;
#if 0
    u32 probe_range_flags_bak;
    u32 snooper_conf_num_bak = probe->snooper_conf_num;
    struct snooper_conf_s *snooper_confs_bak[SNOOPER_MAX] = {0};

    /* Backup and clear current snooper config*/
    probe_range_flags_bak = probe->probe_range_flags;

    snooper_conf_num_bak = probe->snooper_conf_num;
    probe->snooper_conf_num = 0;
    (void)memcpy(&snooper_confs_bak, &probe->snooper_confs, snooper_conf_num_bak * (sizeof(struct snooper_conf_s *)));
    (void)memset(&probe->snooper_confs, 0, snooper_conf_num_bak * (sizeof(struct snooper_conf_s *)));
#endif

    /* free current snooper config */
    for (i = 0 ; i < probe->snooper_conf_num ; i++) {
        free_snooper_conf(probe->snooper_confs[i]);
        probe->snooper_confs[i] = NULL;
    }
    probe->snooper_conf_num = 0;
    probe->probe_range_flags = 0;
    if (parse_snooper_probe(probe, json)) {
        ERROR("[PROBEMNG] Failed to parse range for probe(%s)\n", probe->name);
        return -1;
    }

    if (parse_snooper_procid(probe, json)) {
        ERROR("[PROBEMNG] Failed to parse proc id for probe(name:%s)\n", probe->name);
        return -1;
    }

    if (parse_snooper_procname(probe, json)) {
        ERROR("[PROBEMNG] Failed to parse proc id for probe(name:%s)\n", probe->name);
        return -1;
    }

    if (parse_snooper_pod_container(probe, json, SNOOPER_OBJNAME_POD)) {
        ERROR("[PROBEMNG] Failed to parse podname for probe(name:%s)\n", probe->name);
        return -1;
    }

    if (parse_snooper_pod_container(probe, json, SNOOPER_OBJNAME_CONTAINERID)) {
        ERROR("[PROBEMNG] Failed to parse container id for probe(name:%s)\n", probe->name);
        return -1;
    }

    if (parse_snooper_gaussdb(probe, json)) {
        ERROR("[PROBEMNG] Failed to parse gaussdb info for probe(name:%s)\n", probe->name);
        return -1;
    }

    refresh_snooper_obj(probe);
    return send_snooper_obj(probe);
#if 0
resume_snooper:
    for (i = 0 ; i < snooper_conf_num_bak ; i++) {
        free_snooper_conf(probe->snooper_confs[i]);
        probe->snooper_confs[i] = snooper_confs_bak[i];
    }
    probe->snooper_conf_num = snooper_conf_num_bak;

resume_range:
    probe->probe_range_flags = probe_range_flags_bak;
    return -1;
#endif
}

void free_snooper_obj(struct snooper_obj_s* snooper_obj)
{
    if (snooper_obj == NULL) {
        return;
    }

    if (snooper_obj->type = SNOOPER_OBJ_GAUSSDB) {
        if (snooper_obj->obj.gaussdb.dbname) {
            (void)free(snooper_obj->obj.gaussdb.dbname);
        }
        if (snooper_obj->obj.gaussdb.usr) {
            (void)free(snooper_obj->obj.gaussdb.usr);
        }
        if (snooper_obj->obj.gaussdb.pass) {
            (void)free(snooper_obj->obj.gaussdb.pass);
        }
        if (snooper_obj->obj.gaussdb.ip) {
            (void)free(snooper_obj->obj.gaussdb.ip);
        }
    }
    (void)free(snooper_obj);
    snooper_obj = NULL;
}

static struct snooper_obj_s* new_snooper_obj(void)
{
    struct snooper_obj_s* snooper_obj = (struct snooper_obj_s *)malloc(sizeof(struct snooper_obj_s));
    if (snooper_obj == NULL) {
        return NULL;
    }

    (void)memset(snooper_obj, 0, sizeof(struct snooper_obj_s));

    return snooper_obj;
}


#define __SYS_PROC_DIR  "/proc"
static inline char __is_proc_dir(const char *dir_name)
{
    if (dir_name[0] >= '1' && dir_name[0] <= '9') {
        return 1;
    }
    return 0;
}

static char __chk_snooper_pattern(const char *conf_pattern, const char *target)
{
    int status;
    regex_t re;

    if (target[0] == 0 || conf_pattern[0] == 0) {
        return 0;
    }

    if (regcomp(&re, conf_pattern, REG_EXTENDED | REG_NOSUB) != 0) {
        return 0;
    }

    status = regexec(&re, target, 0, NULL, 0);
    regfree(&re);

    return (status == 0) ? 1 : 0;
}

#define __SYS_PROC_COMM             "/proc/%s/comm"
#define __CAT_SYS_PROC_COMM         "/usr/bin/cat /proc/%s/comm 2> /dev/null"
#define __PROC_NAME_MAX             64
#define __PROC_CMDLINE_MAX          4096
static int __read_proc_comm(const char *dir_name, char *comm, size_t size)
{
    char proc_comm_path[PATH_LEN];
    char cat_comm_cmd[COMMAND_LEN];

    proc_comm_path[0] = 0;
    (void)snprintf(proc_comm_path, PATH_LEN, __SYS_PROC_COMM, dir_name);
    if (access((const char *)proc_comm_path, 0) != 0) {
        return -1;
    }

    cat_comm_cmd[0] = 0;
    (void)snprintf(cat_comm_cmd, COMMAND_LEN, __CAT_SYS_PROC_COMM, dir_name);

    return exec_cmd((const char *)cat_comm_cmd, comm, size);
}

#define __SYS_PROC_CMDLINE          "/proc/%s/cmdline"
static int __read_proc_cmdline(const char *dir_name, char *cmdline, u32 size)
{
    FILE *f = NULL;
    char path[LINE_BUF_LEN];
    int index = 0;

    path[0] = 0;
    (void)snprintf(path, LINE_BUF_LEN, __SYS_PROC_CMDLINE, dir_name);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

    /* parse line */
    while (!feof(f)) {
        if (index >= size - 1) {
            cmdline[index] = '\0';
            break;
        }
        cmdline[index] = fgetc(f);
        if (cmdline[index] == '\"') {
            if (index > size -2) {
                cmdline[index] = '\0';
                break;
            } else {
                cmdline[index] = '\\';
                cmdline[index + 1] =  '\"';
                index++;
            }
        } else if (cmdline[index] == '\0') {
            cmdline[index] = ' ';
        } else if (cmdline[index] == EOF) {
            cmdline[index] = '\0';
        }
        index++;
    }

    cmdline[index] = 0;

    (void)fclose(f);
    return 0;
}

static int __get_snooper_obj_idle(struct probe_s *probe, size_t size)
{
    int pos = -1;
    for (int i = 0; i < size; i++) {
        if (probe->snooper_objs[i] == NULL) {
            pos = i;
            break;
        }
    }
    return pos;
}

static int add_snooper_obj_procid(struct probe_s *probe, u32 proc_id)
{
    pthread_rwlock_wrlock(&probe->rwlock);
    int pos = __get_snooper_obj_idle(probe, SNOOPER_MAX);
    if (pos < 0) {
        pthread_rwlock_unlock(&probe->rwlock);
        return -1;
    }

    struct snooper_obj_s* snooper_obj = new_snooper_obj();
    if (snooper_obj == NULL) {
        pthread_rwlock_unlock(&probe->rwlock);
        return -1;
    }
    snooper_obj->type = SNOOPER_OBJ_PROC;
    snooper_obj->obj.proc.proc_id = proc_id;

    probe->snooper_objs[pos] = snooper_obj;
    pthread_rwlock_unlock(&probe->rwlock);
    return 0;
}

static int add_snooper_obj_cgrp(struct probe_s *probe, u32 knid)
{
    pthread_rwlock_wrlock(&probe->rwlock);
    int pos = __get_snooper_obj_idle(probe, SNOOPER_MAX);
    if (pos < 0) {
        pthread_rwlock_unlock(&probe->rwlock);
        return -1;
    }

    struct snooper_obj_s* snooper_obj = new_snooper_obj();
    if (snooper_obj == NULL) {
        pthread_rwlock_unlock(&probe->rwlock);
        return -1;
    }
    snooper_obj->type = SNOOPER_OBJ_CGRP;
    snooper_obj->obj.cgrp.knid = knid;
    snooper_obj->obj.cgrp.type = CGP_TYPE_CPUACCT;

    probe->snooper_objs[pos] = snooper_obj;
    pthread_rwlock_unlock(&probe->rwlock);
    return 0;
}

static int add_snooper_obj_gaussdb(struct probe_s *probe, struct snooper_gaussdb_s *db_param)
{
    pthread_rwlock_wrlock(&probe->rwlock);
    int pos = __get_snooper_obj_idle(probe, SNOOPER_MAX);
    if (pos < 0) {
        pthread_rwlock_unlock(&probe->rwlock);
        return -1;
    }

    struct snooper_obj_s* snooper_obj = new_snooper_obj();
    if (snooper_obj == NULL) {
        pthread_rwlock_unlock(&probe->rwlock);
        return -1;
    }

    snooper_obj->type = SNOOPER_OBJ_GAUSSDB;
    if (db_param->ip) {
        snooper_obj->obj.gaussdb.ip = strdup(db_param->ip);
    }
    if (db_param->dbname) {
        snooper_obj->obj.gaussdb.dbname = strdup(db_param->dbname);
    }
    if (db_param->usr) {
        snooper_obj->obj.gaussdb.usr = strdup(db_param->usr);
    }
    if (db_param->pass) {
        snooper_obj->obj.gaussdb.pass = strdup(db_param->pass);
    }
    snooper_obj->obj.gaussdb.port = db_param->port;

    probe->snooper_objs[pos] = snooper_obj;
    pthread_rwlock_unlock(&probe->rwlock);
    return 0;
}

static int gen_snooper_by_procname(struct probe_s *probe, struct snooper_conf_s *snooper_conf)
{
    int ret;
    DIR *dir = NULL;
    struct dirent *entry;
    char comm[__PROC_NAME_MAX];
    char cmdline[__PROC_CMDLINE_MAX];

    if (snooper_conf->type != SNOOPER_CONF_APP) {
        return 0;
    }

    dir = opendir(__SYS_PROC_DIR);
    if (dir == NULL) {
        return -1;
    }

    do {
        entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (!__is_proc_dir(entry->d_name) == -1) {
            continue;
        }

        ret = __read_proc_comm(entry->d_name, comm, __PROC_NAME_MAX);
        if (ret) {
            continue;
        }

        if (!__chk_snooper_pattern((const char *)snooper_conf->conf.app.comm, (const char *)comm)) {
            // 'comm' Unmatched
            continue;
        }

        if (snooper_conf->conf.app.cmdline != NULL) {
            cmdline[0] = 0;
            ret = __read_proc_cmdline(entry->d_name, cmdline, __PROC_CMDLINE_MAX);
            if (ret) {
                continue;
            }

            if (strstr(cmdline, snooper_conf->conf.app.cmdline) == NULL) {
                // 'cmdline' Unmatched
                continue;
            }
        }

        // Well matched
        (void)add_snooper_obj_procid(probe, (u32)atoi(entry->d_name));
    } while (1);

    closedir(dir);
    return 0;
}

static int gen_snooper_by_procid(struct probe_s *probe, struct snooper_conf_s *snooper_conf)
{
    if (snooper_conf->type != SNOOPER_CONF_PROC_ID) {
        return 0;
    }

    return add_snooper_obj_procid(probe, snooper_conf->conf.proc_id);
}

static int gen_snooper_by_container(struct probe_s *probe, struct snooper_conf_s *snooper_conf)
{
    int ret;
    unsigned int inode;
    if (snooper_conf->type != SNOOPER_CONF_CONTAINER_ID || snooper_conf->conf.container_id[0] == 0) {
        return 0;
    }

    ret = get_container_cpucg_inode((const char *)snooper_conf->conf.container_id, &inode);
    if (ret) {
        return ret;
    }

    return add_snooper_obj_cgrp(probe, inode);
}

static int gen_snooper_by_pod(struct probe_s *probe, struct snooper_conf_s *snooper_conf)
{
    int i, ret;
    unsigned int inode;
    char pod[POD_NAME_LEN];

    if (snooper_conf->type != SNOOPER_CONF_POD || snooper_conf->conf.pod == NULL) {
        return 0;
    }

    container_tbl* cstbl = get_all_container();
    if (cstbl == NULL) {
        return 0;
    }

    container_info *p = cstbl->cs;
    for (i = 0; i < cstbl->num; i++) {
        pod[0] = 0;
        ret = get_container_pod((const char *)p->abbrContainerId, pod, POD_NAME_LEN);
        if (ret || strcasecmp(pod, snooper_conf->conf.pod)) {
            p++;
            continue;
        }

        ret = get_container_cpucg_inode((const char *)p->abbrContainerId, &inode);
        if (ret) {
            p++;
            continue;
        }

        (void)add_snooper_obj_cgrp(probe, inode);
        p++;
    }
    free_container_tbl(&cstbl);
    return 0;
}

static int gen_snooper_by_gaussdb(struct probe_s *probe, struct snooper_conf_s *snooper_conf)
{
    if (snooper_conf->type != SNOOPER_CONF_GAUSSDB) {
        return 0;
    }

    return add_snooper_obj_gaussdb(probe, &(snooper_conf->conf.gaussdb));
}



typedef int (*probe_snooper_generator)(struct probe_s *, struct snooper_conf_s *);
struct snooper_generator_s {
    enum snooper_conf_e type;
    probe_snooper_generator generator;
};
struct snooper_generator_s snooper_generators[] = {
    {SNOOPER_CONF_APP,           gen_snooper_by_procname   },
    {SNOOPER_CONF_GAUSSDB,       gen_snooper_by_gaussdb    },
    {SNOOPER_CONF_PROC_ID,       gen_snooper_by_procid     },
    {SNOOPER_CONF_POD,           gen_snooper_by_pod        },
    {SNOOPER_CONF_CONTAINER_ID,  gen_snooper_by_container  }
};

/* Flush current snooper obj and re-generate */
static void refresh_snooper_obj(struct probe_s *probe)
{
    int i,j;
    struct snooper_conf_s * snooper_conf;
    struct snooper_generator_s *generator;
    size_t size = sizeof(snooper_generators) / sizeof(struct snooper_generator_s);

    pthread_rwlock_wrlock(&probe->rwlock);
    for (i = 0 ; i < SNOOPER_MAX ; i++) {
        free_snooper_obj(probe->snooper_objs[i]);
        probe->snooper_objs[i] = NULL;
    }
    pthread_rwlock_unlock(&probe->rwlock);

    for (i = 0; i < probe->snooper_conf_num; i++) {
        snooper_conf = probe->snooper_confs[i];
        for (j = 0; j < size ; j++) {
            if (snooper_conf->type == snooper_generators[j].type) {
                generator = &(snooper_generators[j]);
                if (generator->generator(probe, snooper_conf)) {
                    return;
                }
                break;
            }
        }

    }
}

static void __rcv_snooper_proc_exec(struct probe_mng_s *probe_mng, const char* comm, u32 proc_id)
{
    int i, j;
    struct probe_s *probe;
    struct snooper_conf_s *snooper_conf;

    for (i = 0; i < PROBE_TYPE_MAX; i++) {
        probe = probe_mng->probes[i];
        if (!probe) {
            continue;
        }

        for (j = 0; j < probe->snooper_conf_num; j++) {
            snooper_conf = probe->snooper_confs[j];
            if (!snooper_conf || snooper_conf->type != SNOOPER_CONF_APP) {
                continue;
            }
            if (!__chk_snooper_pattern((const char *)(snooper_conf->conf.app.comm), comm)) {
                continue;
            }
            (void)add_snooper_obj_procid(probe, proc_id);
        }
        send_snooper_obj(probe);
    }
}

static void __rcv_snooper_proc_exit(struct probe_mng_s *probe_mng, u32 proc_id)
{
    int i, j;
    struct probe_s *probe;
    struct snooper_obj_s *snooper_obj;

    for (i = 0; i < PROBE_TYPE_MAX; i++) {
        probe = probe_mng->probes[i];
        if (!probe) {
            continue;
        }

        pthread_rwlock_wrlock(&probe->rwlock);
        for (j = 0; j < SNOOPER_MAX; j++) {
            snooper_obj = probe->snooper_objs[j];
            if (!snooper_obj || snooper_obj->type != SNOOPER_OBJ_PROC) {
                continue;
            }

            if (snooper_obj->obj.proc.proc_id == proc_id) {
                free_snooper_obj(snooper_obj);
                probe->snooper_objs[j] = NULL;
                snooper_obj = NULL;
            }
        }
        pthread_rwlock_unlock(&probe->rwlock);
        send_snooper_obj(probe);
    }
}

static void rcv_snooper_proc_evt(void *ctx, int cpu, void *data, __u32 size)
{
    struct snooper_proc_evt_s *evt = data;
    char comm[TASK_COMM_LEN];

    comm[0] = 0;
    char *p = strrchr(evt->filename, '/');
    if (p) {
        strncpy(comm, p + 1, TASK_COMM_LEN - 1);
    } else {
        strncpy(comm, evt->filename, TASK_COMM_LEN - 1);
    }

    if (evt->proc_event == PROC_EXEC) {
        __rcv_snooper_proc_exec(__probe_mng_snooper, (const char *)comm, (u32)evt->pid);
    } else {
        __rcv_snooper_proc_exit(__probe_mng_snooper, (u32)evt->pid);
    }
}

static void rcv_snooper_cgrp_evt(void *ctx, int cpu, void *data, __u32 size)
{
    struct snooper_cgrp_evt_s *msg_data = (struct snooper_cgrp_evt_s *)data;

    // TODO: chk snooper pattern

    return;
}

static void loss_data(void *ctx, int cpu, u64 cnt)
{
    // TODO: debuging
}

int load_snooper_bpf(struct probe_mng_s *probe_mng)
{
    int ret = 0;
    struct snooper_bpf *snooper_skel;

    __probe_mng_snooper = probe_mng;

    /* Open load and verify BPF application */
    snooper_skel = snooper_bpf__open();
    if (!snooper_skel) {
        ret = -1;
        ERROR("Failed to open BPF snooper_skel.\n");
        goto end;
    }

    if (snooper_bpf__load(snooper_skel)) {
        ret = -1;
        ERROR("Failed to load BPF snooper_skel.\n");
        goto end;
    }

    /* Attach tracepoint handler */
    ret = snooper_bpf__attach(snooper_skel);
    if (ret) {
        ERROR("Failed to attach BPF snooper_skel.\n");
        goto end;
    }
    INFO("Succeed to load and attach BPF snooper_skel.\n");

    probe_mng->snooper_proc_pb = create_pref_buffer2(GET_MAP_FD(snooper, snooper_proc_channel),
                                                        rcv_snooper_proc_evt, loss_data);
    probe_mng->snooper_cgrp_pb = create_pref_buffer2(GET_MAP_FD(snooper, snooper_cgrp_channel),
                                                        rcv_snooper_cgrp_evt, loss_data);
    probe_mng->snooper_skel = snooper_skel;

    if (probe_mng->snooper_proc_pb == NULL || probe_mng->snooper_cgrp_pb == NULL) {
        ret = -1;
        goto end;
    }

    return 0;

end:
    if (snooper_skel) {
        snooper_bpf__destroy(snooper_skel);
        probe_mng->snooper_skel = NULL;
    }

    if (probe_mng->snooper_proc_pb) {
        perf_buffer__free(probe_mng->snooper_proc_pb);
        probe_mng->snooper_proc_pb = NULL;
    }
    if (probe_mng->snooper_cgrp_pb) {
        perf_buffer__free(probe_mng->snooper_cgrp_pb);
        probe_mng->snooper_cgrp_pb = NULL;
    }
    return ret;
}

void unload_snooper_bpf(struct probe_mng_s *probe_mng)
{
    if (probe_mng->snooper_skel) {
        snooper_bpf__destroy(probe_mng->snooper_skel);
        probe_mng->snooper_skel = NULL;
    }

    if (probe_mng->snooper_proc_pb) {
        perf_buffer__free(probe_mng->snooper_proc_pb);
        probe_mng->snooper_proc_pb = NULL;
    }
    if (probe_mng->snooper_cgrp_pb) {
        perf_buffer__free(probe_mng->snooper_cgrp_pb);
        probe_mng->snooper_cgrp_pb = NULL;
    }
    __probe_mng_snooper = NULL;
}