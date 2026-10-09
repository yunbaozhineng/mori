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
#include "mori/application/transport/rdma/providers/jmrd/jmrd.hpp"

#include <hip/hip_runtime_api.h>
#include <infiniband/verbs.h>

#include <iostream>

#include "mori/application/utils/check.hpp"
#include "mori/application/utils/math.hpp"
#include "mori/utils/mori_log.hpp"

namespace mori {
namespace application {

/* ---------------------------------------------------------------------------------------------- */
/*                                        Device Attributes                                       */
/* ---------------------------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------------------------- */
/*                                          JmrdCqContainer */
/* ---------------------------------------------------------------------------------------------- */
JmrdCqContainer::JmrdCqContainer(struct ibv_context* ibv_context, const RdmaEndpointConfig& config)
    : config(config) {
  auto dv_api = JmrdDvApi::Instance();

  cqeNum = config.maxCqeNum;
  cqSize = dv_api.jm_get_cq_size(ibv_context, cqeNum);

  auto dbrSize = dv_api.jm_get_cq_dbr_size(ibv_context);

  if (config.onGpu) {
    /// allocate cq queue memory
    HIP_RUNTIME_CHECK(hipExtMallocWithFlags(&cqUmemAddr, cqSize, hipDeviceMallocUncached));
    HIP_RUNTIME_CHECK(hipMemset(cqUmemAddr, 0x0, cqSize));

    /// allocate cq dbr memory
    HIP_RUNTIME_CHECK(hipExtMallocWithFlags(&dbrUmemAddr, dbrSize, hipDeviceMallocUncached));
    HIP_RUNTIME_CHECK(hipMemset(dbrUmemAddr, 0, dbrSize));
  } else {
    int status = posix_memalign(&cqUmemAddr, config.alignment, cqSize);
    assert(!status);
    memset(cqUmemAddr, 0x0, cqSize);

    status = posix_memalign(&dbrUmemAddr, config.alignment, dbrSize);
    assert(!status);
    memset(dbrUmemAddr, 0x0, dbrSize);
  }

  struct jmdv_umem cqe_addr;
  struct jmdv_umem dbr_addr;

  cqe_addr.mem_addr = cqUmemAddr;
  cqe_addr.size = cqSize;
  cqe_addr.type = config.onGpu ? JMDV_MEM_TYPE_GPU_VA : JMDV_MEM_TYPE_HOST_VA;

  dbr_addr.mem_addr = dbrUmemAddr;
  dbr_addr.size = dbrSize;
  dbr_addr.type = config.onGpu ? JMDV_MEM_TYPE_GPU_VA : JMDV_MEM_TYPE_HOST_VA;

  ibv_cq = dv_api.jm_umem_create_cq(ibv_context, cqeNum, &cqe_addr, &dbr_addr, NULL);

  assert(ibv_cq != nullptr);

  uint8_t cqe_size = 0;
  uint32_t cqe_count = 0;

  int status = dv_api.jm_get_cq_info(ibv_cq, &cqn, &cqe_size, &cqe_count);

  assert(!status);

  cqeNum = cqe_count;
  cqeSize = cqe_size;

  MORI_APP_TRACE(
      "JMRD CQ created: cqn={}, cqeNum={}, cqeSize={} cqSize={}, cqUmemAddr=0x{:x}, dbrSize={}, "
      "dbrUmemAddr=0x{:x}",
      cqn, cqeNum, cqeSize, cqSize, reinterpret_cast<uintptr_t>(cqUmemAddr), dbrSize,
      reinterpret_cast<uintptr_t>(dbrUmemAddr));
}

JmrdCqContainer::~JmrdCqContainer() {
  JmrdDvApi::Instance().jm_umem_destroy_cq(ibv_cq);

  /// free memory
  if (config.onGpu) {
    HIP_RUNTIME_CHECK(hipFree(cqUmemAddr));
    HIP_RUNTIME_CHECK(hipFree(dbrUmemAddr));
  } else {
    free(cqUmemAddr);
    free(dbrUmemAddr);
  }
}

/* ---------------------------------------------------------------------------------------------- */
/*                                         JmrdQpContainer                                        */
/* ---------------------------------------------------------------------------------------------- */
JmrdQpContainer::JmrdQpContainer(struct ibv_context* ibv_context, const RdmaEndpointConfig& config,
                                 struct ibv_cq* ibv_cq, JmrdDeviceContext* device_context)
    : ibv_context(ibv_context), config(config), device_context(device_context) {
  assert(config.maxMsgSge <= 2 && "max sge larger than 2");
  // assert(config.maxRecvWr == 0 && "cannot support recv yet");
  assert(config.maxInlineData <= 32 && "max inline data cannot exceed 32");

  auto dv_api = JmrdDvApi::Instance();
  const int max_inline_data = 32;

  sqSize = dv_api.jm_get_sq_size(ibv_context, config.maxMsgsNum, config.maxMsgSge, max_inline_data);
  assert(sqSize > 0);

  dbrSize = dv_api.jm_get_qp_dbr_size(ibv_context);
  assert(dbrSize > 0);

  if (config.onGpu) {
    /// allocate sq queue memory
    HIP_RUNTIME_CHECK(hipExtMallocWithFlags(&sqUmemAddr, sqSize, hipDeviceMallocUncached));
    HIP_RUNTIME_CHECK(hipMemset(sqUmemAddr, 0x0, sqSize));

    /// allocate sq dbr memory
    HIP_RUNTIME_CHECK(hipExtMallocWithFlags(&dbrUmemAddr, dbrSize, hipDeviceMallocUncached));
    HIP_RUNTIME_CHECK(hipMemset(dbrUmemAddr, 0, dbrSize));
  } else {
    int status = posix_memalign(&sqUmemAddr, config.alignment, sqSize);
    assert(!status);
    memset(sqUmemAddr, 0x0, sqSize);

    status = posix_memalign(&dbrUmemAddr, config.alignment, dbrSize);
    assert(!status);
    memset(dbrUmemAddr, 0x0, dbrSize);
  }

  /// get db addr
  struct jmdv_uar db_uar;
  int status = dv_api.jm_get_qp_db(ibv_context, &db_uar);
  assert(!status);

  dbUarAddr = db_uar.reg_addr;
  dbSize = 8;

  if (config.onGpu) {
    dbUarAddr = JmrdPtrMap::GetMappedPtr(dbUarAddr, dbSize);
  }

  assert(dbUarAddr != nullptr);

  auto pd = device_context->GetIbvPd();
  /// create qp
  struct ibv_qp_init_attr_ex qp_attr {};

  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.pd = pd;
  qp_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
  qp_attr.send_cq = ibv_cq;
  qp_attr.recv_cq = ibv_cq;
  qp_attr.cap.max_inline_data = config.maxInlineData;
  qp_attr.cap.max_send_sge = config.maxMsgSge;
  qp_attr.cap.max_send_wr = config.maxMsgsNum;
  qp_attr.cap.max_recv_sge = 2;

  /// do not support recv yet
  qp_attr.cap.max_recv_wr = 0;

  struct jmdv_umem sq_addr;
  struct jmdv_umem dbr_addr;

  sq_addr.mem_addr = sqUmemAddr;
  sq_addr.size = sqSize;
  sq_addr.type = config.onGpu ? JMDV_MEM_TYPE_GPU_VA : JMDV_MEM_TYPE_HOST_VA;

  dbr_addr.mem_addr = dbrUmemAddr;
  dbr_addr.size = dbrSize;
  dbr_addr.type = config.onGpu ? JMDV_MEM_TYPE_GPU_VA : JMDV_MEM_TYPE_HOST_VA;

  ibv_qp = dv_api.jm_umem_create_qp(ibv_context, &qp_attr, &sq_addr, &dbr_addr);

  assert(ibv_qp != nullptr);

  status = dv_api.jm_get_sq_info(ibv_qp, &sqWqeNum, &sqWqeSize);
  assert(!status);

  qpn = ibv_qp->qp_num;

  MORI_APP_TRACE(
      "JMRD QP created: qpn={} sqWqeNum={} sqWqeSize={} sqSize={} sqUmemAddr=0x{:x} "
      "dbrUmemAddr=0x{:x} dbrSize={} dbUarAddr=0x{:x}",
      qpn, sqWqeNum, sqWqeSize, sqSize, reinterpret_cast<uintptr_t>(sqUmemAddr),
      reinterpret_cast<uintptr_t>(dbrUmemAddr), dbrSize, reinterpret_cast<uintptr_t>(dbUarAddr));

  /// allocate atomic buffer
  atomicIbufSize = (RoundUpPowOfTwo(config.atomicIbufSlots) + 1) * ATOMIC_IBUF_SLOT_SIZE;
  if (config.onGpu) {
    HIP_RUNTIME_CHECK(
        hipExtMallocWithFlags(&atomicIbufAddr, atomicIbufSize, hipDeviceMallocUncached));
    HIP_RUNTIME_CHECK(hipMemset(atomicIbufAddr, 0, atomicIbufSize));
  } else {
    status = posix_memalign(&atomicIbufAddr, config.alignment, atomicIbufSize);
    memset(atomicIbufAddr, 0, atomicIbufSize);
    assert(!status);
  }

  int atomicIbufAccessFlag =
      MaybeAddRelaxedOrderingFlag(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                                  IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
  atomicIbufMr = ibv_reg_mr(pd, atomicIbufAddr, atomicIbufSize, atomicIbufAccessFlag);
  assert(atomicIbufMr);

  MORI_APP_TRACE(
      "JMRD Atomic ibuf allocated: addr=0x{:x}, slots={}, size={}, lkey=0x{:x}, rkey=0x{:x}",
      reinterpret_cast<uintptr_t>(atomicIbufAddr), RoundUpPowOfTwo(config.atomicIbufSlots),
      atomicIbufSize, atomicIbufMr->lkey, atomicIbufMr->rkey);
}

JmrdQpContainer::~JmrdQpContainer() {
  /// free atomic resource
  ibv_dereg_mr(atomicIbufMr);

  if (config.onGpu) {
    HIP_RUNTIME_CHECK(hipFree(atomicIbufAddr));
  } else {
    free(atomicIbufAddr);
  }
  /// free qp resource
  auto dv_api = JmrdDvApi::Instance();
  dv_api.jm_umem_destroy_qp(ibv_qp);

  if (config.onGpu) {
    HIP_RUNTIME_CHECK(hipFree(sqUmemAddr));
    HIP_RUNTIME_CHECK(hipFree(dbrUmemAddr));
  } else {
    free(sqUmemAddr);
    free(dbrUmemAddr);
  }
}

void JmrdQpContainer::ModifyRst2Init() {
  struct ibv_qp_attr attr {};

  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = config.portId;
  attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
                         IBV_ACCESS_REMOTE_ATOMIC;

  int attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  int status = ibv_modify_qp(ibv_qp, &attr, attr_mask);

  assert(!status);
}
void JmrdQpContainer::ModifyInit2Rtr(const RdmaEndpointHandle& local_handle,
                                     const RdmaEndpointHandle& remote_handle,
                                     const ibv_port_attr& portAttr) {
  struct ibv_qp_attr attr {};
  int attr_mask;

  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = portAttr.active_mtu;
  attr.dest_qp_num = remote_handle.qpn;
  attr.rq_psn = remote_handle.psn;

  attr.max_dest_rd_atomic = 32;
  attr.min_rnr_timer = 12;

  attr.ah_attr.is_global = 1;
  attr.ah_attr.sl = ReadRdmaServiceLevelEnv().value_or(0);
  attr.ah_attr.grh.traffic_class = ReadRdmaTrafficClassEnv().value_or(120);

  attr.ah_attr.port_num = config.portId;
  attr.ah_attr.grh.hop_limit = 16;

  std::copy(std::begin(remote_handle.eth.gid), std::end(remote_handle.eth.gid),
            attr.ah_attr.grh.dgid.raw);

  attr.ah_attr.grh.sgid_index = local_handle.eth.gidIdx;

  attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_RQ_PSN | IBV_QP_DEST_QPN | IBV_QP_AV |
              IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

  MORI_APP_TRACE(
      "JMRD ModifyInit2Rtr, remote_handle.psn:{}, remote_handle.qpn:{}, gidIdx:{}, port_num:{}",
      remote_handle.psn, remote_handle.qpn, local_handle.eth.gidIdx, config.portId);

  int status = ibv_modify_qp(ibv_qp, &attr, attr_mask);
  assert(!status);
}

void JmrdQpContainer::ModifyRtr2Rts(const RdmaEndpointHandle& local_handle,
                                    const RdmaEndpointHandle& remote_handle, uint32_t qp_idx) {
  struct ibv_qp_attr attr {};
  int attr_mask;

  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.sq_psn = remote_handle.psn;
  attr.max_rd_atomic = 32;

  attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
              IBV_QP_MAX_QP_RD_ATOMIC;

  int status = ibv_modify_qp(ibv_qp, &attr, attr_mask);

  assert(!status);

  const char* env_str = std::getenv("MORI_JMRD_ENABLE_UDP_SPORT");
  bool enable_udp_sport = false;

  if (env_str) {
    std::string val(env_str);
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    enable_udp_sport = (val == "1" || val == "true" || val == "on");
  }

  if (!enable_udp_sport) return;

  uint16_t udp_sport = device_context->GetUdpSport(qp_idx);

  if (udp_sport < 0xC000) {
    MORI_APP_WARN("JMRD set qp udp sport failed: sport {} is less than 0xC000", udp_sport);
    return;
  }

  uint16_t sport[2];
  sport[0] = udp_sport;
  sport[1] = 0;

  status = JmrdDvApi::Instance().jm_set_udp_src_port(ibv_qp, sport, 0x1);

  if (status < 0) {
    MORI_APP_WARN("JMRD set udp src port {}  for qp (qpn:{} idx:{}) failed", udp_sport, qpn,
                  qp_idx);
  } else {
    MORI_APP_INFO("JMRD set udp src port {}  for qp (qpn:{} idx:{}) done", udp_sport, qpn, qp_idx);
  }
}

/* ---------------------------------------------------------------------------------------------- */
/*                                        JmrdDeviceContext                                       */
/* ---------------------------------------------------------------------------------------------- */

JmrdDeviceContext::JmrdDeviceContext(RdmaDevice* rdma_device, ibv_pd* in_pd)
    : RdmaDeviceContext(rdma_device, in_pd) {}

JmrdDeviceContext::~JmrdDeviceContext() {}

RdmaEndpoint JmrdDeviceContext::CreateRdmaEndpoint(const RdmaEndpointConfig& config) {
  assert(!config.enableSrq && "not implemented");

  auto context = GetIbvContext();
  auto cq = new JmrdCqContainer(context, config);
  auto qp = new JmrdQpContainer(context, config, cq->ibv_cq, this);

  RdmaEndpoint endpoint{};

  endpoint.vendorId = RdmaDeviceVendorId::Jaguar;

  endpoint.handle.psn = 0;
  endpoint.handle.portId = config.portId;
  endpoint.handle.maxSge = config.maxMsgSge;
  endpoint.handle.qpn = qp->qpn;

  const ibv_port_attr* portAttr = GetRdmaDevice()->GetPortAttr(config.portId);
  assert(portAttr != nullptr);

  GidSelectionResult gidSelection =
      AutoSelectGidIndex(context, config.portId, portAttr, config.gidIdx);
  assert(gidSelection.gidIdx >= 0 && gidSelection.valid);
  std::copy(std::begin(gidSelection.gid.raw), std::end(gidSelection.gid.raw),
            endpoint.handle.eth.gid);
  endpoint.handle.eth.gidIdx = gidSelection.gidIdx;

  /// qp info
  endpoint.wqHandle.mtuSize = 256U << (portAttr->active_mtu - 1);
  endpoint.wqHandle.sqAddr = qp->sqUmemAddr;
  endpoint.wqHandle.rqAddr = qp->rqUmemAddr;
  endpoint.wqHandle.dbrRecAddr = qp->dbrUmemAddr;
  endpoint.wqHandle.dbrAddr = qp->dbUarAddr;
  endpoint.wqHandle.sqWqeNum = qp->sqWqeNum;
  endpoint.wqHandle.rqWqeNum = qp->rqwqeNum;

  /// cq info
  endpoint.cqHandle.cqAddr = cq->cqUmemAddr;
  endpoint.cqHandle.consIdx = 0;
  endpoint.cqHandle.cqeNum = cq->cqeNum;
  endpoint.cqHandle.cqeSize = cq->cqeSize;
  endpoint.cqHandle.dbrRecAddr = cq->dbrUmemAddr;

  /// atomic info
  endpoint.atomicIbuf.addr = reinterpret_cast<uintptr_t>(qp->atomicIbufAddr);
  endpoint.atomicIbuf.lkey = qp->atomicIbufMr->lkey;
  endpoint.atomicIbuf.rkey = qp->atomicIbufMr->rkey;
  endpoint.atomicIbuf.nslots = RoundUpPowOfTwo(config.atomicIbufSlots);

  cqPool.insert({cq->cqn, std::move(std::unique_ptr<JmrdCqContainer>(cq))});
  qpPool.insert({qp->qpn, std::move(std::unique_ptr<JmrdQpContainer>(qp))});

  MORI_APP_TRACE(
      "JMRD endpoint created: qpn={}, cqn={}, portId={}, gidIdx={}, mtu={} atomicIbuf addr=0x{:x}, "
      "nslots={}",
      qp->qpn, cq->cqn, config.portId, endpoint.handle.eth.gidIdx, endpoint.wqHandle.mtuSize,
      endpoint.atomicIbuf.addr, endpoint.atomicIbuf.nslots);

  return endpoint;
}

void JmrdDeviceContext::ConnectEndpoint(const RdmaEndpointHandle& local,
                                        const RdmaEndpointHandle& remote, uint32_t qp_idx) {
  uint32_t local_qpn = local.qpn;
  assert(qpPool.find(local_qpn) != qpPool.end());
  auto qp = qpPool.at(local_qpn).get();

  MORI_APP_TRACE("JMRD connecting endpoint: local_qpn={}, remote_qpn={}, qpId={}", local_qpn,
                 remote.qpn, qp_idx);

  RdmaDevice* rdmaDevice = GetRdmaDevice();

  const ibv_port_attr* port_attr = rdmaDevice->GetPortAttr(local.portId);
  assert(port_attr != nullptr);

  qp->ModifyRst2Init();
  qp->ModifyInit2Rtr(local, remote, *port_attr);
  qp->ModifyRtr2Rts(local, remote, qp_idx);

  MORI_APP_TRACE("JMRD endpoint connected successfully: local_qpn={}, remote_qpn={}", local_qpn,
                 remote.qpn);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                           JmrdDevice                                           */
/* ---------------------------------------------------------------------------------------------- */
JmrdDevice::JmrdDevice(ibv_device* in_device) : RdmaDevice(in_device) {}
JmrdDevice::~JmrdDevice() {}

RdmaDeviceContext* JmrdDevice::CreateRdmaDeviceContext() {
  ibv_pd* pd = ibv_alloc_pd(defaultContext);
  return new JmrdDeviceContext(this, pd);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                        JmrdPtrMap                                        */
/* ---------------------------------------------------------------------------------------------- */

void* JmrdPtrMap::GetMappedPtr(void* orig_ptr, size_t size, bool is_mem) {
  static JmrdPtrMap map;

  std::lock_guard<std::mutex> guard(map.map_lock);

  auto ir = map.ptr_map.find(orig_ptr);

  if (ir != map.ptr_map.end()) return ir->second;

  ///
  uint32_t flag = hipHostRegisterPortable | hipHostRegisterMapped;
  void* mapped_ptr = nullptr;

  HIP_RUNTIME_CHECK(hipHostRegister(orig_ptr, size, flag));
  HIP_RUNTIME_CHECK(hipHostGetDevicePointer(&mapped_ptr, orig_ptr, 0));

  map.ptr_map[orig_ptr] = mapped_ptr;

  return mapped_ptr;
}

}  // namespace application
}  // namespace mori
