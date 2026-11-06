/*
 * The touch screen of the Xiaomi 11 Pro (mars): a FocalTech FTM5 (K2) on the SPI bus of the SoC's
 * serial engine 11 (QUPV3_WRAP1_S3), as a source of touches for /dev/hakios-touch
 * (sys/hakios.input.h). Only with boot-arg touchscreen=1: every address below belongs to this phone.
 *
 * The driver is the mars port of hakios.goodix.c (the redwood touch driver): the same GENI/SPI,
 * RPMh, TLMM and clock plumbing, and the same polled report loop, but the FocalTech FTM5 protocol
 * instead of Goodix's, on the serial engine and pins that the mars device tree gives the touch
 * screen (fts@0 under &qupv3_se11_spi):
 *   power    the chip's I/O supply is L2C of the PM8350C ("ldoc2" in the command database),
 *            switched through RPMh; the analog supply is behind GPIO 132
 *   clocks   the serial engine's three clocks, by vote in the global clock controller
 *   pins     GPIO 48-51 to the serial engine, plus reset (22), interrupt (23) and the analog
 *            supply switch (132)
 *   SPI      the serial engine ("GENI") in FIFO mode, polled; or, if the engine cannot be used,
 *            the same four pins driven by hand
 *   chip     a FTM5 in SPI4: 0xFA framing (a 32-bit register address, big-endian) for reads and
 *            writes, an 8-byte event FIFO drained with the read-one command 0x86, touch
 *            coordinates 12 bits
 * No interrupt is used: a thread looks at the interrupt line 125 times a second (boot-arg
 * touch_poll_us=N: every N microseconds instead of 8000). With touch_lat=1 a line for every report
 * read ("tl: c before US seen US read US"), for measuring how long a touch takes to show.
 *
 * The firmware the chip runs is the vendor's st_fts_k2.ftb, already flashed at the factory; this
 * driver only reads the running chip and does not download firmware. Register layouts: Linux
 * drivers/spi/spi-geni-qcom.c, drivers/soc/qcom/{qcom-geni-se,rpmh-rsc,cmd-db}.c,
 * drivers/clk/qcom/gcc-sm8350.c, drivers/pinctrl/qcom/pinctrl-sm8350.c,
 * drivers/input/touchscreen/focaltech_touch/{ftsCore,ftsIO,ftsEvent,ftsHW}.h of the mars kernel
 * (st_fts_k2), and the phone's own device tree (mars-sm8350.dtsi).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern void hakios_tl(const char *format, ...) __printflike(1, 2);

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* RPMh's sequencer (TCS 0 of the RSC): the other drivers write the same slots, so every request goes through this lock */
extern lck_mtx_t hakios_rsc_lock;

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_MISO                48
#define PIN_MOSI                49
#define PIN_CLK                 50
#define PIN_CS                  51
#define PIN_INT                 23      /* low = the chip has a report */
#define PIN_RESET               22      /* low = in reset */
#define PIN_AVDD                132     /* high = analog supply on */
#define PIN_FUNCTION_SPI        1       /* "qup11" */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the chip's I/O supply through RPMh --------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL

#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("focaltech: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("focaltech: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done;

	lck_mtx_lock(&hakios_rsc_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("focaltech: RPMh: the command slots stay busy\n");
			lck_mtx_unlock(&hakios_rsc_lock);
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

/* The chip's I/O supply: 1.8 V from L2C of the PM8350C, which the command database knows as "ldoc2" (the same LDO the redwood touch screen uses). */
static bool
io_supply_on(void)
{
	uint32_t base = rpmh_address("ldoc2");
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 1800, 1, VRM_MODE_LDO_HPM };

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("focaltech: RPMh: no regulator \"ldoc2\" (address %#x)\n", base);
		return false;
	}
	printf("focaltech: RPMh: ldoc2 is at %#x; slot status %#x, control %#x, irq enable %#x status %#x\n", base,
	    rd(TCS(TCS_STATUS)), rd(TCS(TCS_CONTROL)), rd(TCS(TCS_IRQ_ENABLE)), rd(TCS(TCS_IRQ_STATUS)));
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	printf("focaltech: I/O supply on (1.8 V)\n");
	return true;
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x52008)         /* the application processor's votes */
#define VOTE_QUP1_CORE_2X       (1u << 18)
#define VOTE_QUP1_CORE          (1u << 19)
#define VOTE_QUP1_M_AHB         (1u << 20)
#define VOTE_QUP1_S_AHB         (1u << 21)
#define VOTE_QUP1_SE3           (1u << 25)             /* serial engine 11 is the 4th of the second group */
#define GCC_QUP1_M_AHB_CBCR     (gcc + 0x18004)         /* bit 31: the clock is off */
#define GCC_QUP1_S_AHB_CBCR     (gcc + 0x18008)
#define GCC_QUP1_SE3_CBCR       (gcc + 0x1839c)         /* halt_reg of qupv3_wrap1_s3 */
#define GCC_QUP1_SE3_CMD        (gcc + 0x183a0)         /* cmd_rcgr: bit 0 = take the new configuration */
#define GCC_QUP1_SE3_CFG        (gcc + 0x183a4)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP1_SE3_DFSR       (gcc + 0x183b4)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP1_SE3_PERF_CFG(l) (gcc + 0x183bc + 4 * (l))
#define GCC_QUP1_SE3_PERF_M(l)  (gcc + 0x183fc + 4 * (l))
#define GCC_QUP1_SE3_PERF_N(l)  (gcc + 0x1843c + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB);
	if (rd(GCC_QUP1_SE3_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP1_SE3_PERF_CFG(level)), rd(GCC_QUP1_SE3_PERF_M(level)),
			    rd(GCC_QUP1_SE3_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("focaltech: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_QUP1_SE3_CFG, (rd(GCC_QUP1_SE3_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP1_SE3_CMD, rd(GCC_QUP1_SE3_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP1_SE3_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP1_SE3_CMD) & 1) {
			printf("focaltech: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP1_SE3_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_SE3);
	for (int i = 0; i < 1000; i++) {
		/*
		 * S_AHB (gcc 0x18008) is not a gate of this SoC: the vendor's table for SM8350 has
		 * qupv3_wrap1's core, core_2x and s0..s5 votes and no s_ahb, and the register reads
		 * "off" whatever is written. The bus runs with M_AHB and the engine's own clock, so
		 * the wait is on those two.
		 */
		if (!((rd(GCC_QUP1_M_AHB_CBCR) | rd(GCC_QUP1_SE3_CBCR)) & CLOCK_OFF)) {
			if (rd(GCC_QUP1_S_AHB_CBCR) & CLOCK_OFF) {
				printf("focaltech: M_AHB and the engine's clock are on; S_AHB still reports off (not waited for)\n");
			}
			return true;
		}
		sleep_us(10);
	}
	printf("focaltech: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP1_M_AHB_CBCR), rd(GCC_QUP1_S_AHB_CBCR),
	    rd(GCC_QUP1_SE3_CBCR));
	return false;
}

/* ---- the serial engine ("GENI") as an SPI master -------------------------------------------------- */

#define SE_PHYS                 0x00a8c000UL    /* qupv3_se11_spi */
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* 0 = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static volatile const char *fts_step;

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints, checks;
	if (checks++ < 12) {
		uint32_t v = rd(GCC_VOTE);
		printf("focaltech: engine check: votes %#x, m_ahb %#x, se %#x\n", v, rd(GCC_QUP1_M_AHB_CBCR),
		    rd(GCC_QUP1_SE3_CBCR));
		if (!(v & (VOTE_QUP1_M_AHB | VOTE_QUP1_SE3))) {
			printf("focaltech: the clocks were voted off; voting again\n");
			wr(GCC_VOTE, v | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB |
			    VOTE_QUP1_SE3);
			sleep_us(500);
		}
	}
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	fts_step = "g-wait-idle";
	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	fts_step = "g-clear";
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	fts_step = "g-len";
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	fts_step = "g-cmd";
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	fts_step = "g-wait-done";
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		fts_step = "g-irq";
		status = rd(GENI_M_IRQ_STATUS);
		fts_step = "g-rx";
		geni_drain(rx, length, &received);
		fts_step = "g-wait-done";
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("focaltech: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- SPI by hand, on the same pins ----------------------------------------------------------------- */

static unsigned bitbang_in = PIN_MISO, bitbang_out = PIN_MOSI;

static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(bitbang_out, false, PULL_NONE, 6);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);

/* ---- the FocalTech FTM5 chip ---------------------------------------------------------------------- */

#define FTS_MAX_TOUCHES         10
#define FTS_MAX_TRANSFER        272

/* FTM5 registers */
#define FTS_REG_DCHIP_ID        0x20000000      /* the chip ID: two bytes (DCHIP_ID_0/1) */
#define FTS_REG_SYSTEM_RESET    0x20000024      /* write 0x81: software reset (only used without a reset GPIO) */
#define FTS_SYSTEM_RESET_VALUE  0x81
#define FTS_CMD_READONE_FIFO    0x87            /* read one 8-byte event out of the FIFO */

#define FTS_EVENT_SIZE          8               /* bytes per FIFO event */
#define FTS_EVT_NOEVENT         0x00
#define FTS_EVT_CONTROLLER_READY 0x03           /* the controller has finished coming up */
#define FTS_EVT_ERROR           0xf0            /* something is wrong */
/* The event id is the high nibble of byte 0: the vendor driver dispatches on evt_data[0] >> 4
 * (fts.c, fts_ts_sleep_work). The low nibble carries how many fingers the chip is tracking, so
 * 0x13 and 0x11 are the same event and must not be compared for equality. Enter (0x1x) and
 * motion (0x2x) are both one sample of a finger on the glass -- the vendor hands both to
 * fts_enter_pointer_event_handler -- and 0x3x is the lift. */
#define FTS_EVT_ID(e)           ((e) & 0xf0)
#define FTS_EVT_ENTER           0x10            /* a finger has landed or moved */
#define FTS_EVT_MOTION          0x20            /* the same, once the finger is known */
#define FTS_EVT_LEAVE           0x30            /* it has lifted */

static uint8_t spi_tx[FTS_MAX_TRANSFER], spi_rx[FTS_MAX_TRANSFER];

/* One FTM5 SPI transaction: 0xFA, then the 32-bit register address big-endian, then data out; a
 * read swaps in as many bytes as it sent. The vendor's framing adds a dummy byte after the address
 * on reads (fts_writeReadU8UX); with full-duplex GENI, the reply lands in rx[6..] (command+address+dummy). */
static volatile const char *fts_step = "none";
static volatile uint32_t fts_ticks;
static volatile uint32_t fts_last_id, fts_id_reads;
static int fts_handmade;

static bool
fts_read(uint32_t address, uint8_t *data, uint32_t length)
{
	uint32_t total = 6 + length;    /* 0xFA + 4 address bytes + 1 dummy + data */

	if (total > FTS_MAX_TRANSFER) {
		return false;
	}
	spi_tx[0] = 0xfb;               /* FTS_CMD_HW_REG_R on SPI (0xfa is the write opcode) */
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	spi_tx[5] = 0x00;               /* the dummy byte */
	memset(spi_tx + 6, 0, length);
	fts_step = "read-tx";
	if (!spi_transfer(spi_tx, spi_rx, total)) {
		fts_step = "read-fail";
		return false;
	}
	fts_step = "read-ok";
	memcpy(data, spi_rx + 6, length);
	return true;
}

static bool
fts_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	uint32_t total = 5 + length;

	if (total > FTS_MAX_TRANSFER) {
		return false;
	}
	spi_tx[0] = 0xfa;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	memcpy(spi_tx + 5, data, length);
	fts_step = "write-tx";
	if (!spi_transfer(spi_tx, NULL, total)) {
		fts_step = "write-fail";
		return false;
	}
	fts_step = "write-ok";
	return true;
}

/* A bare host command: the chip takes opcode + arguments with no register address (fts_write_dma_safe). */
static bool
fts_command(const uint8_t *cmd, uint32_t length)
{
	if (length > FTS_MAX_TRANSFER) {
		return false;
	}
	memcpy(spi_tx, cmd, length);
	return spi_transfer(spi_tx, NULL, length);
}

/* Read one FIFO event: command byte 0x86, then a dummy, then eight data bytes (see fts_read). */
static bool
fts_read_event(uint8_t *event)
{
	uint32_t total = 2 + FTS_EVENT_SIZE;

	if (total > FTS_MAX_TRANSFER) {
		return false;
	}
	spi_tx[0] = FTS_CMD_READONE_FIFO;
	spi_tx[1] = 0x00;               /* the dummy byte */
	memset(spi_tx + 2, 0, FTS_EVENT_SIZE);
	if (!spi_transfer(spi_tx, spi_rx, total)) {
		return false;
	}
	memcpy(event, spi_rx + 2, FTS_EVENT_SIZE);
	return true;
}

/* Read a whole run of FIFO events in one burst, the way the vendor driver drains it: one transfer pops
 * every event it returns, whereas a transfer per event leaves the queue standing still. */
static unsigned
fts_read_events(uint8_t *events, unsigned want)
{
	uint32_t total = 2 + want * FTS_EVENT_SIZE;

	if (want == 0 || total > FTS_MAX_TRANSFER) {
		return 0;
	}
	spi_tx[0] = FTS_CMD_READONE_FIFO;
	spi_tx[1] = 0x00;               /* the dummy byte */
	memset(spi_tx + 2, 0, want * FTS_EVENT_SIZE);
	if (!spi_transfer(spi_tx, spi_rx, total)) {
		return 0;
	}
	memcpy(events, spi_rx + 2, want * FTS_EVENT_SIZE);
	return want;
}

/* The vendor's fts_system_reset: pull the reset line low for 10 ms, then let it go. */
static void
fts_system_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(10);
	gpio_set(PIN_RESET, true);
}

/* Wait for the controller to say it is ready (event byte 0x03), the way ftsCore's pollForEvent does. */
static bool
fts_wait_controller_ready(unsigned attempts)
{
	uint8_t event[FTS_EVENT_SIZE];

	for (unsigned i = 0; i < attempts; i++) {
		if (!fts_read_event(event)) {
			return false;
		}
		if (event[0] == FTS_EVT_CONTROLLER_READY) {
			return true;
		}
		if (event[0] != FTS_EVT_NOEVENT && (event[0] & 0xf0) == FTS_EVT_ERROR) {
			printf("focaltech: controller reports an error (event %02x)\n", event[0]);
			return false;
		}
		sleep_ms(50);
	}
	return false;
}

/* Is the chip there and running? The K2's DCHIP_ID_0 is 0x36 in the common FTM5 family (also seen:
 * 0x39/0x40/0x48/0x50 by variant), so only a non-blank answer is required, not a particular one. */
static bool
fts_probe(bool verbose)
{
	uint8_t id[2] = { 0, 0 };

	if (!fts_read(FTS_REG_DCHIP_ID, id, sizeof(id))) {
		return false;
	}
	fts_last_id = ((uint32_t)id[0] << 8) | id[1];
	fts_id_reads++;
	fts_step = "id-taken";
	(void)verbose;
	return id[0] == 0x36 && id[1] == 0x39;
}

/* What has been seen so far: said once a minute, so that the end of the log always tells. */
static struct {
	uint32_t polls, reports, touches, bad, last_x, last_y;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down;
	uint32_t x, y;
} contact[FTS_MAX_TOUCHES];
static uint32_t flip;           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */
static uint32_t max_x = 1440, max_y = 3200;    /* the device tree's x-max/y-max of fts@0 */

/* One FTM5 FIFO event, the vendor's layout (fts_enter_pointer_event_handler): byte 0 is the event
 * id (0x13 enter/motion, 0x14 leave), byte 1 is the touch id in the high nibble and the touch type
 * in the low nibble, bytes 2-4 hold a 12-bit x and y, byte 5 the angle and bytes 6-7 the major and
 * minor axis. */
/* HAKIOS: touch_trace also shows what the driver hands to iOS, which is what the gesture
 * recogniser actually sees. */
static int fts_trace;

/* HAKIOS: touch_trace keeps the shape of the last gesture and prints it once, at the lift: the
 * point the finger landed on, how far it travelled and where it left. Printed at the lift so it
 * is the last thing on the screen rather than the first thing scrolled off it. */
static struct {
	bool live;
	uint32_t id, down_x, down_y, last_x, last_y, moves, top, bottom;
	uint64_t down_us, up_us;
} gesture;

static uint64_t
now_us(void)
{
	uint64_t us = 0;

	absolutetime_to_nanoseconds(mach_absolute_time(), &us);
	return us / 1000;
}

/* HAKIOS: touch_selftest -- one swipe of our own, from the bottom edge to the top, posted through
 * the same path the chip's samples take. Nothing about the controller is involved. */
static void
selftest_swipe(void)
{
	const uint32_t x = 720 * 65535 / max_x;
	uint32_t y;

	printf("focaltech: selftest: one swipe from the bottom edge\n");
	hakios_touch_post(HAKIOS_TOUCH_DOWN, 0, x, 65514);
	for (y = 65000; y > 4000; y -= 1200) {
		sleep_ms(12);
		hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, x, y);
	}
	sleep_ms(12);
	hakios_touch_post(HAKIOS_TOUCH_UP, 0, x, y);
	printf("focaltech: selftest: the swipe has been posted\n");
}

static void
trace_post(const char *what, uint32_t id, uint32_t x, uint32_t y)
{
	if (!fts_trace) {
		return;
	}
	if (what[0] == 'd') {
		gesture.live = true;
		gesture.id = id;
		gesture.down_x = gesture.last_x = x;
		gesture.down_y = gesture.last_y = y;
		gesture.top = gesture.bottom = y;
		gesture.moves = 0;
		gesture.down_us = now_us();
	} else if (gesture.live) {
		gesture.moves++;
		gesture.last_x = x;
		gesture.last_y = y;
		if (y < gesture.top) {
			gesture.top = y;
		}
		if (y > gesture.bottom) {
			gesture.bottom = y;
		}
		if (what[0] == 'u') {
			gesture.live = false;
			gesture.up_us = now_us();
			printf("focaltech: gesture id %u down %u,%u y %u..%u upto %u,%u\n"
			    "focaltech: gesture %u samples in %llu us, y went %u -> %u -> %u (%u tall)\n",
			    id, gesture.down_x, gesture.down_y, gesture.down_y, gesture.top,
			    gesture.last_x, gesture.last_y,
			    gesture.moves, gesture.up_us - gesture.down_us,
			    gesture.down_y, y, gesture.top, gesture.down_y);
		}
	}
}

static void
fts_report(const uint8_t *event)
{
	uint32_t type = FTS_EVT_ID(event[0]), id = event[1] >> 4;
	uint32_t x = ((uint32_t)(event[3] & 0x0f) << 8) | event[2];
	uint32_t y = ((uint32_t)event[4] << 4) | ((event[3] & 0xf0) >> 4);

/* HAKIOS: touch_trace -- every event, raw, with the time it was read, so that one whole swipe can be
 * followed from the first sample to the last. */
	{
		static int trace_arg = -1;

		if (trace_arg < 0) {
			trace_arg = 0;
			PE_parse_boot_argn("touch_trace", &trace_arg, sizeof(trace_arg));
		}
		if (trace_arg) {
			uint64_t us = 0;

			absolutetime_to_nanoseconds(mach_absolute_time(), &us);
			printf("focaltech: ev %02x %02x %02x %02x %02x %02x %02x %02x id %u x %u y %u us %llu\n",
			    event[0], event[1], event[2], event[3], event[4], event[5], event[6], event[7],
			    id, x, y, us / 1000);
		}
	}

	if (type != FTS_EVT_ENTER && type != FTS_EVT_MOTION && type != FTS_EVT_LEAVE) {
		return;
	}
	if (id >= FTS_MAX_TOUCHES) {
		return;
	}
	if (x >= max_x) {
		x = max_x ? max_x - 1 : 0;
	}
	if (y >= max_y) {
		y = max_y ? max_y - 1 : 0;
	}
	x = x * 65535 / max_x;
	y = y * 65535 / max_y;
	if (flip & 1) {
		x = 65535 - x;
	}
	if (flip & 2) {
		y = 65535 - y;
	}
	seen.touches++;
	seen.last_x = x;
	seen.last_y = y;
	if (type == FTS_EVT_LEAVE) {
		if (contact[id].down) {
			trace_post("up", id, x, y);
			hakios_touch_post(HAKIOS_TOUCH_UP, id, x, y);
		}
		contact[id].down = false;
	} else if (!contact[id].down) {
		trace_post("down", id, x, y);
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		contact[id].down = true;
	} else if (contact[id].x != x || contact[id].y != y) {
		trace_post("move", id, x, y);
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
	}
	contact[id].x = x;
	contact[id].y = y;
}

/* Drain the event FIFO: the vendor reads one event, acts on it, and repeats until NOEVENT. */
static void
fts_drain(void)
{
	uint8_t batch[FTS_EVENT_SIZE * 32];

	seen.polls++;
	for (unsigned round = 0; round < 8; round++) {
		unsigned got = fts_read_events(batch, 32);
		unsigned used = 0;

		if (got == 0) {
			seen.bad++;
			return;
		}
		for (unsigned i = 0; i < got; i++) {
			uint8_t *event = batch + i * FTS_EVENT_SIZE;
			unsigned left;

			if (event[0] == FTS_EVT_NOEVENT) {
				return;
			}
			if ((event[0] & 0xf0) == FTS_EVT_ERROR) {
				if (seen.bad < 8) {
					printf("focaltech: controller error event %02x\n", event[0]);
				}
				seen.bad++;
				return;
			}
			seen.reports++;
			fts_report(event);
			used = i + 1;
			left = event[7] & 0x1f;
			if (left == 0) {
				round = 4;
				break;
			}
		}
		if (used == 0) {
			return;
		}
	}
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	printf("focaltech: %s: votes %#x, clocks %#x %#x %#x, rate cfg %#x dfs %#x; pins cs/clk/mosi/miso cfg %#x %#x %#x %#x,"
	    " reset %#x/%u, int %#x/%u, avdd %#x/%u\n", when, rd(GCC_VOTE), rd(GCC_QUP1_M_AHB_CBCR),
	    rd(GCC_QUP1_S_AHB_CBCR), rd(GCC_QUP1_SE3_CBCR), rd(GCC_QUP1_SE3_CFG), rd(GCC_QUP1_SE3_DFSR), rd(GPIO_CFG(PIN_CS)),
	    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_RESET)),
	    rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)), rd(GPIO_CFG(PIN_AVDD)), rd(GPIO_IO(PIN_AVDD)));
}

/* Power the chip up from nothing, in the vendor driver's order. False if the I/O supply could not be switched on. */
static bool
power_on(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
	/* without its I/O rail the chip must not get the analog supply or be driven: stop before AVDD */
	if (!io_supply_on()) {
		return false;
	}
	sleep_ms(3);
	gpio_output(PIN_AVDD, true, PULL_NONE, 2);
	sleep_ms(15);
	/*
	 * The reset line has been low since the first line of this function, and the vendor's
	 * power-on order is the other way round: both rails, then the reset released, then the
	 * chip's own boot before the first read of its registers (fts_gl_reset_ctrl: 50 ms at
	 * least, 200 ms safe). Left low, every read of the chip returns 0xff.
	 */
	gpio_set(PIN_RESET, true);
	{
		/* boot-arg touch_reset_ms=N: how long the chip gets to boot (vendor: 50 ms at least, 200 safe) */
		uint32_t boot_ms = 0;

		PE_parse_boot_argn("touch_reset_ms", &boot_ms, sizeof(boot_ms));
		if (boot_ms < 10 || boot_ms > 3000) {
			boot_ms = 200;
		}
		sleep_ms(boot_ms);
		printf("focaltech: reset released, the chip had %u ms to boot\n", boot_ms);
	}
	return true;
}

static void
use_engine_pins(void)
{
	gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
}

/* The FTM5 parts come up in three-wire SPI and only drive MISO once ICR says SPI4 (fts_spi4_mode_set
 * in the vendor's fts.c). Until then a read just clocks our own MOSI straight back in. */
static bool
fts_spi4_setup(void)
{
	static const struct { uint32_t address; uint8_t value; const char *name; } seq[] = {
		{ 0x20000032, 0x10, "gpio direction" },
		{ 0x20000034, 0x02, "gpio pull-up" },
		{ 0x2000003f, 0x07, "gpio config 2" },
		{ 0x2000003d, 0x30, "gpio config 0" },
		{ 0x2000002d, 0x02, "interface control (SPI4)" },
	};

	for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
		fts_step = seq[i].name;
		if (!fts_write(seq[i].address, &seq[i].value, 1)) {
			printf("focaltech: the write to the %s register did not go out\n", seq[i].name);
			return false;
		}
		sleep_ms(2);
	}
	sleep_ms(10);
	return true;
}

static const char *volatile fts_stage = "init";
static int fts_quiet;   /* boot-arg quiet: keep the log for the touch lines only */
static int fts_watch;   /* boot-arg touch_watch: the watchdog thread and the periodic lines */

/* Read a few registers first: if the bus itself is dead, this is where it shows. */
static void
bus_probe(void)
{
	static const struct { uint32_t offset; const char *name; } regs[] = {
		{ 0x000, "geni status" },
		{ 0x040, "geni irq" },
		{ 0x600, "m cmd0" },
		{ 0x804, "rx fifo status" },
	};

	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		fts_step = regs[i].name;
		printf("focaltech: bus probe: %s reads %#x\n", regs[i].name, rd(SE_PHYS + regs[i].offset));
	}
}

static bool
bring_up(void)
{
	const char *problem = "its clocks cannot be turned on";

	show_state("as found");
	fts_stage = "power-on";
	if (!power_on()) {
		printf("focaltech: the I/O supply is not on; the chip is left alone\n");
		return false;
	}

	{
		int bb = 0;   /* the engine is the default; the pins are the last resort */

		PE_parse_boot_argn("touch_bitbang", &bb, sizeof(bb));
		if (bb) {
			/* the serial engine stops answering once the full system runs; the pads still work */
			printf("focaltech: driving the bus by hand (touch_bitbang)\n");
			spi_transfer = bitbang_transfer;
			fts_stage = "bitbang";
			for (int pass = 0; pass < 2; pass++) {
				fts_ticks++;
				bitbang_in = pass == 0 ? PIN_MISO : PIN_MOSI;
				bitbang_out = pass == 0 ? PIN_MOSI : PIN_MISO;
				bitbang_init();
				(void)fts_spi4_setup();
				for (unsigned attempt = 0; attempt < 3; attempt++) {
					fts_ticks++;
					fts_step = "probing";
					if (fts_probe(true)) {
						fts_stage = "hand-ok";
						fts_handmade = 1;
						return true;
					}
					fts_step = "sleeping";
					sleep_ms(20);
					fts_step = "next-attempt";
				}
			}
			return false;
		}
	}
	fts_stage = "clocks";
	{
		int probe = 0;

		PE_parse_boot_argn("touch_geniprobe", &probe, sizeof(probe));
		if (probe) {
			bus_probe();
		}
	}
	if (clocks_on()) {
		printf("focaltech: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
		    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
		uint32_t spi_hz = 7000000;

		/* boot-arg touch_spi_hz=N: the bus at N Hz (the device tree gives the chip 7 MHz) */
		PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
		if (spi_hz < 100000 || spi_hz > 10000000) {
			spi_hz = 7000000;
		}
		printf("focaltech: the bus at %u Hz\n", spi_hz);
		fts_stage = "engine-init";
		problem = geni_spi_init(spi_hz);
	}
	if (problem == NULL) {
		fts_stage = "pins";
		use_engine_pins();
		spi_transfer = geni_transfer;
		show_state("powered, on the serial engine");
		fts_stage = "spi4";
		if (fts_spi4_setup()) {
			printf("focaltech: the chip has been told to use SPI4\n");
		}
		{
			bool answered = false;

			for (unsigned attempt = 0; attempt < 10 && !answered; attempt++) {
				printf("focaltech: probe attempt %u\n", attempt + 1);
				answered = fts_probe(true);
				if (!answered) {
					sleep_ms(50);
				}
			}
			if (answered) {
				fts_handmade = 1;   /* the chip is up and in SPI4; a system reset would undo that */
				return true;
			}
		}
		printf("focaltech: the chip did not answer through the serial engine; trying the pins by hand\n");
	} else {
		printf("focaltech: %s; driving the pins by hand\n", problem);
	}

	/* by hand; which of the two data pins is the input is not written down anywhere, so try both */
	spi_transfer = bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		if (fts_probe(true)) {
			printf("focaltech: the chip answers on pins driven by hand (data in on gpio%u)\n", bitbang_in);
			return true;
		}
	}
	show_state("no answer");
	return false;
}

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

#include <sys/proc_internal.h>
#include <sys/resource.h>

extern int fill_task_rusage(task_t task, rusage_info_current *ri);

static char process_cpu_line[448];
static uint64_t proc_cpu_last[512];
static int      proc_cpu_pid[512];
/* the three largest absolute totals, so that "nothing printed" can be told apart from "all zero" */
static struct { char comm[24]; uint64_t total; } proc_cpu_top[3];

static int
process_cpu_collect(proc_t p, void *arg)
{
	rusage_info_current ri = { 0 };
	uint64_t now, delta = 0, delta_ns = 0;
	unsigned at = *(unsigned *)arg;
	int pid = p->p_pid;
	unsigned slot;

	if (pid <= 0 || fill_task_rusage(proc_task(p), &ri) != 0) {
		return 0;
	}
	now = ri.ri_user_time + ri.ri_system_time;
	{
		uint64_t total_ns = 0;

		absolutetime_to_nanoseconds(now, &total_ns);
		for (int k = 0; k < 3; k++) {
			if (total_ns > proc_cpu_top[k].total) {
				for (int j = 2; j > k; j--) {
					proc_cpu_top[j] = proc_cpu_top[j - 1];
				}
				snprintf(proc_cpu_top[k].comm, sizeof(proc_cpu_top[k].comm), "%s", p->p_comm);
				proc_cpu_top[k].total = total_ns;
				break;
			}
		}
	}
	slot = (unsigned)pid % 512;
	if (proc_cpu_pid[slot] != pid) {
		proc_cpu_pid[slot] = pid;
		proc_cpu_last[slot] = now;
		return 0;
	}
	delta = now > proc_cpu_last[slot] ? now - proc_cpu_last[slot] : 0;
	proc_cpu_last[slot] = now;
	/* the rusage times are in mach absolute time, not nanoseconds: convert before comparing */
	absolutetime_to_nanoseconds(delta, &delta_ns);
	if (delta_ns >= 20 * NSEC_PER_MSEC && at < 320) {
		at += snprintf(process_cpu_line + at, sizeof(process_cpu_line) - at, " %s:%llu", p->p_comm,
		    delta_ns / NSEC_PER_MSEC);
	}
	return 0;
}

static void
process_cpu_print(void)
{
	unsigned at = 0;

	process_cpu_line[0] = 0;
	for (int k = 0; k < 3; k++) {
		proc_cpu_top[k].comm[0] = 0;
		proc_cpu_top[k].total = 0;
	}
	at = snprintf(process_cpu_line, sizeof(process_cpu_line), "focaltech: process cpu (ms):");
	proc_iterate(PROC_ALLPROCLIST, process_cpu_collect, &at, NULL, NULL);
	at += snprintf(process_cpu_line + at, sizeof(process_cpu_line) - at,
	    " | totals:%s:%llu%s:%llu%s:%llu\n", proc_cpu_top[0].comm, proc_cpu_top[0].total / NSEC_PER_MSEC,
	    proc_cpu_top[1].comm, proc_cpu_top[1].total / NSEC_PER_MSEC,
	    proc_cpu_top[2].comm, proc_cpu_top[2].total / NSEC_PER_MSEC);
	printf("%s", process_cpu_line);
}

static void
busy_line(void)
{
	extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
	static uint64_t was_idle[16];
	static uint64_t was_wall;
	uint64_t wall = mach_absolute_time(), idle, d_idle = 0, d_wall = 0;
	char line[160];
	int at;

	if (was_wall == 0) {
		was_wall = wall;
		for (int i = 0; i < 16; i++) {
			(void)ml_cpu_idle_time(i, &was_idle[i]);
		}
		return;
	}
	absolutetime_to_nanoseconds(wall - was_wall, &d_wall);
	at = snprintf(line, sizeof(line), "focaltech: cpu busy");
	for (int i = 0; i < 8; i++) {
		if (!ml_cpu_idle_time(i, &idle)) {
			continue;
		}
		absolutetime_to_nanoseconds(idle - was_idle[i], &d_idle);
		at += snprintf(line + at, sizeof(line) - at, " %u%%",
		    d_wall && d_idle < d_wall ? (unsigned)(100 - d_idle * 100 / d_wall) : 100);
		was_idle[i] = idle;
	}
	line[at++] = '\n';
	line[at] = 0;
	printf("%s", line);
	was_wall = wall;
}

static void
focaltech_watchdog(__unused void *parameter, __unused wait_result_t result)
{
	int secs = 0;
	unsigned i;
	PE_parse_boot_argn("touch_panic_secs", &secs, sizeof(secs));
	for (i = 1;; i++) {
		sleep_us(5000000);
		{
			static int busy_on = -1;
			if (busy_on < 0) {
				busy_on = 0;
				PE_parse_boot_argn("cpu_busy", &busy_on, sizeof(busy_on));
			}
			if (busy_on) {
				static int proc_on = -1;

				if (proc_on < 0) {
					proc_on = 0;
					PE_parse_boot_argn("proc_cpu", &proc_on, sizeof(proc_on));
				}
				busy_line();
				if (proc_on) {
					process_cpu_print();
				}
			}
		}
		if (gcc != NULL) {
			uint32_t v = rd(GCC_VOTE);

			if (fts_quiet) { goto quiet_skip; }
			printf("focaltech: watchdog %u s: stage %s, step %s, ticks %u, ids %u/%04x, votes %#x, m_ahb %#x, se %#x\n",
			    i * 5, fts_stage, fts_step, fts_ticks, fts_id_reads, fts_last_id, v, rd(GCC_QUP1_M_AHB_CBCR),
			    rd(GCC_QUP1_SE3_CBCR));
			{
				int wd_se = 0;

				PE_parse_boot_argn("touch_wd_se", &wd_se, sizeof(wd_se));
				if (wd_se) {
					printf("focaltech: watchdog reads the engine: status %#x, irq %#x, txlen %#x, rxlen %#x\n",
					    rd(GENI_STATUS), rd(GENI_M_IRQ_STATUS), rd(SPI_TX_TRANS_LEN),
					    rd(SPI_RX_TRANS_LEN));
				}
			}
quiet_skip: ;
			if (fts_stage != NULL && strcmp(fts_stage, "poll") != 0 &&
			    !(v & (VOTE_QUP1_M_AHB | VOTE_QUP1_SE3))) {
				printf("focaltech: the clocks are voted off; voting again\n");
				wr(GCC_VOTE, v | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB |
				    VOTE_QUP1_SE3);
			}
		} else {
			printf("focaltech: watchdog %u s: stage %s\n", i * 5, fts_stage);
		}
		if (secs > 0 && (int)(i * 5) >= secs) {
			panic("HAKIOS touch: stage %s after %u s", fts_stage, i * 5);
		}
	}
}

static void
focaltech_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0, idle_looks;
	uint32_t poll_us = 8000, latency = 0, reads = 0;
	uint64_t before = 0, reading = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_lat", &latency, sizeof(latency));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	idle_looks = 80000 / poll_us;   /* a report read every 80 ms even with the line high, in case it is missed */

	fts_stage = "thread-start";
	printf("focaltech: probe: the thread runs\n");
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	printf("focaltech: probe: registers mapped (tlmm %#lx, gcc %#lx, se %#lx, rsc %#lx)\n",
	    (unsigned long)tlmm, (unsigned long)gcc, (unsigned long)se, (unsigned long)rsc);

	fts_stage = "bringing-up";
	if (!bring_up()) {
		printf("focaltech: no touch screen; the thread stays and retries (stage %s, step %s)\n", fts_stage, fts_step);
		for (unsigned r = 0;; r++) {
			fts_ticks++;
			sleep_ms(5000);
			if (fts_watch) {
				printf("focaltech: still no touch screen: stage %s, step %s, retry %u\n",
				    fts_stage, fts_step, r);
			}
			if (r % 6 == 5) {
				(void)bring_up();
			}
		}
	}
	/* bring the controller up and wait until it says it is ready */
	if (!fts_handmade) {
		fts_system_reset();
	}
	{
		uint32_t diag = 0;

		if (PE_parse_boot_argn("touch_diag", &diag, sizeof(diag)) && diag) {
			uint8_t id[2] = { 0, 0 }, fw[2] = { 0, 0 };
			uint8_t e1[8], e2[8];
			uint32_t rx0, rx1;
			bool ok_id, ok_fw, ok_e1, ok_e2;

			memset(e1, 0, sizeof(e1));
			memset(e2, 0, sizeof(e2));
			ok_id = fts_read(FTS_REG_DCHIP_ID, id, 2);
			rx0 = ((uint32_t)spi_rx[0] << 24) | ((uint32_t)spi_rx[1] << 16) | ((uint32_t)spi_rx[2] << 8) | spi_rx[3];
			rx1 = ((uint32_t)spi_rx[4] << 24) | ((uint32_t)spi_rx[5] << 16) | ((uint32_t)spi_rx[6] << 8) | spi_rx[7];
			ok_fw = fts_read(0x20000004, fw, 2);
			ok_e1 = fts_read_event(e1);
			ok_e2 = fts_read_event(e2);
			printf("focaltech: diag id %d %02x %02x fw %d %02x %02x ev %d/%d int %u\n",
			    ok_id, id[0], id[1], ok_fw, fw[0], fw[1], ok_e1, ok_e2, gpio_get(PIN_INT));
			printf("focaltech: HAKIOS touch_diag id=%d %02x%02x fw=%d %02x%02x rx=%08x %08x int=%u ev1=%d %02x%02x%02x%02x ev2=%d %02x%02x%02x%02x",
			    ok_id, id[0], id[1], ok_fw, fw[0], fw[1], rx0, rx1, gpio_get(PIN_INT),
			    ok_e1, e1[0], e1[1], e1[2], e1[3], ok_e2, e2[0], e2[1], e2[2], e2[3]);
		}
	}
	if (!fts_handmade && !fts_wait_controller_ready(40)) {
		printf("focaltech: the controller does not say it is ready after reset\n");
	}
	fts_stage = "poll";
	printf("focaltech: touch screen ready; the interrupt line is looked at every %u us\n", poll_us);
	{
		/* The chip idles until the host says which scanning to run: 0xA0, active mode, multi-touch. */
		static const uint8_t scan_on[3] = { 0xa0, 0x00, 0x01 };

		if (fts_command(scan_on, sizeof(scan_on))) {
			printf("focaltech: scan mode set to active multi-touch\n");
		} else {
			printf("focaltech: the scan mode command did not go out\n");
		}
	}
	{
		static int selftest = -1;

		if (selftest < 0) {
			selftest = 0;
			PE_parse_boot_argn("touch_selftest", &selftest, sizeof(selftest));
		}
		if (selftest) {
			sleep_ms(70000);
			selftest_swipe();
		}
	}
	for (unsigned tick = 1;; tick++) {
		{ static uint64_t t0; static int panic_secs = -1;
		  if (panic_secs < 0) { panic_secs = 0; PE_parse_boot_argn("touch_panic_secs", &panic_secs, sizeof(panic_secs)); }
		  if (t0 == 0) { t0 = micros_now(); }
		  if (panic_secs > 0 && micros_now() - t0 > (uint64_t)panic_secs * 1000000ULL) {
			panic("HAKIOS touch: alive %d s, %u events, %u fingers, %u bad, int %d, last %u,%u",
			    panic_secs, seen.reports, seen.touches, seen.bad, gpio_get(PIN_INT) ? 1 : 0, seen.last_x, seen.last_y);
		  } }
		if ((tick % (2 * (1000000 / poll_us))) == 1) {
			if (!fts_watch || fts_quiet) { goto quiet_alive_skip; }
			printf("focaltech: alive tick %u, %u events, int %s\n", tick, seen.reports, gpio_get(PIN_INT) ? "high" : "low");
quiet_alive_skip: ;
		}
		uint64_t looked = micros_now(), read_at;

		/* the interrupt line is low while a report waits; look anyway every 80 ms in case it is missed */
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			fts_drain();
			read_at = micros_now();
			reads++;
			reading += read_at - looked;
			if (latency) {
				hakios_tl("tl: c before %llu seen %llu read %llu\n", before, looked, read_at);
			}
		} else if (++idle >= idle_looks) {
			idle = 0;
			fts_drain();
			reads++;
			reading += micros_now() - looked;
		}
		before = looked;
		if (tick % (2 * (1000000 / poll_us)) == 0) {
			if (!fts_watch || fts_quiet) { goto quiet_seen_skip; }
		printf("focaltech: seen %u events, %u finger positions, %u bad, int %s, last %u,%u\n",
			    seen.reports, seen.touches, seen.bad, gpio_get(PIN_INT) ? "high" : "low",
			    seen.last_x, seen.last_y);
quiet_seen_skip: ;
		}
		sleep_us(poll_us);
	}
}

void
hakios_focaltech_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	printf("focaltech: probe: init called\n");
	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		printf("focaltech: probe: no touchscreen= boot argument (or 0): the driver is left out\n");
		return;
	}
	printf("focaltech: probe: touchscreen=%u, starting the thread\n", enabled);
	PE_parse_boot_argn("quiet", &fts_quiet, sizeof(fts_quiet));
	PE_parse_boot_argn("touch_watch", &fts_watch, sizeof(fts_watch));
	PE_parse_boot_argn("touch_trace", &fts_trace, sizeof(fts_trace));
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(focaltech_thread, NULL, &thread) == KERN_SUCCESS) {
		if (fts_watch) {
			thread_t wd;
			kernel_thread_start(focaltech_watchdog, NULL, &wd);
		}
		thread_deallocate(thread);
	}
}
