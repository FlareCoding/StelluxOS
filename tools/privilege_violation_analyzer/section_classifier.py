#!/usr/bin/env python3

class SectionClassifier:
    """
    Classifies sections into privileged vs unprivileged based on their names.
    """

    PRIVILEGED_SECTIONS = {".ktext", ".kdata", ".krodata", ".bootstrap"}

    @staticmethod
    def normalize_section_name(sec_name: str) -> str:
        if not sec_name.startswith('.'):
            return sec_name
        parts = sec_name.split('.')
        if len(parts) >= 2:
            return f".{parts[1]}"
        return sec_name

    @classmethod
    def is_section_privileged(cls, raw_name: str) -> bool:
        normalized = cls.normalize_section_name(raw_name)
        return normalized in cls.PRIVILEGED_SECTIONS

    def gather_sections(self, elffile):
        """
        Returns a dict { normalized_sec_name: { 'name':..., 'privileged': bool } }
        but we won't need it heavily, as we do section-based marking in symbol_analyzer.
        """
        results = {}
        for section in elffile.iter_sections():
            raw = section.name
            if not raw:
                continue
            norm = self.normalize_section_name(raw)
            priv = self.is_section_privileged(raw)
            if norm not in results:
                results[norm] = {
                    "name": norm,
                    "privileged": priv
                }
        return results
