#ifndef STLXDM_SHELL_PROGRAMS_HPP
#define STLXDM_SHELL_PROGRAMS_HPP

/* A program the shell's panels offer: its label, and its path and space separated args */
struct shell_program {
    const char* label;
    const char* path;
    const char* args;
};

/* Runs the program its first argument names, with the rest as that program's arguments */
constexpr const char* TERMINAL_PATH = "/bin/stlxterm";

/* Opens the folder its argument names, or a file's folder with the file selected */
constexpr const char* FILE_MANAGER_PATH = "/bin/file-manager";

constexpr shell_program SETTINGS_PROGRAM = { "Settings", "/bin/stlxsettings", "" };
constexpr shell_program TASK_MONITOR_PROGRAM = { "Task monitor", TERMINAL_PATH, "/bin/stlxtop" };

#endif
