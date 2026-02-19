import * as vscode from 'vscode';
import { PrivilegedRegion, ViewMode } from './types';

let borderDecorationType: vscode.TextEditorDecorationType | undefined;
let tintDecorationType: vscode.TextEditorDecorationType | undefined;
let markerDecorationType: vscode.TextEditorDecorationType | undefined;

interface DecorationConfig {
    borderColor: string;
    borderWidth: number;
    markerHighlightColor: string;
    tintColor: string;
    showOverviewRuler: boolean;
}

function readConfig(): DecorationConfig {
    const cfg = vscode.workspace.getConfiguration('stellux.dynpriv');
    return {
        borderColor: cfg.get<string>('borderColor', '#b8860b'),
        borderWidth: cfg.get<number>('borderWidth', 2),
        markerHighlightColor: cfg.get<string>('markerHighlightColor', 'rgba(184, 134, 11, 0.08)'),
        tintColor: cfg.get<string>('tintColor', 'rgba(184, 134, 11, 0.04)'),
        showOverviewRuler: cfg.get<boolean>('showOverviewRuler', false),
    };
}

function createDecorationTypes(config: DecorationConfig): void {
    // Left border bar (+ optional overview ruler marks)
    const borderOptions: vscode.DecorationRenderOptions = {
        borderWidth: `0 0 0 ${config.borderWidth}px`,
        borderStyle: 'solid',
        borderColor: config.borderColor,
        isWholeLine: true,
        rangeBehavior: vscode.DecorationRangeBehavior.ClosedClosed,
    };

    if (config.showOverviewRuler) {
        borderOptions.overviewRulerColor = config.borderColor;
        borderOptions.overviewRulerLane = vscode.OverviewRulerLane.Left;
    }

    borderDecorationType = vscode.window.createTextEditorDecorationType(borderOptions);

    // Background tint (Enhanced mode)
    tintDecorationType = vscode.window.createTextEditorDecorationType({
        isWholeLine: true,
        backgroundColor: config.tintColor,
    });

    // Slightly stronger highlight on the __PRIVILEGED_* marker line
    markerDecorationType = vscode.window.createTextEditorDecorationType({
        isWholeLine: true,
        backgroundColor: config.markerHighlightColor,
    });
}

/**
 * Recreate all decoration types from current settings.
 * Must be called before applyDecorations, and again when settings change.
 */
export function rebuildDecorationTypes(): void {
    disposeDecorations();
    createDecorationTypes(readConfig());
}

/**
 * Apply decorations to an editor based on regions and the current view mode.
 */
export function applyDecorations(
    editor: vscode.TextEditor,
    regions: PrivilegedRegion[],
    mode: ViewMode,
): void {
    if (!borderDecorationType) {
        rebuildDecorationTypes();
    }

    if (mode === ViewMode.Off) {
        clearDecorations(editor);
        return;
    }

    const borderRanges: vscode.Range[] = [];
    const tintRanges: vscode.Range[] = [];
    const markerRanges: vscode.Range[] = [];

    for (const region of regions) {
        const fullRange = new vscode.Range(region.startLine, 0, region.endLine, Number.MAX_SAFE_INTEGER);
        const markerRange = new vscode.Range(region.markerLine, 0, region.markerLine, Number.MAX_SAFE_INTEGER);

        borderRanges.push(fullRange);
        markerRanges.push(markerRange);

        if (mode === ViewMode.Enhanced) {
            tintRanges.push(fullRange);
        }
    }

    editor.setDecorations(borderDecorationType!, borderRanges);
    editor.setDecorations(markerDecorationType!, markerRanges);
    editor.setDecorations(tintDecorationType!, tintRanges);
}

/**
 * Remove all decorations from an editor.
 */
export function clearDecorations(editor: vscode.TextEditor): void {
    if (borderDecorationType) {
        editor.setDecorations(borderDecorationType, []);
    }
    if (tintDecorationType) {
        editor.setDecorations(tintDecorationType, []);
    }
    if (markerDecorationType) {
        editor.setDecorations(markerDecorationType, []);
    }
}

/**
 * Dispose all decoration types. Call on extension deactivation.
 */
export function disposeDecorations(): void {
    borderDecorationType?.dispose();
    tintDecorationType?.dispose();
    markerDecorationType?.dispose();
    borderDecorationType = undefined;
    tintDecorationType = undefined;
    markerDecorationType = undefined;
}
