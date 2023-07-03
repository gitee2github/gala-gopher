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
 * Description: data stream define
 ******************************************************************************/
#ifndef __DATA_STREAM_H__
#define __DATA_STREAM_H__

#pragma once

#include <stdlib.h>

#include "l7.h"

/**
 * The status of a single parse.
 */
typedef enum parse_state_t {
    // Parse succeeded. Raw data of buffer is consumed.
    STATE_SUCCESS = 0,

    // Parse failed. Raw data of buffer is not consumed. Output is invalid.
    STATE_INVALID,

    // Parse is partial. Raw data of buffer is partially consumed. The parsed output is not fully.
    STATE_NEEDS_MORE_DATA,

    // Parse succeeded. Raw data of buffer is consumed, but the output is ignored.
    STATE_IGNORE,

    // End of stream.
    // Parse succeeded. Row data of buffer is consumed, and output is valid.
    // Parser should stop parsing.
    STATE_EOS,

    STATE_NOT_FOUND,

    STATE_UNKNOWN
} parse_state_t;

struct frame_data_s {
    enum message_type_t msg_type;
    void *frame;
    u64 timestamp_ns;
};

/*
  Used to cache L7 message frame from protocol parser
*/
#define __FRAME_BUF_SIZE   (1024)
struct frame_buf_s {
    struct frame_data_s *frames[__FRAME_BUF_SIZE];
    size_t frame_buf_size;
    size_t current_pos;
};

#define RAW_DATA_FLAGS_INVALID  (0x00000001)

/*
  Used to cache raw data from bpf
*/
struct raw_data_s {
    int flags;
    u64 timestamp_ns;
    size_t data_len;

    // current_pos有效值：[0, data_len - 1]，current_pos = data_len时，证明已解析完当前data[]
    size_t current_pos;
    char data[0];
};

/**
 * Record of matching request and response frames.
 */
struct record_data_s {
    void *record;   // protocol_record
    u64 latency;    // latency of record: resp.timestamp_ns - req.timestamp_ns
};

/**
 * Records of matching request and response frames.
 */
#define RECORD_BUF_SIZE 1024
struct record_buf_s {
    struct record_data_s *records[RECORD_BUF_SIZE];
    size_t record_buf_size;
    size_t err_count;   // error-matched frame-pair count
    size_t req_count;   // raw request frame count
    size_t resp_count;  // raw response frame count
};

/*
  Used to cache continuity data from bpf
*/
#define __RAW_BUF_SIZE   (50)
struct raw_buf_s {
    size_t raw_buf_size;
    struct raw_data_s *raw_datas[__RAW_BUF_SIZE];
};

/*
  Used to Manages data(raw and parsed) in tx OR rx direction on a connection.
*/
struct data_stream_s {
    struct raw_buf_s raw_bufs;
    struct frame_buf_s frame_bufs;

    enum proto_type_t type;
};

int init_data_stream(struct data_stream_s *data_stream);
void deinit_data_stream(struct data_stream_s *data_stream);
void data_stream_pop_frames(struct data_stream_s *data_stream);
int data_stream_parse_frames(enum message_type_t msg_type, struct data_stream_s *data_stream);
int data_stream_add_raw_data(struct data_stream_s *data_stream, const char *data, size_t data_len, u64 timestamp_ns);

#endif
