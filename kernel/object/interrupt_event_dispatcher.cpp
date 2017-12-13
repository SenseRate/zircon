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

#include <err.h>

// static
zx_status_t InterruptEventDispatcher::Create(uint32_t vector,
                                             uint32_t flags,
                                             fbl::RefPtr<Dispatcher>* dispatcher,
                                             zx_rights_t* rights) {
    // Remap the vector if we have been asked to do so.
    if (flags & ZX_INTERRUPT_REMAP_IRQ)
        vector = remap_interrupt(vector);


    bool default_mode = false;
    enum interrupt_trigger_mode tm = IRQ_TRIGGER_MODE_EDGE;
    enum interrupt_polarity pol = IRQ_POLARITY_ACTIVE_LOW;
    switch (flags & ZX_INTERRUPT_MODE_MASK) {
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

    // If this is not a valid interrupt vector, fail.
    if (!is_valid_interrupt(vector, 0))
        return ZX_ERR_INVALID_ARGS;

    // Attempt to construct the dispatcher.
    fbl::AllocChecker ac;
    InterruptEventDispatcher* disp = new (&ac) InterruptEventDispatcher(vector);
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    // Hold a ref while we check to see if someone else owns this vector or not.
    // If things go wrong, this ref will be released and the IED will get
    // cleaned up automatically.
    auto disp_ref = fbl::AdoptRef<Dispatcher>(disp);

    // Looks like things went well.  Register our callback and unmask our
    // interrupt.
    if (!default_mode) {
        zx_status_t status = configure_interrupt(vector, tm, pol);
        if (status != ZX_OK) {
            return status;
        }
    }
    zx_status_t status = register_int_handler(vector, IrqHandler, disp);
    if (status != ZX_OK) {
        return status;
    }
    unmask_interrupt(vector);
    disp->handler_registered_ = true;

    // Transfer control of the new dispatcher to the creator and we are done.
    *rights     = ZX_DEFAULT_INTERRUPT_RIGHTS;
    *dispatcher = fbl::move(disp_ref);

    return ZX_OK;
}

InterruptEventDispatcher::~InterruptEventDispatcher() {
    // If we were successfully instantiated, then unconditionally mask our vector and
    // clear out our handler (allowing others to  claim the vector if they desire).
    if (handler_registered_) {
        mask_interrupt(vector_);
        register_int_handler(vector_, nullptr, nullptr);
    }
}

zx_status_t InterruptEventDispatcher::InterruptComplete() {
    canary_.Assert();

    unsignal();
    unmask_interrupt(vector_);
    return ZX_OK;
}

zx_status_t InterruptEventDispatcher::UserSignal() {
    canary_.Assert();

    mask_interrupt(vector_);
    signal(true, ZX_ERR_CANCELED);
    return ZX_OK;
}

enum handler_return InterruptEventDispatcher::IrqHandler(void* ctx) {
    InterruptEventDispatcher* thiz = reinterpret_cast<InterruptEventDispatcher*>(ctx);

    // TODO(johngro): make sure that this is safe to do from an IRQ.
    mask_interrupt(thiz->vector_);

    if (thiz->signal() > 0) {
        return INT_RESCHEDULE;
    } else {
        return INT_NO_RESCHEDULE;
    }
}
