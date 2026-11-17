#ifndef BHARAT_RUST_SVC_H
#define BHARAT_RUST_SVC_H

/* The Rust "hello" service (user/services/hello-rs), run by the kernel.
 *
 * Decision D4 puts new services in Rust, in user mode. User mode does not
 * exist yet, so for V1 the Rust code is linked into the kernel (when
 * rustc and its x86_64-unknown-none target are installed; build.py skips
 * it otherwise) and runs on its own kernel thread, "rust-hello". Callers
 * never call the Rust code directly: they post one request at a time into
 * a mailbox and wait for the answer, the same shape a ring will have in
 * Phase 13, when the service moves to user mode (decision D43).
 *
 * Limits: one request at a time (others wait their turn); requests and
 * answers are at most RUST_SVC_IO_MAX bytes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RUST_SVC_GREET   1     /* answer: "Namaste from Rust" */
#define RUST_SVC_CRC32   2     /* answer: CRC-32 of the request, 4 bytes */

#define RUST_SVC_IO_MAX  4096

enum {
    RUST_SVC_OK = 0,
    RUST_SVC_ERR_BAD_OP = -1,      /* from the Rust code */
    RUST_SVC_ERR_BAD_ARGS = -2,
    RUST_SVC_ERR_TOO_SMALL = -3,
    RUST_SVC_ERR_NOT_BUILT = -10,  /* this kernel was built without Rust */
    RUST_SVC_ERR_TIMEOUT = -11,    /* the service did not answer */
};

/* Start the service thread. False (with a log line saying why) if the
 * kernel was built without Rust or the thread cannot start. */
bool rust_svc_start(void);
bool rust_svc_built(void);

/* Send one request and wait for the answer (up to a second). Returns
 * RUST_SVC_OK and the answer's length in *out_len, or an error. The tid of
 * the thread that answered goes to *by (NULL: not wanted). */
int rust_svc_call(uint32_t op, const void *in, size_t in_len, void *out, size_t cap, size_t *out_len,
                  uint32_t *by);

#endif /* BHARAT_RUST_SVC_H */
