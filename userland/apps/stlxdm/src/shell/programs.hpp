#ifndef STLXDM_SHELL_PROGRAMS_HPP
#define STLXDM_SHELL_PROGRAMS_HPP

/* A program the shell's panels offer: its label, and its path and space separated args */
struct shell_program {
    const char* label;
    const char* path;
    const char* args;
};

constexpr shell_program SETTINGS_PROGRAM = { "Settings", "/bin/stlxsettings", "" };
constexpr shell_program TASK_MONITOR_PROGRAM = { "Task monitor", "/bin/stlxterm", "/bin/stlxtop" };

#endif
