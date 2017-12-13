// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <zircon/types.h>
#include <fbl/canary.h>
#include <fbl/mutex.h>
#include <fbl/vector.h>
#include <object/interrupt_dispatcher.h>
#include <sys/types.h>

class InterruptEventDispatcher final : public InterruptDispatcher {

    struct Interrupt {
        InterruptEventDispatcher* dispatcher;
        zx_time_t timestamp;
        uint32_t flags;
        uint32_t vector;
        uint32_t slot;
    };

public:
    static zx_status_t Create(fbl::RefPtr<Dispatcher>* dispatcher,
                              zx_rights_t* rights);

    InterruptEventDispatcher(const InterruptDispatcher &) = delete;
    InterruptEventDispatcher& operator=(const InterruptDispatcher &) = delete;

    ~InterruptEventDispatcher() final;

    zx_status_t Bind(uint32_t slot, uint32_t vector, uint32_t options) final;
    zx_status_t Unbind(uint32_t slot) final;
    zx_status_t WaitForInterrupt(uint64_t& out_slots) final;
    zx_status_t WaitForInterruptWithTimeStamp(uint32_t& out_slot,
                                              zx_time_t& out_timestamp) final;
    zx_status_t UserSignal(uint32_t slot, zx_time_t timestamp) final;
    void PreWait() final;
    void PostWait() final;

private:
    explicit InterruptEventDispatcher() {}

    static enum handler_return IrqHandler(void* ctx);

    fbl::Canary<fbl::magic("INED")> canary_;

    // next slot for WaitForInterruptWithTimeStamp round-robin
    uint32_t next_wait_timestamp_slot_ = 0;

    // interrupts bound to this dispatcher
    fbl::Vector<Interrupt> interrupts_ TA_GUARDED(lock_);
    fbl::Mutex lock_;
};
