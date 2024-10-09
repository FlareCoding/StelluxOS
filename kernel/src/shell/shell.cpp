#include "shell.h"
#include <memory/kmemory.h>
#include <arch/x86/per_cpu_data.h>
#include <arch/x86/cpuid.h>
#include <acpi/shutdown.h>
#include <time/ktime.h>
#include <kelevate/kelevate.h>
#include <kprint.h>

void showHelpOptions() {
    kstl::string helpString =
R"(Command         Description
-------         -----------
help            shows available commands
ps              displays running processes on the system
meminfo         displays system memory information
shutdown        shuts the system down if running in a VM

)";

    current->console->write(helpString.c_str());
}

void handleShutdownCommand() {
    RUN_ELEVATED({
        if (cpuid_isRunningUnderQEMU()) {
            current->console->write("Shutting the system down in 1 second...\n");
            msleep(1000);
            vmshutdown();
        } else {
            current->console->write("Shutdown command is not yet supported on real hardware\n");
        }
    });
}

void handlePsCommand() {
    ProcessTable::lockAccess();
    
    for (size_t i = 0; i < ProcessTable::getGlobalTaskCount(); i++) {
        auto task = ProcessTable::getTaskByProcessTableIndex(i);
        kuPrint("%lli - %s\n", task->pid, task->name);
    }

    ProcessTable::unlockAccess();
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

    if (cmdStr == "ps") {
        handlePsCommand();
        return;
    }
    
    current->console->write("Unrecognized command: '");
    current->console->write(cmdStr.c_str());
    current->console->write("'\n");
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
        console->write(prompt, promptLen);
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
            console->write(prompt, promptLen);
            continue;
        }

        // Process the command
        parseCommand(cmdBuffer);

        zeromem(cmdBuffer, sizeof(cmdBuffer));
        console->write(prompt, promptLen);
    }

    exitKernelThread();
}
