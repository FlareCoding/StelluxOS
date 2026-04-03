import * as vscode from 'vscode';
import { ViewMode, VIEW_MODE_CYCLE, PrivilegedRegion } from './types';

const COMMAND_ID = 'stellux.dynpriv.toggle';

export class StatusBarManager {
    private readonly _item: vscode.StatusBarItem;
    private _mode: ViewMode;

    constructor(initialMode: ViewMode) {
        this._mode = initialMode;
        this._item = vscode.window.createStatusBarItem(
            vscode.StatusBarAlignment.Right,
            50,
        );
        this._item.command = COMMAND_ID;
        this.updateDisplay([], 0);
        this._item.show();
    }

    get mode(): ViewMode {
        return this._mode;
    }

    /**
     * Cycle to the next view mode: Off -> Subtle -> Enhanced -> Off
     * Returns the new mode.
     */
    cycleMode(): ViewMode {
        const currentIndex = VIEW_MODE_CYCLE.indexOf(this._mode);
        const nextIndex = (currentIndex + 1) % VIEW_MODE_CYCLE.length;
        this._mode = VIEW_MODE_CYCLE[nextIndex];
        return this._mode;
    }

    setMode(mode: ViewMode): void {
        this._mode = mode;
    }

    /**
     * Update the status bar display with current file stats.
     */
    updateDisplay(regions: PrivilegedRegion[], totalLines: number): void {
        if (this._mode === ViewMode.Off) {
            this._item.text = '$(shield) DynPriv: Off';
            this._item.tooltip = 'Click to enable Dynamic Privilege View';
            return;
        }

        if (regions.length === 0) {
            this._item.text = '$(shield) No priv regions';
            this._item.tooltip = `Mode: ${this._mode} | Click to cycle`;
            return;
        }

        // Count total privileged lines
        let privLines = 0;
        for (const r of regions) {
            privLines += (r.endLine - r.startLine + 1);
        }

        this._item.text = `$(shield) ${regions.length} priv | ${privLines}/${totalLines} lines`;
        this._item.tooltip = `${regions.length} privileged region${regions.length === 1 ? '' : 's'}, ${privLines} lines\nMode: ${this._mode}\nClick to cycle`;
    }

    dispose(): void {
        this._item.dispose();
    }
}
