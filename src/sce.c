/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#include "sce.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* SCE file loading                                                      */
/* --------------------------------------------------------------------- */

int sce_load(const char *path, SCE *s) {
    memset(s, 0, sizeof(*s));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    s->data = malloc(sz > 0 ? (size_t)sz : 1);
    if (!s->data) { fclose(f); return -1; }
    s->size = (size_t)sz;

    if (sz > 0 && fread(s->data, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(s->data); s->data = NULL; return -1;
    }
    fclose(f);
    snprintf(s->path, sizeof(s->path), "%s", path);

    if (sz < 0x22) {
        s->nslots = 0;
        s->bc_start = 0x22;
        s->slots = malloc(2);
        return -1;
    }

    s->nslots   = s->data[0x20] | (s->data[0x21] << 8);
    s->bc_start = 0x22 + s->nslots * 2;
    if (s->bc_start > s->size) {
        s->nslots = 0;
        s->slots = malloc(2);
        return -1;
    }

    if (s->nslots > 0) {
        s->slots = malloc(s->nslots * sizeof(uint16_t));
        if (!s->slots) { return -1; }
        for (int i = 0; i < s->nslots; i++) {
            size_t off = 0x22 + (size_t)i * 2;
            s->slots[i] = (off + 1 < s->size)
                        ? (uint16_t)(s->data[off] | (s->data[off + 1] << 8))
                        : 0xFFFF;
        }
    } else {
        s->slots = malloc(2);
    }
    return 0;
}

void sce_free(SCE *s) {
    if (!s) return;
    free(s->data);  s->data  = NULL;
    free(s->slots); s->slots = NULL;
    s->size = 0; s->nslots = 0;
}

/* --------------------------------------------------------------------- */
/* Bytecode stream helpers                                               */
/* --------------------------------------------------------------------- */

/* Read a byte from the bytecode. Sets vm->halted on overflow. */
static int rd_byte(VM *vm) {
    if (vm->sce->bc_start + vm->pc >= vm->sce->size) {
        vm->halted = 1;
        return 0;
    }
    return vm->sce->data[vm->sce->bc_start + vm->pc++];
}

/* Read a little-endian 16-bit word. Sets vm->halted on overflow. */
static uint16_t rd_word(VM *vm) {
    if (vm->sce->bc_start + vm->pc + 1 >= vm->sce->size) {
        vm->halted = 1;
        return 0;
    }
    size_t off = vm->sce->bc_start + vm->pc;
    uint16_t w = (uint16_t)(vm->sce->data[off]
                         | (vm->sce->data[off + 1] << 8));
    vm->pc += 2;
    return w;
}

/* Read an operand. If bit15 is set, it's an index into vars[]. */
static uint16_t rd_operand(VM *vm) {
    uint16_t w = rd_word(vm);
    if (w & 0x8000) {
        int idx = w & 0x7FFF;
        return (idx < VM_NVARS) ? vm->vars[idx] : 0;
    }
    return w;
}

/* --------------------------------------------------------------------- */
/* Tracing                                                               */
/* --------------------------------------------------------------------- */

static void log_line(VM *vm, const char *fmt, ...) {
    if (!vm->trace) return;
    va_list ap; va_start(ap, fmt);
    if (vm->out) vfprintf(vm->out, fmt, ap);
    else         vfprintf(stdout, fmt, ap);
    va_end(ap);
}

static void emit_line(VM *vm, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (vm->out) vfprintf(vm->out, fmt, ap);
    else         vfprintf(stdout, fmt, ap);
    va_end(ap);
}

/* --------------------------------------------------------------------- */
/* Jump helpers                                                          */
/* --------------------------------------------------------------------- */

static void vm_jump(VM *vm, size_t pc) {
    if (pc >= vm->sce->size - vm->sce->bc_start) {
        fprintf(stderr, "[vm] jump out of range: 0x%zX\n", pc);
        vm->halted = 1;
        return;
    }
    vm->pc = pc;
}

/*
 * Resolve a jump target against the pristine slot table.
 *
 * The compiler stores scene labels in .SCE slots, and the VM copies
 * them into vars[] at startup. Some games write script variables into
 * low vars[] indices and clobber the copy, so we always look at the
 * slot table, not vars[].
 *
 * A slot value of 0xFFFF means "uncompiled scene". Returning that
 * would trigger a halt in vm_jump, so we fall back to vars[spec]
 * if it holds something sensible, and finally to the literal spec.
 */
static uint16_t resolve_target(VM *vm, uint16_t spec) {
    if (vm->sce && vm->sce->slots && spec < vm->sce->nslots) {
        uint16_t t = vm->sce->slots[spec];
        if (t != 0xFFFF) return t;
        fprintf(stderr, "[vm] resolve_target: slot[%u] is 0xFFFF "
                "(uncompiled scene)\n", spec);
    }
    if (spec < VM_NVARS && vm->vars[spec] != 0xFFFF)
        return vm->vars[spec];
    return spec;
}

/* --------------------------------------------------------------------- */
/* .RC parsing                                                           */
/* --------------------------------------------------------------------- */

/* Load <path>.RC (same directory as the .SCE) and populate vm->rc[].
 * Format: "ID:X,Y,MODE\r\n" per line; X/Y are in 8x16 character cells,
 * MODE is a PC-98 text color index. */
static int rc_parse(VM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int id, x, y, mode;
        if (sscanf(line, "%d:%d,%d,%d", &id, &x, &y, &mode) == 4) {
            if (id >= 0 && id < 256) {
                vm->rc[id].x    = (uint8_t)x;
                vm->rc[id].y    = (uint8_t)y;
                vm->rc[id].mode = (uint8_t)mode;
                if (id + 1 > vm->rc_count) vm->rc_count = id + 1;
            }
        }
    }
    fclose(f);
    return 0;
}

/* --------------------------------------------------------------------- */
/* VM initialization                                                     */
/* --------------------------------------------------------------------- */

void vm_init(VM *vm, SCE *sce, M00 *m00) {
    memset(vm, 0, sizeof(*vm));
    vm->sce = sce;
    vm->m00 = m00;
    vm->max_steps = 100000;

    /* Copy the slot table into the label range. Everything else stays 0. */
    if (sce && sce->slots) {
        int lim = sce->nslots < VM_NVARS ? sce->nslots : VM_NVARS;
        for (int i = 0; i < lim; i++) {
            if (sce->slots[i] != 0xFFFF) vm->vars[i] = sce->slots[i];
        }
    }
}

/* --------------------------------------------------------------------- */
/* Main dispatch loop                                                    */
/* --------------------------------------------------------------------- */

int vm_run(VM *vm, size_t entry_pc) {
    /* Lazy-load the .RC table on first run. */
    if (vm->rc_count == 0 && vm->sce->path[0]) {
        char rcpath[1024];
        snprintf(rcpath, sizeof(rcpath), "%s", vm->sce->path);
        char *dot = strrchr(rcpath, '.');
        if (dot && (size_t)(dot - rcpath) + 4 < sizeof(rcpath)) {
            dot[1] = 'R'; dot[2] = 'C'; dot[3] = 0;
            rc_parse(vm, rcpath);
        }
    }

    vm->pc = entry_pc;
    char utf8buf[2048];

    while (!vm->halted && vm->steps < vm->max_steps) {
        vm->steps++;
        size_t op_pc = vm->pc;
        int op = rd_byte(vm);

        /* --- Menus ------------------------------------------------------ */
        if (op >= 0x01 && op <= 0x09) {
            int n = op;
            uint16_t args[32];
            if (n > 32) n = 32;
            log_line(vm, "%04zX: PUSH_A %d args:", op_pc, n);
            for (int i = 0; i < n; i++) {
                args[i] = rd_word(vm);
                log_line(vm, " %u", args[i]);
            }
            log_line(vm, "\n");

            int idx = 0;
            if (vm->wait_fn) {
                int r = vm->wait_fn(vm, n);
                if (r < 0) { vm->halted = 1; return 0; }
                idx = r;
            }
            if (idx < 0 || idx >= n) idx = 0;
            uint16_t targ = args[idx];
            /*
             * If the menu target is 0, don't jump — fall through to
             * the instruction after PUSH_A. Some scripts use the
             * argument slot as a variable that starts at zero, and
             * treating 0 as "goto scene 1" restarts the whole game.
             */
            if (targ == 0) {
                log_line(vm, "    [PUSH_A] op %d -> args[%d]=0 (fall-through)\n",
                         idx + 1, idx);
            } else {
                uint16_t tpc = resolve_target(vm, targ);
                log_line(vm, "    [PUSH_A] op %d -> args[%d]=%u -> scene=%u -> 0x%X\n",
                         idx + 1, idx, targ, targ, tpc);
                vm_jump(vm, tpc);
            }
        }
        else if (op >= 0x11 && op <= 0x19) {
            int n = op - 0x10;
            uint16_t args[32];
            if (n > 32) n = 32;
            log_line(vm, "%04zX: PUSH_B %d args:", op_pc, n);
            for (int i = 0; i < n; i++) {
                args[i] = rd_word(vm);
                log_line(vm, " %u", args[i]);
            }
            log_line(vm, "\n");

            int idx = 0;
            if (vm->wait_fn) {
                int r = vm->wait_fn(vm, n);
                if (r < 0) { vm->halted = 1; return 0; }
                idx = r;
            }
            if (idx < 0 || idx >= n) idx = 0;
            uint16_t targ = args[idx];
if (targ == 0) {
log_line(vm, "    [PUSH_B] op %d -> args[%d]=0 (fall-through)\n",
idx + 1, idx);
} else {
uint16_t tpc = resolve_target(vm, targ);
log_line(vm, "    [PUSH_B] op %d -> scene=%u -> 0x%X\n",
idx + 1, targ, tpc);
vm_jump(vm, tpc);
}
        }

        /* --- DROP ------------------------------------------------------- */
        /* Wait for a key. Sets v[0..2]=1 so the compiler-emitted loop
         * "v[3] = 1 + v[1] + v[2]; if v[3] > 2 goto <next scene>" exits. */
        else if (op == 0x30) {
            vm->wait_count++;
            log_line(vm, "%04zX: DROP (wait_key)\n", op_pc);
            if (vm->wait_on_drop && vm->wait_fn) {
                int r = vm->wait_fn(vm, 1);
                if (r < 0) { vm->halted = 1; return 0; }
            }
            vm->vars[0] = 1;
            vm->vars[1] = 1;
            vm->vars[2] = 1;
            if (vm->wait_count > 25000) vm->halted = 1;
        }

        /* --- Control flow ---------------------------------------------- */
        else if (op == 0x31) {
            uint16_t a = rd_operand(vm);
            uint16_t b = rd_operand(vm);
            uint16_t c = rd_operand(vm);
            if (a >= b) {
                uint16_t t = resolve_target(vm, c);
                log_line(vm, "%04zX: BRANCH -> vars[%u]=0x%X\n", op_pc, c, t);
                vm_jump(vm, t);
            } else {
                log_line(vm, "%04zX: BRANCH (not taken)\n", op_pc);
            }
        }
        else if (op == 0x32) {
            uint16_t c = rd_operand(vm);
            uint16_t t = resolve_target(vm, c);
            log_line(vm, "%04zX: JMP_IND vars[%u]=0x%X\n", op_pc, c, t);
            vm_jump(vm, t);
        }
        else if (op == 0x33) {
            uint16_t c = rd_operand(vm);
            uint16_t t = resolve_target(vm, c);
            if (vm->call_ind_fn) vm->call_ind_fn(vm, c, t);
            if (vm->csp < 256) vm->call_stack[vm->csp++] = (uint16_t)vm->pc;
            log_line(vm, "%04zX: CALL_IND vars[%u]=0x%X\n", op_pc, c, t);
            vm_jump(vm, t);
        }
        else if (op == 0x34) {
            if (vm->csp > 0) {
                vm->pc = vm->call_stack[--vm->csp];
                log_line(vm, "%04zX: RET -> 0x%zX\n", op_pc, vm->pc);
            } else {
                vm->halted = 1;
            }
        }
        else if (op >= 0x35 && op <= 0x3A) {
            /* IF_3A (0x3A) has a different argument layout:
             * one byte (literal) + two words. */
            uint16_t a, b, c;
            if (op == 0x3A) {
                a = rd_byte(vm);
                b = rd_word(vm);
                c = rd_word(vm);
            } else {
                a = rd_operand(vm);
                b = rd_operand(vm);
                c = rd_operand(vm);
            }
            int cond = 0;
            switch (op) {
                case 0x35: cond = (a == b); break;
                case 0x36: cond = (a != b); break;
                case 0x37: cond = (a >  b); break;
                case 0x38: cond = (a <  b); break;
                case 0x39: cond = (a >= b); break;
                case 0x3A: cond = (a <= b); break;
            }
            if (cond) {
                uint16_t t = resolve_target(vm, c);
                log_line(vm, "%04zX: IF%02X -> vars[%u]=0x%X\n", op_pc, op, c, t);
                vm_jump(vm, t);
            } else {
                log_line(vm, "%04zX: IF%02X (not taken)\n", op_pc, op);
            }
        }

        /* --- CALL_SCRIPT: draw text at a slot -------------------------- */
        else if (op == 0x40) {
            int id_ = rd_byte(vm);
            int sub = rd_byte(vm);
            uint16_t m00off = rd_word(vm);
            vm->call_count++;
            log_line(vm, "%04zX: CALL id=%d sub=%d m00=0x%04X\n",
                     op_pc, id_, sub, m00off);
            if (vm->m00) {
                size_t slen = 0;
                const uint8_t *s = m00_get(vm->m00, m00off, &slen);
                if (s && slen > 0) {
                    int n = sjis_to_utf8(s, slen, utf8buf, sizeof(utf8buf));
                    if (n > 0) {
                        emit_line(vm, "    [%d/%d] %s\n", id_, sub, utf8buf);
                        if (vm->call_fn) {
                            vm->call_fn(vm, id_, sub, m00off, utf8buf);
                        }
                    }
                } else if (vm->call_fn) {
                    vm->call_fn(vm, id_, sub, m00off, "");
                }
            }
        }

        /* --- Variable arithmetic --------------------------------------- */
        else if (op == 0x41) {
            int var = rd_byte(vm);
            uint16_t v = rd_operand(vm);
            if (var >= 0 && var < VM_NVARS) vm->vars[var] = v;
            log_line(vm, "%04zX: STORE v[%d] = %u\n", op_pc, var, v);
        }
        else if (op == 0x42) {
            uint16_t ticks = rd_operand(vm);
            log_line(vm, "%04zX: DELAY %u ticks\n", op_pc, ticks);
            if (vm->delay_fn) vm->delay_fn(vm, ticks);
        }
        else if (op == 0x43) {
            /* GUARD: load P<id>.GLZ. With arg_x == arg_y == 0xFFFF the
             * callee uses the header's pad_x / pad_y (portraits). */
            uint16_t id = rd_operand(vm);
            int ret = 0;
            if (vm->load_glz_fn) {
                ret = vm->load_glz_fn(vm, (int)id, 0xFFFF, 0xFFFF);
            }
            if (ret == 0 && 25 < VM_NVARS) vm->vars[25] = id;
            log_line(vm, "%04zX: GUARD id=%u (ret=%d)\n", op_pc, id, ret);
        }
        else if (op == 0x44) {
            uint16_t v = rd_operand(vm);
            if (vm->print_clear_fn) vm->print_clear_fn(vm, v);
            log_line(vm, "%04zX: PRINT_CLEAR flags=%u (bit1=%d)\n",
                     op_pc, v, (v >> 1) & 1);
        }
        else if (op >= 0x45 && op <= 0x48) {
            int var = rd_byte(vm);
            uint16_t v = rd_operand(vm);
            uint16_t cur = (var >= 0 && var < VM_NVARS) ? vm->vars[var] : 0;
            uint16_t nv = cur;
            if      (op == 0x45) nv = (uint16_t)(cur + v);
            else if (op == 0x46) nv = (uint16_t)(cur - v);
            else if (op == 0x47) nv = (uint16_t)(cur & v);
            else if (op == 0x48) nv = (uint16_t)(cur | v);
            if (var >= 0 && var < VM_NVARS) vm->vars[var] = nv;
            log_line(vm, "%04zX: OP%02X v[%d]: %u -> %u\n",
                     op_pc, op, var, cur, nv);
        }

        /* --- Argument stack -------------------------------------------- */
        else if (op == 0x49) {
            uint16_t v = rd_operand(vm);
            if (vm->csp < 256) vm->call_stack[vm->csp++] = v;
            log_line(vm, "%04zX: PUSH_ARG %u\n", op_pc, v);
        }
        else if (op == 0x4A) {
            uint16_t v = (vm->csp > 0) ? vm->call_stack[--vm->csp] : 0;
            int var = rd_byte(vm);
            if (var >= 0 && var < VM_NVARS) vm->vars[var] = v;
            log_line(vm, "%04zX: POP_ARG v[%d] = %u\n", op_pc, var, v);
        }
        else if (op == 0x4B) {
            uint16_t a = rd_operand(vm);
            uint16_t b = rd_operand(vm);
            uint16_t c = rd_operand(vm);
            if (a == b) {
                uint16_t t = resolve_target(vm, c);
                vm_jump(vm, t);
            } else if (0x19 < VM_NVARS) {
                vm->vars[0x19] = a;
            }
        }

        else if (op == 0xFF) {
            vm->halted = 1;
            log_line(vm, "%04zX: HALT\n", op_pc);
        }
        else {
            vm->unknown_ops++;
            log_line(vm, "%04zX: (unknown) 0x%02X\n", op_pc, op);
            if (vm->unknown_ops > 1000) vm->halted = 1;
        }
    }
    return vm->halted ? 0 : -1;
}