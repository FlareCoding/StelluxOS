import * as vscode from 'vscode';
import { PrivilegedRegion, ViewMode } from './types';

let borderDecorationType: vscode.TextEditorDecorationType | undefined;
let tintDecorationType: vscode.TextEditorDecorationType | undefined;
let markerDecorationType: vscode.TextEditorDecorationType | undefined;
let elevatedBorderDecorationType: vscode.TextEditorDecorationType | undefined;
let elevatedTintDecorationType: vscode.TextEditorDecorationType | undefined;

interface DecorationConfig {
    borderColor: string;
    borderWidth: number;
    markerHighlightColor: string;
    tintColor: string;
    elevatedBorderColor: string;
    elevatedTintColor: string;
    showOverviewRuler: boolean;
}

function readConfig(): DecorationConfig {
    const cfg = vscode.workspace.getConfiguration('stellux.dynpriv');
    return {
        borderColor: cfg.get<string>('borderColor', '#b8860b'),
        borderWidth: cfg.get<number>('borderWidth', 2),
        markerHighlightColor: cfg.get<string>('markerHighlightColor', 'rgba(184, 134, 11, 0.08)'),
        tintColor: cfg.get<string>('tintColor', 'rgba(184, 134, 11, 0.04)'),
        elevatedBorderColor: cfg.get<string>('elevatedBorderColor', '#e87f5f'),
        elevatedTintColor: cfg.get<string>('elevatedTintColor', 'rgba(232, 95, 134, 0.06)'),
        showOverviewRuler: cfg.get<boolean>('showOverviewRuler', false),
    };
}

function createDecorationTypes(config: DecorationConfig): void {
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
    tintDecorationType = vscode.window.createTextEditorDecorationType({ isWholeLine: true, backgroundColor: config.tintColor });
    markerDecorationType = vscode.window.createTextEditorDecorationType({ isWholeLine: true, backgroundColor: config.markerHighlightColor });
    elevatedBorderDecorationType = vscode.window.createTextEditorDecorationType({ borderWidth: `0 0 0 ${config.borderWidth}px`, borderStyle: 'dashed', borderColor: config.elevatedBorderColor, isWholeLine: true, rangeBehavior: vscode.DecorationRangeBehavior.ClosedClosed });
    elevatedTintDecorationType = vscode.window.createTextEditorDecorationType({ isWholeLine: true, backgroundColor: config.elevatedTintColor });
}

export function rebuildDecorationTypes(): void { disposeDecorations(); createDecorationTypes(readConfig()); }

export function applyDecorations(editor: vscode.TextEditor, regions: PrivilegedRegion[], mode: ViewMode): void {
    if (!borderDecorationType) { rebuildDecorationTypes(); }
    if (mode === ViewMode.Off) { clearDecorations(editor); return; }
    const borderRanges: vscode.Range[] = [];
    const tintRanges: vscode.Range[] = [];
    const markerRanges: vscode.Range[] = [];
    const elevBorderRanges: vscode.Range[] = [];
    const elevTintRanges: vscode.Range[] = [];
    for (const region of regions) {
        const fullRange = new vscode.Range(region.startLine, 0, region.endLine, Number.MAX_SAFE_INTEGER);
        if (region.kind === 'elevated') {
            elevBorderRanges.push(fullRange);
            if (mode === ViewMode.Enhanced) { elevTintRanges.push(fullRange); }
        } else {
            const markerRange = new vscode.Range(region.markerLine, 0, region.markerLine, Number.MAX_SAFE_INTEGER);
            borderRanges.push(fullRange);
            markerRanges.push(markerRange);
            if (mode === ViewMode.Enhanced) { tintRanges.push(fullRange); }
        }
    }
    editor.setDecorations(borderDecorationType!, borderRanges);
    editor.setDecorations(markerDecorationType!, markerRanges);
    editor.setDecorations(tintDecorationType!, tintRanges);
    editor.setDecorations(elevatedBorderDecorationType!, elevBorderRanges);
    editor.setDecorations(elevatedTintDecorationType!, elevTintRanges);
}

export function clearDecorations(editor: vscode.TextEditor): void {
    if (borderDecorationType) { editor.setDecorations(borderDecorationType, []); }
    if (tintDecorationType) { editor.setDecorations(tintDecorationType, []); }
    if (markerDecorationType) { editor.setDecorations(markerDecorationType, []); }
    if (elevatedBorderDecorationType) { editor.setDecorations(elevatedBorderDecorationType, []); }
    if (elevatedTintDecorationType) { editor.setDecorations(elevatedTintDecorationType, []); }
}

export function disposeDecorations(): void {
    borderDecorationType?.dispose(); tintDecorationType?.dispose(); markerDecorationType?.dispose();
    elevatedBorderDecorationType?.dispose(); elevatedTintDecorationType?.dispose();
    borderDecorationType = undefined; tintDecorationType = undefined; markerDecorationType = undefined;
    elevatedBorderDecorationType = undefined; elevatedTintDecorationType = undefined;
}
