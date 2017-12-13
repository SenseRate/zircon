// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <kernel/event.h>
#include <zircon/types.h>
#include <fbl/atomic.h>
#include <object/dispatcher.h>
#include <sys/types.h>

// TODO:
// - maintain a uint32_t state instead of single bit
// - provide a way to bind an ID to another ID
//   to notify a specific bit in state when that ID trips
//   (by default IDs set bit0 of their own state)
// - provide either a dedicated syscall or wire up UserSignal()
//   to allow userspace to set bits for "virtual" interrupts
// - return state via out param on sys_interrupt_wait

// Note that unlike most Dispatcher subclasses, this one is further
// subclassed, and so cannot be final.
class InterruptDispatcher : public Dispatcher {
public:
    InterruptDispatcher& operator=(const InterruptDispatcher &) = delete;

    zx_obj_type_t get_type() const final { return ZX_OBJ_TYPE_INTERRUPT; }

    // Signal the IRQ from non-IRQ state in response to a user-land request.
    virtual zx_status_t UserSignal(uint32_t slot, zx_time_t timestamp) = 0;

    virtual zx_status_t Bind(uint32_t slot, uint32_t vector, uint32_t options) = 0;
    virtual zx_status_t Unbind(uint32_t slot) = 0;
    virtual zx_status_t WaitForInterrupt(uint64_t& out_slots) = 0;
    virtual zx_status_t WaitForInterruptWithTimeStamp(uint32_t& out_slot,
                                                      zx_time_t& out_timestamp) = 0;
    virtual void PreWait() = 0;
    virtual void PostWait() = 0;

    virtual void on_zero_handles() final {
        // Ensure any waiters stop waiting
        event_signal_etc(&event_, false, ZX_ERR_CANCELED);
    }

protected:
    InterruptDispatcher() : signals_(0) {
        event_init(&event_, false, EVENT_FLAG_AUTOUNSIGNAL);
    }

    zx_status_t wait(uint64_t& out_signals) {
        while (true) {
            uint64_t signals = signals_.exchange(0);
            if (signals) {
                if (signals & (1ul << ZX_INTERRUPT_CANCEL)) {
                    return ZX_ERR_CANCELED;
                }
                PostWait();
                out_signals = signals;
                return ZX_OK;
            }

            PreWait();
            zx_status_t status = event_wait_deadline(&event_, ZX_TIME_INFINITE, true);
            if (status != ZX_OK) {
                return status;
            }
        }
    }

    int signal(uint64_t signals, bool resched = false) {
        uint64_t old_signals = signals_.load();
        while (true) {
            if (signals_.compare_exchange_strong(&old_signals, old_signals | signals,
                                                 fbl::memory_order_seq_cst,
                                                 fbl::memory_order_seq_cst)) {
                return event_signal_etc(&event_, resched, ZX_OK); 
            }
        }
    }

private:
    event_t event_;
    fbl::atomic<uint64_t> signals_;
};
