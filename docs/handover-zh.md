# 交接文档：HackiOS on mars（给接手的模型/人）

最后更新：2026-11-12。写这份文档的会话把中文界面和壁纸做完了，同时把触摸弄坏了，
没修好就停手了。下面写的都是可核实的现状，不是叙述。

## 0. 一句话

iOS 16.5 的 Darwin 内核（XNU 8796.121.2）已经能在小米 11 Pro（代号 `mars`）上启动，
跑出中文界面和壁纸；**触摸是坏的**，这是唯一挡在“能用”前面的事。仓库已推到 GitHub。

## 1. 环境

- 仓库在 WSL：`/opt/hackios/hackios`（Ubuntu，root 用户）。Windows 侧**能读不能写**
  `\\wsl$\Ubuntu\opt\hackios\hackios\...`（写入报 `GetFileSecurityW EIO (Win32 1)`）——
  要改文件就 `wsl cp` 到 Windows 侧改完再 `cp` 回去。
- 手机：小米 11 Pro（`M2102K1AC`，SM8350），USB 接在 Windows 上；`fastboot`/`adb`
  是 Windows 的二进制，WSL 里 `lsusb` 看不到设备，这是正常的。
- **每个终端都要先 `export DEVICE=mars`**。不设的话 `xl` 默认按 `redwood` 走，
  会去编 u-boot，然后失败：`mkubootimg.py: no u-boot.bin for redwood` /
  `xl: image build failed (u-boot)`。
- 不要把带引号/正则的命令塞进 `wsl.exe bash -lc "..."`，会被转义吃掉。写成 `.sh`
  文件放 Windows 侧，再用 `wsl bash /mnt/d/.../x.sh` 跑。

## 2. 仓库与远端

- HEAD `dea680a`，工作区干净；`main` 已同步到 `git@github.com:coldreset/hackios-mars.git`。
- 最近三个提交（都在 `main` 上）：
  - `9c83878` boot: the menu's first item runs the payload, and answers the host
  - `e31ad40` ui: the interface speaks Chinese, and the home screen has its wallpaper
  - `dea680a` docs: what is known about a screen that draws and does not listen
- **磁盘上的镜像与源码不一致**：`userland/root.apfs` 是 05:56 的（含已被还原的背光钩子），
  `userland/system.apfs` 是 04:41 的。开始任何实验前先按 §6 重建，否则你测的不是源码。

## 3. 跑一轮实验的固定流程（约 7 分钟）

```sh
cd /opt/hackios/hackios && export DEVICE=mars
./xl state                                   # loader / android / fastboot / absent
./xl reload                                  # ~2 分钟
./xl ramdisk userland/system.apfs            # 2386 MB，72–73 s，32 MB/s
XL_SECS=200 XL_CMDLINE="debug=0x8 cpus=8 rd=md0 touchscreen=1" ./xl run
```

- 系统镜像（2.5 GB）每次都要重传，因为内核的根就是它（`xl ramdisk` 把它作为
  resident image 交给 loader）。没有它内核会立刻 panic：`Process 1 exec of
  /sbin/launchd failed, errno 2`。
- **一个 loader 会话里只能跑一次内核**。第二次会被 loader 拒绝：
  `a second one has died at once every time it was tried`。所以每轮都要 `./xl reload`。
- 内核在跑的时候 `./xl state` 回答 `absent`（不是 `loader`），`xl screenshot` 也进不去
  （它走 `enter()`，三条探测路都失败）。**桌面上的画面只能靠人眼看**——这是本项目最贵
  的一条限制。
- 日志：`xl run` 结束会自己抓一份到 `logs/<时分秒>-run.log`，`./xl log` 也能抓。抓回来
  **可能是 0 字节**（11 日 05:16–06:04 连续三轮如此，06:14 那轮又正常），失败就重抓，
  不要当成“没有日志”。
- `XL_SECS` 是内核跑多久后返回；期间手机不可操作（除了音量/电源键在开机菜单里）。

## 4. 现在什么能用、什么坏、什么没做

能用（都已提交、已推到 main）：

- 开机菜单：音量键上下、电源键确认；`fastboot oem menu up|down|select` 也能驱动；
  菜单第一项自己启动 HackiOS（不需要主机再发 `oem run`）。
- 中文界面：`AppleLanguages = (zh-Hans, en)` + 把 `.loctable` 的 `zh_CN` 内容抄进
  `en`/`Base`（Darwin 在这里只按 bundle 的 development region 取语言，钩 NSBundle 无效）。
- 壁纸：用 Apple 自带的 launch angel `com.apple.PosterBoard`（`MachServices =
  com.apple.postboardservices.services`），`CollectionsPoster` 会真的起来。
- 字体：把 MiSans 的 `wght=500` 实例化后作为 TrueType 字面拼进 `PingFang.ttc`，
  补上 SpringBoard 要的 `.PingFangSC-Medium`，否则每个汉字都是空心方块。
- 开机进度环 = 真实传输进度（`splash_ring_show`）。

坏的：**触摸**（屏在画，滑不动，电源键也没反应）。见 §5。

没做：

- App 切换器卡片还是白框。根因已知：`/var/mobile/Library/SplashBoard/Snapshots` 从来
  没有被写过，卡片里那个 846×1827 的纯色图就是占位符。
- 菜单“用电源键确认启动”这条路径没在真机验证过（只验证了 `oem menu select`）。

## 5. 触摸回归（最重要，先修这个）

### 证据表

| 轮次（日志） | 现象 | 日志特征 |
|---|---|---|
| 02:37 / 03:17 | 能操作 | 没有 `displayState: off`；`hakios: input: touch screen service` 有 |
| 03:48 / 05:06 | 滑不动 | `displayState: off` 两条；`touch screen service` **没有**（输入线程没建出服务） |
| 06:04（带背光钩子） | 仍滑不动 | 没有 `displayState: off`；`touch screen service` **有**；没有 `create touch stream` |

结论：至少两种坏法——(a) 注入的输入线程没建出虚拟触摸服务；(b) 服务建出来了仍收不到触摸。
两者都可能同源于“开机变慢后，backboardd 扫数字转换器时我们的服务还没就位”。

### 已排除

- 两个探针 job（`com.hakios.languagetest.plist`、`com.hakios.fonttest.plist`）：摘掉后照坏。
- SpringBoard 的背光状态：钩 `-[SBBacklightController screenIsOn]`→YES 之后，`displayState`
  那条消失，但触摸照坏。**这个理论已作废**，`docs/touch-regression.md` 里那段结论要改。

### 下一个实验（只改一处，7 分钟）

`userland/src/inject.c:930`：

```c
    sleep(3);   /* let backboardd get as far as starting its event server */
```

改成 `sleep(1);`（让虚拟触摸服务早 2 秒出现，抢在 backboardd 扫数字转换器之前），
然后：`cd userland && GL=1 ./build.sh`（只重建启动卷，约 1 分钟）→ 按 §3 跑一轮 →
**让人看屏幕能不能滑**。日志只作旁证（`touch screen service`、`create touch stream`、
`displayState`）。

如果失败，两条退路（按顺序）：

1. 反方向：打开 `brightness_fixes()`（`userland/src/inject.c:569`，它装的是
   `wait_for_als_arrival`，`:560`；`:1226` 的注释说当初打开后 backboardd 反而收不到触摸，
   先读懂当年为什么关）。
2. 确认元凶：退回 `9c83878`（无中文、无壁纸，但触控好），重建 system.apfs（约 10 分钟）
   + 一轮，看触摸是否回来。回来就说明中文那批改动里的某处在拖慢/干扰输入路径。

## 6. 构建

```sh
cd /opt/hackios/hackios/userland
GL=1 ./build.sh                                   # 只重建启动卷 root.apfs，约 1 分钟
SYSTEM=1 GL=1 CJK_FONT=/tmp/sysa/system/fonts/MiSansVF.ttf ./build.sh   # 系统卷+启动卷，8–10 分钟
```

- 不设 `CJK_FONT` 时 build.sh 会警告
  `PingFang has no Medium face: every Chinese word will be a hollow box (set CJK_FONT)`。
- `SYSTEM=1` 强制重建 `system.apfs`；否则只有 `root.apfs` 会更新。
- 字体源文件在 `/tmp/sysa/system/fonts/MiSansVF.ttf`（从 Android ROM 里抽出来的，
  `/tmp` 会清空，需要时按 §7 重新抽）。

## 7. 工具手册

- 看镜像内容：`python3 tools/apfs/apfs.py {info|ls|tree|cat|stat|census} IMAGE [PATH] [-v VOLUME]`。
  启动卷 `userland/root.apfs`（19.7 MB）装 `/lib`（`libhakios*.dylib`）、`/LaunchDaemons`、
  `/fw`、`/bin`、`/private`；系统卷 `userland/system.apfs`（2.5 GB）装 `/redwood`
  （真正的 Darwin 根）、`/Applications`、`/System`。
- 中文字体：`python3 tools/cjkface.py <PingFang.ttc> <MiSansVF.ttf>`；
  `--check` 只检查。它把 TrueType 字面追加进 TTC，不动 CFF（改 CFF 会把字体编成 7 个
  字形的残废，试过两次都失败）。
- 中文资源：`python3 userland/mklocalizations.py sysroot`（建 `zh-Hans.lproj`/`zh_CN.lproj`
  标记目录，并把 `.loctable` 的 `zh_CN` 抄进 `en`/`Base`）。
- QEMU：`./qemu/run-darwin.sh 60`（同一份镜像、`-serial stdio`、日志在 `/tmp/qemu-*.log`）。
  **看不到画面**（screendump 只有苹果 logo），但日志干净、启动只要 ~15 s，适合先验证崩溃。
- Android ROM 里抽字体：`7z x /opt/hackios/rom/super.img system_a.img` →
  `fsck.erofs --extract=/tmp/sysa /tmp/lp/system_a.img`（erofs-utils 已装）。

## 8. 代码锚点（行号会漂，按函数名 grep）

| 位置 | 是什么 |
|---|---|
| `xnuloader/loader.c` `keys_service` / `spmi_read` / `menu_select` / `start_payload` | 开机菜单、SPMI 按键、菜单启动系统 |
| `xnuloader/loader.c` `splash_ring_show` / `splash_present` | 进度环、帧缓冲回写（**不回写就花屏**，见 `docs/boot-menu.md`） |
| `userland/src/inject.c:930` | `sleep(3)`，虚拟触摸服务的时序 |
| `userland/src/inject.c:560/569/1226` | `wait_for_als_arrival` / `brightness_fixes` / 为什么关掉 |
| `userland/src/inject_sb.c` `switcher_wallpaper_overlay` | 切换器卡片；里面已有打印卡片图层树的探针（`hakios: card %u: ...`） |
| `userland/build.sh` | 打包：拷贝 poster angel、`fontservicesd`、`mklocalizations`、`cjkface` |
| `userland/src/language_test.m` | 语言/字体探针（以 mobile 身份跑，输出到 `/dev/console`） |

## 9. 别踩的坑（都是花过钱换来的）

1. 忘 `export DEVICE=mars`：白跑 2 分钟。
2. 一个 loader 会话跑第二次内核：必然失败。
3. 等待手机时要接受 `state` 的四个值：`loader` / `android` / `fastboot` / `absent`。
4. `xl reload` 在 run 进行中会拒绝：`a run is in progress: only state, screenshot, log,
   stop and reboot now`。
5. 日志抓回 0 字节：重抓，不要当没日志。
6. `pkill -f qemu-system-aarch64` 会把自己那条 shell 也杀掉（命令行自带匹配），
   用 `pkill -f 'qemu-sy[s]tem-aarch64'`。
7. `git commit -F 文件`，别用 heredoc 穿过 `wsl.exe bash -lc`（消息会被截断）。
8. 一轮实验 = 7 分钟 + 一次 2.4 GB 上传。任何“试试看”先算这个成本。
9. 截图**不是**证据：早先 `xl screenshot` 一直显示完美画面，而手机屏上是花屏——原因是
   帧缓冲没回写（软件缓存）。同理，日志里的“看起来正常”也要用真机观感复核。

## 10. 接手先做这 5 件事

1. `git log -3`、`git status`：确认在 `dea680a` 且干净。
2. 按 §6 重建两份镜像，确认编译输出没有 `error:`。
3. `./xl state` 确认手机在 `android` 或 `loader`。
4. 做 §5 的那一个实验，**让人看屏幕**（这是唯一可靠的判据）。
5. 不管结果如何，先把结论写回 `docs/touch-regression.md`——现在那一版里“背光”的结论
   已被推翻，必须改。
