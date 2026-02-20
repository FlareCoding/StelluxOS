import { PrivilegedKind, PrivilegedRegion } from './types';

const PRIV_MARKER = /__PRIVILEGED_(CODE|DATA|RODATA|BSS)/;
const ELEVATED_MARKER = /\bRUN_ELEVATED\s*\(/;

interface MarkerHit {
    kind: PrivilegedKind;
    line: number;
    col?: number; // column of opening '(' for RUN_ELEVATED
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
 * Scan document lines for __PRIVILEGED_* and RUN_ELEVATED markers and resolve their scope.
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
 * Quick check: does raw text contain any __PRIVILEGED_ or RUN_ELEVATED marker?
 * Used for fast file-tree scanning without full parse.
 */
export function hasPrivilegedMarkers(text: string): boolean {
    return PRIV_MARKER.test(text) || ELEVATED_MARKER.test(text);
}

/**
 * Count how many __PRIVILEGED_ and RUN_ELEVATED markers appear in raw text.
 */
export function countPrivilegedMarkers(text: string): number {
    const privGlobal = /__PRIVILEGED_(CODE|DATA|RODATA|BSS)/g;
    const elevGlobal = /\bRUN_ELEVATED\s*\(/g;
    let count = 0;
    while (privGlobal.exec(text)) { count++; }
    while (elevGlobal.exec(text)) { count++; }
    return count;
}

function findMarkers(lines: string[]): MarkerHit[] {
    const hits: MarkerHit[] = [];
    for (let i = 0; i < lines.length; i++) {
        const privMatch = PRIV_MARKER.exec(lines[i]);
        if (privMatch) {
            hits.push({ kind: kindFromCapture(privMatch[1]), line: i });
        }
        const elevMatch = ELEVATED_MARKER.exec(lines[i]);
        if (elevMatch) {
            const parenCol = lines[i].indexOf('(', elevMatch.index);
            hits.push({ kind: 'elevated', line: i, col: parenCol });
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
    if (marker.kind === 'elevated') {
        return resolveElevatedScope(lines, marker);
    } else if (marker.kind === 'code') {
        return resolveCodeScope(lines, marker);
    } else {
        return resolveDataScope(lines, marker);
    }
}

/**
 * Resolve scope for RUN_ELEVATED(...) by matching the outer parentheses.
 * The region covers just the RUN_ELEVATED(...) statement including the
 * do { ... } while(0) content between the parens.
 */
function resolveElevatedScope(lines: string[], marker: MarkerHit): PrivilegedRegion | null {
    const startLine = marker.line;
    const parenCol = marker.col ?? lines[startLine].indexOf('(', 0);
    if (parenCol === -1) {
        return { kind: 'elevated', markerLine: startLine, startLine, endLine: startLine };
    }

    let depth = 0;
    for (let i = startLine; i < lines.length; i++) {
        const line = lines[i];
        const colStart = (i === startLine) ? parenCol : 0;
        for (let ch = colStart; ch < line.length; ch++) {
            if (line[ch] === '(') { depth++; }
            else if (line[ch] === ')') {
                depth--;
                if (depth === 0) {
                    return { kind: 'elevated', markerLine: startLine, startLine, endLine: i };
                }
            }
        }
    }

    return { kind: 'elevated', markerLine: startLine, startLine, endLine: startLine };
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
