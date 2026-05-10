// Copyright (c) 2026 Maruf Zaber
// SPDX-License-Identifier: MIT
//
// =============================================================================
//  hyp.cpp — Minimal Linux-syscall emulator on macOS (gVisor-shaped)
// =============================================================================
//
//  WHAT THIS PROGRAM DOES
//  ----------------------
//  Runs a tiny Linux/aarch64 program inside a VM, but instead of giving that
//  program a real Linux kernel, we emulate Linux syscalls in this host
//  process. The model is the same as Google's gVisor: an unprivileged "guest"
//  app is paired with a userspace kernel that implements the syscall ABI.
//
//  The guest program here is hand-encoded aarch64 that does:
//
//      write(1, "hi\n", 3);
//      exit(0);
//
//  When the guest issues a syscall, the CPU traps back to us. We decode the
//  Linux syscall convention (number in X8, args in X0–X5, return in X0),
//  service it on the host, and resume the guest. The expected output is
//  literally:
//
//      hi
//
//  (and a host exit code of 0, propagated from the guest's exit(0)).
//
//  ROADMAP
//  -------
//  This is **commit 1** of the pivot from "minimal hypervisor demo" to
//  "gVisor-on-macOS." Two simplifications are deliberately left for later:
//
//    1. Trap instruction. A real Linux/aarch64 program issues syscalls with
//       `svc #0`. Routing svc to the host requires either an EL1 vector
//       table that forwards via `hvc`, or HCR_EL2.TGE. For now the guest
//       runs at EL1 and uses `hvc #0` directly — same Linux ABI in the
//       registers, but a non-Linux trap instruction. Commit 2 adds a real
//       EL0/EL1 split and switches the guest to `svc`.
//
//    2. Memory translation. The guest runs with the MMU off, so its
//       "virtual" addresses are guest-physical addresses, which we map 1:1
//       to a single host page. No page-table walk is needed yet.
//
//  Even with those two simplifications, the dispatch loop you see below is
//  the actual heart of a gVisor-style userspace kernel: trap, decode, look
//  up syscall, execute on the host, return value to the guest, resume.
//  Adding more syscalls is just more `case` arms.
//
//  PREREQUISITES
//  -------------
//    * Apple Silicon Mac (M1 or later).
//    * Xcode Command Line Tools: `xcode-select --install`.
//    * Codesigning with the entitlement com.apple.security.hypervisor.
//      The Makefile handles this automatically.
// =============================================================================

#include <Hypervisor/Hypervisor.h>   // hv_vm_*, hv_vcpu_*, HV_REG_*
#include <sys/mman.h>                // mmap, munmap
#include <unistd.h>                  // ::write
#include <cerrno>                    // errno, EFAULT, ENOSYS
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// CHECK aborts the program if a Hypervisor.framework call fails. Real
// hypervisors handle errors more gracefully, but for an educational example
// "die loudly with the failed call shown" is the right call.
#define CHECK(call) do {                                              \
    hv_return_t _r = (call);                                          \
    if (_r != HV_SUCCESS) {                                           \
        std::fprintf(stderr, #call " failed: 0x%x\n", _r);            \
        std::exit(1);                                                 \
    }                                                                 \
} while (0)

// Linux/aarch64 syscall numbers we implement. The full table lives in the
// Linux kernel at include/uapi/asm-generic/unistd.h.
constexpr uint64_t SYS_write = 64;
constexpr uint64_t SYS_exit  = 93;

int main() {
    // -------------------------------------------------------------------------
    //  STEP 1 / Create the VM.
    //
    //  Hypervisor.framework allows ONE VM per process. hv_vm_create asks
    //  macOS to allocate the per-VM control structures and to enable EL2
    //  virtualization for this process. From here on, the process is a
    //  hypervisor — even though our intended use is "userspace kernel."
    // -------------------------------------------------------------------------
    CHECK(hv_vm_create(nullptr));

    // -------------------------------------------------------------------------
    //  STEP 2 / Allocate one host page that will back the guest's RAM.
    //
    //  16 KiB is the page size on Apple Silicon. The guest sees this single
    //  page at guest physical address 0; that's both its code and its data.
    // -------------------------------------------------------------------------
    constexpr size_t   kPageSize = 0x4000;          // 16 KiB
    constexpr uint64_t kGuestPA  = 0x0;             // guest physical base
    void* host_mem = mmap(nullptr, kPageSize,
                          PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (host_mem == MAP_FAILED) { std::perror("mmap"); return 1; }

    // -------------------------------------------------------------------------
    //  STEP 3 / Lay out the guest program and its data.
    //
    //  We place hand-encoded aarch64 instructions at offset 0, and the
    //  string "hi\n" at offset 0x100. The instructions follow the Linux
    //  aarch64 syscall ABI:
    //
    //      X8         = syscall number
    //      X0..X5     = arguments
    //      svc/hvc #0 = make the call
    //      X0         = return value (after the call)
    //
    //  Disassembly:
    //
    //      movz x0, #1         ; fd = 1 (stdout)
    //      movz x1, #0x100     ; buf = guest address of "hi\n"
    //      movz x2, #3         ; len = 3
    //      movz x8, #64        ; SYS_write
    //      hvc  #0             ; trap to host (commit 2 will switch to svc)
    //      movz x0, #0         ; status = 0
    //      movz x8, #93        ; SYS_exit
    //      hvc  #0
    //
    //  Each `movz xD, #imm` encodes as 0xD2800000 | (imm << 5) | D, with
    //  hw=0 (no shift). hvc #0 is 0xD4000002. You can verify any of these
    //  with `echo 'movz x0, #1' | clang -target aarch64 -c -x assembler -`
    //  and `objdump -d`.
    // -------------------------------------------------------------------------
    const uint32_t guest_code[] = {
        0xd2800020,   // movz x0, #1
        0xd2802001,   // movz x1, #0x100
        0xd2800062,   // movz x2, #3
        0xd2800808,   // movz x8, #64    (SYS_write)
        0xd4000002,   // hvc  #0
        0xd2800000,   // movz x0, #0
        0xd2800ba8,   // movz x8, #93    (SYS_exit)
        0xd4000002,   // hvc  #0
    };
    std::memcpy(host_mem, guest_code, sizeof(guest_code));
    std::memcpy((uint8_t*)host_mem + 0x100, "hi\n", 3);

    // -------------------------------------------------------------------------
    //  STEP 4 / Map the host page into the guest's physical address space.
    //
    //  The single page covers both code and data, so it must be RWX. A real
    //  kernel emulator would split code (RX) from data (RW) and never grant
    //  the guest write access to its own program. This is a milestone-1
    //  shortcut.
    // -------------------------------------------------------------------------
    CHECK(hv_vm_map(host_mem, kGuestPA, kPageSize,
                    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

    // -------------------------------------------------------------------------
    //  STEP 5 / Create a virtual CPU.
    // -------------------------------------------------------------------------
    hv_vcpu_t        vcpu;
    hv_vcpu_exit_t*  exit_info = nullptr;
    CHECK(hv_vcpu_create(&vcpu, &exit_info, nullptr));

    // -------------------------------------------------------------------------
    //  STEP 6 / Initialize vCPU registers.
    //
    //  PC = 0 (start of our code), PSTATE = 0x3c4 (EL1, all async masks
    //  set). MMU is left off — guest "virtual" addresses pass straight
    //  through to guest-physical addresses, which we mapped 1:1 above.
    //
    //  No stack pointer is set: the milestone-1 guest never touches memory
    //  beyond its hardcoded buffer, so no stack is needed.
    // -------------------------------------------------------------------------
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC,   kGuestPA));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c4));

    // -------------------------------------------------------------------------
    //  STEP 7 / The syscall dispatch loop.
    //
    //  This is the gVisor-shaped core: run the guest natively, catch every
    //  trap, decode it as a Linux syscall, service it on the host, write
    //  the return value back into the guest's X0, advance past the trap
    //  instruction, resume.
    //
    //  EXCEPTION CLASSES (top 6 bits of ESR_EL2). The only one we handle
    //  is HVC; anything else means a guest bug or an unimplemented feature
    //  and we abort. The full list is in the ARM ARM, "ESR_ELx Encoding."
    //    0x16 = HVC instruction execution in AArch64 state
    //
    //  PC ADVANCE. None needed. HVF leaves HV_REG_PC already pointing at
    //  the instruction *after* the HVC, matching ELR_EL2 = HVC_PC + 4.
    //  Re-entering the guest as-is resumes at the next instruction.
    // -------------------------------------------------------------------------
    int guest_exit_code = 0;
    bool guest_done = false;

    while (!guest_done) {
        CHECK(hv_vcpu_run(vcpu));

        if (exit_info->reason != HV_EXIT_REASON_EXCEPTION) {
            std::fprintf(stderr, "unexpected exit reason: %u\n",
                         (unsigned)exit_info->reason);
            return 1;
        }

        uint64_t esr = exit_info->exception.syndrome;
        uint32_t ec  = (esr >> 26) & 0x3f;
        if (ec != 0x16) {
            std::fprintf(stderr,
                         "unexpected exception, EC=0x%x ESR=0x%llx\n",
                         ec, (unsigned long long)esr);
            return 1;
        }

        // Pull the Linux syscall ABI registers out of the guest.
        uint64_t nr, x0, x1, x2;
        CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X8, &nr));
        CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0));
        CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1));
        CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2));

        int64_t ret = -ENOSYS;
        switch (nr) {
            case SYS_write: {
                // x0 = fd, x1 = buf (guest physical address), x2 = count.
                // Bounds-check against our single-page guest RAM, then
                // write directly from the host-mapped backing page.
                if (x1 > kPageSize || x2 > kPageSize - x1) {
                    ret = -EFAULT;
                    break;
                }
                ssize_t n = ::write((int)x0,
                                    (uint8_t*)host_mem + x1,
                                    (size_t)x2);
                ret = (n < 0) ? -errno : n;
                break;
            }
            case SYS_exit: {
                guest_exit_code = (int)x0;
                guest_done = true;
                break;
            }
            default:
                std::fprintf(stderr, "unimplemented syscall %llu\n",
                             (unsigned long long)nr);
                ret = -ENOSYS;
                break;
        }

        // Linux returns the syscall result in X0. We stash it before
        // advancing PC so even SYS_exit's value is observable from a
        // future debugger; here it's harmless because we exit the loop.
        CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, (uint64_t)ret));
    }

    // -------------------------------------------------------------------------
    //  STEP 8 / Tear down. Order matters: vCPU before VM, then unmap guest
    //  memory, then destroy the VM, then release the host page. The host
    //  process exits with the guest's exit status.
    // -------------------------------------------------------------------------
    CHECK(hv_vcpu_destroy(vcpu));
    CHECK(hv_vm_unmap(kGuestPA, kPageSize));
    CHECK(hv_vm_destroy());
    munmap(host_mem, kPageSize);
    return guest_exit_code;
}
