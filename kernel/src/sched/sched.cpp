#include "sched.h"
#include <memory/kmemory.h>

Scheduler s_globalScheduler;

Scheduler& Scheduler::get() {
    return s_globalScheduler;
}

void Scheduler::init() {
    for (uint64_t i = 0; i < MAX_QUEUED_PROCESSES; i++) {
        auto& pcb = m_taskQueue[i];
        zeromem(&pcb, sizeof(PCB));

        pcb.state = ProcessState::TERMINATED;
    }
}

int32_t Scheduler::addTask(PCB task) {
    if (m_taskCount >= MAX_QUEUED_PROCESSES) {
        return -1; // Scheduler is full
    }
    for (uint64_t i = 0; i < MAX_QUEUED_PROCESSES; ++i) {
        if (m_taskQueue[i].state == ProcessState::TERMINATED) {
            m_taskQueue[i] = task;
            m_taskCount++;
            return i; // Return the index where the task was placed
        }
    }

    return -1;
}

void Scheduler::removeTask(int32_t index) {
    if (index < 0 || index >= MAX_QUEUED_PROCESSES) {
        return;
    }

    m_taskQueue[index].state = ProcessState::TERMINATED;
    m_taskCount--;
}

void Scheduler::removeTaskWithPid(uint64_t pid) {
    for (uint64_t i = 0; i < MAX_QUEUED_PROCESSES; i++) {
        if (m_taskQueue[i].pid == pid) {
            removeTask(i);
            break;
        }
    }
}

ProcessState Scheduler::getTaskState(int32_t index) {
    if (index < 0 || index >= MAX_QUEUED_PROCESSES) {
        return ProcessState::TERMINATED;
    }

    return m_taskQueue[index].state;
}

PCB* Scheduler::getNextTask() {
    uint64_t startingIndex = m_currentTaskIndex;
    do {
        m_currentTaskIndex = (m_currentTaskIndex + 1) % MAX_QUEUED_PROCESSES;
        if (m_taskQueue[m_currentTaskIndex].state == ProcessState::READY) {
            return &m_taskQueue[m_currentTaskIndex];
        }
    } while (m_currentTaskIndex != startingIndex);

    return nullptr; // No task is ready to run
}
