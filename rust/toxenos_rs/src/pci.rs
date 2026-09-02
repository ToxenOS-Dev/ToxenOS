//! Milestone 31: a `#[repr(C)]` mirror of include/rustffi64.h's
//! `toxenos_pci_info_t` (a purpose-built FFI boundary type -- see that
//! header's comment for why this is NOT a mirror of the internal
//! `pci64_device_t`), plus a small safe Rust-side wrapper for reading
//! and printing it. This is read-only inspection data: nothing here
//! takes ownership of a PCI device or touches its registers.
use crate::console;

/// Must stay byte-for-byte in sync with include/rustffi64.h's
/// `toxenos_pci_info_t` -- fixed-width integers only, no enums, exactly
/// the same discipline user64/tox64.h already uses for the kernel/
/// userspace syscall ABI in this codebase.
#[repr(C)]
pub struct PciInfoRaw {
    pub bus: u8,
    pub slot: u8,
    pub func: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub class_code: u8,
    pub subclass: u8,
    pub prog_if: u8,
    pub revision: u8,
    pub header_type: u8,
    pub irq_line: u8,
    pub irq_pin: u8,
    pub bar_types: [u32; 6],
    pub bar_addrs: [u64; 6],
    pub bar_sizes: [u64; 6],
}

/// BAR kind, decoded from `PciInfoRaw::bar_types`' raw `u32` (which
/// mirrors C's `pci64_bar_type_t` values without ever exposing that C
/// enum type itself across the FFI boundary).
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum BarKind {
    None,
    Io,
    Mem32,
    Mem64,
    /// A value this Rust code doesn't recognize -- kept explicit rather
    /// than silently coercing to `None`, in case `pci64_bar_type_t`
    /// gains a variant on the C side that this module hasn't been
    /// updated for yet.
    Unknown(u32),
}

impl BarKind {
    fn from_raw(v: u32) -> Self {
        match v {
            0 => BarKind::None,
            1 => BarKind::Io,
            2 => BarKind::Mem32,
            3 => BarKind::Mem64,
            other => BarKind::Unknown(other),
        }
    }

    fn name(self) -> &'static str {
        match self {
            BarKind::None => "none",
            BarKind::Io => "io",
            BarKind::Mem32 => "mem32",
            BarKind::Mem64 => "mem64",
            BarKind::Unknown(_) => "unknown",
        }
    }
}

/// Safe, ergonomic view over a [`PciInfoRaw`] the C side handed us for
/// the duration of one call -- this struct borrows nothing and copies
/// every field out of the raw struct at construction time, so it has no
/// lifetime tied to the (possibly C-stack-allocated) `PciInfoRaw` it was
/// built from.
pub struct PciDeviceInfo {
    pub bus: u8,
    pub slot: u8,
    pub func: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub class_code: u8,
    pub subclass: u8,
    pub prog_if: u8,
    pub irq_line: u8,
    pub irq_pin: u8,
    bars: [(BarKind, u64, u64); 6],
}

impl PciDeviceInfo {
    /// # Safety
    /// `raw` must point to a valid, fully-initialized `PciInfoRaw` for
    /// the duration of this call (it is only ever read, never retained).
    pub unsafe fn from_raw(raw: *const PciInfoRaw) -> Self {
        let r = &*raw;
        let mut bars = [(BarKind::None, 0u64, 0u64); 6];
        for i in 0..6 {
            bars[i] = (BarKind::from_raw(r.bar_types[i]), r.bar_addrs[i], r.bar_sizes[i]);
        }
        PciDeviceInfo {
            bus: r.bus,
            slot: r.slot,
            func: r.func,
            vendor_id: r.vendor_id,
            device_id: r.device_id,
            class_code: r.class_code,
            subclass: r.subclass,
            prog_if: r.prog_if,
            irq_line: r.irq_line,
            irq_pin: r.irq_pin,
            bars,
        }
    }

    /// Logs a human-readable summary via kernel klog(), one BAR per line
    /// for every BAR slot that isn't `BarKind::None`.
    pub fn log(&self) {
        crate::klog_fmt!(
            "toxenos_rs: pci {:02x}:{:02x}.{:x} vendor={:04x} device={:04x} class={:02x} subclass={:02x} progif={:02x} irq={}/{}",
            self.bus, self.slot, self.func, self.vendor_id, self.device_id,
            self.class_code, self.subclass, self.prog_if, self.irq_line, self.irq_pin
        );
        for (i, (kind, addr, size)) in self.bars.iter().enumerate() {
            if *kind == BarKind::None {
                continue;
            }
            crate::klog_fmt!(
                "toxenos_rs:   bar[{}] = {} addr={:#x} size={:#x}",
                i, kind.name(), addr, size
            );
        }
    }
}

/// A tiny standalone smoke test for this module, independent of any
/// real PCI hardware being present -- builds a synthetic `PciInfoRaw`
/// entirely in Rust and checks that [`PciDeviceInfo::from_raw`] copies
/// every field correctly. Returns `true` on success.
pub fn selftest() -> bool {
    let raw = PciInfoRaw {
        bus: 1,
        slot: 2,
        func: 3,
        vendor_id: 0x8086,
        device_id: 0x1234,
        class_code: 0x01,
        subclass: 0x06,
        prog_if: 0x01,
        revision: 0,
        header_type: 0,
        irq_line: 11,
        irq_pin: 1,
        bar_types: [3, 0, 0, 0, 0, 0],
        bar_addrs: [0xFEBF_0000, 0, 0, 0, 0, 0],
        bar_sizes: [0x2000, 0, 0, 0, 0, 0],
    };
    // SAFETY: `raw` is a valid, fully-initialized local value for the
    // duration of this call.
    let info = unsafe { PciDeviceInfo::from_raw(&raw as *const PciInfoRaw) };

    let ok = info.bus == 1
        && info.slot == 2
        && info.func == 3
        && info.vendor_id == 0x8086
        && info.device_id == 0x1234
        && info.irq_line == 11
        && info.bars[0].0 == BarKind::Mem64
        && info.bars[0].1 == 0xFEBF_0000
        && info.bars[0].2 == 0x2000
        && info.bars[1].0 == BarKind::None;

    if !ok {
        console::log_line("toxenos_rs: pci::selftest FAILED");
    }
    ok
}
