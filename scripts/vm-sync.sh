#!/bin/bash
# ============================================================
#  在 macOS 上开发、在 Linux VM 上构建的最省心流程
#
#  为什么不直接在挂载目录里编译？
#    挂载（multipass 的 9p 或 sshfs）的元数据比 VM 本地盘慢几百倍、
#    大块吞吐只有它的 1/4 ~ 1/20；9p 还不支持 mmap（clangd/LSP 用不了）、
#    sshfs 有约 1 秒属性缓存（会让 make 漏编）。
#    把源码 rsync 进 VM 本地盘再构建，上面这些问题全部消失。
#
#  实测（本项目，同一台 VM）：
#    写入挂载（9p）        22 ~ 27 MB/s
#    写入挂载（sshfs）     110 MB/s
#    scp → VM 本地盘       95 ~ 98 MB/s   ← 而且落在真实文件系统上
#    编译同一个项目        本地盘 1.3s  vs  挂载 2.5s
#
#  用法（**在 Mac 上跑**）：
#    bash scripts/vm-sync.sh              # 同步 + 冒烟构建
#    bash scripts/vm-sync.sh sync         # 只同步
#    bash scripts/vm-sync.sh build        # 在 VM 上编译（增量）
#    bash scripts/vm-sync.sh test         # 在 VM 上跑一遍矩阵压测
#    bash scripts/vm-sync.sh shell        # 进 VM 并 cd 到同步目录
#
#  环境变量：
#    VM   目标主机，默认 ubuntu@192.168.252.3
#    DST  VM 上的工作目录，默认 /home/ubuntu/work/io-study
# ============================================================
set -euo pipefail

VM=${VM:-ubuntu@192.168.252.3}
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DST=${DST:-/home/ubuntu/work/$(basename "$SRC")}

# ---- 安全护栏：--delete 只在"专用工作目录"上生效 -------------------------
# 这个脚本会用 rsync --delete 把 DST 变成 SRC 的镜像（删掉 DST 里多出来的文件）。
# 所以 DST 必须是一个**专门给构建用的目录**，不能是家目录、根目录或挂载点。
case "$DST" in
  /|/home|/home/ubuntu|/root|"") echo "❌ DST 不能是 $DST（危险目标）" >&2; exit 1 ;;
esac

do_sync() {
  echo ">>> 同步 $SRC/  ->  $VM:$DST/"
  ssh "$VM" "mkdir -p '$DST'"
  local t0=$SECONDS
  # 注意：macOS 自带的是 rsync 2.6.9，没有 --info=progress2 等新参数，
  # 所以这里只用最基础的选项，进度自己打印。
  rsync -az --delete \
    --exclude '.git/' \
    --exclude 'bin/' \
    --exclude 'results/' \
    --exclude '__pycache__/' \
    --exclude '*.pyc' \
    --exclude '.cache/' \
    --exclude 'compile_commands.json' \
    --exclude 'third_party/libco/build/' \
    "$SRC/" "$VM:$DST/"
  local files size
  files=$(ssh "$VM" "find '$DST' -type f | wc -l")
  size=$(ssh "$VM" "du -sh '$DST' | cut -f1")
  echo ">>> 完成：$files 个文件 / $size，用时 $((SECONDS - t0)) 秒"
}

do_build() {
  echo ">>> 在 VM 本地盘上编译"
  ssh "$VM" "cd '$DST' && make -j\$(nproc) all examples 2>&1 | grep -E '\[OK\]|error|warning' | tail -20"
}

do_smoke() {
  echo ">>> 冒烟：强制全量构建"
  ssh "$VM" "cd '$DST' && make -B -j\$(nproc) all examples >/tmp/smoke.log 2>&1 && \
             echo \"  构建 OK（\$(grep -cE '\[OK\]' /tmp/smoke.log) 个目标）\" && \
             grep -iE 'error|warning' /tmp/smoke.log | head -3 || true"
}

do_test() {
  echo ">>> 在 VM 上跑网络+磁盘矩阵（quick）"
  ssh "$VM" "cd '$DST' && python3 scripts/bench_matrix.py net --secs 1 --warmup 0 2>&1 | tail -12"
}

case "${1:-all}" in
  sync)  do_sync ;;
  build) do_build ;;
  smoke) do_smoke ;;
  test)  do_test ;;
  shell) ssh -t "$VM" "cd '$DST' && exec bash" ;;
  all)   do_sync && do_smoke && echo ">>> 全部完成" ;;
  *)     echo "用法: $0 [sync|build|smoke|test|shell|all]" >&2; exit 1 ;;
esac
