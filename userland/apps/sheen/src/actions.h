#ifndef SHEEN_ACTIONS_H
#define SHEEN_ACTIONS_H

/* Every command Sheen offers, listed once with its label and keys. The shortcuts are built from `COMMANDS`, and
 * Scintilla keeps only movement, selection, deletion and clipboard keys, so this is the one keymap. */
enum class command {
    NEW_FILE,
    OPEN_FILE,
    SAVE,
    SAVE_AS,
    SAVE_ALL,
    CLOSE_TAB,
    QUIT,
    UNDO,
    REDO,
    CUT,
    COPY,
    PASTE,
    SELECT_ALL,
    WORD_WRAP,
    ZOOM_IN,
    ZOOM_OUT,
    ZOOM_RESET,
    RECENT_TAB,
    NEXT_TAB,
    PREVIOUS_TAB,
    MOVE_TAB_LEFT,
    MOVE_TAB_RIGHT,
    TAB_1,
    TAB_2,
    TAB_3,
    TAB_4,
    TAB_5,
    TAB_6,
    TAB_7,
    TAB_8,
    LAST_TAB,
};

enum class command_kind {
    ACTION,
    TOGGLE, // a command with an on and off state that belongs to the current tab
};

struct command_entry {
    command id;
    const char* label;
    const char* shortcut;  // in QKeySequence's portable text, empty for none
    const char* alternate; // a second key for the same command, empty for none
    command_kind kind;
};

inline constexpr command_entry COMMANDS[] = {
    { command::NEW_FILE, "New File", "Ctrl+N", "", command_kind::ACTION },
    { command::OPEN_FILE, "Open...", "Ctrl+O", "", command_kind::ACTION },
    { command::SAVE, "Save", "Ctrl+S", "", command_kind::ACTION },
    { command::SAVE_AS, "Save As...", "Ctrl+Shift+S", "", command_kind::ACTION },
    { command::SAVE_ALL, "Save All", "Ctrl+Alt+S", "", command_kind::ACTION },
    { command::CLOSE_TAB, "Close Tab", "Ctrl+W", "Ctrl+F4", command_kind::ACTION },
    { command::QUIT, "Quit", "Ctrl+Q", "", command_kind::ACTION },
    { command::UNDO, "Undo", "Ctrl+Z", "", command_kind::ACTION },
    { command::REDO, "Redo", "Ctrl+Shift+Z", "Ctrl+Y", command_kind::ACTION },
    { command::CUT, "Cut", "Ctrl+X", "", command_kind::ACTION },
    { command::COPY, "Copy", "Ctrl+C", "", command_kind::ACTION },
    { command::PASTE, "Paste", "Ctrl+V", "", command_kind::ACTION },
    { command::SELECT_ALL, "Select All", "Ctrl+A", "", command_kind::ACTION },
    { command::WORD_WRAP, "Word Wrap", "Alt+Z", "", command_kind::TOGGLE },
    { command::ZOOM_IN, "Zoom In", "Ctrl+=", "Ctrl++", command_kind::ACTION },
    { command::ZOOM_OUT, "Zoom Out", "Ctrl+-", "", command_kind::ACTION },
    { command::ZOOM_RESET, "Reset Zoom", "Ctrl+0", "", command_kind::ACTION },
    { command::RECENT_TAB, "Recent Tab", "Ctrl+Tab", "", command_kind::ACTION },
    { command::NEXT_TAB, "Next Tab", "Ctrl+PgDown", "", command_kind::ACTION },
    { command::PREVIOUS_TAB, "Previous Tab", "Ctrl+PgUp", "", command_kind::ACTION },
    { command::MOVE_TAB_LEFT, "Move Tab Left", "Ctrl+Shift+PgUp", "", command_kind::ACTION },
    { command::MOVE_TAB_RIGHT, "Move Tab Right", "Ctrl+Shift+PgDown", "", command_kind::ACTION },
    { command::TAB_1, "Tab 1", "Alt+1", "", command_kind::ACTION },
    { command::TAB_2, "Tab 2", "Alt+2", "", command_kind::ACTION },
    { command::TAB_3, "Tab 3", "Alt+3", "", command_kind::ACTION },
    { command::TAB_4, "Tab 4", "Alt+4", "", command_kind::ACTION },
    { command::TAB_5, "Tab 5", "Alt+5", "", command_kind::ACTION },
    { command::TAB_6, "Tab 6", "Alt+6", "", command_kind::ACTION },
    { command::TAB_7, "Tab 7", "Alt+7", "", command_kind::ACTION },
    { command::TAB_8, "Tab 8", "Alt+8", "", command_kind::ACTION },
    { command::LAST_TAB, "Last Tab", "Alt+9", "", command_kind::ACTION },
};

#endif
