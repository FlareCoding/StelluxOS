#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "builtin_commands.h"

// Built-in command implementations

// Echo command - prints its arguments
int cmd_echo(terminal_t* term, int argc, char* argv[]) {
    if (!term) {
        return -1;
    }
    
    // Print all arguments separated by spaces
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            terminal_write_string(term, " ");
        }
        terminal_write_string(term, argv[i]);
    }
    
    // Add newline at the end
    terminal_write_string(term, "\r\n");
    
    return 0;
}

// Clear command - clears the terminal screen
int cmd_clear(terminal_t* term, int argc, char* argv[]) {
    (void)argc; // Unused parameter
    (void)argv; // Unused parameter
    
    if (!term) {
        return -1;
    }
    
    terminal_clear_screen(term);
    return 0;
}

// Getpid command - prints the current process ID
int cmd_getpid(terminal_t* term, int argc, char* argv[]) {
    (void)argc; // Unused parameter
    (void)argv; // Unused parameter
    
    if (!term) {
        return -1;
    }
    
    pid_t pid = getpid();
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d\r\n", (int)pid);
    terminal_write_string(term, pid_str);
    
    return 0;
}

// Help command - shows available built-in commands
int cmd_help(terminal_t* term, int argc, char* argv[]) {
    (void)argc; // Unused parameter
    (void)argv; // Unused parameter
    
    if (!term) {
        return -1;
    }
    
    terminal_write_string(term, "Available built-in commands:\r\n");
    terminal_write_string(term, "============================\r\n");
    
    for (int i = 0; i < builtin_command_count; i++) {
        char help_line[256];
        snprintf(help_line, sizeof(help_line), "  %-10s - %s\r\n", 
                builtin_commands[i].name, builtin_commands[i].description);
        terminal_write_string(term, help_line);
    }
    
    terminal_write_string(term, "\r\n");
    return 0;
}

// Command processing functions

// Split a command line into arguments
void split_command_line(const char* line, char* argv[], int max_args, int* argc) {
    if (!line || !argv || !argc || max_args <= 0) {
        if (argc) *argc = 0;
        return;
    }
    
    *argc = 0;
    
    // Make a copy of the line since we need to modify it
    char* line_copy = malloc(strlen(line) + 1);
    if (!line_copy) {
        if (argc) *argc = 0;
        return;
    }
    strcpy(line_copy, line);
    
    char* p = line_copy;
    
    // Skip leading whitespace
    while (*p && (*p == ' ' || *p == '\t')) {
        p++;
    }
    
    while (*p && *argc < max_args - 1) {
        argv[*argc] = p;
        (*argc)++;
        
        // Find end of argument
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        
        // Null-terminate the argument
        if (*p) {
            *p = '\0';
            p++;
        }
        
        // Skip whitespace
        while (*p && (*p == ' ' || *p == '\t')) {
            p++;
        }
    }
    
    argv[*argc] = NULL; // Null-terminate the argument list
}

// Execute a built-in command
int execute_builtin_command(terminal_t* term, const char* command, int argc, char* argv[]) {
    if (!term || !command) {
        return -1;
    }
    
    // Find the command
    for (int i = 0; i < builtin_command_count; i++) {
        if (strcmp(command, builtin_commands[i].name) == 0) {
            return builtin_commands[i].func(term, argc, argv);
        }
    }
    
    // Command not found
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "Command not found: %s\r\n", command);
    terminal_write_string(term, error_msg);
    return -1;
}

// Process a complete command line
void process_command(terminal_t* term, const char* command_line) {
    if (!term || !command_line) {
        return;
    }
    
    // Skip empty lines
    if (strlen(command_line) == 0) {
        return;
    }
    
    char* argv[32];
    int argc = 0;
    
    // Split the command line into arguments
    split_command_line(command_line, argv, 32, &argc);
    
    if (argc > 0) {
        // Execute the command
        execute_builtin_command(term, argv[0], argc, argv);
        
        // Free the allocated memory
        if (argv[0]) {
            free(argv[0]);
        }
    }
}

// Built-in commands table
const builtin_command_t builtin_commands[] = {
    {"echo", "Print arguments to the terminal", cmd_echo},
    {"clear", "Clear the terminal screen", cmd_clear},
    {"getpid", "Print the current process ID", cmd_getpid},
    {"help", "Show available built-in commands", cmd_help}
};

const int builtin_command_count = sizeof(builtin_commands) / sizeof(builtin_commands[0]);
