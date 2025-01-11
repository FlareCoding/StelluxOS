#include <types.h>
#include <serial/serial.h>
#include <sched/sched.h>
#include <core/string.h>
#include <time/time.h>
#include <dynpriv/dynpriv.h>
#include <acpi/shutdown.h>

constexpr size_t MAX_COMMAND_LENGTH = 256;

void process_command(const kstl::string& command) {
    if (command == "help") {
        serial::printf("Available commands:\n");
        serial::printf("  help         - Show this help message\n");
        serial::printf("  clear        - Clear the screen\n");
        serial::printf("  echo [text]  - Echo the text back\n");
        serial::printf("  shutdown     - Shutdown the system\n");
    } else if (command.starts_with("echo ")) {
        serial::printf(command.substring(5).c_str());
        serial::printf("\n");
    } else if (command == "shutdown") {
        serial::printf("Shutting system down...\n");
        msleep(800);
        RUN_ELEVATED({
            vmshutdown();
        });
    } else if (command == "clear") {
        serial::printf("\033[2J\033[H"); // ANSI escape codes to clear screen and move cursor to home
    } else {
        serial::printf("Unknown command. Type 'help' for a list of commands.\n");
    }
}

void shell_loop() {
    serial::printf("Shell started. Type 'help' for a list of commands.\n\n");

    char command_buffer[MAX_COMMAND_LENGTH] = {0};
    size_t command_length = 0;

    while (true) {
        char input = serial::read(serial::g_kernel_uart_port);

        if (input == '\n' || input == '\r') {
            // Process the command when Enter is pressed
            serial::printf("\n");
            if (command_length > 0) {
                command_buffer[command_length] = '\0';
                process_command(command_buffer);
                command_length = 0; // Reset the buffer for the next command
            }
            serial::printf("shell> "); // Print a new prompt
        } else if ((input == '\b' || input == 127) && command_length > 0) {
            // Handle backspace (127 is the ASCII DEL character)
            command_length--;
            serial::printf("\b \b"); // Move cursor back, overwrite with space, and move back again
        } else if (command_length < MAX_COMMAND_LENGTH - 1) {
            // Add the character to the command buffer
            command_buffer[command_length++] = input;
            serial::printf("%c", input);
        }
    }
}

int main() {
    //shell_loop();
    return 0;
}
