#include <types.h>
#include <core/klog.h>
#include <sched/sched.h>
#include <core/string.h>
#include <time/time.h>
#include <dynpriv/dynpriv.h>
#include <acpi/fadt.h>
#include <arch/x86/cpuid.h>
#include <arch/x86/msr.h>

constexpr size_t MAX_COMMAND_LENGTH = 256;

void process_command(const kstl::string& command) {
    if (command == "help") {
        kprint("Available commands:\n");
        kprint("  help         - Show this help message\n");
        kprint("  clear        - Clear the screen\n");
        kprint("  echo [text]  - Echo the text back\n");
        kprint("  shutdown     - Shutdown the system\n");
        kprint("  reboot       - Reboot the system\n");
        kprint("  cpuinfo      - Prints the information about the system's CPU\n");
    } else if (command.starts_with("echo ")) {
        kprint(command.substring(5).c_str());
        kprint("\n");
    } else if (command == "shutdown") {
        msleep(100);
        RUN_ELEVATED({
            auto& fadt = acpi::fadt::get();
            fadt.shutdown();
        });
    } else if (command == "reboot") {
        msleep(100);
        RUN_ELEVATED({
            auto& fadt = acpi::fadt::get();
            fadt.reboot();
        });
    } else if (command == "cpuinfo") {
        char cpu_vendor_str[24] = { 0 };
        int32_t cpu_temperature = -1;
        RUN_ELEVATED({
            arch::x86::cpuid_read_vendor_id(cpu_vendor_str);
            cpu_temperature = arch::x86::msr::read_cpu_temperature();

            kprint("Vendor: %s\n", cpu_vendor_str);
            kprint("Current Temp: %iC\n", cpu_temperature);
        });
    } else if (command == "clear") {
        kprint("\033[2J\033[H"); // ANSI escape codes to clear screen and move cursor to home
    } else {
        kprint("Unknown command. Type 'help' for a list of commands.\n");
    }
}

void shell_loop() {
    kprint("Shell started. Type 'help' for a list of commands.\n\n");

    char command_buffer[MAX_COMMAND_LENGTH] = {0};
    size_t command_length = 0;

    while (true) {
        char input = serial::read(serial::g_kernel_uart_port);

        if (input == '\n' || input == '\r') {
            // Process the command when Enter is pressed
            kprint("\n");
            if (command_length > 0) {
                command_buffer[command_length] = '\0';
                process_command(command_buffer);
                command_length = 0; // Reset the buffer for the next command
            }
            kprint("shell> "); // Print a new prompt
        } else if ((input == '\b' || input == 127) && command_length > 0) {
            // Handle backspace (127 is the ASCII DEL character)
            command_length--;
            kprint("\b \b"); // Move cursor back, overwrite with space, and move back again
        } else if (command_length < MAX_COMMAND_LENGTH - 1) {
            // Add the character to the command buffer
            command_buffer[command_length++] = input;
            kprint("%c", input);
        }
    }
}

int main() {
    shell_loop();
    return 0;
}
