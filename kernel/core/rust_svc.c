#include "rust_svc.h"
#include "proc.h"
#include "sched.h"
#include "waitq.h"
#include "spinlock.h"
#include "panic.h"
#include "kprintf.h"
#include "string.h"

#ifdef BHARAT_RUST

/* user/services/hello-rs/src/lib.rs */
extern int32_t hello_rs_handle(uint32_t op, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                               size_t *out_len);

/* Called by the Rust panic handler: a panic there is a bug. */
__attribute__((noreturn)) void rust_svc_panic(void)
{
    panic("the Rust service panicked");
}

/* The mailbox: one request at a time. The caller copies its bytes in and
 * the answer out, so the service never sees the caller's memory (as it
 * will not, once it lives in user mode). */
static struct {
    bool busy;                         /* a caller owns the mailbox */
    bool asked, answered;
    uint32_t op, by;
    size_t in_len, out_cap, out_len;
    int status;
    uint8_t in[RUST_SVC_IO_MAX], out[RUST_SVC_IO_MAX];
} box;
static struct spinlock box_lock = SPINLOCK_INIT(LOCK_CLASS_RUST_SVC);
static struct waitq box_wq = WAITQ_INIT;
static bool running;

static bool has_request(void *arg)
{
    (void)arg;
    return __atomic_load_n(&box.asked, __ATOMIC_ACQUIRE);
}

static int64_t service_main(void *arg)
{
    (void)arg;
    thread_current()->flags |= THREAD_CONTAIN;     /* a crash stops only this thread */
    for (;;) {
        waitq_wait(&box_wq, has_request, NULL);
        /* The mailbox is ours until `answered` is set; no lock is held
         * while the Rust code runs. */
        size_t n = 0;
        int st = hello_rs_handle(box.op, box.in, box.in_len, box.out, box.out_cap, &n);
        box.out_len = st == 0 ? n : 0;
        box.status = st;
        box.by = thread_current()->tid;
        __atomic_store_n(&box.asked, false, __ATOMIC_RELEASE);
        __atomic_store_n(&box.answered, true, __ATOMIC_RELEASE);
    }
    return 0;
}

bool rust_svc_built(void)
{
    return true;
}

bool rust_svc_start(void)
{
    proc_status_t err;
    if (thread_create(process_kernel(), "rust-hello", service_main, NULL, &err) == NULL) {
        kprintf("[rust] cannot start the Rust service: %s\n", proc_status_str(err));
        return false;
    }
    running = true;
    kprintf("[rust] Rust service \"hello\" started (no_std, linked in until user mode).\n");
    return true;
}

int rust_svc_call(uint32_t op, const void *in, size_t in_len, void *out, size_t cap, size_t *out_len,
                  uint32_t *by)
{
    if (!running)
        return RUST_SVC_ERR_TIMEOUT;
    if ((in == NULL && in_len > 0) || in_len > RUST_SVC_IO_MAX || out == NULL || out_len == NULL)
        return RUST_SVC_ERR_BAD_ARGS;
    /* Wait for the mailbox to be free. */
    for (;;) {
        {
            SPIN_GUARD(&box_lock);
            if (!box.busy) {
                box.busy = true;
                break;
            }
        }
        sched_sleep(1);
    }
    box.op = op;
    box.in_len = in_len;
    if (in_len > 0)
        memcpy(box.in, in, in_len);
    box.out_cap = cap < RUST_SVC_IO_MAX ? cap : RUST_SVC_IO_MAX;
    __atomic_store_n(&box.answered, false, __ATOMIC_RELEASE);
    __atomic_store_n(&box.asked, true, __ATOMIC_RELEASE);
    waitq_wake_all(&box_wq);
    int st = RUST_SVC_ERR_TIMEOUT;
    for (unsigned i = 0; i < SCHED_TIMER_HZ; i++) {
        if (__atomic_load_n(&box.answered, __ATOMIC_ACQUIRE)) {
            st = box.status;
            *out_len = box.out_len;
            if (st == RUST_SVC_OK)
                memcpy(out, box.out, box.out_len);
            if (by != NULL)
                *by = box.by;
            break;
        }
        sched_sleep(1);
    }
    if (st == RUST_SVC_ERR_TIMEOUT) {
        /* The service is gone (it crashed and was contained) or stuck: an
         * answer may still land in the mailbox, so it stays taken and
         * later calls fail at once. */
        running = false;
        kprintf("[rust] the Rust service did not answer; it is off until restart\n");
        return st;
    }
    SPIN_GUARD(&box_lock);
    box.busy = false;
    return st;
}

#else  /* built without Rust: the API stays, every call says so */

bool rust_svc_built(void)
{
    return false;
}

bool rust_svc_start(void)
{
    kprintf("[rust] the Rust service is not in this build (no rustc or x86_64-unknown-none target, "
            "or BHARAT_NO_RUST); skipped.\n");
    return false;
}

int rust_svc_call(uint32_t op, const void *in, size_t in_len, void *out, size_t cap, size_t *out_len,
                  uint32_t *by)
{
    (void)op; (void)in; (void)in_len; (void)out; (void)cap; (void)out_len; (void)by;
    return RUST_SVC_ERR_NOT_BUILT;
}

#endif
