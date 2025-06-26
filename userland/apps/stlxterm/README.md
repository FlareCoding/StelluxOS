# Stellux Terminal Emulator (stlxterm)

A modern terminal emulator for StelluxOS with a modular, scalable architecture designed for robust terminal functionality.

## Overview

The `stlxterm` application provides a graphical terminal emulator that can run command-line applications and display their output. It's built with a modular architecture that separates concerns into distinct components:

- **Terminal Core**: Handles display, cursor management, and character grid
- **ANSI Parser**: Processes ANSI escape sequences for colors, cursor movement, etc.
- **Process Manager**: Manages child processes and their I/O

## Architecture

### Core Components

1. **Terminal (`terminal.h/c`)**
   - Character grid management (120x40 max)
   - Cursor positioning and blinking
   - Color and attribute handling
   - Scrolling and screen clearing
   - Event handling and rendering

2. **ANSI Parser (`ansi_parser.h/c`)**
   - ANSI escape sequence parsing
   - Color code conversion
   - Cursor movement commands
   - Graphics mode handling

3. **Process Manager (`process_manager.h/c`)**
   - Child process creation and management
   - Pipe-based I/O redirection
   - Process status monitoring
   - Input/output buffering

### File Structure

```
stlxterm/
├── include/
│   ├── terminal.h          # Core terminal interface
│   ├── ansi_parser.h       # ANSI escape sequence handling
│   └── process_manager.h   # Process management interface
├── src/
│   ├── main.c             # Application entry point
│   ├── terminal.c         # Terminal core implementation
│   ├── ansi_parser.c      # ANSI parser implementation
│   └── process_manager.c  # Process manager implementation
├── build/                 # Build artifacts
├── Makefile              # Build configuration
└── README.md             # This file
```

## Features

### Current Implementation
- ✅ Basic terminal display with character grid
- ✅ Cursor positioning and blinking
- ✅ Keyboard input handling
- ✅ Character writing and string output
- ✅ Basic scrolling
- ✅ Window management with graphics library
- ✅ Modular architecture foundation

### Planned Features
- 🔄 Full ANSI escape sequence support
- 🔄 Process spawning and management
- 🔄 Color and attribute support
- 🔄 Text selection and copying
- 🔄 Terminal resizing
- 🔄 Multiple terminal tabs
- 🔄 Configuration options
- 🔄 Scrollback buffer
- 🔄 Unicode support

## Building

The terminal emulator is built as part of the StelluxOS userland applications:

```bash
# Build all applications including stlxterm
make -C userland/apps

# Build only stlxterm
make -C userland/apps/stlxterm

# Clean stlxterm build artifacts
make -C userland/apps/stlxterm clean
```

## Usage

The terminal emulator creates a graphical window with a dark theme and displays a welcome message. Currently, it supports basic keyboard input and character display.

### Key Features
- **Character Input**: Type printable characters to see them displayed
- **Enter Key**: Creates new lines
- **Backspace**: Deletes characters and moves cursor back
- **Tab**: Moves cursor to next tab stop
- **Auto-scroll**: Automatically scrolls when content exceeds terminal height

## Development Status

This is a foundational implementation that provides the basic structure for a full-featured terminal emulator. The modular design allows for incremental development of features:

1. **Phase 1** (Current): Basic display and input ✅
2. **Phase 2**: ANSI escape sequence support
3. **Phase 3**: Process management and execution
4. **Phase 4**: Advanced features (selection, tabs, etc.)

## Technical Details

### Terminal Grid
- Maximum size: 120 columns × 40 rows
- Default size: 80 columns × 24 rows
- Character cell: 8×16 pixels (approximate)
- Supports individual cell attributes (color, bold, etc.)

### Color Support
- 8-color ANSI palette (black, red, green, yellow, blue, magenta, cyan, white)
- Bright variants for foreground colors
- Customizable default colors
- Per-cell foreground and background colors

### Event Handling
- Keyboard events (key press/release)
- Mouse events (movement, buttons, scroll)
- Window events (resize, close)
- Custom event callback system

## Future Enhancements

1. **Unicode Support**: Full UTF-8 character handling
2. **Terminal Profiles**: Configurable color schemes and fonts
3. **Session Management**: Save/restore terminal sessions
4. **Plugin System**: Extensible functionality
5. **Performance Optimization**: Efficient rendering and updates
6. **Accessibility**: Screen reader support and keyboard navigation

## Contributing

When adding new features to the terminal emulator:

1. Follow the modular architecture
2. Add appropriate header declarations
3. Implement stub functions for future features
4. Update this README with new capabilities
5. Test thoroughly with various input scenarios

The terminal emulator is designed to be a robust, feature-rich component of the StelluxOS desktop environment.
