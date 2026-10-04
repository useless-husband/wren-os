#!/bin/bash
# ============================================================
#  開機看看：把自己寫的作業系統 wren-os 開機到命令列（shell）
#
#  這個檔案在 Finder 裡雙擊就會打開「終端機」來執行。
#  它會做三件事：
#    1. 檢查需要的工具有沒有裝（沒裝會告訴你怎麼裝）。
#    2. 編譯核心（build/Image）、所有使用者程式，並做出一顆
#       虛擬硬碟（build/fs.img），第一次大約 20 秒。
#    3. 開機：如果這台 Mac 有 LeapVM（你自己寫的虛擬機），
#       就用 LeapVM；沒有的話改用 QEMU。
#
#  開機後會看到 "$ "，可以打這些指令試試看：
#    ls /bin                 列出所有程式
#    cat /motd.txt | wc      管線：把一個程式的輸出接給另一個
#    ps                      看現在有哪些行程、在哪顆 CPU 上
#    usertests               跑核心的完整測試（40 多項）
#    stress 10               4 顆 CPU 一起壓力測試 10 秒
#    poweroff                關機
#
#  怎麼離開：打 poweroff 再按 Enter；或先按 Ctrl-A 放開，再按 x。
#
#  需要先裝好：
#    Xcode Command Line Tools（提供 make）：xcode-select --install
#    llvm、lld、qemu（brew install llvm lld qemu）
#  （蘋果內建的 clang 不能產生 ARM 開發板用的 ELF 執行檔，所以要用 Homebrew 的 llvm。）
# ============================================================

# 先切換到這個檔案所在的資料夾。資料夾名稱有空白和中文，
# 所以 "$(dirname "$0")" 一定要用雙引號包起來。
cd "$(dirname "$0")" || exit 1

pause_and_exit() {
  read -r -p "按 Enter 關閉視窗..."
  exit "$1"
}

# 檢查需要的工具有沒有裝
if ! command -v make >/dev/null 2>&1 || ! command -v cc >/dev/null 2>&1; then
  echo "找不到 make 或 cc。請在終端機執行：xcode-select --install"
  pause_and_exit 1
fi
if [ ! -x /opt/homebrew/opt/llvm/bin/clang ] && ! clang -print-targets 2>/dev/null | grep -q aarch64; then
  echo "找不到能產生 ARM64 ELF 的 clang。請在終端機執行：brew install llvm lld"
  pause_and_exit 1
fi
if [ ! -x /opt/homebrew/opt/lld/bin/ld.lld ] && ! command -v ld.lld >/dev/null 2>&1; then
  echo "找不到 ld.lld（連結器）。請在終端機執行：brew install lld"
  pause_and_exit 1
fi

# 找 LeapVM：可以用環境變數 LEAPVM 指定位置，否則看桌面上的預設資料夾
LEAPVM_BIN="${LEAPVM:-$HOME/Desktop/Claude專案/Mac自製Linux虛擬機 LeapVM/leapvm}"
if [ ! -x "$LEAPVM_BIN" ] && ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
  echo "沒有 LeapVM，也找不到 QEMU。請在終端機執行：brew install qemu"
  pause_and_exit 1
fi

echo "== 1/2 編譯核心、使用者程式和虛擬硬碟（只有第一次或改過檔案才會花時間）..."
if ! make -s all; then
  echo "編譯失敗，上面的訊息會說明原因。"
  pause_and_exit 1
fi

echo
echo "== 2/2 開機"
echo "   離開方法：打 poweroff 再按 Enter；或先按 Ctrl-A 放開，再按 x"
echo "   （虛擬硬碟是 build/fs.img，你建立的檔案下次開機還會在。"
echo "     想還原成全新的硬碟：刪掉 build/fs.img 再雙擊一次。）"
echo
if [ -x "$LEAPVM_BIN" ]; then
  echo "   使用 LeapVM（4 顆 CPU、256 MB 記憶體）"
  echo
  "$LEAPVM_BIN" -k build/Image -c 4 -m 256 --no-net --disk build/fs.img
else
  echo "   這台 Mac 沒有 LeapVM，改用 QEMU（4 顆 CPU、256 MB 記憶體）"
  echo
  qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a72 -smp 4 -m 256M -nographic \
    -kernel build/Image -drive file=build/fs.img,if=none,format=raw,id=d0 \
    -device virtio-blk-device,drive=d0
fi
status=$?
echo
echo "虛擬機已經關閉（結束代碼 ${status}）。"
pause_and_exit 0
