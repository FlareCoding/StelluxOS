#ifndef STLXTERM_PROCESS_MANAGER_H
#define STLXTERM_PROCESS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include "terminal.h"

// Process state
typedef enum {
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_STOPPED,
    PROCESS_STATE_TERMINATED,
    PROCESS_STATE_ZOMBIE
} process_state_t;

// Process information
typedef struct {
    pid_t pid;
    process_state_t state;
    char command[256];
    int exit_code;
    bool has_exited;
} process_info_t;

// Process manager structure
typedef struct {
    process_info_t current_process;
    int stdin_pipe[2];
    int stdout_pipe[2];
    int stderr_pipe[2];
    bool pipes_created;
    bool process_running;
    
    // Input/output buffers
    char input_buffer[1024];
    int input_buffer_pos;
    char output_buffer[4096];
    int output_buffer_pos;
    char error_buffer[4096];
    int error_buffer_pos;
} process_manager_t;

// Function prototypes
int process_manager_init(process_manager_t* pm);
void process_manager_cleanup(process_manager_t* pm);
void process_manager_reset(process_manager_t* pm);

// Process control
int process_manager_start_process(process_manager_t* pm, const char* command);
int process_manager_stop_process(process_manager_t* pm);
int process_manager_kill_process(process_manager_t* pm);
bool process_manager_is_process_running(process_manager_t* pm);

// Input/output handling
int process_manager_write_input(process_manager_t* pm, const char* data, int length);
int process_manager_read_output(process_manager_t* pm, char* buffer, int max_length);
int process_manager_read_error(process_manager_t* pm, char* buffer, int max_length);
void process_manager_flush_output(process_manager_t* pm, terminal_t* term);

// Process monitoring
void process_manager_check_process_status(process_manager_t* pm);
int process_manager_get_exit_code(process_manager_t* pm);

// Utility functions
const char* process_manager_get_command(process_manager_t* pm);
pid_t process_manager_get_pid(process_manager_t* pm);

#endif // STLXTERM_PROCESS_MANAGER_H
