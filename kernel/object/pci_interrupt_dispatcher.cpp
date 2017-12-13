// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#if WITH_DEV_PCIE

#include <object/pci_interrupt_dispatcher.h>

#include <kernel/auto_lock.h>
#include <zircon/rights.h>
#include <fbl/alloc_checker.h>
#include <object/pci_device_dispatcher.h>
#include <platform.h>

PciInterruptDispatcher::~PciInterruptDispatcher() {
    if (device_) {
        // Unregister our handler.
        __UNUSED zx_status_t ret;
        ret = device_->RegisterIrqHandler(irq_id_, nullptr, nullptr);
        DEBUG_ASSERT(ret == ZX_OK);  // This should never fail.

        // Release our reference to our device.
        device_ = nullptr;
    }
}

pcie_irq_handler_retval_t PciInterruptDispatcher::IrqThunk(const PcieDevice& dev,
                                                           uint irq_id,
                                                           void* ctx) {
    DEBUG_ASSERT(ctx);
    PciInterruptDispatcher* thiz = (PciInterruptDispatcher*)ctx;
    thiz->timestamp_ = current_time();

    // Mask the IRQ at the PCIe hardware level if we can, and (if any threads
    // just became runable) tell the kernel to trigger a reschedule event.
    bool mask = (thiz->flags_ == (LEVEL_TRIGGERED & MASKABLE));
    if (thiz->signal(1 /*FIXME*/) > 0) {
        return (mask ? PCIE_IRQRET_MASK_AND_RESCHED : PCIE_IRQRET_RESCHED);
    } else {
        return (mask ? PCIE_IRQRET_MASK : PCIE_IRQRET_NO_ACTION);
    }
}

zx_status_t PciInterruptDispatcher::Create(
        const fbl::RefPtr<PcieDevice>& device,
        uint32_t irq_id,
        uint32_t flags,
        zx_rights_t* out_rights,
        fbl::RefPtr<Dispatcher>* out_interrupt) {
    // Sanity check our args
    if (!device || !out_rights || !out_interrupt || (flags & ~FLAGS_MASK)) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (!is_valid_interrupt(irq_id, 0)) {
        return ZX_ERR_INTERNAL;
    }

    fbl::AllocChecker ac;
    // Attempt to allocate a new dispatcher wrapper.
    auto interrupt_dispatcher = new (&ac) PciInterruptDispatcher(irq_id, flags);
    fbl::RefPtr<Dispatcher> dispatcher = fbl::AdoptRef<Dispatcher>(interrupt_dispatcher);
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    // Stash reference to the underlying device in the dispatcher we just
    // created, then attempt to register our dispatcher with the bus driver.
    DEBUG_ASSERT(device);
    interrupt_dispatcher->device_ = device;
    zx_status_t result = device->RegisterIrqHandler(irq_id,
                                                           IrqThunk,
                                                           interrupt_dispatcher);
    if (result != ZX_OK) {
        interrupt_dispatcher->device_ = nullptr;
        return result;
    }

    // Everything seems to have gone well.  Make sure the interrupt is unmasked
    // (if it is maskable) then transfer our dispatcher refererence to the
    // caller.
    if (flags & MASKABLE) {
        device->UnmaskIrq(irq_id);
    }
    *out_interrupt = fbl::move(dispatcher);
    *out_rights    = ZX_DEFAULT_PCI_INTERRUPT_RIGHTS;
    return ZX_OK;
}

zx_status_t PciInterruptDispatcher::Bind(uint32_t slot, uint32_t vector, uint32_t options) {
    canary_.Assert();

    // PCI interrupt handles are automatically bound on creation and unbound on handle close
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t PciInterruptDispatcher::Unbind(uint32_t slot) {
    canary_.Assert();

    // PCI interrupt handles are automatically bound on creation and unbound on handle close
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t PciInterruptDispatcher::WaitForInterrupt(uint64_t& out_slots) {
    canary_.Assert();

    return wait(out_slots);
}

zx_status_t PciInterruptDispatcher::WaitForInterruptWithTimeStamp(uint32_t& out_slot,
                                                                  zx_time_t& out_timestamp) {
    canary_.Assert();

    if (flags_ == (LEVEL_TRIGGERED & MASKABLE))
        device_->UnmaskIrq(irq_id_);

    uint64_t slots;
    zx_status_t status = wait(slots);
    if (status == ZX_OK) {
        out_slot = IRQ_SLOT;
        out_timestamp = timestamp_;
    }
    return status;
}

zx_status_t PciInterruptDispatcher::UserSignal(uint32_t slot, zx_time_t timestamp) {
    canary_.Assert();

    if (slot == ZX_INTERRUPT_CANCEL && (flags_ & MASKABLE))
        device_->MaskIrq(irq_id_);

    timestamp_ = timestamp;
    signal(1 << slot, true);
    return ZX_OK;
}

void PciInterruptDispatcher::PreWait() {
    if (flags_ == (LEVEL_TRIGGERED & MASKABLE)) {
        device_->UnmaskIrq(irq_id_);
    }
}

void PciInterruptDispatcher::PostWait() {
    // level triggered interrupt is masked by the IRQ handler so we have nothing to do here
}

#endif  // if WITH_DEV_PCIE
