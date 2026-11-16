#include "drivers/serial/uart/pl011.h"
#include "hw/cpu.h"
#include "hw/mmio.h"
#include "irq/irq.h"
#include "mm/early_mmu.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"

namespace serial::uart {

constexpr size_t REGISTER_BLOCK_SIZE = 0x1000;

// Register offsets from base (same for all PL011 implementations)
constexpr uintptr_t REG_DR = 0x00;    // Data register
constexpr uintptr_t REG_FR = 0x18;    // Flag register
constexpr uintptr_t REG_IBRD = 0x24;  // Integer baud rate divisor
constexpr uintptr_t REG_FBRD = 0x28;  // Fractional baud rate divisor
constexpr uintptr_t REG_LCR = 0x2C;   // Line control register
constexpr uintptr_t REG_CR = 0x30;    // Control register
constexpr uintptr_t REG_IMSC = 0x38;  // Interrupt mask set/clear
constexpr uintptr_t REG_MIS = 0x40;   // Masked interrupt status
constexpr uintptr_t REG_ICR = 0x44;   // Interrupt clear register

// Data register bits
constexpr uint32_t DR_DATA = 0xFF; // Received byte, without its error flags

// Flag register bits
constexpr uint32_t FR_RXFE = (1 << 4); // Receive FIFO empty
constexpr uint32_t FR_TXFF = (1 << 5); // Transmit FIFO full

// IMSC / MIS / ICR bits
constexpr uint32_t INT_RX      = (1 << 4); // Receive interrupt
constexpr uint32_t INT_RT      = (1 << 6); // Receive timeout interrupt

// Control register bits
constexpr uint32_t CR_UARTEN = (1 << 0); // UART enable
constexpr uint32_t CR_TXE = (1 << 8);    // Transmit enable
constexpr uint32_t CR_RXE = (1 << 9);    // Receive enable

// Line control register bits
constexpr uint32_t LCR_PEN = (1 << 1);   // Parity enable
constexpr uint32_t LCR_EPS = (1 << 2);   // Even parity
constexpr uint32_t LCR_STP2 = (1 << 3);  // Two stop bits
constexpr uint32_t LCR_FEN = (1 << 4);   // Enable FIFOs
constexpr uint32_t LCR_WLEN_SHIFT = 5;   // Word length

// The divisor registers hold a divisor from 1 to 65535 as its integer part and its fraction in 64ths
constexpr uint32_t FBRD_BITS = 6;
constexpr uint32_t FBRD_MASK = (1u << FBRD_BITS) - 1;
constexpr uint32_t DIVISOR_MIN = 1u << FBRD_BITS;
constexpr uint32_t DIVISOR_MAX = 0xFFFFu << FBRD_BITS;

__PRIVILEGED_CODE static void handle_pl011_interrupt(uint32_t, void* context) {
    static_cast<pl011*>(context)->handle_receive_interrupt();
}

int32_t pl011::divisor_for(uint32_t clock_hz, uint32_t baud) {
    if (baud == 0) {
        return ERR_UNSUPPORTED;
    }

    uint64_t divisor = baud_divisor(clock_hz, baud, FBRD_BITS);

    // Rounding to 64ths keeps any divisor of 1 or more within 1 percent of `baud`, so only the range can refuse it
    bool fits_registers = divisor >= DIVISOR_MIN && divisor <= DIVISOR_MAX;
    if (!fits_registers) {
        return ERR_UNSUPPORTED;
    }

    return static_cast<int32_t>(divisor);
}

int32_t pl011::line_control_for(const settings& line) {
    bool has_data_bits = line.data_bits >= DATA_BITS_MIN && line.data_bits <= DATA_BITS_MAX;
    bool has_stop_bits = line.stop_bits == 1 || line.stop_bits == 2;
    if (!has_data_bits || !has_stop_bits) {
        return ERR_UNSUPPORTED;
    }

    uint32_t word_length = static_cast<uint32_t>(line.data_bits - DATA_BITS_MIN) << LCR_WLEN_SHIFT;
    uint32_t stop_bits = line.stop_bits == 2 ? LCR_STP2 : 0;
    uint32_t parity_enable = line.parity_mode != parity::none ? LCR_PEN : 0;
    uint32_t parity_even = line.parity_mode == parity::even ? LCR_EPS : 0;

    return static_cast<int32_t>(LCR_FEN | word_length | stop_bits | parity_enable | parity_even);
}

__PRIVILEGED_CODE int32_t pl011::init(const settings& line) {
    int32_t divisor = divisor_for(m_clock_hz, line.baud);
    int32_t line_control = line_control_for(line);
    if (divisor < 0 || line_control < 0) {
        return ERR_UNSUPPORTED;
    }

    // Map UART if not already mapped
    if (m_base == 0) {
        if (early_mmu::init() != early_mmu::OK) {
            return ERR_NO_DEVICE;
        }

        m_base = early_mmu::map_device(m_phys, REGISTER_BLOCK_SIZE);
        if (m_base == 0) {
            return ERR_NO_DEVICE;
        }
    }

    // Disable UART while configuring
    mmio::write32(m_base + REG_CR, 0);

    // Disable all interrupts
    mmio::write32(m_base + REG_IMSC, 0);

    mmio::write32(m_base + REG_IBRD, static_cast<uint32_t>(divisor) >> FBRD_BITS);
    mmio::write32(m_base + REG_FBRD, static_cast<uint32_t>(divisor) & FBRD_MASK);
    mmio::write32(m_base + REG_LCR, static_cast<uint32_t>(line_control));

    // Enable UART, TX, and RX
    mmio::write32(m_base + REG_CR, CR_UARTEN | CR_TXE | CR_RXE);

    return OK;
}

__PRIVILEGED_CODE int32_t pl011::remap_registers() {
    uintptr_t kva_base = 0;
    uintptr_t kva_va = 0;
    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(m_phys),
        REGISTER_BLOCK_SIZE,
        paging::PAGE_KERNEL_RW,
        kva_base, kva_va);
    if (rc != vmm::OK) {
        return ERR_NO_DEVICE;
    }

    m_base = kva_va;

    return OK;
}

__PRIVILEGED_CODE int32_t pl011::enable_receive_interrupt(receive_callback on_receive, void* context) {
    int32_t rc = irq::register_handler(m_irq, handle_pl011_interrupt, this);
    if (rc != irq::OK) {
        return ERR_UNSUPPORTED;
    }

    m_on_receive = on_receive;
    m_receive_context = context;

    if (m_prepare_receive_interrupt) {
        m_prepare_receive_interrupt(m_irq);
    }

    irq::unmask(m_irq);
    mmio::write32(m_base + REG_IMSC, INT_RX | INT_RT);

    return OK;
}

__PRIVILEGED_CODE void pl011::handle_receive_interrupt() {
    uint32_t mis = mmio::read32(m_base + REG_MIS);

    if (mis & (INT_RX | INT_RT)) {
        while ((mmio::read32(m_base + REG_FR) & FR_RXFE) == 0) {
            uint8_t byte = static_cast<uint8_t>(mmio::read32(m_base + REG_DR) & DR_DATA);
            if (m_on_receive) {
                m_on_receive(m_receive_context, byte);
            }
        }

        if (mis & INT_RT) {
            mmio::write32(m_base + REG_ICR, INT_RT);
        }
    }
}

__PRIVILEGED_CODE void pl011::write(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        // Wait for transmit FIFO to have space
        while ((mmio::read32(m_base + REG_FR) & FR_TXFF) != 0) {
            cpu::pause();
        }

        mmio::write32(m_base + REG_DR, static_cast<uint32_t>(data[i]));
    }
}

} // namespace serial::uart
