set disassembly-flavor intel
tui new-layout mylayout regs 1 {src 1 asm 1} 2 cmd 1 status 0
layout mylayout
winh cmd 18
focus cmd
set print pretty on
set radix 16