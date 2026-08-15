# dump_memblock: 打印 memblock.memory / memblock.reserved 的所有 region
# 用法: (gdb) dump_memblock
# 依赖: vmlinux 符号已加载 (memblock 在 .data 段, 虚拟地址 0xffff8000821b4800)
python
class DumpMemblock(gdb.Command):
    """打印 memblock.memory 与 memblock.reserved 的所有 region (base/size/flags/nid)"""
    def __init__(self):
        super().__init__("dump_memblock", gdb.COMMAND_USER)

    def _dump(self, t, tname):
        cnt = int(t["cnt"])
        total = int(t["total_size"])
        regions = t["regions"]
        gdb.write("[%s] cnt=%d  total_size=0x%x\n" % (tname, cnt, total))
        for i in range(cnt):
            r = regions[i]
            base = int(r["base"])
            size = int(r["size"])
            flags = int(r["flags"])
            nid = int(r["nid"])
            gdb.write("  [%2d] base=0x%016x  end=0x%016x  size=0x%x  flags=0x%x  nid=%d\n"
                      % (i, base, base + size - 1, size, flags, nid))

    def invoke(self, arg, from_tty):
        try:
            mb = gdb.parse_and_eval("memblock")
        except Exception as e:
            gdb.write("!! cannot resolve 'memblock': %s\n" % e)
            return
        gdb.write("memblock @ 0x%x\n" % int(gdb.parse_and_eval("&memblock")))
        self._dump(mb["memory"], "memory")
        self._dump(mb["reserved"], "reserved")

DumpMemblock()
end
