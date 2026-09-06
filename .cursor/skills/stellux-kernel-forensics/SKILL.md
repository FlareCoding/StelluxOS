---
name: stellux-kernel-forensics
description: Diagnose intermittent StelluxOS kernel failures with evidence instead of guesses, including panics, memory corruption, use-after-free, double frees, list corruption, SMP races, and stale TLB translations. Use when a kernel panic or heap fatal is reported, when a failure reproduces only sometimes, when a hypothesis needs a discriminating QEMU experiment, or when a crash must be captured with every CPU frozen for inspection.
---

# StelluxOS Kernel Forensics

Method for bugs that do not reproduce on demand. Boot and GUI mechanics live in
the stellux-qemu-testing skill, command cookbooks and layout facts live in
[reference.md](reference.md). The scripts under `scripts/` are the harness,
run them rather than rewriting them.

## Principles

- Decode the crash completely before touching QEMU. A panic dump plus a
  disassembly of the faulting instruction usually names the exact field and
  the exact operation, which is most of the diagnosis.
- Trace the object's ownership end to end in the source before forming a
  hypothesis: every alloc, free, enqueue, dequeue, handoff, and what the
  allocator does to freed bytes. Corruption fingerprints are decoded by
  knowing what freed and fresh memory look like.
- State each hypothesis as a prediction that a cheap experiment can falsify,
  then run that experiment. Prefer experiments that change one variable.
- Keep "consistent with" and "proven" apart in every report. When a
  prediction fails, say so and find the discriminator, never explain it away.
- Never collide with the owner's QEMU: `pgrep -fl qemu-system` first, and use
  `-snapshot` so instances never fight over the image write lock.

## Workflow

Copy this checklist and track progress:

```
- [ ] 0 Preserve: save the serial log, note tree state and image build time
- [ ] 1 Decode: CR2/FAR offset -> field, error code, faulting instruction
- [ ] 2 Trace ownership in the source, list every hypothesis
- [ ] 3 Reproduce: stress.sh baseline rate at the failing configuration
- [ ] 4 Discriminate: smp=1, other arch, patched kernel via inject_kernel.sh
- [ ] 5 Capture: catch_frozen.sh, compare CPU view against page-walk view
- [ ] 6 Confirm: the smallest kernel change that removes the failure
- [ ] 7 Report: mechanism, evidence table, what it is not, fix options
```

**0 Preserve.** Ask for the raw serial log or run headless with `| tee`.
Record `git log -1`, `git status`, and the image timestamp against the commit,
since an image older than the tree explains many ghosts. One-off messages such
as a heap fatal seen once are evidence too, write them down verbatim.

**1 Decode.** Map the faulting address to a struct field with `ptype /o`,
name the instruction with `llvm-objdump`, and read only the registers that are
operands of that instruction (reference.md, "Decoding a panic dump"). Write one
sentence of the form "the CPU read X at offset Y of object Z and it was W".

**2 Trace ownership.** For the object type in that sentence, enumerate each
site that allocates, frees, links, unlinks, or hands it across a boundary, and
check the allocator's free-path side effects on the bytes you saw. Then list
every hypothesis that produces exactly the observed fingerprint, including
the ones outside the subsystem that crashed: allocator, paging, scheduler.

**3 Reproduce.** Establish the failure rate before changing anything:

```bash
.cursor/skills/stellux-kernel-forensics/scripts/stress.sh \
    --smp 4 --runs 16 --parallel 4 --cmd 'ping 10.0.2.2 20' \
    --done 'packets transmitted' --fail 'KERNEL PANIC|\[FATAL\]' --count 'bytes from' --tag base
```

Each run boots a fresh headless instance, types the command at the serial
prompt, and classifies the run. Read the per-run lines, then the
`failing=N clean=M` summary. A `booted=0` run is a harness problem, fix it
before trusting any number. Rates drift with host load (the same bug went from
44 to 25 percent between two sessions), so a clean comparison batch means
something only next to a baseline that reproduced in the same session.

**4 Discriminate.** Pick the experiment whose outcome the hypotheses disagree on:

| Question | Experiment | Reading |
|---|---|---|
| Cross-CPU at all? | same batch with `--smp 1` | a lock-protected structure cannot be corrupted by one CPU, stale TLBs need two |
| Architecture mechanism? | same batch with `--arch aarch64` | aarch64 TLB invalidation is a broadcast, x86 `invlpg` is local |
| A specific mechanism? | one-line kernel change via `inject_kernel.sh`, same batch | changes nothing else about timing or allocation order |
| Timing dependent? | vary `--parallel` and host load | rate moves with load for races, not for logic bugs |

Run the comparison batch with the same command, run count, and session as the
baseline. See reference.md, "Reading run statistics", for what counts as evidence.

**5 Capture.** Freeze the whole machine at the fault and compare views:

```bash
.cursor/skills/stellux-kernel-forensics/scripts/catch_frozen.sh \
    --smp 4 --cmd 'ping 10.0.2.2 30' --dump-ram --tag c1 --gdb-port 4701
```

Run several in parallel with distinct `--tag` and `--gdb-port` until one
reports `frozen=1`. Attaching gdb seems to lower the reproduction rate, so
expect more rounds than the stress baseline suggests. The evidence directory
holds `gdb.txt` with per-CPU backtraces, control registers, the trap frame, a
symbolized stack walk of the faulting context, page-walk reads and `gva2gpa` of
every kernel address in the fault registers, `ram_*.bin` if requested, and
`serial.log` including the kernel's own panic report, which is printed only
after the capture. gdb reads through the page tables and never through the TLB, so a value the
CPU reported in the trap frame that differs from what gdb reads at the same
address proves the CPU used a different translation. Search dumps with
`scan_ram.py` (page conditions or byte-string find with a page-offset
histogram). Always pass `--kernel` when the image holds an injected kernel.

**6 Confirm.** The strongest evidence is the smallest change that makes the
rate go to zero at the failing configuration, built with `inject_kernel.sh`
into a scratch export so the tree stays untouched. Match the clean sample to at
least twice the baseline batch. A confirmation patch is not the fix, the fix is
designed afterwards at the layer that owns the invariant.

**7 Report.** State the mechanism as a sequence of events with `file:line`
evidence, give the run table (configuration, boots, failures), list what the
bug is not and why, and separate fix options by how completely they restore the
invariant. Leave the scratch work in `/tmp`, and leave `git status` clean.

## Scripts

All scripts take `--help`-style headers at the top of the file, run from the
repo root or set `STLX_ROOT`, and default to `images/stellux-<arch>.img`.

- `stress.sh`: parallel boots, one command each, per-run classification and a
  summary. `--expect-clean` makes it a gate that exits nonzero on any failure.
- `catch_frozen.sh`: one instance with gdb armed on `panic::on_trap`, freezes
  all CPUs at a panic, collects evidence, `--dump-ram`, `--gdb-extra`, `--keep`.
- `inject_kernel.sh`: `git archive` scratch export, kernel-only build, inject
  into a copy of the image. Prints the ELF to use for symbols.
- `scan_ram.py`: page-condition search or byte-string find over RAM dumps.
  Negative offsets need the `--show=-8:8` form.
- `frozen_evidence.py`: sourced by gdb inside the frozen session, the stack
  walk and address dumps. Extend it when a bug needs other registers.
- `qemu_lib.sh`: shared firmware discovery, QEMU start, prompt waits, HMP helper.

## Pitfalls that cost a batch each

- A second QEMU on the same image fails with `Failed to get "write" lock`.
  The scripts pass `-snapshot`, do the same in any hand-written command.
- After a panic only the faulting CPU halts. The others keep running for as
  long as it takes you to attach, and they recycle the very frames you wanted
  to inspect. Freeze with a breakpoint, never attach after the fact.
- `pmemsave` paths must be double quoted in HMP, see reference.md.
- gdb exiting detaches and resumes the VM. Everything, including `monitor`
  commands, belongs inside the one frozen session.
- Symbols must come from the ELF that is in the image. After `inject_kernel.sh`
  that is the scratch build's ELF, not `build/kernel/<arch>/kernel.elf`.
- macOS bash 3.2 rejects `"${ARR[@]}"` on an empty array under `set -u`, write
  `${ARR[@]+"${ARR[@]}"}`.
- The serial shell echoes keystrokes with `[2K` redraw sequences, filter with
  `grep -v 2K` and never count prompts to measure anything.
- A registers dump is not a narrative. `rsi` held the same stale value across
  every crash of one bug and meant nothing.
- Heap addresses are deterministic across boots of the same image, so a
  pointer from one capture, such as a list sentinel, usually holds in the next.
