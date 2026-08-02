# Python 辅助: 符号名 -> QEMU 物理地址 (同 fix_vmlinux_head_entry.sh 公式)
# ---------------------------------------------------------------------------
python
import subprocess
import os.path

_sym_cache = None

def _load_sym_cache():
    """从 nm vmlinux 加载全量符号缓存"""
    global _sym_cache
    if _sym_cache is not None:
        return
    _sym_cache = {}
    if not os.path.isfile("vmlinux"):
        return
    try:
        out = subprocess.check_output(["nm", "vmlinux"], text=True)
        for line in out.split("\n"):
            parts = line.split()
            if len(parts) >= 3:
                _sym_cache[parts[2]] = int(parts[0], 16)
    except Exception:
        pass

def _sym2phys(name):
    """
    内核符号名 -> QEMU 物理地址.
    公式: phys = (vaddr + 0x40200000 - 0x80000000) & 0xffffffff
    """
    _load_sym_cache()
    vaddr = _sym_cache.get(name)
    if vaddr is None:
        return None
    return (vaddr + 0x40200000 - 0x80000000) & 0xffffffff

class _HeadSBP(gdb.Breakpoint):
    """带自动上下文的断点: 命中时打印符号名+pc+lr 然后停止等待单步."""
    def __init__(self, phys_addr, sym_name, desc, banner=False):
        super().__init__(f"*0x{phys_addr:x}", type=gdb.BP_BREAKPOINT)
        self.sym_name = sym_name
        self.desc    = desc
        self.banner  = banner

    def stop(self):
        if self.banner:
            gdb.write("\n" + "=" * 60 + "\n")
            gdb.write(f"BP: {self.sym_name} -- {self.desc}\n")
            gdb.write("=" * 60 + "\n")
        else:
            gdb.write(f"\n--- BP: {self.sym_name} ({self.desc}) ---\n")
        try:
            f  = gdb.selected_frame()
            pc = f.pc()
            lr = f.read_register("lr")
            gdb.write(f"pc={pc:#018x}  lr={lr:#018x}\n")
        except Exception:
            pass
        return True   # 总是停止

def _bp(name, desc, banner=False):
    """通过符号名设断点 (同时支持 global/local 符号)."""
    phys = _sym2phys(name)
    if phys is None:
        gdb.write(f"  !! WARNING: '{name}' not found in vmlinux\n")
        return
    _HeadSBP(phys, name, desc, banner)
    gdb.write(f"  BP: {name} @ 0x{phys:x}  ({desc})\n")

end

# ---------------------------------------------------------------------------
# 守卫: 仅在 Linux 内核构建目录中激活
# ---------------------------------------------------------------------------
python
import os
IN_KERNEL = os.path.isfile("vmlinux") and os.access("fix_vmlinux_head_entry.sh", os.X_OK)
if IN_KERNEL:
    gdb.write("=> Kernel tree detected, loading head.S debug config...\n")
    gdb.execute("set $KERNEL_TREE = 1")
else:
    gdb.execute("set $KERNEL_TREE = 0")
    gdb.write("(skipping -- not in kernel build tree)\n")
end

if $KERNEL_TREE

# =========================================================================
# 1. 基础 GDB 设置
# =========================================================================
set architecture aarch64
set confirm off
set pagination off
set print pretty on
set disassemble-next-line on
set print symbol-loading off

# ---- TUI 样式 ----
set tui border-kind acs
set tui border-mode bold-standout
set tui tab-width 2
set style tui-active-border foreground red
set tui compact-source on

# =========================================================================
# 2. 加载 vmlinux 符号 @ 物理地址 (MMU=OFF 阶段用)
#    fix_vmlinux_head_entry.sh 输出形如:
#    add-symbol-file vmlinux -s .head.text 0x40200000 -s .text 0x40210000 ...
# =========================================================================
shell ./fix_vmlinux_head_entry.sh > /tmp/_gdb_vmlinux_fix
source /tmp/_gdb_vmlinux_fix
shell rm -f /tmp/_gdb_vmlinux_fix

# =========================================================================
# 3. 连接 QEMU GDB stub
# =========================================================================
printf "Connecting to QEMU (localhost:1234)...\n"
target remote :1234

# =========================================================================
# 4. 断点 — 全部由 Python 根据 nm vmlinux 动态计算物理地址
#    内核重编后地址自动更新，无需手动修改
# =========================================================================
printf "Setting head.S breakpoints...\n"

python
# ---- 入口 ----
_bp("_text",                   "Image header: first instruction, MMU=OFF", banner=True)
_bp("primary_entry",           "head.S:85  main entry",                      banner=True)

# ---- MMU 状态 & 参数保存 ----
_bp("record_mmu_state",        "head.S:134 record SCTLR_ELx.M -> x19")
_bp("preserve_boot_args",      "head.S:170 save x0..x3 -> boot_args[]")

# ---- ID map 页表构建 ----
_bp("__pi_create_init_idmap",  "build identity-mapping page tables")
_bp("__pi_map_range",          "single-level page table population")

# ---- EL 初始化 & CPU 配置 ----
_bp("init_kernel_el",          "head.S:270 exception level setup")
_bp("__cpu_setup",             "proc.S: TCR/MAIR CPU init")

# ---- MMU 边界 ----
_bp("__primary_switch",        "head.S:508 last stop before MMU ON",         banner=True)
_bp("__enable_mmu",            "head.S:459 writes SCTLR_EL1 -> MMU ON")

# ---- MMU 开启后 ----
_bp("__primary_switched",      "head.S:220 first code with MMU=ON")
_bp("start_kernel",            "init/main.c",                                banner=True)
end

# =========================================================================
# 5. 自定义命令
# =========================================================================

# --- 帮助 ---
define gdbhelp
    printf "========== head.S Single-Step Debug ==========\n"
    printf "\n"
    printf " STEPPING (核心命令):\n"
    printf "  si              step one instruction (跟进 call)\n"
    printf "  ni              next instruction  (跳过 call)\n"
    printf "  c               continue to next breakpoint\n"
    printf "  fin / finish    run until this function returns\n"
    printf "\n"
    printf " REGISTERS / STATE:\n"
    printf "  hdctx           full x0-x30 + sp/lr/pc + NZCV dump\n"
    printf "  mmustat         MMU/cache state at entry (from x19)\n"
    printf "  bootargs        dump boot_args[0..3]\n"
    printf "  flags           N/Z/C/V condition flags\n"
    printf "  p/x $xN         print single register (GDB builtin)\n"
    printf "\n"
    printf " MEMORY:\n"
    printf "  memdump ADDR [N]  dump N x 64-bit at addr (default 4)\n"
    printf "  memins  ADDR [N]  disassemble N instrs  (default 4)\n"
    printf "\n"
    printf " TUI:\n"
    printf "  my_tui         reload 3-pane layout\n"
    printf "  tui disable    exit TUI mode\n"
    printf "  Ctrl-X A       toggle TUI (GDB builtin)\n"
    printf "\n"
    printf " NAVIGATION / BREAKPOINTS:\n"
    printf "  headsym         show head.S execution order + BP list\n"
    printf "  info b          list all breakpoints (GDB builtin)\n"
    printf "  delete N        remove breakpoint N\n"
    printf "  disable N       temporarily disable breakpoint N\n"
    printf "\n"
    printf " PAGE TABLES:\n"
    printf "  dump_pgtables PGD       dump PGD->PUD->PMD\n"
    printf "  verify_idmap VA PGD     walk tables for one VA\n"
    printf "  idmap_check PGD [VA..]  combined dump + verify\n"
    printf "\n"
    printf " map_range (page table walk loop):\n"
    printf "  map_dump        derive all locals from x0-x7\n"
    printf "=================================================\n"
end
document gdbhelp
    Show all custom commands for head.S debugging
end

# --- 断点流程总览 ---
define headsym
    printf "========== head.S Execution Flow ==========\n"
    printf " Ord  Symbol                  Source\n"
    printf " ---  ----------------------  -----------------------\n"
    printf " [1]  _text                   Image header entry\n"
    printf " [2]  primary_entry           head.S:85\n"
    printf " [3]  record_mmu_state        head.S:134  (MMU -> x19)\n"
    printf " [4]  preserve_boot_args      head.S:170  (save x0..x3)\n"
    printf " [5]  __pi_create_init_idmap  init ID map page tables\n"
    printf "  |   __pi_map_range          populate each level\n"
    printf " [6]  init_kernel_el          head.S:270  (EL2->EL1)\n"
    printf " [7]  __cpu_setup             proc.S: TCR,MAIR init\n"
    printf " [8]  __primary_switch        head.S:508  (pre-MMU)\n"
    printf " [9]  __enable_mmu            head.S:459  ★ MMU ON ★\n"
    printf "[10]  __primary_switched      head.S:220  (VIRTUAL addr)\n"
    printf "[11]  start_kernel            init/main.c (C world)\n"
    printf "\n"
    printf " TIP: 'c' = jump to next BP; 'si' = step 1 instr\n"
    printf "=================================================\n"
end
document headsym
    Show head.S breakpoint order and source locations
end

# --- TUI: regs(上全宽) | src(左)+asm(右) | cmd(底) ---
# 顶层默认 vertical: regs(上) → 水平组(中) → cmd(下)
# {-horizontal src 1 asm 1}: 中间行 src 和 asm 左右并排
define my_tui
    tui new-layout mylayout regs 6 {-horizontal src 1 asm 1} 10 cmd 1
    layout mylayout
    tui reg general
    winheight cmd 3
    refresh
    printf "TUI ready -- type 'c' to hit _text\n"
end
document my_tui
    Layout: regs(top-full) | src(left)+asm(right) | cmd(bottom).
end

# =========================================================================
# 6. 启动提示
# =========================================================================
printf "\n"
printf "============================================================\n"
printf " head.S single-step debug -- READY\n"
printf " Type 'c'  -> run to _text (first instruction)\n"
printf " Type 'si' -> step one instruction\n"
printf "============================================================\n"

end
# <<< end of if $KERNEL_TREE