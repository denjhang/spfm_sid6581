/* sid_rsid.c - PSID/RSID -> SID register stream, in plain C.
 *
 * A .sid file is 6502 code; the music only exists as writes to $D400-$D41F.
 * This file emulates the CPU (fake6502, from rubbermallet.org via the
 * ChristmasSID sketch), the VIC raster counter, both CIA timers, and the
 * NMI/IRQ lines those timers drive. It runs init once and then advances the
 * machine a fixed number of CPU cycles per tick - the video frame for a normal
 * tune, the CIA timer period for a CIA-timed subsong - recording every SID
 * write with a timestamp.
 *
 * Why cycles and not just "call play once per frame": the frame is only the
 * pacing unit. Most tunes do all their work inside play(), but the classic
 * "volume-register digi" tracks (The Great Giana Sisters) put their sample
 * player in an NMI handler fired by CIA2 timer A thousands of times per
 * second. Nothing inside play() would ever reach that code, so the loop has to
 * keep the clocks running and take interrupts at instruction boundaries, with
 * the CPU idling at IDLE_PC once play() has RTS'd.
 *
 * Where it came from: the 6502/CIA/VIC part is copied from the STC32G
 * chiptune project (usb_cdc_test/tools/sid_extract.c) so both projects agree
 * on what a .sid sounds like. Only the entry points, the machine reset, the
 * interrupt plumbing and the cycle accounting changed: it is now a library
 * that the player links against, not a stand-alone tool that writes a file.
 *
 * Timestamps: init-phase writes are all pinned to t=0 (they must be applied
 * before the first play tick), play-tick writes use
 * tick*cycles_per_tick + intra-tick cycle, so PCM/digi songs keep their
 * sub-tick ordering.
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
/* Set by absx/absy/indy, consumed by getvalue(): a read that carries into the
 * next page costs one extra cycle, a store never does. */
static uint8_t  s_page_crossed = 0;
static uint8_t  s_page_extra = 0;
/* Cycle cost of a taken branch (1) or a taken branch that crosses a page (2).
 * The PSID model stamps writes from branch penalties alone - that is all the
 * original code ever accumulated - so it needs its own copy of this number. */
static uint8_t  s_branch_ticks = 0;

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

/* CIA1 ($DC00) and CIA2 ($DD00).
 *
 * Both chips share one register map and both are needed. CIA1 timer A is the
 * usual IRQ/vsync clock; CIA2 timer A is what the classic "volume-register
 * digi" tunes use to fire an NMI thousands of times per second (The Great
 * Giana Sisters is the textbook case - see docs/驱动说明.md §6.8).
 *
 * Modelled after WebSid cia.c: a counter decrements once per CPU cycle, an
 * underflow reloads it from the latch and sets the matching ICR source bit.
 *
 *   $xx04/05 timer A latch lo/hi     $xx06/07 timer B latch lo/hi
 *   $xx0D    ICR: read = status (bit7 asserting, bit0 A, bit1 B), clears on
 *                 read; write = set/clear mask, bit7 of the byte selects
 *   $xx0E    timer A control (bit0 start, bit3 one-shot, bit4 force load)
 *   $xx0F    timer B control
 *
 * On the C64 CIA1's ICR bit7 drives /IRQ and CIA2's drives /NMI, which is why
 * the same code path serves both: only the destination signal differs.
 */
typedef struct {
    uint16_t latchA, latchB;
    /* Remaining cycles until underflow, i.e. the latch value plus one. The
     * +1 is not cosmetic: on a real 6526 a latch of 0 means 65536 cycles, not
     * "fire immediately". Getting that wrong turns a latch-0 timer into a
     * per-cycle interrupt storm that starves the play routine completely
     * (seen as 842000 NMIs in 6 s and zero register writes). */
    uint32_t counterA, counterB;
    uint8_t  startedA, startedB;
    uint8_t  icr_status;      /* bit0 timer A, bit1 timer B, bit7 asserting */
    uint8_t  icr_mask;
    uint8_t  ctrlA, ctrlB;    /* raw $xx0E / $xx0F */
    uint8_t  force_loadA, force_loadB;
} cia_t;

static cia_t cia1, cia2;

/* /IRQ can be pulled by CIA1 or the VIC raster; /NMI only by CIA2. */
static uint8_t hw_irq_pending = 0;
static uint8_t hw_nmi_pending = 0;

/* PSID model by default: play() once per frame, nothing else runs.
 *
 * The RSID model (full clock + interrupts for the whole tick) is what digi
 * tracks need, but it multiplies the write rate by two to three orders of
 * magnitude - Giana's drums alone are ~6800 $D418 writes per second, where a
 * normal tune has ~50 register writes per 20 ms frame. Neither 1.5 Mbaud nor
 * the FTDI latency timer can carry that in real time, so making it the default
 * would trade one working class of tunes for another. Opt in with --rsid. */
static int s_rsid_mode = 0;

void sid_set_rsid_mode(int on) { s_rsid_mode = on ? 1 : 0; }
int  sid_get_rsid_mode(void)   { return s_rsid_mode; }

/* Diagnostics: how many interrupts were actually taken. A digi tune that plays
 * silence shows zero NMI here, which distinguishes "the timer never fired"
 * from "the handler ran but wrote nothing useful". */
unsigned long sidr_dbg_nmi_taken = 0;
unsigned long sidr_dbg_nmi_ticks = 0;
unsigned long sidr_dbg_irq_taken = 0;

static uint16_t s_dbg_nmi_vector = 0, s_dbg_irq_vector = 0;
static uint16_t s_dbg_cia1_latchA = 0, s_dbg_cia2_latchA = 0;
static uint8_t  s_dbg_cia1_ctrlA = 0, s_dbg_cia2_ctrlA = 0, s_dbg_cia2_started = 0;

/* 6526 counts `latch + 1` cycles before underflow, and a latch of 0 is the
 * longest setting (65536), not the shortest. A 0 latch treated as 1 cycle
 * turns the timer into a per-cycle NMI storm: Giana Sisters' title subsong
 * produced 561000 NMIs in 4 s and zero register writes that way. */
static inline uint32_t cia_period(uint16_t latch)
{
    return latch ? (uint32_t)latch + 1u : 65536u;
}

static inline uint8_t cia_read(cia_t *c, uint8_t off){
    switch (off) {
    case 0x04: return (uint8_t)(c->counterA & 0xff);
    case 0x05: return (uint8_t)((c->counterA >> 8) & 0xff);
    case 0x06: return (uint8_t)(c->counterB & 0xff);
    case 0x07: return (uint8_t)((c->counterB >> 8) & 0xff);
    case 0x0D: {
        uint8_t st = c->icr_status;
        c->icr_status = 0;          /* reading the ICR clears every source bit */
        return st;
    }
    case 0x0E: return c->ctrlA;
    case 0x0F: return c->ctrlB;
    default:   return 0;
    }
}

static inline void cia_write(cia_t *c, uint8_t off, uint8_t v)
{
    switch (off) {
    case 0x04:
        c->latchA = (uint16_t)((c->latchA & 0xff00) | v);
        if (!c->startedA) c->counterA = cia_period(c->latchA);
        break;
    case 0x05:
        c->latchA = (uint16_t)((c->latchA & 0x00ff) | (v << 8));
        if (!c->startedA) c->counterA = cia_period(c->latchA);
        break;
    case 0x06:
        c->latchB = (uint16_t)((c->latchB & 0xff00) | v);
        if (!c->startedB) c->counterB = cia_period(c->latchB);
        break;
    case 0x07:
        c->latchB = (uint16_t)((c->latchB & 0x00ff) | (v << 8));
        if (!c->startedB) c->counterB = cia_period(c->latchB);
        break;
    case 0x0D:
        if (v & 0x80) c->icr_mask |= (uint8_t)(v & 0x1f);
        else          c->icr_mask &= (uint8_t)~(v & 0x1f);
        break;
    case 0x0E:
        c->ctrlA = v;
        c->startedA = v & 0x01;
        if (v & 0x10) c->force_loadA = 1;
        if ((v & 0x01) && c->counterA == 0) c->counterA = cia_period(c->latchA);
        break;
    case 0x0F:
        c->ctrlB = v;
        c->startedB = v & 0x01;
        if (v & 0x10) c->force_loadB = 1;
        if ((v & 0x01) && c->counterB == 0) c->counterB = cia_period(c->latchB);
        break;
    default:
        break;
    }
}

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
    /* CIA1 ($DC00) / CIA2 ($DD00) */
    if (address >= 0xDC00 && address <= 0xDC0F) return cia_read(&cia1, (uint8_t)(address & 0x0F));
    if (address >= 0xDD00 && address <= 0xDD0F) return cia_read(&cia2, (uint8_t)(address & 0x0F));
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
    /* CIA1 ($DC00) / CIA2 ($DD00) */
    if (address >= 0xDC00 && address <= 0xDC0F) { cia_write(&cia1, (uint8_t)(address & 0x0F), value); return; }
    if (address >= 0xDD00 && address <= 0xDD0F) { cia_write(&cia2, (uint8_t)(address & 0x0F), value); return; }
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
    s_page_crossed = (startpage != (ea & 0xFF00));
    pc += 2;
}
static inline void absy(void) {
    ea = (read6502(pc) | (read6502(pc + 1) << 8));
    startpage = ea & 0xFF00;
    ea += y;
    s_page_crossed = (startpage != (ea & 0xFF00));
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
    s_page_crossed = (startpage != (ea & 0xFF00));
}

static inline uint16_t getvalue(void) {
    if (useaccum) return a;
    /* Absolute,X / Absolute,Y / (Ind),Y reads pay one extra cycle when the
     * index carries into the next page. Stores never do, which is why this
     * lives in the read path and not in absx()/absy()/indy(). */
    if (s_page_crossed) s_page_extra = 1;
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
        s_branch_ticks = ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1;
    }
}
static inline void bcs(void) {
    if ((cpustatus & FLAG_CARRY) == FLAG_CARRY) {
        oldpc = pc; pc += reladdr;
        s_branch_ticks = ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1;
    }
}
static inline void beq(void) {
    if ((cpustatus & FLAG_ZERO) == FLAG_ZERO) {
        oldpc = pc; pc += reladdr;
        s_branch_ticks = ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1;
    }
}
static inline void op_bit(void) {
    value = getvalue(); result = a & value;
    zerocalc(result); cpustatus = (cpustatus & 0x3F) | (value & 0xC0);
}
static inline void bmi(void) {
    if ((cpustatus & FLAG_SIGN) == FLAG_SIGN) {
        oldpc = pc; pc += reladdr;
        s_branch_ticks = ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1;
    }
}
static inline void bne(void) {
    if ((cpustatus & FLAG_ZERO) == 0) {
        oldpc = pc; pc += reladdr;
        s_branch_ticks = ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1;
    }
}
static inline void bpl(void) {
    if ((cpustatus & FLAG_SIGN) == 0) {
        oldpc = pc; pc += reladdr;
        s_branch_ticks = ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1;
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

/* 硬件 NMI (CIA2 timer A/B 的 ICR bit7 驱动). 真机 6502 上 NMI 不可屏蔽,
 * 优先级高于 IRQ, 向量在 $FFFA/$FFFB。Giana Sisters 这类 digi 鼓靠它每毫秒
 * 进一次中断, 往 $D418 丢一个 4 bit 采样值。 */

/* NMI 向量: 真机取 $FFFA/$FFFB. 我们没有 KERNAL ROM, 而这类曲子几乎都在 init
 * 里把 $0318/$0319 写成自己的处理程序(C64 上 $FFFA 的 ROM 内容就是跳向 $0318),
 * 所以先看 $0318/$0319, 再退到 $FFFA/$FFFB。零页地址视为未设。 */
static inline uint16_t nmi_vector(void) {
    uint16_t v = (uint16_t)(RAM[0x0318] | (RAM[0x0319] << 8));
    if (v >= 0x0100) return v;
    v = (uint16_t)(RAM[0xFFFA] | (RAM[0xFFFB] << 8));
    if (v >= 0x0100) return v;
    return 0;
}

static inline void nmi_jump(void) {
    push16(pc);
    push8(cpustatus & ~FLAG_BREAK);
    setinterrupt();
    pc = nmi_vector();
    hw_nmi_pending = 0;
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

/* 一块 CIA 的每周期推进。sources: ICR 里能出现的源位 (CIA1 = A+B, CIA2 = A+B)。
 * `assert_irq` / `assert_nmi` 决定 underflow 且 mask 打开时拉哪根线:
 * C64 上 CIA1 的 ICR bit7 拉 /IRQ, CIA2 的拉 /NMI. */
static inline void cia_clock(cia_t *c, int assert_irq, int assert_nmi)
{
    if (c->force_loadA) { c->counterA = cia_period(c->latchA); c->force_loadA = 0; }
    if (c->force_loadB) { c->counterB = cia_period(c->latchB); c->force_loadB = 0; }

    if (c->startedA) {
        if (--c->counterA == 0) {
            c->counterA = cia_period(c->latchA);
            c->icr_status |= 0x01;
            if (c->icr_mask & 0x01) {
                c->icr_status |= 0x80;
                if (assert_nmi) hw_nmi_pending = 1;
                else if (assert_irq) hw_irq_pending = 1;
            }
            if (c->ctrlA & 0x08) { c->startedA = 0; c->ctrlA &= ~0x01; }
        }
    }
    if (c->startedB) {
        if (--c->counterB == 0) {
            c->counterB = cia_period(c->latchB);
            c->icr_status |= 0x02;
            if (c->icr_mask & 0x02) {
                c->icr_status |= 0x80;
                if (assert_nmi) hw_nmi_pending = 1;
                else if (assert_irq) hw_irq_pending = 1;
            }
            if (c->ctrlB & 0x08) { c->startedB = 0; c->ctrlB &= ~0x01; }
        }
    }
}

/* 每 CPU 周期调一次: VIC 每 VIC_cpr 个周期推进一行, 两个 CIA 各递减一次。 */
static uint8_t s_cyc_in_line = 0;
static inline void check_hardware_irq(void) {
    if (++s_cyc_in_line >= VIC_cpr) {
        s_cyc_in_line = 0;
        vicClock();
    }
    cia_clock(&cia1, 1, 0);   /* CIA1 -> /IRQ */
    cia_clock(&cia2, 0, 1);   /* CIA2 -> /NMI */
}
static inline void bvc(void) {
    if ((cpustatus & FLAG_OVERFLOW) == 0) {
        oldpc = pc; pc += reladdr;
        s_branch_ticks = ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1;
    }
}
static inline void bvs(void) {
    if ((cpustatus & FLAG_OVERFLOW) == FLAG_OVERFLOW) {
        oldpc = pc; pc += reladdr;
        s_branch_ticks = ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1;
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

/* Base cycle count for each opcode (NMOS 6502). Taken branches add one cycle,
 * a taken branch or an indexed read that crosses a page adds another; both are
 * handled outside the table. Undefined opcodes are treated as 2-cycle NOPs,
 * matching the switch statement's default. */
static const uint8_t op_cycles[256] = {
/*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/* 0 */  7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
/* 1 */  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/* 2 */  6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
/* 3 */  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/* 4 */  6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
/* 5 */  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/* 6 */  6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
/* 7 */  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/* 8 */  2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
/* 9 */  2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
/* A */  2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
/* B */  2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
/* C */  2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
/* D */  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/* E */  2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
/* F */  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7
};

/* Execute one instruction and return how many CPU cycles it took. */
static int exec6502(void) {
    opcode = read6502(pc++);
    cpustatus |= FLAG_CONSTANT;
    useaccum = 0;
    s_page_crossed = 0;
    s_page_extra = 0;
    s_branch_ticks = 0;
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
    return op_cycles[opcode] + s_page_extra;
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
/* IRQ 向量: 真机 $FFFE/$FFFF 指向 KERNAL 的 IRQ 入口, KERNAL 再经 $0314 分发。
 * 我们没有 KERNAL, 所以 $FFFE 没被写(零页)时就退到 $0314 —— 很多 PSID 只写
 * $0314。两者都没有就没有 IRQ 可用。 */
static inline uint16_t irq_vector(void) {
    uint16_t v = (uint16_t)(RAM[0xFFFE] | (RAM[0xFFFF] << 8));
    if (v >= 0x0100) return v;
    v = (uint16_t)(RAM[0x0314] | (RAM[0x0315] << 8));
    if (v >= 0x0100) return v;
    return 0;
}

static inline void irq_jump(void) {
    push16(pc);
    push8(cpustatus & ~FLAG_BREAK);
    setinterrupt();
    pc = irq_vector();
    hw_irq_pending = 0;
}

/* Where the CPU sits once the frame's play routine has returned. Zero page is
 * never executable code, so any pc below $0100 is a safe marker: run_machine()
 * recognises it and just advances the clocks instead of fetching an opcode. */
#define IDLE_PC 0x0002

/* 中断响应本身 7 个周期, 和真 6502 一致 (push3 + 取向量)。 */
#define IRQ_CYCLES 7

/* 指令边界上响应中断: 先 NMI 后 IRQ, 和真机一致。响应本身 7 个周期。 */
static int service_interrupt(void)
{
    if (hw_nmi_pending && nmi_vector() >= 0x0100) {
        sidr_dbg_nmi_taken++;
        nmi_jump();
        return IRQ_CYCLES;
    }
    if (hw_irq_pending && !(cpustatus & FLAG_INTERRUPT) && irq_vector() >= 0x0100) {
        sidr_dbg_irq_taken++;
        irq_jump();
        return IRQ_CYCLES;
    }
    return 0;
}

/* PSID model: call play() and stop when it returns. This is the original
 * behaviour, kept byte-identical in output because almost every tune was
 * verified against it.
 *
 * Two details matter and both were nearly lost when the RSID model was added:
 *
 * 1. **No cycle budget.** A play routine is allowed to burn more cycles than a
 *    video frame contains - plenty of tunes do - and cutting it off at the
 *    frame boundary truncates it mid-way. When the cut happens before the code
 *    that writes $D418 (volume), the chip stays stuck at whatever volume value
 *    was reached, which sounds exactly like "the volume stopped changing".
 *    The original capped at 1,000,000 instructions, which is never reached.
 * 2. **The clocks do not advance.** Only branch penalties feed the intra-frame
 *    timestamp, so a play routine's writes are stamped within a fraction of a
 *    millisecond of the frame start. Advancing VIC/CIA here instead would also
 *    fire interrupts the PSID model is not supposed to service. */
static void call_play_psid(uint16_t play_addr)
{
    pc = play_addr;
    sp = 0xFF;
    push16(0);
    push16(0);
    uint32_t guard = 0;
    while (pc > 1 && guard++ < 1000000) {
        exec6502();
        /* Only branch penalties were ever accumulated here, and the intra-frame
         * timestamps were calibrated against that. Adding full opcode cycles
         * would shift every event later within its frame (and was the other
         * half of the "timing looks different" complaint). */
        clockticks6502 += s_branch_ticks;
    }
}

/* PSID model wrapper that keeps the two timestamp accounting rules the old
 * code had: init pinned to 0, play writes stamped from the frame base. */
static void tick_psid(uint16_t play_addr)
{
    if (play_addr >= 0x0100) call_play_psid(play_addr);
}/* Run the machine for exactly `cycles` CPU cycles starting at `entry_pc`.
 *
 * This is what makes digi tracks work. Under the PSID model play() is called
 * once per video frame and only the cycles play() itself burns are counted;
 * between frames nothing runs, so a CIA-timed NMI that is supposed to fire
 * thousands of times per second never fires at all. Here the clock is the loop
 * counter: every cycle advances VIC and both CIAs, interrupts are taken at
 * instruction boundaries, and once play() has RTS'd the CPU parks at IDLE_PC
 * while the clocks keep running for the rest of the tick. */
static void run_machine(uint16_t entry_pc, uint32_t cycles)
{
    if (entry_pc >= 0x0100) {
        pc = entry_pc;
        sp = 0xFF;
        push16(0);
        push16(0);
    } else {
        pc = IDLE_PC;
    }

    uint32_t spent = 0;
    while (spent < cycles) {
        /* Play routines are allowed to RTS; from then on we are idling. */
        if (pc < 2) pc = IDLE_PC;

        int n = service_interrupt();
        if (n == 0 && pc != IDLE_PC) n = exec6502();
        else if (n == 0) n = 1;               /* idle: one cycle at a time */

        for (int i = 0; i < n && spent < cycles; i++) {
            check_hardware_irq();
            clockticks6502++;
            spent++;
        }
    }
}

void sidr_debug_report(void)
{
    fprintf(stderr, "  [dbg] nmi vector=$%04X  irq vector=$%04X\n",
            (unsigned)s_dbg_nmi_vector, (unsigned)s_dbg_irq_vector);
    fprintf(stderr, "  [dbg] cia1 latchA=%u ctrlA=0x%02X | cia2 latchA=%u ctrlA=0x%02X started=%u\n",
            (unsigned)s_dbg_cia1_latchA, s_dbg_cia1_ctrlA,
            (unsigned)s_dbg_cia2_latchA, s_dbg_cia2_ctrlA, (unsigned)s_dbg_cia2_started);
    fprintf(stderr, "  [dbg] nmi taken=%lu  irq taken=%lu\n",
            sidr_dbg_nmi_taken, sidr_dbg_irq_taken);
}

/* Snapshot taken while the machine is still alive: reset_machine() wipes RAM at
 * the end of every extract, so the vectors have to be copied out first. */
static void debug_snapshot(void)
{
    s_dbg_nmi_vector  = nmi_vector();
    s_dbg_irq_vector  = irq_vector();
    s_dbg_cia1_latchA = cia1.latchA;
    s_dbg_cia1_ctrlA  = cia1.ctrlA;
    s_dbg_cia2_latchA = cia2.latchA;
    s_dbg_cia2_ctrlA  = cia2.ctrlA;
    s_dbg_cia2_started = cia2.startedA;
}

/* init 只跑一次, 而且它的写入全部钉在 t=0, 所以这里不需要走硬件时钟:
 * 只要把那串寄存器写出来就行。跑满周期预算或 RTS 即止。 */
static void call_init(uint16_t init_addr, uint8_t song, uint32_t max_cycles) {
    reset_cpu();
    a = song;            /* SIDPod: na=song (init 用 A 传子曲目) */
    pc = init_addr;
    sp = 0xFF;
    push16(0);           /* 假返回地址 0, RTS 后 pc=0 */
    push16(0);           /* SIDPod push 两次 (额外保护) */
    uint32_t cy = 0;
    while (pc > 1 && cy < max_cycles) {
        int n = exec6502();
        cy += (uint32_t)n;
        clockticks6502 += n;
    }
}

/* A tick is one unit of real time: the video frame for a normal tune, the CIA
 * timer period for a CIA-timed subsong. run_machine() fills exactly that many
 * cycles, so both the play routine and the interrupts it depends on advance at
 * the right wall-clock rate. */
static void call_play(uint16_t play_addr, uint32_t cycles) {
    run_machine(play_addr, cycles);
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
 * play rate
 *
 * Almost every tune wants play() once per video frame. The header's speed
 * word can say otherwise for an individual subsong: bit N set means subsong N
 * is driven by CIA timer A, i.e. play() belongs at the timer's own rate, not
 * at the frame rate. SIDPod (src/audio/C64.cpp: useCIA()) makes exactly this
 * test - IS_BIT_SET(info.speed, currentSong) - and Diderik recommends it in
 * "SID file format" §2.2. Getting it wrong is not a musical detail: a
 * CIA-timed tune wants its play routine called far more often than 50 Hz, and
 * at 50 Hz the digi/PCM writes land 20 ms apart and turn into silence or
 * noise.
 *
 * The period is read back from the latch the init routine left at $DC04/05,
 * because that is the only place the value exists - a header probe cannot see
 * it, so the caller has to run init first, which is what playSong() does in
 * SIDPod.
 * ============================================================ */
static int subsong_uses_cia(const PSID *si, int subsong)
{
    if (subsong < 0 || subsong > 31) return 0;
    return (si->speed >> subsong) & 1u;
}

/* Cycles between two play() calls. `vsync_cycles` is the PAL/NTSC frame in CPU
 * cycles; a CIA-timed subsong replaces it with timer A's latch. Timer A
 * decrements once per cycle in the emulator, so the latch value IS the period
 * in cycles. A zero latch means the init routine never programmed it, which
 * would divide by zero and would also mean the tune cannot be CIA-timed, so
 * the frame rate is the right fallback. */
static uint32_t pick_play_cycles(uint32_t vsync_cycles, int cia, uint32_t cia_latch)
{
    if (cia && cia_latch > 0) return cia_latch;
    return vsync_cycles;
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
    hw_nmi_pending = 0;

    VIC_D011 = 0x1b; VIC_D012 = 0; VIC_D019 = 0; VIC_D01A = 0;
    VIC_y = 0;
    VIC_lines = 312; VIC_cpr = 63;
    s_cyc_in_line = 0;

    memset(&cia1, 0, sizeof(cia1));
    memset(&cia2, 0, sizeof(cia2));

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

    /* Two playback models, chosen by the caller (see sid_rsid.h).
     *
     * PSID: play() once per video frame, no clock, no interrupts - the shape
     * the player has always used, byte-identical in output to the version
     * before the digi work. RSID: run the machine for a full tick so interrupt
     * handlers (digi samples) actually execute. */
    if (!s_rsid_mode) {
        int32_t n_frames = (int32_t)(seconds * vsync_hz);
        if (n_frames < 0) n_frames = 0;
        uint16_t play_addr = get_play_addr(si.play_addr);
        if (play_addr == 0) n_frames = 0;

        info->cia_timed   = 0;
        info->play_cycles = (uint32_t)g_cycles_per_frame;
        info->play_hz     = vsync_hz;

        for (int32_t fr = 0; fr < n_frames; fr++) {
            g_current_frame = fr;
            g_current_cycle = (int64_t)fr * (int64_t)g_cycles_per_frame;
            g_frame_base_cycle = clockticks6502;
            tick_psid(play_addr);
        }
    } else {
        /* Now that init has run, the header's speed bit and the timer latch are
         * both knowable, and together they decide how often play() belongs to be
         * called. See pick_play_cycles() above. */
        int      cia_timed = subsong_uses_cia(&si, subsong);
        uint32_t play_cycles = pick_play_cycles((uint32_t)g_cycles_per_frame,
                                                cia_timed, cia1.latchA);
        if (play_cycles == 0) play_cycles = (uint32_t)g_cycles_per_frame;

        info->cia_timed   = cia_timed;
        info->play_cycles = play_cycles;
        info->play_hz     = (double)cpu_hz / (double)play_cycles;

        int32_t n_plays = (int32_t)(seconds * cpu_hz / (double)play_cycles);
        if (n_plays < 0) n_plays = 0;
        uint16_t play_addr = get_play_addr(si.play_addr);

        /* Each tick covers play_cycles of real time. Timestamps stay absolute
         * because g_current_cycle is set from the tick index, and on_sid_write
         * adds the intra-tick offset. */
        for (int32_t fr = 0; fr < n_plays; fr++) {
            g_current_frame = fr;
            g_current_cycle = (int64_t)fr * (int64_t)play_cycles;
            g_frame_base_cycle = clockticks6502;
            call_play(play_addr, play_cycles);
        }
    }

    sid_write_hook = NULL;
    debug_snapshot();

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
static int32_t   s_stream_tick = 0;
static int32_t   s_stream_play_cycles = 19656;  /* frame, or CIA latch */
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

    /* PSID model (default): the tick is always a video frame and play() is
     * called once and left to return. RSID model: the tick may be the CIA
     * timer period and the machine runs for the whole tick. See sid_rsid.h. */
    uint32_t play_cycles;
    if (!s_rsid_mode) {
        play_cycles = (uint32_t)g_cycles_per_frame;
        info->cia_timed   = 0;
        info->play_cycles = play_cycles;
        info->play_hz     = vsync_hz;
    } else {
        /* After init the speed bit and the CIA latch are both readable. A
         * CIA-timed subsong (Giana Sisters' in-game tracks are the classic
         * case) wants play() at the timer's rate, not at 50 Hz. */
        int cia_timed = subsong_uses_cia(&si, subsong);
        play_cycles = pick_play_cycles((uint32_t)g_cycles_per_frame,
                                       cia_timed, cia1.latchA);
        info->cia_timed   = cia_timed;
        info->play_cycles = play_cycles;
        info->play_hz     = (double)cpu_hz / (double)play_cycles;
    }

    /* The init writes are handed out as the first tick, so the caller gets
     * them before it starts counting time. */
    s_stream_play_addr   = get_play_addr(si.play_addr);
    s_stream_play_cycles = (int32_t)play_cycles;
    s_stream_tick = 0;
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
        g_current_cycle = (int64_t)s_stream_tick * (int64_t)s_stream_play_cycles;
        g_frame_base_cycle = clockticks6502;
        if (s_rsid_mode) call_play(s_stream_play_addr, (uint32_t)s_stream_play_cycles);
        else             tick_psid(s_stream_play_addr);
        s_stream_tick++;
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

/* How long one sid_stream_next_frame() tick lasts. For a video-frame tune this
 * is 19.96 ms; for a CIA-timed one it is the timer period, which can be a few
 * milliseconds. The player's scheduler needs the real number or it will space
 * the whole stream wrongly. */
double sid_stream_frame_ms(void)
{
    if (s_stream_play_cycles <= 0 || s_stream_cpu_hz <= 0) return 19.96;
    return (double)s_stream_play_cycles / s_stream_cpu_hz * 1000.0;
}

void sid_stream_close(void)
{
    if (!s_streaming) return;
    sid_write_hook = NULL;
    reset_machine();
    s_streaming = 0;
    s_stream_play_addr = 0;
    s_stream_tick = 0;
    s_stream_play_cycles = 19656;
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

