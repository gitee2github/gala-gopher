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
 * Author: Siki
 * Create: 2022-09-05
 * Description: system probe just in 1 thread, include tcp/net/iostat/inode
 ******************************************************************************/
#ifndef SYSTEM_MEMINFO_RPOBE__H
#define SYSTEM_MEMINFO_RPOBE__H

#pragma once

#define KEY_BUF_LEN 256


struct system_meminfo_field {
    char key[KEY_BUF_LEN];
    unsigned long long value;
};


enum mem_infos {
    MEM_TOTAL = 0,
    MEM_FREE,
    MEM_AVAILABLE,
    BUFFERS,
    CACHED,
    ACTIVE,
    INACTIVE,
    SWAP_TOTAL,
    SWAP_FREE,

    TOTAL_DATA_INDEX,
};

int system_meminfo_init(void);
void system_meminfo_destroy(void);
int system_meminfo_probe(void);


#endif
