#include <gdb/gdb_stub.h>
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
    : m_initialized(false), m_logging_enabled(false),
      m_serial_port_base(serial_port_base), m_trap_frame(nullptr),
      m_should_exit_handler(false) {
    zeromem(m_incoming_buf, INBOUND_PACKET_BUF_SIZE);
}

void gdb_stub::run_handler(ptregs* trap_frame) {
    m_should_exit_handler = false;
    m_trap_frame = trap_frame;

    if (m_initialized) {
        // Notify the client that the kernel trapped
        _notify_gdb_client_of_trap_status();
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
        char c = _read_byte();

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
            char h = _read_byte();
            char l = _read_byte();

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
                if (length < INBOUND_PACKET_BUF_SIZE) {
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
        if (length < INBOUND_PACKET_BUF_SIZE) {
            m_incoming_buf[length] = c;
        }
        length++;

        // Add to running checksum
        checksum_calc += (unsigned char)c;

        // If we exceed buffer, discard
        if (length >= INBOUND_PACKET_BUF_SIZE) {
            // Just restart
            start_found = false;
            length = 0;
            checksum_calc = 0;
        }
    }
}

gdb_stub::gdb_packet_type gdb_stub::_get_packet_type() const {
    if (memcmp(m_incoming_buf, "qSupported", 10) == 0) {
        return gdb_packet_type::QUERY_SUPPORTED;
    } else if (memcmp(m_incoming_buf, "qAttached", 10) == 0) {
        return gdb_packet_type::QUERY_ATTACHED;
    } else if (memcmp(m_incoming_buf, "vCont?", 6) == 0) {
        return gdb_packet_type::VCONT_QUERY;
    } else if (m_incoming_buf[0] == '?') {
        return gdb_packet_type::TRAP_STATUS;
    } else if (m_incoming_buf[0] == 'g' && strlen(m_incoming_buf) == 1) {
        return gdb_packet_type::READ_REGISTERS;
    } else if (m_incoming_buf[0] == 'm') {
        return gdb_packet_type::READ_MEMORY;
    } else if (m_incoming_buf[0] == 's' && strlen(m_incoming_buf) == 1) {
        return gdb_packet_type::SINGLE_STEP;
    } else if (memcmp(m_incoming_buf, "Z0", 2) == 0) {
        return gdb_packet_type::SET_BREAKPOINT;
    } else if (memcmp(m_incoming_buf, "z0", 2) == 0) {
        return gdb_packet_type::CLEAR_BREAKPOINT;
    } else if (memcmp(m_incoming_buf, "M", 1) == 0) {
        return gdb_packet_type::WRITE_MEMORY;
    } else if (memcmp(m_incoming_buf, "X", 1) == 0) {
        return gdb_packet_type::BINARY_WRITE_MEMORY;
    } else if (memcmp(m_incoming_buf, "H", 1) == 0) {
        return gdb_packet_type::THREAD_OP;
    } else if (m_incoming_buf[0] == 'c' && strlen(m_incoming_buf) == 1) {
        return gdb_packet_type::CONTINUE_EXECUTION;
    } else if (m_incoming_buf[0] == 'D' && strlen(m_incoming_buf) == 1) {
        return gdb_packet_type::DETACH;
    }
    return gdb_packet_type::UNKNOWN;
}

void gdb_stub::handle_packet(int packet_len) {
    if (packet_len <= 0) {
        return;
    }

    _dbg_log("GDB packet: '%s' (len=%d)\n", m_incoming_buf, packet_len);

    switch (_get_packet_type()) {
        case gdb_packet_type::QUERY_SUPPORTED: {
            _handle_supported_features_packet();
            break;
        }
        case gdb_packet_type::QUERY_ATTACHED: {
            _handle_attached_packet();
            break;
        }
        case gdb_packet_type::VCONT_QUERY: {
            _handle_vcont_query_packet();
            break;
        }
        case gdb_packet_type::TRAP_STATUS: {
            _notify_gdb_client_of_trap_status();
            break;
        }
        case gdb_packet_type::READ_REGISTERS: {
            _handle_registers_read_packet();
            break;
        }
        case gdb_packet_type::READ_MEMORY: {
            _handle_memory_read_packet(m_incoming_buf);
            break;
        }
        case gdb_packet_type::SINGLE_STEP: {
            _handle_single_step_packet();
            break;
        }
        case gdb_packet_type::SET_BREAKPOINT: {
            _handle_set_breakpoint_packet(m_incoming_buf);
            break;
        }
        case gdb_packet_type::CLEAR_BREAKPOINT: {
            _handle_clear_breakpoint_packet(m_incoming_buf);
            break;
        }
        case gdb_packet_type::WRITE_MEMORY: {
            _handle_memory_write_packet(m_incoming_buf);
            break;
        }
        case gdb_packet_type::BINARY_WRITE_MEMORY: {
            _handle_memory_binary_write_packet(m_incoming_buf);
            break;
        }
        case gdb_packet_type::THREAD_OP: {
            _send_ok();
            break;
        }
        case gdb_packet_type::CONTINUE_EXECUTION: {
            _handle_continue_packet();
            break;
        }
        case gdb_packet_type::DETACH: {
            _handle_detach_packet();
            break;
        }
        case gdb_packet_type::UNKNOWN:
        default:
            _send_empty_packet();
            break;
    }
}

void gdb_stub::send_packet(const char* payload) const {
    _dbg_log("gdb_stub::send_packet('%s')\n", payload);

    // Calculate checksum
    uint8_t sum = 0;
    for (const char* p = payload; *p; p++) {
        sum += (uint8_t)(*p);
    }

    // Send start '$'
    _write_byte('$');

    // Send payload
    for (const char* p = payload; *p; p++) {
        _write_byte(*p);
    }

    // Send '#'
    _write_byte('#');

    // Convert checksum to two hex digits
    static const char hex_digits[] = "0123456789abcdef";
    _write_byte(hex_digits[(sum >> 4) & 0xF]);
    _write_byte(hex_digits[sum & 0xF]);
}

char gdb_stub::_read_byte() const {
    return serial::read(m_serial_port_base);
}

void gdb_stub::_write_byte(char c) const {
    serial::write(m_serial_port_base, c);
}

void gdb_stub::send_ack(bool good) {
    // If checksum matched => '+', else '-'
    if (good) {
        _write_byte('+');
    } else {
        _write_byte('-');
    }
}

void gdb_stub::_send_empty_packet() const {
    send_packet("");
}

void gdb_stub::_send_ok() const {
    send_packet(RESPONSE_OK);
}

void gdb_stub::_send_error(const char* error_code) const {
    send_packet(error_code);
}

void gdb_stub::_notify_gdb_client_of_trap_status() const {
    send_packet("S05");
}

bool gdb_stub::_is_virtual_address_mapped(uintptr_t vaddr) const {
    bool mapped = false;

    RUN_ELEVATED({
        mapped = (paging::get_physical_address(reinterpret_cast<void*>(vaddr))) != 0;
    });

    return mapped;
}

bool gdb_stub::_write_binary_data_to_memory(uint64_t addr, const char* data, size_t length) const {
    for (size_t i = 0; i < length; ++i) {
        uintptr_t write_addr = addr + i;

        if (!_is_virtual_address_mapped(write_addr)) {
            return false; // Memory not mapped
        }

        // Write the data byte to the memory address
        *reinterpret_cast<volatile char*>(write_addr) = data[i];
    }

    return true; // Success
}

void gdb_stub::_handle_supported_features_packet() const {
    // Respond with supported features
    send_packet("PacketSize=4000;swbreak+;hwbreak+;qXfer:features:read-;multiprocess-;"
                "qRelocInsn-;fork-events-;vfork-events-;exec-events-;QThreadEvents-;"
                "no-resumed+;memory-tagging-;xmlRegisters=i386");
}

void gdb_stub::_handle_attached_packet() {
    // Respond with "1" to indicate the debugger is attached
    send_packet("1");

    // Mark the debugger as initialized
    m_initialized = true;

    _dbg_log("GDB client attached, debugger initialized.\n");
}

void gdb_stub::_handle_vcont_query_packet() const {
    // Respond with supported vCont actions: continue (c) and step (s)
    send_packet("vCont;c;s");
}

void gdb_stub::_handle_single_step_packet() {
    // Enable the Trap Flag (TF) in the RFLAGS register for single-stepping
    m_trap_frame->hwframe.rflags |= 0x100;

    // Indicate that the GDB handler should exit after processing this packet
    m_should_exit_handler = true;

    // Respond with "OK" to acknowledge the packet
    _send_ok();
}

void gdb_stub::_handle_continue_packet() {
    // Indicate that the GDB handler should exit after processing this packet
    m_should_exit_handler = true;

    // Respond with "OK" to acknowledge the packet
    _send_ok();
}

void gdb_stub::_handle_detach_packet() {
    // Indicate that the GDB handler should exit after processing this packet
    m_should_exit_handler = true;

    // TO-DO: clear out any active breakpoints

    // Respond with "OK" to acknowledge the detach packet
    _send_ok();

    _dbg_log("GDB detach request handled: exiting handler.\n");

    // De-initialize the stub
    m_initialized = false;
}

void gdb_stub::_build_registers_packet(char* buffer, size_t buffer_size) const {
    // Ensure the buffer is valid and has space.
    if (buffer == nullptr || buffer_size == 0) {
        return;
    }

    // Zero out the buffer for safety.
    zeromem(buffer, buffer_size);

    size_t pos = 0;

    // Helper macro to append a register value safely.
    auto append_register = [&](uint64_t reg_value) {
        if (pos + 16 >= buffer_size) { // Each register requires 16 characters (8 bytes as hex)
            return;
        }
        append_u64_le(buffer, pos, reg_value);
    };

    // General-purpose registers: RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP
    append_register(m_trap_frame->rax);
    append_register(m_trap_frame->rbx);
    append_register(m_trap_frame->rcx);
    append_register(m_trap_frame->rdx);
    append_register(m_trap_frame->rsi);
    append_register(m_trap_frame->rdi);
    append_register(m_trap_frame->rbp);
    append_register(m_trap_frame->hwframe.rsp);

    // Registers R8..R15
    append_register(m_trap_frame->r8);
    append_register(m_trap_frame->r9);
    append_register(m_trap_frame->r10);
    append_register(m_trap_frame->r11);
    append_register(m_trap_frame->r12);
    append_register(m_trap_frame->r13);
    append_register(m_trap_frame->r14);
    append_register(m_trap_frame->r15);

    // Instruction pointer and flags
    append_register(m_trap_frame->hwframe.rip);
    append_register(m_trap_frame->hwframe.rflags);

    // Segment registers
    append_register(m_trap_frame->hwframe.cs);
    append_register(m_trap_frame->hwframe.ss);
    append_register(m_trap_frame->ds);
    append_register(m_trap_frame->es);
    append_register(m_trap_frame->fs);
    append_register(m_trap_frame->gs);

    // Reserved fields (FS_BASE, GS_BASE, etc.)
    append_register(0);
    append_register(0);
    append_register(0);
    append_register(0);

    // Null-terminate the buffer if space permits
    if (pos < buffer_size) {
        buffer[pos] = '\0';
    }
}

void gdb_stub::_handle_registers_read_packet() const {
    // Allocate a buffer for the response
    char* response_buffer = reinterpret_cast<char*>(zmalloc(512));
    if (response_buffer == nullptr) {
        _send_error(RESPONSE_ERROR_GENERIC); // Allocation failure
        return;
    }

    // Build the packet
    _build_registers_packet(response_buffer, 512);

    // Send the packet
    send_packet(response_buffer);

    // Free the allocated buffer
    free(response_buffer);
}

void gdb_stub::_handle_memory_read_packet(const char* packet) const {
    // Extract address and length from the packet
    const char* addr_str = &packet[1];
    char* comma = nullptr;

    size_t addr_str_len = 0;
    while (addr_str[addr_str_len] && addr_str[addr_str_len] != ',') {
        addr_str_len++;
    }

    if (addr_str[addr_str_len] != ',') {
        _send_error(RESPONSE_ERROR_GENERIC); // Malformed packet
        return;
    }

    comma = const_cast<char*>(&addr_str[addr_str_len]);
    *comma = '\0'; // Temporarily null-terminate the address portion
    const char* len_str = comma + 1;

    uint64_t addr = 0;
    uint64_t length = 0;
    if (parse_hex64(addr_str, addr) < 0 || parse_hex64(len_str, length) < 0) {
        _send_error(RESPONSE_ERROR_GENERIC); // Invalid address or length
        return;
    }

    *comma = ','; // Restore the original comma in the packet

    // Allocate a response buffer large enough to hold hex-encoded data
    const size_t max_buffer_size = 4096; // Use the defined buffer size
    char* buffer = reinterpret_cast<char*>(zmalloc(max_buffer_size));
    if (buffer == nullptr) {
        _send_error(RESPONSE_ERROR_GENERIC); // Allocation failure
        return;
    }

    // Ensure buffer can accommodate the encoded data (2 hex chars per byte + null terminator)
    if (length > (max_buffer_size - 1) / 2) {
        free(buffer);
        _send_error(RESPONSE_ERROR_GENERIC); // Buffer too small
        return;
    }

    // Read memory and encode into the buffer
    char* write_ptr = buffer;
    for (uint64_t i = 0; i < length; ++i) {
        uintptr_t read_addr = addr + i;

        if (!_is_virtual_address_mapped(read_addr)) {
            free(buffer);
            _send_error(RESPONSE_ERROR_GENERIC); // Address not mapped
            return;
        }

        // Read byte and encode to hex
        uint8_t byte = *reinterpret_cast<volatile uint8_t*>(read_addr);
        *write_ptr++ = int_to_hex_char((byte >> 4) & 0xF); // High nibble
        *write_ptr++ = int_to_hex_char(byte & 0xF);        // Low nibble
    }

    // Null-terminate the buffer
    *write_ptr = '\0';

    // Send the response
    send_packet(buffer);
    free(buffer);
}

void gdb_stub::_handle_memory_binary_write_packet(const char* packet) const {
    // Convert the entire packet to a `kstl::string` for parsing
    kstl::string packet_str(packet);

    // Parse: "X<address>,<length>:<data>"
    size_t comma_pos = packet_str.find(',');
    size_t colon_pos = packet_str.find(':');

    // Validate positions
    if (comma_pos == kstl::string::npos || colon_pos == kstl::string::npos || colon_pos < comma_pos) {
        _send_error(RESPONSE_ERROR_GENERIC); // Malformed packet
        return;
    }

    // Extract and parse the address and length
    kstl::string address_string = packet_str.substring(1, comma_pos - 1);
    kstl::string length_string = packet_str.substring(comma_pos + 1, colon_pos - comma_pos - 1);
    kstl::string binary_data = packet_str.substring(colon_pos + 1);

    uint64_t address = 0, length = 0;
    if (parse_hex64(address_string.c_str(), address) < 0 || parse_hex64(length_string.c_str(), length) < 0) {
        _send_error(RESPONSE_ERROR_GENERIC); // Invalid address or length
        return;
    }

    // Validate the length of the binary data
    if (binary_data.length() < length) {
        _send_error(RESPONSE_ERROR_GENERIC); // Incomplete data
        return;
    }

    // Perform the memory write
    if (!_write_binary_data_to_memory(address, binary_data.c_str(), length)) {
        _dbg_log("Failed to write binary data to memory at 0x%llx\n", address);
        _send_error(RESPONSE_ERROR_GENERIC); // Memory write failed
        return;
    }

    _dbg_log("Binary data written to memory at 0x%llx, length %llu\n", address, length);
    _send_ok();
}

void gdb_stub::_handle_memory_write_packet(const char* packet) const {
    // Convert the entire packet to a `kstl::string` for parsing
    kstl::string packet_str(packet);

    // Parse: "M<address>,<length>:<data>"
    size_t comma_pos = packet_str.find(',');
    size_t colon_pos = packet_str.find(':');

    // Validate positions
    if (comma_pos == kstl::string::npos || colon_pos == kstl::string::npos || colon_pos < comma_pos) {
        _send_error(RESPONSE_ERROR_GENERIC); // Malformed packet
        return;
    }

    // Extract address, length, and data
    kstl::string address_string = packet_str.substring(1, comma_pos - 1);
    kstl::string length_string = packet_str.substring(comma_pos + 1, colon_pos - comma_pos - 1);
    kstl::string hex_data = packet_str.substring(colon_pos + 1);

    uint64_t address = 0, length = 0;
    if (parse_hex64(address_string.c_str(), address) < 0 || parse_hex64(length_string.c_str(), length) < 0) {
        _send_error(RESPONSE_ERROR_GENERIC); // Invalid address or length
        return;
    }

    // Validate the hex-encoded data length
    if (hex_data.length() != length * 2) { // Each byte is represented by 2 hex characters
        _send_error(RESPONSE_ERROR_GENERIC); // Data length mismatch
        return;
    }

    // Decode hex-encoded data into a temporary buffer
    char* decoded_data = reinterpret_cast<char*>(zmalloc(length));
    if (decoded_data == nullptr) {
        _send_error(RESPONSE_ERROR_GENERIC); // Allocation failure
        return;
    }

    for (size_t i = 0; i < length; ++i) {
        char high = hex_data[i * 2];
        char low = hex_data[i * 2 + 1];
        int byte = (hex_char_to_int(high) << 4) | hex_char_to_int(low);

        if (byte < 0) {
            free(decoded_data);
            _send_error(RESPONSE_ERROR_GENERIC); // Invalid hex character
            return;
        }

        decoded_data[i] = static_cast<char>(byte);
    }

    // Perform the memory write
    if (!_write_binary_data_to_memory(address, decoded_data, length)) {
        free(decoded_data);
        _dbg_log("Failed to write hex data to memory at 0x%llx\n", address);
        _send_error(RESPONSE_ERROR_GENERIC); // Memory write failed
        return;
    }

    free(decoded_data);

    _dbg_log("Memory written to 0x%llx, length %llu\n", address, length);
    _send_ok();
}

void gdb_stub::_handle_set_breakpoint_packet(const char* packet) {
    // Convert the packet to a `kstl::string`
    kstl::string packet_str(packet);

    // Parse: "Z0,<address>,1"
    size_t first_comma = packet_str.find(',');
    size_t second_comma = packet_str.find(',', first_comma + 1);

    // Validate positions
    if (first_comma == kstl::string::npos || second_comma == kstl::string::npos || second_comma < first_comma) {
        _send_error(RESPONSE_ERROR_GENERIC); // Malformed packet
        return;
    }

    // Extract the address as a string
    kstl::string address_string = packet_str.substring(first_comma + 1, second_comma - first_comma - 1);

    // Convert the address to uint64_t
    uint64_t address = 0;
    if (parse_hex64(address_string.c_str(), address) < 0) {
        _send_error(RESPONSE_ERROR_GENERIC); // Invalid address
        return;
    }

    // Check if a breakpoint is already set
    if (m_breakpoints.find(address) && m_breakpoints.get(address)->active) {
        _dbg_log("Failed to set breakpoint at 0x%llx, breakpoint already active\n", address);
        _send_error(RESPONSE_ERROR_ALREADY_SET);
        return;
    }

    // Check if the address is mapped
    if (!_is_virtual_address_mapped(address)) {
        _dbg_log("Failed to set breakpoint at 0x%llx, address not mapped\n", address);
        _send_error(RESPONSE_ERROR_GENERIC);
        return;
    }

    // Set the breakpoint
    breakpoint bp{};
    bp.address = address;
    bp.original_byte = *reinterpret_cast<const uint8_t*>(address);
    bp.active = true;

    m_breakpoints.insert(address, bp);

    // Overwrite the original byte with the `int3` opcode (0xCC)
    *reinterpret_cast<uint8_t*>(address) = 0xCC;

    _dbg_log("Breakpoint set at 0x%llx\n", address);
    _send_ok();
}

void gdb_stub::_handle_clear_breakpoint_packet(const char* packet) {
    // Convert the packet to a `kstl::string`
    kstl::string packet_str(packet);

    // Parse: "z0,<address>,1"
    size_t first_comma = packet_str.find(',');
    size_t second_comma = packet_str.find(',', first_comma + 1);

    // Validate positions
    if (first_comma == kstl::string::npos || second_comma == kstl::string::npos || second_comma < first_comma) {
        _send_error(RESPONSE_ERROR_GENERIC); // Malformed packet
        return;
    }

    // Extract the address as a string
    kstl::string address_string = packet_str.substring(first_comma + 1, second_comma - first_comma - 1);

    // Convert the address to uint64_t
    uint64_t address = 0;
    if (parse_hex64(address_string.c_str(), address) < 0) {
        _send_error(RESPONSE_ERROR_GENERIC); // Invalid address
        return;
    }

    // Check if the breakpoint exists
    if (!m_breakpoints.find(address)) {
        _dbg_log("No active breakpoint at 0x%llx to remove\n", address);
        _send_error(RESPONSE_ERROR_GENERIC); // No breakpoint set
        return;
    }

    breakpoint* bp = m_breakpoints.get(address);

    // Check if the breakpoint is active
    if (!bp->active) {
        _dbg_log("Breakpoint at 0x%llx is not active\n", address);
        _send_error(RESPONSE_ERROR_GENERIC); // Inactive breakpoint
        return;
    }

    // Restore the original byte at the breakpoint address
    *reinterpret_cast<uint8_t*>(bp->address) = bp->original_byte;

    // Mark the breakpoint as inactive
    bp->active = false;

    _dbg_log("Breakpoint removed at 0x%llx\n", address);
    _send_ok();
}
