#ifndef STLXTERM_BUILTIN_COMMANDS_H
#define STLXTERM_BUILTIN_COMMANDS_H

#include <stdint.h>
#include <stdbool.h>
#include "terminal.h"

// Built-in command function type
typedef int (*builtin_command_func_t)(terminal_t* term, int argc, char* argv[]);

// Built-in command structure
typedef struct {
    const char* name;
    const char* description;
    builtin_command_func_t func;
} builtin_command_t;

// Built-in command functions
int cmd_echo(terminal_t* term, int argc, char* argv[]);
int cmd_clear(terminal_t* term, int argc, char* argv[]);
int cmd_getpid(terminal_t* term, int argc, char* argv[]);
int cmd_help(terminal_t* term, int argc, char* argv[]);

// Command processing functions
void process_command(terminal_t* term, const char* command_line);
int execute_builtin_command(terminal_t* term, const char* command, int argc, char* argv[]);
void split_command_line(const char* line, char* argv[], int max_args, int* argc);

// Available built-in commands
extern const builtin_command_t builtin_commands[];
extern const int builtin_command_count;

#endif // STLXTERM_BUILTIN_COMMANDS_H
