/* sid_rsid.c - PSID/RSID -> SID register stream, in plain C.
 *
 * A .sid file is 6502 code; the music only exists as writes to $D400-$D41F.
 * This file emulates the CPU (fake6502, from rubbermallet.org via the
 * ChristmasSID sketch), the VIC raster counter and the two CIA timers, runs
 * init once and play once per frame, and records every SID write with a
 * timestamp.
 *
 * Where it came from: the 6502/CIA/VIC part is copied from the STC32G
 * chiptune project (usb_cdc_test/tools/sid_extract.c) so both projects agree
 * on what a .sid sounds like. Only the entry points and the machine reset
 * changed: it is now a library that the player links against, not a
 * stand-alone tool that writes a file.
 *
 * Timestamps: init-phase writes are all pinned to t=0 (they must be applied
 * before the first play frame), play-frame writes use
 * frame*cycles_per_frame + intra-frame cycle, so PCM/digi songs keep their
 * sub-frame ordering.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "sid_rsid.h"

/* ============================================================
 * fake6502 核心 (来自 ChristmasSID 03_6502.ino, 改纯 C)
 * http://rubbermallet.org/fake6502.c
 * ============================================================ */

#define FLAG_CARRY     0x01
#define FLAG_ZERO      0x02
#define FLAG_INTERRUPT 0x04
#define FLAG_DECIMAL   0x08
#define FLAG_BREAK     0x10
#define FLAG_CONSTANT  0x20
#define FLAG_OVERFLOW  0x40
#define FLAG_SIGN      0x80
#define BASE_STACK     0x100

#define setcarry()     cpustatus |= FLAG_CARRY
#define clearcarry()   cpustatus &= (~FLAG_CARRY)
#define setzero()      cpustatus |= FLAG_ZERO
#define clearzero()    cpustatus &= (~FLAG_ZERO)
#define setinterrupt() cpustatus |= FLAG_INTERRUPT
#define clearinterrupt() cpustatus &= (~FLAG_INTERRUPT)
#define setdecimal()   cpustatus |= FLAG_DECIMAL
#define cleardecimal() cpustatus &= (~FLAG_DECIMAL)
#define setoverflow()  cpustatus |= FLAG_OVERFLOW
#define clearoverflow() cpustatus &= (~FLAG_OVERFLOW)
#define setsign()      cpustatus |= FLAG_SIGN
#define clearsign()    cpustatus &= (~FLAG_SIGN)

#define saveaccum(n)   a = ((uint8_t)(n) & 0x00FF)
#define zerocalc(n)    do { if ((n) & 0x00FF) clearzero(); else setzero(); } while(0)
#define signcalc(n)    do { if ((n) & 0x0080) setsign(); else clearsign(); } while(0)
#define carrycalc(n)   do { if ((n) & 0xFF00) setcarry(); else clearcarry(); } while(0)
#define overflowcalc(n,m,o) do { if (((n) ^ (m)) & ((n) ^ (o)) & 0x0080) setoverflow(); else clearoverflow(); } while(0)

/* 64KB RAM */
static uint8_t RAM[0x10000];

/* CPU 寄存器 */
static uint8_t  a = 0, x = 0, y = 0, sp = 0xFD, cpustatus = 0;
static uint16_t pc = 0, oldpc, ea, reladdr, value, result;
static uint8_t  opcode, useaccum;
static int32_t  clockticks6502 = 0;
static uint16_t temp16, startpage, eahelp, eahelp2;

/* SID 寄存器写入捕获 */
static uint8_t SID[32];
/* 捕获回调 (由 main 设置): 写 $D400-$D41F 时调用 */
static void (*sid_write_hook)(uint8_t reg, uint8_t val) = NULL;

/* VIC II 寄存器 (raster IRQ 用, RSID 必需).
 * 关键: $D011(bit7=raster hi), $D012(raster lo), $D019(IRQ状态/ack), $D01A(IRQ mask)
 * _y: 当前 raster 行 (PAL 0-311), 每周期 vicClock 推进. */
static uint8_t VIC_D011 = 0x1b;   /* screen ctrl, bit7 = raster bit8 */
static uint8_t VIC_D012 = 0;      /* raster compare lo */
static uint8_t VIC_D019 = 0;      /* IRQ status/ack (bit7=IRQ发生, bit0=raster命中) */
static uint8_t VIC_D01A = 0;      /* IRQ mask (bit0=raster IRQ 使能) */
static uint16_t VIC_y = 0;        /* 当前 raster 行 */
static uint16_t VIC_lines = 312;  /* PAL 312 行 */
static uint8_t VIC_cpr = 63;      /* PAL 63 周期/行 */

/* CIA timer (完整 timer A/B, 照 WebSid cia.c 核心逻辑, 覆盖 KERNAL CIA 场景).
 * CIA1 ($DC00-) timer A 是 C64 vsync/IRQ 主时钟 (KERNAL 用).
 * $DC04/05 = timer A latch lo/hi (写入设 latch, 启动时重装 counter)
 * $DC06/07 = timer B latch lo/hi
 * $DC0D = ICR: 读=中断状态(bit7=有IRQ, bit0=timerA, bit1=timerB) 读后清;
 *          写=设/清 mask (bit7=1设 bit7=0清, 低2位是 timer A/B 使能)
 * $DC0E = timer A ctrl (bit0=start, bit3=one-shot, bit4=force load)
 * $DC0F = timer B ctrl */
static uint16_t CIA_latchA = 0, CIA_latchB = 0;     /* latch (DC04/05, DC06/07) */
static uint16_t CIA_counterA = 0, CIA_counterB = 0; /* 当前计数 (递减) */
static uint8_t  CIA_startedA = 0, CIA_startedB = 0; /* DC0E/0F bit0 */
static uint8_t  CIA_icr_status = 0;   /* 中断状态 (读后清) */
static uint8_t  CIA_icr_mask = 0;     /* 中断 mask (DC0D 写入) */
static uint8_t  CIA_DC0E_reg = 0, CIA_DC0F_reg = 0; /* ctrl 寄存器原值 */
static uint8_t  CIA_force_load_A = 0, CIA_force_load_B = 0; /* force load 待处理 */

/* 硬件 IRQ 信号 (VIC 或 CIA 触发, 供 cpu 注入) */
static uint8_t hw_irq_pending = 0;

static inline uint8_t read6502(uint16_t address) {
    /* 标准 SID 寄存器 (0-0x1c) 读 SID 数组 */
    if (address >= 0xD400 && address < 0xD420) {
        return SID[address - 0xD400];
    }
    /* VIC II 寄存器 */
    if (address == 0xD011) return VIC_D011;
    if (address == 0xD012) return VIC_D012;
    if (address == 0xD019) return VIC_D019;
    if (address == 0xD01A) return VIC_D01A;
    /* CIA1 timer A/B counter (读返回当前计数, 对齐真实 CIA/WebSid) */
    if (address == 0xDC04) return CIA_counterA & 0xff;
    if (address == 0xDC05) return (CIA_counterA >> 8) & 0xff;
    if (address == 0xDC06) return CIA_counterB & 0xff;
    if (address == 0xDC07) return (CIA_counterB >> 8) & 0xff;
    if (address == 0xDC0D) {
        /* DC0D 读: 返回中断状态 (bit7=有IRQ, bit0=timerA, bit1=timerB), 读后清 */
        uint8_t st = CIA_icr_status;
        CIA_icr_status = 0;
        return st;
    }
    if (address == 0xDC0E) return CIA_DC0E_reg;
    if (address == 0xDC0F) return CIA_DC0F_reg;
    return RAM[address];
}

static inline void write6502(uint16_t address, uint8_t value) {
    /* 标准 SID 寄存器 + digi 地址 */
    if (address >= 0xD400 && address < 0xD500) {
        uint8_t reg = address - 0xD400;
        if (reg <= 0x1c) SID[reg] = value;
        if (sid_write_hook) sid_write_hook(reg, value);
        return;
    }
    /* VIC II 寄存器 */
    if (address == 0xD011) { VIC_D011 = value; return; }
    if (address == 0xD012) { VIC_D012 = value; return; }
    if (address == 0xD019) {
        /* $D019: bit0 写1清 raster latch, bit7 写1清 IRQ */
        if (value & 0x01) VIC_D019 &= ~0x81;   /* ack raster IRQ */
        return;
    }
    if (address == 0xD01A) { VIC_D01A = value; return; }
    /* CIA1 timer latch/ctrl (照 WebSid cia.c setTimerLatch/setControl) */
    if (address == 0xDC04) {
        CIA_latchA = (CIA_latchA & 0xff00) | value;
        if (!CIA_startedA) CIA_counterA = CIA_latchA;   /* stopped 时写 lo 也重装 */
        return;
    }
    if (address == 0xDC05) {
        CIA_latchA = (CIA_latchA & 0xff) | (value << 8);
        if (!CIA_startedA) CIA_counterA = CIA_latchA;   /* hi 写入重装 */
        return;
    }
    if (address == 0xDC06) { CIA_latchB = (CIA_latchB & 0xff00) | value; if (!CIA_startedB) CIA_counterB = CIA_latchB; return; }
    if (address == 0xDC07) { CIA_latchB = (CIA_latchB & 0xff) | (value << 8); if (!CIA_startedB) CIA_counterB = CIA_latchB; return; }
    if (address == 0xDC0D) {
        /* DC0D 写: bit7=1 设 mask, bit7=0 清 mask (对齐 WebSid setInterruptMask) */
        if (value & 0x80) CIA_icr_mask |= (value & 0x1f);
        else CIA_icr_mask &= ~(value & 0x1f);
        return;
    }
    if (address == 0xDC0E) {
        CIA_DC0E_reg = value;
        CIA_startedA = value & 0x01;
        if (value & 0x10) CIA_force_load_A = 1;   /* force load (bit4) */
        if ((value & 0x01) && CIA_counterA == 0) CIA_counterA = CIA_latchA;
        return;
    }
    if (address == 0xDC0F) {
        CIA_DC0F_reg = value;
        CIA_startedB = value & 0x01;
        if (value & 0x10) CIA_force_load_B = 1;
        if ((value & 0x01) && CIA_counterB == 0) CIA_counterB = CIA_latchB;
        return;
    }
    RAM[address] = value;
}

static inline void push16(uint16_t pushval) {
    write6502(BASE_STACK + sp, (pushval >> 8) & 0xFF);
    write6502(BASE_STACK + ((sp - 1) & 0xFF), pushval & 0xFF);
    sp -= 2;
}
static inline void push8(uint8_t pushval) {
    write6502(BASE_STACK + sp--, pushval);
}
static inline uint16_t pull16(void) {
    temp16 = read6502(BASE_STACK + ((sp + 1) & 0xFF)) | (read6502(BASE_STACK + ((sp + 2) & 0xFF)) << 8);
    sp += 2;
    return temp16;
}
static inline uint8_t pull8(void) {
    return read6502(BASE_STACK + ++sp);
}

/* addressing modes */
static inline void imp(void) {}
static inline void acc(void) { useaccum = 1; }
static inline void imm(void) { ea = pc++; }
static inline void zp(void)  { ea = read6502(pc++); }
static inline void zpx(void) { ea = (read6502(pc++) + x) & 0xFF; }
static inline void zpy(void) { ea = (read6502(pc++) + y) & 0xFF; }
static inline void rel(void) {
    reladdr = read6502(pc++);
    if (reladdr & 0x80) reladdr |= 0xFF00;
}
static inline void abso(void) {
    ea = read6502(pc) | (read6502(pc + 1) << 8);
    pc += 2;
}
static inline void absx(void) {
    ea = (read6502(pc) | (read6502(pc + 1) << 8));
    startpage = ea & 0xFF00;
    ea += x;
    pc += 2;
}
static inline void absy(void) {
    ea = (read6502(pc) | (read6502(pc + 1) << 8));
    startpage = ea & 0xFF00;
    ea += y;
    pc += 2;
}
static inline void ind(void) {
    eahelp = read6502(pc) | (read6502(pc + 1) << 8);
    eahelp2 = (eahelp & 0xFF00) | ((eahelp + 1) & 0x00FF);
    ea = read6502(eahelp) | (read6502(eahelp2) << 8);
    pc += 2;
}
static inline void indx(void) {
    eahelp = ((read6502(pc++) + x) & 0xFF);
    ea = read6502(eahelp & 0x00FF) | (read6502((eahelp + 1) & 0x00FF) << 8);
}
static inline void indy(void) {
    eahelp = read6502(pc++);
    eahelp2 = (eahelp & 0xFF00) | ((eahelp + 1) & 0x00FF);
    ea = read6502(eahelp) | (read6502(eahelp2) << 8);
    startpage = ea & 0xFF00;
    ea += y;
}

static inline uint16_t getvalue(void) {
    if (useaccum) return a;
    return read6502(ea);
}
static inline void putvalue(uint16_t saveval) {
    if (useaccum) a = (saveval & 0x00FF);
    else write6502(ea, (saveval & 0x00FF));
}

/* instructions */
static inline void adc(void) {
    value = getvalue();
    result = a + value + (cpustatus & FLAG_CARRY);
    carrycalc(result); zerocalc(result); overflowcalc(result, a, value); signcalc(result);
    saveaccum(result);
}
static inline void op_and(void) {
    value = getvalue(); result = a & value;
    zerocalc(result); signcalc(result); saveaccum(result);
}
static inline void asl(void) {
    value = getvalue(); result = value << 1;
    carrycalc(result); zerocalc(result); signcalc(result); putvalue(result);
}
static inline void bcc(void) {
    if ((cpustatus & FLAG_CARRY) == 0) {
        oldpc = pc; pc += reladdr;
        if ((oldpc & 0xFF00) != (pc & 0xFF00)) clockticks6502 += 2; else clockticks6502++;
    }
}
static inline void bcs(void) {
    if ((cpustatus & FLAG_CARRY) == FLAG_CARRY) {
        oldpc = pc; pc += reladdr;
        if ((oldpc & 0xFF00) != (pc & 0xFF00)) clockticks6502 += 2; else clockticks6502++;
    }
}
static inline void beq(void) {
    if ((cpustatus & FLAG_ZERO) == FLAG_ZERO) {
        oldpc = pc; pc += reladdr;
        if ((oldpc & 0xFF00) != (pc & 0xFF00)) clockticks6502 += 2; else clockticks6502++;
    }
}
static inline void op_bit(void) {
    value = getvalue(); result = a & value;
    zerocalc(result); cpustatus = (cpustatus & 0x3F) | (value & 0xC0);
}
static inline void bmi(void) {
    if ((cpustatus & FLAG_SIGN) == FLAG_SIGN) {
        oldpc = pc; pc += reladdr;
        if ((oldpc & 0xFF00) != (pc & 0xFF00)) clockticks6502 += 2; else clockticks6502++;
    }
}
static inline void bne(void) {
    if ((cpustatus & FLAG_ZERO) == 0) {
        oldpc = pc; pc += reladdr;
        if ((oldpc & 0xFF00) != (pc & 0xFF00)) clockticks6502 += 2; else clockticks6502++;
    }
}
static inline void bpl(void) {
    if ((cpustatus & FLAG_SIGN) == 0) {
        oldpc = pc; pc += reladdr;
        if ((oldpc & 0xFF00) != (pc & 0xFF00)) clockticks6502 += 2; else clockticks6502++;
    }
}
static inline void brk(void) {
    pc++;
    push16(pc);
    push8(cpustatus | FLAG_BREAK);
    setinterrupt();
    pc = read6502(0xFFFE) | (read6502(0xFFFF) << 8);
}

/* 硬件 IRQ (VIC raster / CIA timer 触发).
 * 对齐 WebSid cpu.c IRQ 流程: push pc+status, set I flag, 跳 $FFFE.
 * 区别 brk: 不 push BREAK flag (硬件 IRQ 不是 BRK). */
static inline void irq6502(void) {
    push16(pc);
    push8(cpustatus & ~FLAG_BREAK);   /* IRQ 不设 BREAK 位 */
    setinterrupt();
    pc = read6502(0xFFFE) | (read6502(0xFFFF) << 8);
    hw_irq_pending = 0;
}

/* VIC II raster 时钟: 每行推进 _y, 命中 raster_latch 时触发 IRQ.
 * 对齐 WebSid vic.c CHECK_IRQ (L131-147). PAL: 312 行/帧. */
static inline void vicClock(void) {
    VIC_y++;
    if (VIC_y >= VIC_lines) VIC_y = 0;
    /* raster_latch = D011 bit7 (raster bit8) << 8 | D012 */
    uint16_t raster_latch = ((VIC_D011 & 0x80) << 1) | VIC_D012;
    if (VIC_y == raster_latch) {
        VIC_D019 |= 0x01;   /* set raster latch bit */
        if (VIC_D01A & 0x01) {   /* raster IRQ 使能 */
            VIC_D019 |= 0x80;    /* signal VIC interrupt */
            hw_irq_pending = 1;
        }
    }
}

/* CIA timer 时钟: 每 CPU 周期调一次, timer A/B 递减, underflow 触发 ICR.
 * 照 WebSid cia.c clockT 核心 (去掉 scripted_transition 复杂状态机, KERNAL 场景够用).
 * underflow: counter→0 时重装 latch, 设 ICR 对应位, 若 mask 使能则触发 IRQ. */
static inline void ciaClock(void) {
    /* force load 处理 (DC0E/0F bit4 写入时设的标志) */
    if (CIA_force_load_A) { CIA_counterA = CIA_latchA; CIA_force_load_A = 0; }
    if (CIA_force_load_B) { CIA_counterB = CIA_latchB; CIA_force_load_B = 0; }

    /* timer A */
    if (CIA_startedA) {
        if (CIA_counterA == 0) {
            /* underflow: 重装 + 设 ICR bit0 */
            CIA_counterA = CIA_latchA;
            CIA_icr_status |= 0x01;   /* timer A 中断源 */
            if (CIA_icr_mask & 0x01) {
                CIA_icr_status |= 0x80;  /* 有 IRQ */
                hw_irq_pending = 1;
            }
            /* one-shot (bit3): underflow 后停止 */
            if (CIA_DC0E_reg & 0x08) { CIA_startedA = 0; CIA_DC0E_reg &= ~0x01; }
        } else {
            CIA_counterA--;
        }
    }
    /* timer B (同理, 简化无 link 模式) */
    if (CIA_startedB) {
        if (CIA_counterB == 0) {
            CIA_counterB = CIA_latchB;
            CIA_icr_status |= 0x02;   /* timer B 中断源 */
            if (CIA_icr_mask & 0x02) {
                CIA_icr_status |= 0x80;
                hw_irq_pending = 1;
            }
            if (CIA_DC0F_reg & 0x08) { CIA_startedB = 0; CIA_DC0F_reg &= ~0x01; }
        } else {
            CIA_counterB--;
        }
    }
}

/* 每 raster 行调一次 (63 周期): 推进 VIC + CIA, 检测 IRQ */
static inline void check_hardware_irq(void) {
    vicClock();
    ciaClock();
}
static inline void bvc(void) {
    if ((cpustatus & FLAG_OVERFLOW) == 0) {
        oldpc = pc; pc += reladdr;
        if ((oldpc & 0xFF00) != (pc & 0xFF00)) clockticks6502 += 2; else clockticks6502++;
    }
}
static inline void bvs(void) {
    if ((cpustatus & FLAG_OVERFLOW) == FLAG_OVERFLOW) {
        oldpc = pc; pc += reladdr;
        if ((oldpc & 0xFF00) != (pc & 0xFF00)) clockticks6502 += 2; else clockticks6502++;
    }
}
static inline void clc(void) { clearcarry(); }
static inline void cld(void) { cleardecimal(); }
static inline void cli(void) { clearinterrupt(); }
static inline void clv(void) { clearoverflow(); }
static inline void cmp(void) {
    value = getvalue(); result = a - value;
    if (a >= (value & 0x00FF)) setcarry(); else clearcarry();
    if (a == (value & 0x00FF)) setzero(); else clearzero();
    signcalc(result);
}
static inline void cpx(void) {
    value = getvalue(); result = x - value;
    if (x >= (value & 0x00FF)) setcarry(); else clearcarry();
    if (x == (value & 0x00FF)) setzero(); else clearzero();
    signcalc(result);
}
static inline void cpy(void) {
    value = getvalue(); result = y - value;
    if (y >= (value & 0x00FF)) setcarry(); else clearcarry();
    if (y == (value & 0x00FF)) setzero(); else clearzero();
    signcalc(result);
}
static inline void dec(void) {
    value = getvalue(); result = value - 1;
    zerocalc(result); signcalc(result); putvalue(result);
}
static inline void dex(void) { x--; zerocalc(x); signcalc(x); }
static inline void dey(void) { y--; zerocalc(y); signcalc(y); }
static inline void eor(void) {
    value = getvalue(); result = a ^ value;
    zerocalc(result); signcalc(result); saveaccum(result);
}
static inline void inc(void) {
    value = getvalue(); result = value + 1;
    zerocalc(result); signcalc(result); putvalue(result);
}
static inline void inx(void) { x++; zerocalc(x); signcalc(x); }
static inline void iny(void) { y++; zerocalc(y); signcalc(y); }
static inline void jmp(void) { pc = ea; }
static inline void jsr(void) { push16(pc - 1); pc = ea; }
static inline void lda(void) {
    value = getvalue(); a = (value & 0x00FF);
    zerocalc(a); signcalc(a);
}
static inline void ldx(void) {
    value = getvalue(); x = (value & 0x00FF);
    zerocalc(x); signcalc(x);
}
static inline void ldy(void) {
    value = getvalue(); y = (value & 0x00FF);
    zerocalc(y); signcalc(y);
}
static inline void lsr(void) {
    value = getvalue(); result = value >> 1;
    if (value & 1) setcarry(); else clearcarry();
    zerocalc(result); signcalc(result); putvalue(result);
}
static inline void nop(void) {}
static inline void ora(void) {
    value = getvalue(); result = a | value;
    zerocalc(result); signcalc(result); saveaccum(result);
}
static inline void pha(void) { push8(a); }
static inline void php(void) { push8(cpustatus | FLAG_BREAK); }
static inline void pla(void) { a = pull8(); zerocalc(a); signcalc(a); }
static inline void plp(void) { cpustatus = pull8() | FLAG_CONSTANT; }
static inline void rol(void) {
    value = getvalue(); result = (value << 1) | (cpustatus & FLAG_CARRY);
    carrycalc(result); zerocalc(result); signcalc(result); putvalue(result);
}
static inline void ror(void) {
    value = getvalue(); result = (value >> 1) | ((cpustatus & FLAG_CARRY) << 7);
    if (value & 1) setcarry(); else clearcarry();
    zerocalc(result); signcalc(result); putvalue(result);
}
static inline void rti(void) {
    cpustatus = pull8(); value = pull16(); pc = value;
}
static inline void rts(void) { value = pull16(); pc = value + 1; }
static inline void sbc(void) {
    value = getvalue() ^ 0x00FF;
    result = a + value + (cpustatus & FLAG_CARRY);
    carrycalc(result); zerocalc(result); overflowcalc(result, a, value); signcalc(result);
    saveaccum(result);
}
static inline void sec(void) { setcarry(); }
static inline void sed(void) { setdecimal(); }
static inline void sei(void) { setinterrupt(); }
static inline void sta(void) { putvalue(a); }
static inline void stx(void) { putvalue(x); }
static inline void sty(void) { putvalue(y); }
static inline void tax(void) { x = a; zerocalc(x); signcalc(x); }
static inline void tay(void) { y = a; zerocalc(y); signcalc(y); }
static inline void tsx(void) { x = sp; zerocalc(x); signcalc(x); }
static inline void txa(void) { a = x; zerocalc(a); signcalc(a); }
static inline void txs(void) { sp = x; }
static inline void tya(void) { a = y; zerocalc(a); signcalc(a); }

/* undocumented (当 nop, SID 不依赖) */
#define lax nop
#define sax nop
#define dcp nop
#define isb nop
#define slo nop
#define rla nop
#define sre nop
#define rra nop

static void exec6502(void) {
    opcode = read6502(pc++);
    cpustatus |= FLAG_CONSTANT;
    useaccum = 0;
    switch (opcode) {
    case 0x00: imp(); brk(); break;
    case 0x01: indx(); ora(); break;
    case 0x05: zp(); ora(); break;
    case 0x06: zp(); asl(); break;
    case 0x08: imp(); php(); break;
    case 0x09: imm(); ora(); break;
    case 0x0A: acc(); asl(); break;
    case 0x0D: abso(); ora(); break;
    case 0x0E: abso(); asl(); break;
    case 0x10: rel(); bpl(); break;
    case 0x11: indy(); ora(); break;
    case 0x15: zpx(); ora(); break;
    case 0x16: zpx(); asl(); break;
    case 0x18: imp(); clc(); break;
    case 0x19: absy(); ora(); break;
    case 0x1D: absx(); ora(); break;
    case 0x1E: absx(); asl(); break;
    case 0x20: abso(); jsr(); break;
    case 0x21: indx(); op_and(); break;
    case 0x24: zp(); op_bit(); break;
    case 0x25: zp(); op_and(); break;
    case 0x26: zp(); rol(); break;
    case 0x28: imp(); plp(); break;
    case 0x29: imm(); op_and(); break;
    case 0x2A: acc(); rol(); break;
    case 0x2C: abso(); op_bit(); break;
    case 0x2D: abso(); op_and(); break;
    case 0x2E: abso(); rol(); break;
    case 0x30: rel(); bmi(); break;
    case 0x31: indy(); op_and(); break;
    case 0x35: zpx(); op_and(); break;
    case 0x36: zpx(); rol(); break;
    case 0x38: imp(); sec(); break;
    case 0x39: absy(); op_and(); break;
    case 0x3D: absx(); op_and(); break;
    case 0x3E: absx(); rol(); break;
    case 0x40: imp(); rti(); break;
    case 0x41: indx(); eor(); break;
    case 0x45: zp(); eor(); break;
    case 0x46: zp(); lsr(); break;
    case 0x48: imp(); pha(); break;
    case 0x49: imm(); eor(); break;
    case 0x4A: acc(); lsr(); break;
    case 0x4C: abso(); jmp(); break;
    case 0x4D: abso(); eor(); break;
    case 0x4E: abso(); lsr(); break;
    case 0x50: rel(); bvc(); break;
    case 0x51: indy(); eor(); break;
    case 0x55: zpx(); eor(); break;
    case 0x56: zpx(); lsr(); break;
    case 0x58: imp(); cli(); break;
    case 0x59: absy(); eor(); break;
    case 0x5D: absx(); eor(); break;
    case 0x5E: absx(); lsr(); break;
    case 0x60: imp(); rts(); break;
    case 0x61: indx(); adc(); break;
    case 0x65: zp(); adc(); break;
    case 0x66: zp(); ror(); break;
    case 0x68: imp(); pla(); break;
    case 0x69: imm(); adc(); break;
    case 0x6A: acc(); ror(); break;
    case 0x6C: ind(); jmp(); break;
    case 0x6D: abso(); adc(); break;
    case 0x6E: abso(); ror(); break;
    case 0x70: rel(); bvs(); break;
    case 0x71: indy(); adc(); break;
    case 0x75: zpx(); adc(); break;
    case 0x76: zpx(); ror(); break;
    case 0x78: imp(); sei(); break;
    case 0x79: absy(); adc(); break;
    case 0x7D: absx(); adc(); break;
    case 0x7E: absx(); ror(); break;
    case 0x81: indx(); sta(); break;
    case 0x84: zp(); sty(); break;
    case 0x85: zp(); sta(); break;
    case 0x86: zp(); stx(); break;
    case 0x88: imp(); dey(); break;
    case 0x8A: imp(); txa(); break;
    case 0x8C: abso(); sty(); break;
    case 0x8D: abso(); sta(); break;
    case 0x8E: abso(); stx(); break;
    case 0x90: rel(); bcc(); break;
    case 0x91: indy(); sta(); break;
    case 0x94: zpx(); sty(); break;
    case 0x95: zpx(); sta(); break;
    case 0x96: zpy(); stx(); break;
    case 0x98: imp(); tya(); break;
    case 0x99: absy(); sta(); break;
    case 0x9A: imp(); txs(); break;
    case 0x9D: absx(); sta(); break;
    case 0xA0: imm(); ldy(); break;
    case 0xA1: indx(); lda(); break;
    case 0xA2: imm(); ldx(); break;
    case 0xA4: zp(); ldy(); break;
    case 0xA5: zp(); lda(); break;
    case 0xA6: zp(); ldx(); break;
    case 0xA8: imp(); tay(); break;
    case 0xA9: imm(); lda(); break;
    case 0xAA: imp(); tax(); break;
    case 0xAC: abso(); ldy(); break;
    case 0xAD: abso(); lda(); break;
    case 0xAE: abso(); ldx(); break;
    case 0xB0: rel(); bcs(); break;
    case 0xB1: indy(); lda(); break;
    case 0xB4: zpx(); ldy(); break;
    case 0xB5: zpx(); lda(); break;
    case 0xB6: zpy(); ldx(); break;
    case 0xB8: imp(); clv(); break;
    case 0xB9: absy(); lda(); break;
    case 0xBA: imp(); tsx(); break;
    case 0xBC: absx(); ldy(); break;
    case 0xBD: absx(); lda(); break;
    case 0xBE: absy(); ldx(); break;
    case 0xC0: imm(); cpy(); break;
    case 0xC1: indx(); cmp(); break;
    case 0xC4: zp(); cpy(); break;
    case 0xC5: zp(); cmp(); break;
    case 0xC6: zp(); dec(); break;
    case 0xC8: imp(); iny(); break;
    case 0xC9: imm(); cmp(); break;
    case 0xCA: imp(); dex(); break;
    case 0xCC: abso(); cpy(); break;
    case 0xCD: abso(); cmp(); break;
    case 0xCE: abso(); dec(); break;
    case 0xD0: rel(); bne(); break;
    case 0xD1: indy(); cmp(); break;
    case 0xD5: zpx(); cmp(); break;
    case 0xD6: zpx(); dec(); break;
    case 0xD8: imp(); cld(); break;
    case 0xD9: absy(); cmp(); break;
    case 0xDD: absx(); cmp(); break;
    case 0xDE: absx(); dec(); break;
    case 0xE0: imm(); cpx(); break;
    case 0xE1: indx(); sbc(); break;
    case 0xE4: zp(); cpx(); break;
    case 0xE5: zp(); sbc(); break;
    case 0xE6: zp(); inc(); break;
    case 0xE8: imp(); inx(); break;
    case 0xE9: imm(); sbc(); break;
    case 0xEB: imm(); sbc(); break;
    case 0xEC: abso(); cpx(); break;
    case 0xED: abso(); sbc(); break;
    case 0xEE: abso(); inc(); break;
    case 0xF0: rel(); beq(); break;
    case 0xF1: indy(); sbc(); break;
    case 0xF5: zpx(); sbc(); break;
    case 0xF6: zpx(); inc(); break;
    case 0xF8: imp(); sed(); break;
    case 0xF9: absy(); sbc(); break;
    case 0xFD: absx(); sbc(); break;
    case 0xFE: absx(); inc(); break;
    default: break;   /* 未定义 opcode 当 nop */
    }
}

/* ============================================================
 * PSID 解析 (对齐 sid_dump.py parse_psid)
 * ============================================================ */
typedef struct {
    char     magic[5];       /* "PSID"/"RSID" + null */
    uint16_t version;
    uint16_t header_len;
    uint16_t load_addr;      /* load_hdr 或 payload[0:2] 小端 */
    uint16_t init_addr;
    uint16_t play_addr;
    uint16_t n_songs;
    uint16_t start_song;     /* 1-based */
    uint32_t speed;          /* bit per song, 0=VSync */
    uint16_t flags;          /* v2+, bit2=PAL, bit3=NTSC */
    char     name[33];
    char     author[33];
    uint8_t *code;
    uint32_t code_size;
} PSID;

static uint16_t rd_be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t rd_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | (p[2] << 8) | p[3]; }

static int parse_psid(const char *path, PSID *si) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open sid"); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = (uint8_t*)malloc(sz);
    if (fread(raw, 1, sz, f) != (size_t)sz) { fclose(f); free(raw); return -1; }
    fclose(f);

    if (sz < 0x76 + 2) { free(raw); return -1; }
    memcpy(si->magic, raw, 4); si->magic[4] = 0;
    if (memcmp(si->magic, "PSID", 4) != 0 && memcmp(si->magic, "RSID", 4) != 0) {
        fprintf(stderr, "不是 PSID/RSID: %s\n", si->magic); free(raw); return -1;
    }
    si->version    = rd_be16(raw + 4);
    si->header_len = rd_be16(raw + 6);
    uint16_t load_hdr  = rd_be16(raw + 8);
    si->init_addr  = rd_be16(raw + 10);
    si->play_addr  = rd_be16(raw + 12);
    si->n_songs    = rd_be16(raw + 14);
    si->start_song = rd_be16(raw + 16);
    si->speed      = rd_be32(raw + 18);
    memcpy(si->name,   raw + 22, 32); si->name[32] = 0;
    memcpy(si->author, raw + 54, 32); si->author[32] = 0;
    si->flags = (si->version >= 2) ? rd_be16(raw + 0x76) : 0;

    /* payload */
    uint8_t *payload = raw + si->header_len;
    long payload_sz = sz - si->header_len;
    if (load_hdr == 0) {
        /* load addr 在 payload 头 2 字节, C64 小端 */
        si->load_addr = payload[0] | (payload[1] << 8);
        si->code = (uint8_t*)malloc(payload_sz - 2);
        memcpy(si->code, payload + 2, payload_sz - 2);
        si->code_size = payload_sz - 2;
    } else {
        si->load_addr = load_hdr;
        si->code = (uint8_t*)malloc(payload_sz);
        memcpy(si->code, payload, payload_sz);
        si->code_size = payload_sz;
    }
    if (si->init_addr == 0) si->init_addr = si->load_addr;
    free(raw);
    return 0;
}

/* ============================================================
 * 寄存器捕获缓冲 (动态数组)
 * ============================================================ */
/* 当前时间戳: 用 CPU 周期数 (亚帧精度, 支持 RSID PCM 帧内多次写入).
 * PAL CPU=985248Hz, 每帧~19656周期. PCM 在帧内密集写 $D418, 需周期级时序.
 * g_current_cycle = frame × cycles_per_frame + 帧内 clockticks6502 偏移 */
static int64_t g_current_cycle = 0;     /* 当前周期 (init=-1 哨兵, play>=0) */
static int64_t g_frame_base_cycle = 0;  /* 当前帧 play 开始时的 clockticks6502 */
static int32_t g_current_frame = -1;
static int32_t g_cycles_per_frame = 19656;  /* PAL: 985248/50.125, NTSC 在 main 里设 */

typedef struct { int64_t cycle; uint8_t reg, val; } CapEvent;
static CapEvent *cap_buf = NULL;
static size_t cap_n = 0, cap_cap = 0;

static void on_sid_write(uint8_t reg, uint8_t val) {
    if (cap_n >= cap_cap) {
        cap_cap = cap_cap ? cap_cap * 2 : 65536;
        cap_buf = (CapEvent*)realloc(cap_buf, cap_cap * sizeof(CapEvent));
    }
    /* Init writes all land at t=0. Without this they would carry the CPU
     * cycles the init routine burned (R-Type spends ~9 ms in init), which
     * sorts them *after* the first play frame even though they ran first. */
    cap_buf[cap_n].cycle = (g_current_frame < 0)
                           ? 0
                           : g_current_cycle + (int64_t)(clockticks6502 - g_frame_base_cycle);
    cap_buf[cap_n].reg   = reg;
    cap_buf[cap_n].val   = val;
    cap_n++;
}

/* ============================================================
 * 调 init / play (用 BRK stub 停止, 对齐 sid_dump.py)
 * stub 放 0xFF00 区 (避开代码区)
 * ============================================================ */
static void reset_cpu(void) {
    a = 0; x = 0; y = 0; sp = 0xFD; cpustatus = FLAG_CONSTANT;
    pc = 0;
}

/* 调 init/play: 照 SIDPod cpuJSR 方式 (push 假返回地址 0, pc>1 停止).
 * 不用 BRK (BRK 会跳 $FFFE, 破坏 init 安装的 IRQ handler).
 * JSR 到目标, 目标 RTS 时返回到假地址 0, pc=0<1 停止.
 * 关键: 不动 $FFFE, 让 init 自由安装 IRQ handler (RSID 需要). */
static void call_init(uint16_t init_addr, uint8_t song, uint32_t max_cycles) {
    reset_cpu();
    a = song;            /* SIDPod: na=song (init 用 A 传子曲目) */
    pc = init_addr;
    sp = 0xFF;
    push16(0);           /* 假返回地址 0, RTS 后 pc=0 */
    push16(0);           /* SIDPod push 两次 (额外保护) */
    uint32_t cy = 0;
    while (pc > 1 && cy < max_cycles) {
        exec6502();
        cy++;
    }
}

/* 调 play: 保留 init 后的 a/x/y/sp 状态 (play 可能依赖), 只重设 pc + push 假返回 */
static void call_play(uint16_t play_addr, uint32_t max_cycles) {
    pc = play_addr;
    sp = 0xFF;
    push16(0);
    push16(0);
    uint32_t cy = 0;
    while (pc > 1 && cy < max_cycles) {
        exec6502();
        cy++;
    }
}

/* play_addr == 0 means "there is no play routine entry point in the header".
 *
 * RSID: the init code installs its own handler on the hardware IRQ vector at
 * $FFFE, so that is where the per-frame music code lives.
 * PSID: the tune is driven from the KERNAL's software IRQ vector at $0314
 * instead, because on a real C64 the ROM IRQ handler dispatches through it.
 *
 * Which one applies is not recorded in the header, so both are checked: an
 * address below $0100 is the zero page and cannot be code, which is what an
 * unset vector looks like. Three files in sid/rsid hit the PSID case. */
static uint16_t get_play_addr(uint16_t play_addr_field) {
    if (play_addr_field != 0) return play_addr_field;

    uint16_t hw = (uint16_t)(RAM[0xFFFE] | (RAM[0xFFFF] << 8));
    if (hw >= 0x0100) return hw;

    uint16_t kernal = (uint16_t)(RAM[0x0314] | (RAM[0x0315] << 8));
    if (kernal >= 0x0100) return kernal;

    return 0;
}

/* ============================================================
 * machine reset
 *
 * The extractor was a one-shot tool, so every global could start at its
 * initialiser and never be touched again. As a library it runs repeatedly in
 * one process (playlist, subsong browsing), and leftover CIA latches or a
 * half-written RAM would silently corrupt the next song. Everything the 6502
 * can observe is therefore reset here, not just the capture buffer.
 * ============================================================ */
static void reset_machine(void)
{
    memset(RAM, 0, sizeof(RAM));
    memset(SID, 0, sizeof(SID));

    a = 0; x = 0; y = 0; sp = 0xFD; cpustatus = FLAG_CONSTANT;
    pc = 0; oldpc = 0; ea = 0; reladdr = 0; value = 0; result = 0;
    opcode = 0; useaccum = 0;
    clockticks6502 = 0;
    temp16 = 0; startpage = 0; eahelp = 0; eahelp2 = 0;

    sid_write_hook = NULL;
    hw_irq_pending = 0;

    VIC_D011 = 0x1b; VIC_D012 = 0; VIC_D019 = 0; VIC_D01A = 0;
    VIC_y = 0;
    VIC_lines = 312; VIC_cpr = 63;

    CIA_latchA = CIA_latchB = 0;
    CIA_counterA = CIA_counterB = 0;
    CIA_startedA = CIA_startedB = 0;
    CIA_icr_status = 0; CIA_icr_mask = 0;
    CIA_DC0E_reg = CIA_DC0F_reg = 0;
    CIA_force_load_A = CIA_force_load_B = 0;

    g_current_cycle = -1;
    g_frame_base_cycle = 0;
    g_current_frame = -1;
    g_cycles_per_frame = 19656;

    cap_n = 0;
}

/* ============================================================
 * public: header probe
 * ============================================================ */
static void copy_meta(char *dst, const uint8_t *src, size_t len)
{
    size_t i;
    for (i = 0; i + 1 < len; i++) dst[i] = (char)src[i];
    dst[i] = 0;
    /* Trailing spaces and NULs are normal in PSID headers. */
    while (i && (dst[i - 1] == ' ' || dst[i - 1] == 0)) dst[--i] = 0;
}

int sid_song_probe(const char *path, sid_song_info_t *info)
{
    PSID si;
    if (parse_psid(path, &si) < 0) return -1;

    memset(info, 0, sizeof(*info));
    copy_meta(info->name, (const uint8_t *)si.name, sizeof(info->name));
    copy_meta(info->author, (const uint8_t *)si.author, sizeof(info->author));
    info->is_rsid    = (memcmp(si.magic, "RSID", 4) == 0);
    info->load_addr  = si.load_addr;
    info->init_addr  = si.init_addr;
    info->play_addr  = si.play_addr;
    info->n_songs    = si.n_songs ? si.n_songs : 1;
    info->start_song = si.start_song ? si.start_song : 1;
    info->speed      = si.speed;
    info->flags      = si.flags;

    uint32_t clk = si.flags & 0x0C;
    if (clk == 0x08) {
        info->pal = 0; info->vsync_hz = 60.0;    info->clock_hz = 1022730.0;
    } else {
        info->pal = (clk != 0x0C); info->vsync_hz = 50.125; info->clock_hz = 985248.0;
    }

    free(si.code);
    return 0;
}

/* ============================================================
 * public: run the song, capture the register stream
 * ============================================================ */
int sid_song_extract(const char *path, int subsong, double seconds,
                     sid_event_t **events, size_t *count,
                     sid_song_info_t *info, char *err, size_t errlen)
{
    if (events) *events = NULL;
    if (count)  *count = 0;
    if (err && errlen) err[0] = 0;

    PSID si;
    if (parse_psid(path, &si) < 0) {
        if (err && errlen) snprintf(err, errlen, "not a PSID/RSID file");
        return -1;
    }

    sid_song_info_t local;
    if (!info) info = &local;
    if (sid_song_probe(path, info) < 0) {
        free(si.code);
        if (err && errlen) snprintf(err, errlen, "header unreadable");
        return -1;
    }

    if (subsong < 0) subsong = si.start_song ? si.start_song - 1 : 0;

    reset_machine();

    uint32_t clk = si.flags & 0x0C;
    double vsync_hz, cpu_hz;
    if (clk == 0x08) { vsync_hz = 60.0; cpu_hz = 1022730.0; }
    else            { vsync_hz = 50.125; cpu_hz = 985248.0; }
    g_cycles_per_frame = (int32_t)(cpu_hz / vsync_hz);
    if (clk == 0x08) { VIC_lines = 263; VIC_cpr = 65; }
    else            { VIC_lines = 312; VIC_cpr = 63; }

    for (uint32_t i = 0; i < si.code_size; i++)
        RAM[(si.load_addr + i) & 0xFFFF] = si.code[i];

    /* Every write made by the init routine lands at t=0: the player has to
     * apply the whole init state before the first play frame, and the SID
     * cannot tell whether those writes were 3 us or 3 ms apart. */
    sid_write_hook = on_sid_write;
    g_current_frame = -1;
    g_current_cycle = 0;
    g_frame_base_cycle = clockticks6502;
    call_init(si.init_addr, (uint8_t)subsong, 2000000);

    int32_t n_frames = (int32_t)(seconds * vsync_hz);
    if (n_frames < 0) n_frames = 0;
    uint16_t play_addr = get_play_addr(si.play_addr);
    if (play_addr == 0) n_frames = 0;

    for (int32_t fr = 0; fr < n_frames; fr++) {
        g_current_frame = fr;
        g_current_cycle = (int64_t)fr * (int64_t)g_cycles_per_frame;
        g_frame_base_cycle = clockticks6502;
        call_play(play_addr, 1000000);
    }

    sid_write_hook = NULL;

    /* Hand the buffer over, then convert cycle counts to milliseconds. */
    size_t n = cap_n;
    sid_event_t *out = NULL;
    if (n) {
        out = (sid_event_t *)malloc(n * sizeof(*out));
        if (!out) {
            free(si.code);
            reset_machine();
            if (err && errlen) snprintf(err, errlen, "out of memory");
            return -1;
        }
        for (size_t i = 0; i < n; i++) {
            int64_t c = cap_buf[i].cycle;
            out[i].t_ms = (c < 0) ? 0.0 : (double)c / cpu_hz * 1000.0;
            out[i].reg  = cap_buf[i].reg;
            out[i].val  = cap_buf[i].val;
        }
    }

    if (events) *events = out;
    else        free(out);
    if (count) *count = n;

    free(si.code);
    reset_machine();
    return 0;
}

void sid_song_free(sid_event_t *events)
{
    free(events);
}

/* ============================================================
 * public: live streaming
 *
 * A .sid header has no length: the play routine is designed to be called
 * forever and loops inside itself, so playing "until the song ends" has no
 * meaning. Rendering a fixed buffer up front therefore forces an arbitrary
 * duration on the user, which is exactly what we do not want.
 *
 * Instead the machine is left running and advanced one frame per call. The
 * caller keeps the pacing, so the emulation cost is at most one frame's worth
 * of 6502 (typically tens of microseconds) hidden inside the 20 ms the
 * previous frame takes to play out. There is no stall and no precomputed end.
 * ============================================================ */

static int       s_streaming = 0;
static uint16_t  s_stream_play_addr = 0;
static double    s_stream_cpu_hz = 985248.0;
static int32_t   s_stream_frame = 0;
static sid_event_t *s_frame_ev = NULL;
static size_t      s_frame_cap = 0;

int sid_stream_open(const char *path, int subsong, sid_song_info_t *info,
                    char *err, size_t errlen)
{
    sid_stream_close();
    if (err && errlen) err[0] = 0;

    PSID si;
    if (parse_psid(path, &si) < 0) {
        if (err && errlen) snprintf(err, errlen, "not a PSID/RSID file");
        return -1;
    }

    sid_song_info_t local;
    if (!info) info = &local;
    if (sid_song_probe(path, info) < 0) {
        free(si.code);
        if (err && errlen) snprintf(err, errlen, "header unreadable");
        return -1;
    }

    if (subsong < 0) subsong = si.start_song ? si.start_song - 1 : 0;

    reset_machine();

    uint32_t clk = si.flags & 0x0C;
    double vsync_hz, cpu_hz;
    if (clk == 0x08) { vsync_hz = 60.0; cpu_hz = 1022730.0; VIC_lines = 263; VIC_cpr = 65; }
    else            { vsync_hz = 50.125; cpu_hz = 985248.0; VIC_lines = 312; VIC_cpr = 63; }

    s_stream_cpu_hz = cpu_hz;
    g_cycles_per_frame = (int32_t)(cpu_hz / vsync_hz);

    for (uint32_t i = 0; i < si.code_size; i++)
        RAM[(si.load_addr + i) & 0xFFFF] = si.code[i];

    /* The init routine's writes all land at t=0; they must be applied before
     * the first play frame. */
    sid_write_hook = on_sid_write;
    g_current_frame = -1;
    g_current_cycle = 0;
    g_frame_base_cycle = clockticks6502;
    call_init(si.init_addr, (uint8_t)subsong, 2000000);

    /* The init writes are handed out as the first frame, so the caller gets
     * them before it starts counting time. */
    s_stream_play_addr = get_play_addr(si.play_addr);
    s_stream_frame = 0;
    s_streaming = 1;

    free(si.code);
    return 0;
}

int sid_stream_next_frame(const sid_event_t **events, size_t *count)
{
    if (!s_streaming) return -1;

    if (g_current_frame < 0) {
        /* First call: hand over the init writes captured by sid_stream_open. */
        g_current_frame = 0;
    } else {
        if (s_stream_play_addr != 0) {
            g_current_cycle = (int64_t)s_stream_frame * (int64_t)g_cycles_per_frame;
            g_frame_base_cycle = clockticks6502;
            call_play(s_stream_play_addr, 1000000);
        }
        s_stream_frame++;
    }

    /* Convert the accumulated cycles to milliseconds into a stable buffer.
     * The machine's cycle counter keeps growing across frames, so t_ms stays
     * monotonic for as long as the stream runs - there is no wrap and no
     * restart at an arbitrary length. */
    size_t n = cap_n;
    if (n > s_frame_cap) {
        size_t want = n;
        sid_event_t *np = (sid_event_t *)realloc(s_frame_ev, want * sizeof(*np));
        if (!np) { cap_n = 0; return -1; }
        s_frame_ev = np;
        s_frame_cap = want;
    }
    for (size_t i = 0; i < n; i++) {
        int64_t c = cap_buf[i].cycle;
        s_frame_ev[i].t_ms = (c < 0) ? 0.0
                            : (double)c / s_stream_cpu_hz * 1000.0;
        s_frame_ev[i].reg = cap_buf[i].reg;
        s_frame_ev[i].val = cap_buf[i].val;
    }
    cap_n = 0;

    if (events) *events = s_frame_ev;
    if (count)  *count = n;
    return 0;
}

double sid_stream_frame_ms(void)
{
    if (g_cycles_per_frame <= 0) return 19.96;
    return (double)g_cycles_per_frame / s_stream_cpu_hz * 1000.0;
}

void sid_stream_close(void)
{
    if (!s_streaming) return;
    sid_write_hook = NULL;
    reset_machine();
    s_streaming = 0;
    s_stream_play_addr = 0;
    s_stream_frame = 0;
}


/* ============================================================
 * .sidreg text cache
 * ============================================================ */
int sid_sidreg_write(const char *path, const sid_event_t *events, size_t count)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (size_t i = 0; i < count; i++)
        fprintf(f, "%.4f %02x %02x\n", events[i].t_ms,
                events[i].reg, events[i].val);
    if (fclose(f) != 0) return -1;
    return 0;
}

/* Grow-on-demand reader: a playlist reuses the buffer between songs, so this
 * stays allocation-free after the first file. */
static sid_event_t *s_read_buf = NULL;
static size_t       s_read_cap = 0;

int sid_sidreg_read(const char *path, sid_event_t **events, size_t *count)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    size_t n = 0;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;

        double t;
        unsigned reg, val;
        if (sscanf(p, "%lf %x %x", &t, &reg, &val) == 3) {
            if (n == s_read_cap) {
                size_t want = s_read_cap ? s_read_cap * 2 : 4096;
                sid_event_t *np = (sid_event_t *)realloc(s_read_buf,
                                                         want * sizeof(*np));
                if (!np) { fclose(f); return -1; }
                s_read_buf = np;
                s_read_cap = want;
            }
            s_read_buf[n].t_ms = t;
            s_read_buf[n].reg  = (uint8_t)reg;
            s_read_buf[n].val  = (uint8_t)val;
            n++;
        }
    }
    fclose(f);

    if (events) *events = s_read_buf;
    if (count)  *count  = n;
    return 0;
}

int sid_sidreg_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int sid_sidreg_cache_name(char *buf, size_t len, const char *cache_dir,
                          const char *sid_path, int subsong, double seconds)
{
    const char *base = sid_path;
    for (const char *p = sid_path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;

    char stem[256];
    size_t k = 0;
    while (base[k] && base[k] != '.' && k < sizeof(stem) - 1) {
        stem[k] = base[k];
        k++;
    }
    stem[k] = 0;

    int r = snprintf(buf, len, "%s/%s_s%d_d%d.sidreg", cache_dir, stem,
                     subsong, (int)seconds);
    return (r < 0 || (size_t)r >= len) ? -1 : 0;
}

