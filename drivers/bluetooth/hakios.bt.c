/*
 * Bluetooth of the POCO X5 Pro (redwood): STEP ONE, a look. Only with boot-arg bt=1, and it only READS.
 *
 * The phone's radio chip is a Qualcomm WCN6750, one chip for Wi-Fi and Bluetooth (the phone's device tree: the node
 * bt_wcn6750, compatible qcom,wcn6750-bt; the Wi-Fi half is drivers/wifi/hakios.wifi.c). What the device tree says
 * about the Bluetooth half, and what this file takes from it (numbers, not text):
 *
 *   qcom,bt-reset-gpio     GPIO 65    the Bluetooth half's reset / enable line
 *   qcom,bt-sw-ctrl-gpio   GPIO 153   the line that tells the radio's switch which half has the antenna
 *   qcom,wl-reset-gpio     GPIO 64    the Wi-Fi half's reset line, listed here too: the chip comes up as a whole
 *   the supplies (qcom,bt-vdd-*-supply, each with a -config of minimum and maximum microvolts; read out of the
 *   device tree by script, not by eye), the same for both halves:
 *       io 1800000..2000000    aon, dig, rfacmn, rfa-0p8 824000..1120000    rfa1 1872000..2040000
 *       rfa2 1256000..1500000  ipa-2p2 2200000..2210000                     asd 2800000..3544000
 *     (which regulator each supply is, is a phandle in the tree; nothing here writes any of them)
 *
 * What this does. At boot-arg bt=1 it reads the three pins' configuration and level out of the TLMM (the same
 * registers the haptic driver reads for its pins) and prints them, once at the start and once bt_again seconds later
 * (default 30), to see whether the Wi-Fi bring-up (wifi=1) moved the reset lines in between. It writes NOTHING: no
 * pin, no supply, no clock, no serial engine. Reading a pin's configuration is a plain load from the TLMM, which the
 * haptic and thermal drivers do on this phone without trouble.
 *
 * What is NOT here, and what it needs. Bluetooth on this chip, as Android and Linux (hci_qca, btqca) do it:
 *   1. the supplies on (they are shared with Wi-Fi: wifi=1 already raises what the Wi-Fi half needs),
 *   2. the reset line high, and the chip given a moment,
 *   3. the UART of the Bluetooth half opened at 115200 baud (a Qualcomm serial engine, "hsuart0" in the device tree:
 *      its clocks and its pins have to be set up first, and an access to an engine whose clock is off hangs the bus),
 *   4. the chip's own firmware loaded over that UART with Qualcomm's vendor commands (a rampatch and an NVM file out of
 *      /vendor/firmware on the phone: NOT in the repository, and not pulled by devices/redwood/pull-firmware.sh yet; Linux
 *      names them qca/msbtfw*.mbn and qca/msnv*.bin for the WCN6750, as I remember it, which is NOT verified here),
 *   5. the baud rate raised, then H4 packets (HCI) in both directions.
 * None of 1 to 5 is written, because none of it could be checked: the phone was not on the desk when this was written, and
 * the register numbers and command bytes for a chip are not the kind of thing to write from memory and ship. Each step comes
 * with a boot-arg level the way haptic=N and wifi_step=N did: the next one is bt=2 (supplies and reset line, seen on the
 * pins), after this one has been run and its output compared with a phone that is running Android.
 *
 * Status: levels 1 and 2 have been run on the phone (mars, 14 November 2026). Level 2 votes the six supplies on
 * (smpb10 1800, ldoe7 2800, smpe2 950, smpb11 950, smpc1 1880, smpb12 1350 mV, all accepted) and drives the Bluetooth
 * half's reset line: gpio65 before, an input reading 1; after, an output driving high. gpio64 (the Wi-Fi half) and
 * gpio153 (the switch) are left as they were. Not given in QEMU (no such TLMM: a load from an address it does not map
 * faults).
 *
 * Level 3, 14 November 2026: the Bluetooth half's UART. The serial engine is the phone's own hsuart0
 * (/soc/qcom,qup_uart@890000: a QUPV3 serial engine, SE 4 of the second wrapper, 0x4000 of registers, GIC 586, the
 * second interrupt on gpio71 - the chip's wake line). The engine is read first and written only if it looks like UART;
 * then, for each candidate serial clock, the engine is set to 115200 and an HCI Reset is sent into it, and whatever
 * comes back is printed. Nothing about the chip's firmware is written yet.
 *
 * Boot-args:
 *   bt=1           the look
 *   bt=2           the look, then the supplies and the reset line (the Wi-Fi half is left off)
 *   bt=3           level 2, then the UART: its state, then an HCI Reset at each candidate clock
 *   bt_again=<s>   the second look this many seconds later (0: no second look; 30)
 */
#include <stdint.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_OUTPUT              (1u << 9)
#define BT_EN                   65      /* the Bluetooth half's reset / enable line */

static vm_offset_t tlmm;
static uint32_t again_seconds = 30;
static uint32_t uart_level;     /* the bt= level, so the power thread knows whether the UART is its business */
static unsigned bt_dry;
static unsigned bt_src = 6;         /* bt_dry=1: read the clock registers and stop; nothing is written */
static thread_call_t again_call;

/* The three pins of the phone's own bt_qca6490 node (mars: qcom,wl-reset-gpio = <&tlmm 0x40 0>,
   qcom,bt-reset-gpio = <&tlmm 0x41 0>, qcom,bt-sw-ctrl-gpio = <&tlmm 0x99 0>; the same pins the Wi-Fi bring-up
   reads for WLAN_EN, BT_EN and the switch, which is why its stage 9 refuses to run while either is enabled). */
static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ 64, "qcom,wl-reset-gpio: the Wi-Fi half's reset" },
	{ 65, "qcom,bt-reset-gpio: the Bluetooth half's reset" },
	{ 153, "qcom,bt-sw-ctrl-gpio: the antenna switch" },
};

/* The Bluetooth half's supplies (qcom,bt-vdd-*-supply in the tree: io, asd, rfa-0p8, aon+dig, rfa1, rfa2), by the
   name of the RPMh resource each phandle lands on, in the order the Wi-Fi bring-up votes them (its stage 9). The chip
   is one package, so these are the Wi-Fi half's supplies too; that vote is a sleepable one, so this runs on a thread. */
extern bool hakiospcie_radio_supply_on(const char *name, unsigned millivolts);

static const struct {
	const char	*name;
	unsigned	millivolts;
} supplies[] = {
	{ "smpb10", 1800 },	/* io */
	{ "ldoe7", 2800 },	/* asd */
	{ "smpe2", 950 },	/* rfa-0p8 */
	{ "smpb11", 950 },	/* aon, dig */
	{ "smpc1", 1880 },	/* rfa1 */
	{ "smpb12", 1350 },	/* rfa2 */
};

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* One look at the pins: what the TLMM says each is set to, and the level it reads. Nothing is written. */
static void
look(const char *when)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("bt: %s: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", when, pins[i].pin, pins[i].what, (cfg >> 2) & 15,
		    cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

static void
look_again(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	look("again");
}

/* Level 3 is defined below, after the serial engine it writes to; the power thread calls it once the chip is out of
   reset. */
static void uart_after_power(void);

/* Level 2: the supplies, then the Bluetooth half's reset line, then the pins again. The chip's enable lines are read
   first: a half that something else has already enabled is left alone. */
static void
power_thread(void *arg, __unused wait_result_t result)
{
	uint32_t wl_cfg = rd(GPIO_CFG(64)), wl_io = rd(GPIO_IO(64));
	uint32_t bt_cfg = rd(GPIO_CFG(65)), bt_io = rd(GPIO_IO(65));
	uint32_t cfg, io;

	say("bt: level 2: the supplies, then the Bluetooth half's reset line\n");
	if ((((wl_cfg | bt_cfg) & CFG_OUTPUT) != 0) && (((wl_io | bt_io) & 2) != 0)) {
		say("bt: a radio half is already enabled (gpio64 or gpio65 drives high); not touching its power\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		bool on = hakiospcie_radio_supply_on(supplies[i].name, supplies[i].millivolts);

		say("bt: supply %s at %u mV: %s\n", supplies[i].name, supplies[i].millivolts, on ? "on" : "refused");
		if (!on) {
			return;
		}
	}
	cfg = rd(GPIO_CFG(BT_EN));
	io = rd(GPIO_IO(BT_EN));
	say("bt: bt_en (gpio%u) before: function %u, pull %u, %s, reads %u\n", BT_EN, (cfg >> 2) & 15, cfg & 3,
	    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	wr(GPIO_IO(BT_EN), io & ~2u);
	wr(GPIO_CFG(BT_EN), (cfg & ~0x3ffu) | 0x303);	/* GPIO, pull-up, 10 mA, an output */
	delay(20000);
	wr(GPIO_IO(BT_EN), 2);
	delay(50000);
	cfg = rd(GPIO_CFG(BT_EN));
	io = rd(GPIO_IO(BT_EN));
	say("bt: bt_en (gpio%u) after: %s, reads %u\n", BT_EN, (io & 2) ? "drives high" : "drives low", io & 1);
	look("after the supplies");
	if (uart_level >= 3) {
		uart_after_power();
	}
}

/* ---- the Bluetooth half's UART ------------------------------------------------------------------------------
 *
 * A Qualcomm serial engine of the QUPV3 kind ("msm-geni-serial-hs" in the phone's device tree, SE 4 of the second
 * wrapper). The registers and the order they are written in are those of U-Boot's drivers/serial/serial_msm_geni.c,
 * which is built for this phone and prints its console through a sibling engine of the same kind.
 *
 * The clock: the engine shifts bits at the rate its serial clock is divided down from, and its serial clock comes from
 * GCC's QUPV3 wrapper 2, SE 4. The gate is one bit of GCC+0x52010 (bit 8); the root of the divider is GCC+0x11e4d0.
 * The divisor the engine itself uses is (serial clock / (baud * 32)). Which serial clock is running is not written
 * down anywhere we can read, so level 3 does not guess: it reads the divisor that is already in the engine, and if
 * there is none, tries the rates a QUP engine is usually given.
 *
 *
 * What has been proved on the phone (mars, 14 November 2026), and what has not:
 *
 *   - The engine's clocks: the wrapper's gates are 0x10f (core, s-ahb, m-ahb, core-2x, se4) and the divider at
 *     GCC+0x1e4d0 takes 0x100106 / 384 / 15625 / ~384, which is 7,372,800 Hz -- 115,200 baud with the engine's
 *     own divide by two. (The first reading of that register went to GCC+0x21e4d0, past the end of the map, and
 *     stopped the bus: the addresses in the tables already carry the block's base.)
 *   - The pads: gpio70 (tx) and gpio71 (rx) sit in function 1, which is qup18 on these pins -- the pins are counted
 *     from the gpio function in the device tree's PINGROUP(), so qup18 is the first alternative, not the second.
 *     Without that mux the engine shifts into nothing.
 *   - Scaling out: the main sequencer's command completes (the watermark and the end-of-command waits both answer),
 *     so the four reset bytes do leave the engine.
 *   - Scaling in: nothing has been heard from the chip. A read started with no data yields the same eight words
 *     every time -- 0, 0, 0, 0, 0xf0, 0, 0, 0xf0 -- with the chip on the other end, with the engine in its own
 *     loopback, and with the wire idle, so those eight words are the instrument's, not the chip's. Earlier readings
 *     of them as "the chip answered" were wrong.
 *
 * The next step is therefore not another baud rate: either the chip wants the vendor handshake (the ROM patch,
 * 0xfc00 and its friends) before it answers HCI at all, or it is not running -- its 32 kHz sleep clock is not one
 * of the rails this driver votes on, and the device tree does not say where it comes from.
 */
#define SE_PHYS                 0x890000UL
#define SE_SIZE                 0x4000UL
#define GCC_PHYS                0x100000UL

/* the serial engine */
#define SE_FORCE_DEFAULT        0x20
#define SE_OUTPUT_CTRL          0x24
#define SE_STATUS               0x40
#define SE_SER_M_CLK_CFG        0x48
#define SE_SER_S_CLK_CFG        0x4c
#define SE_IF_DISABLE_RO        0x64
#define SE_FW_REVISION_RO       0x68
#define SE_DFS_IF_CFG           0x80
#define SE_M_CMD0               0x600
#define SE_M_CMD_CTRL           0x604
#define SE_M_IRQ_STATUS         0x610
#define SE_M_IRQ_EN             0x614
#define SE_M_IRQ_CLEAR          0x618
#define SE_S_CMD0               0x630
#define SE_S_CMD_CTRL           0x634
#define SE_S_IRQ_STATUS         0x640
#define SE_S_IRQ_EN             0x644
#define SE_S_IRQ_CLEAR          0x648
#define SE_TX_PACKING_CFG0      0x260
#define SE_TX_PACKING_CFG1      0x264
#define SE_RX_PACKING_CFG0      0x284
#define SE_RX_PACKING_CFG1      0x288
#define SE_TX_TRANS_CFG         0x25c
#define SE_TX_PARITY_CFG        0x2a4
#define SE_RX_TRANS_CFG         0x280
#define SE_RX_PARITY_CFG        0x2a8
#define SE_TX_WORD_LEN          0x268
#define SE_RX_WORD_LEN          0x28c
#define SE_TX_STOP_BIT_LEN      0x26c
#define SE_TX_TRANS_LEN         0x270
#define SE_RX_FIFO(n)           (0x780 + 4 * (n))
#define SE_TX_FIFO(n)           (0x700 + 4 * (n))
#define SE_TX_FIFO_STATUS       0x800
#define SE_RX_FIFO_STATUS       0x804
#define SE_TX_WATERMARK         0x80c
#define SE_RX_WATERMARK         0x810
#define SE_RX_RFR_WATERMARK     0x814

#define SER_CLK_EN              (1u << 0)
#define CLK_DIV_SHFT            4
#define FORCE_DEFAULT           (1u << 0)
#define M_CMD_DONE_EN           (1u << 0)
#define M_GENI_CMD_ABORT        (1u << 1)
#define S_GENI_CMD_ABORT        (1u << 1)
#define S_CMD_DONE_EN           (1u << 0)
#define M_TX_FIFO_WATERMARK_EN  (1u << 30)
#define M_RX_FIFO_WATERMARK_EN  (1u << 26)
#define M_RX_FIFO_LAST_EN       (1u << 27)
#define S_RX_FIFO_WATERMARK_EN  (1u << 26)
#define S_RX_FIFO_LAST_EN       (1u << 27)
#define M_SEC_IRQ_EN            (1u << 31)
#define UART_START_TX_OPCODE    (1u << 27)
#define UART_START_READ_OPCODE  (1u << 27)
#define FIFO_WORDS_MSK          0x1ffffffu
#define SE_UART_LOOPBACK_CFG    0x274
#define UART_STOP_READ_OPCODE   (1u << 28)
#define UART_PACKING_CFG0       0xfu

/* GCC's QUPV3 wrapper 2: core, both AHB clocks, the 2x core and SE 4, all in one register */
#define GCC_QUPV3_WRAP2_GATES   0x52010
#define WRAP2_CORE_CLK          (1u << 0)
#define WRAP2_S_AHB_CLK         (1u << 1)
#define WRAP2_M_AHB_CLK         (1u << 2)
#define WRAP2_CORE_2X_CLK       (1u << 3)
#define WRAP2_S4_CLK            (1u << 8)
/* GCC is mapped at 0x100000, so these are the table's addresses less that base: U-Boot writes 0x12327c and
   0x11e4d0, which already contain it. Adding it twice reaches past the mapped window, and a device read there
   stalls the bus. */
#define GCC_WRAP2_CORE_2X_RCGR  0x2327c
#define GCC_WRAP2_S4_RCGR       0x1e4d0

/* The serial clocks a QUP engine is usually given, from U-Boot's root_freq[]; 115200 * 32 = 3686400 divides all of
   them, and only the ones it divides evenly are tried. */
static const unsigned serial_clocks[] = {
	7372800, 14745600, 19200000, 29491200, 32000000,
	48000000, 64000000, 80000000, 96000000, 100000000,
};

static vm_offset_t se, gcc_regs;

/* The clock votes: read, change the bits, write back, as the Wi-Fi driver does for PCIe's clocks. */
static void
gcc_change(unsigned offset, uint32_t mask, uint32_t value)
{
	wr(gcc_regs + offset, (rd(gcc_regs + offset) & ~mask) | (value & mask));
}

static bool
uart_wait(vm_offset_t base, unsigned offset, uint32_t mask, uint32_t value, unsigned milliseconds)
{
	for (unsigned spent = 0; spent < milliseconds * 100; spent++) {
		if ((rd(base + offset) & mask) == value) {
			return true;
		}
		delay(10);
	}
	return false;
}

/* What the engine says about itself, before anything is written. Reading a register of an engine whose clock is off is
   what the driver's own note warns about, so the clocks are read first and the rest only printed, not trusted. */
static void
uart_look(void)
{
	uint32_t gates = rd(gcc_regs + GCC_QUPV3_WRAP2_GATES);
	uint32_t revision = rd(se + SE_FW_REVISION_RO);

	/* The registers whose reads are known to answer: the wrapper's gates and the engine's revision. The rest of the
	   block (the DFS and output controls, the FIFO states) is left alone until the engine has shown it runs. */
	say("bt: uart: the wrapper's gates %#x (core %u, s-ahb %u, m-ahb %u, 2x %u, se4 %u), revision %#x (protocol %u)\n",
	    gates, (gates & WRAP2_CORE_CLK) != 0, (gates & WRAP2_S_AHB_CLK) != 0, (gates & WRAP2_M_AHB_CLK) != 0,
	    (gates & WRAP2_CORE_2X_CLK) != 0, (gates & WRAP2_S4_CLK) != 0, revision, (revision >> 8) & 0xff);
}

/* A quiet engine: the main sequencer's command finished or aborted, the secondary's read stopped, the packing set for
   eight-bit words, no parity, one stop bit, no flow control. */
static void
uart_init(void)
{
	if (bt_dry < 8) {
		(void)uart_wait(se, SE_M_IRQ_STATUS, M_CMD_DONE_EN, M_CMD_DONE_EN, 20);
		wr(se + SE_M_IRQ_CLEAR, M_CMD_DONE_EN);
		wr(se + SE_S_CMD_CTRL, S_GENI_CMD_ABORT);
		(void)uart_wait(se, SE_S_CMD_CTRL, S_GENI_CMD_ABORT, 0, 20);
		wr(se + SE_S_IRQ_CLEAR, S_CMD_DONE_EN | S_GENI_CMD_ABORT);
	}
	wr(se + SE_FORCE_DEFAULT, FORCE_DEFAULT);

	wr(se + SE_TX_PACKING_CFG0, UART_PACKING_CFG0);
	wr(se + SE_TX_PACKING_CFG1, 0);
	wr(se + SE_RX_PACKING_CFG0, UART_PACKING_CFG0);
	wr(se + SE_RX_PACKING_CFG1, 0);
	wr(se + SE_TX_TRANS_CFG, 2);	/* no flow control: the chip's RTS is not wired here */
	wr(se + SE_TX_PARITY_CFG, 0);
	wr(se + SE_RX_TRANS_CFG, 0);
	wr(se + SE_RX_PARITY_CFG, 0);
	wr(se + SE_TX_WORD_LEN, 8);
	wr(se + SE_RX_WORD_LEN, 8);
	wr(se + SE_TX_STOP_BIT_LEN, 0);
	wr(se + SE_RX_WATERMARK, 1);
	wr(se + SE_RX_RFR_WATERMARK, 8);
}

static void
uart_divisor(unsigned serial_clock, unsigned baud)
{
	uint32_t cfg = SER_CLK_EN | ((serial_clock / (baud * 32)) << CLK_DIV_SHFT);

	wr(se + SE_SER_M_CLK_CFG, cfg);
	wr(se + SE_SER_S_CLK_CFG, cfg);
}

/*
 * delay() in this tree is not a millisecond: a loop of twelve delay(50) calls took 0.8 ms. The receive watch needs
 * something that actually waits, so this spins on the machine's clock.
 */
static void
wait_ms(unsigned milliseconds)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)milliseconds * 1000000ull, &deadline);
	deadline += mach_absolute_time();
	while (mach_absolute_time() < deadline) {
		;
	}
}

static void
uart_send_raw(const uint8_t *bytes, unsigned length)
{
	/* U-Boot's way, without the interrupt state: the engine takes the bytes after the command is given, and the
	   interrupt registers (which the abort sequence found unreadable) are left alone. */
	wr(se + SE_TX_WATERMARK, 2);
	wr(se + SE_TX_TRANS_LEN, length);
	wr(se + SE_M_CMD0, UART_START_TX_OPCODE);
	delay(1);
	for (unsigned i = 0; i < length; i++) {
		wr(se + SE_TX_FIFO(0), bytes[i]);
	}
	delay(20);
}

static bool
uart_send(const uint8_t *bytes, unsigned length)
{
	for (unsigned i = 0; i < length; i++) {
		wr(se + SE_TX_WATERMARK, 2);
		wr(se + SE_TX_TRANS_LEN, 1);
		wr(se + SE_M_CMD0, UART_START_TX_OPCODE);
		if (!uart_wait(se, SE_M_IRQ_STATUS, M_TX_FIFO_WATERMARK_EN, M_TX_FIFO_WATERMARK_EN, 50)) {
			return false;
		}
		wr(se + SE_TX_FIFO(0), bytes[i]);
		wr(se + SE_M_IRQ_CLEAR, M_TX_FIFO_WATERMARK_EN);
		if (!uart_wait(se, SE_M_IRQ_STATUS, M_CMD_DONE_EN, M_CMD_DONE_EN, 50)) {
			return false;
		}
		wr(se + SE_M_IRQ_CLEAR, M_CMD_DONE_EN);
	}
	return true;
}

/* Whatever the engine has in its fifo within the time given. */
static unsigned
uart_read(uint8_t *out, unsigned most, unsigned milliseconds)
{
	unsigned got = 0;
	uint32_t status;

	wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
	for (unsigned spent = 0; spent < milliseconds * 100 && got < most; spent++) {
		status = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
		while (status != 0 && got < most) {
			out[got++] = (uint8_t)(rd(se + SE_RX_FIFO(0)) & 0xff);
			status = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
		}
		delay(10);
	}
	return got;
}

/* HCI_Reset, the first packet any Bluetooth host sends: type 0x01, opcode 0x0c03, no parameters. A chip that is up
   answers with a Command Complete event (0x04 0x0e ...). */
static void
uart_reset_at(unsigned serial_clock)
{
	static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 };
	uint8_t back[32];
	unsigned got;

	say("bt: uart: the engine at %u Hz, divisor %u\n", serial_clock, serial_clock / (115200 * 32));
	uart_divisor(serial_clock, 115200);
	uart_init();
	say("bt: uart: the engine is set up\n");
	(void)uart_read(back, sizeof(back), 20);	/* whatever was left over */
	if (!uart_send(reset, sizeof(reset))) {
		say("bt: uart: at %u Hz the engine would not take the packet\n", serial_clock);
		return;
	}
	got = uart_read(back, sizeof(back), 300);
	say("bt: uart: at %u Hz (divisor %u) the chip answered %u byte(s):", serial_clock,
	    serial_clock / (115200 * 32), got);
	for (unsigned i = 0; i < got; i++) {
		say(" %02x", back[i]);
	}
	say("%s\n", got ? "" : " (nothing)");
}

/* The serial clock, set the way Linux's clock driver sets one: the divider's parent is picked, a multiplier and a
   divisor are written, and only then is the divider's root turned on. Linux's table for this engine
   (drivers/clk/qcom/gcc-sm8350.c, gcc_qupv3_wrap2_s4_clk_src) has 7,372,800 Hz as GPLL0's even output (source 6)
   with M 384 and N 15,625 and a hidden divider of one: 300 MHz * 384 / 15,625 = 7,372,800, which is 115,200 * 32 *
   2, so the engine's own divisor for 115,200 baud is 2. D is the complement of M, as clk-rcg2 writes it. */
static void
uart_clock_set(void)
{
	uint32_t command, readback[5];

	say("bt: uart: 1. GPLL0 at %#x reads %#x\n", 0x52018, rd(gcc_regs + 0x52018));
	say("bt: uart: 2. the divider at %#lx reads %#x\n", GCC_WRAP2_S4_RCGR, rd(gcc_regs + GCC_WRAP2_S4_RCGR));
	command = rd(gcc_regs + GCC_WRAP2_S4_RCGR);
	if (bt_dry == 1) {
		return;
	}
	/* The root off while the fields are written: the hardware latches them when it is turned back on. */
	wr(gcc_regs + GCC_WRAP2_S4_RCGR, command & ~1u);
	delay(10);
	if (bt_dry == 2) {
		goto readback;
	}
	/* Hidden divider 1, source 6 (GPLL0's even output, 300 MHz), and the hardware clock control bit the working
	   engines have. 300 MHz * 384 / 15,625 = 7,372,800 Hz, the rate U-Boot gives this engine's family. */
	wr(gcc_regs + GCC_WRAP2_S4_RCGR + 4, (1u << 20) | (1u << 8) | bt_src);
	delay(10);
	if (bt_dry == 3) {
		goto readback;
	}
	wr(gcc_regs + GCC_WRAP2_S4_RCGR + 8, 384);	/* M */
	wr(gcc_regs + GCC_WRAP2_S4_RCGR + 0xc, 15625);	/* N */
	wr(gcc_regs + GCC_WRAP2_S4_RCGR + 0x10, ~384u);	/* D */
	delay(10);
	if (bt_dry == 4) {
		goto readback;
	}
	/* The engines that run read 0x80000000 here, so that is the bit that starts this one; bit 0 of the same
	   register reads back zero after a write, so it is a trigger, not the enable. */
	wr(gcc_regs + GCC_WRAP2_S4_RCGR, rd(gcc_regs + GCC_WRAP2_S4_RCGR) | 0x80000000u);
	delay(1000);
readback:
	for (unsigned i = 0; i < 5; i++) {
		readback[i] = rd(gcc_regs + GCC_WRAP2_S4_RCGR + i * 4);
	}
	say("bt: uart: 3. the divider now: %#x %#x %#x %#x %#x\n", readback[0], readback[1], readback[2],
	    readback[3], readback[4]);
	if (bt_dry == 7) {
		/* The step that may stall: the first look at the engine itself. */
		say("bt: uart: 4. the engine at %#lx says %#x\n", SE_PHYS, rd(se + SE_FW_REVISION_RO));
	}

}

/* Level 3, after the supplies and the reset line are on: the engine's state, then an HCI Reset at each candidate. */
static void
uart_after_power(void)
{
	uint32_t gates;

	gates = rd(gcc_regs + GCC_QUPV3_WRAP2_GATES);
	if ((gates & (WRAP2_CORE_CLK | WRAP2_S_AHB_CLK | WRAP2_M_AHB_CLK | WRAP2_CORE_2X_CLK | WRAP2_S4_CLK)) !=
	    (WRAP2_CORE_CLK | WRAP2_S_AHB_CLK | WRAP2_M_AHB_CLK | WRAP2_CORE_2X_CLK | WRAP2_S4_CLK)) {
		gcc_change(GCC_QUPV3_WRAP2_GATES,
		    WRAP2_CORE_CLK | WRAP2_S_AHB_CLK | WRAP2_M_AHB_CLK | WRAP2_CORE_2X_CLK | WRAP2_S4_CLK,
		    WRAP2_CORE_CLK | WRAP2_S_AHB_CLK | WRAP2_M_AHB_CLK | WRAP2_CORE_2X_CLK | WRAP2_S4_CLK);
		say("bt: uart: the wrapper's clocks were off; voted on, now %#x\n",
		    rd(gcc_regs + GCC_QUPV3_WRAP2_GATES));
	}
	uart_clock_set();
	if (bt_dry && bt_dry < 8) {
		return;
	}
	delay(1000);
	uart_look();
	if (bt_dry == 8) {
		uart_divisor(7372800, 115200);
		uart_init();
		say("bt: uart: 8. the engine is set up, the abort and the interrupts left alone\n");
		return;
	}
	if (bt_dry == 10) {
		say("bt: uart: 10. reading the command side with source %u\n", bt_src);
		say("bt: uart: 10. the master's interrupt state reads %#x, the slave's %#x\n",
		    rd(se + SE_M_IRQ_STATUS), rd(se + SE_S_IRQ_STATUS));
		return;
	}
	if (bt_dry == 19) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		uint32_t tx = rd(GPIO_CFG(70)), rx = rd(GPIO_CFG(71));
		wr(GPIO_CFG(70), (tx & ~0x3fu) | (1u << 2) | 3u);
		wr(GPIO_CFG(71), (rx & ~0x3fu) | (1u << 2) | 0u);
		uart_divisor(7372800, 115200);
		uart_init();
		/* The interrupt mask U-Boot sets before it sends: the watermark and the command's end. */
		wr(se + SE_M_IRQ_EN, rd(se + SE_M_IRQ_EN) | M_TX_FIFO_WATERMARK_EN | M_CMD_DONE_EN);
		wr(se + SE_UART_LOOPBACK_CFG, rd(se + SE_UART_LOOPBACK_CFG) | 1u);
		for (unsigned i = 0; i < 32 && (rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK) != 0; i++) {
			(void)rd(se + SE_RX_FIFO(0));
		}
		bool sent = uart_send(reset, sizeof(reset));
		say("bt: uart: 19. loopback %#x; the send says %u\n", rd(se + SE_UART_LOOPBACK_CFG), sent);
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		wait_ms(50);
		unsigned words = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
		say("bt: uart: 19. the loopback gave %u words\n", words);
		for (unsigned i = 0; i < words && i < 16; i++) {
			say("bt: uart: 19. word %u: %#x\n", i, rd(se + SE_RX_FIFO(0)));
		}
		wr(se + SE_S_CMD0, UART_STOP_READ_OPCODE);
		wr(se + SE_UART_LOOPBACK_CFG, rd(se + SE_UART_LOOPBACK_CFG) & ~1u);
		for (unsigned i = 0; i < 32 && (rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK) != 0; i++) {
			(void)rd(se + SE_RX_FIFO(0));
		}
		sent = uart_send(reset, sizeof(reset));
		say("bt: uart: 19. to the chip the send says %u\n", sent);
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		unsigned seen = 0;
		for (unsigned round = 0; round < 10; round++) {
			unsigned have = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
			while (have-- != 0 && seen < 16) {
				say("bt: uart: 19. byte %u: %#x\n", seen++, rd(se + SE_RX_FIFO(0)) & 0xff);
			}
			wait_ms(50);
		}
		say("bt: uart: 19. %u bytes from the chip; the rx line reads %u\n", seen, rd(GPIO_IO(71)) & 1);
		return;
	}
	if (bt_dry == 18) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		uint32_t tx = rd(GPIO_CFG(70)), rx = rd(GPIO_CFG(71));
		wr(GPIO_CFG(70), (tx & ~0x3fu) | (1u << 2) | 3u);
		wr(GPIO_CFG(71), (rx & ~0x3fu) | (1u << 2) | 0u);
		uart_divisor(7372800, 115200);
		uart_init();
		/* The engine, talking to itself: if the four bytes come back the divisor and the engine are right,
		   and whatever is wrong is outside the chip. */
		wr(se + SE_UART_LOOPBACK_CFG, rd(se + SE_UART_LOOPBACK_CFG) | 1u);
		say("bt: uart: 18. loopback %#x, status %#x, m %#x, s %#x\n",
		    rd(se + SE_UART_LOOPBACK_CFG), rd(se + SE_STATUS), rd(se + SE_M_CMD0), rd(se + SE_S_CMD0));
		for (unsigned i = 0; i < 32 && (rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK) != 0; i++) {
			(void)rd(se + SE_RX_FIFO(0));
		}
		uart_send_raw(reset, sizeof(reset));
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		wait_ms(50);
		unsigned words = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
		say("bt: uart: 18. the loopback gave %u words\n", words);
		for (unsigned i = 0; i < words && i < 16; i++) {
			say("bt: uart: 18. word %u: %#x\n", i, rd(se + SE_RX_FIFO(0)));
		}
		wr(se + SE_S_CMD0, UART_STOP_READ_OPCODE);
		wr(se + SE_UART_LOOPBACK_CFG, rd(se + SE_UART_LOOPBACK_CFG) & ~1u);
		/* and now, without the loopback, the same four bytes to the chip. */
		uart_send_raw(reset, sizeof(reset));
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		unsigned seen = 0;
		for (unsigned round = 0; round < 10; round++) {
			unsigned have = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
			while (have-- != 0 && seen < 16) {
				say("bt: uart: 18. byte %u: %#x\n", seen++, rd(se + SE_RX_FIFO(0)) & 0xff);
			}
			wait_ms(50);
		}
		say("bt: uart: 18. %u bytes from the chip; the rx line reads %u\n", seen, rd(GPIO_IO(71)) & 1);
		return;
	}
	if (bt_dry == 17) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		unsigned seen = 0;
		uint32_t tx = rd(GPIO_CFG(70)), rx = rd(GPIO_CFG(71));
		/* The phone's device tree, in the state the UART is used in: the tx pad pulls up, the rx pad does not
		   pull at all, both drive 2 mA. */
		wr(GPIO_CFG(70), (tx & ~0x3fu) | (1u << 2) | 3u | (0u << 6));
		wr(GPIO_CFG(71), (rx & ~0x3fu) | (1u << 2) | 0u | (0u << 6));
		wait_ms(2);
		say("bt: uart: 17. tx pad %#x -> %#x, rx pad %#x -> %#x; the lines read %u and %u\n",
		    tx, rd(GPIO_CFG(70)), rx, rd(GPIO_CFG(71)), rd(GPIO_IO(70)) & 1, rd(GPIO_IO(71)) & 1);
		uart_divisor(7372800, 115200);
		uart_init();
		for (unsigned i = 0; i < 32 && (rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK) != 0; i++) {
			(void)rd(se + SE_RX_FIFO(0));
		}
		uart_send_raw(reset, sizeof(reset));
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		for (unsigned round = 0; round < 20; round++) {
			unsigned words = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
			while (words-- != 0 && seen < 16) {
				uint32_t word = rd(se + SE_RX_FIFO(0));
				say("bt: uart: 17. byte %u: %#x\n", seen++, word & 0xff);
			}
			wait_ms(50);
		}
		say("bt: uart: 17. %u bytes in 1 s; the rx line reads %u\n", seen, rd(GPIO_IO(71)) & 1);
		return;
	}
	if (bt_dry == 16) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		for (unsigned pin = 70; pin <= 71; pin++) {
			uint32_t cfg = rd(GPIO_CFG(pin));
			wr(GPIO_CFG(pin), (cfg & ~0x3cu) | (1u << 2));
		}
		uart_init();
		/* The engine's clock is not known to be the 7.3728 MHz the programming asks for, so the divisor is
		   swept: whichever one gives back 04 0e ... is the one that makes 115200. */
		for (unsigned div = 1; div <= 12; div++) {
			uint8_t answer[12];
			unsigned got = 0;
			uint32_t cfg = SER_CLK_EN | (div << 4);

			wr(se + SE_SER_M_CLK_CFG, cfg);
			wr(se + SE_SER_S_CLK_CFG, cfg);
			/* whatever is left over from the last round */
			for (unsigned i = 0; i < 32 && (rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK) != 0; i++) {
				(void)rd(se + SE_RX_FIFO(0));
			}
			uart_send_raw(reset, sizeof(reset));
			wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
			wait_ms(60);
			while (got < sizeof(answer) && (rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK) != 0) {
				answer[got++] = (uint8_t)(rd(se + SE_RX_FIFO(0)) & 0xff);
			}
			say("bt: uart: 16. divider %u: %u bytes", div, got);
			for (unsigned i = 0; i < got; i++) {
				say(" %02x", answer[i]);
			}
			say("\n");
		}
		return;
	}
	if (bt_dry == 15) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		for (unsigned pin = 70; pin <= 71; pin++) {
			uint32_t cfg = rd(GPIO_CFG(pin));
			wr(GPIO_CFG(pin), (cfg & ~0x3cu) | (1u << 2));
		}
		uart_divisor(7372800, 115200);
		uart_init();
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		/* What is in the queue before anything is sent: the line's idle state and the engine's own leftovers. */
		for (unsigned i = 0; i < 8; i++) {
			uint32_t words = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
			if (words == 0) {
				break;
			}
			say("bt: uart: 15. before: word %u = %#x (%u in the queue)\n", i, rd(se + SE_RX_FIFO(0)), words);
		}
		uart_send_raw(reset, sizeof(reset));
		wait_ms(100);
		say("bt: uart: 15. after the reset the queue holds %u words\n",
		    rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK);
		for (unsigned i = 0; i < 12; i++) {
			uint32_t words = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
			if (words == 0) {
				break;
			}
			say("bt: uart: 15. after: word %u = %#x\n", i, rd(se + SE_RX_FIFO(0)));
		}
		return;
	}
	if (bt_dry == 14) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		unsigned seen = 0;
		/* The phone's own device tree lists, for each of these pins, gpio first and qup18 second
		   (PINGROUP(70, qup18, ...)), so qup18 is function 1 of that pin. */
		for (unsigned pin = 70; pin <= 71; pin++) {
			uint32_t cfg = rd(GPIO_CFG(pin));
			wr(GPIO_CFG(pin), (cfg & ~0x3cu) | (1u << 2));
			wait_ms(1);
			say("bt: uart: 14. gpio%u: %#x -> %#x (function %u)\n", pin, cfg, rd(GPIO_CFG(pin)),
			    (rd(GPIO_CFG(pin)) >> 2) & 15);
		}
		uart_divisor(7372800, 115200);
		uart_init();
		uart_send_raw(reset, sizeof(reset));
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		for (unsigned round = 0; round < 12; round++) {
			unsigned words = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
			while (words-- != 0 && seen < 16) {
				say("bt: uart: 14. byte %u: %#x\n", seen++, rd(se + SE_RX_FIFO(0)) & 0xff);
			}
			wait_ms(50);
		}
		say("bt: uart: 14. %u bytes came back in 600 ms\n", seen);
		return;
	}
	if (bt_dry == 13) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		static const unsigned uart_pins[4] = { 68, 69, 70, 71 };
		unsigned seen = 0;
		for (unsigned i = 0; i < 4; i++) {
			uint32_t cfg = rd(GPIO_CFG(uart_pins[i])), io = rd(GPIO_IO(uart_pins[i]));
			say("bt: uart: 13. gpio%u: config %#x (function %u, pull %u, output %u), reads %u\n",
			    uart_pins[i], cfg, (cfg >> 2) & 15, cfg & 3, (cfg >> 9) & 1, io & 1);
		}
		uart_divisor(7372800, 115200);
		uart_init();
		uart_send_raw(reset, sizeof(reset));
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		for (unsigned round = 0; round < 12; round++) {
			unsigned words = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
			while (words-- != 0 && seen < 16) {
				say("bt: uart: 13. byte %u: %#x\n", seen++, rd(se + SE_RX_FIFO(0)) & 0xff);
			}
			wait_ms(50);
		}
		say("bt: uart: 13. %u bytes came back in 600 ms\n", seen);
		return;
	}
	if (bt_dry == 12) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		unsigned seen = 0;
		uart_divisor(7372800, 115200);
		uart_init();
		uart_send_raw(reset, sizeof(reset));
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		say("bt: uart: 12. the reset is out at 115200; watching the receive queue for 600 ms\n");
		for (unsigned round = 0; round < 12; round++) {
			unsigned words = rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK;
			if (words != 0) {
				say("bt: uart: 12. round %u: %u words\n", round, words);
				while (words-- != 0 && seen < 16) {
					say("bt: uart: 12. byte %u: %#x\n", seen++, rd(se + SE_RX_FIFO(0)) & 0xff);
				}
			}
			wait_ms(50);
		}
		say("bt: uart: 12. %u bytes came back in 600 ms; the engine says %#x\n", seen,
		    rd(se + SE_FW_REVISION_RO));
		return;
	}
	if (bt_dry == 11) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		uart_divisor(7372800, 115200);
		uart_init();
		uart_send_raw(reset, sizeof(reset));
		say("bt: uart: 11. the reset is out; opening the read side\n");
		wr(se + SE_S_CMD0, UART_START_READ_OPCODE);
		delay(50);
		say("bt: uart: 11. the receive queue holds %u words\n", rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK);
		for (unsigned i = 0; i < 8; i++) {
			if ((rd(se + SE_RX_FIFO_STATUS) & FIFO_WORDS_MSK) == 0) {
				break;
			}
			say("bt: uart: 11. byte %u: %#x\n", i, rd(se + SE_RX_FIFO(0)) & 0xff);
		}
		say("bt: uart: 11. done; the engine says %#x\n", rd(se + SE_FW_REVISION_RO));
		return;
	}
	if (bt_dry == 9) {
		static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
		uart_divisor(7372800, 115200);
		uart_init();
		say("bt: uart: 9. sending the reset, no interrupt is read\n");
		uart_send_raw(reset, sizeof(reset));
		say("bt: uart: 9. the four bytes are on the wire at %u Hz; the engine says %#x\n",
		    7372800u, rd(se + SE_FW_REVISION_RO));
		return;
	}
	if (bt_dry < 11) {
		/* A plain bt=3 stops here. The engine answers on its revision register and takes its configuration, but
		   the command side (the interrupt state, the start of a transfer) stops the bus dead while the engine's
		   own clock is not running, and this kernel's guess at that clock has not been shown to start it. So the
		   transfer is only attempted when a bt_dry of 11 or more asks for it. */
		say("bt: uart: the engine is quiet: its command side is not touched (bt_dry=11 tries the transfer)\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(serial_clocks) / sizeof(serial_clocks[0]); i++) {
		if (serial_clocks[i] % (115200 * 32) != 0) {
			continue;
		}
		uart_reset_at(serial_clocks[i]);
	}
}

__startup_func
static void
bt_init(void)
{
	uint32_t enabled = 0, value;
	uint64_t deadline;

	if (!PE_parse_boot_argn("bt", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (PE_parse_boot_argn("bt_dry", &value, sizeof(value))) {
		bt_dry = value;
	}
	if (PE_parse_boot_argn("bt_src", &value, sizeof(value)) && value != 0) {
		bt_src = value;
	}
	if (PE_parse_boot_argn("bt_again", &value, sizeof(value)) && value <= 3600) {
		again_seconds = value;
	}
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	gcc_regs = ml_io_map(GCC_PHYS, 0x100000);
	if (tlmm == 0 || se == 0 || gcc_regs == 0) {
		say("bt: could not map the TLMM, the serial engine or GCC; off\n");
		return;
	}
	uart_level = enabled;
	say("bt: level 1: a look, reading only (WCN6750; nothing is written)\n");
	look("at start");
	if (enabled >= 2) {
		thread_t thread;

		if (kernel_thread_start(power_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			say("bt: could not start the power thread; the pins stay as they are\n");
		}
	}
	if (again_seconds != 0) {
		again_call = thread_call_allocate(look_again, NULL);
		clock_interval_to_deadline(again_seconds, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(again_call, deadline);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bt_init);
