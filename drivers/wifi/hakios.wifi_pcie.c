/*
 * OnePlus 9 / SM8350 PCIe Wi-Fi bring-up. Android identifies Qualcomm
 * 17cb:1103 (CNSS QCA6490), revision 1, on PCIe root complex 0.
 * Register locations: the device FDT and OnePlusOSS SM8350 sources at
 * 13ca08fdf0979fdd61d5e8991661874bb2d19150: gcc-lahaina.c, lahaina-pcie.dtsi,
 * pci-msm.c. No network identity, calibration or credentials belong here.
 * Stages 2..15 keep endpoint DMA off; 16 verifies an unmapped DMA guard;
 * 17 transfers the original BHI prefix; 18 adds MSI/MHI events; 19 loads the
 * complete image; 20/21 test IPCR/QRTR and QMI, without RF or networking.
 * hakiospcie_wifi=1 only reads; it never starts the redwood AHB/WPSS path.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/hakios.input.h>
#include <sys/errno.h>
#include <sys/hakios.wifi_pcie.h>
#include <sys/hakios.radio_pcie.h>
#include <sys/hakios.lx.h>
#include <sys/hakios.qrtr.h>
#include <sys/systm.h>
#include <sys/random.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include "hakiospcie_mhi_protocol.h"

/* ---- what this file uses before it defines it (see the end of the file) ---- */
bool hakios_qrtr_attach(int (*send)(const void *, unsigned));
void hakios_qrtr_deliver(const void *packet, unsigned length);
void hakios_qrtr_poll(void);
void hakios_qrtr_detach(void);
int  hakiospcie_lx_os_thread(const char *name, void (*function)(void *), void *argument);

void hakiospcie_lx_os_wakeup(const void *address);
void hakiospcie_lx_os_unlock(void);
bool hakiospcie_lx_os_radio_address(unsigned char *address);
void hakiospcie_lx_os_net_attach(const unsigned char *address);


extern void hakiospcie_tl(const char *format, ...) __printflike(1, 2);
static vm_offset_t gcc, pins, parf, dbi, phy, config, bar;
static bool phy_started, perst_saved, radio_started, link_started, bar_saved;
static unsigned iatu_count;
static uint32_t perst_cfg, perst_io;
static uint32_t radio_cfg, radio_io, clkreq_cfg, clkreq_io, iatu_values[2][7];
static uint32_t bar0_old, bar1_old, endpoint_command_old;
static bool sid_saved, bus_master_was_enabled, msi_saved;
static unsigned msi_offset, msi_data_offset, msi_mask_offset;
static uint32_t msi_old_control, msi_old_low, msi_old_high, msi_old_data, msi_old_mask;
static uint32_t sid_table_old[256], sid_cfg_old;
extern uint64_t kvtophys(vm_offset_t va);
/* These late BSD-init mappings must use kernel_map. The permanent io_submap
 * is only 8 MiB, and concurrent display/GPU/touch leave too little room for
 * WLAN's BAR and SMMU. Register mappings retain the existing kernel lifetime:
 * stop restores/quiesces hardware while they are valid, and this RAM kernel
 * is then discarded. DMA data aliases below still have explicit teardown. */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern lck_mtx_t hakios_smmu_lock;
static void dma_restore(void);
static void driver_quiesce(void);
static bool driver_receive(const uint8_t *, unsigned);
static unsigned driver_in_seq, driver_out_seq;
static LCK_GRP_DECLARE(hakiospcie_wifi_group, "lemonade-wifi");
static LCK_MTX_DECLARE(hakiospcie_mhi_lock, &hakiospcie_wifi_group);
static LCK_MTX_DECLARE(hakiospcie_window_lock, &hakiospcie_wifi_group);
static unsigned wifi_mode;
/* wifi_mem_empty: answer a memory request with an empty reply */
static unsigned wifi_mem_empty;
/* wifi_mem_align: round the addresses in a memory reply down to this boundary */
static unsigned wifi_mem_align;
/* wifi_mem_build: answer with a reply we compose, inside the window we mapped */
static unsigned wifi_mem_build;
/* wifi_mem_shift: undo the eight bit shift in the sizes of a memory reply */
static unsigned wifi_mem_shift;
/* wifi_mem_type: the type field to write into each segment of a memory reply */
static unsigned wifi_mem_type;
static unsigned wifi_board_type;
/* keep the file id the radio asked for instead of rewriting it to 0 */
static unsigned board_file_id_keep;
static unsigned wifi_skip_regdb;
static unsigned wifi_ping;
static unsigned wifi_board_pace = 20;
static unsigned wifi_board_elf;
static unsigned board_total_constant;
static unsigned board_repeat_first;
static unsigned wifi_board_end;
static unsigned wifi_board_cap;   /* hand ath11k only this many bytes of board data */   /* tell ath11k to cut the board file short */   /* 1: claim the fourth piece is the last one */ /* 1: send the same first piece over and over */ /* 1: keep total_size at the size of the whole file */
static unsigned board_file_total; /* 1: hand out the vendor ELF, which ath11k sends as bdf type 1 */
/* wifi_mem_real: name memory we really allocated, not a spot in the window */
static unsigned wifi_mem_real = 1;
/* wifi_m3: kilobytes of host memory to offer the radio for its crash dump */
static unsigned wifi_m3;
/* the two regions the radio asked for, mapped once before the conversation starts */
static uint64_t mem_host_iova[2];
static vm_offset_t mem_host_memory[2];
static bool mem_host_ready;
struct hakiospcie_packet { struct hakiospcie_packet *next; unsigned length; uint8_t data[]; };
static struct {
    volatile bool enabled, wanted, running, failed;
    bool attached, called;
    unsigned queued;
    struct hakiospcie_packet *first, *last;
} driver_runtime;

/* Qualcomm's hypervisor emulates some SMMU registers. Use simple register
 * operands, as the existing Adreno driver does; never STR WZR or indexed STR. */
static uint32_t __attribute__((noinline)) rd(vm_offset_t base, unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(base + offset) : "memory");
    return value;
}
static void __attribute__((noinline)) wr(vm_offset_t address, uint32_t value) {
    __asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(address) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void __attribute__((noinline)) wr16(vm_offset_t address, uint16_t value) {
    __asm__ volatile("strh %w0, [%1]" : : "r"(value), "r"(address) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void __attribute__((noinline)) wr64(vm_offset_t address, uint64_t value) {
    __asm__ volatile("str %0, [%1]" : : "r"(value), "r"(address) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint64_t __attribute__((noinline)) rd64(vm_offset_t address) {
    uint64_t value;
    __asm__ volatile("ldr %0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}
static void pause_us(unsigned us) { delay_for_interval(us, NSEC_PER_USEC); }
static bool wait_mask(vm_offset_t base, unsigned offset, uint32_t mask, uint32_t value) {
    for (unsigned i = 0; i < 1000; i++) {
        if ((rd(base, offset) & mask) == value) { return true; }
        pause_us(10);
    }
    hakiospcie_tl("wifi-pcie: wait failed offset %#x mask %#x value %#x actual %#x\n",
        offset, mask, value, rd(base, offset));
    return false;
}
/* Only our masked GCC bits are restored; unrelated USB/display votes survive. */
static struct { unsigned offset; uint32_t mask, value; } saved_gcc[16];
static unsigned saved_count;
static struct { unsigned offset; uint32_t value; } saved_parf[12];
static unsigned parf_count;
static struct { unsigned offset; uint32_t value; bool half; } saved_dbi[8];
static unsigned dbi_count;
static void change_dbi(unsigned offset, uint32_t value, bool half) {
    unsigned i;
    for (i = 0; i < dbi_count; i++) { if (saved_dbi[i].offset == offset) { break; } }
    if (i == dbi_count) {
        if (dbi_count == sizeof(saved_dbi) / sizeof(saved_dbi[0])) { panic("Wi-Fi DBI journal full"); }
        saved_dbi[i].offset = offset; saved_dbi[i].value = rd(dbi, offset); saved_dbi[i].half = half; dbi_count++;
    }
    if (half) { wr16(dbi + offset, value); } else { wr(dbi + offset, value); }
}
static void change_parf(unsigned offset, uint32_t value) {
    unsigned i;
    for (i = 0; i < parf_count; i++) { if (saved_parf[i].offset == offset) { break; } }
    if (i == parf_count) {
        if (parf_count == sizeof(saved_parf) / sizeof(saved_parf[0])) { panic("Wi-Fi PARF journal full"); }
        saved_parf[i].offset = offset; saved_parf[i].value = rd(parf, offset); parf_count++;
    }
    wr(parf + offset, value);
}
static void change_gcc(unsigned offset, uint32_t mask, uint32_t value) {
    unsigned i;
    for (i = 0; i < saved_count; i++) { if (saved_gcc[i].offset == offset) { break; } }
    if (i == saved_count) {
        if (saved_count == sizeof(saved_gcc) / sizeof(saved_gcc[0])) { panic("Wi-Fi register journal full"); }
        saved_gcc[i].offset = offset; saved_gcc[i].mask = 0; saved_gcc[i].value = rd(gcc, offset);
        saved_count++;
    }
    saved_gcc[i].mask |= mask;
    wr(gcc + offset, (rd(gcc, offset) & ~mask) | (value & mask));
}
/* Final, non-sleeping fallback after other CPUs have stopped. Normal return
 * calls hakiospcie_wifi_stop() while threads can still drain their queues. */
void hakiospcie_wifi_quiesce(void) {
    if (!bar_saved && !link_started && !radio_started) { return; }
    driver_runtime.wanted = false;
    driver_runtime.enabled = false;
    if (bar_saved && (rd(dbi, 0x80) & (1u << 29))) {
        if (msi_saved) { wr16(config + msi_offset + 2, msi_old_control & ~1u); }
        wr16(config + 4, endpoint_command_old & ~6u);
    }
    if (pins && perst_saved) {
        wr(pins + 94 * 0x1000 + 4, rd(pins, 94 * 0x1000 + 4) & ~2u);
    }
    if (pins && radio_started) {
        wr(pins + 64 * 0x1000 + 4, rd(pins, 64 * 0x1000 + 4) & ~2u);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    hakiospcie_tl("wifi-pcie: emergency endpoint reset; DMA pages retained; Android reboot required\n");
}
void hakiospcie_wifi_stop(void) {
    driver_quiesce();
    if (!saved_count && !parf_count && !perst_saved) { return; }
    if (bar_saved) {
        if (rd(dbi, 0x80) & (1u << 29)) {
            if (msi_saved) {
                wr16(config + msi_offset + 2, msi_old_control & ~1u);
                if (msi_old_control & 0x100) { wr(config + msi_mask_offset, msi_old_mask); }
                wr(config + msi_offset + 4, msi_old_low);
                if (msi_old_control & 0x80) { wr(config + msi_offset + 8, msi_old_high); }
                wr16(config + msi_data_offset, msi_old_data); wr16(config + msi_offset + 2, msi_old_control);
                bool restored = (rd(config, msi_offset) >> 16) == msi_old_control &&
                    rd(config, msi_offset + 4) == msi_old_low && (rd(config, msi_data_offset) & 0xffff) == msi_old_data &&
                    (!(msi_old_control & 0x80) || rd(config, msi_offset + 8) == msi_old_high) &&
                    (!(msi_old_control & 0x100) || rd(config, msi_mask_offset) == msi_old_mask);
                hakiospcie_tl("wifi-pcie: PCI MSI disabled and original configuration restored\n");
                hakiospcie_tl("wifi-pcie: PCI MSI restore readback %s\n", restored ? "verified" : "failed");
                msi_saved = false;
            }
            wr16(config + 4, endpoint_command_old & ~6u);
            if (bus_master_was_enabled) {
                hakiospcie_tl("wifi-pcie: endpoint bus master clear %s\n", (rd(config, 4) & 4) ? "failed" : "verified");
            }
            wr(config + 0x14, bar1_old); wr(config + 0x10, bar0_old);
            wr16(config + 4, endpoint_command_old);
            hakiospcie_tl("wifi-pcie: endpoint BAR and memory decode restored\n");
        } else { hakiospcie_tl("wifi-pcie: endpoint link lost during cleanup; PERST reset required\n"); }
        bar_saved = false;
    }
    while (iatu_count) {
        unsigned n = --iatu_count;
        vm_offset_t at = dbi + 0x1000 + 0x200 * n;
        wr(at + 4, 0);
        for (unsigned i = 0; i < 7; i++) { if (i != 1) { wr(at + 4 * i, iatu_values[n][i]); } }
        wr(at + 4, iatu_values[n][1]);
    }
    if (link_started) {
        wr(parf + 0x1b0, rd(parf, 0x1b0) & ~(1u << 8));
        wr(pins + 94 * 0x1000 + 4, rd(pins, 94 * 0x1000 + 4) & ~2u);
        pause_us(1000);
        link_started = false;
    }
    dma_restore(); /* endpoint is in reset before any translation is removed */
    if (sid_saved) {
        for (unsigned i = 0; i < 256; i++) { wr(parf + 0x2000 + 4 * i, sid_table_old[i]); }
        wr(parf + 0x2c00, sid_cfg_old);
        sid_saved = false;
        hakiospcie_tl("wifi-pcie: BDF/SID table restored after endpoint reset\n");
    }
    while (dbi_count) {
        unsigned i = --dbi_count;
        if (saved_dbi[i].half) { wr16(dbi + saved_dbi[i].offset, saved_dbi[i].value); }
        else { wr(dbi + saved_dbi[i].offset, saved_dbi[i].value); }
    }
    if (radio_started) {
        wr(pins + 64 * 0x1000 + 4, rd(pins, 64 * 0x1000 + 4) & ~2u);
        pause_us(1000);
    }
    if (phy_started) {
        wr(phy + 0x200, 1); /* PCS software reset */
        wr(phy + 0x244, 0); /* PCS stop */
        wr(phy + 0x240, 0); /* vendor PHY power-down */
        change_gcc(0x52008, 1u << 4, 0);
        phy_started = false;
    }
    /* The vendor switches PIPE back to XO before collapsing this domain. */
    if (parf_count) { change_gcc(0x6b054, 3, 2); }
    while (parf_count) {
        unsigned i = --parf_count;
        wr(parf + saved_parf[i].offset, saved_parf[i].value);
    }
    while (saved_count) {
        unsigned i = --saved_count;
        wr(gcc + saved_gcc[i].offset,
            (rd(gcc, saved_gcc[i].offset) & ~saved_gcc[i].mask) |
            (saved_gcc[i].value & saved_gcc[i].mask));
        if (saved_gcc[i].offset == 0x6b05c) {
            wr(gcc + 0x6b058, rd(gcc, 0x6b058) | 1);
            (void)wait_mask(gcc, 0x6b058, 1, 0);
        }
    }
    if (perst_saved) {
        wr(pins + 94 * 0x1000 + 4, perst_io);
        wr(pins + 94 * 0x1000, perst_cfg);
        perst_saved = false;
    }
    if (radio_started) {
        wr(pins + 64 * 0x1000 + 4, radio_io); wr(pins + 64 * 0x1000, radio_cfg);
        wr(pins + 95 * 0x1000 + 4, clkreq_io); wr(pins + 95 * 0x1000, clkreq_cfg);
        radio_started = false;
        hakiospcie_tl("wifi-pcie: WLAN_EN/CLKREQ restored; endpoint held off\n");
    }
    hakiospcie_tl("wifi-pcie: GCC votes restored; RPMh supply votes retained until reboot; %s\n",
        bus_master_was_enabled ? "endpoint DMA stopped before unmap" : "DMA never enabled");
}
static bool platform_is_lemonade(void) {
    DTEntry entry;
    const void *value;
    unsigned size;
    /*
     * mars hands the kernel the vendor's Linux-shaped tree: there is no /arm-io and nothing
     * names the machine (the CNSS node is there, at /soc/qcom,cnss-qca6490@b0000000).  The
     * boot argument hakiospcie_wifi= is the opt-in, so a tree that says nothing is not a
     * refusal; only a tree that names a different machine is.
     */
    if (SecureDTLookupEntry(NULL, "/arm-io", &entry) != kSuccess) {
        hakiospcie_tl("wifi-pcie: no /arm-io here (vendor-shaped tree); the boot argument is the opt-in\n");
        return true;
    }
    if (SecureDTGetProperty(entry, "device_type", &value, &size) != kSuccess) {
        hakiospcie_tl("wifi-pcie: /arm-io names no machine; the boot argument is the opt-in\n");
        return true;
    }
    hakiospcie_tl("wifi-pcie: /arm-io device_type is \"%.*s\" (%u bytes)\n",
        (int)(size > 0 && ((const char *)value)[size - 1] == 0 ? size - 1 : size),
        (const char *)value, size);
    /* The OnePlus 9 says "lemonade"; this tree runs on mars (Mi 11 Pro), the same SM8350. */
    return (size == sizeof("lemonade") && !memcmp(value, "lemonade", size)) ||
        (size == sizeof("mars") && !memcmp(value, "mars", size));
}

static bool inspect(void) {
    static const struct { const char *name; unsigned offset; } clocks[] = {
        {"gdsc", 0x6b004}, {"core_reset", 0x6b000}, {"phy_reset", 0x6c01c},
        {"vote0", 0x52000}, {"vote1", 0x52008}, {"collapse_vote", 0x52128},
        {"cfg_ahb", 0x6b024}, {"aux", 0x6b028}, {"mstr_axi", 0x6b01c},
        {"slv_axi", 0x6b014}, {"q2a_axi", 0x6b010}, {"pipe", 0x6b030},
        {"clkref", 0x8c004}, {"rchng", 0x6b038}, {"tbu", 0x9000c},
        {"ddrss", 0x8d080}, {"aggre0", 0x6b080}, {"aggre1", 0x8d084},
        {"aux_src", 0x6b058}, {"aux_cfg", 0x6b05c}, {"pipe_src", 0x6b054},
        {"rchng_src", 0x6b03c}, {"rchng_cfg", 0x6b040}
    };
    static const unsigned gpio[] = {64, 65, 94, 95, 96, 153};
    gcc = ml_io_map_unmappable(0x100000, 0x100000, VM_WIMG_IO);
    pins = ml_io_map_unmappable(0xf100000, 0x100000, VM_WIMG_IO);
    if (!gcc || !pins) { return false; }
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        hakiospcie_tl("wifi-pcie: GCC %s %#x = %#x\n", clocks[i].name,
            clocks[i].offset, rd(gcc, clocks[i].offset));
    }
    for (unsigned i = 0; i < sizeof(gpio) / sizeof(gpio[0]); i++) {
        hakiospcie_tl("wifi-pcie: GPIO%u cfg %#x io %#x irq %#x\n", gpio[i],
            rd(pins, gpio[i] * 0x1000), rd(pins, gpio[i] * 0x1000 + 4),
            rd(pins, gpio[i] * 0x1000 + 8));
    }
    /* A read of an unpowered interconnect can abort the CPU. */
    if (!(rd(gcc, 0x6b004) & (1u << 31)) || (rd(gcc, 0x6b000) & 1) ||
        ((rd(gcc, 0x6b024) | rd(gcc, 0x6b01c) | rd(gcc, 0x6b014)) & (1u << 31))) {
        hakiospcie_tl("wifi-pcie: PCIe power/clocks not ready; PARF/DBI/endpoint not accessed\n");
        return true;
    }
    parf = ml_io_map_unmappable(0x1c00000, 0x3000, VM_WIMG_IO);
    if (!parf) { return false; }
    hakiospcie_tl("wifi-pcie: PARF sys %#x pm %#x phy %#x ltssm %#x device_type %#x\n",
        rd(parf, 0), rd(parf, 0x20), rd(parf, 0x40), rd(parf, 0x1b0), rd(parf, 0x1000));
    hakiospcie_tl("wifi-pcie: inspection leaves DBI/endpoint untouched\n");
    return true;
}
/* SM8350 QMP PCIe v3: board DT revision 21101, not the generic 10100 table.
 * Only non-unique controller initialization constants; no radio calibration. */
static const struct { unsigned offset; uint32_t value; } hakiospcie_pcie_phy[] = {
    {0x0240, 0x03},
    {0x0094, 0x08},
    {0x0154, 0x34},
    {0x016c, 0x08},
    {0x0058, 0x0f},
    {0x00a4, 0x42},
    {0x0110, 0x24},
    {0x011c, 0x03},
    {0x0118, 0xb4},
    {0x010c, 0x02},
    {0x01bc, 0x11},
    {0x00bc, 0x82},
    {0x00d4, 0x03},
    {0x00d0, 0x55},
    {0x00cc, 0x55},
    {0x00b0, 0x1a},
    {0x00ac, 0x0a},
    {0x00c4, 0x68},
    {0x00e0, 0x02},
    {0x00dc, 0xaa},
    {0x00d8, 0xab},
    {0x00b8, 0x34},
    {0x00b4, 0x14},
    {0x0158, 0x01},
    {0x0074, 0x06},
    {0x007c, 0x16},
    {0x0084, 0x36},
    {0x0078, 0x06},
    {0x0080, 0x16},
    {0x0088, 0x36},
    {0x01b0, 0x1e},
    {0x01ac, 0xca},
    {0x01b8, 0x18},
    {0x01b4, 0xa2},
    {0x0050, 0x07},
    {0x0010, 0x01},
    {0x001c, 0x31},
    {0x0020, 0x01},
    {0x0024, 0xde},
    {0x0028, 0x07},
    {0x0030, 0x4c},
    {0x0034, 0x06},
    {0x0ee4, 0x20},
    {0x0e84, 0x75},
    {0x0e90, 0x3f},
    {0x115c, 0x7f},
    {0x1160, 0xff},
    {0x1164, 0xbf},
    {0x1168, 0x3f},
    {0x116c, 0xd8},
    {0x1170, 0xdc},
    {0x1174, 0xdc},
    {0x1178, 0x5c},
    {0x117c, 0x34},
    {0x1180, 0xa6},
    {0x1190, 0x34},
    {0x10d8, 0x07},
    {0x0e3c, 0x1d},
    {0x0e40, 0x0c},
    {0x10dc, 0x00},
    {0x104c, 0x08},
    {0x1050, 0x08},
    {0x1044, 0xf0},
    {0x11a4, 0x38},
    {0x10cc, 0xf0},
    {0x0694, 0x00},
    {0x0654, 0x00},
    {0x06a8, 0x0f},
    {0x0048, 0x90},
    {0x0620, 0xc1},
    {0x0388, 0x77},
    {0x0398, 0x0b},
    {0x02dc, 0x05},
    {0x0200, 0x00},
    {0x0244, 0x03},
};
/* CRC-8, polynomial 0x07, over the requester ID in network byte order. */
static uint8_t requester_hash(uint16_t requester) {
    uint8_t crc = 0;
    for (unsigned byte = 0; byte < 2; byte++) {
        crc ^= (requester >> (byte == 0 ? 8 : 0)) & 255;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ ((crc & 0x80) ? 0x07 : 0);
        }
    }
    return crc;
}
static bool route_endpoint(void) {
    if ((rd(config, 4) | rd(dbi, 4)) & 4) { return false; }
    unsigned hash = requester_hash(0x0100);
    if (!hash) { return false; } /* root BDF zero occupies hash zero */
    sid_cfg_old = rd(parf, 0x2c00);
    for (unsigned i = 0; i < 256; i++) { sid_table_old[i] = rd(parf, 0x2000 + 4 * i); }
    sid_saved = true;
    /* This root was reset and has just one endpoint, still unable to master. */
    for (unsigned i = 0; i < 256; i++) { wr(parf + 0x2000 + 4 * i, 0); }
    wr(parf + 0x2000 + 4 * hash, 0x01000100); /* BDF 01:00.0 -> relative SID 1 */
    wr(parf + 0x2c00, sid_cfg_old & ~1u);
    for (unsigned i = 0; i < 256; i++) {
        if (rd(parf, 0x2000 + 4 * i) != (i == hash ? 0x01000100u : 0)) { return false; }
    }
    if (rd(parf, 0x2c00) & 1) { return false; }
    hakiospcie_tl("wifi-pcie: stage 14 BDF 0x100 -> SID 0x1c01 hash %#x table verified; bus master disabled\n", hash);
    return true;
}

/* A privately owned stage-1 address space, with no identity mapping or
 * firmware-owned context. The first probe leaves PCI bus mastering
 * off. IDR0.ATOSNS means standard translation requests are NOT supported;
 * software table validation is kept distinct from a real endpoint DMA test. */
#define WIFI_DMA_IOVA 0xa0004000ull
#define WIFI_DMA_SIZE 0x80000u
#define WIFI_PHYS_MASK 0x0000fffffffff000ull
#define WIFI_FSR_FAULT 0xc00001feu /* excludes read-only translation-format bits [10:9] */
#define WIFI_MHI_IOVA 0xa0200000ull
#define WIFI_MHI_SIZE (8u * 1024 * 1024)
/* Everything handed out for rings and buffers starts here, so the two windows
 * the radio asks for (its DDR heap at 0xa1000000, calibration at 0xa1600000)
 * stay free and can be mapped where stock CNSS puts them. Only pool
 * allocations are held below WIFI_DATA_LIMIT; a caller that names its own
 * address, as the radio's two windows do, may sit anywhere this address space
 * reaches, which is why the ceiling is not applied to those.
 */
#define WIFI_ALLOC_IOVA 0xa6000000ull
#define WIFI_DATA_LIMIT 0xb0000000ull
/* Page 0 is the level-1 table; page 1 is the level-2 table for the 0x80000000
 * gigabyte that holds the transport, the MHI arena and the pool; page 2 is the
 * transport leaf and pages 3-6 the four MHI leaves; page 7 is the level-2
 * table for the 0xc0000000 gigabyte, where the radio's own windows are; pages
 * 8-127 are spare leaves, one per 2 MB slot, handed out as slots are used.
 * 120 leaves is 240 MB of ring and buffer address space.
 */
#define WIFI_L2_HIGH_GB 3u
#define WIFI_L2_HIGH_PAGE 7u
#define WIFI_LEAF_FIRST 8u
/* Page-number ceilings tried for the two host-memory windows, smallest first: 0x40000 is
 * 4 GB if the kernel counts 16 KB pages, 0x100000 is 4 GB if it counts 4 KB ones. The radio
 * is the only reader of these pages, so a failed attempt costs nothing. */
static const unsigned wifi_window_ceilings[] = { 0x40000u, 0x100000u, 0x400000u };
#define WIFI_TABLE_PAGES (WIFI_LEAF_FIRST + 120u)
#define WIFI_TABLE_SIZE (WIFI_TABLE_PAGES * 4096)
static const unsigned dma_cb_offsets[] = {0, 0x10, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x38, 0x3c};
static struct {
    vm_offset_t mmio, cb, tables, data, extra, control;
    unsigned match, context;
    uint32_t smr, s2cr, cbar, cba2r, cb_old[9];
    bool owned, retain_pages;
    unsigned allocation_count;
    uint64_t next_iova;
    unsigned leaf_next;
    struct { vm_offset_t cached, uncached; unsigned size; uint64_t iova; } allocations[32];
} dma_space;

static void dma_flush(vm_offset_t start, unsigned size) {
    for (vm_offset_t line = start; line < start + size; line += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
static bool dma_tlb_sync(void) {
    wr(dma_space.cb + 0x610, dma_space.context);
    wr(dma_space.cb + 0x7f0, 0);
    return wait_mask(dma_space.cb, 0x7f4, 1, 0);
}
static void dma_restore(void) {
    if (dma_space.retain_pages) { return; }
    if (dma_space.owned) {
        lck_mtx_lock(&hakios_smmu_lock);
        /* Keep the stream faulting while its context is dismantled. */
        wr(dma_space.mmio + 0xc00 + 4 * dma_space.match, 2u << 16);
        wr(dma_space.cb, 0);
        if (!dma_tlb_sync()) {
            dma_space.retain_pages = true;
            lck_mtx_unlock(&hakios_smmu_lock);
            hakiospcie_tl("wifi-pcie: DMA cleanup failed; tables retained with endpoint reset\n");
            return;
        }
        if (rd(dma_space.cb, 0x58) & WIFI_FSR_FAULT) { wr(dma_space.cb + 0x58, rd(dma_space.cb, 0x58) & WIFI_FSR_FAULT); }
        for (unsigned i = 1; i < 9; i++) { wr(dma_space.cb + dma_cb_offsets[i], dma_space.cb_old[i]); }
        wr(dma_space.mmio + 0x1000 + 4 * dma_space.context, dma_space.cbar);
        wr(dma_space.mmio + 0x1800 + 4 * dma_space.context, dma_space.cba2r);
        wr(dma_space.cb, dma_space.cb_old[0]);
        wr(dma_space.mmio + 0x800 + 4 * dma_space.match, dma_space.smr);
        wr(dma_space.mmio + 0xc00 + 4 * dma_space.match, dma_space.s2cr);
        uint32_t cbar_after = rd(dma_space.mmio, 0x1000 + 4 * dma_space.context);
        bool cbar_ok = cbar_after == dma_space.cbar;
        /* The hypervisor refuses a return to the unused stage-2 reset type.
         * Linux teardown also leaves CBAR: with no stream, SCTLR.M=0 and the
         * old TTBRs restored, this inactive stage-1 type owns no memory. */
        if (!cbar_ok && dma_space.cbar == 0 && cbar_after == 0x1f300 &&
            !(rd(dma_space.cb, 0) & 1) && !(dma_space.smr & (1u << 31))) {
            cbar_ok = true;
            hakiospcie_tl("wifi-pcie: inactive CBAR stage-1 type retained until reboot; stream invalid and context disabled\n");
        }
        bool restored = cbar_ok && rd(dma_space.mmio, 0x800 + 4 * dma_space.match) == dma_space.smr &&
            rd(dma_space.mmio, 0xc00 + 4 * dma_space.match) == dma_space.s2cr &&
            rd(dma_space.mmio, 0x1800 + 4 * dma_space.context) == dma_space.cba2r;
        for (unsigned i = 0; i < 9; i++) {
            uint32_t actual = rd(dma_space.cb, dma_cb_offsets[i]);
            if (actual != dma_space.cb_old[i]) {
                hakiospcie_tl("wifi-pcie: restore CB offset %#x expected %#x actual %#x\n", dma_cb_offsets[i], dma_space.cb_old[i], actual);
                restored = false;
            }
        }
        if (!restored) {
            hakiospcie_tl("wifi-pcie: restore globals SMR %#x/%#x S2CR %#x/%#x CBAR %#x/%#x CBA2R %#x/%#x (actual/old)\n",
                rd(dma_space.mmio, 0x800 + 4 * dma_space.match), dma_space.smr,
                rd(dma_space.mmio, 0xc00 + 4 * dma_space.match), dma_space.s2cr,
                rd(dma_space.mmio, 0x1000 + 4 * dma_space.context), dma_space.cbar,
                rd(dma_space.mmio, 0x1800 + 4 * dma_space.context), dma_space.cba2r);
        }
        dma_space.owned = false;
        lck_mtx_unlock(&hakios_smmu_lock);
        hakiospcie_tl("wifi-pcie: DMA context/stream release %s; translation registers restored after endpoint reset\n", restored ? "verified" : "failed");
        if (!restored) { dma_space.retain_pages = true; return; }
    }
    if (dma_space.control) { ml_io_unmap(dma_space.control, PAGE_SIZE); dma_space.control = 0; }
    while (dma_space.allocation_count) {
        unsigned n = --dma_space.allocation_count;
        if (dma_space.allocations[n].uncached) { ml_io_unmap(dma_space.allocations[n].uncached, dma_space.allocations[n].size); }
        kmem_free(kernel_map, dma_space.allocations[n].cached, dma_space.allocations[n].size);
    }
    if (dma_space.extra) { kmem_free(kernel_map, dma_space.extra, round_page(WIFI_MHI_SIZE)); dma_space.extra = 0; }
    if (dma_space.tables) { kmem_free(kernel_map, dma_space.tables, round_page(WIFI_TABLE_SIZE)); dma_space.tables = 0; }
    if (dma_space.data) { kmem_free(kernel_map, dma_space.data, round_page(WIFI_DMA_SIZE)); dma_space.data = 0; }
}
static bool dma_translate(uint64_t address, uint64_t *result) {
    wr64(dma_space.cb + 0x800, address);
    if (!wait_mask(dma_space.cb, 0x8f0, 1, 0)) { return false; }
    *result = rd64(dma_space.cb + 0x50);
    return true;
}
/* Table pages come from one allocation but are not physically contiguous, so a
 * physical address found in an entry is mapped back to the page it came from
 * by comparison, never by arithmetic on the physical difference. Anything that
 * does not name a page of our own table fails, which is what makes an unmapped
 * guard address an answer rather than a guess. */
static int dma_table_page(uint64_t physical) {
    if (!physical) { return -1; }
    for (unsigned page = 0; page < WIFI_TABLE_PAGES; page++) {
        if (kvtophys(dma_space.tables + page * 4096) == physical) { return (int)page; }
    }
    return -1;
}
static uint64_t *dma_l2(uint64_t address) {
    if (!dma_space.tables || address >> 39) { return NULL; }
    uint64_t *l1 = (void *)dma_space.tables;
    uint64_t entry = l1[(address >> 30) & 511];
    if ((entry & 3) != 3) { return NULL; }
    int page = dma_table_page(entry & WIFI_PHYS_MASK);
    if (page < 0) { return NULL; }
    return (void *)(dma_space.tables + page * 4096);
}
static bool dma_walk(uint64_t address, uint64_t *physical) {
    uint64_t *l2 = dma_l2(address);
    if (!l2) { return false; }
    uint64_t entry = l2[(address >> 21) & 511];
    if ((entry & 3) != 3) { return false; }
    int page = dma_table_page(entry & WIFI_PHYS_MASK);
    if (page < 0) { return false; }
    uint64_t pte = ((uint64_t *)(dma_space.tables + page * 4096))[(address >> 12) & 511];
    if ((pte & 3) != 3) { return false; }
    *physical = (pte & WIFI_PHYS_MASK) | (address & 4095);
    return true;
}
/* The leaf page for the 2 MB slot that holds address: the one already
 * published there when it is ours (two allocations can share a slot, because a
 * pool allocation may start on the page after the previous one), otherwise a
 * fresh one from the spare pages. An entry that is not one of our pages is
 * refused rather than overwritten. */
static uint64_t *dma_leaf(uint64_t address) {
    uint64_t *l2 = dma_l2(address);
    if (!l2) { return NULL; }
    unsigned index = (address >> 21) & 511;
    if (l2[index]) {
        if ((l2[index] & 3) != 3) { return NULL; }
        int page = dma_table_page(l2[index] & WIFI_PHYS_MASK);
        if (page < (int)WIFI_LEAF_FIRST || page >= (int)WIFI_TABLE_PAGES) { return NULL; }
        return (void *)(dma_space.tables + page * 4096);
    }
    if (dma_space.leaf_next >= WIFI_TABLE_PAGES) { return NULL; }
    vm_offset_t leaf = dma_space.tables + dma_space.leaf_next++ * 4096;
    l2[index] = kvtophys(leaf) | 3;
    return (void *)leaf;
}
/* Only allocated pages are published, separated by a native-page guard. The
 * mapping stays owned until endpoint reset; a failure never frees live DMA.
 * Every refusal names the step it stopped at: a bare no costs a phone cycle. */
static bool dma_allocate_at(unsigned requested, bool coherent, uint64_t at, vm_offset_t *memory, uint64_t *address) {
    if (!dma_space.owned || !requested || requested > 64u * 1024 * 1024 ||
        dma_space.allocation_count == 32) {
        hakiospcie_tl("QMI probe: DMA allocation refused: %s\n",
            !dma_space.owned ? "the address space is not owned" : !requested ? "nothing was asked for" :
            requested > 64u * 1024 * 1024 ? "more than 64 MB was asked for" : "all 32 slots are used");
        return false;
    }
    unsigned size = round_page(requested);
    uint64_t iova = at ? at : (dma_space.next_iova ? dma_space.next_iova : WIFI_ALLOC_IOVA);
    if ((iova >> 39) || (!at && iova > WIFI_DATA_LIMIT - size - PAGE_SIZE)) {
        hakiospcie_tl("QMI probe: DMA allocation refused: %#llx + %u bytes is outside the window\n", iova, size);
        return false;
    }
    vm_offset_t cached = 0, uncached = 0;
    kern_return_t result = KERN_FAILURE;
    if (!coherent) {
        result = kmem_alloc(kernel_map, &cached, size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
    } else if (at) {
        for (unsigned attempt = 0; attempt < 3; attempt++) {
            result = kmem_alloc_contig(kernel_map, &cached, size, 0, wifi_window_ceilings[attempt], 0,
                KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
            if (result == KERN_SUCCESS) {
                hakiospcie_tl("QMI probe: the window at %#llx took ceiling %#x on attempt %u\n",
                    iova, wifi_window_ceilings[attempt], attempt);
                break;
            }
        }
    } else {
        for (unsigned attempt = 0; attempt < 4; attempt++) {
            ppnum_t ceiling = attempt < 3 ? wifi_window_ceilings[attempt] : 0;
            result = kmem_alloc_contig(kernel_map, &cached, size, 0, ceiling, 0,
                KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
            if (result == KERN_SUCCESS) {
                hakiospcie_tl("QMI probe: the pool memory for %#llx took ceiling %#x on attempt %u\n",
                    iova, ceiling, attempt);
                break;
            }
        }
    }
    if (result != KERN_SUCCESS) {
        hakiospcie_tl("QMI probe: DMA allocation refused: %u bytes of memory could not be had\n", size);
        return false;
    }
    unsigned allocation = dma_space.allocation_count++;
    dma_space.allocations[allocation].cached = cached;
    dma_space.allocations[allocation].size = size;
    dma_space.allocations[allocation].iova = iova;
    if (!at) { dma_space.next_iova = iova + size + PAGE_SIZE; }
    hakiospcie_tl("QMI probe: DMA allocation %u: %u bytes asked, %u mapped, at %#llx\n",
        allocation, requested, size, iova);
    hakiospcie_tl("QMI probe: DMA allocation %u: its memory is at %#llx, %u bytes\n",
        allocation, (unsigned long long)kvtophys(cached), size);
    dma_flush(cached, size);
    if (coherent) {
        uncached = ml_io_map_unmappable(kvtophys(cached), size, VM_WIMG_WCOMB);
        if (!uncached) {
            hakiospcie_tl("QMI probe: DMA allocation %u refused: it has no uncached alias\n", allocation);
            return false;
        }
        dma_space.allocations[allocation].uncached = uncached;
    }
    const char *why = NULL;
    lck_mtx_lock(&hakios_smmu_lock);
    for (unsigned offset = 0; !why && offset < size; offset += 4096) {
        uint64_t iova_page = iova + offset, physical = kvtophys(cached + offset);
        uint64_t *leaf = dma_leaf(iova_page);
        if (!leaf) { why = "no leaf page is left for its 2 MB slot"; break; }
        if (!physical || physical >> 36 || physical & 4095) { why = "its memory is not 36-bit addressable"; break; }
        if (coherent && physical != kvtophys(cached) + offset) { why = "its memory is not physically contiguous"; break; }
        if (leaf[(iova_page >> 12) & 511]) { why = "its 4 KB entry is already in use"; break; }
        leaf[(iova_page >> 12) & 511] = physical | 3ull | (1ull << 6) | (2ull << 8) | (1ull << 10) |
            (1ull << 11) | (1ull << 53) | (1ull << 54);
    }
    dma_flush(dma_space.tables, WIFI_TABLE_SIZE);
    if (!why && !dma_tlb_sync()) { why = "the table invalidation never completed"; }
    lck_mtx_unlock(&hakios_smmu_lock);
    for (unsigned offset = 0; !why && offset < size; offset += 4096) {
        uint64_t physical = 0;
        if (!dma_walk(iova + offset, &physical) || physical != kvtophys(cached + offset)) {
            why = "the table did not read back what was written";
        }
    }
    if (!why) {
        uint64_t unused = 0;
        if (dma_walk(iova - 4096, &unused)) { why = "the guard page below it is mapped"; }
        else if (dma_walk(iova + size, &unused)) { why = "the guard page above it is mapped"; }
    }
    if (why) {
        hakiospcie_tl("QMI probe: DMA allocation %u refused: %s (at %#llx, %u bytes)\n",
            allocation, why, iova, size);
        return false;
    }
    *memory = coherent ? uncached : cached; *address = iova;
    hakiospcie_tl("wifi-pcie: owned DMA allocation %u requested %u mapped %u pages %u guards verified\n",
        allocation, requested, size, size / 4096);
    return true;
}
static bool dma_allocate(unsigned requested, bool coherent, vm_offset_t *memory, uint64_t *address) {
    return dma_allocate_at(requested, coherent, 0, memory, address);
}
/* The radio cannot be given memory above its own reach.  On this board the
 * loader hands the kernel a window that starts at 0xf4000000, so the whole
 * first four gigabytes is a 192 MB strip; measure what is left of it before the
 * driver asks for its own buffers. */
extern unsigned int vm_page_free_count;

static void dma_low_probe(void) {
	static const unsigned sizes[] = { 1u << 20, 2u << 20, 4u << 20, 6160384u, 8u << 20 };
	hakiospcie_tl("wifi-pcie: low memory: %u pages free\n", vm_page_free_count);
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		vm_offset_t got = 0;
		kern_return_t r = kmem_alloc_contig(kernel_map, &got, sizes[i], 0, 0x40000u, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
		if (r != KERN_SUCCESS) {
			hakiospcie_tl("wifi-pcie: low memory: %u bytes below 4 GB no\n", sizes[i]);
			continue;
		}
		hakiospcie_tl("wifi-pcie: low memory: %u bytes below 4 GB yes, at %#llx\n",
		    sizes[i], kvtophys(got));
		kmem_free(kernel_map, got, sizes[i]);
	}
}

static bool dma_prepare(bool extended) {
    dma_low_probe();
    if ((rd(config, 4) | rd(dbi, 4)) & 4) { return false; }
    dma_space.mmio = ml_io_map_unmappable(0x15000000, 0x100000, VM_WIMG_IO);
    if (!dma_space.mmio) { return false; }
    uint32_t scr = rd(dma_space.mmio, 0), id0 = rd(dma_space.mmio, 0x20), id1 = rd(dma_space.mmio, 0x24);
    unsigned matches = id0 & 255, contexts = id1 & 255, s2contexts = (id1 >> 16) & 255;
    /* This probe supports only the exact MMU-500 register geometry observed. */
    if ((scr & 9) || !(scr & (1u << 10)) || !(id0 & (1u << 30)) ||
        (id1 & 0xf0000000u) != 0x60000000u || matches > 255 || !matches || contexts > 128 || !contexts ||
        rd(dma_space.mmio, 0x48)) { return false; }
    if (kmem_alloc(kernel_map, &dma_space.tables, round_page(WIFI_TABLE_SIZE), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
        VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
        kmem_alloc(kernel_map, &dma_space.data, round_page(WIFI_DMA_SIZE), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
        VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) { return false; }
    uint64_t *l1 = (void *)dma_space.tables, *l2 = (void *)(dma_space.tables + 4096), *l3 = (void *)(dma_space.tables + 8192);
    dma_space.leaf_next = WIFI_LEAF_FIRST;
    l1[WIFI_L2_HIGH_GB] = kvtophys((vm_offset_t)(dma_space.tables + WIFI_L2_HIGH_PAGE * 4096)) | 3;
    l1[(WIFI_DMA_IOVA >> 30) & 511] = kvtophys((vm_offset_t)l2) | 3;
    l2[(WIFI_DMA_IOVA >> 21) & 511] = kvtophys((vm_offset_t)l3) | 3;
    for (unsigned offset = 0; offset < WIFI_DMA_SIZE; offset += 4096) {
        uint64_t physical = kvtophys(dma_space.data + offset);
        if (!physical || physical >> 36 || physical & 4095) { return false; }
        l3[((WIFI_DMA_IOVA + offset) >> 12) & 511] = physical |
            3ull | (1ull << 6) | (2ull << 8) | (1ull << 10) | (1ull << 11) | (1ull << 53) | (1ull << 54);
    }
    if (extended) {
        if (PAGE_SIZE != 16384 || kmem_alloc(kernel_map, &dma_space.extra, round_page(WIFI_MHI_SIZE),
            KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) { return false; }
        for (unsigned n = 0; n < 4; n++) {
            uint64_t *leaf = (void *)(dma_space.tables + (3 + n) * 4096);
            l2[((WIFI_MHI_IOVA >> 21) & 511) + n] = kvtophys((vm_offset_t)leaf) | 3;
            for (unsigned page = 0; page < 512; page++) {
                unsigned offset = n * 0x200000 + page * 4096;
                uint64_t physical = kvtophys(dma_space.extra + offset);
                if (!physical || physical >> 36 || physical & 4095) { return false; }
                leaf[page] = physical | 3ull | (1ull << 6) | (2ull << 8) | (1ull << 10) |
                    (1ull << 11) | (1ull << 53) | (1ull << 54);
            }
        }
        /* Only the first native 16K page holds shared contexts/rings. After
         * this flush use its uncached alias exclusively until endpoint reset. */
        dma_flush(dma_space.extra, WIFI_MHI_SIZE);
        dma_space.control = ml_io_map_unmappable(kvtophys(dma_space.extra), PAGE_SIZE, VM_WIMG_WCOMB);
        if (!dma_space.control) { return false; }
        for (unsigned offset = 0; offset < WIFI_MHI_SIZE; offset += 4096) {
            uint64_t physical = 0;
            if (!dma_walk(WIFI_MHI_IOVA + offset, &physical) || physical != kvtophys(dma_space.extra + offset)) { return false; }
        }
        uint64_t unused = 0;
        if (dma_walk(WIFI_MHI_IOVA - 4096, &unused) || dma_walk(WIFI_MHI_IOVA + WIFI_MHI_SIZE, &unused)) { return false; }
        hakiospcie_tl("wifi-pcie: MHI DMA arena verified 2048 pages with outer guards; control uses uncached alias\n");
    }
    dma_flush(dma_space.tables, WIFI_TABLE_SIZE);
    dma_flush(dma_space.data, WIFI_DMA_SIZE);
    lck_mtx_lock(&hakios_smmu_lock);
    bool used[128] = {false}, collision = false;
    dma_space.match = matches; dma_space.context = contexts;
    for (unsigned i = 0; i < matches; i++) {
        uint32_t smr = rd(dma_space.mmio, 0x800 + 4 * i), s2cr = rd(dma_space.mmio, 0xc00 + 4 * i);
        if (!(smr & (1u << 31))) { if (dma_space.match == matches) { dma_space.match = i; } continue; }
        if (((smr ^ 0x1c01) & ~((smr >> 16) & 0x7fff) & 0x7fff) == 0) { collision = true; }
        if (((s2cr >> 16) & 3) == 0 && (s2cr & 255) < 128) { used[s2cr & 255] = true; }
    }
    for (unsigned i = s2contexts; i < contexts; i++) {
        vm_offset_t cb = dma_space.mmio + 0x80000 + 4096 * i;
        if (!used[i] && !(rd(cb, 0) & 1) && !(rd(cb, 0x58) & WIFI_FSR_FAULT)) { dma_space.context = i; dma_space.cb = cb; break; }
    }
    if (collision || dma_space.match == matches || dma_space.context == contexts) {
        lck_mtx_unlock(&hakios_smmu_lock); return false;
    }
    unsigned match = dma_space.match, context = dma_space.context;
    dma_space.smr = rd(dma_space.mmio, 0x800 + 4 * match); dma_space.s2cr = rd(dma_space.mmio, 0xc00 + 4 * match);
    dma_space.cbar = rd(dma_space.mmio, 0x1000 + 4 * context); dma_space.cba2r = rd(dma_space.mmio, 0x1800 + 4 * context);
    for (unsigned i = 0; i < 9; i++) { dma_space.cb_old[i] = rd(dma_space.cb, dma_cb_offsets[i]); }
    dma_space.owned = true; /* journal is complete before the first write */
    hakiospcie_tl("wifi-pcie: claiming SMMU match %u context %u for SID 0x1c01 only\n", match, context);
    wr(dma_space.cb, 0);
    wr(dma_space.mmio + 0x1800 + 4 * context, 1);
    wr(dma_space.mmio + 0x1000 + 4 * context, (1u << 16) | (3u << 8) | (15u << 12));
    wr(dma_space.cb + 0x10, 1u | (1u << 4) | (7u << 15));
    wr(dma_space.cb + 0x30, 25u | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23));
    uint64_t root = kvtophys(dma_space.tables) | ((uint64_t)context << 48);
    wr(dma_space.cb + 0x20, root); wr(dma_space.cb + 0x24, root >> 32);
    wr(dma_space.cb + 0x28, 0); wr(dma_space.cb + 0x2c, 0);
    wr(dma_space.cb + 0x38, 0xf404ff44); wr(dma_space.cb + 0x3c, 0);
    bool ok = dma_tlb_sync();
    if (ok) {
        wr(dma_space.cb, 0x1127);
        wr(dma_space.mmio + 0xc00 + 4 * match, context);
        wr(dma_space.mmio + 0x800 + 4 * match, 0x80001c01);
        ok = rd(dma_space.mmio, 0x800 + 4 * match) == 0x80001c01 &&
            (rd(dma_space.mmio, 0xc00 + 4 * match) & 0x300ff) == context &&
            (rd(dma_space.cb, 0) & 1) && rd(dma_space.cb, 0x20) == (uint32_t)root &&
            rd(dma_space.cb, 0x24) == (uint32_t)(root >> 32);
    }
    lck_mtx_unlock(&hakios_smmu_lock);
    hakiospcie_tl("wifi-pcie: SMMU readback SMR %#x S2CR %#x CBAR %#x CBA2R %#x SCTLR %#x TCR %#x TCR2 %#x TTBR %#x:%#x expected %#llx\n",
        rd(dma_space.mmio, 0x800 + 4 * match), rd(dma_space.mmio, 0xc00 + 4 * match),
        rd(dma_space.mmio, 0x1000 + 4 * context), rd(dma_space.mmio, 0x1800 + 4 * context),
        rd(dma_space.cb, 0), rd(dma_space.cb, 0x30), rd(dma_space.cb, 0x10),
        rd(dma_space.cb, 0x24), rd(dma_space.cb, 0x20), root);
    if (!ok) { return false; }
    unsigned verified = 0;
    for (unsigned offset = 0; offset < WIFI_DMA_SIZE; offset += 4096) {
        uint64_t translated = 0;
        if (!dma_walk(WIFI_DMA_IOVA + offset, &translated) || translated != kvtophys(dma_space.data + offset)) {
            hakiospcie_tl("wifi-pcie: table page %u failed\n", offset / 4096);
            return false;
        }
        verified++;
    }
    uint64_t before = 0, after = 0;
    if (dma_walk(WIFI_DMA_IOVA - 4096, &before) || dma_walk(WIFI_DMA_IOVA + WIFI_DMA_SIZE, &after)) { return false; }
    if (!(id0 & (1u << 26))) {
        for (unsigned offset = 0; offset < WIFI_DMA_SIZE; offset += 4096) {
            uint64_t translated = 0;
            if (!dma_translate(WIFI_DMA_IOVA + offset, &translated) || (translated & 1) ||
                (translated & WIFI_PHYS_MASK) != kvtophys(dma_space.data + offset)) { return false; }
        }
        if (!dma_translate(WIFI_DMA_IOVA - 4096, &before) || !dma_translate(WIFI_DMA_IOVA + WIFI_DMA_SIZE, &after) ||
            !(before & 1) || !(after & 1)) { return false; }
        hakiospcie_tl("wifi-pcie: optional ATOS hardware translation checks passed\n");
    } else {
        hakiospcie_tl("wifi-pcie: ATOS unsupported (IDR0.ATOSNS=1); table walk is software validation only\n");
    }
    hakiospcie_tl("wifi-pcie: SMMU pre-DMA raw FSR %#x global %#x\n", rd(dma_space.cb, 0x58), rd(dma_space.mmio, 0x48));
    if ((rd(dma_space.cb, 0x58) & WIFI_FSR_FAULT) || rd(dma_space.mmio, 0x48)) { return false; }
    hakiospcie_tl("wifi-pcie: stage 15 SMMU configured; table walk verified %u pages and unmapped guards; bus master disabled\n", verified);
    return true;
}
static bool bhi_request(uint64_t address, unsigned size) {
    if (!dma_space.owned || rd(bar, 0x224) != 0x400c0110 || rd(bar, 0x28) != 0x100 ||
        rd(bar, 0x128) != 0 || (rd(dma_space.cb, 0x58) & WIFI_FSR_FAULT) || rd(dma_space.mmio, 0x48)) { return false; }
    bus_master_was_enabled = true;
    change_dbi(4, rd(dbi, 4) | 6, true);
    wr16(config + 4, (rd(config, 4) & 0xffff) | 6);
    if (!(rd(config, 4) & 4) || !(rd(dbi, 4) & 4)) { return false; }
    wr(bar + 0x12c, 0);
    wr(bar + 0x10c, address >> 32); wr(bar + 0x108, address); wr(bar + 0x110, size);
    wr(bar + 0x118, 1);
    return true;
}
static bool dma_fault_probe(void) {
    /* No firmware is supplied: ask PBL to read an unmapped guard page, then
     * require a translation fault from this exact SID before any real image. */
    uint64_t guard = WIFI_DMA_IOVA + WIFI_DMA_SIZE;
    hakiospcie_tl("wifi-pcie: stage 16 requesting PBL read of unmapped guard %#llx, 4096 bytes; no firmware supplied\n", guard);
    if (!bhi_request(guard, 4096)) { return false; }
    for (unsigned i = 0; i < 2000; i++) {
        if ((rd(dma_space.cb, 0x58) & WIFI_FSR_FAULT) || rd(dma_space.mmio, 0x48)) { break; }
        pause_us(1000);
    }
    uint32_t fsr = rd(dma_space.cb, 0x58), global = rd(dma_space.mmio, 0x48);
    uint64_t far = rd64(dma_space.cb + 0x60);
    uint32_t sid = rd(dma_space.mmio, 0x1400 + 4 * dma_space.context) & 0xffff;
    hakiospcie_tl("wifi-pcie: DMA guard result FSR %#x FAR %#llx SID %#x global %#x BHI %#x EE %u\n",
        fsr, far, sid, global, rd(bar, 0x12c), rd(bar, 0x128));
    if (!(fsr & 2) || (far & ~4095ull) != guard || sid != 0x1c01 || global) { return false; }
    hakiospcie_tl("wifi-pcie: stage 16 endpoint DMA guard blocked by owned SMMU context; SID 0x1c01 confirmed\n");
    return true;
}
static uint32_t wifi_crc32(const uint8_t *data, unsigned count) {
    uint32_t crc = ~0u;
    for (unsigned i = 0; i < count; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) { crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    }
    return ~crc;
}
extern const uint8_t *hakios_image_file(const char *, const char *, uint64_t *, int *);
static bool firmware_probe(void) {
    uint64_t size = 0;
    const uint8_t *firmware = hakios_image_file("fw", "amss.bin", &size, NULL);
    /*
     * The size and the checksum are this phone's own image, not the OnePlus 9's: the driver
     * arrived with the lemonade firmware's fingerprint (5,178,848 bytes, CRC 0xaf54a935) and
     * mars ships a different build of the same QCA6490 firmware.  This image was extracted
     * from the phone's own NON-HLOS.bin (image/amss.bin, sha256
     * 7c2b2a98265da095...); the check is still an identity check, it just names the right file.
     */
    if (!firmware || size != 5236304 || memcmp(firmware, "\177ELF", 4) ||
        wifi_crc32(firmware, WIFI_DMA_SIZE) != 0xb163aa48u) {
        hakiospcie_tl("wifi-pcie: original-device firmware missing or BHI prefix checksum mismatch\n");
        return false;
    }
    memcpy((void *)dma_space.data, firmware, WIFI_DMA_SIZE);
    if (wifi_crc32((const uint8_t *)dma_space.data, WIFI_DMA_SIZE) != 0xb163aa48u) {
        hakiospcie_tl("wifi-pcie: the copy in the DMA buffer is not the image we were given\n");
        return false;
    }
    dma_flush(dma_space.data, WIFI_DMA_SIZE);
    hakiospcie_tl("wifi-pcie: stage 17 transferring verified original-device BHI prefix, 524288 bytes\n");
    if (!bhi_request(WIFI_DMA_IOVA, WIFI_DMA_SIZE)) { return false; }
    uint32_t status = 0, ee = 0, fsr = 0, global = 0;
    bool ready = false;
    for (unsigned i = 0; i < 1000; i++) {
        status = rd(bar, 0x12c); ee = rd(bar, 0x128);
        fsr = rd(dma_space.cb, 0x58); global = rd(dma_space.mmio, 0x48);
        if ((fsr & WIFI_FSR_FAULT) || global || (status >> 30) == 3) { break; }
        if ((status >> 30) == 2 && ee == 1) { ready = true; break; }
        pause_us(10000);
    }
    hakiospcie_tl("wifi-pcie: BHI result status %#x EE %u MHI %#x FSR %#x global %#x error %#x\n",
        status, ee, rd(bar, 0x48), fsr, global, rd(bar, 0x130));
    if (!ready) { return false; }
    hakiospcie_tl("wifi-pcie: stage 17 BHI transfer complete; execution environment SBL; no SMMU fault\n");
    return true;
}
/* Polling first: PCI MSI targets an owned mailbox, without a CPU IRQ handler.
 * Device-written ring pointers and
 * records live in the uncached control alias; MMIO doors use high-then-low. */
static struct {
    unsigned channel_db, event_db, consumed[3], returned[3], events, m0_events, ee_events;
    bool m0_seen, mission_seen;
} mhi;
static const unsigned mhi_ring_offset[3] = {0x3000, 0x2000, 0x3200};
static const unsigned mhi_ring_count[3] = {32, 256, 32};
static void mhi_write_address(unsigned offset, uint64_t value) {
    wr(bar + offset + 4, value >> 32); wr(bar + offset, value);
}
static bool mhi_dma_ok(void) {
    uint32_t fsr = rd(dma_space.cb, 0x58), global = rd(dma_space.mmio, 0x48);
    if (!(fsr & WIFI_FSR_FAULT) && !global) { return true; }
    hakiospcie_tl("wifi-pcie: MHI DMA fault FSR %#x FAR %#llx global %#x\n", fsr, rd64(dma_space.cb + 0x60), global);
    return false;
}
#define MHI_IPCR_MTU 32768u
#define MHI_IPCR_RX 0x600000u
#define MHI_IPCR_TX 0x700000u
#define QRTR_CONTROL_PORT 0xfffffffeu
struct hakiospcie_qrtr_header { uint32_t version, type, source_node, source_port, confirm, size, dest_node, dest_port; };
struct hakiospcie_qrtr_control { uint32_t command, words[4]; };
static struct {
    unsigned command_next, command_code, tx_write, tx_read, rx_write, rx_read;
    unsigned tx_packets, tx_completed, rx_packets, hellos, services;
    uint64_t pending_command;
    uint32_t service_node, service_port, service_instance;
    bool channels, hello_sent, capability_seen, capability_ok;
    unsigned capability_count;
    uint8_t announcement[128];
    unsigned announcement_length;
    unsigned transaction, message, response_result, response_error, memory_count;
    bool response_seen, memory_ready;
    struct { uint32_t size, type, configs; uint64_t address; } memory[16];
} ipcr;
static bool ipcr_transfer_event(unsigned channel, unsigned code, uint64_t pointer, unsigned length);
static void mhi_context_address(vm_offset_t context, unsigned offset, uint64_t address) {
    wr(context + offset, address); wr(context + offset + 4, address >> 32);
}
static bool ipcr_send_parts(const void *head, unsigned head_length, const void *body,
    unsigned body_length, unsigned length) {
    unsigned next = hakiospcie_mhi_next(ipcr.tx_write, 32);
    if (!ipcr.channels || !length || length > MHI_IPCR_MTU || length < head_length + body_length ||
        next == ipcr.tx_read) { return false; }
    unsigned index = ipcr.tx_write;
    vm_offset_t buffer = dma_space.extra + MHI_IPCR_TX + index * MHI_IPCR_MTU;
    memcpy((void *)buffer, head, head_length);
    if (body_length) { memcpy((void *)(buffer + head_length), body, body_length); }
    if (length > head_length + body_length) {
        memset((void *)(buffer + head_length + body_length), 0, length - head_length - body_length);
    }
    dma_flush(buffer, length);
    volatile struct hakiospcie_mhi_element *tre = (void *)(dma_space.control + 0x3600 + index * 16);
    tre->pointer = WIFI_MHI_IOVA + MHI_IPCR_TX + index * MHI_IPCR_MTU;
    tre->word0 = length; tre->word1 = (2u << 16) | (1u << 9);
    __asm__ volatile("dsb sy" ::: "memory");
    ipcr.tx_write = next; ipcr.tx_packets++;
    uint64_t write = WIFI_MHI_IOVA + 0x3600 + next * 16;
    mhi_context_address(dma_space.control + 20 * 44, 36, write);
    mhi_write_address(mhi.channel_db + 20 * 8, write);
    return true;
}

static bool ipcr_send(const void *packet, unsigned length) {
    return ipcr_send_parts(packet, length, NULL, 0, length);
}
static bool ipcr_qrtr_send(uint32_t type, uint32_t source_port, uint32_t node, uint32_t port,
    const void *payload, unsigned size) {
    struct hakiospcie_qrtr_header header = {1, type, 1, source_port, 0, size, node, port};
    unsigned length = sizeof(header) + ((size + 3) & ~3u);
    if (!size || length > MHI_IPCR_MTU) { return false; }
    /* the firmware download sends thousands of bytes at once: no stack copy, no 256 byte cap */
    return ipcr_send_parts(&header, sizeof(header), payload, size, length);
}
/*
 * ath11k only tells the firmware where to put a crash dump after the memory
 * reply has been answered (ATH11K_QMI_EVENT_FW_MEM_READY), which never happens
 * here.  Say it before the reply instead, so a firmware that dies on the reply
 * still has somewhere to write its last words: QMI_WLANFW_M3_INFO_REQ_V01
 * (0x3C), TLV 1 the address (u64), TLV 2 the size (u32).
 */
static vm_offset_t wifi_m3_memory;
static uint64_t wifi_m3_address;
static unsigned wifi_m3_size;
static unsigned wifi_m3_transaction;
static void wifi_m3_tell(void) {
    uint8_t request[7 + 18] = {0};
    unsigned transaction;

    if (!wifi_m3 || wifi_m3_address) { return; }
    if (!dma_allocate(wifi_m3 << 10, true, &wifi_m3_memory, &wifi_m3_address)) {
        hakiospcie_tl("QMI probe: no dump area for the radio (%u KB)\n", wifi_m3);
        return;
    }
    wifi_m3_size = wifi_m3 << 10;
    transaction = ++wifi_m3_transaction + 0x800;
    request[1] = transaction; request[2] = transaction >> 8;
    request[3] = 0x3c; request[4] = 0;
    request[5] = 18; request[6] = 0;
    request[7] = 1; request[8] = 8; request[9] = 0;
    memcpy(request + 10, &wifi_m3_address, 8);
    request[18] = 2; request[19] = 4; request[20] = 0;
    memcpy(request + 21, &wifi_m3_size, 4);
    if (!ipcr_qrtr_send(1, 0x4000, ipcr.service_node, ipcr.service_port, request, sizeof(request))) {
        hakiospcie_tl("QMI probe: the dump area could not be announced\n");
        return;
    }
    hakiospcie_tl("QMI probe: told the radio its dump area is %#llx, %u bytes\n",
        wifi_m3_address, wifi_m3_size);
}
/*
 * The memory request has to be understood here, not further down: once ath11k is running
 * every packet goes straight to driver_receive() and never reaches the TLV walk in
 * ipcr_receive(), so ipcr.memory[] stayed empty and the reply was built from ath11k's
 * one-byte-off decode of the request.
 */
void
hakiospcie_note_memory_request(const uint8_t *data, unsigned size)
{
    unsigned at = 0, found = 0;

    if (ipcr.memory_count || size < 22) { return; }

    /* Do not guess the QRTR framing: look for the QMI indication itself. Its size field
     * must agree with the bytes that follow, which is what makes the match safe. */
    for (unsigned i = 0; i + 7 <= size; i++) {
        if (data[i] != 4 || data[i + 3] != 0x35 || data[i + 4] != 0) { continue; }
        if ((unsigned)(data[i + 5] | (data[i + 6] << 8)) == size - i - 7) { at = i; found = 1; break; }
    }
    if (!found) {
        return;
    }

    const uint8_t *qmi = data + at;
    unsigned end_all = size - at, pos = 7;
    hakiospcie_tl("QMI probe: 0x35 found at %u, %u bytes of body\n", at, end_all - 7);
    for (unsigned o = 0; o < end_all && o < 40; o += 8) {
        hakiospcie_tl("QMI probe: 0x35 %02u: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            o, o + 0 < end_all ? qmi[o + 0] : 0, o + 1 < end_all ? qmi[o + 1] : 0,
            o + 2 < end_all ? qmi[o + 2] : 0, o + 3 < end_all ? qmi[o + 3] : 0,
            o + 4 < end_all ? qmi[o + 4] : 0, o + 5 < end_all ? qmi[o + 5] : 0,
            o + 6 < end_all ? qmi[o + 6] : 0, o + 7 < end_all ? qmi[o + 7] : 0);
    }
    while (pos + 3 <= end_all) {
        unsigned type = qmi[pos], length = qmi[pos + 1] | (qmi[pos + 2] << 8); pos += 3;
        if (length > end_all - pos) { hakiospcie_tl("QMI probe: 0x35 tlv overruns\n"); return; }
        if (type == 1 && length && qmi[pos] && qmi[pos] <= 16) {
            unsigned count = qmi[pos], at2 = pos + 1, end = pos + length;
            for (unsigned i = 0; i < count; i++) {
                if (end - at2 < 9) { return; }
                memcpy(&ipcr.memory[i].size, qmi + at2, 4);
                memcpy(&ipcr.memory[i].type, qmi + at2 + 4, 4);
                unsigned configs = qmi[at2 + 8]; at2 += 9;
                if (configs > 2 || configs * 13 > end - at2) { return; }
                ipcr.memory[i].configs = configs; at2 += configs * 13;
                hakiospcie_tl("QMI probe: the radio asks for type %u, %u bytes, %u configs\n",
                    ipcr.memory[i].type, ipcr.memory[i].size, configs);
            }
            if (at2 != end) { return; }
            ipcr.memory_count = count;
            hakiospcie_tl("QMI probe: the request is understood, %u segments\n", count);
            wifi_m3_tell();
        }
        pos += length;
    }
}

static bool ipcr_receive(const uint8_t *data, unsigned size) {
    hakiospcie_note_memory_request(data, size);
    if (driver_runtime.enabled) { ipcr.rx_packets++; return driver_receive(data, size); }
    struct hakiospcie_qrtr_header header = {0};
    unsigned offset;
    if (size >= sizeof(header) && data[0] == 1) {
        memcpy(&header, data, sizeof(header)); offset = sizeof(header);
    } else if (size >= 16 && data[0] == 3) {
        header.type = data[1]; header.confirm = data[2] & 1;
        memcpy(&header.size, data + 4, 4);
        header.source_node = data[8] | (data[9] << 8); header.source_port = data[10] | (data[11] << 8);
        header.dest_node = data[12] | (data[13] << 8); header.dest_port = data[14] | (data[15] << 8);
        if (header.source_port == 0xfffe) { header.source_port = QRTR_CONTROL_PORT; }
        if (header.dest_port == 0xfffe) { header.dest_port = QRTR_CONTROL_PORT; }
        offset = 16 + data[3];
    } else { hakiospcie_tl("wifi-pcie: IPCR packet has unsupported QRTR version/length\n"); return false; }
    if (offset > size || !header.size || header.size > size - offset) { return false; }
    ipcr.rx_packets++;
    if (ipcr.rx_packets <= 20) {
        hakiospcie_tl("wifi-pcie: QRTR packet type %u payload %u bytes\n", header.type, header.size);
    }
    if (header.confirm) {
        struct hakiospcie_qrtr_control resume = {7, {header.dest_node, header.dest_port, 0, 0}};
        if (!ipcr_qrtr_send(7, header.dest_port, header.source_node, header.source_port, &resume, sizeof(resume))) { return false; }
    }
    if (header.dest_port == QRTR_CONTROL_PORT && header.size >= 4) {
        struct hakiospcie_qrtr_control control = {0};
        memcpy(&control, data + offset, header.size < sizeof(control) ? header.size : sizeof(control));
        if (control.command == 2) {
            ipcr.hellos++;
            if (!ipcr.hello_sent) {
                struct hakiospcie_qrtr_control hello = {2, {0}};
                if (!ipcr_qrtr_send(2, QRTR_CONTROL_PORT, header.source_node, QRTR_CONTROL_PORT, &hello, sizeof(hello))) { return false; }
                ipcr.hello_sent = true;
            }
        } else if (control.command == 4 && header.size >= sizeof(control)) {
            ipcr.services++;
            hakiospcie_tl("wifi-pcie: QRTR service %#x version %u instance %u announced\n",
                control.words[0], control.words[1] & 255, control.words[1] >> 8);
            if (control.words[0] == 0x45 && (control.words[1] & 255) == 1) {
                ipcr.service_node = control.words[2]; ipcr.service_port = control.words[3];
                ipcr.service_instance = control.words[1] >> 8;
                if (size <= sizeof(ipcr.announcement)) {
                    memcpy(ipcr.announcement, data, size); ipcr.announcement_length = size;
                }
            }
        }
    } else if (header.type == 1 && header.dest_port == 0x4000 &&
        header.source_node == ipcr.service_node && header.source_port == ipcr.service_port) {
        const uint8_t *qmi = data + offset;
        if (header.size < 7) { return false; }
        unsigned transaction = qmi[1] | (qmi[2] << 8), message = qmi[3] | (qmi[4] << 8);
        unsigned payload = qmi[5] | (qmi[6] << 8);
        if (payload != header.size - 7) { return false; }
        hakiospcie_tl("wifi-pcie: QMI packet type %u transaction %u message %#x payload %u bytes\n", qmi[0], transaction, message, payload);
        if (qmi[0] == 2 && transaction == ipcr.transaction && message == ipcr.message) {
            unsigned at = 7;
            while (at + 3 <= header.size) {
                unsigned type = qmi[at], length = qmi[at + 1] | (qmi[at + 2] << 8); at += 3;
                if (length > header.size - at) { return false; }
                if (type == 2 && length == 4) {
                    unsigned result = qmi[at] | (qmi[at + 1] << 8), error = qmi[at + 2] | (qmi[at + 3] << 8);
                    ipcr.response_seen = true; ipcr.response_result = result; ipcr.response_error = error;
                    hakiospcie_tl("wifi-pcie: WLFW response message %#x result %u error %u\n", message, result, error);
                    if (message == 0x24) {
                        ipcr.capability_seen = true; ipcr.capability_ok = result == 0 && error == 0;
                        ipcr.capability_count++;
                        hakiospcie_tl("wifi-pcie: WLFW capability response result %u error %u\n", result, error);
                    }
                }
                at += length;
            }
            if (at != header.size) { return false; }
        } else if (qmi[0] == 4 && message == 0x35) {
            unsigned at = 7;
            hakiospcie_tl("QMI probe: 0x35 in, %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                header.size, qmi[0], qmi[1], qmi[2], qmi[3], qmi[4], qmi[5], qmi[6], qmi[7]);
            while (at + 3 <= header.size) {
                unsigned type = qmi[at], length = qmi[at + 1] | (qmi[at + 2] << 8); at += 3;
                hakiospcie_tl("QMI probe: tlv type %u length %u\n", type, length);
                if (length > header.size - at) { hakiospcie_tl("QMI probe: tlv overruns\n"); return false; }
                if (type == 1) {
                    if (!length || !qmi[at] || qmi[at] > 16 || ipcr.memory_count) {
                        hakiospcie_tl("QMI probe: count %u length %u refused\n", qmi[at], length);
                        return false;
                    }
                    unsigned count = qmi[at], pos = at + 1, end = at + length;
                    for (unsigned i = 0; i < count; i++) {
                        if (end - pos < 9) { hakiospcie_tl("QMI probe: segment %u short\n", i); return false; }
                        memcpy(&ipcr.memory[i].size, qmi + pos, 4);
                        memcpy(&ipcr.memory[i].type, qmi + pos + 4, 4);
                        unsigned configs = qmi[pos + 8]; pos += 9;
                        if (configs > 2 || configs * 13 > end - pos) {
                            hakiospcie_tl("QMI probe: segment %u configs %u bad\n", i, configs);
                            return false;
                        }
                        ipcr.memory[i].configs = configs; pos += configs * 13;
                        hakiospcie_tl("wifi-pcie: WLFW memory request %u type %u bytes %u configs %u\n",
                            i, ipcr.memory[i].type, ipcr.memory[i].size, configs);
                    }
                    if (pos != end) { hakiospcie_tl("QMI probe: %u bytes left over\n", end - pos); return false; }
                    ipcr.memory_count = count;
                    hakiospcie_tl("QMI probe: the request is understood, %u segments\n", count);
                }
                at += length;
            }
            if (at != header.size) { hakiospcie_tl("QMI probe: ended at %u of %u\n", at, header.size); return false; }
        } else if (qmi[0] == 4 && message == 0x37) {
            ipcr.memory_ready = true;
            hakiospcie_tl("wifi-pcie: WLFW firmware memory ready indication\n");
        }
    }
    return true;
}
static bool ipcr_transfer_event(unsigned channel, unsigned code, uint64_t pointer, unsigned length) {
    if (!ipcr.channels || (channel != 20 && channel != 21)) { return false; }
    unsigned ring_offset = channel == 20 ? 0x3600 : 0x3800;
    uint64_t base = WIFI_MHI_IOVA + ring_offset;
    if (code == 6) {
        unsigned index = channel == 20 ? ipcr.tx_write : ipcr.rx_write;
        mhi_write_address(mhi.channel_db + channel * 8, base + index * 16);
        return true;
    }
    if ((code != 2 && code != 4) || !hakiospcie_mhi_pointer_valid(pointer, base, 32)) {
        hakiospcie_tl("QMI probe: RX rejected ch %u code %u length %u pointer %#llx\n",
            channel, code, length, (unsigned long long)pointer);
        return false;
    }
    unsigned index = (unsigned)((pointer - base) / 16);
    if (channel == 20) {
        unsigned outstanding = (ipcr.tx_write - ipcr.tx_read) & 31;
        unsigned done = ((index - ipcr.tx_read) & 31) + 1;
        if (!outstanding || done > outstanding) { return false; }
        ipcr.tx_read = hakiospcie_mhi_next(index, 32); ipcr.tx_completed += done;
        return true;
    }
    if (index != ipcr.rx_read || !length || length > MHI_IPCR_MTU) {
        hakiospcie_tl("QMI probe: RX out of order ch %u index %u rx_read %u length %u\n",
            channel, index, ipcr.rx_read, length);
        return false;
    }
    vm_offset_t buffer = dma_space.extra + MHI_IPCR_RX + index * MHI_IPCR_MTU;
    /* RX buffers are never written by the CPU after initial zero/flush. */
    dma_flush(buffer, MHI_IPCR_MTU);
    bool ok = ipcr_receive((const uint8_t *)buffer, length);
    dma_flush(buffer, MHI_IPCR_MTU);
    if (!ok) { return false; }
    ipcr.rx_read = hakiospcie_mhi_next(ipcr.rx_read, 32);
    ipcr.rx_write = hakiospcie_mhi_next(ipcr.rx_write, 32);
    if (ipcr.rx_packets <= 40) {
        /* HAKIOS: the radio goes quiet partway through a download; these are
         * the four numbers that say whether the host gave its buffers back. */
        uint64_t ev = ((uint64_t)rd(dma_space.control + 0x800 + 1 * 44, 32) << 32) |
            rd(dma_space.control + 0x800 + 1 * 44, 28);
        hakiospcie_tl("QMI ring: in %u of %u bytes at index %u; rx read %u write %u; "
            "event read %#llx consumed %u returned %u; rx packets %u\n",
            length, MHI_IPCR_MTU, index, ipcr.rx_read, ipcr.rx_write,
            (unsigned long long)ev, mhi.consumed[1], mhi.returned[1], ipcr.rx_packets);
    }
    uint64_t write = base + ipcr.rx_write * 16;
    mhi_context_address(dma_space.control + 21 * 44, 36, write);
    mhi_write_address(mhi.channel_db + 21 * 8, write);
    return true;
}

static bool mhi_events(void) {

    if (!mhi_dma_ok()) { return false; }
    for (unsigned n = 0; n < 3; n++) {
        uint64_t base = WIFI_MHI_IOVA + mhi_ring_offset[n];
        unsigned budget = mhi_ring_count[n];
        bool recycled = false;
        while (budget--) {
            __asm__ volatile("dmb oshld" ::: "memory");
            vm_offset_t context = dma_space.control + 0x800 + n * 44;
            uint64_t pointer = ((uint64_t)rd(context, 32) << 32) | rd(context, 28);
            if (!hakiospcie_mhi_pointer_valid(pointer, base, mhi_ring_count[n])) {
                hakiospcie_tl("wifi-pcie: MHI event ring %u invalid read pointer %#llx\n", n, pointer);
                for (unsigned word = 0; word < 11; word++) {
                    hakiospcie_tl("wifi-pcie: MHI context %u word %u = %#x\n", n, word, rd(context, 4 * word));
                }
                vm_offset_t event_base = dma_space.control + mhi_ring_offset[n];
                hakiospcie_tl("wifi-pcie: MHI first event words %#x %#x %#x %#x next %#x %#x %#x %#x\n",
                    rd(event_base, 0), rd(event_base, 4), rd(event_base, 8), rd(event_base, 12),
                    rd(event_base, 16), rd(event_base, 20), rd(event_base, 24), rd(event_base, 28));
                return false;
            }
            if (pointer == base + mhi.consumed[n] * 16) { break; }
            volatile struct hakiospcie_mhi_element *event = (void *)(dma_space.control +
                mhi_ring_offset[n] + mhi.consumed[n] * 16);
            uint32_t word0 = event->word0, word1 = event->word1;
            unsigned type = (word1 >> 16) & 255, value = word0 >> 24;
            if (n == 0) {
                hakiospcie_tl("wifi-pcie: MHI event ring 0 raw word0 %#x word1 %#x at %#llx\n",
                    word0, word1, (unsigned long long)(base + mhi.consumed[n] * 16));
            }
            if (mhi.events < 32) {
                hakiospcie_tl("wifi-pcie: MHI event ring %u type %#x value %u channel %u\n", n, type, value, word1 >> 24);
            }
            mhi.events++;
            if (type == 0x20) {
                if (value == 2) { mhi.m0_seen = true; mhi.m0_events++; }
                else if (value == 0xff) {
                    /* HAKIOS: the radio reports 0xff here right after it is handed its
                     * memory. Nobody made it say that on 15 November, so keep reading
                     * events and log what the entry really held. */
                    hakiospcie_tl("wifi-pcie: MHI event ring %u says 0xff: word0 %#x word1 %#x "
                        "read %#llx consumed %u returned %u; carrying on\n", n, word0, word1,
                        (unsigned long long)(base + mhi.consumed[n] * 16), mhi.consumed[n],
                        mhi.returned[n]);
                }
            } else if (type == 0x40) {
                mhi.ee_events++;
                if (value == 2 || value == 4) { mhi.mission_seen = true; }
                if (value == 3) { return false; } /* crash-dump environment */
            } else if (type == 0x21) {
                if (!ipcr.pending_command || event->pointer != ipcr.pending_command || value != 1) {
                    hakiospcie_tl("wifi-pcie: IPCR command completion mismatch code %u\n", value);
                    return false;
                }
                ipcr.command_code = value;
            } else if (type == 0x22) {
                if (!ipcr_transfer_event(word1 >> 24, value, event->pointer, word0 & 0xffff)) { return false; }
            } else if (type != 0x50) {
                hakiospcie_tl("wifi-pcie: unsupported MHI event type %#x\n", type);
                return false;
            }
            mhi.consumed[n] = hakiospcie_mhi_next(mhi.consumed[n], mhi_ring_count[n]);
            mhi.returned[n] = hakiospcie_mhi_next(mhi.returned[n], mhi_ring_count[n]);
            wr(context + 36, (uint32_t)(base + mhi.returned[n] * 16));
            wr(context + 40, 0);
            recycled = true;
        }
        if (recycled) {
            __asm__ volatile("dsb sy" ::: "memory");
            mhi_write_address(mhi.event_db + n * 8, base + mhi.returned[n] * 16);
        }
    }
    return mhi_dma_ok();
}
/* Give MHI's interrupt writes a valid owned destination even while polling.
 * This is a DMA mailbox, not a GIC interrupt: SMMU protects it like the rings. */
static bool mhi_msi_prepare(void) {
    unsigned next = rd(config, 0x34) & 255;
    for (unsigned count = 0; count < 32 && next; count++) {
        if (next < 0x40 || next > 0xe8 || (next & 3)) { return false; }
        uint32_t cap = rd(config, next);
        if ((cap & 255) != 5) { next = (cap >> 8) & 255; continue; }
        uint32_t control = cap >> 16;
        hakiospcie_tl("wifi-pcie: PCI MSI capability %#x control %#x\n", next, control);
        if ((control & 1) || ((control >> 1) & 7) < 2) { return false; }
        msi_offset = next; msi_old_control = control;
        msi_data_offset = next + ((control & 0x80) ? 12 : 8);
        msi_mask_offset = msi_data_offset + 4;
        msi_old_low = rd(config, next + 4); msi_old_high = (control & 0x80) ? rd(config, next + 8) : 0;
        msi_old_data = rd(config, msi_data_offset) & 0xffff;
        if (control & 0x100) { msi_old_mask = rd(config, msi_mask_offset); }
        msi_saved = true;
        wr(dma_space.control + 0x1800, 0xfeed0000);
        wr(config + next + 4, (uint32_t)(WIFI_MHI_IOVA + 0x1800));
        if (control & 0x80) { wr(config + next + 8, 0); }
        wr16(config + msi_data_offset, 0x100);
        if (control & 0x100) { wr(config + msi_mask_offset, msi_old_mask & ~15u); }
        wr16(config + next + 2, (control & ~0x70u) | 0x21u); /* four messages */
        bool ok = ((rd(config, next) >> 16) & 0x71) == 0x21 &&
            rd(config, next + 4) == WIFI_MHI_IOVA + 0x1800 &&
            (!(control & 0x80) || !rd(config, next + 8));
        hakiospcie_tl("wifi-pcie: PCI MSI owned-mailbox configuration %s; four vectors, CPU IRQ disabled\n", ok ? "verified" : "failed");
        return ok;
    }
    return false;
}

static bool mhi_ready(void) {
    if (!dma_space.control || (rd(bar, 0x38) & 2) || !(rd(bar, 0x48) & 1) ||
        rd(bar, 0x128) != 1) { return false; }
    if (!mhi_msi_prepare()) { return false; }
    mhi.channel_db = rd(bar, 0x18); mhi.event_db = rd(bar, 0x20);
    if (mhi.channel_db < 0x100 || mhi.channel_db > 0x1ff000 || (mhi.channel_db & 7) ||
        mhi.event_db < 0x100 || mhi.event_db > 0x1ff000 || (mhi.event_db & 7)) { return false; }
    hakiospcie_tl("wifi-pcie: MHI control alias physical %#llx cached physical %#llx next %#llx\n",
        kvtophys(dma_space.control), kvtophys(dma_space.extra), kvtophys(dma_space.extra + 0x3000));
    volatile struct hakiospcie_mhi_context *channels = (void *)dma_space.control;
    volatile struct hakiospcie_mhi_context *events = (void *)(dma_space.control + 0x800);
    volatile struct hakiospcie_mhi_context *command = (void *)(dma_space.control + 0xc00);
    /* IPCR is channel pair 20/21 in this board's original MHI configuration.
     * Leave all channels disabled until a later explicit START command. */
    channels[20].type = 1; channels[20].index = 1;
    channels[21].type = 2; channels[21].index = 1;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t base = WIFI_MHI_IOVA + mhi_ring_offset[i];
        events[i].type = 1;
        events[i].index = i == 2 ? 0 : i + 1;
        events[i].base = base; events[i].length = mhi_ring_count[i] * 16;
        events[i].read = base; events[i].write = base + (mhi_ring_count[i] - 1) * 16;
        mhi.returned[i] = mhi_ring_count[i] - 1;
    }
    command->base = WIFI_MHI_IOVA + 0x3400; command->length = 32 * 16;
    command->read = command->write = command->base;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned n = 0; n < 3; n++) {
        vm_offset_t context = dma_space.control + 0x800 + n * 44;
        hakiospcie_tl("wifi-pcie: MHI context %u before publish cfg/type/index %#x/%#x/%#x base %#x:%#x len %#x:%#x rp %#x:%#x wp %#x:%#x\n",
            n, rd(context, 0), rd(context, 4), rd(context, 8), rd(context, 16), rd(context, 12),
            rd(context, 24), rd(context, 20), rd(context, 32), rd(context, 28), rd(context, 40), rd(context, 36));
        if (rd(context, 12) != WIFI_MHI_IOVA + mhi_ring_offset[n] || rd(context, 16) != 0 ||
            rd(context, 28) != WIFI_MHI_IOVA + mhi_ring_offset[n] || rd(context, 32) != 0) {
            hakiospcie_tl("wifi-pcie: MHI context readback before publish failed\n");
            return false;
        }
    }
    mhi_write_address(mhi.channel_db + 127 * 8, 0);
    mhi_write_address(0x58, WIFI_MHI_IOVA);
    mhi_write_address(0x60, WIFI_MHI_IOVA + 0x800);
    mhi_write_address(0x68, WIFI_MHI_IOVA + 0xc00);
    wr(bar + 0x10, (rd(bar, 0x10) & 0xffff) | (3u << 16));
    /* Match the original board's advertised MHI window. This advertises
     * addresses only: SMMU still maps just our two bounded allocations. */
    mhi_write_address(0x80, 0xa0000000ull);
    mhi_write_address(0x88, 0xb0000000ull);
    mhi_write_address(0x98, 0xa0000000ull);
    mhi_write_address(0xa0, 0xb0000000ull);
    if (rd(bar, 0x58) != WIFI_MHI_IOVA || rd(bar, 0x60) != WIFI_MHI_IOVA + 0x800 ||
        ((rd(bar, 0x10) >> 16) & 0xffff) != 3) { return false; }
    for (unsigned i = 0; i < 3; i++) { mhi_write_address(mhi.event_db + i * 8, events[i].write); }
    hakiospcie_tl("wifi-pcie: MHI contexts published NER 3 CHDB %#x ERDB %#x CFG %#x VER %#x; requesting M0\n", mhi.channel_db, mhi.event_db, rd(bar, 0x10), rd(bar, 8));
    wr(bar + 0x38, 2u << 8);
    mhi_write_address(mhi.channel_db + 127 * 8, 1); /* keep firmware awake */
    bool ready = false;
    for (unsigned i = 0; i < 5000; i++) {
        if (!mhi_events()) { break; }
        uint32_t status = rd(bar, 0x48);
        if ((status & 4) || ((status >> 8) & 255) == 0xff) { break; }
        if (((status >> 8) & 255) == 2 && mhi.m0_seen) { ready = true; break; }
        pause_us(1000);
    }
    hakiospcie_tl("wifi-pcie: MHI ready result status %#x EE %u events %u M0events %u EEevents %u FSR %#x global %#x\n",
        rd(bar, 0x48), rd(bar, 0x128), mhi.events, mhi.m0_events, mhi.ee_events,
        rd(dma_space.cb, 0x58), rd(dma_space.mmio, 0x48));
    hakiospcie_tl("wifi-pcie: MHI MSI mailbox %#x\n", rd(dma_space.control, 0x1800));
    if (!ready || !mhi_dma_ok()) { return false; }
    hakiospcie_tl("wifi-pcie: stage 18 MHI M0 confirmed by device-written event; polling transport ready\n");
    return true;
}
static bool mhi_full_firmware(void) {
    uint64_t size = 0;
    const uint8_t *image = hakios_image_file("fw", "amss.bin", &size, NULL);
    /* mars's own build of the same QCA6490 firmware (see firmware_probe above). */
    if (!image || size != 5236304 || wifi_crc32(image, size) != 0xb4376a2au) { return false; }
    unsigned bhie = rd(bar, 0x2c);
    if (bhie < 0x100 || bhie > 0x1fff00 || bhie & 3) { return false; }
    vm_offset_t copy = dma_space.extra + 0x20000;
    memcpy((void *)copy, image, size);
    if (wifi_crc32((const uint8_t *)copy, size) != 0xb4376a2au) { return false; }
    dma_flush(copy, size);
    volatile struct hakiospcie_bhie_vector *vectors = (void *)(dma_space.control + 0x1000);
    unsigned count = 0;
    for (unsigned offset = 0; offset < size; offset += WIFI_DMA_SIZE) {
        vectors[count].address = WIFI_MHI_IOVA + 0x20000 + offset;
        vectors[count].length = size - offset < WIFI_DMA_SIZE ? size - offset : WIFI_DMA_SIZE;
        count++;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    hakiospcie_tl("wifi-pcie: stage 19 BHIE original firmware %u bytes, %u vectors, offset %#x\n", (unsigned)size, count, bhie);
    mhi_write_address(bhie + 0x2c, WIFI_MHI_IOVA + 0x1000);
    wr(bar + bhie + 0x34, count * sizeof(*vectors));
    wr(bar + bhie + 0x3c, (rd(bar, bhie + 0x3c) & 0xc0000000u) | 2);
    bool complete = false;
    for (unsigned i = 0; i < 15000; i++) {
        if (!mhi_events()) { break; }
        uint32_t status = rd(bar, bhie + 0x44), ee = rd(bar, 0x128), state = rd(bar, 0x48);
        if ((status >> 30) == 3 || ee == 3 || (state & 4)) { break; }
        if ((status >> 30) == 2 && (ee == 2 || ee == 4) && mhi.mission_seen) { complete = true; break; }
        pause_us(1000);
    }
    hakiospcie_tl("wifi-pcie: BHIE result status %#x EE %u MHI %#x events %u mission_event %u FSR %#x global %#x\n",
        rd(bar, bhie + 0x44), rd(bar, 0x128), rd(bar, 0x48), mhi.events, mhi.mission_seen,
        rd(dma_space.cb, 0x58), rd(dma_space.mmio, 0x48));
    if (!complete || !mhi_dma_ok()) { return false; }
    hakiospcie_tl("wifi-pcie: stage 19 full firmware running; mission environment confirmed by event; no SMMU fault\n");
    return true;
}
static bool ipcr_start_command(unsigned channel) {
    unsigned index = ipcr.command_next;
    uint64_t address = WIFI_MHI_IOVA + 0x3400 + index * 16;
    volatile struct hakiospcie_mhi_element *command = (void *)(dma_space.control + 0x3400 + index * 16);
    command->pointer = 0; command->word0 = 0; command->word1 = (channel << 24) | (0x12u << 16);
    ipcr.pending_command = address; ipcr.command_code = 0;
    ipcr.command_next = hakiospcie_mhi_next(index, 32);
    uint64_t write = WIFI_MHI_IOVA + 0x3400 + ipcr.command_next * 16;
    mhi_context_address(dma_space.control + 0xc00, 36, write);
    __asm__ volatile("dsb sy" ::: "memory");
    mhi_write_address(0x70, write);
    for (unsigned i = 0; i < 3000 && !ipcr.command_code; i++) {
        if (!mhi_events()) { return false; }
        pause_us(1000);
    }
    hakiospcie_tl("wifi-pcie: IPCR channel %u START completion %u\n", channel, ipcr.command_code);
    ipcr.pending_command = 0;
    return ipcr.command_code == 1;
}
static bool ipcr_qmi_request(unsigned message, const void *payload, unsigned length) {
    uint8_t request[192] = {0};
    if (length > sizeof(request) - 7 || ipcr.transaction == 0xffff) { return false; }
    unsigned transaction = ++ipcr.transaction;
    ipcr.message = message; ipcr.response_seen = false;
    request[1] = transaction; request[2] = transaction >> 8;
    request[3] = message; request[4] = message >> 8;
    request[5] = length; request[6] = length >> 8;
    if (length) { memcpy(request + 7, payload, length); }
    if (!ipcr_qrtr_send(1, 0x4000, ipcr.service_node, ipcr.service_port, request, length + 7)) { return false; }
    for (unsigned i = 0; i < 5000 && !ipcr.response_seen; i++) {
        if (!mhi_events()) { return false; }
        pause_us(1000);
    }
    if (!ipcr.response_seen) {
        /* HAKIOS: no answer came; say what the rings looked like when it stopped. */
        uint64_t ev = ((uint64_t)rd(dma_space.control + 0x800 + 1 * 44, 32) << 32) |
            rd(dma_space.control + 0x800 + 1 * 44, 28);
        hakiospcie_tl("QMI wait: no answer to 0x%x; rx read %u write %u packets %u; "
            "event read %#llx consumed %u returned %u; events %u; FSR %#x global %#x\n",
            message, ipcr.rx_read, ipcr.rx_write, ipcr.rx_packets,
            (unsigned long long)ev, mhi.consumed[1], mhi.returned[1], mhi.events,
            rd(dma_space.cb, 0x58), rd(dma_space.mmio, 0x48));
    }
    return ipcr.response_seen && !ipcr.response_result && !ipcr.response_error;
}
static bool ipcr_probe(unsigned step) {
    if (!mhi.mission_seen || (rd(bar, 0x128) != 2 && rd(bar, 0x128) != 4)) { return false; }
    for (unsigned channel = 20; channel <= 21; channel++) {
        vm_offset_t context = dma_space.control + channel * 44;
        uint64_t base = WIFI_MHI_IOVA + (channel == 20 ? 0x3600 : 0x3800);
        wr(context, 1u | (2u << 8)); wr(context + 4, channel == 20 ? 1 : 2); wr(context + 8, 1);
        mhi_context_address(context, 12, base); mhi_context_address(context, 20, 32 * 16);
        mhi_context_address(context, 28, base); mhi_context_address(context, 36, base);
    }
    for (unsigned i = 0; i < 32; i++) {
        volatile struct hakiospcie_mhi_element *tre = (void *)(dma_space.control + 0x3800 + i * 16);
        tre->pointer = WIFI_MHI_IOVA + MHI_IPCR_RX + i * MHI_IPCR_MTU;
        tre->word0 = MHI_IPCR_MTU; tre->word1 = (2u << 16) | (1u << 9);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    ipcr.channels = true;
    if (!ipcr_start_command(20) || !ipcr_start_command(21)) { return false; }
    ipcr.rx_write = 31;
    mhi_context_address(dma_space.control + 21 * 44, 36, WIFI_MHI_IOVA + 0x3800 + 31 * 16);
    mhi_write_address(mhi.channel_db + 21 * 8, WIFI_MHI_IOVA + 0x3800 + 31 * 16);
    struct hakiospcie_qrtr_control hello = {2, {0}};
    if (!ipcr_qrtr_send(2, QRTR_CONTROL_PORT, 0xffffffffu, QRTR_CONTROL_PORT, &hello, sizeof(hello))) { return false; }
    bool ready = false;
    for (unsigned i = 0; i < 5000; i++) {
        if (!mhi_events()) { return false; }
        if (ipcr.service_port && ipcr.tx_completed && ipcr.rx_packets) { ready = true; break; }
        pause_us(1000);
    }
    hakiospcie_tl("wifi-pcie: IPCR result tx %u completed %u rx %u hellos %u services %u WLFW %u instance %u FSR %#x global %#x\n",
        ipcr.tx_packets, ipcr.tx_completed, ipcr.rx_packets, ipcr.hellos, ipcr.services,
        ipcr.service_port != 0, ipcr.service_instance, rd(dma_space.cb, 0x58), rd(dma_space.mmio, 0x48));
    if (!ready || !mhi_dma_ok()) { return false; }
    hakiospcie_tl("wifi-pcie: stage 20 IPCR channels 20/21 active; QRTR bidirectional and WLFW service found\n");
    if (step == 20 || step >= 22) { return true; }
    /* WLFW requires the client registration and host capabilities before a
     * target capability query. Subscribe to the PCIe memory handshake. */
    static const uint8_t registration[] = {
        0x15,4,0, 0x4c,0x45,0x4e,0x4b, /* kernel client */
        0x16,1,0,1, 0x17,1,0,1, 0x18,1,0,1, 0x1b,1,0,1
    };
    static const uint8_t host[] = {
        0x10,4,0,1,0,0,0, /* one client */
        0x14,1,0,1, /* board data */
        0x16,1,0,1, 0x17,1,0,1, /* original M3 image/cache */
        0x1a,1,0,0 /* calibration not yet done */
    };
    if (!ipcr_qmi_request(0x20, registration, sizeof(registration)) ||
        !ipcr_qmi_request(0x34, host, sizeof(host))) { return false; }
    for (unsigned i = 0; i < 2000 && !ipcr.memory_count && !ipcr.memory_ready; i++) {
        if (!mhi_events()) { return false; }
        pause_us(1000);
    }
    if (ipcr.memory_count) {
        uint8_t response[4 + 17 * 8] = {1, 0, 0, 0};
        if (ipcr.memory_count > 8) { return false; }
        unsigned length = 1 + 17 * ipcr.memory_count;
        response[1] = length; response[2] = length >> 8; response[3] = ipcr.memory_count;
        for (unsigned i = 0; i < ipcr.memory_count; i++) {
            vm_offset_t memory = 0;
            if (ipcr.memory[i].configs || (ipcr.memory[i].type != 1 && ipcr.memory[i].type != 4) ||
                !dma_allocate(ipcr.memory[i].size, false, &memory, &ipcr.memory[i].address)) { return false; }
            memcpy(response + 4 + 17 * i, &ipcr.memory[i].address, 8);
            memcpy(response + 12 + 17 * i, &ipcr.memory[i].size, 4);
            memcpy(response + 16 + 17 * i, &ipcr.memory[i].type, 4);
        }
        if (!ipcr_qmi_request(0x36, response, length + 3)) { return false; }
        for (unsigned i = 0; i < 10000 && !ipcr.memory_ready; i++) {
            if (!mhi_events()) { return false; }
            pause_us(1000);
        }
        if (!ipcr.memory_ready) { return false; }
    }
    if (!ipcr_qmi_request(0x24, NULL, 0)) { return false; }
    hakiospcie_tl("wifi-pcie: QMI result response %u success %u tx_completed %u rx %u FSR %#x global %#x\n",
        ipcr.capability_seen, ipcr.capability_ok, ipcr.tx_completed, ipcr.rx_packets,
        rd(dma_space.cb, 0x58), rd(dma_space.mmio, 0x48));
    if (!ipcr.capability_seen || !ipcr.capability_ok || !mhi_dma_ok()) { return false; }
    hakiospcie_tl("wifi-pcie: stage 21 WLFW capability request/response successful over QMI; no SMMU fault\n");
    for (unsigned n = 0; n < 8; n++) {
        unsigned before = ipcr.capability_count;
        if (!ipcr_qmi_request(0x24, NULL, 0)) {
            hakiospcie_tl("QMI probe: ping %u could not be sent\n", n);
            break;
        }
        for (unsigned i = 0; i < 3000 && ipcr.capability_count == before; i++) {
            if (!mhi_events()) { break; }
            pause_us(1000);
        }
        hakiospcie_tl("QMI probe: ping %u answered %u (count %u, rx %u)\n", n,
            ipcr.capability_count != before, ipcr.capability_count, ipcr.rx_packets);
        if (ipcr.capability_count == before) { break; }
    }
    return true;
}

unsigned hakiospcie_wifi_driver_mode(void) {
    return driver_runtime.enabled ? wifi_mode : 0;
}
void *hakiospcie_wifi_driver_memory(unsigned long size, unsigned long long *iova) {
    vm_offset_t memory = 0; uint64_t address = 0;
    if (wifi_mem_real == 4) { hakiospcie_tl("wifi-pcie: radio memory: skipped by the wifi_mem_real=4 argument\n"); return NULL; }
    if (!hakiospcie_wifi_driver_mode()) { hakiospcie_tl("wifi-pcie: radio memory: not in driver mode\n"); return NULL; }
    if (driver_runtime.failed) { hakiospcie_tl("wifi-pcie: radio memory: the driver has already failed\n"); return NULL; }
    if (size > 64u * 1024 * 1024) { hakiospcie_tl("wifi-pcie: radio memory: %lu bytes is over the 64 MB this path gives\n", size); return NULL; }
    if (!dma_allocate(size, true, &memory, &address)) {
        hakiospcie_tl("wifi-pcie: radio memory: %lu bytes could not be had (dma_allocate said no)\n", size);
        return NULL;
    }
    hakiospcie_tl("wifi-pcie: radio memory: %lu bytes at %#llx\n", size, address);
    *iova = address; return (void *)memory;
}
/* the fallback path asks this before handing out the regulatory database */
int
hakiospcie_wifi_regdb_skipped(const char *bare_name)
{
	return wifi_skip_regdb && bare_name != NULL && !strcmp(bare_name, "regdb.bin");
}

const uint8_t *hakiospcie_wifi_driver_file(const char *name, uint64_t *size) {
    hakiospcie_tl("wifi-pcie: file request \"%s\"\n", name);
    /* The driver asks both ways: "ath11k/QCA6490/hw1.0/board.bin" and "QCA6490/hw1.0/firmware-2.bin". */
    static const char prefix[] = "ath11k/";
    if (!hakiospcie_wifi_driver_mode()) { hakiospcie_tl("wifi-pcie:   not in driver mode\n"); return NULL; }
    if (!strncmp(name, prefix, sizeof(prefix) - 1)) { name += sizeof(prefix) - 1; }
    if (strncmp(name, "QCA6490/hw1.0/", 14)) { hakiospcie_tl("wifi-pcie:   not under QCA6490/hw1.0/: %s\n", name); return NULL; }
    const char *file = name + 14;
    /* wifi_skip_regdb: let the board data go first, to see whether the radio
     * only takes one board file before it stops answering. */
    if (wifi_skip_regdb && !strcmp(file, "regdb.bin")) {
        hakiospcie_tl("wifi-pcie:   regdb is skipped on purpose, so the board data goes first\n");
        return NULL;
    }
    /* firmware-2.bin is ath11k's own container (magic "QCOM-ATH11K-FW"): the AMSS and M3 images in one file. */
    if (strcmp(file, "board.bin") && strcmp(file, "board-2.bin") && strcmp(file, "regdb.bin") &&
        strcmp(file, "m3.bin") && strcmp(file, "bdwlan.bin") && strcmp(file, "amss.bin") &&
        strcmp(file, "firmware-2.bin")) {
        hakiospcie_tl("wifi-pcie:   %s is not one of the files we hand out\n", file);
        return NULL;
    }
    /* ath11k asks for the board data under its own name; the card's BDF is the same bytes. */
    /*
     * The vendor ships the board data as an ELF container, and this radio's
     * firmware takes it that way: ath11k picks the BDF type from the first four
     * bytes (qmi.c:2436), so an ELF here means type 1, ten chunks, and no cal
     * data afterwards. A bare .data section is what the radio ignored.
     */
    const char *stored = !strcmp(file, "board-2.bin") ? "board-2.bin" :
        !strcmp(file, "board.bin") ? (wifi_board_elf ? "bdwlan.elf" : "bdwlan.bin") : file;
    const uint8_t *data = hakios_image_file("fw", stored, size, NULL);
    if (!data || !*size || *size > 8u * 1024 * 1024) {
        hakiospcie_tl("wifi-pcie:   fw/%s: %s (%llu bytes)\n", file, data ? "too big or empty" : "not in the image",
            (unsigned long long)(size ? *size : 0));
        return NULL;
    }
    /* wifi_board_cap: hand ath11k only part of the board file, so its last
     * piece carries end=1 and the radio sees a complete transfer. */
    if (wifi_board_cap && (!strcmp(file, "board.bin") || !strcmp(file, "board-2.bin")) &&
        *size > wifi_board_cap) {
        hakiospcie_tl("wifi-pcie: ath11k gets only the first %u of %u bytes of %s\n",
            wifi_board_cap, (unsigned)*size, file);
        *size = wifi_board_cap;
    }
    hakiospcie_tl("wifi-pcie: ath11k asks for %s and gets %s, %u bytes\n", name, file, (unsigned)*size);
    return data;
}
void *hakiospcie_wifi_driver_bar(void) { return hakiospcie_wifi_driver_mode() ? (void *)bar : NULL; }
static bool driver_window(uint32_t offset, unsigned *windowed) {
    if (!hakiospcie_wifi_driver_mode() || driver_runtime.failed || !bar || (offset & 3) || offset >= 0x2000000) { return false; }
    if (offset < 0x80000) { *windowed = offset; return true; }
    uint32_t value = 0x40000000u | ((offset >> 19) & 0x3f);
    if (rd(bar, 0x310c) != value) {
        wr(bar + 0x310c, value);
        if (rd(bar, 0x310c) != value) { driver_runtime.failed = true; return false; }
    }
    *windowed = 0x80000 + (offset & 0x7ffff);
    return true;
}
static unsigned reg_read_calls, reg_write_calls;

uint32_t hakiospcie_wifi_register_read(uint32_t offset) {
    unsigned at; uint32_t value = 0;
    if (reg_read_calls < 32) {
        hakiospcie_tl("wifi-pcie: read %#x (call %u, thread %p)\n", offset, reg_read_calls, current_thread());
        reg_read_calls++;
    }
    lck_mtx_lock(&hakiospcie_window_lock);
    if (driver_window(offset, &at)) { value = rd(bar, at); }
    lck_mtx_unlock(&hakiospcie_window_lock);
    return value;
}
void hakiospcie_wifi_register_write(uint32_t offset, uint32_t value) {
    unsigned at;
    if (reg_write_calls < 32) {
        hakiospcie_tl("wifi-pcie: write %#x = %#x (call %u, thread %p)\n", offset, value, reg_write_calls,
            current_thread());
        reg_write_calls++;
    }
    lck_mtx_lock(&hakiospcie_window_lock);
    if (driver_window(offset, &at)) { wr(bar + at, value); }
    lck_mtx_unlock(&hakiospcie_window_lock);
}
static bool driver_receive(const uint8_t *data, unsigned length) {
    if (!length || length > 8192 + 32 || driver_runtime.queued >= 32) { return false; }
    hakiospcie_tl("QMI seq: in %u, %u bytes, head %02x %02x %02x %02x (queued %u)\n",
        ++driver_in_seq, length, length > 0 ? data[0] : 0, length > 1 ? data[1] : 0,
        length > 2 ? data[2] : 0, length > 3 ? data[3] : 0, driver_runtime.queued);
    struct hakiospcie_packet *packet = kalloc_data(sizeof(*packet) + length, Z_WAITOK | Z_ZERO);
    if (!packet) { return false; }
    packet->length = length; memcpy(packet->data, data, length);
    if (driver_runtime.last) { driver_runtime.last->next = packet; }
    else { driver_runtime.first = packet; }
    driver_runtime.last = packet; driver_runtime.queued++;
    return true;
}
static int driver_send(const void *packet, unsigned length) {
    uint8_t empty[7];
    const uint8_t *bytes = packet;
    if (wifi_mem_empty && length >= 7 && bytes[0] == 0 && bytes[3] == 0x36 && bytes[4] == 0) {
        memcpy(empty, bytes, 5);
        empty[5] = 0; empty[6] = 0;
        hakiospcie_tl("QMI probe: answering the memory request with an empty reply, not %u bytes\n", length);
        packet = empty; length = sizeof(empty);
    }
    lck_mtx_lock(&hakiospcie_mhi_lock);
    int error = !driver_runtime.enabled || driver_runtime.failed ? ENXIO : ipcr_send(packet, length) ? 0 : ENOBUFS;
    hakiospcie_tl("QMI seq: out %u, %u bytes, head %02x %02x %02x %02x, error %d\n",
        ++driver_out_seq, length, length > 0 ? bytes[0] : 0, length > 1 ? bytes[1] : 0,
        length > 2 ? bytes[2] : 0, length > 3 ? bytes[3] : 0, error);
    lck_mtx_unlock(&hakiospcie_mhi_lock);
    return error;
}
static void driver_wait(void) {
    lx_os_lock(); lx_os_sleep(&driver_runtime.wanted, 1000000); hakiospcie_lx_os_unlock();
}
static void driver_worker(void *unused) {
    (void)unused;
    hakios_qrtr_deliver(ipcr.announcement, ipcr.announcement_length);
    while (driver_runtime.wanted) {
        lck_mtx_lock(&hakiospcie_mhi_lock);
        bool ok = mhi_events();
        lck_mtx_unlock(&hakiospcie_mhi_lock);
        if (!ok) { driver_runtime.failed = true; break; }
        /* QRTR callbacks can send QMI replies and take sleeping locks. Never
         * call them under the transport lock or while an RX slot is owned. */
        while (driver_runtime.first) {
            struct hakiospcie_packet *packet = driver_runtime.first;
            driver_runtime.first = packet->next;
            if (!driver_runtime.first) { driver_runtime.last = NULL; }
            driver_runtime.queued--;
            hakios_qrtr_deliver(packet->data, packet->length);
            kfree_data(packet, sizeof(*packet) + packet->length);
        }
        hakios_qrtr_poll();
        driver_wait();
    }
    driver_runtime.running = false;
}
static void driver_quiesce(void) {
    if (driver_runtime.called) { hakios_lx_stop(); driver_runtime.called = false; }
    driver_runtime.wanted = false;
    hakiospcie_lx_os_wakeup(&driver_runtime.wanted);
    for (unsigned n = 0; driver_runtime.running && n < 2000; n++) { driver_wait(); }
    lck_mtx_lock(&hakiospcie_mhi_lock);
    driver_runtime.enabled = false;
    lck_mtx_unlock(&hakiospcie_mhi_lock);
    if (driver_runtime.running) {
        /* An unjoined worker must not lose its mappings. The caller still
         * resets the endpoint; RAM is reclaimed by the next kernel boot. */
        dma_space.retain_pages = true;
        hakiospcie_tl("wifi-pcie: driver worker stop timed out; DMA pages retained\n");
        return;
    }
    if (driver_runtime.attached) { hakios_qrtr_detach(); driver_runtime.attached = false; }
    while (driver_runtime.first) {
        struct hakiospcie_packet *packet = driver_runtime.first;
        driver_runtime.first = packet->next;
        kfree_data(packet, sizeof(*packet) + packet->length);
    }
    driver_runtime.last = NULL; driver_runtime.queued = 0;
}
/* Whether the ath11k object was linked in: its lx_start() is the strong one; this weak one is only the fallback. */
bool
hakios_lx_built_in(void)
{
	extern int lx_start(void);
	return lx_start != NULL;
}

static volatile int driver_start_result = -1;

static void
driver_start_thread(void *unused)
{
	(void)unused;
	hakiospcie_tl("wifi-pcie: the ath11k thread is in lx_start()\n");
	driver_start_result = hakios_lx_start();
	hakiospcie_tl("wifi-pcie: lx_start() came back with %d\n", driver_start_result);
	wakeup((event_t)&driver_start_result);
}

static bool driver_probe(void) {
    /* A line that names this build, so a screenshot cannot be of an older kernel. */
    hakiospcie_tl("wifi-pcie: stage 22 of the kernel built %s %s (ath11k %s)\n", __DATE__, __TIME__,
        hakios_lx_built_in() ? "linked in" : "NOT linked in");
    if (!ipcr.announcement_length || !hakios_qrtr_attach(driver_send)) { return false; }
    /* The radio asks for host memory in the middle of the handshake; handing it
       addresses we own means mapping them here, before any QMI traffic, not
       while a reply is on its way out. */
    /* wifi_mem_real is 1 (map and hand over), 2 (map but hand over the unmapped
       fallback, which separates a bad mapping from the radio writing into it) or
       0 (do not map at all). */
    if (wifi_mem_real && !mem_host_ready) {
        /* Stock CNSS gives the firmware a DDR heap at 0xa1000000 and calibration
         * memory at 0xa1600000, and its heap must not be one of our rings. */
        static const uint64_t at[2] = { 0xa1000000ull, 0xa1600000ull };
        static const unsigned size[2] = { 6160384u, 3801088u };
        vm_offset_t memory = 0;
        uint64_t iova = 0;
        bool all = true;
        for (unsigned i = 0; i < 2; i++) {
            if (!dma_allocate_at(size[i], true, at[i], &memory, &iova)) { all = false; break; }
            mem_host_iova[i] = iova;
            mem_host_memory[i] = memory;
        }
        mem_host_ready = all;
        hakiospcie_tl("QMI probe: host memory for the radio: %s %#llx and %#llx\n",
            all ? "mapped" : "not all mapped", mem_host_iova[0], mem_host_iova[1]);
    }
    driver_runtime.attached = driver_runtime.enabled = driver_runtime.wanted = driver_runtime.running = true;
    if (hakiospcie_lx_os_thread("lemonade-mhi", driver_worker, NULL)) {
        driver_runtime.running = false; return false;
    }
    if (wifi_mode == 24) {
        uint8_t address[6];
        if (!hakiospcie_lx_os_radio_address(address)) { return false; }
        hakiospcie_lx_os_net_attach(address);
    }
    driver_runtime.called = true;
    /* ath11k waits for the firmware inside lx_start(); on this machine that wait can be endless, and the boot thread
       must not go with it. Ninety seconds is more than the driver's own sixty-second deadline. */
    driver_start_result = -1;
    hakiospcie_tl("wifi-pcie: starting ath11k on a thread of its own\n");
    if (hakiospcie_lx_os_thread("ath11k-start", driver_start_thread, NULL)) {
        hakiospcie_tl("wifi-pcie: the thread could not be made; the stage stops here\n");
        return false;
    }
    /* lx_os_sleep() here came back at once (its clock is not running this early), so the wait is a plain delay. */
    for (unsigned i = 0; i < 900 && driver_start_result < 0; i++) {
        delay(100000);
    }
    int result = driver_start_result;
    if (result < 0) {
        hakiospcie_tl("wifi-pcie: ath11k has not come back from its start after 90 s; the stage stops here\n");
        return false;
    }
    bool ok = result == 0 && !driver_runtime.failed && mhi_dma_ok();
    hakiospcie_tl("wifi-pcie: ath11k driver result %d transport_failed %u FSR %#x global %#x\n",
        result, driver_runtime.failed, rd(dma_space.cb, 0x58), rd(dma_space.mmio, 0x48));
    if (!ok) {
        /* A capability request is the cheapest thing the firmware answers:
           an empty body, no transaction state on our side. If this comes back
           the radio is alive and it is the memory reply it objected to. */
        hakiospcie_tl("QMI probe: the memory handshake failed; asking the firmware whether it still speaks\n");
        static const uint8_t ping[] = {0x00, 0x77, 0x00, 0x24, 0x00, 0x00, 0x00};
        hakiospcie_tl("QMI probe: sending a capability request, the cheapest thing it always answers\n");
        int sent = ipcr_send(ping, sizeof(ping)) ? 0 : ENOBUFS;
        hakiospcie_tl("QMI probe: it left with %d; waiting three seconds for any answer\n", sent);
        for (unsigned waited = 0; waited < 30; waited++) { delay(100000); }
        hakiospcie_tl("QMI probe: after the wait, MHI BHI %#x BHIE %#x EE %#x\n",
            rd(bar, 0x28), rd(bar, 0x224), rd(bar, 0x128));
        /* Did the radio write anything into the buffer it was told to use, and
           did the SMMU see a translation fault on the way?  The two addresses
           are the ones ath11k's own reply named (0xa6000000 + 0x11b00 and
           0xa6000000 + 0x5f1b00), which is where the firmware keeps its DDR
           heap and its calibration data when it accepts the host memory. */
        {
            static const uint64_t asked[2] = { 0xa6011b00ull, 0xa65f1b00ull };
            for (unsigned n = 0; n < dma_space.allocation_count; n++) {
                vm_offset_t cached = dma_space.allocations[n].cached;
                unsigned size = dma_space.allocations[n].size;
                uint64_t iova = dma_space.allocations[n].iova;
                if (!cached || !size || size < 8 * 4) { continue; }
                for (unsigned i = 0; i < 2; i++) {
                    if (asked[i] < iova || asked[i] >= iova + size) { continue; }
                    volatile const uint32_t *w =
                        (volatile const uint32_t *)(cached + (vm_offset_t)(asked[i] - iova));
                    hakiospcie_tl("QMI probe: the buffer at %#llx holds %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        asked[i], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                }
                if (size >= 44u << 20) {
                    volatile const uint32_t *base = (volatile const uint32_t *)cached;
                    hakiospcie_tl("QMI probe: the whole buffer %#llx starts %08x %08x %08x %08x\n",
                        iova, base[0], base[1], base[2], base[3]);
                }
            }
            if (wifi_m3_memory && wifi_m3_size) {
                volatile const uint32_t *dump = (volatile const uint32_t *)wifi_m3_memory;
                unsigned non_zero = 0, words = wifi_m3_size / 4u;
                hakiospcie_tl("QMI probe: the dump area %#llx starts %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    wifi_m3_address, dump[0], dump[1], dump[2], dump[3], dump[4], dump[5], dump[6], dump[7]);
                for (unsigned w = 0; w < words; w++) { if (dump[w]) { non_zero++; } }
                hakiospcie_tl("QMI probe: the dump area holds %u non-zero words of %u\n", non_zero, words);
            }
            hakiospcie_tl("QMI probe: SMMU fault status %#x far %#llx, MHI status %#x EE %#x\n",
                rd(dma_space.cb, 0x58), rd64(dma_space.cb + 0x60), rd(bar, 0x12c), rd(bar, 0x128));
        }
    }
    if (ok) { hakiospcie_tl("wifi-pcie: stage 22 QCA6490 major-1 ath11k startup completed\n"); }
    return ok;
}
static bool mhi_probe(unsigned step) {
    if (!mhi_ready()) { return false; }
    if (step == 18) { return true; }
    if (!mhi_full_firmware()) { return false; }
    if (step == 19) { return true; }
    if (!ipcr_probe(step)) { return false; }
    return step < 22 || driver_probe();
}

static bool power_root(unsigned step) {
    /* Do not reset a root complex inherited in use. */
    if (!(rd(gcc, 0x6b024) & (1u << 31))) {
        hakiospcie_tl("wifi-pcie: root already clocked; refusing reset/power experiment\n");
        return false;
    }
    if (!hakiospcie_radio_supply_on("ldob6", 1200)) {
        hakiospcie_tl("wifi-pcie: stage 2 rail ldob6 1200 mV was refused\n");
        return false;
    }
    if (!hakiospcie_radio_supply_on("ldob5", 880)) {
        hakiospcie_tl("wifi-pcie: stage 2 rail ldob5 880 mV was refused\n");
        return false;
    }
    hakiospcie_tl("wifi-pcie: stage 2 rails complete\n");
    if (step == 2) { return true; }
    change_gcc(0x52128, 1, 0); /* PCIe0's votable power domain */
    if (!wait_mask(gcc, 0x6b004, 1u << 31, 1u << 31)) { return false; }
    change_gcc(0x6b004, 1u << 11, 1u << 11); /* retain flip-flops */
    hakiospcie_tl("wifi-pcie: stage 3 power-domain complete\n");
    if (step == 3) { return true; }
    change_gcc(0x6b054, 3, 0); /* gcc_parent_map_4: external PHY PIPE */
    /* AUX = XO / 1, no M/N mode; preserve other RCG fields. */
    change_gcc(0x6b05c, 0x371f, 1);
    wr(gcc + 0x6b058, rd(gcc, 0x6b058) | 1);
    if (!wait_mask(gcc, 0x6b058, 1, 0)) { return false; }
    change_gcc(0x52000, (1u << 11) | (1u << 12) | (1u << 18) | (1u << 19) | (1u << 22),
        (1u << 11) | (1u << 12) | (1u << 18) | (1u << 19) | (1u << 22));
    change_gcc(0x8c004, 1, 1); /* root refclk, present in device clock list */
    change_gcc(0x52008, 0x2f, 0x2f); /* cfg, master, slave, aux, q2a; no PIPE */
    if (!wait_mask(gcc, 0x6b024, 1u << 31, 0) || !wait_mask(gcc, 0x6b014, 1u << 31, 0) ||
        !wait_mask(gcc, 0x6b028, 1u << 31, 0) || !wait_mask(gcc, 0x6b038, 1u << 31, 0)) { return false; }
    hakiospcie_tl("wifi-pcie: stage 4 clocks complete M/S/CFG %#x/%#x/%#x\n",
        rd(gcc, 0x6b01c), rd(gcc, 0x6b014), rd(gcc, 0x6b024));
    if (step == 4) { return true; }
    perst_cfg = rd(pins, 94 * 0x1000); perst_io = rd(pins, 94 * 0x1000 + 4);
    perst_saved = true;
    wr(pins + 94 * 0x1000 + 4, perst_io & ~2u);
    wr(pins + 94 * 0x1000, (perst_cfg & ~0x3fcu) | (1u << 9));
    pause_us(1000); /* hold the endpoint in reset throughout these probes */
    change_gcc(0x6b000, 1, 1); pause_us(1000); change_gcc(0x6b000, 1, 0);
    change_gcc(0x6c01c, 1, 1); pause_us(1000); change_gcc(0x6c01c, 1, 0);
    hakiospcie_tl("wifi-pcie: stage 5 resets complete\n");
    if (step == 5) { return true; }
    parf = ml_io_map_unmappable(0x1c00000, 0x3000, VM_WIMG_IO);
    dbi = ml_io_map_unmappable(0x60000000, 0x2000, VM_WIMG_IO);
    if (!parf || !dbi) { return false; }
    hakiospcie_tl("wifi-pcie: stage 6 reading PARF first %#x\n", rd(parf, 0));
    /* pci-msm.c initializes PARF routing BEFORE any DesignWare DBI access. */
    change_parf(0x1000, 4); /* root complex mode */
    change_parf(0x40, rd(parf, 0x40) & ~1u); /* PCIe clocks/reset path */
    change_parf(0x350, 0); /* DBI base */
    change_parf(0, 0x365e);
    change_parf(0x174, rd(parf, 0x174) | (1u << 4));
    change_parf(0x358, 0x4000000);
    change_parf(0x1a8, rd(parf, 0x1a8) | (1u << 31));
    hakiospcie_tl("wifi-pcie: stage 6 controller routing complete; DBI not yet read\n");
    if (step == 6) { return true; }
    phy = ml_io_map_unmappable(0x1c06000, 0x2000, VM_WIMG_IO);
    if (!phy) { return false; }
    hakiospcie_tl("wifi-pcie: stage 7 programming board PHY sequence\n");
    phy_started = true;
    for (unsigned i = 0; i < sizeof(hakiospcie_pcie_phy) / sizeof(hakiospcie_pcie_phy[0]); i++) {
        wr(phy + hakiospcie_pcie_phy[i].offset, hakiospcie_pcie_phy[i].value);
    }
    pause_us(1000);
    change_gcc(0x52008, 1u << 4, 1u << 4);
    if (!wait_mask(phy, 0x214, 1u << 6, 0)) { return false; }
    hakiospcie_tl("wifi-pcie: stage 7 PHY ready status %#x PIPE %#x; PERST held low\n", rd(phy, 0x214), rd(gcc, 0x6b030));
    if (step == 7) { return true; }
    uint32_t id = rd(dbi, 0);
    hakiospcie_tl("wifi-pcie: powered root id %#x command %#x link %#x PARF %#x\n",
        id, rd(dbi, 4), rd(dbi, 0x80), rd(parf, 0));
    if (id != 0x010c17cb) { return false; }
    if (step == 8) { return true; }
    /* CNSS QCA6490: keep BT unchanged; refuse to disturb an enabled radio. */
    if ((rd(pins, 64 * 0x1000 + 4) | rd(pins, 65 * 0x1000 + 4)) & 1) {
        hakiospcie_tl("wifi-pcie: radio already enabled; refusing shared power experiment\n");
        return false;
    }
    /* Board supply phandles -> RPMh resource names, from its regulator nodes. */
    if (!hakiospcie_radio_supply_on("smpb10", 1800) || !hakiospcie_radio_supply_on("ldoe7", 2800) ||
        !hakiospcie_radio_supply_on("smpe2", 950) || !hakiospcie_radio_supply_on("smpb11", 950) ||
        !hakiospcie_radio_supply_on("smpc1", 1880) || !hakiospcie_radio_supply_on("smpb12", 1350)) { return false; }
    radio_cfg = rd(pins, 64 * 0x1000); radio_io = rd(pins, 64 * 0x1000 + 4);
    clkreq_cfg = rd(pins, 95 * 0x1000); clkreq_io = rd(pins, 95 * 0x1000 + 4);
    radio_started = true;
    wr(pins + 95 * 0x1000, (clkreq_cfg & ~0x3ffu) | 7); /* alt1, pull-up, 2 mA input */
    pause_us(100000); /* required after BT_EN is observed low */
    wr(pins + 64 * 0x1000 + 4, radio_io | 2);
    wr(pins + 64 * 0x1000, (radio_cfg & ~0x3ffu) | 0x303); /* GPIO, pull-up, 10 mA output */
    pause_us(1000);
    if (!(rd(pins, 64 * 0x1000 + 4) & 1)) { return false; }
    hakiospcie_tl("wifi-pcie: stage 9 radio rails and WLAN_EN ready; PERST held low\n");
    if (step == 9) { return true; }
    /* Gen1 bring-up first. Status bits use W1C: modify only the low control half. */
    change_dbi(0xa0, (rd(dbi, 0xa0) & ~0xfu) | 1, true);
    change_dbi(0x80, rd(dbi, 0x80) & ~3u, true); /* no ASPM until power management exists */
    pause_us(10000);
    wr(pins + 94 * 0x1000 + 4, rd(pins, 94 * 0x1000 + 4) | 2);
    pause_us(1000);
    link_started = true;
    change_parf(0x1b0, rd(parf, 0x1b0) | (1u << 8));
    bool linked = false;
    for (unsigned i = 0; i < 100; i++) {
        if ((rd(dbi, 0xf28) & (1u << 10)) && (rd(dbi, 0x80) & (1u << 29))) { linked = true; break; }
        pause_us(5000);
    }
    hakiospcie_tl("wifi-pcie: link %s ELBI %#x status %#x LTSSM %#x\n",
        linked ? "up" : "timeout", rd(dbi, 0xf28), rd(dbi, 0x80), rd(parf, 0x1b0));
    if (!linked) { return false; }
    hakiospcie_tl("wifi-pcie: stage 10 PCIe link ready; DMA disabled\n");
    if (step == 10) { return true; }
    config = ml_io_map_unmappable(0x60100000, 0x1000, VM_WIMG_IO);
    if (!config || (rd(dbi, 0x1004) & (1u << 31))) { return false; }
    change_dbi(0x18, (rd(dbi, 0x18) & 0xff000000u) | 0x010100, false);
    for (unsigned i = 0; i < 7; i++) { iatu_values[0][i] = rd(dbi, 0x1000 + 4 * i); }
    iatu_count = 1;
    wr(dbi + 0x1004, 0);
    wr(dbi + 0x1000, 4); /* outbound CFG0, bus 1/device 0/function 0 */
    wr(dbi + 0x1008, 0x60100000); wr(dbi + 0x100c, 0);
    wr(dbi + 0x1010, 0x60100fff); wr(dbi + 0x1014, 0x01000000); wr(dbi + 0x1018, 0);
    wr(dbi + 0x1004, 1u << 31);
    if (!wait_mask(dbi, 0x1004, 1u << 31, 1u << 31)) { return false; }
    pause_us(1000);
    uint32_t endpoint = rd(config, 0), command = rd(config, 4), revision = rd(config, 8);
    hakiospcie_tl("wifi-pcie: endpoint id %#x command %#x class/revision %#x\n", endpoint, command, revision);
    if (endpoint != 0x110317cb || (command & 4)) { return false; }
    if (step == 11) { return true; }
    if (rd(dbi, 0x1204) & (1u << 31)) { return false; }
    bar = ml_io_map_unmappable(0x60400000, 0x200000, VM_WIMG_IO);
    if (!bar) { return false; }
    bar0_old = rd(config, 0x10); bar1_old = rd(config, 0x14); endpoint_command_old = command & 0xffff;
    if ((bar0_old & 7) != 4) { /* Known board BAR0 is 64-bit memory. */
        hakiospcie_tl("wifi-pcie: unexpected BAR type %#x; mapping refused\n", bar0_old & 15);
        return false;
    }
    bar_saved = true;
    wr(config + 0x10, 0x60400000 | (bar0_old & 15)); wr(config + 0x14, 0);
    change_dbi(0x20, 0x60506040, false); /* bridge window 0x60400000..0x605fffff */
    change_dbi(4, (rd(dbi, 4) & ~4u) | 2, true); /* memory decode, no bus master */
    for (unsigned i = 0; i < 7; i++) { iatu_values[1][i] = rd(dbi, 0x1200 + 4 * i); }
    iatu_count = 2;
    wr(dbi + 0x1204, 0); wr(dbi + 0x1200, 0); /* outbound MEM */
    wr(dbi + 0x1208, 0x60400000); wr(dbi + 0x120c, 0); wr(dbi + 0x1210, 0x605fffff);
    wr(dbi + 0x1214, 0x60400000); wr(dbi + 0x1218, 0); wr(dbi + 0x1204, 1u << 31);
    if (!wait_mask(dbi, 0x1204, 1u << 31, 1u << 31)) { return false; }
    wr16(config + 4, (endpoint_command_old & ~4u) | 2);
    pause_us(1000);
    if (rd(config, 4) & 4) { return false; }
    uint32_t hw = rd(bar, 0x224), length = rd(bar, 0), version = rd(bar, 8);
    uint32_t bhi = rd(bar, 0x28), bhie = rd(bar, 0x2c), status = rd(bar, 0x48);
    hakiospcie_tl("wifi-pcie: BAR TCSR version %#x (major %u minor %#x) MHI len %#x version %#x status %#x BHI %#x BHIE %#x\n",
        hw, (hw >> 8) & 15, hw & 255, length, version, status, bhi, bhie);
    if (hw == 0 || hw == ~0u || !length || length == ~0u || !bhi || bhi > 0x1fff80 || (bhi & 3)) { return false; }
    /* Read only non-identifying boot state, never BHI serial/key-hash registers. */
    uint32_t ee = rd(bar, bhi + 0x28), boot_status = rd(bar, bhi + 0x2c);
    hakiospcie_tl("wifi-pcie: BHI execution environment %u status %#x; no firmware transfer\n", ee, boot_status);
    if (ee > 6) { return false; }
    hakiospcie_tl("wifi-pcie: stage 12 BAR/MHI registers readable; endpoint bus master disabled\n");
    if (step == 12) { return true; }
    vm_offset_t smmu = ml_io_map_unmappable(0x15000000, 0x2000, VM_WIMG_IO);
    if (!smmu) { return false; }
    uint32_t scr0 = rd(smmu, 0), idr0 = rd(smmu, 0x20), idr1 = rd(smmu, 0x24);
    unsigned matches = idr0 & 255, valid = 0, covering = 0;
    if (scr0 == ~0u || !matches || matches > 255) { return false; }
    hakiospcie_tl("wifi-pcie: Apps SMMU control %#x idr0 %#x idr1 %#x\n", scr0, idr0, idr1);
    for (unsigned i = 0; i < matches; i++) {
        uint32_t smr = rd(smmu, 0x800 + 4 * i), s2cr = rd(smmu, 0xc00 + 4 * i);
        bool wide = (scr0 & (1u << 3)) != 0;
        if (!(wide ? s2cr & (1u << 10) : smr & (1u << 31))) { continue; }
        valid++;
        uint32_t stream = smr & (wide ? 0xffff : 0x7fff), mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);
        if (((stream ^ 0x1c01) & ~mask) == 0) {
            covering++;
            hakiospcie_tl("wifi-pcie: SMMU match %u stream %#x mask %#x s2cr %#x\n", i, stream, mask, s2cr);
        }
    }
    hakiospcie_tl("wifi-pcie: stage 13 SMMU survey complete; %u/%u matches used, %u cover endpoint; SMMU writes=0\n", valid, matches, covering);
    if (step == 13) { return true; }
    if (covering || !route_endpoint()) { return false; }
    if (step == 14) { return true; }
    if (!dma_prepare(step >= 18)) { return false; }
    if (step == 15) { return true; }
    if (step == 16) { return dma_fault_probe(); }
    if (!firmware_probe()) { return false; }
    return step == 17 || mhi_probe(step);
}
void hakiospcie_wifi_init(void) {
    unsigned mode = 0;
    bool have = PE_parse_boot_argn("hakiospcie_wifi", &mode, sizeof(mode));
    printf("wifi-pcie: init entered; the argument says %s %u\n", have ? "yes" : "no", mode);
    if (!have || !mode) { return; }
    if (!platform_is_lemonade()) {
        hakiospcie_tl("wifi-pcie: refused on another platform\n");
        return;
    }
    if (mode < 1 || mode > 24) {
        hakiospcie_tl("wifi-pcie: mode %u unsupported; no hardware access\n", mode);
        return;
    }
    if (mode >= 17) {
        unsigned guard_verified = 0;
        PE_parse_boot_argn("hakiospcie_dma_verified", &guard_verified, sizeof(guard_verified));
        if (guard_verified != 1) {
            hakiospcie_tl("wifi-pcie: firmware probe requires verified endpoint guard evidence\n");
            return;
        }
    }
    if (mode >= 2) {
        unsigned touch = 0, gpu = 0, cpu = 0, runtime = 0;
        PE_parse_boot_argn("touchscreen", &touch, sizeof(touch));
        PE_parse_boot_argn("gpu", &gpu, sizeof(gpu));
        PE_parse_boot_argn("cpufreq", &cpu, sizeof(cpu));
        PE_parse_boot_argn("hakiospcie_runtime", &runtime, sizeof(runtime));
        /* Component probes retain their isolation requirement. Only the full
         * station lifecycle in the explicit integrated runtime may coexist
         * with graphics and input; it still requires the DMA guard above. */
        if ((touch || gpu || cpu) && !(mode == 24 && runtime == 1)) {
            hakiospcie_tl("wifi-pcie: power probe refused alongside touch/GPU/CPU-policy work\n");
            return;
        }
        if (mode == 24 && runtime == 1)
            hakiospcie_tl("wifi-pcie: integrated station runtime enabled\n");
    }
    PE_parse_boot_argn("wifi_mem_empty", &wifi_mem_empty, sizeof(wifi_mem_empty));
    PE_parse_boot_argn("wifi_mem_align", &wifi_mem_align, sizeof(wifi_mem_align));
    PE_parse_boot_argn("wifi_mem_build", &wifi_mem_build, sizeof(wifi_mem_build));
    PE_parse_boot_argn("wifi_mem_shift", &wifi_mem_shift, sizeof(wifi_mem_shift));
    PE_parse_boot_argn("wifi_mem_type", &wifi_mem_type, sizeof(wifi_mem_type));
    PE_parse_boot_argn("wifi_board_type", &wifi_board_type, sizeof(wifi_board_type));
    PE_parse_boot_argn("board_file_id_keep", &board_file_id_keep, sizeof(board_file_id_keep));
    PE_parse_boot_argn("wifi_skip_regdb", &wifi_skip_regdb, sizeof(wifi_skip_regdb));
    PE_parse_boot_argn("wifi_ping", &wifi_ping, sizeof(wifi_ping));
    PE_parse_boot_argn("wifi_board_pace", &wifi_board_pace, sizeof(wifi_board_pace));
    PE_parse_boot_argn("wifi_board_elf", &wifi_board_elf, sizeof(wifi_board_elf));
    PE_parse_boot_argn("wifi_board_end", &wifi_board_end, sizeof(wifi_board_end));
    PE_parse_boot_argn("wifi_board_cap", &wifi_board_cap, sizeof(wifi_board_cap));
    PE_parse_boot_argn("board_total_constant", &board_total_constant, sizeof(board_total_constant));
    PE_parse_boot_argn("board_repeat_first", &board_repeat_first, sizeof(board_repeat_first));
    PE_parse_boot_argn("wifi_mem_real", &wifi_mem_real, sizeof(wifi_mem_real));
    PE_parse_boot_argn("wifi_m3", &wifi_m3, sizeof(wifi_m3));
    hakiospcie_tl("wifi-pcie: begin PCIe/CNSS survey mode %u\n", mode);
    wifi_mode = mode;
    bool ok = inspect();
    hakiospcie_tl("wifi-pcie: inspection %s; hardware writes=0; network unavailable\n", ok ? "complete" : "failed");
    if (ok && mode >= 2) {
        ok = power_root(mode);
        hakiospcie_tl("wifi-pcie: stage %u %s; networking not yet verified\n", mode, ok ? "passed" : "failed");
        if (!ok && mem_host_ready && mem_host_memory[0]) {
            /* The heap was handed over, so name the two windows rather than read
               them: a read through an uncached alias of live device memory is the
               one access that can take the machine with it, and it told us
               nothing the addresses do not. */
            hakiospcie_tl("QMI probe: the heap was at %#llx and the calibration window at %#llx\n",
                mem_host_iova[0], mem_host_iova[1]);
        }
        if (ok && mode == 24) {
            hakiospcie_tl("wifi-pcie: station offered to iOS; runtime kept until normal stop\n");
        } else {
            hakiospcie_wifi_stop();
        }
    }
}


/* ---- what this first step stands in for ---------------------------------------------------------------
 * The OnePlus 9 port carries its own QRTR router inside its Wi-Fi driver, and the Linux shim its ath11k
 * object links against. This tree's QRTR is the ops interface the modem offers (sys/hakios.qrtr.h) and the
 * ath11k object is not linked in yet, so these say what they are instead of failing silently.
 */

static lck_grp_t *pcie_group;
static lck_mtx_t *pcie_lock;

static void
pcie_lock_once(void)
{
	if (pcie_lock == NULL) {
		pcie_group = lck_grp_alloc_init("wifi-pcie", LCK_GRP_ATTR_NULL);
		pcie_lock = lck_mtx_alloc_init(pcie_group, LCK_ATTR_NULL);
	}
}

void
lx_os_lock(void)
{
	pcie_lock_once();
	if (pcie_lock != NULL) {
		lck_mtx_lock(pcie_lock);
	}
}

void
hakiospcie_lx_os_unlock(void)
{
	if (pcie_lock != NULL) {
		lck_mtx_unlock(pcie_lock);
	}
}

int
lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
	uint64_t interval, deadline;

	if (pcie_lock == NULL) {
		return 0;
	}
	nanoseconds_to_absolutetime(nanoseconds, &interval);
	clock_absolutetime_interval_to_deadline(interval, &deadline);
	return lck_mtx_sleep_deadline(pcie_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT, deadline);
}

void
hakiospcie_tl(const char *format, ...)
{
	va_list list;

	va_start(list, format);
	printf("wifi-pcie: ");
	vprintf(format, list);
	va_end(list);
}



/*
 * The QMI helpers (sys/hakios.qmi.h) talk to the firmware through these four calls: a port of our own node of the
 * IPC router, datagrams to and from it, and the question "where is that service". The router's packets travel over
 * the IPCR channels stage 20 brought up (hakiospcie_qrtr_header in front of the payload), which is what this is.
 */
struct qrtr_lookup {
	uint32_t service, version, instance;
	struct qrtr_lookup *next;
};
struct qrtr_port {
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
	uint32_t id;                    /* our port number on this node */
	struct qrtr_lookup *lookups;    /* what this port asked to be told about */
	struct qrtr_port *next;
};

static struct qrtr_port *qrtr_ports;
static uint32_t qrtr_next_port = 0x4100;
static int (*qrtr_send_raw)(const void *, unsigned);

static struct hakios_qrtr_port *
qrtr_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	struct qrtr_port *port = kalloc_type(struct qrtr_port, Z_WAITOK | Z_ZERO);

	if (port == NULL) {
		return NULL;
	}
	port->receive = receive;
	port->server = server;
	port->context = context;
	port->id = qrtr_next_port++;
	port->next = qrtr_ports;
	qrtr_ports = port;
	return (struct hakios_qrtr_port *)port;
}

/* HAKIOS: the board download carries file id 0xff on this machine because the
 * radio never told us a board id.  A real one is a small number, so rewrite it
 * to 0: 0xff may send the radio looking for a window that does not exist.
 */
static void
qmi_board_type_claim(uint8_t *data, unsigned length)
{
	unsigned i;

	if (wifi_board_type == 0 || length < 18) { return; }
	if (data[3] != 0x25 || data[4] != 0x00) { return; }
	for (i = 7; i + 4 <= length; i++) {
		if (data[i] == 0x15 && data[i + 1] == 0x01 && data[i + 2] == 0x00) {
			if (data[i + 3] != wifi_board_type) {
				hakiospcie_tl("QMI probe: the board data is claimed to be type %u, not %u\n",
				    wifi_board_type, data[i + 3]);
			}
			data[i + 3] = (uint8_t)wifi_board_type;
			return;
		}
	}
}

static void
qmi_board_file_id_zero(uint8_t *data, unsigned length)
{
	unsigned i;

	if (length < 18) { return; }
	if (data[3] != 0x25 || data[4] != 0x00) { return; }
	for (i = 7; i + 7 <= length; i++) {
		if (data[i] == 0x10 && data[i + 1] == 0x04 && data[i + 2] == 0x00) {
			if (data[i + 3] != 0 || data[i + 4] != 0) {
				hakiospcie_tl("QMI probe: the board data is asked for as file 0, not %02x\n", data[i + 3]);
			}
			data[i + 3] = 0; data[i + 4] = 0; data[i + 5] = 0; data[i + 6] = 0;
			return;
		}
	}
}

static int
qrtr_send(struct hakios_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	hakiospcie_tl("wifi-pcie: QMI send %u bytes to node %u port %#x\n", (unsigned)length, node, to_port);
	struct qrtr_port *mine = (struct qrtr_port *)port;

	if (mine == NULL || qrtr_send_raw == NULL || length == 0 || length > MHI_IPCR_MTU) {
		return -EINVAL;
	}
	if (length >= 18 && ((const uint8_t *)data)[3] == 0x25) {
		uint8_t *edited = (uint8_t *)data;
		if (!board_file_id_keep) { qmi_board_file_id_zero(edited, (unsigned)length); }
		qmi_board_type_claim(edited, (unsigned)length);
		/* The radio stops answering partway through the board file. Give it
		 * a moment between pieces: the copy is done by its own firmware, and
		 * a burst of six kilobyte messages may be more than it can take. */
		if (wifi_board_pace != 0 && edited[15] != 0x04) {
			unsigned seg = 0, total = 0;
			for (unsigned i = 7; i + 7 <= length; i++) {
				if (edited[i] == 0x11 && edited[i + 1] == 0x04 && edited[i + 2] == 0x00) {
					total = (unsigned)edited[i + 3] | ((unsigned)edited[i + 4] << 8) |
					    ((unsigned)edited[i + 5] << 16) | ((unsigned)edited[i + 6] << 24);
				}
				if (edited[i] == 0x12 && edited[i + 1] == 0x04 && edited[i + 2] == 0x00) {
					seg = (unsigned)edited[i + 3];
				}
			}
			if (board_file_total == 0) {
				board_file_total = total;
			} else if (board_total_constant && total != board_file_total) {
				for (unsigned i = 7; i + 7 <= length; i++) {
					if (edited[i] == 0x11 && edited[i + 1] == 0x04 &&
					    edited[i + 2] == 0x00) {
						edited[i + 3] = (uint8_t)(board_file_total & 0xff);
						edited[i + 4] = (uint8_t)((board_file_total >> 8) & 0xff);
						edited[i + 5] = (uint8_t)((board_file_total >> 16) & 0xff);
						edited[i + 6] = (uint8_t)((board_file_total >> 24) & 0xff);
						total = board_file_total;
						break;
					}
				}
			}
			hakiospcie_tl("QMI probe: board piece %u, total %u (%u left), %u ms apart\n",
			    seg, total, total, wifi_board_pace);
			if (board_repeat_first) {
				for (unsigned i = 7; i + 7 <= length; i++) {
					if (edited[i] == 0x12 && edited[i + 1] == 0x04 &&
					    edited[i + 2] == 0x00) {
						edited[i + 3] = 0; edited[i + 4] = 0;
						edited[i + 5] = 0; edited[i + 6] = 0;
						break;
					}
				}
				seg = 0;  /* the wire says segment zero; say so here too */
			}
			if (wifi_board_end && seg == 3) {
				for (unsigned i = length >= 8 ? length - 8 : 7; i + 7 <= length; i++) {
					if (edited[i] == 0x14 && edited[i + 1] == 0x01 &&
					    edited[i + 2] == 0x00) {
						edited[i + 3] = 1;
						hakiospcie_tl("QMI probe: calling piece %u the last one\n", seg);
						break;
					}
				}
			}
			{
				unsigned wire = 0;
				for (unsigned i = 7; i + 7 <= length; i++) {
					if (edited[i] == 0x12 && edited[i + 1] == 0x04 &&
					    edited[i + 2] == 0x00) { wire = (unsigned)edited[i + 3]; break; }
				}
				hakiospcie_tl("QMI probe: wire piece %u\n", wire);
			}
			delay(wifi_board_pace * 1000);
		}
	}
	/* wifi_mem_empty: hand the radio an empty respond-mem, the way Linux'
	 * delayed path does, to see whether it is the payload that silences it. */
	if (wifi_mem_empty && length >= 7 && ((const uint8_t *)data)[0] == 0 &&
	    ((const uint8_t *)data)[3] == 0x36 && ((const uint8_t *)data)[4] == 0) {
		uint8_t empty[7];
		memcpy(empty, data, 5);
		empty[5] = 0; empty[6] = 0;
		hakiospcie_tl("QMI probe: answering the memory request with an empty reply, not %u bytes\n", (unsigned)length);
		data = empty; length = sizeof(empty);
	} else if (wifi_mem_shift && length >= 27 && ((const uint8_t *)data)[3] == 0x36 && ((const uint8_t *)data)[4] == 0) {
		/* The size of each segment comes out eight bits too high: 0x01005e00
		 * where the radio asked for 0x0001005e. Find each segment by its
		 * address and divide the size that follows it back down. */
		/* From the dump: each segment is eight bytes of address, one pad
		 * byte, four bytes of size, then four bytes of type. */
		uint8_t *editable = (uint8_t *)data;
		for (unsigned at = 7; at + 17 <= length; at++) {
			uint64_t address = 0;
			memcpy(&address, editable + at, sizeof(address));
			if (address < WIFI_ALLOC_IOVA || address >= WIFI_DATA_LIMIT) { continue; }
			uint32_t size = 0;
			memcpy(&size, editable + at + 9, sizeof(size));
			uint32_t fixed = size >> 8;
			if ((size & 0xff) || fixed < 1024 || fixed > 8u * 1024 * 1024) { continue; }
				memcpy(editable + at + 9, &fixed, sizeof(fixed));
			hakiospcie_tl("QMI probe: segment at %#llx says %u bytes, corrected to %u\n",
			    address, size, fixed);
			if (wifi_mem_type) {
				uint32_t wanted = wifi_mem_type;
				memcpy(editable + at + 13, &wanted, sizeof(wanted));
				hakiospcie_tl("QMI probe: its type is now %u, not the zero that was there\n",
				    wifi_mem_type);
			}
			if (wifi_mem_align) {
				uint64_t mask = (uint64_t)wifi_mem_align - 1;
				uint64_t rounded = address & ~mask;
				if (rounded != address) {
					memcpy(editable + at, &rounded, sizeof(rounded));
					hakiospcie_tl("QMI probe: that segment moved from %#llx to %#llx\n",
					    address, rounded);
				}
			}
		}
		for (unsigned at = 0; at < length; at += 8) {
			char line[80];
			int used = 0;
			used += snprintf(line + used, sizeof(line) - used, "QMI probe: final +%u:", at);
			for (unsigned b = at; b < length && b < at + 8; b++) {
				used += snprintf(line + used, sizeof(line) - used, " %02x", ((const uint8_t *)data)[b]);
			}
			hakiospcie_tl("%s\n", line);
		}
	} else if (wifi_mem_real != 6 && ipcr.memory_count && length >= 7 && ((const uint8_t *)data)[3] == 0x36 &&
	    ((const uint8_t *)data)[4] == 0) {
		/*
		 * Answer from our own reading of the request, not from ath11k's decode of it:
		 * the radio wants each segment handed back with the type and size it asked for
		 * (Linux echoes type verbatim, qmi.c:1900), and ath11k's decoder reads those
		 * fields one byte off. addr, size, type, restore: seventeen bytes a segment.
		 */
		static uint8_t answered[11 + 17 * 16];
		static uint64_t seg_iova[16];
		static uint32_t seg_size[16];
		hakiospcie_tl("QMI probe: answering from our own table, %u segments\n", ipcr.memory_count);
		unsigned count = ipcr.memory_count > 16 ? 16 : ipcr.memory_count;
		unsigned at = 11;

		/* the QMI header stays what ath11k wrote: type, transaction, message */
		memcpy(answered, data, 7);
		answered[7] = 0x01;
		answered[10] = (uint8_t)count;
		for (unsigned i = 0; i < count; i++) {
			uint32_t size = ipcr.memory[i].size;
			uint32_t type = ipcr.memory[i].type;
			uint64_t addr = 0;

			/* real host memory, published in the card's own stage one table: the
			 * radio writes the board data and the calibration straight into it */
			if (wifi_mem_real != 2 && mem_host_ready && i < 2 && mem_host_iova[i]) {
				addr = mem_host_iova[i];
			} else if (wifi_mem_real != 1) {
				addr = 0xa3000000ull + (uint64_t)i * 0x2000000ull;
			} else if (seg_iova[i] && seg_size[i] >= size) {
				addr = seg_iova[i];
			} else if (size && size <= 64u * 1024 * 1024) {
				vm_offset_t memory = 0;
				uint64_t fresh = 0;
				if (dma_allocate(size, true, &memory, &fresh)) {
					seg_iova[i] = fresh;
					seg_size[i] = size;
					addr = fresh;
					hakiospcie_tl("QMI probe: %u bytes of host memory for type %u at %#llx\n",
					    size, type, addr);
				}
			}
			if (!addr) {
				addr = 0xa3000000ull + (uint64_t)i * 0x2000000ull;
				hakiospcie_tl("QMI probe: no host memory for type %u, falling back to %#llx\n",
				    type, addr);
			}

			memcpy(answered + at, &addr, 8);
			memcpy(answered + at + 8, &size, 4);
			memcpy(answered + at + 12, &type, 4);
			answered[at + 16] = 0x00;
			hakiospcie_tl("QMI probe: echoing type %u, %u bytes at %#llx\n", type, size, addr);
			at += 17;
		}
		{
			unsigned body = at - 7;
			unsigned tlv = body - 3;

			answered[5] = (uint8_t)(body & 0xff);
			answered[6] = (uint8_t)((body >> 8) & 0xff);
			answered[8] = (uint8_t)(tlv & 0xff);
			answered[9] = (uint8_t)((tlv >> 8) & 0xff);
		}
		data = answered;
		length = at;
		hakiospcie_tl("QMI probe: answering from the request itself: %u bytes, %u segments\n", at, count);
	} else if (length >= 7 && ((const uint8_t *)data)[3] == 0x4c) {
		hakiospcie_tl("QMI probe: device info out, %u bytes\n", length);
	} else if (length >= 12 && ((const uint8_t *)data)[3] == 0x25) {
		/* the board file download: one segment at a time, and the radio answers each */
		const uint8_t *b = (const uint8_t *)data;
		unsigned at;
		hakiospcie_tl("QMI probe: 0x25 out, %u bytes\n", length);
		for (at = 0; at + 8 <= 32 && at < length; at += 8)
			hakiospcie_tl("QMI probe: 0x25 %02u: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    at, b[at], b[at+1], b[at+2], b[at+3], b[at+4], b[at+5], b[at+6], b[at+7]);
		if (length >= 16) {
			at = length - 16;
			hakiospcie_tl("QMI probe: 0x25 tail %u: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    at, b[at], b[at+1], b[at+2], b[at+3], b[at+4], b[at+5], b[at+6], b[at+7]);
			hakiospcie_tl("QMI probe: 0x25 tail %u: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    at + 8, b[at+8], b[at+9], b[at+10], b[at+11], b[at+12], b[at+13], b[at+14], b[at+15]);
		}
	} else if (wifi_mem_build && length >= 7 && ((const uint8_t *)data)[3] == 0x36 && ((const uint8_t *)data)[4] == 0) {
		/* The two regions are inside the 44 MB window dma_prepare() mapped, far
		 * from the rings at its start, and page aligned. Sizes are the ones the
		 * radio asked for. */
		uint8_t built[47];
		uint64_t first = 0xa3000000ull, second = 0xa3200000ull;
		uint32_t first_size = 65530, second_size = 262202, zero = 0;
		unsigned found = 0;
		for (unsigned at = 7; at + 8 <= length && found < 2; at++) {
			uint64_t value = 0;
			memcpy(&value, (const uint8_t *)data + at, sizeof(value));
			if (value < WIFI_ALLOC_IOVA || value >= WIFI_DATA_LIMIT || (value & 4095)) { continue; }
			if (found == 0) { first = value; } else { second = value; }
			found++;
		}
		memcpy(built, data, 5);
		built[5] = 0x28; built[6] = 0x00;
		built[7] = 0x01; built[8] = 0x25; built[9] = 0x00;
		built[10] = 0x02;
		memcpy(built + 11, &first, 8);
		built[19] = 0x00;
		memcpy(built + 20, &first_size, 4);
		memcpy(built + 24, &zero, 4);
		built[28] = 0x00;
		memcpy(built + 29, &second, 8);
		built[37] = 0x00;
		memcpy(built + 38, &second_size, 4);
		memcpy(built + 42, &zero, 4);
		built[46] = 0x00;
		hakiospcie_tl("QMI probe: composing a 47 byte reply, eighteen bytes a segment: %#llx/%u and %#llx/%u\n",
		    first, first_size, second, second_size);
		data = built;
		length = sizeof(built);
	} else if (wifi_mem_align && length >= 12 && ((const uint8_t *)data)[3] == 0x36 && ((const uint8_t *)data)[4] == 0) {
		uint8_t *editable = (uint8_t *)data;
		uint64_t mask = (uint64_t)wifi_mem_align - 1;
		for (unsigned at = 7; at + 8 <= length; at++) {
			uint64_t value = 0;
			memcpy(&value, editable + at, sizeof(value));
			if (value < WIFI_ALLOC_IOVA || value >= WIFI_DATA_LIMIT || (value & mask) == 0) { continue; }
			uint64_t fixed = value & ~mask;
			memcpy(editable + at, &fixed, sizeof(fixed));
			hakiospcie_tl("QMI probe: segment address %#llx aligned down to %#llx\n",
			    value, fixed);
		}
	} else if (length >= 12 && ((const uint8_t *)data)[3] == 0x36 && ((const uint8_t *)data)[4] == 0) {
		const uint8_t *body = (const uint8_t *)data + 7;
		unsigned reported = body[0];
		hakiospcie_tl("QMI probe: the reply carries %u segments\n", reported);
		for (unsigned i = 0; i < reported && i < 4; i++) {
			uint64_t addr = 0; uint32_t size = 0, type = 0;
			memcpy(&addr, body + 1 + i * 17, sizeof(addr));
			memcpy(&size, body + 9 + i * 17, sizeof(size));
			memcpy(&type, body + 13 + i * 17, sizeof(type));
			hakiospcie_tl("QMI probe: segment %u: %#llx for %u bytes, type %u\n", i, addr, size, type);
		}
	}
	{
		/*
		 * The last piece of a file has to carry end = 1, or the radio keeps
		 * waiting for more pieces and never looks at what follows.  Our QMI
		 * encoder writes that byte as zero, so set it here: a piece whose
		 * total_size is no larger than one piece is the last one.
		 */
		uint8_t *b = (uint8_t *)(uintptr_t)data;

		if (length >= 24 && b[3] == 0x25) {
			/* total_size is the fourth TLV, at a fixed place in this message */
			unsigned at, total = 0xffffffffu, end_at = 0;

			for (at = 7; at + 7 <= 40 && at + 7 <= (unsigned)length; at++) {
				if (b[at] == 0x11 && b[at + 1] == 0x04 && b[at + 2] == 0x00) {
					total = b[at + 3] | (b[at + 4] << 8) | ((unsigned)b[at + 5] << 16) | ((unsigned)b[at + 6] << 24);
					break;
				}
			}
			/* end and bdf_type are the last eight bytes: end, then bdf type */
			if (b[length - 8] == 0x14 && b[length - 7] == 0x01 && b[length - 6] == 0x00) {
				end_at = (unsigned)length - 5;
			}
			if (end_at != 0 && total <= 6144 && b[end_at] != 1) {
				b[end_at] = 1;
				hakiospcie_tl("QMI probe: last piece of %u bytes: end set to 1\n", total);
			}
		}
	}
	return ipcr_qrtr_send(1, mine->id, node, to_port, data, (unsigned)length) ? 0 : -EIO;
}

/* what the radio has already told us about, so a lookup can be answered from memory */
struct qrtr_known {
	uint32_t service, version, instance, node, port;
	struct qrtr_known *next;
};

static struct qrtr_known *qrtr_known;

static void
qrtr_remember(bool found, uint32_t service, uint32_t version, uint32_t instance, uint32_t node, uint32_t port)
{
	struct qrtr_known *known = qrtr_known, **at = &qrtr_known;

	while (known != NULL) {
		if (known->service == service && known->version == version && known->instance == instance) {
			break;
		}
		known = known->next;
	}
	if (!found) {
		if (known != NULL) {
			*at = known->next;
			kfree_type(struct qrtr_known, known);
		}
		return;
	}
	if (known == NULL) {
		known = kalloc_type(struct qrtr_known, Z_WAITOK | Z_ZERO);
		if (known == NULL) {
			return;
		}
		known->service = service; known->version = version; known->instance = instance;
		known->next = qrtr_known; qrtr_known = known;
	}
	known->node = node; known->port = port;
}

static int
qrtr_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	struct qrtr_port *mine = (struct qrtr_port *)port;
	struct qrtr_lookup *ask;
	/* QRTR_TYPE_NEW_LOOKUP is 8, its body (service, version, instance); the router answers with NEW_SERVER. */
	struct hakiospcie_qrtr_control control = {0};

	if (mine == NULL) {
		return -EINVAL;
	}
	for (ask = mine->lookups; ask != NULL; ask = ask->next) {
		if (ask->service == service && ask->version == version && ask->instance == instance) {
			break;
		}
	}
	if (ask == NULL) {
		ask = kalloc_type(struct qrtr_lookup, Z_WAITOK | Z_ZERO);
		if (ask == NULL) {
			return -ENOMEM;
		}
		ask->service = service; ask->version = version; ask->instance = instance;
		ask->next = mine->lookups; mine->lookups = ask;
	}
	for (struct qrtr_known *known = qrtr_known; known != NULL; known = known->next) {
		if (known->service != service ||
		    (version != 0xffffffffu && known->version != version) ||
		    (instance != 0xffffffffu && known->instance != instance)) {
			continue;
		}
		hakiospcie_tl("wifi-pcie: service %#x version %u instance %u is already known: node %u port %#x\n",
		    service, version, instance, known->node, known->port);
		if (((struct qrtr_port *)port)->server != NULL) {
			((struct qrtr_port *)port)->server(((struct qrtr_port *)port)->context, true, service, version, instance, known->node, known->port);
		}
		return 0;
	}
	control.command = 8;
	control.words[0] = service;
	control.words[1] = version;
	control.words[2] = instance;
	if (!ipcr_qrtr_send(8, mine->id, 0xffffffffu, QRTR_CONTROL_PORT, &control, sizeof(control))) {
		return -EIO;
	}
	hakiospcie_tl("wifi-pcie: asked the router where service %#x version %u instance %u is\n",
	    service, version, instance);
	return 0;
}

static void
qrtr_close(struct hakios_qrtr_port *port)
{
	struct qrtr_port *mine = (struct qrtr_port *)port, **at = &qrtr_ports;

	if (mine == NULL) {
		return;
	}
	while (mine->lookups != NULL) {
		struct qrtr_lookup *ask = mine->lookups;

		mine->lookups = ask->next;
		kfree_type(struct qrtr_lookup, ask);
	}
	while (*at != NULL && *at != mine) {
		at = &(*at)->next;
	}
	if (*at == mine) {
		*at = mine->next;
	}
	kfree_type(struct qrtr_port, mine);
}

static const struct hakios_qrtr_ops qrtr_ops = {
	.open = qrtr_open,
	.send = qrtr_send,
	.lookup = qrtr_lookup,
	.close = qrtr_close,
};

bool
hakios_qrtr_attach(int (*send)(const void *, unsigned))
{
	qrtr_send_raw = send;
	hakios_qmi_set_transport(&qrtr_ops);
	hakiospcie_tl("wifi-pcie: the QRTR router is up (one node of our own, ports 0x%x and up)\n", qrtr_next_port);
	return true;
}

/* One whole QRTR packet, as it came off the IPCR channel. */
void
hakios_qrtr_deliver(const void *packet, unsigned length)
{
	struct hakiospcie_qrtr_header header = {0};
	const uint8_t *data = (const uint8_t *)packet;
	unsigned offset;

	if (length < sizeof(header) || data[0] != 1) {
		return;
	}
	memcpy(&header, data, sizeof(header));
	offset = sizeof(header);
	if (offset > length || header.size > length - offset) {
		return;
	}
	if (header.dest_port != QRTR_CONTROL_PORT) {
		hakiospcie_tl("wifi-pcie: QMI receive %u bytes for port %#x from node %u port %#x\n",
		    header.size, header.dest_port, header.source_node, header.source_port);
	}
	if (header.dest_port == QRTR_CONTROL_PORT && header.size >= 8) {
		/* NEW_SERVER (4) and DEL_SERVER (5): (service, instance << 8 | version, node, port). */
		struct hakiospcie_qrtr_control control = {0};

		memcpy(&control, data + offset, header.size < sizeof(control) ? header.size : sizeof(control));
		if (control.command == 4 || control.command == 5) {
			uint32_t service = control.words[0];
			uint32_t version = control.words[1] & 255;
			uint32_t instance = control.words[1] >> 8;

			qrtr_remember(control.command == 4, service, version, instance, control.words[2], control.words[3]);
			hakiospcie_tl("wifi-pcie: the router says service %#x version %u instance %u is %s (node %u port %#x)\n",
			    service, version, instance, control.command == 4 ? "there" : "gone",
			    control.words[2], control.words[3]);
			for (struct qrtr_port *port = qrtr_ports; port != NULL; port = port->next) {
				if (port->server == NULL) {
					continue;
				}
				for (struct qrtr_lookup *ask = port->lookups; ask != NULL; ask = ask->next) {
					if (ask->service != service ||
					    (ask->version != 0xffffffffu && ask->version != version) ||
					    (ask->instance != 0xffffffffu && ask->instance != instance)) {
						continue;
					}
					port->server(port->context, control.command == 4, service, version, instance,
					    control.words[2], control.words[3]);
				}
			}
		}
		return;
	}
	for (struct qrtr_port *port = qrtr_ports; port != NULL; port = port->next) {
		if (port->id == header.dest_port && port->receive != NULL) {
			port->receive(port->context, header.source_node, header.source_port, data + offset, header.size);
			return;
		}
	}
}

void
hakios_qrtr_poll(void)
{
}

void
hakios_qrtr_detach(void)
{
	while (qrtr_ports != NULL) {
		qrtr_close((struct hakios_qrtr_port *)qrtr_ports);
	}
	qrtr_send_raw = NULL;
	hakios_qmi_set_transport(NULL);
}

int
hakiospcie_lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
	thread_t thread;

	if (kernel_thread_start((thread_continue_t)function, argument, &thread) != KERN_SUCCESS) {
		return EAGAIN;
	}
	thread_set_thread_name(thread, name);
	thread_deallocate(thread);
	return 0;
}

void
hakiospcie_lx_os_wakeup(const void *address)
{
	wakeup((event_t)address);
}

bool
hakiospcie_lx_os_radio_address(unsigned char *address)
{
	static unsigned char local[6];
	static bool ready;

	if (!ready) {
		read_random(local, sizeof(local));
		local[0] = (local[0] & 0xfe) | 2;
		ready = true;
	}
	memcpy(address, local, sizeof(local));
	return true;
}

void
hakiospcie_lx_os_net_attach(const unsigned char *address)
{
	(void)address;
	hakiospcie_tl("the frames are not handed to en0 yet\n");
}

/*
 * The object built by tools/mk-ath11k.sh has these: lx_start() brings the radio up (it is the entry point the
 * Linux driver's probe ends in), lx_stop() puts it down. This is what the stages call.
 */
extern int lx_start(void);
extern void lx_stop(void);

int
hakios_lx_start(void)
{
	int result = lx_start();

	hakiospcie_tl("wifi-pcie: the ath11k object returned %d\n", result);
	return result;
}

void
hakios_lx_stop(void)
{
	lx_stop();
}
