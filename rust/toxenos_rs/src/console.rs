//! Thin, safe wrapper around kernel/klog.c's `klog`/`klog_hex`. This is
//! the ONLY console output Rust kernel code has this milestone -- no
//! attempt is made to route through kernel/console64.c's VGA/framebuffer
//! path separately, since klog() already mirrors to it on the C side for
//! every existing caller.
use crate::ffi;
use core::fmt::{self, Write};

/// A zero-sized `core::fmt::Write` sink that copies formatted output into
/// a fixed-size stack buffer and flushes it to `klog()` a NUL-terminated
/// chunk at a time. Kept deliberately simple (no dynamic allocation --
/// this must remain usable from the panic handler, where allocating
/// could itself be the reason we're panicking) and deliberately small
/// (128 bytes): kernel log lines here are short diagnostic messages, not
/// arbitrary text.
pub struct KlogWriter {
    buf: [u8; 128],
    len: usize,
}

impl KlogWriter {
    pub const fn new() -> Self {
        KlogWriter { buf: [0; 128], len: 0 }
    }

    fn flush(&mut self) {
        if self.len == 0 {
            return;
        }
        self.buf[self.len.min(self.buf.len() - 1)] = 0;
        // SAFETY: `buf` is a local, NUL-terminated, valid-for-`len`+1
        // byte array; klog() only reads it (never retains the pointer
        // past the call, matching its existing C callers' usage).
        unsafe { ffi::klog(self.buf.as_ptr()) };
        self.len = 0;
    }
}

impl Write for KlogWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for &b in s.as_bytes() {
            if self.len >= self.buf.len() - 1 {
                self.flush();
            }
            self.buf[self.len] = b;
            self.len += 1;
        }
        self.flush();
        Ok(())
    }
}

/// Logs one line (a trailing `\n` is added) via klog(). Safe: builds a
/// bounded, NUL-terminated buffer before ever touching the FFI boundary.
pub fn log_line(s: &str) {
    let mut w = KlogWriter::new();
    let _ = w.write_str(s);
    let _ = w.write_str("\n");
}

/// Logs `label` followed by `val` in klog_hex()'s own "0xXXXXXXXX" format.
/// `label` is copied into a small stack buffer first so an arbitrary
/// `&str` (not guaranteed NUL-terminated) can still cross the FFI
/// boundary safely.
pub fn log_hex(label: &str, val: u32) {
    let mut buf = [0u8; 64];
    let n = label.len().min(buf.len() - 1);
    buf[..n].copy_from_slice(&label.as_bytes()[..n]);
    buf[n] = 0;
    // SAFETY: `buf` is NUL-terminated and valid for the duration of this call.
    unsafe { ffi::klog_hex(buf.as_ptr(), val) };
}

/// `println!`-style formatted logging, e.g. `klog_fmt!("value={}", x);`.
#[macro_export]
macro_rules! klog_fmt {
    ($($arg:tt)*) => {{
        let mut w = $crate::console::KlogWriter::new();
        let _ = core::fmt::write(&mut w, core::format_args!($($arg)*));
        let _ = core::fmt::Write::write_str(&mut w, "\n");
    }};
}
