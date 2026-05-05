# nanovisor

A minimal, educational ARM64 hypervisor for Apple Silicon, built on macOS
Hypervisor.framework. The whole thing is under 100 lines of C++ — small
enough to hold in your head, large enough to demonstrate the entire VM
lifecycle.

## What it does

Creates a virtual machine, maps one page of "guest RAM", writes three
ARM64 instructions into it, runs a virtual CPU until the guest traps back
to the host, then prints the value of register X0.

The guest program:

```asm
mov  x0, #42       ; X0 = 42
add  x0, x0, #1    ; X0 = 43
hvc  #0            ; trap to the hypervisor
```

Expected output:

```
guest exited via HVC, x0 = 43
```

## Why "educational"?

Real hypervisors (KVM, QEMU's HVF backend, Xen) handle hundreds of exit
types, MMU configuration, device emulation, interrupt routing, and
multi-vCPU scheduling. This one demonstrates the irreducible core of
the model:

1. **Memory virtualization** via stage-2 mappings (`hv_vm_map`)
2. **CPU virtualization** via a vCPU object that runs guest code natively
3. **VM exits** via the ARM `HVC` instruction and ESR_EL2 syndrome decoding
4. **The host/guest boundary** — what the host controls vs. what the guest sees

Once these click, the rest of any hypervisor codebase is just more of the
same, plus device emulation.

## Prerequisites

- Apple Silicon Mac (M1 or later)
- Xcode Command Line Tools: `xcode-select --install`

## Build & run

```sh
make run
```

The Makefile compiles `hyp.cpp` and ad-hoc codesigns the binary with the
`com.apple.security.hypervisor` entitlement. Without that entitlement,
`hv_vm_create` returns `HV_DENIED`.

## Code tour

| File | What's in it |
|---|---|
| `hyp.cpp` | The whole hypervisor. Heavily commented; read top-to-bottom. |
| `hyp.entitlements` | Grants `com.apple.security.hypervisor`. Required by macOS. |
| `Makefile` | Compile + ad-hoc codesign. |

The 8 numbered steps inside `hyp.cpp` map 1:1 to the lifecycle of any HVF
program:

1. `hv_vm_create` — ask macOS to allow this process to run guests
2. `mmap` — allocate page-aligned host memory to back the guest's RAM
3. Write guest instructions into that memory
4. `hv_vm_map` — install stage-2 page tables so the guest can see them
5. `hv_vcpu_create` — make a virtual CPU
6. `hv_vcpu_set_reg` — set PC and PSTATE
7. `hv_vcpu_run` — run until the guest traps; loop until done
8. `hv_vcpu_destroy` / `hv_vm_unmap` / `hv_vm_destroy` — tear down

## Background: ARM64 exception levels

On ARMv8 the CPU runs at one of four privilege levels:

| Level | Typical occupant |
|---|---|
| EL0 | userspace processes |
| EL1 | kernel |
| EL2 | hypervisor |
| EL3 | secure monitor (firmware/boot) |

macOS itself runs at EL2 and exposes Hypervisor.framework as the user-mode
control surface for guest VMs. When you call `hv_vcpu_run`, macOS programs
the EL2 control registers, drops the CPU into EL1, and lets the guest
execute natively until something (a hypercall, an MMIO access, a timer
interrupt, an undefined instruction) traps back into EL2.

This makes macOS + Hypervisor.framework a **Type-2 hypervisor**: a host
OS running normal apps alongside a thin virtualization layer it exposes
to userspace. KVM on Linux and WHPX on Windows are the same idea. Xen
and ESXi, by contrast, are Type-1 — they boot directly on hardware with
no host OS underneath.

## Suggested extensions

If you want to keep learning by extending this:

1. **Toy MMIO**: map a separate "device" page, have the guest store a byte
   to it, catch the data abort, print the value. Now you have a primitive
   `printf` for the guest.
2. **Turn on the MMU**: set `SCTLR_EL1.M=1`, install a single L1 page table,
   translate a guest virtual address.
3. **Load a flat binary**: read bytes from disk into guest memory instead
   of hardcoding the program.
4. **Handle the timer**: enable the virtual timer, catch
   `HV_EXIT_REASON_VTIMER_ACTIVATED`, inject a virtual IRQ into the guest.
5. **Two vCPUs**: run two threads, each with its own vCPU, sharing guest
   memory. Now you have an SMP guest.

Each is a small, self-contained extension to the file you already have.

## Further reading

- [ARM Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest)
  — especially the chapters on Exception Levels and AArch64 Virtualization.
- [Hypervisor.framework documentation](https://developer.apple.com/documentation/hypervisor)
- The Linux KVM userspace API documentation (`Documentation/virt/kvm/api.rst`
  in the kernel tree) — the conceptual ancestor of HVF, with much more
  written about it.

## License

[MIT](LICENSE) — Copyright (c) 2026 Maruf Zaber
