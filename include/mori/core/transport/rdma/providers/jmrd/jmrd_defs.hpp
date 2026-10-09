// SPDX-License-Identifier: MIT
// Copyright © 2021-2026 Shenzhen Yunbao Microsystems. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#pragma once

#include <cstdint>

namespace mori {
namespace core {

enum jm_opcode {
  MORI_JM_OP_SEND,
  MORI_JM_OP_SEND_WITH_INV,
  MORI_JM_OP_SEND_WITH_IMM,
  MORI_JM_OP_RDMA_WRITE,
  MORI_JM_OP_RDMA_WRITE_WITH_IMM,
  MORI_JM_OP_RDMA_READ,
  MORI_JM_OP_ATOM_CMP_AND_SWAP,
  MORI_JM_OP_ATOM_FETCH_AND_ADD,
  MORI_JM_OP_RSV_0,
  MORI_JM_OP_DFT_8 = MORI_JM_OP_RSV_0,
  MORI_JM_OP_RSV_1,
  MORI_JM_OP_RSV_2,
  MORI_JM_OP_LOCAL_INV,
  MORI_JM_OP_MASK = 0x1f,
};

enum jm_cqe_st {
  MORI_JM_CQE_ST_SUCCESS = 0x00,
  MORI_JM_CQE_ST_LOCAL_LENGTH_ERR = 0x01,
  MORI_JM_CQE_ST_LOCAL_QP_OP_ERR = 0x02,
  MORI_JM_CQE_ST_LOCAL_PROT_ERR = 0x04,
  MORI_JM_CQE_ST_WR_FLUSH_ERR = 0x05,
  MORI_JM_CQE_ST_MW_BIND_ERR = 0x06,
  MORI_JM_CQE_ST_BAD_RESP_ERR = 0x10,
  MORI_JM_CQE_ST_LOCAL_ACCESS_ERR = 0x11,
  MORI_JM_CQE_ST_REMOTE_INVAL_REQ_ERR = 0x12,
  MORI_JM_CQE_ST_REMOTE_ACCESS_ERR = 0x13,
  MORI_JM_CQE_ST_REMOTE_OP_ERR = 0x14,
  MORI_JM_CQE_ST_TRANSPORT_RETRY_EXC_ERR = 0x15,
  MORI_JM_CQE_ST_RNR_RETRY_EXC_ERR = 0x16,
  MORI_JM_CQE_ST_REMOTE_ABORT_ERR = 0x22,
  MORI_JM_CQE_ST_GENERAL_ERR = 0x23,
  MORI_JM_CQE_ST_STATUS_MASK = 0xff,
};

enum {
  MORI_JM_RQ_DBR = 0,
  MORI_JM_SQ_DBR = 64,

  MORI_JM_SQE_FLAG_CQE = 1 << 0,
  MORI_JM_SQE_FLAG_FENCE = 1 << 1,
  MORI_JM_SQE_FLAG_INLINE = 1 << 4,

  MORI_JM_SEND_WQE_SHIFT = 6,
};

struct jm_sge {
  uint32_t len;
  uint32_t key;
  uint64_t va;
};

static_assert(sizeof(jm_sge) == 16, "sge must be 16 bytes");

struct jm_sqe {
  uint8_t opcode;  /// 0-4: op 5-6:rsv 7: owner
  uint8_t flags;   /// 0: cqe 4: inline
  uint16_t wqe_idx;
  // bit 32
  uint32_t qpn;      //?
  uint32_t msg_len;  //
  uint32_t imm;      // immediate
                     // bit 128
  uint32_t fpsn_sge_num;
  // bit 160
  uint32_t rkey_ssn;  // remote key or ssn

  // bit 192
  uint64_t rva;  // remote virtal address
                 // bit 256

  jm_sge sge0;
  jm_sge sge1;
};

static_assert(sizeof(jm_sqe) == 64, "sqe must be 64 bytes");

/// set the first bit one sge entry to indicated done
/// at least 4
struct jm_rqe {
  jm_sge sge[4];
};

static_assert(sizeof(jm_rqe) == 64, "rqe must be 64 bytes");

struct jm_cqe {
  uint8_t opcode : 5;
  uint8_t cq_inline : 1;
  uint8_t is_rq : 1;
  uint8_t owner : 1;
  uint8_t status;
  uint16_t wqe_idx;        // sq or rq index
                           //  bit 32
  uint32_t rkey_imm;       // remote key or immediate
                           //  bit 64
  uint32_t srqn : 24;      //
  uint32_t ingress : 2;    //
  uint32_t rsv_0 : 6;      //
                           // bit 96
  uint32_t affi_qpn : 24;  // qpn
  uint32_t sub_type : 8;   //
                           // bit 128
  uint32_t affi_msg_len;
  //  bit 160
  uint32_t smac_hi : 32;
  uint32_t smac_lo : 16;
  uint32_t roce_type : 2;
  uint32_t vlan_id : 12;
  uint32_t rsv_1 : 1;
  uint32_t vlan_id_valid : 1;
  // bit 224

  uint32_t rqpn : 24;  // remote qpn
  uint32_t sl : 3;     // service level
  uint32_t portn : 3;  //
  uint32_t grh : 1;
  uint32_t loop : 1;
  // bit 256
  uint8_t inline_data[32];
  // bit 512
};

static_assert(sizeof(jm_cqe) == 64, "cqe must be 64 bytes");

// sq additional doorbell
struct jm_sq_db {
  uint32_t tag;
  uint32_t idx;
};

static_assert(sizeof(jm_sq_db) == 8, "db doorbell must be 8 bytes");

/// sq/rq use db_record
struct jm_db_record {
  uint16_t rq_pi;
  uint16_t rsv_0[31];
  uint16_t sq_pi;
  uint16_t rsv_1[31];
  uint16_t xrd_eec_sq_ci;
  uint16_t rsv_2[31];
  uint16_t xrd_bitmap[16];
  uint16_t rsv_3[16];
};

static_assert(sizeof(jm_db_record) == 256, "db doorbell must be 2048 bytes");

//
// looks this is not used
struct jm_cq_db {
  uint32_t tag : 24;
  uint32_t cmd : 1;
  uint32_t flag : 1;
  uint32_t rsv_0 : 6;
  // bit 32
  uint32_t idx : 24;
  uint32_t cmd_sn : 2;  //
  uint32_t nofity : 1;  //
  uint32_t rsv_1 : 5;
};

static_assert(sizeof(jm_cq_db) == 8, "cq doorbell must be 8 bytes");

struct jm_sqe2 {
  uint8_t opcode : 5;
  uint8_t rsv_0 : 2;
  uint8_t owner : 1;      // the invert flag
                          // bit 8
  uint8_t cqe : 1;        // gen cqe or not
  uint8_t fence : 1;      //
  uint8_t so : 1;         // strict order, fixed to zero
  uint8_t se : 1;         // solicited event
  uint8_t in_line : 1;    // inline data
  uint8_t rtt_probe : 1;  // do rtt probe
  uint8_t rsv_1 : 2;      //
                          // bit 16
  uint16_t sq_idx;
  // bit 32
  uint32_t qpn;      //?
  uint32_t msg_len;  //
  uint32_t imm;      // immediate
                     // bit 128
  uint32_t sge_num : 8;
  uint32_t fpsn : 24;  //?
                       // bit 160
  uint32_t rkey_ssn;   // remote key or ssn

  // bit 192
  uint64_t rva;  // remote virtal address
                 // bit 256

  jm_sge sge0;
  jm_sge sge1;
};

struct jm_atomic_sge {
  uint64_t swap_add;
  uint64_t compare;
};

}  // namespace core
}  // namespace mori
