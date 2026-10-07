/* clang-format off */
/*
 * vm/safepoint_trap.S
 *
 * The safepoint trampoline: the stub that a trap fault redirects execution
 * to, so that stopping the thread happens in ordinary context rather than
 * inside a signal handler. See the note in rt/pal/posix/trap.c for why that
 * matters.
 *
 * It has to be assembly. It runs at an arbitrary point in a function, where
 * no compiler-generated prologue has executed, so there is no frame for C
 * code to save into and no way to know which registers the faulting code
 * still needs. What it has to do is save everything the ABI says a call may
 * clobber, call into C, restore, and jump back to the faulting instruction.
 *
 * This is a port of refs/shared/SafepointPollTrampoline-{aarch64,x86_64}.S
 * with the symbol names changed to this project's. The instruction
 * sequences are the reference's, unchanged: they were arrived at by
 * getting them wrong first, and there is nothing here to improve.
 *
 * The flow, end to end:
 *
 *   generated code loads from its trap cell
 *     -> the page is armed, so it faults
 *       -> the signal handler records the faulting PC and repoints the
 *          saved PC at cvm_safepoint_stub
 *         -> the kernel returns, and this stub runs on the faulting
 *            thread's own stack
 *           -> it calls cvm_safepoint_stub_park, which takes locks
 *           -> it jumps back to the faulting load, which now succeeds
 *
 * `.S` rather than `.s` because the file is preprocessed: one file covers
 * two architectures and three platforms.
 */

#if defined(CVM_SAFEPOINT_TRAPS) && defined(__aarch64__) &&                  \
    (defined(__linux__) || defined(__APPLE__))

/* q0-q7 (128 B) + x0-x30 (248 B) + resume at 376, rounded up to 384. */
#define CVM_STUB_FRAME 384

.text
.balign 16

#if defined(__APPLE__)
/* Mach-O C symbols carry a leading underscore. */
#define CVM_PARK _cvm_safepoint_stub_park
#define CVM_RESUME _cvm_safepoint_stub_resume_pc
.globl _cvm_safepoint_stub
_cvm_safepoint_stub:
#else
#define CVM_PARK cvm_safepoint_stub_park
#define CVM_RESUME cvm_safepoint_stub_resume_pc
.globl cvm_safepoint_stub
.type cvm_safepoint_stub, @function
cvm_safepoint_stub:
#endif
    sub sp, sp, #CVM_STUB_FRAME

    /* Floating-point and SIMD registers first, so the general-purpose ones
     * are saved as late as possible before the call. */
    stp q0, q1, [sp, #0]
    stp q2, q3, [sp, #32]
    stp q4, q5, [sp, #64]
    stp q6, q7, [sp, #96]

    stp x0, x1, [sp, #128]
    stp x2, x3, [sp, #144]
    stp x4, x5, [sp, #160]
    stp x6, x7, [sp, #176]
    stp x8, x9, [sp, #192]
    stp x10, x11, [sp, #208]
    stp x12, x13, [sp, #224]
    stp x14, x15, [sp, #240]
    stp x16, x17, [sp, #256]
    stp x18, x19, [sp, #272]
    stp x20, x21, [sp, #288]
    stp x22, x23, [sp, #304]
    stp x24, x25, [sp, #320]
    stp x26, x27, [sp, #336]
    stp x28, x29, [sp, #352]
    str x30, [sp, #368]

    /* Park, then ask where to resume. The order matters: parking ends only
     * when the collection does, and the resume address is only meaningful
     * after that. */
    bl CVM_PARK
    bl CVM_RESUME
    str x0, [sp, #376]

    ldr x30, [sp, #368]
    ldp x28, x29, [sp, #352]
    ldp x26, x27, [sp, #336]
    ldp x24, x25, [sp, #320]
    ldp x22, x23, [sp, #304]
    ldp x20, x21, [sp, #288]
    ldp x18, x19, [sp, #272]
    ldp x16, x17, [sp, #256]
    ldp x14, x15, [sp, #240]
    ldp x12, x13, [sp, #224]
    ldp x10, x11, [sp, #208]
    ldp x8, x9, [sp, #192]
    ldp x6, x7, [sp, #176]
    ldp x4, x5, [sp, #160]
    ldp x2, x3, [sp, #144]
    ldp x0, x1, [sp, #128]

    ldp q6, q7, [sp, #96]
    ldp q4, q5, [sp, #64]
    ldp q2, q3, [sp, #32]
    ldp q0, q1, [sp, #0]

    /* The resume address goes into a scratch register, because every
     * register the faulting code owns has just been restored. */
    ldr x16, [sp, #376]
    add sp, sp, #CVM_STUB_FRAME
    br x16

#elif defined(CVM_SAFEPOINT_TRAPS) && defined(__x86_64__) &&                \
    (defined(__linux__) || defined(__APPLE__))

/* XMM0-15 (256 B) + resume at 256 + GPRs (120 B) at 272, padded to 400 so
 * the stack stays 16-byte aligned across the calls. */
#define CVM_STUB_FRAME 400

.text
.balign 16

#if defined(__APPLE__)
#define CVM_PARK _cvm_safepoint_stub_park
#define CVM_RESUME _cvm_safepoint_stub_resume_pc
.globl _cvm_safepoint_stub
_cvm_safepoint_stub:
#else
#define CVM_PARK cvm_safepoint_stub_park
#define CVM_RESUME cvm_safepoint_stub_resume_pc
.globl cvm_safepoint_stub
.type cvm_safepoint_stub, @function
cvm_safepoint_stub:
#endif
    subq $CVM_STUB_FRAME, %rsp

    /* movdqu rather than movaps: a safepoint can interrupt code at a point
     * where the stack is not 16-byte aligned, and aligning it here would
     * mean moving the faulting code's own data. */
    movdqu %xmm0, 0(%rsp)
    movdqu %xmm1, 16(%rsp)
    movdqu %xmm2, 32(%rsp)
    movdqu %xmm3, 48(%rsp)
    movdqu %xmm4, 64(%rsp)
    movdqu %xmm5, 80(%rsp)
    movdqu %xmm6, 96(%rsp)
    movdqu %xmm7, 112(%rsp)
    movdqu %xmm8, 128(%rsp)
    movdqu %xmm9, 144(%rsp)
    movdqu %xmm10, 160(%rsp)
    movdqu %xmm11, 176(%rsp)
    movdqu %xmm12, 192(%rsp)
    movdqu %xmm13, 208(%rsp)
    movdqu %xmm14, 224(%rsp)
    movdqu %xmm15, 240(%rsp)

    movq %rax, 272(%rsp)
    movq %rbx, 280(%rsp)
    movq %rcx, 288(%rsp)
    movq %rdx, 296(%rsp)
    movq %rsi, 304(%rsp)
    movq %rdi, 312(%rsp)
    movq %r8,  320(%rsp)
    movq %r9,  328(%rsp)
    movq %r10, 336(%rsp)
    movq %r11, 344(%rsp)
    movq %rbp, 352(%rsp)
    movq %r12, 360(%rsp)
    movq %r13, 368(%rsp)
    movq %r14, 376(%rsp)
    movq %r15, 384(%rsp)

    call CVM_PARK
    call CVM_RESUME
    movq %rax, 256(%rsp)

    movq 384(%rsp), %r15
    movq 376(%rsp), %r14
    movq 368(%rsp), %r13
    movq 360(%rsp), %r12
    movq 352(%rsp), %rbp
    movq 344(%rsp), %r11
    movq 336(%rsp), %r10
    movq 328(%rsp), %r9
    movq 320(%rsp), %r8
    movq 312(%rsp), %rdi
    movq 304(%rsp), %rsi
    movq 296(%rsp), %rdx
    movq 288(%rsp), %rcx
    movq 280(%rsp), %rbx
    movq 272(%rsp), %rax

    movdqu 240(%rsp), %xmm15
    movdqu 224(%rsp), %xmm14
    movdqu 208(%rsp), %xmm13
    movdqu 192(%rsp), %xmm12
    movdqu 176(%rsp), %xmm11
    movdqu 160(%rsp), %xmm10
    movdqu 144(%rsp), %xmm9
    movdqu 128(%rsp), %xmm8
    movdqu 112(%rsp), %xmm7
    movdqu 96(%rsp), %xmm6
    movdqu 80(%rsp), %xmm5
    movdqu 64(%rsp), %xmm4
    movdqu 48(%rsp), %xmm3
    movdqu 32(%rsp), %xmm2
    movdqu 16(%rsp), %xmm1
    movdqu 0(%rsp), %xmm0

    movq 256(%rsp), %r10
    addq $CVM_STUB_FRAME, %rsp
    jmp *%r10

#else

/*
 * Trap mode is off for this build, or this platform has no trampoline.
 *
 * Emitting a stub that does nothing would be worse than emitting nothing:
 * the handler would redirect execution into it and the process would
 * continue from the wrong place. `cvm_safepoint_trap_usable` reports false
 * instead, and the collector polls.
 */
.text
.balign 4
.globl cvm_safepoint_stub_unsupported
cvm_safepoint_stub_unsupported:
    ret

#endif

#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
