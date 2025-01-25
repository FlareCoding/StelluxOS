#ifndef GDB_STUB_H
#define GDB_STUB_H
#include <process/ptregs.h>
#include <core/string.h>
#include <memory/memory.h>
#include <kstl/hashmap.h>

class gdb_stub {
public:
    static kstl::shared_ptr<gdb_stub> get();
    static kstl::shared_ptr<gdb_stub> create(uint16_t serial_port_base);

    static void perform_initial_trap();

    explicit gdb_stub(uint16_t serial_port_base);

    void run_handler(ptregs* trap_frame);

    int read_packet();

    void handle_packet(int packet_len);

    void send_packet(const char* payload);

private:
    static kstl::shared_ptr<gdb_stub> s_global_kernel_stub;

    bool        m_initialized;
    bool        m_logging_enabled;
    uint16_t    m_serial_port_base;
    ptregs*     m_trap_frame;

    bool        m_should_exit_handler;

    // Buffer to store incoming packet data (without '$'/'#xx').
    static const size_t PACKET_BUF_SIZE = 512;
    char m_incoming_buf[PACKET_BUF_SIZE];

    // Represents a single breakpoint.
    struct breakpoint {
        uint64_t address;
        uint8_t original_byte;
        bool active;
    };

    // address --> breakpoint struct
    kstl::hashmap<uint64_t, breakpoint> m_breakpoints;

    // Reads one byte from serial.
    char read_byte();

    // Writes one byte to serial.
    void write_byte(char c);

    // Sends '+' or '-' ack.
    void send_ack(bool good);

private:
    void build_g_packet(char* out_buf, size_t buf_size);
    void build_m_packet(const char* pkt, char* out_buf, size_t out_buf_size);

    void handle_X_packet(const kstl::string& packet);
    void handle_M_packet(const kstl::string& packet);
    void handle_Z0_packet(const kstl::string& packet);
    void handle_z0_packet(const kstl::string& packet);
};

#endif // GDB_STUB_H
