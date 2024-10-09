#ifndef PROCESS_TABLE_H
#define PROCESS_TABLE_H
#include <kvector.h>
#include <process/process.h>

class ProcessTable {
public:
    static void registerTask(Task* task);
    static void unregisterTask(Task* task);
    
    static size_t getGlobalTaskCount();

    static Task* getTaskByName(const char* name);
    static Task* getTaskByPid(pid_t pid);
    static Task* getTaskByProcessTableIndex(size_t idx);

    static void lockAccess();
    static void unlockAccess();
};

#endif
