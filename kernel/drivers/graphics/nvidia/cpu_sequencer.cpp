// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/cpu_sequencer.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

// Where the payload's header fields and its steps lie, in bytes from its start
constexpr uint32_t BUFFER_WORDS_OFFSET = 0;
constexpr uint32_t STEP_WORDS_OFFSET   = 4;
constexpr uint32_t SAVE_AREA_OFFSET    = 8;
constexpr uint32_t STEPS_OFFSET        = 40;

constexpr uint32_t SAVE_AREA_WORDS      = 8;
constexpr uint32_t SEQUENCER_WORD_BYTES = 4;

// NVIDIA's opcodes, each followed by the words of arguments `argument_words` gives
constexpr uint32_t OPCODE_WRITE_REGISTER    = 0;
constexpr uint32_t OPCODE_MODIFY_REGISTER   = 1;
constexpr uint32_t OPCODE_POLL_REGISTER     = 2;
constexpr uint32_t OPCODE_DELAY             = 3;
constexpr uint32_t OPCODE_STORE_REGISTER    = 4;
constexpr uint32_t OPCODE_RESET_GSP         = 5;
constexpr uint32_t OPCODE_START_GSP         = 6;
constexpr uint32_t OPCODE_WAIT_FOR_GSP_HALT = 7;
constexpr uint32_t OPCODE_RESUME_GSP        = 8;
constexpr uint32_t OPCODE_COUNT             = 9;

// The words of arguments after an opcode as NVIDIA's driver counts them, 0 for a falcon step or an opcode it does not
// know
static uint32_t argument_words(uint32_t opcode) {
    switch (opcode) {
        case OPCODE_WRITE_REGISTER:
            return 2;
        case OPCODE_MODIFY_REGISTER:
            return 3;
        case OPCODE_POLL_REGISTER:
            return 5;
        case OPCODE_DELAY:
            return 1;
        case OPCODE_STORE_REGISTER:
            return 2;
        default:
            return 0;
    }
}

// Whether a step's first argument is the offset of the register it reads or writes
static bool names_register(uint32_t opcode) {
    switch (opcode) {
        case OPCODE_WRITE_REGISTER:
        case OPCODE_MODIFY_REGISTER:
        case OPCODE_POLL_REGISTER:
        case OPCODE_STORE_REGISTER:
            return true;
        default:
            return false;
    }
}

// The step's argument at `index`, counting from 0 after its opcode
static uint32_t argument(const uint8_t* arguments, uint32_t index) {
    return read_u32(arguments + index * SEQUENCER_WORD_BYTES);
}

// Register steps reach only whole aligned words of BAR0, as an access past the mapping faults the CPU and an unaligned
// one faults on aarch64
static bool is_register_in_bar0(const cpu_sequencer_context& context, uint32_t offset) {
    return offset % SEQUENCER_WORD_BYTES == 0 && lies_inside(offset, SEQUENCER_WORD_BYTES, context.register_bytes);
}

// Polls as NVIDIA's driver does: its own timeout counts from after the first read, and the deadline is checked before
// each read after it
static int32_t poll_register(const registers& regs, const cpu_sequencer_context& context, const uint8_t* arguments) {
    uint32_t offset = argument(arguments, 0);
    uint32_t bits = argument(arguments, 1);
    uint32_t value = argument(arguments, 2);
    uint32_t timeout_us = argument(arguments, 3);
    uint32_t error_code = argument(arguments, 4);

    uint32_t read = regs.read(offset);
    uint64_t start = clock::now_ns();
    uint64_t own_deadline = start + timeout_us * NS_PER_US;
    bool ends_at_own_timeout = timeout_us != 0 && own_deadline < context.deadline_ns;
    uint64_t deadline = ends_at_own_timeout ? own_deadline : context.deadline_ns;
    while ((read & bits) != value) {
        if (clock::now_ns() >= deadline) {
            log::error(
                "nvidia: the CPU sequencer's poll of register 0x%X for 0x%X under mask 0x%X timed out after %lu us, "
                "the register reading 0x%X, the step's error code 0x%X",
                offset,
                value,
                bits,
                (clock::now_ns() - start) / NS_PER_US,
                read,
                error_code
            );
            return ERR_TIMEOUT;
        }

        cpu::relax();
        read = regs.read(offset);
    }

    return OK;
}

// As in NVIDIA's driver, the value's bits are set whole, those outside the mask included
static void modify_register(registers& regs, const uint8_t* arguments) {
    uint32_t offset = argument(arguments, 0);
    uint32_t modified = (regs.read(offset) & ~argument(arguments, 1)) | argument(arguments, 2);

    regs.write(offset, modified);
}

static void delay(uint32_t microseconds) {
    uint64_t end = clock::now_ns() + microseconds * NS_PER_US;
    while (clock::now_ns() < end) {
        cpu::relax();
    }
}

static int32_t store_register(const registers& regs, uint8_t* payload, const uint8_t* arguments, uint32_t word) {
    uint32_t index = argument(arguments, 1);
    if (index >= SAVE_AREA_WORDS) {
        log::error(
            "nvidia: the CPU sequencer's store at word %u names save area index %u, past its %u words",
            word,
            index,
            SAVE_AREA_WORDS
        );
        return ERR_BAD_MESSAGE;
    }

    write_u32(payload + SAVE_AREA_OFFSET + index * SEQUENCER_WORD_BYTES, regs.read(argument(arguments, 0)));

    return OK;
}

static int32_t reset_gsp(registers& regs, const cpu_sequencer_context& context) {
    int32_t rc = reset_falcon(
        regs,
        falcon_id::gsp,
        context.boot0,
        context.ready_timeout_ns,
        time_left_until(context.deadline_ns)
    );
    if (rc != OK) {
        return rc;
    }

    allow_falcon_dma_without_context(regs, falcon_id::gsp);

    return OK;
}

static int32_t run_step(
    registers& regs,
    const cpu_sequencer_context& context,
    uint8_t* payload,
    uint32_t opcode,
    const uint8_t* arguments,
    uint32_t word
) {
    switch (opcode) {
        case OPCODE_WRITE_REGISTER:
            regs.write(argument(arguments, 0), argument(arguments, 1));
            return OK;
        case OPCODE_MODIFY_REGISTER:
            modify_register(regs, arguments);
            return OK;
        case OPCODE_POLL_REGISTER:
            return poll_register(regs, context, arguments);
        case OPCODE_DELAY:
            delay(argument(arguments, 0));
            return OK;
        case OPCODE_STORE_REGISTER:
            return store_register(regs, payload, arguments, word);
        case OPCODE_RESET_GSP:
            return reset_gsp(regs, context);
        case OPCODE_START_GSP:
            start_falcon(regs, falcon_id::gsp);
            return OK;
        case OPCODE_WAIT_FOR_GSP_HALT:
            return wait_for_falcon_halt(regs, falcon_id::gsp, time_left_until(context.deadline_ns));
        case OPCODE_RESUME_GSP:
            return resume_gsp_firmware(
                regs,
                context.libos_arguments_address,
                context.app_version,
                context.ready_timeout_ns,
                time_left_until(context.deadline_ns)
            );
        default:
            log::error("nvidia: the CPU sequencer's step at word %u has unknown opcode %u", word, opcode);
            return ERR_BAD_MESSAGE;
    }
}

int32_t run_cpu_sequencer(
    registers& regs,
    const cpu_sequencer_context& context,
    uint8_t* payload,
    uint32_t payload_bytes
) {
    if (payload_bytes < STEPS_OFFSET) {
        log::error("nvidia: the CPU sequencer's payload of %u bytes is shorter than its header", payload_bytes);
        return ERR_BAD_MESSAGE;
    }

    uint32_t buffer_words = read_u32(payload + BUFFER_WORDS_OFFSET);
    uint32_t step_words = read_u32(payload + STEP_WORDS_OFFSET);
    uint32_t payload_words = (payload_bytes - STEPS_OFFSET) / SEQUENCER_WORD_BYTES;
    if (step_words >= buffer_words) {
        log::error(
            "nvidia: the CPU sequencer's steps take %u words, not fewer than the %u its buffer holds",
            step_words,
            buffer_words
        );
        return ERR_BAD_MESSAGE;
    }

    if (step_words > payload_words) {
        log::error(
            "nvidia: the CPU sequencer's steps take %u words, more than the %u its payload holds after its header",
            step_words,
            payload_words
        );
        return ERR_BAD_MESSAGE;
    }

    const uint8_t* steps = payload + STEPS_OFFSET;
    uint32_t step_counts[OPCODE_COUNT] = {};
    uint32_t word = 0;
    while (word < step_words) {
        uint32_t opcode = read_u32(steps + word * SEQUENCER_WORD_BYTES);
        uint32_t arguments = word + 1;
        if (argument_words(opcode) > step_words - arguments) {
            log::error("nvidia: the CPU sequencer's step at word %u runs past its steps' %u words", word, step_words);
            return ERR_BAD_MESSAGE;
        }

        const uint8_t* argument_bytes = steps + arguments * SEQUENCER_WORD_BYTES;
        if (names_register(opcode) && !is_register_in_bar0(context, argument(argument_bytes, 0))) {
            log::error(
                "nvidia: the CPU sequencer's step at word %u names register 0x%X, not a multiple of 4 or past BAR0's "
                "0x%lX bytes",
                word,
                argument(argument_bytes, 0),
                context.register_bytes
            );
            return ERR_BAD_MESSAGE;
        }

        int32_t rc = run_step(regs, context, payload, opcode, argument_bytes, word);
        if (rc != OK) {
            log::error("nvidia: the CPU sequencer stopped at its step at word %u, opcode %u: %d", word, opcode, rc);
            return rc;
        }

        step_counts[opcode]++;
        word = arguments + argument_words(opcode);
    }

    log::info(
        "nvidia: ran the CPU sequencer's %u words of steps, counted by opcode: write %u, modify %u, poll %u, delay %u, "
        "store %u, falcon reset %u, falcon start %u, halt wait %u, resume %u",
        step_words,
        step_counts[OPCODE_WRITE_REGISTER],
        step_counts[OPCODE_MODIFY_REGISTER],
        step_counts[OPCODE_POLL_REGISTER],
        step_counts[OPCODE_DELAY],
        step_counts[OPCODE_STORE_REGISTER],
        step_counts[OPCODE_RESET_GSP],
        step_counts[OPCODE_START_GSP],
        step_counts[OPCODE_WAIT_FOR_GSP_HALT],
        step_counts[OPCODE_RESUME_GSP]
    );

    return OK;
}

} // namespace drivers::nvidia
