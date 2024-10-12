#include "shell.h"
#include <memory/kmemory.h>
#include <arch/x86/per_cpu_data.h>
#include <arch/x86/cpuid.h>
#include <acpi/shutdown.h>
#include <time/ktime.h>
#include <kelevate/kelevate.h>
#include <drivers/graphics/vga_text_driver.h>
#include <kprint.h>

void showHelpOptions() {
    kstl::string helpString =
R"(Command         Description
-------         -----------
help            shows available commands
clear           clears the VGA screen buffer
whoami          prints the current user's name
ps              displays running processes on the system
meminfo         displays system memory information
shutdown        shuts the system down if running in a VM

)";

    kprintf(helpString.c_str());
}

void handleShutdownCommand() {
    RUN_ELEVATED({
        if (cpuid_isRunningUnderQEMU()) {
            kprintf("Shutting the system down in 1 second...\n");
            msleep(1000);
            vmshutdown();
        } else {
            kprintf("Shutdown command is not yet supported on real hardware\n");
        }
    });
}

void handleClearCommand() {
    VGADriver::clearScreen();
    VGATextDriver::resetCursorPos();
}

void handleWhoamiCommand() {
    kprintf("root\n");
}

void handlePsCommand() {
    ProcessTable::lockAccess();
    
    kprintf("PID   CPU   Name\n");
    kprintf("----------------\n");
    for (size_t i = 0; i < ProcessTable::getGlobalTaskCount(); i++) {
        auto task = ProcessTable::getTaskByProcessTableIndex(i);
        kprintf("%lli   %i   %s\n", task->pid, task->cpu, task->name);
    }

    ProcessTable::unlockAccess();
}

void handleTestCommand() {
    kprintf("----------------------\n");
    kprintf("before elevating\n");
    kprintf("current->elevated: %i\n", current->elevated);
    RUN_ELEVATED({
        for (int i = 0; i < 10; i++) {
            kprintf("elevated print: %i / 10  ", i + 1);
            kprintf("current->elevated: %i\n", current->elevated);
            sleep(1);
        }
    });
    kprintf("after lowering\n");
    kprintf("current->elevated: %i\n", current->elevated);
    kprintf("----------------------\n");
}

void parseCommand(const char* cmd) {
    kstl::string cmdStr = kstl::string(cmd);

    if (cmdStr == "help") {
        showHelpOptions();
        return;
    }

    if (cmdStr == "shutdown") {
        handleShutdownCommand();
        return;
    }

    if (cmdStr == "clear") {
        handleClearCommand();
        return;
    }

    if (cmdStr == "whoami") {
        handleWhoamiCommand();
        return;
    }

    if (cmdStr == "ps") {
        handlePsCommand();
        return;
    }

    if (cmdStr == "test") {
        handleTestCommand();
        return;
    }
    
    kprintf("Unrecognized command: '");
    kprintf(cmdStr.c_str());
    kprintf("'\n");
}

void userShellTestEntry(void*) {
    // Get the task's console interface
    Console* console = current->console;

    // This process will grab focus of the global console
    setActiveConsole(console);

    // Prompt for the shell
    const char* prompt = "shell> ";
    const size_t promptLen = strlen(prompt);

    if (console) {
        kprintf(prompt, promptLen);
    }

    const size_t bufferSize = 1024;
    char* cmdBuffer = (char*)kzmalloc(bufferSize);

    while (true) {
        if (!console) {
            continue;
        }

        size_t bytesRead = console->readLine(cmdBuffer, bufferSize - 1);
        if (!bytesRead) {
            zeromem(cmdBuffer, sizeof(cmdBuffer));
            kprintf(prompt, promptLen);
            continue;
        }

        // Process the command
        parseCommand(cmdBuffer);

        zeromem(cmdBuffer, sizeof(cmdBuffer));
        kprintf(prompt, promptLen);
    }

    exitKernelThread();
}
