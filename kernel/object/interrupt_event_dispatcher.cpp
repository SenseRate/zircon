// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/interrupt_event_dispatcher.h>

#include <kernel/auto_lock.h>
#include <dev/interrupt.h>
#include <zircon/rights.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <platform.h>

// static
zx_status_t InterruptEventDispatcher::Create(fbl::RefPtr<Dispatcher>* dispatcher,
                                             zx_rights_t* rights) {
    // Attempt to construct the dispatcher.
    fbl::AllocChecker ac;
    InterruptEventDispatcher* disp = new (&ac) InterruptEventDispatcher();
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    // Hold a ref while we check to see if someone else owns this vector or not.
    // If things go wrong, this ref will be released and the IED will get
    // cleaned up automatically.
    auto disp_ref = fbl::AdoptRef<Dispatcher>(disp);

    // Transfer control of the new dispatcher to the creator and we are done.
    *rights     = ZX_DEFAULT_INTERRUPT_RIGHTS;
    *dispatcher = fbl::move(disp_ref);

    return ZX_OK;
}

InterruptEventDispatcher::~InterruptEventDispatcher() {
    for (const auto& interrupt : interrupts_) {
        mask_interrupt(interrupt.vector);
        register_int_handler(interrupt.vector, nullptr, nullptr);
    }
}

zx_status_t InterruptEventDispatcher::Bind(uint32_t slot, uint32_t vector, uint32_t options) {
    canary_.Assert();

    if (slot >= ZX_INTERRUPT_MAX_WAIT_SLOTS)
        return ZX_ERR_OUT_OF_RANGE;

    // Remap the vector if we have been asked to do so.
    if (options & ZX_INTERRUPT_REMAP_IRQ)
        vector = remap_interrupt(vector);

    if (!is_valid_interrupt(vector, 0))
        return ZX_ERR_INVALID_ARGS;

    bool default_mode = false;
    enum interrupt_trigger_mode tm = IRQ_TRIGGER_MODE_EDGE;
    enum interrupt_polarity pol = IRQ_POLARITY_ACTIVE_LOW;
    switch (options & ZX_INTERRUPT_MODE_MASK) {
        case ZX_INTERRUPT_MODE_DEFAULT:
            default_mode = true;
            break;
        case ZX_INTERRUPT_MODE_EDGE_LOW:
            tm = IRQ_TRIGGER_MODE_EDGE;
            pol = IRQ_POLARITY_ACTIVE_LOW;
            break;
        case ZX_INTERRUPT_MODE_EDGE_HIGH:
            tm = IRQ_TRIGGER_MODE_EDGE;
            pol = IRQ_POLARITY_ACTIVE_HIGH;
            break;
        case ZX_INTERRUPT_MODE_LEVEL_LOW:
            tm = IRQ_TRIGGER_MODE_LEVEL;
            pol = IRQ_POLARITY_ACTIVE_LOW;
            break;
        case ZX_INTERRUPT_MODE_LEVEL_HIGH:
            tm = IRQ_TRIGGER_MODE_LEVEL;
            pol = IRQ_POLARITY_ACTIVE_HIGH;
            break;
        default:
            return ZX_ERR_INVALID_ARGS;
    }

    fbl::AutoLock lock(&lock_);

    size_t index = interrupts_.size();
    for (size_t i = 0; i < index; i++) {
        const auto& interrupt = interrupts_[i];
        if (interrupt.vector == vector || interrupt.slot == slot) {
            return ZX_ERR_ALREADY_BOUND;
        }
    }

    if (!default_mode) {
        zx_status_t status = configure_interrupt(vector, tm, pol);
        if (status != ZX_OK) {
            return status;
        }
    }

    Interrupt interrupt;
    interrupt.dispatcher = this;
    interrupt.timestamp = 0;
    interrupt.flags = options;
    interrupt.vector = vector;
    interrupt.slot = slot;
    interrupts_.push_back(interrupt);

    zx_status_t status = register_int_handler(vector, IrqHandler, &interrupts_[index]);
    if (status != ZX_OK) {
        interrupts_.erase(index);
        return status;
    }

    unmask_interrupt(vector);

    return ZX_OK;
}

zx_status_t InterruptEventDispatcher::Unbind(uint32_t slot) {
    canary_.Assert();

    if (slot >= ZX_INTERRUPT_MAX_WAIT_SLOTS)
        return ZX_ERR_OUT_OF_RANGE;

    fbl::AutoLock lock(&lock_);

    size_t size = interrupts_.size();
    for (size_t i = 0; i < size; i++) {
        Interrupt& interrupt = interrupts_[i];
        if (interrupt.slot == slot) {
            mask_interrupt(interrupt.vector);
            register_int_handler(interrupt.vector, nullptr, nullptr);

            interrupts_.erase(i);

            return ZX_OK;
        }
    }

    return ZX_ERR_NOT_FOUND;
}

zx_status_t InterruptEventDispatcher::WaitForInterrupt(uint64_t& out_slots) {
    canary_.Assert();

    return wait(out_slots);
}

zx_status_t InterruptEventDispatcher::WaitForInterruptWithTimeStamp(uint32_t& out_slot,
                                                                    zx_time_t& out_timestamp) {
    canary_.Assert();

    uint64_t slots;
    zx_status_t status = wait(slots);
    if (status != ZX_OK)
        return status;

    DEBUG_ASSERT(slots);

    uint32_t slot = next_wait_timestamp_slot_;
    while (true) {
        uint64_t mask = (1 << slot);
        uint32_t next = slot + 1;
         if (next == ZX_INTERRUPT_MAX_WAIT_SLOTS)
            next = 0;

        if (slots & mask) {
            out_slot = slot;
            for (const auto& interrupt : interrupts_) {
                if (interrupt.slot == slot) {
                    out_timestamp = interrupt.timestamp;
                    break;
                }
            }

            // reset any remaining slots
            slots &= ~mask;
            if (slots)
                signal(slots);

            next_wait_timestamp_slot_ = next;
            return ZX_OK;
        } else {
            slot = next;
        }
    }
}

zx_status_t InterruptEventDispatcher::UserSignal(uint32_t slot, zx_time_t timestamp) {
    canary_.Assert();

    if (slot == ZX_INTERRUPT_CANCEL) {
        for (const auto& interrupt : interrupts_) {
            mask_interrupt(interrupt.vector);
        }
    } else {
        size_t size = interrupts_.size();
        for (size_t i = 0; i < size; i++) {
            Interrupt& interrupt = interrupts_[i];
            if (interrupt.slot == slot) {
                interrupt.timestamp = timestamp;
                break;
            }
        }
    }

    signal(1 << slot, true);
    return ZX_OK;
}

enum handler_return InterruptEventDispatcher::IrqHandler(void* ctx) {
    Interrupt* interrupt = reinterpret_cast<Interrupt*>(ctx);
    interrupt->timestamp = current_time();
    InterruptEventDispatcher* thiz = interrupt->dispatcher;

    if (interrupt->flags && ZX_INTERRUPT_MODE_LEVEL_MASK)
        mask_interrupt(interrupt->vector);

    if (thiz->signal(1 << interrupt->slot) > 0) {
        return INT_RESCHEDULE;
    } else {
        return INT_NO_RESCHEDULE;
    }
}

void InterruptEventDispatcher::PreWait() {
    for (const auto& interrupt : interrupts_) {
        if (interrupt.flags && ZX_INTERRUPT_MODE_LEVEL_MASK) {
            unmask_interrupt(interrupt.vector);
        }
    }
}

void InterruptEventDispatcher::PostWait() {
    for (const auto& interrupt : interrupts_) {
        if (interrupt.flags && ZX_INTERRUPT_MODE_LEVEL_MASK) {
            mask_interrupt(interrupt.vector);
        }
    }
}
