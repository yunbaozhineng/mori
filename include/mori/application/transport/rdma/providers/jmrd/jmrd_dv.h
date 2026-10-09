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

#if __has_include(<infiniband/jmdv.h>)
#include <infiniband/jmdv.h>
#else

#include <infiniband/verbs.h>

enum jmdv_umem_type {
  JMDV_MEM_TYPE_HOST_VA = 0,
  JMDV_MEM_TYPE_GPU_VA = 1,
};

enum jmdv_caps {
  JMDV_CAPS_RRSP = 1 << 0,
  JMDV_CAPS_WQE_FPSN = 1 << 1,
};

struct jmdv_umem {
  void* mem_addr;
  size_t size;
  enum jmdv_umem_type type;
};

struct jmdv_uar {
  int size;
  void* reg_addr;
};

struct jmdv_cq_init_attr {
  uint64_t comp_mask; /* Use enum jmdv_cq_init_attr_mask */
  uint8_t collapsed;
  uint8_t filter_err_cqe;
};

#endif
