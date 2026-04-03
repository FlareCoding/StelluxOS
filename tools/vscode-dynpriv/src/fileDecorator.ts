import * as vscode from 'vscode';
import { hasPrivilegedMarkers, countPrivilegedMarkers } from './scanner';
import { ViewMode } from './types';

const KERNEL_FILE_GLOB = '**/kernel/**/*.{h,cpp,S}';
const PRIV_MARKER_RE = /__PRIVILEGED_(CODE|DATA|RODATA|BSS)/;

/**
 * Provides file tree badges for files containing privileged code.
 * Files with at least one __PRIVILEGED_* marker get an orange "P" badge.
 */
export class PrivilegeFileDecorator implements vscode.FileDecorationProvider {
    private readonly _onDidChangeFileDecorations = new vscode.EventEmitter<vscode.Uri | vscode.Uri[] | undefined>();
    readonly onDidChangeFileDecorations = this._onDidChangeFileDecorations.event;

    private readonly _fileCache = new Map<string, number>();
    private _mode: ViewMode = ViewMode.Subtle;
    private _scanPromise: Promise<void> | undefined;

    constructor() {
        this._scanPromise = this.scanWorkspace();
    }

    setMode(mode: ViewMode): void {
        this._mode = mode;
        this._onDidChangeFileDecorations.fire(undefined);
    }

    async provideFileDecoration(uri: vscode.Uri): Promise<vscode.FileDecoration | undefined> {
        if (this._mode === ViewMode.Off) {
            return undefined;
        }

        // Wait for initial scan to complete
        if (this._scanPromise) {
            await this._scanPromise;
        }

        const count = this._fileCache.get(uri.fsPath);
        if (!count || count === 0) {
            return undefined;
        }

        return {
            badge: 'P',
            tooltip: `${count} privileged region${count === 1 ? '' : 's'}`,
            color: new vscode.ThemeColor('dynpriv.badgeColor'),
            propagate: false,
        };
    }

    /**
     * Re-scan a single file (call on file save).
     */
    async rescanFile(uri: vscode.Uri): Promise<void> {
        if (!this.isKernelFile(uri)) {
            return;
        }

        try {
            const doc = await vscode.workspace.openTextDocument(uri);
            const text = doc.getText();
            const count = countPrivilegedMarkers(text);
            const prev = this._fileCache.get(uri.fsPath) ?? 0;

            if (count !== prev) {
                this._fileCache.set(uri.fsPath, count);
                this._onDidChangeFileDecorations.fire(uri);
            }
        } catch {
            // File may have been deleted or inaccessible
        }
    }

    private async scanWorkspace(): Promise<void> {
        try {
            const files = await vscode.workspace.findFiles(KERNEL_FILE_GLOB);

            for (const file of files) {
                try {
                    const doc = await vscode.workspace.openTextDocument(file);
                    const text = doc.getText();
                    if (hasPrivilegedMarkers(text)) {
                        this._fileCache.set(file.fsPath, countPrivilegedMarkers(text));
                    }
                } catch {
                    // Skip files that can't be opened
                }
            }
        } catch {
            // Workspace may not be available
        }

        this._scanPromise = undefined;
        this._onDidChangeFileDecorations.fire(undefined);
    }

    private isKernelFile(uri: vscode.Uri): boolean {
        const path = uri.fsPath;
        return path.includes('/kernel/') &&
            (path.endsWith('.h') || path.endsWith('.cpp') || path.endsWith('.S'));
    }

    dispose(): void {
        this._onDidChangeFileDecorations.dispose();
        this._fileCache.clear();
    }
}
