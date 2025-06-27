#ifndef STLXDM_SYS_H
#define STLXDM_SYS_H

/**
 * @brief Launches the stlxterm terminal application
 */
void stlxdm_launch_terminal();

/**
 * @brief Launches a process by name and returns its handle
 * @param process_name The name of the process to launch (without path or extension)
 * @return Process handle on success, -1 on failure
 */
int stlxdm_launch_process(const char* process_name);

#endif // STLXDM_SYS_H
