import { PrivilegedKind, PrivilegedRegion } from './types';

const PRIV_MARKER = /__PRIVILEGED_(CODE|DATA|RODATA|BSS)/;

interface MarkerHit {
    kind: PrivilegedKind;
    line: number;
}

function kindFromCapture(capture: string): PrivilegedKind {
    switch (capture) {
        case 'CODE': return 'code';
        case 'DATA': return 'data';
        case 'RODATA': return 'rodata';
        case 'BSS': return 'bss';
        default: return 'code';
    }
}

/**
 * Scan document lines for __PRIVILEGED_* markers and resolve their scope.
 * Returns an array of PrivilegedRegion describing each privileged span.
 */
export function scanDocument(lines: string[]): PrivilegedRegion[] {
    const markers = findMarkers(lines);
    const regions: PrivilegedRegion[] = [];

    for (const marker of markers) {
        const region = resolveScope(lines, marker);
        if (region) {
            regions.push(region);
        }
    }

    return regions;
}

/**
 * Quick check: does raw text contain any __PRIVILEGED_ marker?
 * Used for fast file-tree scanning without full parse.
 */
export function hasPrivilegedMarkers(text: string): boolean {
    return PRIV_MARKER.test(text);
}

/**
 * Count how many __PRIVILEGED_ markers appear in raw text.
 */
export function countPrivilegedMarkers(text: string): number {
    const global = /__PRIVILEGED_(CODE|DATA|RODATA|BSS)/g;
    let count = 0;
    while (global.exec(text)) {
        count++;
    }
    return count;
}

function findMarkers(lines: string[]): MarkerHit[] {
    const hits: MarkerHit[] = [];
    for (let i = 0; i < lines.length; i++) {
        const match = PRIV_MARKER.exec(lines[i]);
        if (match) {
            hits.push({ kind: kindFromCapture(match[1]), line: i });
        }
    }
    return hits;
}

/**
 * Given a marker hit, determine the full range of the privileged region.
 *
 * For CODE markers:
 *   - Find the next '{' (may be on the marker line or a subsequent line).
 *   - If a ';' appears before any '{', this is a declaration-only line (header).
 *   - Otherwise brace-match to the closing '}'.
 *
 * For DATA/RODATA/BSS markers:
 *   - Find the terminating ';' tracking brace depth for struct initializers.
 */
function resolveScope(lines: string[], marker: MarkerHit): PrivilegedRegion | null {
    if (marker.kind === 'code') {
        return resolveCodeScope(lines, marker);
    } else {
        return resolveDataScope(lines, marker);
    }
}

function resolveCodeScope(lines: string[], marker: MarkerHit): PrivilegedRegion | null {
    const startLine = marker.line;

    // Scan forward from marker line to find '{' or ';'
    for (let i = marker.line; i < lines.length; i++) {
        const line = lines[i];
        for (let ch = 0; ch < line.length; ch++) {
            const c = line[ch];
            if (c === ';') {
                // Declaration only (e.g. header prototype), mark just these lines
                return {
                    kind: 'code',
                    markerLine: marker.line,
                    startLine,
                    endLine: i,
                };
            }
            if (c === '{') {
                // Found opening brace -- brace-match to find the closing '}'
                const endLine = findMatchingBrace(lines, i, ch);
                if (endLine === -1) {
                    // Unmatched brace, fall back to just the marker line
                    return {
                        kind: 'code',
                        markerLine: marker.line,
                        startLine,
                        endLine: i,
                    };
                }
                return {
                    kind: 'code',
                    markerLine: marker.line,
                    startLine,
                    endLine,
                };
            }
        }
    }

    // No '{' or ';' found, just mark the marker line
    return {
        kind: 'code',
        markerLine: marker.line,
        startLine,
        endLine: startLine,
    };
}

function resolveDataScope(lines: string[], marker: MarkerHit): PrivilegedRegion | null {
    const startLine = marker.line;
    let braceDepth = 0;

    // Scan forward from marker line, tracking brace depth, looking for final ';'
    for (let i = marker.line; i < lines.length; i++) {
        const line = lines[i];
        for (let ch = 0; ch < line.length; ch++) {
            const c = line[ch];
            if (c === '{') {
                braceDepth++;
            } else if (c === '}') {
                braceDepth--;
            } else if (c === ';' && braceDepth <= 0) {
                return {
                    kind: marker.kind,
                    markerLine: marker.line,
                    startLine,
                    endLine: i,
                };
            }
        }
    }

    // No terminator found, mark just the marker line
    return {
        kind: marker.kind,
        markerLine: marker.line,
        startLine,
        endLine: startLine,
    };
}

/**
 * Starting from the '{' at lines[startLine][startCol], find the line
 * containing the matching closing '}'. Returns -1 if not found.
 */
function findMatchingBrace(lines: string[], startLine: number, startCol: number): number {
    let depth = 0;

    for (let i = startLine; i < lines.length; i++) {
        const line = lines[i];
        const colStart = (i === startLine) ? startCol : 0;

        for (let ch = colStart; ch < line.length; ch++) {
            const c = line[ch];
            if (c === '{') {
                depth++;
            } else if (c === '}') {
                depth--;
                if (depth === 0) {
                    return i;
                }
            }
        }
    }

    return -1;
}
