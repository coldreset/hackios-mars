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
