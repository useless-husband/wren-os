# wren-os

一個從零開始、用 C 和組合語言寫的 64 位元 ARM（AArch64）多核心類 Unix 作業系統核心。
同一個核心 `Image` 可以在兩台不同的虛擬機上開機：QEMU 的 `virt` 機器，和 LeapVM
（我自己寫的 Apple Silicon 虛擬機）；所有裝置都是從 device tree 查出來的。
它有支援管線、重新導向和背景工作的 shell，最多可用 8 顆 CPU，
檔案存在有預寫日誌（write-ahead log）的硬碟上，斷電後檔案系統仍然一致（用 600 次模擬斷電測過）。
它的 C 函式庫還有一個保守式垃圾回收器和一個記憶體外洩偵測器，會掃描程式的暫存器、堆疊和全域變數。

這是模仿 MIT（6.1810，xv6）、哈佛（CS 161，Chickadee）、耶魯（CPSC 422，mCertiKOS）
作業系統課程的**學習用重做**，不是新點子，也和這些課程沒有關係。程式碼都是自己寫的，
設計借用了這些系統的知名做法，出處列在[相關專案](#相關專案)。

[English README](README.md) · [課程式期末報告](docs/report.md) · [設計說明](docs/DESIGN.md) ·
[垃圾回收器設計（英文）](docs/GC.md) · [給初學者的導讀](docs/導讀.zh-TW.md)

```
wren-os: booting on "linux,dummy-virt", image at 0x40200000, 4 cpu(s) in the device tree
memory: 253 MiB free of 256 MiB, kernel 372 KiB
virtio-blk: 0xa003e00, legacy transport, 65536 KiB, flush yes
smp: 4 cpu(s) online
fs: 16384 blocks, 1024 inodes, log 65 blocks at 2
init: starting /bin/sh
$ cat /motd.txt | grep -n wren
1:Welcome to wren-os, a small multicore Unix-like kernel for AArch64.
2:Try: ls /bin, cat /motd.txt | wc, grep -n wren /motd.txt, ps, usertests.
$ echo hello > /hi.txt; cat /hi.txt
hello
$ sleep 2000 &
[1] 9
$ ps
  PID  PPID STATE     CPU    MEM(KiB)  TICKS NAME
    1     0 sleeping    0          24      0 init
    2     1 sleeping    1          32      0 sh
    9     2 sleeping    0          24      0 sleep
   10     2 running     0          24      0 ps
$ usertests -q | grep -v OK
usertests: 4 cpu(s), 64644 free pages
pid 109 (usertests): segmentation fault at 0x33000 (write), pc 0x18ef4: killed
pid 128 (usertests): segmentation fault at 0x33010 (exec), pc 0x33010: killed
...                                  （故意製造的錯誤：測試要確認核心有抓到）
usertests: 38 passed, 0 failed in 2443 ms
ALL TESTS PASSED
[1] Done  sleep 2000 &
$ poweroff
wren-os: power off
```

（在 QEMU 上用 4 顆 CPU 實際跑出來的畫面，只省略了幾行錯誤訊息。）

## 做了什麼

| 部分 | 內容 |
|---|---|
| 開機 | arm64 Linux `Image` 檔頭；如果從 EL2 進來就降到 EL1；MMU 關閉時用組合語言建臨時頁表；核心放在高半部位址；自己寫的 FDT 解析器讀 device tree；用 break-before-make 切換到正式頁表 |
| 記憶體 | 有參考計數的 buddy 實體頁分配器；4 層頁表、4 KiB 頁；每個行程一個帶 ASID 標籤的位址空間；copy-on-write `fork`；延遲配置的 heap（`sbrk`）和堆疊；使用者和核心都做到 W^X；核心 heap（`kmalloc`） |
| 例外 | 例外向量表；`svc` 系統呼叫；page fault（延遲配置、COW、segfault）；GICv3 分配器與重分配器；每顆 CPU 的虛擬計時器；處理器間中斷 |
| 多核心 | 用 PSCI `CPU_ON` 啟動其他 CPU；有死結偵測的 ticket spinlock；sleep lock；每顆 CPU 一個排程執行緒的搶占式排程器；浮點/SIMD 暫存器跟著行程切換 |
| 行程 | `fork exec exit waitpid kill getpid getppid sbrk sleep uptime`、pipe、檔案描述符、`dup`/`dup2`、`lseek`、`ps` |
| 儲存 | 支援兩種 virtio-mmio 介面的 virtio-blk 驅動（QEMU 預設的 legacy，和 LeapVM 唯一的 modern）；buffer cache；有直接、單層間接、雙層間接區塊和目錄的 inode 檔案系統；用 CRC-32C 保護確認紀錄的預寫日誌；主機端 `mkfs` 和 `fsck` |
| 使用者程式 | 小型 libc；支援 `|`、`<` `>` `>>`、`&`、`;` 的 `sh`；`ls cat echo grep wc mkdir rm ln kill ps sleep poweroff`；測試程式 `usertests`、`stress`、`fswork`、`bench` |
| 記憶體工具 | 保守式 mark-sweep 垃圾回收器（`gc_malloc`）：按大小分類的頁、標記點陣圖、認得指向物件中間的指標、標記堆疊滿了也不會漏；`malloc` 的外洩偵測器（`leakcheck 程式` 或呼叫 `leak_check()`），列出沒人指到的區塊和配置它的位置；示範程式 `gcdemo`、`leakdemo`，測試 `gctest`，效能 `gcbench` |

核心約 5,700 行 C 和組合語言，使用者程式 4,200 行，主機工具 500 行，測試 2,400 行。

## 運作方式

![架構圖](docs/architecture.svg)

- **一個 Image，兩台機器。** QEMU 把核心載入到 `0x40200000`、序列埠在 `0x09000000`；
  LeapVM 載入到 `0x40000000`、序列埠在 `0x0c000000`。核心連結在固定的虛擬位址
  `0xffffffffc0000000`，不管被放在哪裡都把自己對應到那裡；記憶體、CPU、GIC、計時器中斷、
  序列埠和 virtio 插槽全部從虛擬機放在 `x0` 的 device tree 讀出來。
- **系統呼叫**是 `svc #0`，編號放在 `x8`。核心從不直接使用使用者給的指標：`copyin`/`copyout`
  用軟體走一遍行程的頁表，順便處理延遲配置和 copy-on-write；錯的指標只會得到 `EFAULT`，
  不會讓核心當機。
- **排程**是一條共用隊伍的輪流制，每顆 CPU 每秒 100 次計時器中斷來搶占。一把鎖（`proc_lock`）
  保護所有行程狀態和隊伍，切換時交給下一個執行者；閒著的 CPU 在 `WFI` 睡覺，
  有工作時用處理器間中斷叫醒。
- **檔案系統日誌**：每個會改硬碟的系統呼叫是一筆交易（寫入會切成 32 KiB 一筆）。
  交易的區塊先寫進日誌，再寫一個帶 CRC-32C 檢查碼（涵蓋檔頭和資料）的檔頭來確認，
  最後才寫回原位。開機掛載時，確認過的交易會重做，檢查碼不對的會丟掉。
  會改硬碟的系統呼叫要等它的交易確認後才回傳。

細節：[docs/DESIGN.md](docs/DESIGN.md)（決策和取捨）、[docs/report.md](docs/report.md)
（開機流程、記憶體配置、例外路徑、鎖、日誌、結果）。

## 垃圾回收器和外洩偵測器

```
$ gcdemo 400
round   50:   7 collections, heap   61 pages (peak   80), live 48 KiB, 1 MiB allocated so far
...
round  400:  61 collections, heap   80 pages (peak   81), live 64 KiB, 15 MiB allocated so far
gcdemo: 400 rounds, 15 MiB allocated, peak heap 324 KiB, 61 collections, pause mean 79 us max 558 us
GCDEMO OK (peak 80 pages after round 100, 81 at the end)
$ leakcheck leakdemo
leakdemo: planted 7 leaked blocks, 352 bytes
leak: 32 bytes at 0x161b0, allocated from pc 0x13eac
leak: 48 bytes at 0x161e0, allocated from pc 0x13ef8
...
leakcheck: 7 leaked blocks, 352 bytes; 6 blocks, 304 bytes still reachable
$ leakcheck leakdemo clean
leakdemo: no leaks planted
leakcheck: no leaks; 6 blocks, 304 bytes still reachable
```

用 `gc_malloc` 拿的記憶體永遠不必 `free`。根（root）是：callee-saved 暫存器（用幾行組合語言存到堆疊上）、
從目前的 `sp` 到 crt0 記下的堆疊頂端、以及連結器符號標出的 `.data`/`.bss`。這些地方和所有被找到的物件裡，
每個對齊的字只要指進某個活著的物件（指到中間也算），那個物件就留下。物件放在 4 GiB 以上、延遲配置的
256 MiB 區域，所以任何 32 位元的整數都不可能被誤認成指標。外洩偵測器對 `malloc` 的 heap 跑同一套標記；
`leakcheck` 設定一個 exec 之後還在的 `personality()` 旗標，這是唯一的核心修改（27 行）。在建樹的
效能測試（`make gcbench`，1 顆 vCPU，Hypervisor.framework）裡，`gc_malloc` 每個節點 12-14 ns，
`malloc` + `free` 是 9 ns；每 MiB 活資料暫停約 140 us。設計、測試、數字和限制：[docs/GC.md](docs/GC.md)。

## 驗證

除了註明的以外，下面全部由 `make test` 執行（CI 每次 push 都會跑）。

| 測試 | 內容 | 結果（本機） |
|---|---|---|
| 主機單元測試（`make unit`） | FDT 解析器：和 LeapVM 完全相同的樹、QEMU 自己產生的 DTB、20,000 個被破壞的檔案；buddy 分配器 40,000 次隨機操作的模型檢查；ELF 檢查：每條規則一個案例、50,000 個亂改檔頭和所有真實程式；printf 與 CRC-32C；垃圾回收器核心對照精確的可達性模型做模型檢查（3 個種子 × 3 種設定 × 20,000 次操作，含只有 4 格的標記堆疊）。用 UBSan 編譯（Linux 上加 ASan） | 5 個程式，429,763 項檢查，0 失敗 |
| 開機組合 | 1、4、8 顆 CPU；legacy 和 modern virtio；從 EL2 進入（`virtualization=on`） | 5/5 |
| Shell | 4 個行程的管線、重新導向、背景工作和完成通知、`kill`、`ps`、`ln`、`rm` | 通過 |
| `usertests`，1 和 4 顆 CPU（8 顆手動跑過） | 40 項在 wren-os 裡跑的測試：fork/wait/孤兒行程、行程表用光、kill、搶占、exec 錯誤、`sbrk`、延遲配置、記憶體不足後恢復、COW 隔離和共享、堆疊成長和溢位、空指標/改程式碼/執行 heap/碰核心位址、傳壞指標給系統呼叫、記憶體外洩、切換後浮點暫存器正確、用到多顆 CPU、雙層間接範圍的大檔案、空洞、目錄、連結、刪除開啟中的檔案、多個行程同時寫檔、fd 上限、pipe | 兩種都 40/40 |
| `fsck` 和日誌重做 | 新映像檔乾淨；抓得到外洩的區塊、使用中卻標成空的區塊、錯的連結數、被兩個檔案共用的區塊、指到資料區外的指標、指向空 inode 的目錄項；重做已確認的日誌、忽略未確認的；核心本身也會重做手寫的已確認交易、丟掉檢查碼錯的 | 10/10 |
| `gctest`，1 和 4 顆 CPU | 13 項用真實根的回收器測試：多次回收後活物件完好、垃圾被回收、300 輪內 heap 不再長大、只存在 x19-x28/d8、堆疊或全域變數裡的指標、指到中間和剛好超過結尾的指標、循環、fork、固定種子的隨機圖、大物件 | 兩種都 13/13 |
| `gcdemo`、外洩偵測 | 一個和四個同時回收的行程在 4 顆 CPU 上 heap 都有上限；`leakcheck leakdemo` 正好列出故意放的 7 個區塊，每個都從 ELF 符號表對回製造它的函式，修好的版本什麼都不報；`leakcheck sh` 之後旗標會被繼承 | 2/2 |
| 多核心壓力測試 | 4 顆 CPU 上 12 個行程同時 fork、exec、pipe、寫檔，每個位元組都檢查；之後硬碟要通過 `fsck` | CI 跑 20 秒；60 秒版本：56,096 次經過檢查的操作、1,095,310 次 context switch、0 頁外洩；另在 LeapVM 上用 8 顆 CPU、24 個行程跑過 |
| 斷電一致性 | 見下方 | 600/600 一致 |
| LeapVM（只在 macOS，CI 會跳過並說明原因） | 同一個 Image 在 LeapVM 上用 4 顆 CPU 開機，跑 `usertests -q`、`stress`、`gctest` 和 `leakcheck leakdemo`；再在 LeapVM 上製造斷電、在 LeapVM 上恢復、用 `fsck` 檢查 | 2/2 |
| 突變測試（`make mutants`） | 11 個故意放的 bug，每個都要讓某個測試失敗 | 11/11 抓到 |

**斷電一致性**（`make crash`，[tests/crash.py](tests/crash.py)）。工作程式 `fswork` 會建立、附加、
覆寫、截斷、連結、刪除檔案，建立和刪除目錄，每個動作前印 `OP k ...`、做完印 `OK k`。
每一輪用全新的硬碟開機、開始工作，然後用三種方式之一「拔插頭」：核心在第 *N* 次寫硬碟前關機
（*N* 在整段工作中隨機）；同樣但第 *N* 次只寫進 8 個磁區中的 1 到 7 個；或用 SIGKILL 在隨機時間
砍掉 QEMU。之後主機上的 `fsck` 自己重做日誌並檢查所有規則；再開機讓核心自己恢復；再跑一次
`fsck`；兩種恢復結果必須完全相同，而且必須等於「所有印了 OK 的動作，加上進行中那個動作的
某個前段」。亂數種子固定並印出來，任何一輪都可以用 `tests/crash.py --seeds N` 重跑。
兩批各 300 輪（種子 1-300 和 1001-1300）：**600/600 一致**（注入 299、寫一半 141、SIGKILL 160；
其中 274 輪開機時有已確認的交易需要重做）。

**突變測試**（[docs/mutants.md](docs/mutants.md)）：拿掉 `fork` 的 TLB 清除、記憶體分配器的鎖、
日誌的確認紀錄、日誌重做、COW 參考計數、浮點暫存器保存、間接區塊的釋放、pipe 的喚醒、
回收器保存暫存器時漏掉兩個、標記堆疊溢位後不重掃、外洩偵測器不往下追，每一個都會讓指定的測試失敗。

## 效能

用 `make bench`（`ACCEL=tcg|hvf`，或 `HV=leapvm`）在 Apple M5（10 核心、16 GB）、macOS 27 上量測，
虛擬機 1 顆 CPU；量測時這台機器同時在跑其他工作（load average 約 8），數字會有一些雜訊。
虛擬機內的時間用它自己的計數器（`CNTVCT_EL0`）；開機時間是從啟動虛擬機程式到第一個 shell 提示字元
的實際時間。

| 項目 | QEMU，TCG（模擬） | QEMU，HVF | LeapVM |
|---|---:|---:|---:|
| 開機到 shell 提示字元 | 69 ms | 69 ms | 41 ms |
| `getpid` 系統呼叫 | 2,243 ns | 66 ns | 69 ns |
| context switch（pipe 來回 / 2） | 26.6 us | 355 ns | 346 ns |
| `fork` + `exit` + `wait` | 84 us | 5.3 us | 5.7 us |
| 有 4 MiB 已使用 heap 的 `fork`（COW） | 194 us | 17.6 us | 15.8 us |
| `fork` + `exec` + `wait` | 379 us | 12.9 us | 11.1 us |
| 延遲配置 page fault | 3.7 us | 468 ns | 450 ns |
| copy-on-write page fault | 7.3 us | 858 ns | 840 ns |
| 循序寫 8 MiB，每次 32 KiB（每次都確保寫進硬碟） | 10.5 MiB/s | 23.9 MiB/s | 10.8 MiB/s |
| 循序讀 8 MiB | 52.8 MiB/s | 111 MiB/s | 250 MiB/s |
| 建立 + 寫 100 B + 關閉，再刪除 | 4.5 ms | 1.6 ms | 2.4 ms |

QEMU-HVF 和 LeapVM 都透過 Apple 的 Hypervisor.framework 讓虛擬機直接在真實 CPU 上跑，
所以 CPU 相關的數字一致；硬碟數字不同，是因為兩個虛擬機實作虛擬硬碟和 flush 的方式不同。
寫 8 MiB 共發出 6,152 次硬碟寫入，資料只有 2,048 塊：每塊先寫日誌再寫原位，
加上每筆交易兩次檔頭寫入（共 257 筆交易）。方法和原始輸出：[docs/benchmarks.md](docs/benchmarks.md)。

## 編譯和執行

需要：`make`、主機的 C 編譯器、有 AArch64 目標的 LLVM `clang` + `lld`、QEMU
（`qemu-system-aarch64`）、系統測試需要 Python 3 和 `pytest`。macOS：`brew install llvm lld qemu`
（蘋果內建的 clang 不能連結 ELF）。Ubuntu：`apt install clang lld llvm qemu-system-arm`。

```sh
make                 # 核心 Image、使用者程式、硬碟映像檔、mkfs/fsck
make qemu            # 開機到 shell（CPUS=4、VIRTIO=legacy|modern、ACCEL=tcg|hvf）；離開：Ctrl-A 再按 x
make leapvm          # 在 LeapVM 上開機（macOS；LEAPVM=leapvm 的路徑）
python3 -m venv .venv && .venv/bin/pip install -r requirements-dev.txt
make test            # 單元測試 + 所有系統測試（約 1 分鐘）
make crash           # 300 次固定種子的斷電測試（CRASH_RUNS=...）
make stress          # 4 顆 CPU 壓力測試 60 秒，之後跑 fsck
make mutants         # 突變測試
make bench           # 效能量測
make gcbench         # gc_malloc 和 malloc/free 比較、回收暫停時間（一樣可加 ACCEL=/HV=）
make lint            # 嚴格警告和 clang 靜態分析
```

在 macOS 上也可以直接雙擊 **`開機看看.command`**：它會檢查工具、編譯全部、開機到 shell；
有裝 LeapVM 就用 LeapVM，沒有就用 QEMU。

## 限制

- 沒有 signal：`kill` 就是結束行程。沒有 thread、`mmap`、socket、使用者與權限、`rename`、
  符號連結、時間戳記。
- 所有排程狀態用一把粗的鎖（`proc_lock`）和一條共用隊伍：好推理，4 到 8 顆 CPU 夠用，再多就不行。
- 所有裝置中斷都送到 CPU 0。只支援 virtio-blk（沒有網路、沒有螢幕）。
- 忽略 4 GiB 以上的記憶體；用 8 位元 ASID（最多 255 個同時存在的位址空間，比 64 個行程的上限還多）。
- 核心 heap 的 slab 頁面不會還給頁分配器。
- 沒有 `fsync`：每個修改在系統呼叫回傳時就已經寫進硬碟，代價是寫入速度（見效能表）。
- 開啟中被刪除的檔案如果遇到斷電，inode 會留著沒人用（`fsck` 會報告為 orphan，目前沒有回收）。
  斷電測試沒有測這種情況。
- 斷電測試假設硬碟依序完成寫入、只有最後一次可能寫一半。日誌的檢查碼設計上也能應付寫入順序被打亂，
  但沒有測試會打亂順序。
- macOS 27 上 AddressSanitizer 的執行環境連空程式都會卡住，所以本機單元測試只用 UBSan；
  Linux 上的 CI 兩個都用。
- 垃圾回收器不做壓縮（compaction），每次都停下程式標記整個 heap（沒有並行、增量或分代回收）。
  因為是保守式，可能被某個剛好像指標的字留住垃圾，也看不到用運算藏起來、沒對齊、或只存在 `malloc`
  記憶體裡的指標。詳見 [docs/GC.md](docs/GC.md#9-limits)。
- 只在模擬和虛擬化的硬體上測過，沒有在實體開發板上跑過。

## 相關專案

- **xv6**（[x86](https://github.com/mit-pdos/xv6-public)）和 **xv6-riscv**
  （[原始碼](https://github.com/mit-pdos/xv6-riscv)，MIT 6.1810 使用）：這裡很多結構的範本：
  每顆 CPU 的排程執行緒、切換時持有鎖、用 channel 的 sleep/wakeup、建在 buffer cache 上的
  inode 檔案系統、group commit 的 redo log、`usertests`。不同之處：wren-os 是 AArch64、
  從 device tree 找裝置、核心放在自己的半邊位址空間（TTBR1）而使用者空間帶 ASID、
  有 copy-on-write fork、延遲配置、buddy 分配器、ticket lock、有檢查碼的確認紀錄、
  系統呼叫回傳前就確認寫入、支援兩種 virtio-mmio，並有斷電注入測試。
- **Chickadee**（[CS 161](https://read.seas.harvard.edu/cs161/)，哈佛）：x86-64 教學核心，
  支援多處理器和核心任務暫停；多 CPU 測試的靈感來源。
- **mCertiKOS**（耶魯 CPSC 422，[課程材料](https://flint.cs.yale.edu/cs422/)）：從經過形式驗證的
  CertiKOS 衍生的分層教學核心。wren-os 沒有任何形式驗證。
- 我看過的 AArch64 教學核心：[ChCore](https://github.com/SJTU-IPADS/OS-Course-Lab)（上海交大 IPADS，
  微核心）、[xv6-aarch64](https://github.com/k-mrm/xv6-aarch64) 和
  [xv6-armv8](https://github.com/hakula139/xv6-armv8)（xv6 移植到 QEMU virt / Raspberry Pi）、
  [TOS-arm](https://github.com/SOARingLab/TOS-arm) 和 [rpi-os](https://github.com/Hongqin-Li/rpi-os)
  （復旦大學課程用、從 xv6 改的核心）、[xv6-multiarch](https://github.com/aryx/xv6-multiarch)
  （很多 xv6 移植放在同一個 repo，包含 arm64），以及
  [raspberry-pi-os](https://github.com/s-matyukevich/raspberry-pi-os) 教學。它們大多是把 xv6
  移植到 AArch64 或是課程骨架；這個是獨立實作。據我所知，「一個 Image 跑兩種虛擬機」加上
  「對日誌做斷電注入測試」在這些專案裡不常見，但用到的技術本身都是標準做法。
- **Boehm-Demers-Weiser 回收器**（[bdwgc](https://github.com/ivmai/bdwgc)）：垃圾回收器的範本
  （保守式掃描、按大小分類的頁加旁邊的點陣圖、認得中間指標、標記堆疊溢位的補救）。
  **Valgrind memcheck** 和 **LeakSanitizer**：外洩偵測「結束時做可達性掃描」的範本，它們報得多很多
  （間接外洩、完整的呼叫堆疊）。**史丹佛 CS140E/CS240LX**：期末專題包含在學生自己的作業系統上做
  Boehm 式回收器和外洩偵測器，這裡就是在 wren-os 上重做這件事。

## 目錄

```
kernel/          核心（boot.S、entry.S、switch.S 和 C 檔；見 docs/report.md 第 2 節）
include/wren/    和使用者程式、主機工具共用的介面（系統呼叫編號、硬碟格式）
lib/             核心、使用者程式、工具共用的字串、printf、CRC-32C
user/            libc（user/lib：malloc + 外洩偵測、gc.c 回收器）、init、sh、工具程式、
                 usertests、stress、fswork、bench、gcdemo、gctest、gcbench、leakdemo、leakcheck
tools/           主機端 mkfs 和 fsck
tests/           Python 測試工具（透過序列埠操作 QEMU/LeapVM）、系統測試、斷電和突變測試；
                 tests/unit/ 是主機端 C 單元測試
docs/            報告、設計說明、垃圾回收器設計、效能、突變測試結果、初學者導讀
```

## 授權

MIT，見 [LICENSE](LICENSE)。
