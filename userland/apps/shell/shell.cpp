#include <types.h>
#include <core/klog.h>
#include <sched/sched.h>
#include <core/string.h>
#include <time/time.h>
#include <dynpriv/dynpriv.h>
#include <acpi/fadt.h>
#include <arch/x86/cpuid.h>
#include <arch/x86/msr.h>
#include <input/system_input_manager.h>

constexpr size_t MAX_COMMAND_LENGTH = 256;

void print_cache_size(const char* level, uint32_t size) {
    if (size >= 1024 * 1024) {
        kprint("%s Cache: %u MB\n", level, size / (1024 * 1024));
    } else if (size >= 1024) {
        kprint("%s Cache: %u KB\n", level, size / 1024);
    } else {
        kprint("%s Cache: %u Bytes\n", level, size);
    }
}

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
        char cpu_brand[52] = { 0 };
        uint32_t cpu_family = 0, cpu_model = 0, cpu_stepping = 0;
        uint32_t num_logical_cores = 0, num_physical_cores = 0;
        uint32_t l1_cache = 0, l2_cache = 0, l3_cache = 0;

        RUN_ELEVATED({
            arch::x86::cpuid_read_vendor_id(cpu_vendor_str);
            cpu_family = arch::x86::cpuid_read_cpu_family();
            cpu_model = arch::x86::cpuid_read_cpu_model();
            cpu_stepping = arch::x86::cpuid_read_cpu_stepping();
            arch::x86::cpuid_read_cpu_brand(cpu_brand);
            num_logical_cores = arch::x86::cpuid_read_logical_cores();
            num_physical_cores = arch::x86::cpuid_read_physical_cores();
            arch::x86::cpuid_read_cache_sizes(&l1_cache, &l2_cache, &l3_cache);

            kprint("Vendor: %s\n", cpu_vendor_str);
            kprint("CPU: %s\n", cpu_brand);
            kprint("Family: 0x%x\n", cpu_family);
            kprint("Model: 0x%x\n", cpu_model);
            kprint("Stepping: 0x%x\n", cpu_stepping);
            kprint("Logical Cores: %u\n", num_logical_cores);
            kprint("Physical Cores: %u\n", num_physical_cores);
            print_cache_size("L1", l1_cache);
            print_cache_size("L2", l2_cache);
            print_cache_size("L3", l3_cache);
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

    auto& input_manager = input::system_input_manager::get();
    auto kbd_queue = input_manager.get_queue(INPUT_QUEUE_ID_KBD);
    if (!kbd_queue) {
        kprint("[SHELL] Input queue for keyboard events was not found!\n");
        return;
    }

    input::input_event_t evt;

    while (true) {
        // char input = serial::read(serial::g_kernel_uart_port);
        if (!kbd_queue->wait_and_pop(evt)) {
            continue;
        }
        char input = static_cast<char>(evt.sdata1);

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
