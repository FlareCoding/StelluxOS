#include "process_table.h"
#include <kstring.h>
#include <sync.h>

DECLARE_SPINLOCK(g_processTableLock);

kstl::vector<Task*> g_globalProcessTable;

void ProcessTable::registerTask(Task* task) {
    lockAccess();
    g_globalProcessTable.pushBack(task);
    unlockAccess();
}

void ProcessTable::unregisterTask(Task* task) {
    lockAccess();

    for (size_t i = 0; i < g_globalProcessTable.size(); i++) {
        if (g_globalProcessTable[i] == task) {
            g_globalProcessTable.erase(i);
            break;
        }
    }

    unlockAccess();
}

size_t ProcessTable::getGlobalTaskCount() {
    return g_globalProcessTable.size();
}

Task* ProcessTable::getTaskByName(const char* name) {
    Task* task = nullptr;

    lockAccess();

    for (size_t i = 0; i < g_globalProcessTable.size(); i++) {
        if (strcmp(g_globalProcessTable[i]->name, name) == 0) {
            task = g_globalProcessTable[i];
            break;
        }
    }

    unlockAccess();

    return task;
}

Task* ProcessTable::getTaskByPid(pid_t pid) {
    Task* task = nullptr;

    lockAccess();

    for (size_t i = 0; i < g_globalProcessTable.size(); i++) {
        if (g_globalProcessTable[i]->pid == pid) {
            task = g_globalProcessTable[i];
            break;
        }
    }

    unlockAccess();

    return task;
}

Task* ProcessTable::getTaskByProcessTableIndex(size_t idx) {
    return g_globalProcessTable[idx];
}

void ProcessTable::lockAccess() {
    acquireSpinlock(&g_processTableLock);
}

void ProcessTable::unlockAccess() {
    releaseSpinlock(&g_processTableLock);
}
