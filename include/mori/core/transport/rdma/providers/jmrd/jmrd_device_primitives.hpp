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

#include <hip/hip_runtime.h>

#include <cassert>
#include <cstddef>

#include "mori/core/transport/rdma/device_primitives.hpp"
#include "mori/core/transport/rdma/providers/jmrd/jmrd_defs.hpp"
#include "mori/core/transport/rdma/utils.hpp"
#include "mori/core/utils/utils.hpp"

namespace mori {
namespace core {

static __device__ __host__ WcStatus JmHandleErrorCqe(struct jm_cqe* cqe) {
  switch (cqe->status) {
    case MORI_JM_CQE_ST_LOCAL_LENGTH_ERR:
      return WC_LOC_LEN_ERR;
    case MORI_JM_CQE_ST_LOCAL_QP_OP_ERR:
      return WC_LOC_QP_OP_ERR;
    case MORI_JM_CQE_ST_LOCAL_PROT_ERR:
      return WC_LOC_PROT_ERR;
    case MORI_JM_CQE_ST_WR_FLUSH_ERR:
      return WC_WR_FLUSH_ERR;
    case MORI_JM_CQE_ST_MW_BIND_ERR:
      return WC_MW_BIND_ERR;
    case MORI_JM_CQE_ST_BAD_RESP_ERR:
      return WC_BAD_RESP_ERR;
    case MORI_JM_CQE_ST_LOCAL_ACCESS_ERR:
      return WC_LOC_ACCESS_ERR;
    case MORI_JM_CQE_ST_REMOTE_INVAL_REQ_ERR:
      return WC_REM_INV_REQ_ERR;
    case MORI_JM_CQE_ST_REMOTE_ACCESS_ERR:
      return WC_REM_ACCESS_ERR;
    case MORI_JM_CQE_ST_REMOTE_OP_ERR:
      return WC_REM_OP_ERR;
    case MORI_JM_CQE_ST_TRANSPORT_RETRY_EXC_ERR:
      return WC_RETRY_EXC_ERR;
    case MORI_JM_CQE_ST_RNR_RETRY_EXC_ERR:
      return WC_RNR_RETRY_EXC_ERR;
    case MORI_JM_CQE_ST_REMOTE_ABORT_ERR:
      return WC_REM_ABORT_ERR;
    default:
      return WC_GENERAL_ERR;
  }
}

/* ---------------------------------------------------------------------------------------------- */
/*                                       Fill fpsn,rsn                                           */
/* ---------------------------------------------------------------------------------------------- */

/*** packed format ******/
/* rsn[63:56] psn[55:33] slot[32:0] */

#define JM_SLOT_OFFSET 0
#define JM_PSN_OFFSET 33
#define JM_RSN_OFFSET 56

#define JM_SLOT_MASK ((1UL << 32) - 1)
#define JM_SLOT_OVERFLOW (1UL << 32)
#define JM_PSN_MASK ((1UL << 22) - 1)
#define JM_PSN_OVERFLOW (1UL << 22)
#define JM_RSN_MASK ((1UL << 8) - 1)

template <bool useRsn>
__device__ uint32_t atomic_add_packed_rsn_and_psn_internal(WorkQueueHandle& wq, uint32_t incSlot,
                                                           uint32_t incPsn, uint32_t incRsn,
                                                           uint32_t* oldPsn, uint32_t* oldRsn) {
  uint64_t packedInc;

  if (useRsn) {
    packedInc = ((uint64_t)incPsn << JM_PSN_OFFSET) | ((uint64_t)incRsn << JM_RSN_OFFSET) |
                ((uint64_t)incSlot << JM_SLOT_OFFSET);
  } else
    packedInc = ((uint64_t)incPsn << JM_PSN_OFFSET) | ((uint64_t)incSlot << JM_SLOT_OFFSET);

  __hip_atomic_fetch_add(&wq.fpsnIdx, incPsn, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_AGENT);

  uint64_t oldPacked =
      __hip_atomic_fetch_add(&wq.psnRsnPack, packedInc, __ATOMIC_ACQ_REL, __HIP_MEMORY_SCOPE_AGENT);

  uint64_t newPacked = oldPacked + packedInc;

  /// get slot and rsn from old packed directly
  uint32_t baseSlot = (uint32_t)((oldPacked >> JM_SLOT_OFFSET) & JM_SLOT_MASK);

  if (useRsn) *oldRsn = (oldPacked >> JM_RSN_OFFSET) & JM_RSN_MASK;

  uint32_t newPsn = __hip_atomic_load(&wq.fpsnIdx, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_AGENT);
  uint32_t basePsn_low = (uint32_t)((oldPacked >> JM_PSN_OFFSET) & JM_PSN_MASK);
  uint32_t basePsn_hi = newPsn & (~JM_PSN_MASK);
  uint32_t newPsn_low = newPsn & JM_PSN_MASK;

  if (basePsn_low > newPsn_low) {
    basePsn_hi -= JM_PSN_OVERFLOW;
  }

  *oldPsn = basePsn_hi + basePsn_low;

  /// only one thread can go inside
  constexpr uint64_t psn_slot_overflow =
      (JM_PSN_OVERFLOW << JM_PSN_OFFSET) | (JM_SLOT_OVERFLOW << JM_SLOT_OFFSET);

  if ((newPacked & psn_slot_overflow) && !(oldPacked & psn_slot_overflow)) {
    __hip_atomic_fetch_and(&wq.psnRsnPack, ~psn_slot_overflow, __ATOMIC_RELAXED,
                           __HIP_MEMORY_SCOPE_AGENT);
  }

  __hip_atomic_fetch_add(&wq.postIdx, incSlot, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_AGENT);

  return baseSlot;
}
inline __device__ uint32_t atomic_add_packed_rsn_and_psn(WorkQueueHandle& wq, uint32_t incSlot,
                                                         uint32_t incPsn, uint32_t incRsn,
                                                         uint32_t* oldPsn, uint32_t* oldRsn) {
  return atomic_add_packed_rsn_and_psn_internal<true>(wq, incSlot, incPsn, incRsn, oldPsn, oldRsn);
}

inline __device__ uint32_t atomic_add_packed_rsn_and_psn(WorkQueueHandle& wq, uint32_t incSlot,
                                                         uint32_t incPsn, uint32_t* oldPsn) {
  return atomic_add_packed_rsn_and_psn_internal<false>(wq, incSlot, incPsn, 0, oldPsn, nullptr);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                           Post Tasks                                           */
/* ---------------------------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------------------------- */
/*                                        Send / Recv APIs                                        */
/* ---------------------------------------------------------------------------------------------- */

inline __device__ uint64_t JmPostSend(WorkQueueHandle& wq, uint32_t curPostIdx, uint32_t curPsnIdx,
                                      bool cqeSignal, uint32_t qpn, uintptr_t laddr, uint64_t lkey,
                                      size_t bytes) {
  uint8_t signalFlag = cqeSignal ? MORI_JM_SQE_FLAG_CQE : 0x00;
  void* queueBuffAddr = wq.sqAddr;
  uint32_t wqeNum = wq.sqWqeNum;

  uint32_t wqeIdx = curPostIdx & (wqeNum - 1);
  uintptr_t wqeAddr =
      reinterpret_cast<uintptr_t>(queueBuffAddr) + (wqeIdx << MORI_JM_SEND_WQE_SHIFT);

  struct jm_sqe* sqe = reinterpret_cast<struct jm_sqe*>(wqeAddr);
  struct jm_sge* sge = &sqe->sge0;
  uint8_t owner = !(curPostIdx & wqeNum);
  sqe->opcode = (owner << 7) | MORI_JM_OP_SEND;
  sqe->flags = signalFlag;
  sqe->wqe_idx = curPostIdx;
  sqe->qpn = qpn;
  sqe->msg_len = bytes;
  sqe->imm = 0;
  sqe->fpsn_sge_num = (curPsnIdx << 8) | 1;
  sqe->rkey_ssn = 0;
  sqe->rva = 0;

  sge->len = (owner << 31) | bytes;
  sge->key = lkey;
  sge->va = laddr;

  uint64_t db_val = ((uint64_t)((curPostIdx + 1) & 0xFFFF) << 32) | qpn;
  return db_val;
}

template <>
inline __device__ uint64_t PostSend<ProviderType::JMRD>(WorkQueueHandle& wq, uint32_t curPostIdx,
                                                        uint32_t curMsntblSlotIdx,
                                                        uint32_t curPsnIdx, bool cqeSignal,
                                                        uint32_t qpn, uintptr_t laddr,
                                                        uint64_t lkey, size_t bytes) {
  return JmPostSend(wq, curPostIdx, curPsnIdx, cqeSignal, qpn, laddr, lkey, bytes);
}

template <>
inline __device__ uint64_t PostSend<ProviderType::JMRD>(WorkQueueHandle& wq, uint32_t qpn,
                                                        uintptr_t laddr, uint64_t lkey,
                                                        size_t bytes) {
  uint32_t mtuSize = wq.mtuSize;
  int psnCnt = (bytes == 0) ? 1 : (bytes + mtuSize - 1) / mtuSize;
  uint32_t curPsnIdx;
  uint32_t curPostIdx = atomic_add_packed_rsn_and_psn(wq, 1, psnCnt, &curPsnIdx);
  return JmPostSend(wq, curPostIdx, curPsnIdx, true, qpn, laddr, lkey, bytes);
}

inline __device__ uint64_t JmPostRecv(WorkQueueHandle& wq, uint32_t curPostIdx, uint32_t qpn,
                                      uintptr_t laddr, uint64_t lkey, size_t bytes) {
  void* queueBuffAddr = wq.rqAddr;
  uint32_t wqeNum = wq.rqWqeNum;

  uint32_t wqeIdx = curPostIdx & (wqeNum - 1);

  void* wqeAddr = reinterpret_cast<char*>(queueBuffAddr) + wqeIdx * sizeof(struct jm_sge);
  struct jm_sge* wqe_sge = reinterpret_cast<struct jm_sge*>(wqeAddr);
  wqe_sge->len = bytes;
  wqe_sge->key = lkey;
  wqe_sge->va = laddr;
  return reinterpret_cast<uint64_t*>(wqe_sge)[0];
}

template <>
inline __device__ uint64_t PostRecv<ProviderType::JMRD>(WorkQueueHandle& wq, uint32_t curPostIdx,
                                                        bool cqeSignal, uint32_t qpn,
                                                        uintptr_t laddr, uint64_t lkey,
                                                        size_t bytes) {
  return JmPostRecv(wq, curPostIdx, qpn, laddr, lkey, bytes);
}

template <>
inline __device__ uint64_t PostRecv<ProviderType::JMRD>(WorkQueueHandle& wq, uint32_t qpn,
                                                        uintptr_t laddr, uint64_t lkey,
                                                        size_t bytes) {
  uint32_t curPostIdx = atomicAdd(&wq.postIdx, 1);
  return JmPostRecv(wq, curPostIdx, qpn, laddr, lkey, bytes);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                        Read / Write APIs                                       */
/* ---------------------------------------------------------------------------------------------- */
template <bool IsRead>
inline __device__ uint64_t JmPostReadWriteImpl(WorkQueueHandle& wq, uint32_t curPostIdx,
                                               uint32_t curRsnIdx, uint32_t curPsnIdx,
                                               bool cqeSignal, uint32_t qpn, uintptr_t laddr,
                                               uint64_t lkey, uintptr_t raddr, uint64_t rkey,
                                               size_t bytes) {
  constexpr uint32_t opcode = IsRead ? MORI_JM_OP_RDMA_READ : MORI_JM_OP_RDMA_WRITE;
  uint8_t signalFlag = cqeSignal ? MORI_JM_SQE_FLAG_CQE : 0x00;
  void* queueBuffAddr = wq.sqAddr;
  uint32_t wqeNum = wq.sqWqeNum;

  uint32_t wqeIdx = curPostIdx & (wqeNum - 1);
  uintptr_t wqeAddr =
      reinterpret_cast<uintptr_t>(queueBuffAddr) + (wqeIdx << MORI_JM_SEND_WQE_SHIFT);

  struct jm_sqe* sqe = reinterpret_cast<struct jm_sqe*>(wqeAddr);
  struct jm_sge* sge = &sqe->sge0;
  uint8_t owner = !(curPostIdx & wqeNum);
  sqe->opcode = (owner << 7) | opcode;
  sqe->flags = signalFlag;
  sqe->wqe_idx = curPostIdx;
  sqe->qpn = qpn;
  sqe->msg_len = bytes;
  sqe->imm = curRsnIdx;
  sqe->fpsn_sge_num = (curPsnIdx << 8) | 1;
  sqe->rkey_ssn = rkey;
  sqe->rva = raddr;

  sge->len = (owner << 31) | bytes;
  sge->key = lkey;
  sge->va = laddr;

  uint64_t db_val = ((uint64_t)((curPostIdx + 1) & 0xFFFF) << 32) | qpn;
  return db_val;
}

template <>
inline __device__ uint64_t PostReadWrite<ProviderType::JMRD, false>(
    WorkQueueHandle& wq, uint32_t curPostIdx, uint32_t curRsnIdx, uint32_t curPsnIdx,
    bool cqeSignal, uint32_t qpn, uintptr_t laddr, uint64_t lkey, uintptr_t raddr, uint64_t rkey,
    size_t bytes) {
  return JmPostReadWriteImpl<false>(wq, curPostIdx, 0, curPsnIdx, cqeSignal, qpn, laddr, lkey,
                                    raddr, rkey, bytes);
}

template <>
inline __device__ uint64_t PostReadWrite<ProviderType::JMRD, true>(
    WorkQueueHandle& wq, uint32_t curPostIdx, uint32_t curRsnIdx, uint32_t curPsnIdx,
    bool cqeSignal, uint32_t qpn, uintptr_t laddr, uint64_t lkey, uintptr_t raddr, uint64_t rkey,
    size_t bytes) {
  return JmPostReadWriteImpl<true>(wq, curPostIdx, curRsnIdx, curPsnIdx, cqeSignal, qpn, laddr,
                                   lkey, raddr, rkey, bytes);
}

template <>
inline __device__ uint64_t PostReadWrite<ProviderType::JMRD, false>(WorkQueueHandle& wq,
                                                                    uint32_t qpn, uintptr_t laddr,
                                                                    uint64_t lkey, uintptr_t raddr,
                                                                    uint64_t rkey, size_t bytes) {
  uint32_t mtuSize = wq.mtuSize;
  int psnCnt = (bytes == 0) ? 1 : (bytes + mtuSize - 1) / mtuSize;
  uint32_t curPsnIdx;
  uint32_t curPostIdx = atomic_add_packed_rsn_and_psn(wq, 1, psnCnt, &curPsnIdx);
  return JmPostReadWriteImpl<false>(wq, curPostIdx, 0, curPsnIdx, true, qpn, laddr, lkey, raddr,
                                    rkey, bytes);
}

template <>
inline __device__ uint64_t PostReadWrite<ProviderType::JMRD, true>(WorkQueueHandle& wq,
                                                                   uint32_t qpn, uintptr_t laddr,
                                                                   uint64_t lkey, uintptr_t raddr,
                                                                   uint64_t rkey, size_t bytes) {
  uint32_t mtuSize = wq.mtuSize;
  int psnCnt = (bytes == 0) ? 1 : (bytes + mtuSize - 1) / mtuSize;
  uint32_t curPsnIdx, curRsnIdx;
  uint32_t curPostIdx = atomic_add_packed_rsn_and_psn(wq, 1, psnCnt, 1, &curPsnIdx, &curRsnIdx);
  return JmPostReadWriteImpl<true>(wq, curPostIdx, curRsnIdx, curPsnIdx, true, qpn, laddr, lkey,
                                   raddr, rkey, bytes);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                        WriteInline APIs                                        */
/* ---------------------------------------------------------------------------------------------- */
inline __device__ uint64_t JmPostWriteInline(WorkQueueHandle& wq, uint32_t curPostIdx,
                                             uint32_t curPsnIdx, bool cqeSignal, uint32_t qpn,
                                             void* val, uintptr_t raddr, uint64_t rkey,
                                             size_t bytes) {
  assert(bytes <= 2 * sizeof(struct jm_sge));
  uint8_t signalFlag = cqeSignal ? MORI_JM_SQE_FLAG_CQE : 0x00;
  void* queueBuffAddr = wq.sqAddr;
  uint32_t wqeNum = wq.sqWqeNum;

  uint32_t wqeIdx = curPostIdx & (wqeNum - 1);
  uintptr_t wqeAddr =
      reinterpret_cast<uintptr_t>(queueBuffAddr) + (wqeIdx << MORI_JM_SEND_WQE_SHIFT);

  struct jm_sqe* sqe = reinterpret_cast<struct jm_sqe*>(wqeAddr);
  uint8_t* p_data = reinterpret_cast<uint8_t*>(&sqe->sge0);
  uint8_t owner = !(curPostIdx & wqeNum);
  sqe->opcode = (owner << 7) | MORI_JM_OP_RDMA_WRITE;
  sqe->flags = signalFlag | MORI_JM_SQE_FLAG_INLINE;
  sqe->wqe_idx = curPostIdx;
  sqe->qpn = qpn;
  sqe->msg_len = bytes;
  sqe->imm = 0;
  sqe->fpsn_sge_num = (curPsnIdx << 8);
  sqe->rkey_ssn = rkey;
  sqe->rva = raddr;

  for (int i = 0; i < bytes; i++) {
    AtomicStoreRelaxed(reinterpret_cast<uint8_t*>(&sqe->sge0) + i,
                       reinterpret_cast<uint8_t*>(val)[i]);
  }

  uint64_t db_val = ((uint64_t)((curPostIdx + 1) & 0xFFFF) << 32) | qpn;
  return db_val;
}

template <>
inline __device__ uint64_t PostWriteInline<ProviderType::JMRD>(
    WorkQueueHandle& wq, uint32_t curPostIdx, uint32_t curMsntblSlotIdx, uint32_t curPsnIdx,
    bool cqeSignal, uint32_t qpn, void* val, uintptr_t raddr, uint64_t rkey, size_t bytes) {
  return JmPostWriteInline(wq, curPostIdx, curPsnIdx, cqeSignal, qpn, val, raddr, rkey, bytes);
}

template <>
inline __device__ uint64_t PostWriteInline<ProviderType::JMRD>(WorkQueueHandle& wq, uint32_t qpn,
                                                               void* val, uintptr_t raddr,
                                                               uint64_t rkey, size_t bytes) {
  uint32_t curPsnIdx;
  uint32_t curPostIdx = atomic_add_packed_rsn_and_psn(wq, 1, 1, &curPsnIdx);
  return JmPostWriteInline(wq, curPostIdx, curPsnIdx, true, qpn, val, raddr, rkey, bytes);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                        Atomic APIs                                             */
/* ---------------------------------------------------------------------------------------------- */
inline __device__ uint64_t JmPostAtomicImpl(WorkQueueHandle& wq, uint32_t curPostIdx,
                                            uint32_t curRsnIdx, uint32_t curPsnIdx, bool cqeSignal,
                                            uint32_t qpn, uintptr_t laddr, uint64_t lkey,
                                            uintptr_t raddr, uint64_t rkey, void* val_1,
                                            void* val_2, uint32_t bytes, atomicType amo_op) {
  uint8_t signalFlag = cqeSignal ? MORI_JM_SQE_FLAG_CQE : 0x00;
  void* queueBuffAddr = wq.sqAddr;
  uint32_t wqeNum = wq.sqWqeNum;

  uint32_t wqeIdx = curPostIdx & (wqeNum - 1);
  uintptr_t wqeAddr =
      reinterpret_cast<uintptr_t>(queueBuffAddr) + (wqeIdx << MORI_JM_SEND_WQE_SHIFT);

  struct jm_sqe* sqe = reinterpret_cast<struct jm_sqe*>(wqeAddr);
  struct jm_sge* sge = &sqe->sge0;
  struct jm_atomic_sge* atmoic_sge = reinterpret_cast<struct jm_atomic_sge*>(&sqe->sge1);
  uint8_t owner = !(curPostIdx & wqeNum);
  uint64_t data = val_1 ? *static_cast<uint64_t*>(val_1) : 0;
  uint64_t cmp = val_2 ? *static_cast<uint64_t*>(val_2) : 0;
  uint32_t opcode = MORI_JM_OP_ATOM_FETCH_AND_ADD;
  switch (amo_op) {
    case AMO_FETCH_INC:
    case AMO_INC: {
      opcode = MORI_JM_OP_ATOM_FETCH_AND_ADD;
      data = 1;
      break;
    }
    case AMO_FETCH_ADD:
    case AMO_SIGNAL_ADD:
    case AMO_ADD: {
      opcode = MORI_JM_OP_ATOM_FETCH_AND_ADD;
      break;
    }
    case AMO_FETCH: {
      opcode = MORI_JM_OP_ATOM_FETCH_AND_ADD;
      data = 0;
      break;
    }
    case AMO_COMPARE_SWAP: {
      opcode = MORI_JM_OP_ATOM_CMP_AND_SWAP;
      break;
    }
    default: {
      MORI_PRINTF("Error: unsupported atomic type (%d)\n", amo_op);
      assert(0);
    }
  }

  sqe->opcode = (owner << 7) | opcode;
  sqe->flags = signalFlag;
  sqe->wqe_idx = curPostIdx;
  sqe->qpn = qpn;
  sqe->msg_len = 8;
  sqe->imm = curRsnIdx;
  sqe->fpsn_sge_num = (curPsnIdx << 8) | 1;
  sqe->rkey_ssn = rkey;
  sqe->rva = raddr;

  sge->len = (owner << 31) | 8;
  sge->key = lkey;
  sge->va = laddr;

  atmoic_sge->swap_add = data;
  atmoic_sge->compare = cmp;

  uint64_t db_val = ((uint64_t)((curPostIdx + 1) & 0xFFFF) << 32) | qpn;
  return db_val;
}

template <>
inline __device__ uint64_t PostAtomic<ProviderType::JMRD>(
    WorkQueueHandle& wq, uint32_t curPostIdx, uint32_t curRsnIdx, uint32_t curPsnIdx,
    bool cqeSignal, uint32_t qpn, uintptr_t laddr, uint64_t lkey, uintptr_t raddr, uint64_t rkey,
    void* val_1, void* val_2, uint32_t typeBytes, atomicType amo_op) {
  return JmPostAtomicImpl(wq, curPostIdx, curRsnIdx, curPsnIdx, cqeSignal, qpn, laddr, lkey, raddr,
                          rkey, val_1, val_2, typeBytes, amo_op);
}

template <>
inline __device__ uint64_t PostAtomic<ProviderType::JMRD>(WorkQueueHandle& wq, uint32_t qpn,
                                                          uintptr_t laddr, uint64_t lkey,
                                                          uintptr_t raddr, uint64_t rkey,
                                                          void* val_1, void* val_2,
                                                          uint32_t typeBytes, atomicType amo_op) {
  uint32_t curPsnIdx, curRsnIdx;
  uint32_t curPostIdx = atomic_add_packed_rsn_and_psn(wq, 1, 1, 1, &curPsnIdx, &curRsnIdx);
  return JmPostAtomicImpl(wq, curPostIdx, curRsnIdx, curPsnIdx, true, qpn, laddr, lkey, raddr, rkey,
                          val_1, val_2, typeBytes, amo_op);
}

#define DEFINE_JM_POST_ATOMIC_SPEC(TYPE)                                                         \
  template <>                                                                                    \
  inline __device__ uint64_t PostAtomic<ProviderType::JMRD, TYPE>(                               \
      WorkQueueHandle & wq, uint32_t curPostIdx, uint32_t curRsnIdx, uint32_t curPsnIdx,         \
      bool cqeSignal, uint32_t qpn, uintptr_t laddr, uint64_t lkey, uintptr_t raddr,             \
      uint64_t rkey, const TYPE val_1, const TYPE val_2, atomicType amo_op) {                    \
    return JmPostAtomicImpl(wq, curPostIdx, curRsnIdx, curPsnIdx, cqeSignal, qpn, laddr, lkey,   \
                            raddr, rkey, (void*)&val_1, (void*)&val_2, sizeof(TYPE), amo_op);    \
  }                                                                                              \
  template <>                                                                                    \
  inline __device__ uint64_t PostAtomic<ProviderType::JMRD, TYPE>(                               \
      WorkQueueHandle & wq, uint32_t qpn, uintptr_t laddr, uint64_t lkey, uintptr_t raddr,       \
      uint64_t rkey, const TYPE val_1, const TYPE val_2, atomicType amo_op) {                    \
    uint32_t typeBytes = sizeof(TYPE);                                                           \
    uint32_t curPsnIdx, curRsnIdx;                                                               \
    uint32_t curPostIdx = atomic_add_packed_rsn_and_psn(wq, 1, 1, 1, &curPsnIdx, &curRsnIdx);    \
    return JmPostAtomicImpl(wq, curPostIdx, curRsnIdx, curPsnIdx, true, qpn, laddr, lkey, raddr, \
                            rkey, (void*)&val_1, (void*)&val_2, typeBytes, amo_op);              \
  }

// DEFINE_JM_POST_ATOMIC_SPEC(uint32_t)
DEFINE_JM_POST_ATOMIC_SPEC(uint64_t)
// DEFINE_JM_POST_ATOMIC_SPEC(int32_t)
DEFINE_JM_POST_ATOMIC_SPEC(int64_t)

#undef DEFINE_JM_POST_ATOMIC_SPEC

/* ---------------------------------------------------------------------------------------------- */
/*                                            Doorbell                                            */
/* ---------------------------------------------------------------------------------------------- */
template <>
inline __device__ void UpdateSendDbrRecord<ProviderType::JMRD>(void* dbrRecAddr, uint32_t wqeIdx) {
  core::AtomicStoreSeqCstSystem(
      reinterpret_cast<uint16_t*>(dbrRecAddr) + MORI_JM_SQ_DBR / sizeof(uint16_t),
      (uint16_t)(wqeIdx & 0xffff));
}

template <>
inline __device__ void UpdateRecvDbrRecord<ProviderType::JMRD>(void* dbrRecAddr, uint32_t wqeIdx) {
  core::AtomicStoreSeqCstSystem(
      reinterpret_cast<uint16_t*>(dbrRecAddr) + MORI_JM_RQ_DBR / sizeof(uint16_t),
      (uint16_t)(wqeIdx & 0xffff));
}

template <>
inline __device__ void RingDoorbell<ProviderType::JMRD>(void* dbrAddr, uint64_t dbrVal) {
  core::AtomicStoreSeqCstSystem(reinterpret_cast<uint64_t*>(dbrAddr), dbrVal);
}

template <>
inline __device__ void UpdateDbrAndRingDbSend<ProviderType::JMRD>(void* dbrRecAddr, uint32_t wqeIdx,
                                                                  void* dbrAddr, uint64_t dbrVal,
                                                                  uint32_t* lockVar) {
  AcquireLock(lockVar);

  UpdateSendDbrRecord<ProviderType::JMRD>(dbrRecAddr, wqeIdx);
  __threadfence_system();
  RingDoorbell<ProviderType::JMRD>(dbrAddr, dbrVal);

  ReleaseLock(lockVar);
}

template <>
inline __device__ void UpdateDbrAndRingDbRecv<ProviderType::JMRD>(void* dbrRecAddr, uint32_t wqeIdx,
                                                                  void* dbrAddr, uint64_t dbrVal,
                                                                  uint32_t* lockVar) {
  AcquireLock(lockVar);

  UpdateRecvDbrRecord<ProviderType::JMRD>(dbrRecAddr, wqeIdx);
  __threadfence_system();
  RingDoorbell<ProviderType::JMRD>(dbrAddr, dbrVal);

  ReleaseLock(lockVar);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                        Completion Queue                                        */
/* ---------------------------------------------------------------------------------------------- */
template <>
inline __device__ int PollCqOnce<ProviderType::JMRD>(void* cqeAddr, uint32_t cqeNum,
                                                     uint32_t consIdx, uint32_t* wqeIdx) {
  // Get CQE pointer
  volatile struct jm_cqe* cqe = reinterpret_cast<volatile struct jm_cqe*>(cqeAddr);
  uint8_t opcode = cqe->opcode;

  if (!!cqe->owner == !!((consIdx - 1) & cqeNum)) {
    return -1;  // CQE not ready yet
  }

  if (cqe->status != 0) {
    return -2;  // CQE indicates an error
  }

  if (wqeIdx) {
    *wqeIdx = cqe->wqe_idx;
  }
  return opcode;
}

template <>
inline __device__ int PollCq<ProviderType::JMRD>(void* cqAddr, uint32_t cqeNum, uint32_t* consIdx) {
  uint32_t curConsIdx = atomicAdd(consIdx, 1);
  uint32_t cqeIdx = (curConsIdx - 1) & (cqeNum - 1);

  // Get CQE pointer
  char* cqeAddr = reinterpret_cast<char*>(cqAddr) + (cqeIdx * sizeof(struct jm_cqe));

  int opcode = -1;
  do {
    opcode = PollCqOnce<ProviderType::JMRD>(cqeAddr, cqeNum, curConsIdx, nullptr);
    // TODO: Explain clearly why adding a compiler barrier fix hang issue
    asm volatile("" ::: "memory");
  } while (opcode == -1);

  if (opcode == -2) {
    MORI_PRINTF("(%s:%d) CQE error\n", __FILE__, __LINE__);
    return -1;
  }
  return opcode;
}

template <>
inline __device__ int PollCq<ProviderType::JMRD>(void* cqAddr, uint32_t cqeNum, uint32_t* consIdx,
                                                 uint32_t* wqeCounter) {
  uint32_t curConsIdx = *consIdx;
  uint32_t cqeIdx = (curConsIdx - 1) & (cqeNum - 1);

  // Get CQE pointer
  char* cqeAddr = reinterpret_cast<char*>(cqAddr) + (cqeIdx * sizeof(struct jm_cqe));

  int opcode = -1;
  do {
    opcode = PollCqOnce<ProviderType::JMRD>(cqeAddr, cqeNum, curConsIdx, wqeCounter);
    // TODO: Explain clearly why adding a compiler barrier fix hang issue
    asm volatile("" ::: "memory");
  } while (opcode == -1);

  if (opcode == -2) {
    MORI_PRINTF("(%s:%d) CQE error\n", __FILE__, __LINE__);
    return -1;
  }
  return opcode;
}

template <>
inline __device__ int PollCq<ProviderType::JMRD>(WorkQueueHandle& wqHandle,
                                                 CompletionQueueHandle& cqHandle, void* cqAddr,
                                                 uint32_t cqeNum, uint32_t* consIdx,
                                                 uint16_t* wqeCounter) {
  return 0;
}

template <>
inline __device__ void UpdateCqDbrRecord<ProviderType::JMRD>(CompletionQueueHandle& cq,
                                                             uint32_t consIdx) {
  reinterpret_cast<uint64_t*>(cq.dbrRecAddr)[0] = consIdx;
}

template <>
inline __device__ int PollCqAndUpdateDbr<ProviderType::JMRD>(CompletionQueueHandle& cq,
                                                             uint32_t* consIdx, uint32_t* lockVar) {
  AcquireLock(lockVar);

  int opcode = PollCq<ProviderType::JMRD>(cq.cqAddr, cq.cqeNum, consIdx);
  if (opcode >= 0) {
    UpdateCqDbrRecord<ProviderType::JMRD>(cq, *consIdx);
  }

  ReleaseLock(lockVar);
  return opcode;
}

}  // namespace core
}  // namespace mori
