//! Milestone 31: the example Rust kernel component -- a read-only PCI
//! inspection module. Receives information about ONE already-discovered
//! PCI device across the C/Rust boundary (see include/rustffi64.h's
//! `toxenos_pci_info_t`), wraps it in Rust types (`crate::pci`), and
//! logs a human-readable summary. Never touches the device's registers,
//! never maps its MMIO BARs, never takes ownership of it -- this is
//! deliberately NOT the beginning of a real driver claiming a device,
//! just a demonstration that a Rust module can receive real kernel data
//! through the FFI boundary and act on it safely.
use crate::console;
use crate::pci::{PciDeviceInfo, PciInfoRaw};

/// # Safety
/// `info` must be a valid, fully-initialized `PciInfoRaw` (i.e. a
/// `toxenos_pci_info_t` on the C side, populated by
/// `pci64_fill_rust_info`) for the duration of this call. Never
/// retained past it.
#[no_mangle]
pub unsafe extern "C" fn toxenos_rust_pci_demo(info: *const PciInfoRaw) -> i32 {
    if info.is_null() {
        console::log_line("toxenos_rs: pci_demo: NULL info pointer");
        return -1;
    }
    console::log_line("toxenos_rs: pci_demo: received one PCI device from C");
    // SAFETY: contract forwarded from this function's own safety
    // comment above.
    let dev = PciDeviceInfo::from_raw(info);
    dev.log();
    console::log_line("toxenos_rs: pci_demo: done (read-only -- device ownership unchanged)");
    0
}
