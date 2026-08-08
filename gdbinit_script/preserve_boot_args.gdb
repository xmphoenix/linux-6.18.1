# ============================================================
# GDB script: preserve_boot_args 调试验证命令
# 来源: learningNote/bili/bili_arm64_head.S_preserve_boot_args.html §7.2
#
# 前置条件: ~/.gdbinit 已在 GDB 启动时自动加载，提供了
#           _sym2phys / _bp 等基础设施。~/.gdbinit 已在
#           preserve_boot_args 处设有断点，到达后手动调用
#           以下命令即可。
#
# 加载方式:
#   (gdb) source gdbinit_script/preserve_boot_args.gdb
#
# 典型流程:
#   (gdb) c                          # 跑到 preserve_boot_args
#   --- BP: preserve_boot_args (...) ---
#   (gdb) pbargs                     # 打印入口寄存器
#   (gdb) si 4                       # 单步过 STP
#   (gdb) bootargs $x0               # x0 已是 boot_args 地址
#   (gdb) si                         # cbnz x19, 0f
#   (gdb) mmustat $x0                # 如果走 MMU on 路径
# ============================================================

# ----------------------------------------------------------
# pbargs — 打印 preserve_boot_args 入口寄存器状态
# 对应 §7.2 断点自动打印的内容，改为手动调用
# ----------------------------------------------------------
define pbargs
  printf "=== preserve_boot_args entry ===\n"
  printf "FDT (x0)      = 0x%lx\n", $x0
  printf "x1            = 0x%lx\n", $x1
  printf "x2            = 0x%lx\n", $x2
  printf "x3            = 0x%lx\n", $x3
  printf "MMU state(x19)= %d (%s)\n", $x19, $x19 ? "ON" : "OFF"
end
document pbargs
  Print x0(FDT)/x1/x2/x3/x19(MMU state) at preserve_boot_args entry.
  Usage: pbargs
end

# ----------------------------------------------------------
# bootargs [phys_addr] — 打印 boot_args[0..3]
#
# 用法:
#   (gdb) bootargs                   # 无参: 自动解析 boot_args 物理地址
#   (gdb) bootargs $x0               # STP 之后 x0 指向 boot_args
#   (gdb) bootargs 0x4173a000        # 手动指定地址
# ----------------------------------------------------------
define bootargs
  if $argc == 0
    python _addr = _sym2phys("boot_args"); gdb.execute(f"bootargs 0x{_addr:x}")
  else
    printf "boot_args[0] (FDT) = 0x%llx\n", *(unsigned long long*)$arg0
    printf "boot_args[1]       = 0x%llx\n", *(unsigned long long*)($arg0 + 8)
    printf "boot_args[2]       = 0x%llx\n", *(unsigned long long*)($arg0 + 16)
    printf "boot_args[3]       = 0x%llx\n", *(unsigned long long*)($arg0 + 24)
  end
end
document bootargs
  Dump boot_args[0..3].
  Usage: bootargs [phys_addr]
  No arg: auto-resolve via _sym2phys("boot_args").
  With arg: use given address (e.g. bootargs $x0 after adr_l).
end

# ----------------------------------------------------------
# mmustat [addr] — 打印 mmu_enabled_at_boot
#
# 用法:
#   (gdb) mmustat                     # 无参: 自动解析地址
#   mmu_enabled_at_boot = 0           # Cold boot (MMU off)
#   mmu_enabled_at_boot = 1           # Warm boot / kexec (MMU on)
#   (gdb) mmustat 0x4173a020          # 手动指定地址
# ----------------------------------------------------------
define mmustat
  if $argc == 0
    python _addr = _sym2phys("mmu_enabled_at_boot"); gdb.execute(f"mmustat 0x{_addr:x}")
  else
    printf "mmu_enabled_at_boot = %d\n", *(int*)$arg0
  end
end
document mmustat
  Print mmu_enabled_at_boot value.
  Usage: mmustat [phys_addr]
  No arg: auto-resolve via _sym2phys("mmu_enabled_at_boot").
  With arg: use given address.
end
