#include <gdb/gdb_stub.h>
#include <serial/serial.h>
#include <memory/memory.h>
#include <dynpriv/dynpriv.h>
#include <memory/paging.h>

kstl::shared_ptr<gdb_stub> gdb_stub::s_global_kernel_stub;

static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline char int_to_hex_char(int x) {
    const char* hex = "0123456789abcdef";
    return hex[x & 0xf];
}

static void append_u64_le(char* buf, size_t& pos, uint64_t val) {
    // For each byte in val, from LSB to MSB:
    for (int i = 0; i < 8; i++) {
        uint8_t b = (val >> (i * 8)) & 0xFF;
        buf[pos++] = int_to_hex_char((b >> 4) & 0xF);
        buf[pos++] = int_to_hex_char(b & 0xF);
    }
}

static int parse_hex64(const char* s, uint64_t& val) {
    val = 0;
    int shift = 0;
    int len = 0;
    while (s[len]) len++;
    for (int i = len - 1; i >= 0; i--) {
        int nib = hex_char_to_int(s[i]);
        if (nib < 0) return -1;
        val |= ((uint64_t)nib << (shift * 4));
        shift++;
    }
    return 0;
}

kstl::shared_ptr<gdb_stub> gdb_stub::get() {
    return s_global_kernel_stub;
}

kstl::shared_ptr<gdb_stub> gdb_stub::create(uint16_t serial_port_base) {
    s_global_kernel_stub = kstl::make_shared<gdb_stub>(serial_port_base);
    return s_global_kernel_stub;
}

void gdb_stub::perform_initial_trap() {
#ifdef ARCH_X86_64
    asm volatile ("int3");
#endif
}

gdb_stub::gdb_stub(uint16_t serial_port_base)
    : m_initialized(false), m_logging_enabled(true),
      m_serial_port_base(serial_port_base), m_trap_frame(nullptr),
      m_should_exit_handler(false) {
    zeromem(m_incoming_buf, PACKET_BUF_SIZE);
}

void gdb_stub::run_handler(ptregs* trap_frame) {
    m_should_exit_handler = false;
    m_trap_frame = trap_frame;

    if (m_initialized) {
        // Notify the client that the kernel trapped
        send_packet("S05");
    }

    uint64_t executed_instruction_addr = m_trap_frame->hwframe.rip - 1;
    if (m_breakpoints.find(executed_instruction_addr)) {
        breakpoint* bp = m_breakpoints.get(executed_instruction_addr);
        if (bp->active) {
            *reinterpret_cast<uint8_t*>(bp->address) = bp->original_byte;
            m_trap_frame->hwframe.rip -= 1;
        }
    }

    while (!m_should_exit_handler) {
        int len = read_packet();
        if (len >= 0) {
            handle_packet(len);
        }
    }
}

int gdb_stub::read_packet() {
    bool start_found = false;
    size_t length = 0;
    uint8_t checksum_calc = 0;

    // Keep trying to find a '$', then read until '#' + 2 checksum hex digits.
    while (true) {
        char c = read_byte();

        if (!start_found) {
            // Wait for start '$'
            if (c == '$') {
                start_found = true;
                length = 0;
                checksum_calc = 0;
            }
            // Ignore all chars until we find '$'
            continue;
        }

        // If we see '#', the next two bytes should be the checksum.
        if (c == '#') {
            // Read two hex digits
            char h = read_byte();
            char l = read_byte();

            int hi = hex_char_to_int(h);
            int lo = hex_char_to_int(l);

            // Invalid hex
            if (hi < 0 || lo < 0) {
                send_ack(false); // NAK
                return -1;
            }

            uint8_t sum_received = (uint8_t)((hi << 4) | lo);

            // Compare
            if (sum_received == checksum_calc) {
                // Good => send '+'
                send_ack(true);

                // Null-terminate the packet if there's space
                if (length < PACKET_BUF_SIZE) {
                    m_incoming_buf[length] = '\0';
                }

                // Return length (payload size)
                return (int)length;
            } else {
                // Bad => send '-'
                send_ack(false);
                // Restart from scratch
                start_found = false;
                length = 0;
                checksum_calc = 0;
            }
            continue;
        }

        // Otherwise, accumulate data
        if (length < PACKET_BUF_SIZE) {
            m_incoming_buf[length] = c;
        }
        length++;

        // Add to running checksum
        checksum_calc += (unsigned char)c;

        // If we exceed buffer, discard
        if (length >= PACKET_BUF_SIZE) {
            // Just restart
            start_found = false;
            length = 0;
            checksum_calc = 0;
        }
    }
}

void gdb_stub::handle_packet(int packet_len) {
    if (packet_len <= 0) {
        return;
    }

    if (m_logging_enabled) {
        serial::printf("GDB packet: '%s' (len=%d)\n", m_incoming_buf, packet_len);
    }

    if (memcmp(m_incoming_buf, "qSupported", 10) == 0) {
        send_packet("PacketSize=4000;swbreak+;hwbreak+;qXfer:features:read-;multiprocess-;"
                    "qRelocInsn-;fork-events-;vfork-events-;exec-events-;QThreadEvents-;"
                    "no-resumed+;memory-tagging-;xmlRegisters=i386");

        m_initialized = true;
    } else if (memcmp(m_incoming_buf, "qAttached", 10) == 0) {
        send_packet("1");
    } else if (memcmp(m_incoming_buf, "vCont?", 6) == 0) {
        send_packet("vCont;c;s");
        return;
    } else if (m_incoming_buf[0] == '?') {
        send_packet("S05");
    } else if (packet_len == 1 && m_incoming_buf[0] == 'g') {
        char out_buf[512] = { 0 };
        build_g_packet(out_buf, sizeof(out_buf));
        send_packet(out_buf);
    } else if (m_incoming_buf[0] == 'm') {
        char* result = reinterpret_cast<char*>(zmalloc(4096));
        build_m_packet(m_incoming_buf, result, 4096);
        send_packet(result);
        free(result);
    } else if (packet_len == 1 && m_incoming_buf[0] == 's') {
        m_trap_frame->hwframe.rflags |= 0x100; // TF bit (Trap Flag)
        m_should_exit_handler = true;
        send_packet("OK");
    } else if (memcmp(m_incoming_buf, "Z0", 2) == 0) {
        handle_Z0_packet(kstl::string(m_incoming_buf));
    } else if (memcmp(m_incoming_buf, "z0", 2) == 0) {
        handle_z0_packet(kstl::string(m_incoming_buf));
    } else if (memcmp(m_incoming_buf, "M", 1) == 0) {
        handle_M_packet(m_incoming_buf);
    } else if (memcmp(m_incoming_buf, "X", 1) == 0) {
        handle_X_packet(m_incoming_buf);
    } else if (memcmp(m_incoming_buf, "H", 1) == 0) {
        send_packet("OK");
    } else if (packet_len == 1 && m_incoming_buf[0] == 'c') {
        m_should_exit_handler = true;
        send_packet("OK");
    } else if (packet_len == 1 && m_incoming_buf[0] == 'D') {
        m_should_exit_handler = true;
        send_packet("OK");
    } else {
        send_packet("");
    }
}

void gdb_stub::send_packet(const char* payload) {
    if (m_logging_enabled) {
        serial::printf("gdb_stub::send_packet('%s')\n", payload);
    }

    // Calculate checksum
    uint8_t sum = 0;
    for (const char* p = payload; *p; p++) {
        sum += (uint8_t)(*p);
    }

    // Send start '$'
    write_byte('$');

    // Send payload
    for (const char* p = payload; *p; p++) {
        write_byte(*p);
    }

    // Send '#'
    write_byte('#');

    // Convert checksum to two hex digits
    static const char hex_digits[] = "0123456789abcdef";
    write_byte(hex_digits[(sum >> 4) & 0xF]);
    write_byte(hex_digits[sum & 0xF]);
}

char gdb_stub::read_byte() {
    return serial::read(m_serial_port_base);
}

void gdb_stub::write_byte(char c) {
    serial::write(m_serial_port_base, c);
}

void gdb_stub::send_ack(bool good) {
    // If checksum matched => '+', else '-'
    if (good) {
        write_byte('+');
    } else {
        write_byte('-');
    }
}

void gdb_stub::build_g_packet(char* out_buf, size_t buf_size) {
    // Zero out to be safe
    if (buf_size > 0) {
        zeromem(out_buf, buf_size);
    }
    size_t pos = 0;

    // 1) RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP
    append_u64_le(out_buf, pos, m_trap_frame->rax);
    append_u64_le(out_buf, pos, m_trap_frame->rbx);
    append_u64_le(out_buf, pos, m_trap_frame->rcx);
    append_u64_le(out_buf, pos, m_trap_frame->rdx);
    append_u64_le(out_buf, pos, m_trap_frame->rsi);
    append_u64_le(out_buf, pos, m_trap_frame->rdi);
    append_u64_le(out_buf, pos, m_trap_frame->rbp);
    append_u64_le(out_buf, pos, m_trap_frame->hwframe.rsp);

    // 2) R8..R15
    append_u64_le(out_buf, pos, m_trap_frame->r8);
    append_u64_le(out_buf, pos, m_trap_frame->r9);
    append_u64_le(out_buf, pos, m_trap_frame->r10);
    append_u64_le(out_buf, pos, m_trap_frame->r11);
    append_u64_le(out_buf, pos, m_trap_frame->r12);
    append_u64_le(out_buf, pos, m_trap_frame->r13);
    append_u64_le(out_buf, pos, m_trap_frame->r14);
    append_u64_le(out_buf, pos, m_trap_frame->r15);

    // 3) RIP, EFLAGS
    append_u64_le(out_buf, pos, m_trap_frame->hwframe.rip);
    append_u64_le(out_buf, pos, m_trap_frame->hwframe.rflags);

    // 4) CS, SS, DS, ES, FS, GS
    append_u64_le(out_buf, pos, m_trap_frame->hwframe.cs);
    append_u64_le(out_buf, pos, m_trap_frame->hwframe.ss);
    append_u64_le(out_buf, pos, m_trap_frame->ds);
    append_u64_le(out_buf, pos, m_trap_frame->es);
    append_u64_le(out_buf, pos, m_trap_frame->fs);
    append_u64_le(out_buf, pos, m_trap_frame->gs);

    // 5) FS_BASE, GS_BASE, K_FS_BASE, K_GS_BASE
    append_u64_le(out_buf, pos, 0);
    append_u64_le(out_buf, pos, 0);
    append_u64_le(out_buf, pos, 0);
    append_u64_le(out_buf, pos, 0);

    // Null-terminate
    if (pos < buf_size) {
        out_buf[pos] = '\0';
    }
}

void gdb_stub::build_m_packet(const char* pkt, char* out_buf, size_t out_buf_size) {
    if (out_buf_size == 0) {
        out_buf[0] = '\0';
        return;
    }

    const char* addr_str = &pkt[1];
    char* comma = nullptr;
    int i;
    for (i = 0; addr_str[i]; i++) {
        if (addr_str[i] == ',') {
            comma = (char*)&addr_str[i];
            break;
        }
    }
    if (!comma) {
        // Invalid
        out_buf[0] = '\0';
        return;
    }
    *comma = '\0';
    const char* len_str = comma + 1;

    uint64_t addr_val, len_val;
    if (parse_hex64(addr_str, addr_val) < 0) {
        out_buf[0] = '\0';
        return;
    }
    if (parse_hex64(len_str, len_val) < 0) {
        out_buf[0] = '\0';
        return;
    }

    if (len_val > (out_buf_size / 2)) {
        len_val = (out_buf_size / 2);
    }

    // Read memory
    static char temp[4096];
    if (len_val > sizeof(temp)) {
        len_val = sizeof(temp);
    }
    for (uint64_t j = 0; j < len_val; j++) {
        uintptr_t addr = (uint64_t)(addr_val + j);
        bool mapped = false;
        RUN_ELEVATED({
            mapped = (paging::get_physical_address(reinterpret_cast<void*>(addr))) != 0;
        });

        if (!mapped) {
            memcpy(out_buf, "E01", 4);
            out_buf[out_buf_size - 1] = '\0';
            return;
        }

        temp[j] = *(char*)(addr); 
    }

    // Hex-encode
    size_t pos = 0;
    for (uint64_t j = 0; j < len_val; j++) {
        unsigned char b = (unsigned char)temp[j];
        out_buf[pos++] = int_to_hex_char((b >> 4) & 0xF);
        out_buf[pos++] = int_to_hex_char(b & 0xF);
    }
    if (pos < out_buf_size) {
        out_buf[pos] = '\0';
    }
}

void gdb_stub::handle_X_packet(const kstl::string& packet) {
    // Parse: "X<address>,<length>:<data>"
    size_t comma_pos = packet.find(',');
    size_t colon_pos = packet.find(':');

    if (comma_pos == kstl::string::npos || colon_pos == kstl::string::npos) {
        send_packet("E01"); // Malformed packet
        return;
    }

    // Extract address and length.
    kstl::string addr_str = packet.substring(1, comma_pos - 1);
    kstl::string len_str = packet.substring(comma_pos + 1, colon_pos - comma_pos - 1);

    // Convert to uint64_t.
    uint64_t addr = 0, length = 0;
    if (parse_hex64(addr_str.c_str(), addr) < 0 || parse_hex64(len_str.c_str(), length) < 0) {
        send_packet("E01"); // Invalid address or length
        return;
    }

    // Extract binary data.
    const char* data = packet.c_str() + colon_pos + 1;

    // Write memory.
    for (size_t i = 0; i < length; i++) {
        *(volatile char*)(addr + i) = data[i];
    }

    if (m_logging_enabled) {
        serial::printf("Binary data written at 0x%lx, length %lu\n", addr, length);
    }

    send_packet("OK");
}


void gdb_stub::handle_M_packet(const kstl::string& packet) {
    // Parse: "M<address>,<length>:<data>"
    size_t comma_pos = packet.find(',');
    size_t colon_pos = packet.find(':');

    if (comma_pos == kstl::string::npos || colon_pos == kstl::string::npos) {
        send_packet("E01"); // Malformed packet
        return;
    }

    // Extract address and length as strings.
    kstl::string addr_str = packet.substring(1, comma_pos - 1);
    kstl::string len_str = packet.substring(comma_pos + 1, colon_pos - comma_pos - 1);

    // Convert to uint64_t.
    uint64_t addr = 0, length = 0;
    if (parse_hex64(addr_str.c_str(), addr) < 0 || parse_hex64(len_str.c_str(), length) < 0) {
        send_packet("E01"); // Invalid address or length
        return;
    }

    // Extract data to write.
    kstl::string data = packet.substring(colon_pos + 1);

    // Validate length.
    if (data.length() != length * 2) { // Each byte is 2 hex characters.
        send_packet("E01"); // Length mismatch
        return;
    }

    // Write memory.
    for (size_t i = 0; i < length; i++) {
        // Decode each byte from hex.
        char high = data[i * 2];
        char low = data[i * 2 + 1];
        int byte = (hex_char_to_int(high) << 4) | hex_char_to_int(low);

        if (byte < 0) {
            send_packet("E01"); // Invalid hex
            return;
        }

        // Write to memory.
        *(volatile char*)(addr + i) = (char)byte;
    }

    if (m_logging_enabled) {
        serial::printf("Memory written at 0x%llx, length %llu\n", addr, length);   
    }

    send_packet("OK");
}

void gdb_stub::handle_Z0_packet(const kstl::string& packet) {
    // Parse: "Z0,<address>,1"
    size_t first_comma = packet.find(',');
    size_t second_comma = packet.find(',', first_comma + 1);

    if (first_comma == kstl::string::npos || second_comma == kstl::string::npos) {
        send_packet("E01"); // Malformed packet
        return;
    }

    // Extract address as a string.
    kstl::string addr_str = packet.substring(first_comma + 1, second_comma - first_comma - 1);

    // Convert address to uint64_t.
    uint64_t addr = 0;
    if (parse_hex64(addr_str.c_str(), addr) < 0) {
        send_packet("E01"); // Invalid address
        return;
    }

    // Check if a breakpoint is already set.
    if (m_breakpoints.find(addr) && m_breakpoints.get(addr)->active) {
        if (m_logging_enabled) {
            serial::printf("Failed to set breakpoint at 0x%llx, breakpoint is already active\n", addr);
        }
        send_packet("E02");
        return;
    }

    bool mapped = false;
    RUN_ELEVATED({
        mapped = (paging::get_physical_address(reinterpret_cast<void*>(addr))) != 0;
    });
    if (!mapped) {
        if (m_logging_enabled) {
            serial::printf("Failed to set breakpoint at 0x%llx, address not mapped\n", addr);
        }

        send_packet("E01"); // Invalid address
        return;
    }

    // Set the breakpoint
    breakpoint bp;
    bp.address = addr;
    bp.original_byte = *reinterpret_cast<uint8_t*>(addr);
    bp.active = true;
    m_breakpoints.insert(addr, bp);

    *reinterpret_cast<uint8_t*>(addr) = 0xcc;

    if (m_logging_enabled) {
        serial::printf("Breakpoint set at 0x%llx\n", addr);
    }

    send_packet("OK");
}

void gdb_stub::handle_z0_packet(const kstl::string& packet) {
    size_t first_comma = packet.find(',');
    size_t second_comma = packet.find(',', first_comma + 1);

    if (first_comma == kstl::string::npos || second_comma == kstl::string::npos) {
        send_packet("E01"); // Malformed packet
        return;
    }

    // Extract address as a string.
    kstl::string addr_str = packet.substring(first_comma + 1, second_comma - first_comma - 1);

    // Convert address to uint64_t.
    uint64_t addr = 0;
    if (parse_hex64(addr_str.c_str(), addr) < 0) {
        send_packet("E01"); // Invalid address
        return;
    }

    // Ensure the breakpoint exists
    if (!m_breakpoints.find(addr)) {
        if (m_logging_enabled) {
            serial::printf("No active breakpoint at 0x%llx to remove\n", addr);
        }

        send_packet("E01"); // No breakpoint set
        return;
    }

    breakpoint* bp = m_breakpoints.get(addr);

    // Restore the original byte.
    *reinterpret_cast<uint8_t*>(bp->address) = bp->original_byte;

    // Mark the breakpoint as inactive.
    bp->active = false;

    if (m_logging_enabled) {
        serial::printf("Breakpoint removed at 0x%llx\n", addr);
    }

    send_packet("OK");
}
