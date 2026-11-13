# PROMPT.md: Prompts for building BharatOS with an AI coding agent

Copy the **master prompt** below, fill in the `{{...}}` parts, and paste it
to start a work session. Shorter prompts for common tasks follow. They all
assume the agent can read the repo, run the `build.py` commands and commit.

The current plan lives in **`docs/UPDATES.md`**. It overrides ROADMAP, HLD
and LLD where they disagree.

---

## Next phase prompt (V1)

Paste this in a fresh session to build the next V1 phase:

```text
Read CLAUDE.md, WAY_AHEAD.md and docs/UPDATES.md §7, then build the next
unfinished V1 phase (Phase 10, then 11, then 12) completely. Follow the
CLAUDE.md git workflow: new branch from main, small steps, run
`python build.py test` after each step (and `--smp 1`), test-faults and
unit when the rules say so, commit in batches, then phase doc, UPDATES
status, README, bug journal and decisions, PR and merge. Keep it simple
and quick: V1 apps are kernel threads behind small APIs, files are in
RAM. Every feature gets headless boot self-tests. Do not stop to ask
unless a decision really needs me; report what you built, test results
and anything left for Phase 12.
```

## Master prompt: implement one step of a phase

```text
You are a senior OS engineer working on BharatOS: a from-scratch x86-64
microkernel in C11 (freestanding) and NASM, with user-space services in C
and Rust (no_std), booted by Limine and tested in QEMU. Its core idea:
every resource is a kernel object reached only through a handle, and all
I/O and IPC go through async shared rings.

TASK
Implement Phase {{N}}, step {{S}}: "{{step title from docs/UPDATES.md §7}}".

READ FIRST (in this order, before writing code)
1. CLAUDE.md: commands, layout, code rules, safety rules, how to add tests.
2. docs/UPDATES.md: §7 Phase {{N}} (steps and "done when"), §8 (designs
   this step uses), §10 (decisions; don't contradict them silently).
3. Only where UPDATES §14 points to them: the matching parts of
   docs/ROADMAP.md and docs/design/LLD.md.
4. docs/BUG_JOURNAL.md (if it exists): past bugs in this area.
5. The existing source files you will change, fully, not just snippets.

HOW TO WORK
- First write a short plan: files to add/change, data structures, locking
  (which lock or IRQ_GUARD protects what), and the tests you will add
  (normal path, bad-input path, and limit/exhaustion path). Then implement.
- Prefer the simplest design that meets the step. If the design in
  UPDATES.md is wrong or more complex than needed, say why, use the
  simpler one, and update UPDATES.md (§8 and §10) in the same change.
- Match existing style: snake_case, area prefixes, status-enum / -E_*
  errors, panic() only for kernel bugs, header guards BHARAT_<NAME>_H.
- Safety rules: touch user memory only via copy_from_user/copy_to_user;
  copy ring entries before checking them; reach resources only via
  handle_get() with type and rights; rights only shrink when passed on.
- Zero compiler warnings. Kernel code stays in C; no new dependencies
  beyond those allowed in UPDATES.md.

VERIFY (do not skip; do not claim success without showing output)
- python build.py test → exit 0, every old check still passes.
- If you touched interrupts, memory, stacks, switching or faults:
  python build.py test-faults.
- Run whichever exist and apply: unit, sim, fuzz, test --smp 4, tla.
- If performance is affected: python build.py bench, and update
  docs/benchmarks.md with before/after numbers.
- New boot self-tests go in kernel/tests/ (or a user test program), with
  the new "[ok] ..." line added to REQUIRED in scripts/test_boot.py.
- If a test fails, find the root cause. Never weaken or skip a test.

FINISH
- Follow the Git workflow in CLAUDE.md: work on the phase branch, commit
  in batches with Conventional Commit messages (no phase numbers), keep
  checks fast, and at phase end open a PR to main and merge it.
- If a bug took real effort, add it to docs/BUG_JOURNAL.md:
  symptom → how found → root cause → fix → test that now guards it.
- If this completes the phase: write docs/phases/phase-{{N}}-<name>.md
  (what, why, tests, numbers, known limits), mark the phase done in
  docs/UPDATES.md §7, update README.md and the boot banner.
- Report back: what you built, test and benchmark results, any change
  from the plan and why, and known limits.

WRITING STYLE
Plain words, no jargon, short sentences, in code comments, docs and
anything users see. Explain a technical term the first time it appears.
```

---

## Short prompts

### Plan a phase before coding

```text
Read CLAUDE.md, docs/UPDATES.md (§7 Phase {{N}}, §8, §10, §14) and the
source files the phase will touch. Do not write code. Produce:
(1) the steps in build order, each small enough to build and test alone;
(2) for each step: files touched, locking plan, tests added (normal, bad
input, limits), and benchmarks if relevant;
(3) any place the design in UPDATES.md is wrong, risky or more complex
than needed, with a simpler alternative;
(4) the riskiest step and how to reduce the risk (spike, host-side test,
TLA+ model, simulation).
Keep it under one page, plain words.
```

### Fix a failing test

```text
`python build.py {{test|test-faults|unit|sim|fuzz}}` fails. Output:
{{paste the failing part, plus the seed for sim/fuzz}}.
Find the root cause before changing anything: reproduce it (same seed),
read the failing test, the code it exercises, docs/BUG_JOURNAL.md and
recent commits (git log -p -5). Explain the cause in 2–3 sentences, then
fix it at the source. Do not weaken the test. Add a test that would have
caught it earlier if none did. Re-run everything relevant and show the
result lines. Add a BUG_JOURNAL.md entry if it was non-trivial.
```

### Review a finished step

```text
Review the last commit(s) for Phase {{N}} against CLAUDE.md and
docs/UPDATES.md §8 and §10. Look for: user pointers or ring entries used
without copying/validation; resources reached without handle_get() or with
rights growing; shared data changed without its lock or IRQ_GUARD();
possible lock-order inversions; unchecked return values; fixed-size tables
without limit checks; missing bad-input or exhaustion tests; anything more
complicated than needed. List only real problems, most serious first, each
with file:line and a concrete fix.
```

### Write a TLA+ spec for a protocol

```text
Write a TLA+ spec in specs/{{name}}.tla for {{protocol: e.g. the BharatFS
commit in docs/UPDATES.md §7 Phase 14 / old LLD Phase 9, or the ring
head/tail protocol in §8.2}}. Model only what matters for correctness:
state, each step as an action, crashes or reordering where real.
State the safety property (e.g. "after any crash, the mounted root is a
complete earlier commit") and any liveness property. Add a .cfg with small
bounds, run TLC via `python build.py tla`, and report states explored and
the result. If TLC finds a bug, explain the trace in plain words and
propose the design fix before changing code.
```

### Add a simulation test

```text
Add a host-side simulation test under sim/ for {{subsystem}}. Build the
real subsystem code on the PC against fakes (disk, clock, network) that
can inject {{failures: lost/reordered writes, bit flips, power cut at any
step, packet loss, partitions}}, driven by a seed. Each run checks the
invariants: {{list}}. A failure must print the seed and a minimal event
log, so `python build.py sim --seed S` reproduces it exactly. Show results
of a 10,000-run batch.
```

### Add or update a benchmark

```text
Add benchmark "{{name}}" to `python build.py bench`: measure with TSC
cycles inside BharatOS (warm-up, then N runs; report median and p99).
Where a fair comparison exists, give the Linux command that measures the
same thing on the same QEMU settings. Update docs/benchmarks.md with the
numbers, machine/QEMU settings, and one line explaining any big
difference.
```

### Write the phase doc

```text
Phase {{N}} is complete. Write docs/phases/phase-{{N}}-<name>.md in the
style of docs/phases/phase-5-scheduler.md: what was built, files, key data
structures, how it works step by step, how it's tested (counts from the
latest test, unit, sim and fuzz runs), benchmark numbers, and honest known
limits. Plain words; explain any technical term the first time. Then mark
the phase done in docs/UPDATES.md §7 and update README.md.
```

### Prepare a demo

```text
Write a one-minute demo script for Phase {{N}} from its "Demo" line in
docs/UPDATES.md §7 (or the 3-minute video plan in §11.4): exact commands,
what the audience sees after each, and one sentence per step linking it to
a user problem (USPs, §4.1) or an engineering point (§4.2). Run every
command to check it works.
```

### Interview prep for a feature

```text
Using the code, tests, docs/benchmarks.md and docs/BUG_JOURNAL.md, write a
2-minute explanation of {{feature}} using the structure in
docs/UPDATES.md §11.2: problem → simple design → hard part → proof (test or
number) → what I'd do differently. Then list 5 likely interviewer
follow-up questions with short, honest answers, including tradeoffs versus
how Linux does it.
```

---

## Why these prompts are shaped this way

- **One source of truth:** every prompt points to `docs/UPDATES.md`, so
  agents follow the current plan, not the older one.
- **Read-first list with an order:** rules, then goal, then exact design,
  then past bugs, then real code. Fewer wrong guesses.
- **Plan first, with locking and three kinds of tests named up front:**
  bad-input and exhaustion tests get designed, not bolted on; locking is
  thought through before multi-core bugs appear.
- **Simplest design, with permission to deviate and record it:** stops
  over-engineering, lets the agent fix a bad spec, keeps the decision log
  honest.
- **Evidence-based verification:** test output, seeds and benchmark
  numbers, never "it should work".
- **Bug journal and benchmarks built as you go:** the interview material
  writes itself.
- **One step per session:** small diffs are easy to review and roll back.
