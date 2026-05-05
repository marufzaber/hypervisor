// Minimal arm64 hypervisor on macOS (Hypervisor.framework, HVF).
// Creates a VM, maps one page of guest RAM, runs three instructions,
// catches the HVC trap, prints the guest's X0 register.

#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(call) do { \
    hv_return_t _r = (call); \
    if (_r != HV_SUCCESS) { \
        std::fprintf(stderr, #call " failed: 0x%x\n", _r); \
        std::exit(1); \
    } \
} while (0)

int main() {
    // 1. Create the VM. One VM per process on HVF.
    CHECK(hv_vm_create(nullptr));

    // 2. Allocate one host page that will back guest physical address 0.
    //    HVF requires the host backing to be page-aligned; mmap gives us that.
    constexpr size_t kPageSize = 0x4000;          // 16 KiB on Apple Silicon
    constexpr uint64_t kGuestPA = 0x0;            // guest physical base
    void* host_mem = mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (host_mem == MAP_FAILED) { std::perror("mmap"); return 1; }

    // 3. Guest program. Encoded little-endian arm64:
    //    movz x0, #42      ; x0 = 42
    //    add  x0, x0, #1   ; x0 = 43
    //    hvc  #0           ; trap to hypervisor (we exit the run loop here)
    const uint32_t guest_code[] = {
        0xd2800540,
        0x91000400,
        0xd4000002,
    };
    std::memcpy(host_mem, guest_code, sizeof(guest_code));

    // 4. Map host memory into the guest's physical address space as RWX.
    CHECK(hv_vm_map(host_mem, kGuestPA, kPageSize,
                    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

    // 5. Create a vCPU.
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t* exit_info = nullptr;
    CHECK(hv_vcpu_create(&vcpu, &exit_info, nullptr));

    // 6. Initial register state.
    //    PC = guest entry point.
    //    PSTATE = DAIF masked + EL1h (M[3:0]=0101) so we start in EL1
    //    with all async exceptions/interrupts masked. 0x3c4 is the canonical
    //    "boot" value used by every HVF tutorial; without it the vCPU faults
    //    immediately on the first instruction.
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, kGuestPA));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c4));

    // 7. Run loop. HVF returns from hv_vcpu_run on every VM exit; we have to
    //    inspect the reason and decide whether to resume or stop.
    for (;;) {
        CHECK(hv_vcpu_run(vcpu));

        if (exit_info->reason == HV_EXIT_REASON_EXCEPTION) {
            // ESR_EL2.EC is bits [31:26] of the syndrome register.
            uint64_t esr = exit_info->exception.syndrome;
            uint32_t ec = (esr >> 26) & 0x3f;

            // EC=0x16 is "HVC instruction execution in AArch64 state".
            if (ec == 0x16) {
                uint64_t x0 = 0;
                CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0));
                std::printf("guest exited via HVC, x0 = %llu\n",
                            (unsigned long long)x0);
                break;
            }
            std::fprintf(stderr, "unexpected exception, EC=0x%x ESR=0x%llx\n",
                         ec, (unsigned long long)esr);
            return 1;
        }

        std::fprintf(stderr, "unexpected exit reason: %u\n",
                     (unsigned)exit_info->reason);
        return 1;
    }

    // 8. Tear down.
    CHECK(hv_vcpu_destroy(vcpu));
    CHECK(hv_vm_unmap(kGuestPA, kPageSize));
    CHECK(hv_vm_destroy());
    munmap(host_mem, kPageSize);
    return 0;
}
