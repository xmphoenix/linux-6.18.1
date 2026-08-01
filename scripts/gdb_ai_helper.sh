#!/bin/bash
# =============================================================================
# gdb_ai_helper.sh - AI 辅助 GDB 调试接口
#
# 用法:
#   1) 从 GDB 交互中 source:
#        (gdb) source /tmp/ai_cmds.gdb
#
#   2) 批量查询模式（非交互）:
#        ./scripts/gdb_ai_helper.sh batch /tmp/ai_batch.gdb
#        结果写入 /tmp/gdb_output.txt
#
#   3) 查询 QEMU 是否在运行:
#        ./scripts/gdb_ai_helper.sh check
# =============================================================================

KERNEL_DIR=/home/ybzhang/kernel/linux-6.18.1

case "$1" in
    batch)
        # 批量执行 GDB 命令，输出到文件
        CMD_FILE="$2"
        OUTPUT_FILE="${3:-/tmp/gdb_output.txt}"

        if [ ! -f "$CMD_FILE" ]; then
            echo "ERROR: command file not found: $CMD_FILE"
            exit 1
        fi

        # 合并 fix_vmlinux + AI 命令
        cat > /tmp/_ai_combined.gdb << 'GDBEOF'
set pagination off
set confirm off
set architecture aarch64
target remote :1234
GDBEOF

        # 添加符号文件
        cd "$KERNEL_DIR" && ./fix_vmlinux_head_entry.sh >> /tmp/_ai_combined.gdb

        # 添加用户的命令
        cat "$CMD_FILE" >> /tmp/_ai_combined.gdb

        # 追加退出
        echo "detach" >> /tmp/_ai_combined.gdb
        echo "quit" >> /tmp/_ai_combined.gdb

        echo "Running GDB batch..."
        gdb-multiarch -batch -x /tmp/_ai_combined.gdb > "$OUTPUT_FILE" 2>&1
        echo "Output saved to: $OUTPUT_FILE"
        head -50 "$OUTPUT_FILE"
        ;;

    check)
        # 检查 QEMU GDB stub 是否在监听
        if ss -tln | grep -q ':1234'; then
            echo "QEMU GDB stub is LISTENING on :1234"
            echo "You can now: ./scripts/gdb_ai_helper.sh batch <cmd_file>"
            echo "  or run: gdb-multiarch  (interactive)"
        else
            echo "QEMU GDB stub is NOT running"
            echo "Start QEMU with: ./launch.sh arm64 debug"
        fi
        ;;

    connect)
        # 启动交互式 GDB（连接已运行的 QEMU）
        cd "$KERNEL_DIR"
        exec gdb-multiarch
        ;;

    *)
        echo "Usage: $0 {batch|check|connect}"
        echo ""
        echo "  batch <cmd.gdb> [out.txt]   - 批量执行 GDB 命令"
        echo "  check                        - 检查 QEMU 是否在监听"
        echo "  connect                      - 启动交互式 GDB（连接已运行 QEMU）"
        echo ""
        echo "Examples:"
        echo "  # 创建命令文件:"
        echo "  echo 'info registers x0 x1 x19' > /tmp/ai_cmds.gdb"
        echo "  echo 'x/4xg 0x40200000'        >> /tmp/ai_cmds.gdb"
        echo "  echo 'bt'                      >> /tmp/ai_cmds.gdb"
        echo ""
        echo "  # 执行:"
        echo "  $0 batch /tmp/ai_cmds.gdb /tmp/gdb_out.txt"
        echo ""
        echo "  # 读取结果:"
        echo "  cat /tmp/gdb_out.txt"
        ;;
esac
