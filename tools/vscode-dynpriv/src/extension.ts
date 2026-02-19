import * as vscode from 'vscode';
import { ViewMode } from './types';
import { scanDocument } from './scanner';
import { applyDecorations, clearDecorations, disposeDecorations, rebuildDecorationTypes } from './decorator';
import { PrivilegeFileDecorator } from './fileDecorator';
import { StatusBarManager } from './statusBar';

const STATE_KEY = 'stellux.dynpriv.viewMode';
const DEBOUNCE_MS = 300;

let statusBar: StatusBarManager;
let fileDecorator: PrivilegeFileDecorator;
let debounceTimer: ReturnType<typeof setTimeout> | undefined;

export function activate(context: vscode.ExtensionContext): void {
    // Determine initial mode from persisted state or config default
    const persisted = context.workspaceState.get<string>(STATE_KEY);
    const configDefault = vscode.workspace.getConfiguration('stellux.dynpriv').get<string>('defaultMode', 'subtle');
    const initialMode = parseMode(persisted ?? configDefault);

    // Status bar
    statusBar = new StatusBarManager(initialMode);
    context.subscriptions.push(statusBar);

    // File tree decorator
    fileDecorator = new PrivilegeFileDecorator();
    fileDecorator.setMode(initialMode);
    context.subscriptions.push(
        vscode.window.registerFileDecorationProvider(fileDecorator),
    );
    context.subscriptions.push(fileDecorator);

    // Toggle command
    context.subscriptions.push(
        vscode.commands.registerCommand('stellux.dynpriv.toggle', () => {
            const newMode = statusBar.cycleMode();
            fileDecorator.setMode(newMode);
            context.workspaceState.update(STATE_KEY, newMode);
            updateActiveEditor();
        }),
    );

    // React to editor changes
    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor(() => {
            updateActiveEditor();
        }),
    );

    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument((event) => {
            const editor = vscode.window.activeTextEditor;
            if (editor && event.document === editor.document) {
                debouncedUpdate();
            }
        }),
    );

    // Re-scan file tree on save
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument((doc) => {
            fileDecorator.rescanFile(doc.uri);
        }),
    );

    // Rebuild decorations when settings change
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration('stellux.dynpriv')) {
                rebuildDecorationTypes();
                updateActiveEditor();
            }
        }),
    );

    // Decorate the initially active editor
    updateActiveEditor();
}

export function deactivate(): void {
    if (debounceTimer) {
        clearTimeout(debounceTimer);
    }
    disposeDecorations();
}

function updateActiveEditor(): void {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        statusBar.updateDisplay([], 0);
        return;
    }

    const doc = editor.document;
    const langId = doc.languageId;

    // Only process C/C++ files
    if (langId !== 'c' && langId !== 'cpp') {
        clearDecorations(editor);
        statusBar.updateDisplay([], 0);
        return;
    }

    const lines: string[] = [];
    for (let i = 0; i < doc.lineCount; i++) {
        lines.push(doc.lineAt(i).text);
    }

    const regions = scanDocument(lines);
    const mode = statusBar.mode;

    applyDecorations(editor, regions, mode);
    statusBar.updateDisplay(regions, doc.lineCount);
}

function debouncedUpdate(): void {
    if (debounceTimer) {
        clearTimeout(debounceTimer);
    }
    debounceTimer = setTimeout(() => {
        updateActiveEditor();
        debounceTimer = undefined;
    }, DEBOUNCE_MS);
}

function parseMode(value: string): ViewMode {
    switch (value) {
        case 'off': return ViewMode.Off;
        case 'subtle': return ViewMode.Subtle;
        case 'enhanced': return ViewMode.Enhanced;
        default: return ViewMode.Subtle;
    }
}
