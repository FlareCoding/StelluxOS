#ifndef GDB_STUB_H
#define GDB_STUB_H
#include <process/ptregs.h>
#include <core/string.h>
#include <memory/memory.h>
#include <kstl/hashmap.h>
#include <serial/serial.h>

class gdb_stub {
public:
    static kstl::shared_ptr<gdb_stub> get();
    static kstl::shared_ptr<gdb_stub> create(uint16_t serial_port_base);

    static void perform_initial_trap();

    explicit gdb_stub(uint16_t serial_port_base);

    void run_handler(ptregs* trap_frame);
    void handle_packet(int packet_len);

    int read_packet();
    void send_packet(const char* payload) const;

private:
    static constexpr const char* RESPONSE_OK = "OK";
    static constexpr const char* RESPONSE_ERROR_GENERIC = "E01";
    static constexpr const char* RESPONSE_ERROR_ALREADY_SET = "E02";

    enum class gdb_packet_type {
        UNKNOWN,
        QUERY_SUPPORTED,
        QUERY_ATTACHED,
        VCONT_QUERY,
        TRAP_STATUS,
        READ_REGISTERS,
        READ_MEMORY,
        SINGLE_STEP,
        SET_BREAKPOINT,
        CLEAR_BREAKPOINT,
        WRITE_MEMORY,
        BINARY_WRITE_MEMORY,
        THREAD_OP,
        CONTINUE_EXECUTION,
        DETACH
    };

    static kstl::shared_ptr<gdb_stub> s_global_kernel_stub;

    bool        m_initialized;
    bool        m_logging_enabled;
    uint16_t    m_serial_port_base;
    ptregs*     m_trap_frame;

    bool        m_should_exit_handler;

    // Buffer to store incoming packet data (without '$'/'#xx').
    static const size_t INBOUND_PACKET_BUF_SIZE = 512;
    char m_incoming_buf[INBOUND_PACKET_BUF_SIZE];

    // Represents a single breakpoint.
    struct breakpoint {
        uint64_t address;
        uint8_t original_byte;
        bool active;
    };

    // address --> breakpoint struct
    kstl::hashmap<uint64_t, breakpoint> m_breakpoints;

    gdb_packet_type _get_packet_type() const;

    // Reads one byte from serial.
    char _read_byte() const;

    // Writes one byte to serial.
    void _write_byte(char c) const;

    // Sends '+' or '-' ack.
    void send_ack(bool good);

    void _send_empty_packet() const;
    void _send_ok() const;
    void _send_error(const char* error_code) const;
    void _notify_gdb_client_of_trap_status() const;

    bool _is_virtual_address_mapped(uintptr_t vaddr) const;
    bool _write_binary_data_to_memory(uint64_t addr, const char* data, size_t length) const;

private:
    void _handle_supported_features_packet() const;
    void _handle_attached_packet();
    void _handle_vcont_query_packet() const;

    void _handle_single_step_packet();
    void _handle_continue_packet();
    void _handle_detach_packet();

    void _build_registers_packet(char* buffer, size_t buffer_size) const;
    void _handle_registers_read_packet() const; 

    void _handle_memory_read_packet(const char* packet) const;
    void _handle_memory_binary_write_packet(const char* packet) const;
    void _handle_memory_write_packet(const char* packet) const;

    void _handle_set_breakpoint_packet(const char* packet);
    void _handle_clear_breakpoint_packet(const char* packet);

private:
    template <typename... Args>
    inline void _dbg_log(const char* format, Args... args) const {
        if (m_logging_enabled) {
            serial::printf(format, args...);
        }
    }
};

#endif // GDB_STUB_H
