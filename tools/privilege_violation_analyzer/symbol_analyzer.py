#!/usr/bin/env python3

from elftools.elf.sections import SymbolTableSection
from cxxfilt import demangle as cxxfilt_demangle
from section_classifier import SectionClassifier

class SymbolAnalyzer:
    """
    Loads symbols, checks if they're in one of the privileged sections => privileged, else unprivileged.
    """

    def __init__(self, elffile, section_dict):
        self.elffile = elffile
        self.section_dict = section_dict
        self.symbols = []
        self.text_symbols = []
        self.addr_to_symbol_map = {}

    def analyze_symbols(self):
        for section in self.elffile.iter_sections():
            if not isinstance(section, SymbolTableSection):
                continue

            for symbol in section.iter_symbols():
                raw_name = symbol.name
                sym_value = symbol.entry["st_value"]
                sym_size  = symbol.entry["st_size"]
                section_index = symbol.entry["st_shndx"]

                if sym_size == 0:
                    continue

                # demangle
                try:
                    demangled = cxxfilt_demangle(raw_name)
                except:
                    demangled = raw_name

                # default
                is_privileged = False
                sec_name = None

                if isinstance(section_index, int):
                    if section_index < self.elffile.num_sections():
                        elf_sec = self.elffile.get_section(section_index)
                        if elf_sec:
                            raw_sec_name = elf_sec.name
                            sec_name = SectionClassifier.normalize_section_name(raw_sec_name)
                            is_privileged = SectionClassifier.is_section_privileged(raw_sec_name)

                sym_dict = {
                    "name": demangled,
                    "address": sym_value,
                    "size": sym_size,
                    "section": sec_name,
                    "privileged": is_privileged
                }
                self.symbols.append(sym_dict)

                if sec_name == ".text" or sec_name == ".ktext":
                    self.text_symbols.append(sym_dict)
                    self.addr_to_symbol_map[sym_value] = sym_dict

    def find_symbol_by_name(self, name):
        for sym in self.symbols:
            if name in sym.get('name'):
                return sym
        return None

    def get_symbols(self):
        return self.symbols
    
    def get_text_symbols(self):
        return self.text_symbols

    def get_symbol_from_addr(self, addr):
        return self.addr_to_symbol_map[addr] if addr in self.addr_to_symbol_map else None
    
    def get_addr_to_sym_map(self):
        return self.addr_to_symbol_map

    @staticmethod
    def simplify_symbol_name(name):
        return name.split('(')[0]
