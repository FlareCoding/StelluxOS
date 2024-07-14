#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <kelevate/kelevate.h>
#include <arch/x86/msr.h>
#include <time/ktime.h>

void ke_test_read_cpu_temps() {
    while (true) {
        uint64_t ia32_therm_status_msr = 0;
        RUN_ELEVATED({
            ia32_therm_status_msr = readMsr(0x19C); // IA32_THERM_STATUS MSR
        });
        int temp_offset = (ia32_therm_status_msr >> 16) & 0x7F; // Extracting temperature

        // Assuming TjMax is 100 degrees Celsius
        // This value might differ based on the CPU. Check your CPU's specific documentation.
        int tj_max = 100; 

        int cpu_temp = tj_max - temp_offset;
        kuPrint("CPU Temperature: %lliC\n", cpu_temp);
        sleep(1);
    }
}
