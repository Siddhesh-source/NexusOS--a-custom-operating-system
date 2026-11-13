#ifndef BHARAT_BENCH_H
#define BHARAT_BENCH_H

/* Micro-benchmarks (context switch, heap, trace), printed as
 * "[bench] <name>: <cycles> cycles/op". Only the bench kernel
 * (`python build.py bench`, BHARAT_BENCH) runs them; in normal builds
 * bench_run() does nothing. */

void bench_run(void);

#endif /* BHARAT_BENCH_H */
