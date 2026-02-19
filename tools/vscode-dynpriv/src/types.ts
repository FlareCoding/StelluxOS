export type PrivilegedKind = 'code' | 'data' | 'rodata' | 'bss';

export interface PrivilegedRegion {
    kind: PrivilegedKind;
    markerLine: number; // line where __PRIVILEGED_* appears (0-based)
    startLine: number; // first line of decorated range (0-based)
    endLine: number; // last line of decorated range, inclusive (0-based)
}

export enum ViewMode {
    Off = 'off',
    Subtle = 'subtle',
    Enhanced = 'enhanced',
}

export const VIEW_MODE_CYCLE: ViewMode[] = [
    ViewMode.Off,
    ViewMode.Subtle,
    ViewMode.Enhanced,
];
