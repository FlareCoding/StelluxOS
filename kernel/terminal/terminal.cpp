#include "terminal/terminal.h"
#include "terminal/console_node.h"
#include "terminal/line_discipline.h"
#include "common/ring_buffer.h"
#include "arch/arch_uart.h"
#include "drivers/serial/uart/ns16550.h"
#include "hw/ioport.h"
#include "pci/pci.h"
#include "pci/pci_class_codes.h"
#include "resource/resource.h"
#include "signals/signal.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "sync/atomic.h"
#include "sync/poll.h"

namespace terminal {

constexpr size_t INPUT_RING_CAPACITY = 4096;
static_assert(INPUT_RING_CAPACITY >= LD_LINE_BUF_MAX + 1,
              "the console's input ring must hold a whole cooked line and its end");

constexpr serial::uart::settings CONSOLE_UART_SETTINGS = {
    .baud        = 115200,
    .data_bits   = 8,
    .parity_mode = serial::uart::parity::none,
    .stop_bits   = 1,
};

// A PCI serial controller at COM1's legacy port is COM1 itself
constexpr uint16_t COM1_LEGACY_PORT = 0x3F8;

__PRIVILEGED_BSS static struct {
    ring_buffer* input_rb;
    line_discipline ld;
    sync::atomic<uint32_t> fg_group; // foreground process group, 0 = none
    serial::uart::device* uart; // carries the kernel log and /dev/console, nullptr until claimed
} g_console;

// The UART of the PCI serial card claim_pci_uart moves the console to
alignas(serial::uart::ns16550) __PRIVILEGED_BSS static uint8_t g_pci_card_storage[sizeof(serial::uart::ns16550)];

__PRIVILEGED_CODE static void serial_echo(void* ctx, const uint8_t* buf, size_t len) {
    (void)ctx;
    console_write(reinterpret_cast<const char*>(buf), len);
}

__PRIVILEGED_DATA static const echo_target g_serial_echo = {
    serial_echo,
    nullptr,
};

__PRIVILEGED_CODE static void console_signal_fn(void* ctx, uint32_t sig) {
    (void)ctx;
    uint32_t fg = g_console.fg_group.load_acquire();
    if (fg) {
        (void)signals::send_to_group_id(fg, sig);
    }
}

__PRIVILEGED_DATA static const signal_target g_console_sig = {
    console_signal_fn,
    nullptr,
};

__PRIVILEGED_CODE int32_t claim_builtin_uart() {
    serial::uart::device* builtin = arch::builtin_uart();
    if (builtin->init(CONSOLE_UART_SETTINGS) != serial::uart::OK) {
        return ERR;
    }

    g_console.uart = builtin;

    return OK;
}

__PRIVILEGED_CODE int32_t remap_uart() {
    if (g_console.uart->remap_registers() != serial::uart::OK) {
        return ERR;
    }

    return OK;
}

__PRIVILEGED_CODE void claim_pci_uart() {
    pci::device* card = pci::find_by_class(pci::CLASS_SIMPLE_COMM, pci::SUB_COMM_SERIAL);
    if (!card || !ioport::available()) {
        return;
    }

    const pci::bar& bar = card->get_bar(0);
    if (bar.type != pci::BAR_IO || bar.phys == 0 || bar.phys == COM1_LEGACY_PORT) {
        return;
    }

    card->enable();

    // PCI serial adapters use PCI interrupt lines, not legacy IRQ 4.
    // RX interrupt routing for PCI UARTs is not supported.
    auto* card_uart = new (g_pci_card_storage) serial::uart::ns16550(
        static_cast<uint16_t>(bar.phys),
        serial::uart::NS16550_STANDARD_CLOCK_HZ,
        nullptr
    );
    if (card_uart->init(CONSOLE_UART_SETTINGS) != serial::uart::OK) {
        log::warn("terminal: the PCI serial card does not answer, keeping the built-in UART");
        return;
    }

    g_console.uart = card_uart;
    log::info("serial: redirected to PCI adapter at %02x:%02x.%x (port 0x%x)",
              card->bus(), card->slot(), card->func(), static_cast<uint32_t>(bar.phys));
}

__PRIVILEGED_CODE void console_write(const char* data, size_t len) {
    serial::uart::device* uart = g_console.uart;
    if (!uart) {
        return;
    }

    uart->write(data, len);
}

__PRIVILEGED_CODE int32_t init() {
    g_console.input_rb = ring_buffer_create(INPUT_RING_CAPACITY);
    if (!g_console.input_rb) {
        log::error("terminal: failed to create ring buffer");
        return ERR;
    }

    ld_init(&g_console.ld);

    if (g_console.uart->enable_receive_interrupt(input_char, nullptr) != serial::uart::OK) {
        log::warn("terminal: serial RX interrupt setup failed");
    }

    void* cn_mem = heap::kzalloc(sizeof(console_node));
    if (!cn_mem) {
        log::error("terminal: failed to allocate console_node");
        return ERR;
    }

    auto* cn = new (cn_mem) console_node(nullptr, "console");
    int32_t rc = devfs::add_char_device("console", cn);
    if (rc != devfs::OK) {
        log::error("terminal: failed to register /dev/console");
        return ERR;
    }

    log::info("terminal: console initialized (cooked mode, /dev/console registered)");
    return OK;
}

__PRIVILEGED_CODE void input_char(void*, uint8_t byte) {
    ld_input(&g_console.ld, g_console.input_rb, &g_serial_echo,
             &g_console_sig, static_cast<char>(byte));
}

__PRIVILEGED_CODE ring_buffer* console_input_rb() {
    return g_console.input_rb;
}

__PRIVILEGED_CODE ssize_t console_read(uint8_t* buf, size_t len, bool nonblock) {
    return ld_read(&g_console.ld, g_console.input_rb, buf, len, nonblock);
}

__PRIVILEGED_CODE static ssize_t terminal_read(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !kdst) {
        return RB_ERR_INVAL;
    }

    auto* rb = static_cast<ring_buffer*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    return ld_read(&g_console.ld, rb, static_cast<uint8_t*>(kdst), count, nonblock);
}

__PRIVILEGED_CODE static ssize_t terminal_write(
    resource::resource_object*, const void* ksrc, size_t count, uint32_t
) {
    console_write(static_cast<const char*>(ksrc), count);
    return static_cast<ssize_t>(count);
}

static void terminal_close(resource::resource_object*) {
}

__PRIVILEGED_CODE static uint32_t terminal_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) return sync::POLL_NVAL;

    auto* rb = static_cast<ring_buffer*>(obj->impl);
    return ring_buffer_poll_read(rb, pt) | sync::POLL_OUT;
}

static const resource::resource_ops g_terminal_ops = {
    .read = terminal_read,
    .write = terminal_write,
    .close = terminal_close,
    .poll = terminal_poll,
};

const resource::resource_ops* get_terminal_ops() {
    return &g_terminal_ops;
}

int32_t set_mode(uint32_t cmd) {
    return ld_set_mode(&g_console.ld, cmd);
}

int32_t console_ioctl(uint32_t cmd, uint64_t arg) {
    // Failures use fs error codes: this sits on the /dev/console node
    // path, where the terminal ERR value would read as fs::ERR_NOENT
    if (cmd == TIOCGPGRP) {
        int32_t g = 0;
        RUN_ELEVATED({
            g = static_cast<int32_t>(g_console.fg_group.load_acquire());
        });

        return mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &g, sizeof(g)) == mm::uaccess::OK
            ? OK : fs::ERR_INVAL;
    }

    if (cmd == TIOCSPGRP) {
        int32_t g = 0;
        if (mm::uaccess::copy_from_user(
                &g, reinterpret_cast<const void*>(arg), sizeof(g)) != mm::uaccess::OK
            || g < 0) {
            return fs::ERR_INVAL;
        }

        int32_t result = OK;
        RUN_ELEVATED({
            // POSIX requires an existing process group, 0 clears the foreground
            if (g > 0 && signals::send_to_group_id(
                    static_cast<uint32_t>(g), 0) != signals::OK) {
                result = fs::ERR_INVAL;
            } else {
                g_console.fg_group.store_release(static_cast<uint32_t>(g));
            }
        });

        return result;
    }

    return set_mode(cmd);
}

} // namespace terminal
