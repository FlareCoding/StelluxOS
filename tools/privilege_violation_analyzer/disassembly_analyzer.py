#!/usr/bin/env python3

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from symbol_analyzer import SymbolAnalyzer

class DisassemblyAnalyzer:
    """
    Disassembles code using Capstone for 64-bit x86.
    """

    def __init__(self, elffile, addr_to_sym=None):
        self.elffile = elffile
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.addr_to_sym = addr_to_sym if addr_to_sym else {}

    def disassemble_symbol(self, symbol_dict):
        # We consider .ktext, .text as executable
        exec_sections = {".text", ".ktext"}
        section = symbol_dict.get("section", None)
        if section not in exec_sections:
            return []

        size = symbol_dict["size"]
        if size == 0:
            return []

        vma = symbol_dict["address"]
        code_bytes = self._read_bytes(vma, size)
        if not code_bytes:
            return []

        instructions = []
        for insn in self.md.disasm(code_bytes, vma):
            mnemonic = insn.mnemonic
            op_str   = insn.op_str

            if mnemonic == "call" and op_str.startswith("0x"):
                try:
                    addr_val = int(op_str, 16)
                    if addr_val in self.addr_to_sym:
                        sym = self.addr_to_sym[addr_val]
                        target_name = SymbolAnalyzer.simplify_symbol_name(sym["name"])
                        privileged_state = "privileged" if sym["privileged"] else "unprivileged"
                        op_str = f"0x{addr_val:X} <{target_name}> ({privileged_state})"
                except:
                    pass
            else:
                continue # ignore the non-call instructions

            instructions.append((insn.address, mnemonic, op_str))
        return instructions

    def _read_bytes(self, vma, size):
        if size == 0:
            return b""
        for seg in self.elffile.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                seg_start = seg["p_vaddr"]
                seg_end   = seg_start + seg["p_memsz"]
                if (vma >= seg_start) and ((vma + size) <= seg_end):
                    file_offset = seg["p_offset"] + (vma - seg_start)
                    seg.stream.seek(file_offset)
                    return seg.stream.read(size)
        return b""
