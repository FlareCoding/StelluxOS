#include "sched.h"
#include <memory/kmemory.h>

RoundRobinScheduler s_globalRRScheduler;

RoundRobinScheduler& RoundRobinScheduler::get() {
    return s_globalRRScheduler;
}

RoundRobinScheduler::RoundRobinScheduler() {
    for (size_t i = 0; i < MAX_QUEUED_PROCESSES; ++i) {
        memset(&m_runQueue[i], 0, sizeof(PCB));
        m_runQueue[i].state = ProcessState::INVALID;
    }
}

PCB* RoundRobinScheduler::peekNextTask() {
    if (m_tasksInQueue == 0) {
        return nullptr;
    }

    if (m_tasksInQueue == 1) {
        return getCurrentTask();
    }

    size_t index = m_currentTaskIndex;
    do {
        index = (index + 1) % MAX_QUEUED_PROCESSES;
        if (m_runQueue[index].state == ProcessState::READY) {
            return &m_runQueue[index];
        }
    } while (m_currentTaskIndex != index);

    return nullptr;
}

bool RoundRobinScheduler::switchToNextTask() {
    // If there is only a single task in the queue
    if (m_tasksInQueue < 2) {
        return false;
    }

    size_t startingIndex = m_currentTaskIndex;
    do {
        m_currentTaskIndex = (m_currentTaskIndex + 1) % MAX_QUEUED_PROCESSES;
        if (m_runQueue[m_currentTaskIndex].state == ProcessState::READY) {
            // Update the state of the processes
            m_runQueue[startingIndex].state = ProcessState::READY;
            m_runQueue[m_currentTaskIndex].state = ProcessState::RUNNING;

            return true;
        }
    } while (m_currentTaskIndex != startingIndex);

    return false;
}

size_t RoundRobinScheduler::addTask(const PCB& task) {
    for (size_t i = 0; i < MAX_QUEUED_PROCESSES; ++i) {
        if (m_runQueue[i].state == ProcessState::INVALID) {
            m_runQueue[i] = task;
            ++m_tasksInQueue;
            return i; // Return the index where the task was placed
        }
    }
    
    return -1;
}

PCB* RoundRobinScheduler::getTask(size_t idx) {
    if (idx >= MAX_QUEUED_PROCESSES) {
        return nullptr;
    }

    return &m_runQueue[idx];

    return nullptr;
}

PCB* RoundRobinScheduler::findTaskByPid(pid_t pid) {
    for (size_t i = 0; i < MAX_QUEUED_PROCESSES; ++i) {
        if (m_runQueue[i].pid == pid) {
            return &m_runQueue[i];
        }
    }

    return nullptr;
}
