# nanovisor

A minimal, educational **Linux-syscall emulator** for Apple Silicon — the
gVisor model, but on macOS. A tiny aarch64 program runs inside a VM, and
when it issues a syscall the host process services it in userspace
instead of forwarding to a real kernel. The whole thing is one file of
C++, small enough to hold in your head.

## What it does

The guest program (hand-encoded aarch64 in `hyp.cpp`):

```asm
movz x0, #1            ; fd = 1 (stdout)
movz x1, #0x100        ; buf = guest address of "hi\n"
movz x2, #3            ; len = 3
movz x8, #64           ; SYS_write
hvc  #0                ; trap to host
movz x0, #0            ; status = 0
movz x8, #93           ; SYS_exit
hvc  #0
```

When `hvc #0` traps the host, we read X8 for the syscall number, X0–X5
for arguments, service the call (real `write(2)` for SYS_write,
loop-exit for SYS_exit), write the return value back to X0, and resume.

Expected output:

```
hi
```

…and a host exit code of 0, propagated from the guest's `exit(0)`.

## Why "gVisor-shaped"?

[gVisor](https://gvisor.dev) is a Google sandbox: it runs Linux
applications by pairing them with a userspace kernel that intercepts
their syscalls. The application thinks it's talking to Linux; really
it's talking to gVisor's `Sentry` process. nanovisor is the same idea
on macOS, using Hypervisor.framework as the syscall-trap mechanism.

The dispatch loop in `hyp.cpp` is the actual irreducible core of any
gVisor-style userspace kernel:

1. Run the guest natively.
2. Catch every trap.
3. Decode it as a Linux syscall (number in X8, args in X0–X5).
4. Service it on the host.
5. Write the return value back to X0.
6. Resume.

Adding more syscalls is just more `case` arms.

## Honest limitations (a.k.a. the roadmap)

This is **commit 1** of the pivot from "minimal hypervisor demo" to
"gVisor-on-macOS." Two corners are deliberately cut:

- **Trap instruction.** A real Linux/aarch64 program issues syscalls
  with `svc #0`. Routing svc to the host requires either an EL1 vector
  table that forwards via `hvc`, or `HCR_EL2.TGE`. For now the guest
  runs at EL1 and uses `hvc #0` directly — same Linux ABI in the
  registers, but a non-Linux trap instruction.
- **Memory translation.** The guest runs with the MMU off, so
  guest virtual = guest physical, mapped 1:1 to a single host page.
  No page-table walks yet.

What's planned, in order:

1. **Real EL0/EL1 split.** Add a 16-entry EL1 vector table whose
   entries forward to the host via `hvc`, plus a boot stub that
   `eret`s into EL0. Switch the guest to `svc #0`. Now we have actual
   privilege separation between the guest and our userspace kernel.
2. **More syscalls.** `read`, `openat`, `close`, `mmap`, `brk`,
   `clock_gettime`, etc. Enough to run a static "hello, world" built
   with musl.
3. **ELF loader.** Read a static-pie aarch64 ELF off disk, set up
   guest memory, build the auxv/argv/envp on the guest stack.
4. **Multiple pages, MMU on.** A real address space with code/data/heap
   regions and proper translation.

## Prerequisites

- Apple Silicon Mac (M1 or later)
- Xcode Command Line Tools: `xcode-select --install`

## Build & run

```sh
make run
```

The Makefile compiles `hyp.cpp` and ad-hoc codesigns the binary with
the `com.apple.security.hypervisor` entitlement. Without that
entitlement, `hv_vm_create` returns `HV_DENIED`.

## Code tour

| File | What's in it |
|---|---|
| `hyp.cpp` | The whole emulator. Heavily commented; read top-to-bottom. |
| `hyp.entitlements` | Grants `com.apple.security.hypervisor`. Required by macOS. |
| `Makefile` | Compile + ad-hoc codesign. |

The 8 numbered steps inside `hyp.cpp` map 1:1 to the lifecycle:

1. `hv_vm_create` — ask macOS to allow this process to run guests
2. `mmap` — allocate page-aligned host memory to back the guest's RAM
3. Lay out guest code and data (the program above, plus `"hi\n"`)
4. `hv_vm_map` — install stage-2 page tables so the guest can see them
5. `hv_vcpu_create` — make a virtual CPU
6. `hv_vcpu_set_reg` — set PC and PSTATE
7. The syscall dispatch loop
8. `hv_vcpu_destroy` / `hv_vm_unmap` / `hv_vm_destroy` — tear down

## Background: ARM64 exception levels

On ARMv8 the CPU runs at one of four privilege levels:

| Level | Typical occupant |
|---|---|
| EL0 | userspace processes |
| EL1 | kernel |
| EL2 | hypervisor |
| EL3 | secure monitor (firmware/boot) |

macOS itself runs at EL2 and exposes Hypervisor.framework as the
user-mode control surface for guest VMs. When you call `hv_vcpu_run`,
macOS programs the EL2 control registers, drops the CPU into EL1, and
lets the guest execute natively until something traps back into EL2.

In a gVisor-style design you ideally want the guest at EL0 (so it has
no privilege to disable interrupts, install vectors, or touch system
registers) with a thin EL1 trampoline that forwards every exception
to the userspace kernel. Commit 1 keeps the guest at EL1 for
simplicity; commit 2 introduces the real EL0/EL1 split.

## Further reading

- [gVisor design docs](https://gvisor.dev/docs/architecture_guide/) —
  particularly the "Platform" doc that covers ptrace vs. KVM as
  syscall-trap mechanisms. nanovisor's Hypervisor.framework path is
  closest to gVisor's KVM platform.
- [ARM Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest)
  — chapters on Exception Levels and AArch64 Virtualization.
- [Hypervisor.framework documentation](https://developer.apple.com/documentation/hypervisor)
- Linux kernel `include/uapi/asm-generic/unistd.h` — the canonical
  syscall-number table we're emulating.

## License

[MIT](LICENSE) — Copyright (c) 2026 Maruf Zaber
