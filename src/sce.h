/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#ifndef SADES_SCE_H
#define SADES_SCE_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#include "m00.h"

/* SADES Virtual Machine.
 *
 * The engine is a stack-driven bytecode interpreter for a visual-novel
 * scripting language. A game ships as three files:
 *
 *     <name>.SCE   compiled bytecode + slot table
 *     <name>.M00   Shift-JIS message archive
 *     <name>.RC    text positions and color modes
 *
 * and a directory of .GLZ / .XSC graphics with a matching .PAL palette.
 *
 * === File format (.SCE) ===
 *
 *   Offset  Size  Field
 *   0x00    0x1A  magic: "for SADES version N.M" + padding
 *   0x1A    0x02  constant 0x0A 0x1A
 *   0x1C    0x04  padding
 *   0x20    0x02  nslots (uint16 LE)
 *   0x22    nslots*2  slot table (uint16 LE per entry)
 *   ...            bytecode starts at 0x22 + nslots*2
 *
 * Each slot holds the bytecode offset (relative to bytecode start)
 * of a label. The compiler emits one slot per SCENE directive in the
 * source; the VM reads them all into vars[0..nslots-1] at startup, so
 * a GOTO N is just "jump to vars[N]".
 *
 * === Operand encoding ===
 *
 *   A 16-bit operand word is either:
 *       bit15 == 0  -> literal value (0..0x7FFF)
 *       bit15 == 1  -> index into vars[] (vars[word & 0x7FFF])
 *
 * This applies to all "operand" arguments of jumps, branches and IFs.
 *
 * === User variables ===
 *
 *   vars[0..19]          script-visible variables (SET A, ADD A, ...)
 *   vars[20..nslots-1]   label table (read once at init)
 *
 * The script never touches the label range; only GOTO/IF/BRANCH do.
 *
 * === Opcode set (42 total) ===
 *
 *   0x01-0x09  PUSH_A N        blocking menu, N options
 *   0x11-0x19  PUSH_B N        same but separate stack
 *   0x30       DROP            wait for key, sets v[0..2]=1
 *   0x31       BRANCH          if a >= b, jump to vars[c]
 *   0x32       JMP_IND         PC = vars[arg]
 *   0x33       CALL_IND        push PC, PC = vars[arg]
 *   0x34       RET
 *   0x35-0x3A  IF_EQ/NE/GT/LT/GE/LE
 *   0x40       CALL_SCRIPT     draw text at id's RC position
 *   0x41       STORE           var[byte] = operand
 *   0x42       DELAY           sleep operand * 32 ms (SADES_DELAY_MULT)
 *   0x43       LOAD_GLZ_BY_ID  GUARD: load P<id>.GLZ
 *   0x44       PRINT_CLEAR     bit0=clear text, bit1=clear VRAM
 *   0x45-0x48  ADD/SUB/AND/OR_VAR
 *   0x49       PUSH_ARG
 *   0x4A       POP_ARG
 *   0x4B       IF_4B           (not used by any known game)
 *   0xFF       HALT
 *
 * See the .c file for the argument layout of each opcode.
 */

#define VM_NVARS 1024

struct VM_;
typedef struct VM_ VM;

typedef struct {
    uint8_t  *data;         /* raw .SCE bytes */
    size_t    size;
    uint16_t  nslots;       /* number of labels */
    size_t    bc_start;     /* 0x22 + nslots*2 */
    uint16_t *slots;        /* nslots entries, copied to vars[] by vm_init */
    char      path[512];    /* original filename, used to locate .RC */
} SCE;

/* === Callbacks ===
 *
 * These let the front-end (main.c) handle I/O and rendering without
 * the VM having to know about SDL, FreeType or the filesystem.
 */

/* Wait for the user. n_opts is the number of menu options (1 for DROP).
 * Returns 0..n_opts-1 to select an option, or -1 to abort the VM. */
typedef int  (*vm_wait_fn_t)(VM *vm, int n_opts);

/* Sleep for the DELAY opcode (0x42). Ticks are scaled by
 * SADES_DELAY_MULT in the front-end (default 32 ms). */
typedef void (*vm_delay_fn_t)(VM *vm, uint16_t ticks);

/* Text was drawn at a slot. `text` is UTF-8, possibly empty. */
typedef void (*vm_call_fn_t)(VM *vm, int id, int sub,
                             uint16_t m00off, const char *text);

/* CALL_IND resolution (tracing hook, may be NULL). */
typedef void (*vm_call_ind_fn_t)(VM *vm, uint16_t arg, uint16_t target);

/* Load a graphic. `arg_x`/`arg_y` may be 0xFFFF meaning "unspecified,
 * use the header's pad_x/pad_y". Returns 0 on success. */
typedef int  (*vm_load_glz_fn_t)(VM *vm, int glz_id, int arg_x, int arg_y);

/* PRINT_CLEAR: bit0 = clear text layer, bit1 = clear VRAM. */
typedef void (*vm_print_clear_fn_t)(VM *vm, uint16_t flags);

typedef struct {
    uint8_t x, y, mode;
} rc_entry_t;

struct VM_ {
    SCE *sce;
    M00 *m00;

    /* VM registers */
    uint16_t vars[VM_NVARS];      /* script + label space */
    uint16_t call_stack[256];     /* CALL_IND return addresses */
    int      csp;
    size_t   pc;                  /* offset into bytecode */
    int      halted;

    /* Tracing / debugging */
    int      trace;
    FILE    *out;                 /* defaults to stdout */
    int      max_steps, steps;
    int      call_count, wait_count, unknown_ops;

    /* Control */
    int      wait_on_drop;        /* 0 = ignore DROP (batch mode) */

    /* Callbacks */
    vm_wait_fn_t        wait_fn;
    vm_delay_fn_t       delay_fn;
    vm_call_fn_t        call_fn;
    vm_call_ind_fn_t    call_ind_fn;
    vm_load_glz_fn_t    load_glz_fn;
    vm_print_clear_fn_t print_clear_fn;

    /* .RC table, loaded lazily on the first vm_run() */
    rc_entry_t rc[256];
    int        rc_count;
};

/* Load a .SCE file. Returns 0 on success, -1 on error. */
int  sce_load(const char *path, SCE *s);

/* Release memory owned by SCE. Safe to call multiple times. */
void sce_free(SCE *s);

/* Prepare a VM from an already-loaded SCE and M00.
 * Copies the slot table into vars[0..nslots-1] and zeroes everything else. */
void vm_init(VM *vm, SCE *sce, M00 *m00);

/* Run the VM starting at the given bytecode offset (0 = entry point).
 * Returns 0 if the VM halted cleanly, -1 on unrecoverable error. */
int  vm_run(VM *vm, size_t entry_pc);

#endif /* SADES_SCE_H */