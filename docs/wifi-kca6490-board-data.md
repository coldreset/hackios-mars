# Wi-Fi（QCA6490 / KCA6490）板级数据投递：实测边界与结论

日期：2026-11-09 夜至 11-10 凌晨。设备：小米 11 Pro（mars / SM8350）。
内核：`xnu-src/xnu-8796.121.2`，ath11k 来自 `xnu-deps/build/ath11k`（`HAKIOS_PCIE=1 ./tools/mk-ath11k.sh`）。

## 已经打通的阶段

| 阶段 | 结果 | 证据 |
|---|---|---|
| PCIe 链路 / SMMU / MHI / QRTR | 通 | stage 10–20 |
| 24 MB 固件下载（0x25 type 3） | 通 | BHIE 5,236,304 B |
| QMI 注册、能力查询 | 通 | `QMI in 0x20 … result 0`、`0x24` 回 106 字节 |
| 内存请求 / 响应（0x35 / 0x36） | **通** | 固件回 `0x36 … 02 04 00 00 00`（result 0） |
| 内存就绪（0x37）、固件版本、m3 info（0x3c） | 通 | `0x37` 指示、`WLAN.HSP.1.1-c3-…-1.5-80739.2.5.569741.4` |
| regdb 下载（0x25 type 4） | **通** | 4 片，每片 `result 0` |
| 板级数据下载（0x25 type 0/1） | **卡住** | 见下 |

板级数据的两个关键地址（按厂商布局，`dma_allocate_at()` 固定 IOVA）：

- DDR 堆：`0xa1000000`，6,160,384 B（固件往里写东西，已验证）
- 校准区：`0xa1600000`，3,801,088 B（目前为空）
- 其余环（MHI/QRTR）一律从 `0xa6000000` 起分配

## 实测的两条硬边界

板级数据 59,868 B（`bdwlan.elf`）或 58,180 B（裸 BDF）：

1. **每条消息太长会被明确拒绝**：14,590 B 的首片立刻回
   `QMI in 0x25 … result=1`，`board file download request failed: 1 19`
   （error 19 = `QMI_PROTOCOL_ERROR_…` 过大类）。6,189 B 的消息则被接受。
2. **消息条数到第 5 条就断**：无论片大小 6,144 还是 2,048、间隔 20 ms 还是
   5,000 ms、`total_size` 递减还是恒定、BDF 用裸文件还是 ELF、`seg_id`
   重复写 0 —— 第 5 条 `0x25` 一律无回应，且此后固件对所有消息（含 `0x3c`）
   都不再应答，直到设备复位。

两条边界合起来：4 × 6,189 ≈ 24.6 KB，装不下 58–59 KB；而 4 × 14,967 才够，
但 14,590 已经被拒。

## 排除项（都做过对照实验）

- 主机侧丢包：RX 的拒绝/乱序路径已改为可见日志，无一条命中。
- 主机侧发送上限：`qrtr_send()` 曾有 `length > 8000` 的静默 EINVAL（已修）；
  15,012 B 的消息现在能真正发出并拿到固件回应。
- 固件版本：`amss.bin`（eval_v1）与 `amss20.bin`（eval_v2）行为完全一致。
- 板级文件变体：`fw-wifi` 里 147 个 `bdwlan.eNN` 全部 59,868 B、互不相同，
  换变体不改变行为；`bdwlan.elf` 只有 1 个 PT_LOAD，没有更小的子镜像。
- 供电/时钟/PHY：stage 1–9 全部通过。

## 下一步（按优先级）

1. **反汇编 `amss.bin`**，找处理 `QMI_WLANFW_BDF_DOWNLOAD` 的那段：确认它维护
   的消息计数/缓冲上限是第一手信息，胜过继续盲试。工具：`llvm-objdump
   --triple=thumbv7-none-eabi`（ARM/Thumb 混排，需按 vaddr 定位）。
2. 二分单条上限（6,189 与 14,590 之间），确认余量。
3. 试 delayed 路径让固件主动重发更小的段表（`mem_seg_len=0` 回包）。

## 复现命令

```bash
cd /opt/hackios/hackios
HAKIOS_PCIE=1 ./tools/mk-ath11k.sh && make kernel
export DEVICE=mars
./xl reload && ./xl ramdisk userland/system.apfs
# 见 default-workspace/r128.sh（跳过 regdb、ELF 板级数据）
bash r128.sh
```

## 2026-11-20：44 MB 分配打通，内存握手卡在固件内部

**分配修好了，并且是通用修法，不是挪常量。** 老代码有三处互相纠缠的错误：

- `dma_allocate_at()` 里 `page = 7 + ((at - WIFI_DATA_IOVA) >> 21)`：`at` 是池起点，常量却是另一个地址，
  无符号相减下溢，页号越界，函数在写任何东西之前就 `return false`；
- 前面的 `iova > WIFI_DATA_LIMIT - size - PAGE_SIZE` 把显式的 `at` 全部拒绝（`0xf0000000` 永远大于 `0xb0000000`）；
- L1 索引 3（`0xc0000000` 以上）根本没有 L2 表，所以即使页号修好也走不了表。

现在改成真的走表：`dma_l2()`/`dma_table_page()`/`dma_leaf()`/`dma_walk()`，表块第 7 页是 L1[3] 的 L2 表，
叶子从第 8 页起（`WIFI_LEAF_FIRST`，共 120 张，`WIFI_TABLE_PAGES` 128）。每次运行的日志都是

    wifi-pcie: owned DMA allocation 2 requested 46137344 mapped 46137344 pages 11264 guards verified

每种拒绝都带原因（`QMI probe: DMA allocation %u refused: %s (at %#llx, %u bytes)`），下一次不用再猜。

**`ladder.sh` 每轮都在上传 2.69 GB 的系统镜像。** `./xl ramdisk userland/system.apfs` 之后，
loader 把它当作 md0 驻留，内核在 bootstrap 就 wired 2.65 GB（`219511 free pages, 173705 wired pages`），
6 GB 窗口里 4 GiB 以下**没有一个连续的 MB**，wifi 的所有缓冲因此落在 0x19f…（约 6.9 GB）。
换成 `run_low.sh`（`./xl ramdisk none`，只带 19.7 MB 的 `root.apfs`）后：

    vm_page_bootstrap: 386117 free pages, 7099 wired pages
    DMA allocation 0: its memory is at 0xfe0cc000 / 1: 0xfe6ac000 / 2: 0xfea4c000

与 11-15 那轮（0xfd17…/0xfd7d…/0xfdab…）形状一致。这一点以后跑 wifi 一定要用 `run_low.sh`。

**固件仍然在内存握手处崩，而且崩在它自己内部。** 我们回的 0x36 与 11-15 **逐字节相同**
（`00 03 00 36 00 26 00 01 23 00 02 00 1b 01 a6 00 … 1b 5f a6 00 …`，池地址 0xa6011b00 / 0xa65f1b00），
QMI 序列（0x20 → 0x34 → 0x35 → 0x36）也逐行相同；11-15 在 4.6 ms 后回 `0x36 … result 0`，今天没有任何回应，
0.2 秒后 `MHI event ring 0 … value 255`（SYS_ERR），`EE 0x3`。加在失败路径上的探针读出：

    the buffer at 0xa6011b00 holds 00000000 …      (8 words, all zero)
    the buffer at 0xa65f1b00 holds 00000000 …
    the whole buffer 0xa6000000 starts 00000000 …
    SMMU fault status 0x400 far 0x…, MHI status 0x80000001 EE 0x3

即：固件**从未读写**我们交给它的那块主机内存，SMMU 也没有任何 fault——它在处理这个应答时自己崩了。
把 6.16 MB 的**窗口**地址交给它（`wifi_mem_real=1`）更糟：整机静默死机，说明它写超了窗口；
交给它池里的地址（`wifi_mem_real=6`，也就是 11-15 的原始行为）只是静默。

**已经排除的（都做了对照实验）**：回包字节（逐字节相同）；宿主驱动代码（11-10、11-15、HEAD 三个树，
含把 `BUILD/obj` 删干净的全量重编，`hakios-kernel` 15,926,200 B、md5 `b36b6e3b…`，仍然死机）；
loader 二进制（`xnuloader/loader.efi` 被 gitignore，一直是 11-19 05:37 那个；从 `2543d95` 重建 242,176 B 后一样）；
固件镜像（镜像里 `fw/amss.bin` 与 `/opt/hackios/fw-wifi/amss.bin` 逐字节相同、CRC 通过）；
内存落点（全部 < 4 GiB）；stage 号（23/24）；冷启动；电量 100%/25 ℃；userland（用 `JOBS= WIFID=0`
做了 13,295,616 B 的最小镜像，无 launchd，结果相同）；Android 侧 Wi-Fi 开关。
11-15 的完整启动参数不能复现（加 `gpu=1` 后内核在 adreno 里 panic）。

**同一台手机的 Android Wi-Fi 是正常的**（`cmd wifi status`：Wi-Fi 6、5200 MHz、2401 Mbps、RSSI −19、
`wpa_supplicant` 在跑），所以射频硬件、供电、固件镜像都没问题。

结论：卡点在固件处理"主机内存应答"的内部路径上，主机侧已无可调变量；要再往下走需要固件自己的
crash dump（要先在它崩之前用 QMI 的 m3/dump 告诉它地址），那是另一条线。
