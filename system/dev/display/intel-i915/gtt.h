// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "mmio-space.h"

#include <zx/vmo.h>

namespace i915 {

class Device;

class Gtt {
public:
    void Init(MmioSpace* mmio_space, uint32_t gtt_size);
    bool Insert(MmioSpace* mmio_space, zx::vmo* buffer,
                uint32_t length, uint32_t pte_padding, uint32_t* gm_addr_out);
};

} // namespace i915
