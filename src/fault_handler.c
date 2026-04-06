/**
 * Hard Fault Handler
 * 
 * Catches hard faults and writes diagnostic info to UART before hanging.
 * Useful for debugging mysterious crashes and heap/stack corruption.
 */

#include <stdio.h>
#include <pico/stdlib.h>
#include <arm_cmse.h>

/* Define intrinsics if not available */
#ifndef __disable_irq
#define __disable_irq() __asm volatile("cpsid i" : : : "memory")
#endif

#ifndef __enable_irq
#define __enable_irq() __asm volatile("cpsie i" : : : "memory")
#endif

/**
 * HardFault_Handler - captures fault context and prints diagnostic info
 * 
 * The ARM Cortex-M0+ pushes 8 registers onto the stack before entering this handler:
 *   R0, R1, R2, R3, R12, LR, PC, xPSR
 * 
 * The stack pointer (MSP or PSP) points to this data.
 * We can extract it to determine where the fault occurred.
 */
__attribute__((naked)) void HardFault_Handler(void)
{
    /* Capture stack pointer and switch to a local frame */
    __asm volatile(
        "tst lr, #4\n"              /* Check if using PSP or MSP */
        "ite eq\n"                  /* If-Then-Else */
        "mrseq r0, msp\n"           /* Use MSP if bit 2 of LR = 0 */
        "mrsne r0, psp\n"           /* Use PSP if bit 2 of LR = 1 */
        "mov r1, lr\n"              /* Save LR (exception return code) */
        "b hardFault_handler_c\n"   /* Call C handler with SP in R0, LR in R1 */
    );
}

/**
 * C implementation of hard fault handler
 * 
 * @param stack_ptr Pointer to the stack frame with fault context
 * @param link_register Exception return code (tells us from where this occurred)
 */
void __attribute__((noinline)) hardFault_handler_c(unsigned long *stack_ptr, unsigned long link_register)
{
    /* Disable interrupts to ensure UART writes complete */
    __disable_irq();
    
    /* Extract fault context from stack frame */
    unsigned long r0  = stack_ptr[0];
    unsigned long r1  = stack_ptr[1];
    unsigned long r2  = stack_ptr[2];
    unsigned long r3  = stack_ptr[3];
    unsigned long r12 = stack_ptr[4];
    unsigned long lr  = stack_ptr[5];      /* Return address (where fault occurred) */
    unsigned long pc  = stack_ptr[6];      /* Program counter at fault */
    unsigned long psr = stack_ptr[7];      /* Program status register */
    
    /* Give UART time to be ready (in case it's not initialized yet) */
    sleep_ms(100);
    
    /* Print fault diagnostics to UART */
    printf("\n");
    printf("========== HARD FAULT DETECTED ==========\n");
    printf("Fault occurred at PC: 0x%08lx\n", pc);
    printf("Return address (LR): 0x%08lx\n", lr);
    printf("Exception return code: 0x%08lx\n", link_register);
    printf("\nStack Frame Register State:\n");
    printf("  R0:   0x%08lx\n", r0);
    printf("  R1:   0x%08lx\n", r1);
    printf("  R2:   0x%08lx\n", r2);
    printf("  R3:   0x%08lx\n", r3);
    printf("  R12:  0x%08lx\n", r12);
    printf("  PSR:  0x%08lx\n", psr);
    printf("Stack Pointer: 0x%08lx\n\n", (unsigned long)stack_ptr);
    
    /* Check fault status registers (if accessible on RP2350) */
    unsigned long cfsr = *(unsigned long *)0xE000ED28;  /* Configurable Fault Status Register */
    unsigned long hfsr = *(unsigned long *)0xE000ED2C;  /* Hard Fault Status Register */
    
    printf("Fault Status Registers:\n");
    printf("  CFSR: 0x%08lx\n", cfsr);
    printf("  HFSR: 0x%08lx\n", hfsr);
    
    /* Decode CFSR bits */
    if (cfsr & 0x00000080) printf("  -> DIVBYZERO: Division by zero\n");
    if (cfsr & 0x00000100) printf("  -> UNALIGNED: Unaligned memory access\n");
    if (cfsr & 0x00010000) printf("  -> IBUSERR: Instruction bus error\n");
    if (cfsr & 0x00020000) printf("  -> PRECISEERR: Precise data bus error at 0x%08lx\n", *(unsigned long *)0xE000ED34);
    if (cfsr & 0x00040000) printf("  -> IMPRECISERR: Imprecise data bus error\n");
    if (cfsr & 0x00080000) printf("  -> UNSTKERR: Bus fault during exception entry\n");
    if (cfsr & 0x00100000) printf("  -> STKERR: Bus fault during exception exit\n");
    if (cfsr & 0x00000001) printf("  -> IACCVIOL: Instruction access violation\n");
    if (cfsr & 0x00000002) printf("  -> DACCVIOL: Data access violation\n");
    if (cfsr & 0x00000008) printf("  -> MUNSTKERR: Mem fault during exception entry\n");
    if (cfsr & 0x00000010) printf("  -> MSTKERR: Mem fault during exception exit\n");
    if (cfsr & 0x00000020) printf("  -> MLSPERR: Mem fault during FP lazy state\n");
    
    printf("\nSystem halted. Use arm-none-eabi-addr2line to decode PC:\n");
    printf("  arm-none-eabi-addr2line -e build/vibecode4.elf 0x%08lx\n", pc);
    printf("=========================================\n\n");
    
    /* Flush output */
    for (int i = 0; i < 100; i++) {
        sleep_ms(1);
    }
    
    /* Hang in a busy loop so you can see the output before watchdog resets */
    while (1) {
        printf(".");
        sleep_ms(500);
    }
}
