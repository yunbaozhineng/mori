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

#include <infiniband/verbs.h>

#include <memory>
#include <mutex>
#include <unordered_map>

#include "mori/application/transport/rdma/providers/dv_loader.hpp"
#include "mori/application/transport/rdma/providers/jmrd/jmrd_dv.h"
#include "mori/application/transport/rdma/rdma.hpp"

namespace mori {
namespace application {

/* ---------------------------------------------------------------------------------------------- */
/*                                        Device Attributes                                       */
/* ---------------------------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------------------------- */
/*                                 Device Data Structure Container                                */
/* ---------------------------------------------------------------------------------------------- */
class JmrdDeviceContext;  // Forward declaration

class JmrdCqContainer {
 public:
  JmrdCqContainer(struct ibv_context* ibv_context, const RdmaEndpointConfig& config);
  ~JmrdCqContainer();

  RdmaEndpointConfig config;

  void* cqUmemAddr{nullptr};
  void* dbrUmemAddr{nullptr};

  uint32_t cqn{0};

  /// cqe number
  uint32_t cqeNum{0};
  /// cqe size
  uint32_t cqeSize{0};
  /// cq queue size
  uint32_t cqSize{0};
  /// dbr size
  uint32_t dbrSize{0};

  struct ibv_cq* ibv_cq{nullptr};
};

class JmrdQpContainer {
 public:
  JmrdQpContainer(struct ibv_context* ibv_context, const RdmaEndpointConfig& config,
                  struct ibv_cq* ibv_cq, JmrdDeviceContext* device_context);
  ~JmrdQpContainer();

  void ModifyRst2Init();
  void ModifyInit2Rtr(const RdmaEndpointHandle& local_handle,
                      const RdmaEndpointHandle& remote_handle, const ibv_port_attr& portAttr);

  void ModifyRtr2Rts(const RdmaEndpointHandle& local_handle,
                     const RdmaEndpointHandle& remote_handle, uint32_t qp_idx);

  struct ibv_context* ibv_context;
  RdmaEndpointConfig config;
  JmrdDeviceContext* device_context;

  struct ibv_qp* ibv_qp{nullptr};

  uint32_t qpn;
  void* sqUmemAddr{nullptr};
  /// NOTE: recv queue not supported yet
  void* rqUmemAddr{nullptr};

  void* dbrUmemAddr{nullptr};

  void* dbUarAddr{nullptr};

  uint32_t dbrSize{0};
  uint32_t dbSize{0};

  /// the queue size
  uint32_t sqSize{0};
  uint32_t rqSize{0};

  uint32_t sqWqeNum{0};
  uint32_t rqwqeNum{0};

  uint32_t sqWqeSize{0};
  uint32_t rqWqeSize{0};

  // Atomic internal buffer fields
  void* atomicIbufAddr{nullptr};
  size_t atomicIbufSize{0};
  ibv_mr* atomicIbufMr{nullptr};
};

/* ---------------------------------------------------------------------------------------------- */
/*                                        JmrdDeviceContext                                       */
/* ---------------------------------------------------------------------------------------------- */
class JmrdDeviceContext : public RdmaDeviceContext {
 public:
  JmrdDeviceContext(RdmaDevice* rdma_device, ibv_pd* inPd);
  ~JmrdDeviceContext() override;

  virtual RdmaEndpoint CreateRdmaEndpoint(const RdmaEndpointConfig&) override;
  virtual void ConnectEndpoint(const RdmaEndpointHandle& local, const RdmaEndpointHandle& remote,
                               uint32_t qpId = 0) override;

 private:
  std::unordered_map<uint32_t, std::unique_ptr<JmrdCqContainer>> cqPool;
  std::unordered_map<uint32_t, std::unique_ptr<JmrdQpContainer>> qpPool;
};

class JmrdDevice : public RdmaDevice {
 public:
  JmrdDevice(ibv_device* device);
  ~JmrdDevice() override;

  RdmaDeviceContext* CreateRdmaDeviceContext() override;
};

/* ---------------------------------------------------------------------------------------------- */
/*                                        JmrdPtrMap                                        */
/* ---------------------------------------------------------------------------------------------- */

struct JmrdPtrMap {
  static void* GetMappedPtr(void* orig_ptr, size_t size, bool is_mem = false);
  std::mutex map_lock;
  std::unordered_map<void*, void*> ptr_map;
};
}  // namespace application
}  // namespace mori
