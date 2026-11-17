//! hello-rs: the first BharatOS service written in Rust (decision D4).
//!
//! A `no_std` library with one entry point in the C calling convention,
//! `hello_rs_handle`: a request (an operation number and some bytes in)
//! and a reply (some bytes out). For now the kernel links it in and runs
//! it on its own kernel thread behind a small mailbox (kernel/core/
//! rust_svc.c); when user mode lands (Phase 13) the same code moves into a
//! user-mode process that answers over a ring, so nothing here touches
//! kernel memory except the two buffers it is handed.
//!
//! Operations:
//!   1  GREET     out = "Namaste from Rust" (no input)
//!   2  CRC32     out = the CRC-32 (IEEE) of the input, 4 bytes, little endian
//!
//! Every pointer and length is checked before use; a bad request gets a
//! negative error code, never a crash.

#![no_std]
#![forbid(unsafe_op_in_unsafe_fn)]

use core::panic::PanicInfo;

pub const OP_GREET: u32 = 1;
pub const OP_CRC32: u32 = 2;

pub const ERR_BAD_OP: i32 = -1;
pub const ERR_BAD_ARGS: i32 = -2;
pub const ERR_TOO_SMALL: i32 = -3;

/// Largest input accepted, so a wrong length cannot make us read far.
pub const IN_MAX: usize = 64 * 1024;

const GREETING: &[u8] = b"Namaste from Rust";

extern "C" {
    /// A Rust panic is a bug in the service: the kernel reports it.
    fn rust_svc_panic() -> !;
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // SAFETY: rust_svc_panic takes no arguments and never returns.
    unsafe { rust_svc_panic() }
}

/// CRC-32 (IEEE 802.3, the one zip and Ethernet use), bit by bit: small,
/// no table, fast enough for a service that checks a few kilobytes.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    !crc
}

/// Work out the reply to one request into `out`; the number of bytes
/// written, or an error code.
fn handle(op: u32, input: &[u8], out: &mut [u8]) -> Result<usize, i32> {
    let reply: [u8; 4];
    let bytes: &[u8] = match op {
        OP_GREET => GREETING,
        OP_CRC32 => {
            reply = crc32(input).to_le_bytes();
            &reply
        }
        _ => return Err(ERR_BAD_OP),
    };
    let dst = out.get_mut(..bytes.len()).ok_or(ERR_TOO_SMALL)?;
    dst.copy_from_slice(bytes);
    Ok(bytes.len())
}

/// The C entry point. Returns 0 and sets `*out_len`, or a negative error.
///
/// # Safety
/// `input` must point to `in_len` readable bytes (or be null with
/// `in_len` 0), `out` to `out_cap` writable bytes, and `out_len` to a
/// writable `usize`; the buffers must not overlap.
#[no_mangle]
pub unsafe extern "C" fn hello_rs_handle(op: u32, input: *const u8, in_len: usize, out: *mut u8,
                                         out_cap: usize, out_len: *mut usize) -> i32 {
    if out.is_null() || out_len.is_null() || (input.is_null() && in_len != 0) || in_len > IN_MAX
        || out_cap > IN_MAX {
        return ERR_BAD_ARGS;
    }
    // SAFETY: checked non-null above; the caller promises the sizes.
    let input: &[u8] = if in_len == 0 { &[] } else { unsafe { core::slice::from_raw_parts(input, in_len) } };
    // SAFETY: as above.
    let out = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
    match handle(op, input, out) {
        Ok(n) => {
            // SAFETY: checked non-null above.
            unsafe { *out_len = n };
            0
        }
        Err(e) => e,
    }
}
