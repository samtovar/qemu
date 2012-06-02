/*
 * Luminary Micro STM32 peripherals
 *
 * Copyright (c) 2006 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#include "sysbus.h"
#include "ssi.h"
#include "arm-misc.h"
#include "devices.h"
#include "qemu-timer.h"
#include "i2c.h"
#include "net.h"
#include "boards.h"
#include "exec-memory.h"

#include "stm32f2xx_defines.h"

#define GPIO_A 0
#define GPIO_B 1
#define GPIO_C 2
#define GPIO_D 3
#define GPIO_E 4
#define GPIO_F 5
#define GPIO_G 6

typedef const struct {
    const char *name;
    uint32_t did0;
    uint32_t did1;
    uint32_t dc0;
    uint32_t dc1;
    uint32_t dc2;
    uint32_t dc3;
    uint32_t dc4;
    uint32_t peripherals;
} stm32_board_info;

/* General purpose timer module.  */

typedef struct gptm_state {
    SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t config;
    uint32_t mode[2];
    uint32_t control;
    uint32_t state;
    uint32_t mask;
    uint32_t load[2];
    uint32_t match[2];
    uint32_t prescale[2];
    uint32_t match_prescale[2];
    uint32_t rtc;
    int64_t tick[2];
    struct gptm_state *opaque[2];
    QEMUTimer *timer[2];
    /* The timers have an alternate output used to trigger the ADC.  */
    qemu_irq trigger;
    qemu_irq irq;
} gptm_state;

static void gptm_update_irq(gptm_state *s)
{
    int level;
    level = (s->state & s->mask) != 0;
    qemu_set_irq(s->irq, level);
}

static void gptm_stop(gptm_state *s, int n)
{
    qemu_del_timer(s->timer[n]);
}

static void gptm_reload(gptm_state *s, int n, int reset)
{
    int64_t tick;
    if (reset)
        tick = qemu_get_clock_ns(vm_clock);
    else
        tick = s->tick[n];

    if (s->config == 0) {
        /* 32-bit CountDown.  */
        uint32_t count;
        count = s->load[0] | (s->load[1] << 16);
        tick += (int64_t)count * system_clock_scale;
    } else if (s->config == 1) {
        /* 32-bit RTC.  1Hz tick.  */
        tick += get_ticks_per_sec();
    } else if (s->mode[n] == 0xa) {
        /* PWM mode.  Not implemented.  */
    } else {
        hw_error("TODO: 16-bit timer mode 0x%x\n", s->mode[n]);
    }
    s->tick[n] = tick;
    qemu_mod_timer(s->timer[n], tick);
}

static void gptm_tick(void *opaque)
{
    gptm_state **p = (gptm_state **)opaque;
    gptm_state *s;
    int n;

    s = *p;
    n = p - s->opaque;
    if (s->config == 0) {
        s->state |= 1;
        if ((s->control & 0x20)) {
            /* Output trigger.  */
	    qemu_irq_pulse(s->trigger);
        }
        if (s->mode[0] & 1) {
            /* One-shot.  */
            s->control &= ~1;
        } else {
            /* Periodic.  */
            gptm_reload(s, 0, 0);
        }
    } else if (s->config == 1) {
        /* RTC.  */
        uint32_t match;
        s->rtc++;
        match = s->match[0] | (s->match[1] << 16);
        if (s->rtc > match)
            s->rtc = 0;
        if (s->rtc == 0) {
            s->state |= 8;
        }
        gptm_reload(s, 0, 0);
    } else if (s->mode[n] == 0xa) {
        /* PWM mode.  Not implemented.  */
    } else {
        hw_error("TODO: 16-bit timer mode 0x%x\n", s->mode[n]);
    }
    gptm_update_irq(s);
}

static uint64_t gptm_read(void *opaque, target_phys_addr_t offset,
                          unsigned size)
{
    gptm_state *s = (gptm_state *)opaque;

    switch (offset) {
    case 0x00: /* CFG */
        return s->config;
    case 0x04: /* TAMR */
        return s->mode[0];
    case 0x08: /* TBMR */
        return s->mode[1];
    case 0x0c: /* CTL */
        return s->control;
    case 0x18: /* IMR */
        return s->mask;
    case 0x1c: /* RIS */
        return s->state;
    case 0x20: /* MIS */
        return s->state & s->mask;
    case 0x24: /* CR */
        return 0;
    case 0x28: /* TAILR */
        return s->load[0] | ((s->config < 4) ? (s->load[1] << 16) : 0);
    case 0x2c: /* TBILR */
        return s->load[1];
    case 0x30: /* TAMARCHR */
        return s->match[0] | ((s->config < 4) ? (s->match[1] << 16) : 0);
    case 0x34: /* TBMATCHR */
        return s->match[1];
    case 0x38: /* TAPR */
        return s->prescale[0];
    case 0x3c: /* TBPR */
        return s->prescale[1];
    case 0x40: /* TAPMR */
        return s->match_prescale[0];
    case 0x44: /* TBPMR */
        return s->match_prescale[1];
    case 0x48: /* TAR */
        if (s->control == 1)
            return s->rtc;
    case 0x4c: /* TBR */
        hw_error("TODO: Timer value read\n");
    default:
        hw_error("gptm_read: Bad offset 0x%x\n", (int)offset);
        return 0;
    }
}

static void gptm_write(void *opaque, target_phys_addr_t offset,
                       uint64_t value, unsigned size)
{
    gptm_state *s = (gptm_state *)opaque;
    uint32_t oldval;

    /* The timers should be disabled before changing the configuration.
       We take advantage of this and defer everything until the timer
       is enabled.  */
    switch (offset) {
    case 0x00: /* CFG */
        s->config = value;
        break;
    case 0x04: /* TAMR */
        s->mode[0] = value;
        break;
    case 0x08: /* TBMR */
        s->mode[1] = value;
        break;
    case 0x0c: /* CTL */
        oldval = s->control;
        s->control = value;
        /* TODO: Implement pause.  */
        if ((oldval ^ value) & 1) {
            if (value & 1) {
                gptm_reload(s, 0, 1);
            } else {
                gptm_stop(s, 0);
            }
        }
        if (((oldval ^ value) & 0x100) && s->config >= 4) {
            if (value & 0x100) {
                gptm_reload(s, 1, 1);
            } else {
                gptm_stop(s, 1);
            }
        }
        break;
    case 0x18: /* IMR */
        s->mask = value & 0x77;
        gptm_update_irq(s);
        break;
    case 0x24: /* CR */
        s->state &= ~value;
        break;
    case 0x28: /* TAILR */
        s->load[0] = value & 0xffff;
        if (s->config < 4) {
            s->load[1] = value >> 16;
        }
        break;
    case 0x2c: /* TBILR */
        s->load[1] = value & 0xffff;
        break;
    case 0x30: /* TAMARCHR */
        s->match[0] = value & 0xffff;
        if (s->config < 4) {
            s->match[1] = value >> 16;
        }
        break;
    case 0x34: /* TBMATCHR */
        s->match[1] = value >> 16;
        break;
    case 0x38: /* TAPR */
        s->prescale[0] = value;
        break;
    case 0x3c: /* TBPR */
        s->prescale[1] = value;
        break;
    case 0x40: /* TAPMR */
        s->match_prescale[0] = value;
        break;
    case 0x44: /* TBPMR */
        s->match_prescale[0] = value;
        break;
    default:
        hw_error("gptm_write: Bad offset 0x%x\n", (int)offset);
    }
    gptm_update_irq(s);
}

static const MemoryRegionOps gptm_ops = {
    .read = gptm_read,
    .write = gptm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_stm32_gptm = {
    .name = "stm32_gptm",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_UINT32(config, gptm_state),
        VMSTATE_UINT32_ARRAY(mode, gptm_state, 2),
        VMSTATE_UINT32(control, gptm_state),
        VMSTATE_UINT32(state, gptm_state),
        VMSTATE_UINT32(mask, gptm_state),
        VMSTATE_UNUSED(8),
        VMSTATE_UINT32_ARRAY(load, gptm_state, 2),
        VMSTATE_UINT32_ARRAY(match, gptm_state, 2),
        VMSTATE_UINT32_ARRAY(prescale, gptm_state, 2),
        VMSTATE_UINT32_ARRAY(match_prescale, gptm_state, 2),
        VMSTATE_UINT32(rtc, gptm_state),
        VMSTATE_INT64_ARRAY(tick, gptm_state, 2),
        VMSTATE_TIMER_ARRAY(timer, gptm_state, 2),
        VMSTATE_END_OF_LIST()
    }
};

static int stm32_gptm_init(SysBusDevice *dev)
{
    gptm_state *s = FROM_SYSBUS(gptm_state, dev);

    sysbus_init_irq(dev, &s->irq);
    qdev_init_gpio_out(&dev->qdev, &s->trigger, 1);

    memory_region_init_io(&s->iomem, &gptm_ops, s,
                          "gptm", 0x1000);
    sysbus_init_mmio(dev, &s->iomem);

    s->opaque[0] = s->opaque[1] = s;
    s->timer[0] = qemu_new_timer_ns(vm_clock, gptm_tick, &s->opaque[0]);
    s->timer[1] = qemu_new_timer_ns(vm_clock, gptm_tick, &s->opaque[1]);
    vmstate_register(&dev->qdev, -1, &vmstate_stm32_gptm, s);
    return 0;
}


/* System controller.  */

typedef struct {
    MemoryRegion iomem;
    uint32_t pborctl;
    uint32_t ldopctl;
    uint32_t int_status;
    uint32_t int_mask;
    uint32_t resc;
    uint32_t rcc;

    uint32_t rcc_cr;
    uint32_t rcc_cfgr;
    uint32_t rcc_anything_else;

    uint32_t rcc2;
    uint32_t rcgc[3];
    uint32_t scgc[3];
    uint32_t dcgc[3];
    uint32_t clkvclr;
    uint32_t ldoarst;
    qemu_irq irq;
    stm32_board_info *board;
} ssys_state;

static void ssys_update(ssys_state *s)
{
  qemu_set_irq(s->irq, (s->int_status & s->int_mask) != 0);
}

#define DID0_VER_MASK        0x70000000
#define DID0_VER_0           0x00000000
#define DID0_VER_1           0x10000000

#define DID0_CLASS_MASK      0x00FF0000
#define DID0_CLASS_SANDSTORM 0x00000000
#define DID0_CLASS_FURY      0x00010000

static int ssys_board_class(const ssys_state *s)
{
    uint32_t did0 = s->board->did0;
    switch (did0 & DID0_VER_MASK) {
    case DID0_VER_0:
        return DID0_CLASS_SANDSTORM;
    case DID0_VER_1:
        switch (did0 & DID0_CLASS_MASK) {
        case DID0_CLASS_SANDSTORM:
        case DID0_CLASS_FURY:
            return did0 & DID0_CLASS_MASK;
        }
        /* for unknown classes, fall through */
    default:
        hw_error("ssys_board_class: Unknown class 0x%08x\n", did0);
    }
}

static uint64_t ssys_read(void *opaque, target_phys_addr_t offset,
                          unsigned size)
{
    ssys_state *s = (ssys_state *)opaque;

    uint64_t result = 0;

    switch (offset)
    {
    case 0x00:
        result = s->rcc_cr; break;
    case 0x08:
        result = s->rcc_cfgr; break;
    default:
        result = s->rcc_anything_else; break;
        //hw_error("ssys_write: Bad offset 0x%x\n", (int)offset);
    }

    printf("stm32_read running: offset %u result %llu (0x%llx)\n", offset, result, result);

    return result;
}

static bool ssys_use_rcc2(ssys_state *s)
{
    return (s->rcc2 >> 31) & 0x1;
}

/*
 * Caculate the sys. clock period in ms.
 */
static void ssys_calculate_system_clock(ssys_state *s)
{
    if (ssys_use_rcc2(s)) {
        system_clock_scale = 5 * (((s->rcc2 >> 23) & 0x3f) + 1);
    } else {
        system_clock_scale = 5 * (((s->rcc >> 23) & 0xf) + 1);
    }
}

static void print_selected_used(const char* str, uint32_t v)
{
    uint32_t selected = v & RCC_CFGR_SW;
    uint32_t used = (v & RCC_CFGR_SWS) >> 2;
    printf("%s: sel 0x%x used 0x%x\n", str, selected, used);
}

static void ssys_write(void *opaque, target_phys_addr_t offset,
                       uint64_t value, unsigned size)
{
    printf("stm32_write running: offset %u value %llu (0x%llx) size %u\n", offset, value, value, size);
    ssys_state *s = (ssys_state *)opaque;

    switch (offset) {
    case 0x00:
        s->rcc_cr = value;
        if (s->rcc_cr | RCC_CR_PLLON)
        {
            s->rcc_cr |= RCC_CR_PLLRDY;
        }
        break;
    case 0x08:
    {
        print_selected_used("incoming", value);

        // Clear out the previously selected system clock and paste in the new ones.
        // selected is the mask 0x03, used is the mask 0x0c
        s->rcc_cfgr = value & (~RCC_CFGR_SWS);
        s->rcc_cfgr |= (s->rcc_cfgr & RCC_CFGR_SW) << 2;

        print_selected_used("after", s->rcc_cfgr);

        break;
    }
    default:
        s->rcc_anything_else = value; break;
        //hw_error("ssys_write: Bad offset 0x%x\n", (int)offset);
    }
    ssys_update(s);
}

static const MemoryRegionOps ssys_ops = {
    .read = ssys_read,
    .write = ssys_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ssys_reset(void *opaque)
{
    ssys_state *s = (ssys_state *)opaque;

    s->pborctl = 0x7ffd;
    s->rcc = 0x078e3ac0;

    if (ssys_board_class(s) == DID0_CLASS_SANDSTORM) {
        s->rcc2 = 0;
    } else {
        s->rcc2 = 0x07802810;
    }
    s->rcgc[0] = 1;
    s->scgc[0] = 1;
    s->dcgc[0] = 1;
    ssys_calculate_system_clock(s);
}

static int stm32_sys_post_load(void *opaque, int version_id)
{
    ssys_state *s = opaque;

    ssys_calculate_system_clock(s);

    return 0;
}

static const VMStateDescription vmstate_stm32_sys = {
    .name = "stm32_sys",
    .version_id = 2,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .post_load = stm32_sys_post_load,
    .fields      = (VMStateField[]) {
        VMSTATE_UINT32(pborctl, ssys_state),
        VMSTATE_UINT32(ldopctl, ssys_state),
        VMSTATE_UINT32(int_mask, ssys_state),
        VMSTATE_UINT32(int_status, ssys_state),
        VMSTATE_UINT32(resc, ssys_state),
        VMSTATE_UINT32(rcc, ssys_state),
        VMSTATE_UINT32_V(rcc2, ssys_state, 2),
        VMSTATE_UINT32_ARRAY(rcgc, ssys_state, 3),
        VMSTATE_UINT32_ARRAY(scgc, ssys_state, 3),
        VMSTATE_UINT32_ARRAY(dcgc, ssys_state, 3),
        VMSTATE_UINT32(clkvclr, ssys_state),
        VMSTATE_UINT32(ldoarst, ssys_state),
        VMSTATE_END_OF_LIST()
    }
};

static int stm32_sys_init(uint32_t base, qemu_irq irq,
                          stm32_board_info * board)
{
    ssys_state *s;

    s = (ssys_state *)g_malloc0(sizeof(ssys_state));
    s->irq = irq;
    s->board = board;

    memory_region_init_io(&s->iomem, &ssys_ops, s, "ssys", 0x00000500);
    memory_region_add_subregion(get_system_memory(), base, &s->iomem);
    ssys_reset(s);
    vmstate_register(NULL, -1, &vmstate_stm32_sys, s);
    return 0;
}

/* I2C controller.  */

typedef struct {
    SysBusDevice busdev;
    i2c_bus *bus;
    qemu_irq irq;
    MemoryRegion iomem;
    uint32_t msa;
    uint32_t mcs;
    uint32_t mdr;
    uint32_t mtpr;
    uint32_t mimr;
    uint32_t mris;
    uint32_t mcr;
} stm32_i2c_state;

#define STELLARIS_I2C_MCS_BUSY    0x01
#define STELLARIS_I2C_MCS_ERROR   0x02
#define STELLARIS_I2C_MCS_ADRACK  0x04
#define STELLARIS_I2C_MCS_DATACK  0x08
#define STELLARIS_I2C_MCS_ARBLST  0x10
#define STELLARIS_I2C_MCS_IDLE    0x20
#define STELLARIS_I2C_MCS_BUSBSY  0x40

static uint64_t stm32_i2c_read(void *opaque, target_phys_addr_t offset,
                                   unsigned size)
{
    stm32_i2c_state *s = (stm32_i2c_state *)opaque;

    switch (offset) {
    case 0x00: /* MSA */
        return s->msa;
    case 0x04: /* MCS */
        /* We don't emulate timing, so the controller is never busy.  */
        return s->mcs | STELLARIS_I2C_MCS_IDLE;
    case 0x08: /* MDR */
        return s->mdr;
    case 0x0c: /* MTPR */
        return s->mtpr;
    case 0x10: /* MIMR */
        return s->mimr;
    case 0x14: /* MRIS */
        return s->mris;
    case 0x18: /* MMIS */
        return s->mris & s->mimr;
    case 0x20: /* MCR */
        return s->mcr;
    default:
        hw_error("strllaris_i2c_read: Bad offset 0x%x\n", (int)offset);
        return 0;
    }
}

static void stm32_i2c_update(stm32_i2c_state *s)
{
    int level;

    level = (s->mris & s->mimr) != 0;
    qemu_set_irq(s->irq, level);
}

static void stm32_i2c_write(void *opaque, target_phys_addr_t offset,
                                uint64_t value, unsigned size)
{
    stm32_i2c_state *s = (stm32_i2c_state *)opaque;

    switch (offset) {
    case 0x00: /* MSA */
        s->msa = value & 0xff;
        break;
    case 0x04: /* MCS */
        if ((s->mcr & 0x10) == 0) {
            /* Disabled.  Do nothing.  */
            break;
        }
        /* Grab the bus if this is starting a transfer.  */
        if ((value & 2) && (s->mcs & STELLARIS_I2C_MCS_BUSBSY) == 0) {
            if (i2c_start_transfer(s->bus, s->msa >> 1, s->msa & 1)) {
                s->mcs |= STELLARIS_I2C_MCS_ARBLST;
            } else {
                s->mcs &= ~STELLARIS_I2C_MCS_ARBLST;
                s->mcs |= STELLARIS_I2C_MCS_BUSBSY;
            }
        }
        /* If we don't have the bus then indicate an error.  */
        if (!i2c_bus_busy(s->bus)
                || (s->mcs & STELLARIS_I2C_MCS_BUSBSY) == 0) {
            s->mcs |= STELLARIS_I2C_MCS_ERROR;
            break;
        }
        s->mcs &= ~STELLARIS_I2C_MCS_ERROR;
        if (value & 1) {
            /* Transfer a byte.  */
            /* TODO: Handle errors.  */
            if (s->msa & 1) {
                /* Recv */
                s->mdr = i2c_recv(s->bus) & 0xff;
            } else {
                /* Send */
                i2c_send(s->bus, s->mdr);
            }
            /* Raise an interrupt.  */
            s->mris |= 1;
        }
        if (value & 4) {
            /* Finish transfer.  */
            i2c_end_transfer(s->bus);
            s->mcs &= ~STELLARIS_I2C_MCS_BUSBSY;
        }
        break;
    case 0x08: /* MDR */
        s->mdr = value & 0xff;
        break;
    case 0x0c: /* MTPR */
        s->mtpr = value & 0xff;
        break;
    case 0x10: /* MIMR */
        s->mimr = 1;
        break;
    case 0x1c: /* MICR */
        s->mris &= ~value;
        break;
    case 0x20: /* MCR */
        if (value & 1)
            hw_error(
                      "stm32_i2c_write: Loopback not implemented\n");
        if (value & 0x20)
            hw_error(
                      "stm32_i2c_write: Slave mode not implemented\n");
        s->mcr = value & 0x31;
        break;
    default:
        hw_error("stm32_i2c_write: Bad offset 0x%x\n",
                  (int)offset);
    }
    stm32_i2c_update(s);
}

static void stm32_i2c_reset(stm32_i2c_state *s)
{
    if (s->mcs & STELLARIS_I2C_MCS_BUSBSY)
        i2c_end_transfer(s->bus);

    s->msa = 0;
    s->mcs = 0;
    s->mdr = 0;
    s->mtpr = 1;
    s->mimr = 0;
    s->mris = 0;
    s->mcr = 0;
    stm32_i2c_update(s);
}

static const MemoryRegionOps stm32_i2c_ops = {
    .read = stm32_i2c_read,
    .write = stm32_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_stm32_i2c = {
    .name = "stm32_i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_UINT32(msa, stm32_i2c_state),
        VMSTATE_UINT32(mcs, stm32_i2c_state),
        VMSTATE_UINT32(mdr, stm32_i2c_state),
        VMSTATE_UINT32(mtpr, stm32_i2c_state),
        VMSTATE_UINT32(mimr, stm32_i2c_state),
        VMSTATE_UINT32(mris, stm32_i2c_state),
        VMSTATE_UINT32(mcr, stm32_i2c_state),
        VMSTATE_END_OF_LIST()
    }
};

static int stm32_i2c_init(SysBusDevice * dev)
{
    stm32_i2c_state *s = FROM_SYSBUS(stm32_i2c_state, dev);
    i2c_bus *bus;

    sysbus_init_irq(dev, &s->irq);
    bus = i2c_init_bus(&dev->qdev, "i2c");
    s->bus = bus;

    memory_region_init_io(&s->iomem, &stm32_i2c_ops, s,
                          "i2c", 0x1000);
    sysbus_init_mmio(dev, &s->iomem);
    /* ??? For now we only implement the master interface.  */
    stm32_i2c_reset(s);
    vmstate_register(&dev->qdev, -1, &vmstate_stm32_i2c, s);
    return 0;
}

/* Analogue to Digital Converter.  This is only partially implemented,
   enough for applications that use a combined ADC and timer tick.  */

#define STELLARIS_ADC_EM_CONTROLLER 0
#define STELLARIS_ADC_EM_COMP       1
#define STELLARIS_ADC_EM_EXTERNAL   4
#define STELLARIS_ADC_EM_TIMER      5
#define STELLARIS_ADC_EM_PWM0       6
#define STELLARIS_ADC_EM_PWM1       7
#define STELLARIS_ADC_EM_PWM2       8

#define STELLARIS_ADC_FIFO_EMPTY    0x0100
#define STELLARIS_ADC_FIFO_FULL     0x1000

typedef struct
{
    SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t actss;
    uint32_t ris;
    uint32_t im;
    uint32_t emux;
    uint32_t ostat;
    uint32_t ustat;
    uint32_t sspri;
    uint32_t sac;
    struct {
        uint32_t state;
        uint32_t data[16];
    } fifo[4];
    uint32_t ssmux[4];
    uint32_t ssctl[4];
    uint32_t noise;
    qemu_irq irq[4];
} stm32_adc_state;

static uint32_t stm32_adc_fifo_read(stm32_adc_state *s, int n)
{
    int tail;

    tail = s->fifo[n].state & 0xf;
    if (s->fifo[n].state & STELLARIS_ADC_FIFO_EMPTY) {
        s->ustat |= 1 << n;
    } else {
        s->fifo[n].state = (s->fifo[n].state & ~0xf) | ((tail + 1) & 0xf);
        s->fifo[n].state &= ~STELLARIS_ADC_FIFO_FULL;
        if (tail + 1 == ((s->fifo[n].state >> 4) & 0xf))
            s->fifo[n].state |= STELLARIS_ADC_FIFO_EMPTY;
    }
    return s->fifo[n].data[tail];
}

static void stm32_adc_fifo_write(stm32_adc_state *s, int n,
                                     uint32_t value)
{
    int head;

    /* TODO: Real hardware has limited size FIFOs.  We have a full 16 entry 
       FIFO fir each sequencer.  */
    head = (s->fifo[n].state >> 4) & 0xf;
    if (s->fifo[n].state & STELLARIS_ADC_FIFO_FULL) {
        s->ostat |= 1 << n;
        return;
    }
    s->fifo[n].data[head] = value;
    head = (head + 1) & 0xf;
    s->fifo[n].state &= ~STELLARIS_ADC_FIFO_EMPTY;
    s->fifo[n].state = (s->fifo[n].state & ~0xf0) | (head << 4);
    if ((s->fifo[n].state & 0xf) == head)
        s->fifo[n].state |= STELLARIS_ADC_FIFO_FULL;
}

static void stm32_adc_update(stm32_adc_state *s)
{
    int level;
    int n;

    for (n = 0; n < 4; n++) {
        level = (s->ris & s->im & (1 << n)) != 0;
        qemu_set_irq(s->irq[n], level);
    }
}

static void stm32_adc_trigger(void *opaque, int irq, int level)
{
    stm32_adc_state *s = (stm32_adc_state *)opaque;
    int n;

    for (n = 0; n < 4; n++) {
        if ((s->actss & (1 << n)) == 0) {
            continue;
        }

        if (((s->emux >> (n * 4)) & 0xff) != 5) {
            continue;
        }

        /* Some applications use the ADC as a random number source, so introduce
           some variation into the signal.  */
        s->noise = s->noise * 314159 + 1;
        /* ??? actual inputs not implemented.  Return an arbitrary value.  */
        stm32_adc_fifo_write(s, n, 0x200 + ((s->noise >> 16) & 7));
        s->ris |= (1 << n);
        stm32_adc_update(s);
    }
}

static void stm32_adc_reset(stm32_adc_state *s)
{
    int n;

    for (n = 0; n < 4; n++) {
        s->ssmux[n] = 0;
        s->ssctl[n] = 0;
        s->fifo[n].state = STELLARIS_ADC_FIFO_EMPTY;
    }
}

static uint64_t stm32_adc_read(void *opaque, target_phys_addr_t offset,
                                   unsigned size)
{
    stm32_adc_state *s = (stm32_adc_state *)opaque;

    /* TODO: Implement this.  */
    if (offset >= 0x40 && offset < 0xc0) {
        int n;
        n = (offset - 0x40) >> 5;
        switch (offset & 0x1f) {
        case 0x00: /* SSMUX */
            return s->ssmux[n];
        case 0x04: /* SSCTL */
            return s->ssctl[n];
        case 0x08: /* SSFIFO */
            return stm32_adc_fifo_read(s, n);
        case 0x0c: /* SSFSTAT */
            return s->fifo[n].state;
        default:
            break;
        }
    }
    switch (offset) {
    case 0x00: /* ACTSS */
        return s->actss;
    case 0x04: /* RIS */
        return s->ris;
    case 0x08: /* IM */
        return s->im;
    case 0x0c: /* ISC */
        return s->ris & s->im;
    case 0x10: /* OSTAT */
        return s->ostat;
    case 0x14: /* EMUX */
        return s->emux;
    case 0x18: /* USTAT */
        return s->ustat;
    case 0x20: /* SSPRI */
        return s->sspri;
    case 0x30: /* SAC */
        return s->sac;
    default:
        hw_error("strllaris_adc_read: Bad offset 0x%x\n",
                  (int)offset);
        return 0;
    }
}

static void stm32_adc_write(void *opaque, target_phys_addr_t offset,
                                uint64_t value, unsigned size)
{
    stm32_adc_state *s = (stm32_adc_state *)opaque;

    /* TODO: Implement this.  */
    if (offset >= 0x40 && offset < 0xc0) {
        int n;
        n = (offset - 0x40) >> 5;
        switch (offset & 0x1f) {
        case 0x00: /* SSMUX */
            s->ssmux[n] = value & 0x33333333;
            return;
        case 0x04: /* SSCTL */
            if (value != 6) {
                hw_error("ADC: Unimplemented sequence %" PRIx64 "\n",
                          value);
            }
            s->ssctl[n] = value;
            return;
        default:
            break;
        }
    }
    switch (offset) {
    case 0x00: /* ACTSS */
        s->actss = value & 0xf;
        break;
    case 0x08: /* IM */
        s->im = value;
        break;
    case 0x0c: /* ISC */
        s->ris &= ~value;
        break;
    case 0x10: /* OSTAT */
        s->ostat &= ~value;
        break;
    case 0x14: /* EMUX */
        s->emux = value;
        break;
    case 0x18: /* USTAT */
        s->ustat &= ~value;
        break;
    case 0x20: /* SSPRI */
        s->sspri = value;
        break;
    case 0x28: /* PSSI */
        hw_error("Not implemented:  ADC sample initiate\n");
        break;
    case 0x30: /* SAC */
        s->sac = value;
        break;
    default:
        hw_error("stm32_adc_write: Bad offset 0x%x\n", (int)offset);
    }
    stm32_adc_update(s);
}

static const MemoryRegionOps stm32_adc_ops = {
    .read = stm32_adc_read,
    .write = stm32_adc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_stm32_adc = {
    .name = "stm32_adc",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_UINT32(actss, stm32_adc_state),
        VMSTATE_UINT32(ris, stm32_adc_state),
        VMSTATE_UINT32(im, stm32_adc_state),
        VMSTATE_UINT32(emux, stm32_adc_state),
        VMSTATE_UINT32(ostat, stm32_adc_state),
        VMSTATE_UINT32(ustat, stm32_adc_state),
        VMSTATE_UINT32(sspri, stm32_adc_state),
        VMSTATE_UINT32(sac, stm32_adc_state),
        VMSTATE_UINT32(fifo[0].state, stm32_adc_state),
        VMSTATE_UINT32_ARRAY(fifo[0].data, stm32_adc_state, 16),
        VMSTATE_UINT32(ssmux[0], stm32_adc_state),
        VMSTATE_UINT32(ssctl[0], stm32_adc_state),
        VMSTATE_UINT32(fifo[1].state, stm32_adc_state),
        VMSTATE_UINT32_ARRAY(fifo[1].data, stm32_adc_state, 16),
        VMSTATE_UINT32(ssmux[1], stm32_adc_state),
        VMSTATE_UINT32(ssctl[1], stm32_adc_state),
        VMSTATE_UINT32(fifo[2].state, stm32_adc_state),
        VMSTATE_UINT32_ARRAY(fifo[2].data, stm32_adc_state, 16),
        VMSTATE_UINT32(ssmux[2], stm32_adc_state),
        VMSTATE_UINT32(ssctl[2], stm32_adc_state),
        VMSTATE_UINT32(fifo[3].state, stm32_adc_state),
        VMSTATE_UINT32_ARRAY(fifo[3].data, stm32_adc_state, 16),
        VMSTATE_UINT32(ssmux[3], stm32_adc_state),
        VMSTATE_UINT32(ssctl[3], stm32_adc_state),
        VMSTATE_UINT32(noise, stm32_adc_state),
        VMSTATE_END_OF_LIST()
    }
};

static int stm32_adc_init(SysBusDevice *dev)
{
    stm32_adc_state *s = FROM_SYSBUS(stm32_adc_state, dev);
    int n;

    for (n = 0; n < 4; n++) {
        sysbus_init_irq(dev, &s->irq[n]);
    }

    memory_region_init_io(&s->iomem, &stm32_adc_ops, s,
                          "adc", 0x1000);
    sysbus_init_mmio(dev, &s->iomem);
    stm32_adc_reset(s);
    qdev_init_gpio_in(&dev->qdev, stm32_adc_trigger, 1);
    vmstate_register(&dev->qdev, -1, &vmstate_stm32_adc, s);
    return 0;
}

/* Some boards have both an OLED controller and SD card connected to
   the same SSI port, with the SD card chip select connected to a
   GPIO pin.  Technically the OLED chip select is connected to the SSI
   Fss pin.  We do not bother emulating that as both devices should
   never be selected simultaneously, and our OLED controller ignores stray
   0xff commands that occur when deselecting the SD card.  */

typedef struct {
    SSISlave ssidev;
    qemu_irq irq;
    int current_dev;
    SSIBus *bus[2];
} stm32_ssi_bus_state;

static const VMStateDescription vmstate_stm32_ssi_bus = {
    .name = "stm32_ssi_bus",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_INT32(current_dev, stm32_ssi_bus_state),
        VMSTATE_END_OF_LIST()
    }
};

static void stm32_init(const char *kernel_filename, const char *cpu_model,
                           stm32_board_info *board)
{
    printf("stm32_init running\n");

    static const int uart_irq[] = {5, 6, 33, 34};
    static const int timer_irq[] = {19, 21, 23, 35};
    static const uint32_t gpio_addr[7] =
      { 0x40004000, 0x40005000, 0x40006000, 0x40007000,
        0x40024000, 0x40025000, 0x40026000};
    static const int gpio_irq[7] = {0, 1, 2, 3, 4, 30, 31};

    MemoryRegion *address_space_mem = get_system_memory();
    qemu_irq *pic;
    DeviceState *gpio_dev[7];
    //qemu_irq gpio_in[7][8];
    qemu_irq gpio_out[7][8];
    qemu_irq adc;
    int sram_size;
    int flash_size;
    i2c_bus *i2c;
    DeviceState *dev;
    int i;
    int j;

    flash_size = ((board->dc0 & 0xffff) + 1) << 1;
    sram_size = (board->dc0 >> 18) + 1;
    pic = armv7m_init(address_space_mem,
                      flash_size, sram_size, kernel_filename, cpu_model);

    if (board->dc1 & (1 << 16)) {
        dev = sysbus_create_varargs("stm32-adc", 0x40038000,
                                    pic[14], pic[15], pic[16], pic[17], NULL);
        adc = qdev_get_gpio_in(dev, 0);
    } else {
        adc = NULL;
    }
    for (i = 0; i < 4; i++) {
        if (board->dc2 & (0x10000 << i)) {
            dev = sysbus_create_simple("stm32-gptm",
                                       0x40030000 + i * 0x1000,
                                       pic[timer_irq[i]]);
            /* TODO: This is incorrect, but we get away with it because
               the ADC output is only ever pulsed.  */
            qdev_connect_gpio_out(dev, 0, adc);
        }
    }

    stm32_sys_init(0x40023800, pic[28], board);

    for (i = 0; i < 7; i++) {
        if (board->dc4 & (1 << i)) {
            gpio_dev[i] = sysbus_create_simple("pl061_luminary", gpio_addr[i],
                                               pic[gpio_irq[i]]);
            for (j = 0; j < 8; j++) {
                //gpio_in[i][j] = qdev_get_gpio_in(gpio_dev[i], j);
                gpio_out[i][j] = NULL;
            }
        }
    }

    if (board->dc2 & (1 << 12)) {
        dev = sysbus_create_simple("stm32-i2c", 0x40020000, pic[8]);
        i2c = (i2c_bus *)qdev_get_child_bus(dev, "i2c");
        (void)i2c;
    }

    for (i = 0; i < 4; i++) {
        if (board->dc2 & (1 << i)) {
            sysbus_create_simple("pl011_luminary", 0x4000c000 + i * 0x1000,
                                 pic[uart_irq[i]]);
        }
    }
    
    if (board->dc4 & (1 << 28)) {
        DeviceState *enet;

        qemu_check_nic_model(&nd_table[0], "stm32");

        enet = qdev_create(NULL, "stm32_enet");
        qdev_set_nic_properties(enet, &nd_table[0]);
        qdev_init_nofail(enet);
        sysbus_mmio_map(sysbus_from_qdev(enet), 0, 0x40048000);
        sysbus_connect_irq(sysbus_from_qdev(enet), 0, pic[42]);
    }
    
    for (i = 0; i < 7; i++) {
        if (board->dc4 & (1 << i)) {
            for (j = 0; j < 8; j++) {
                if (gpio_out[i][j]) {
                    qdev_connect_gpio_out(gpio_dev[i], j, gpio_out[i][j]);
                }
            }
        }
    }
}

/* Board init.  */
static stm32_board_info stm32_boards[] = {
  { "LM3S811EVB",
    0,
    0x0032000e,
    0x001f001f, /* dc0 */
    0x001132bf,
    0x01071013,
    0x3f0f01ff,
    0x0000001f,
    0
  }
};

/* FIXME: Figure out how to generate these from stm32_boards.  */
static void stm32f2xx_init(ram_addr_t ram_size,
                     const char *boot_device,
                     const char *kernel_filename, const char *kernel_cmdline,
                     const char *initrd_filename, const char *cpu_model)
{
    stm32_init(kernel_filename, cpu_model, &stm32_boards[0]);
}

static QEMUMachine stm32f2xx_machine = {
    .name = "stm32f2xx",
    .desc = "STM32F2xx ",
    .init = stm32f2xx_init,
};

static void stm32_machine_init(void)
{
    qemu_register_machine(&stm32f2xx_machine);
}

machine_init(stm32_machine_init);

static void stm32_i2c_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *sdc = SYS_BUS_DEVICE_CLASS(klass);

    sdc->init = stm32_i2c_init;
}

static TypeInfo stm32_i2c_info = {
    .name          = "stm32-i2c",
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(stm32_i2c_state),
    .class_init    = stm32_i2c_class_init,
};

static void stm32_gptm_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *sdc = SYS_BUS_DEVICE_CLASS(klass);

    sdc->init = stm32_gptm_init;
}

static TypeInfo stm32_gptm_info = {
    .name          = "stm32-gptm",
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(gptm_state),
    .class_init    = stm32_gptm_class_init,
};

static void stm32_adc_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *sdc = SYS_BUS_DEVICE_CLASS(klass);

    sdc->init = stm32_adc_init;
}

static TypeInfo stm32_adc_info = {
    .name          = "stm32-adc",
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(stm32_adc_state),
    .class_init    = stm32_adc_class_init,
};

static void stm32_register_types(void)
{
    type_register_static(&stm32_i2c_info);
    type_register_static(&stm32_gptm_info);
    type_register_static(&stm32_adc_info);
}

type_init(stm32_register_types)
