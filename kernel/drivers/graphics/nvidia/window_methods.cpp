// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/window_methods.h"
#include "drivers/graphics/nvidia/display_light.h"

namespace drivers::nvidia {

// NVIDIA's composition depth for a head's main layer, the most layers a head has (NVKMS_MAX_LAYERS_PER_HEAD) less the
// main layer's index, 0
constexpr uint32_t MAIN_LAYER_DEPTH = 8;

// The input look-up table's control: read directly by a color's top 10 bits, its size counting the header's entries
constexpr uint32_t INPUT_TABLE_SIZE    = LOOK_UP_TABLE_HEADER_ENTRIES + LOOK_UP_TABLE_ENTRIES;
constexpr uint32_t INPUT_TABLE_CONTROL = rm::INPUT_TABLE_MODE_DIRECT10 | INPUT_TABLE_SIZE << rm::INPUT_TABLE_SIZE_SHIFT;

namespace {

// The methods written so far into a window image's methods
struct method_writer {
    window_image_methods& out;
    uint32_t              count;
};

} // namespace

// Appends a method of one value with no bound check, as `WINDOW_IMAGE_METHODS` counts every method the builder appends
static void append(method_writer& writer, uint32_t method, uint32_t value) {
    writer.out.methods[writer.count] = {.method = method, .values = {value, 0}, .count = 1};
    writer.count++;
}

// Appends the surface's six slots in NVIDIA's order, each eye's planes in turn, each slot's context DMA before its
// offset. Only the left eye's first plane holds the surface, at offset 0
static void append_surface_slots(method_writer& writer, rm::handle surface_context_dma) {
    for (uint32_t eye = 0; eye < rm::SURFACE_EYES_MAX; eye++) {
        for (uint32_t plane = 0; plane < rm::SURFACE_PLANES_MAX; plane++) {
            uint32_t slot = eye + plane * rm::SURFACE_EYES_MAX;
            rm::handle context_dma = slot == 0 ? surface_context_dma : 0;

            append(writer, rm::WINDOW_SET_SURFACE_CONTEXT_DMA + slot * 4, context_dma);
            append(writer, rm::WINDOW_SET_SURFACE_OFFSET + slot * 4, 0);
        }
    }
}

// Appends the surface's size, its storage as rows of pixels and each plane's pitch, only plane 0 holding pixels
static void append_surface_layout(method_writer& writer, const window_image& image) {
    uint32_t size = image.width | image.height << rm::SIZE_HEIGHT_SHIFT;

    append(writer, rm::WINDOW_SET_SIZE, size);
    append(writer, rm::WINDOW_SET_SIZE_IN, size);
    append(writer, rm::WINDOW_SET_SIZE_OUT, size);
    append(writer, rm::WINDOW_SET_STORAGE, rm::STORAGE_ROWS_OF_PIXELS);

    for (uint32_t plane = 0; plane < rm::SURFACE_PLANES_MAX; plane++) {
        uint32_t pitch = plane == 0 ? image.pitch / rm::PLANE_PITCH_UNIT_BYTES : 0;
        append(writer, rm::WINDOW_SET_PLANE_STORAGE + plane * 4, pitch);
    }
}

// Appends no timestamp to wait for, its two halves in one method as NVIDIA writes them
static void append_no_timestamp(method_writer& writer) {
    writer.out.methods[writer.count] = {.method = rm::WINDOW_SET_UPDATE_TIMESTAMP, .values = {0, 0}, .count = 2};
    writer.count++;
}

// Appends the color pipeline from the format matrix, the identity, to the composition, the window's pixels opaque
static void append_color_pipeline(method_writer& writer, rm::handle look_up_table_context_dma) {
    for (uint32_t row = 0; row < rm::FORMAT_MATRIX_ROWS; row++) {
        for (uint32_t column = 0; column < rm::FORMAT_MATRIX_COLUMNS; column++) {
            uint32_t coefficient = row * rm::FORMAT_MATRIX_COLUMNS + column;
            uint32_t value = column == row ? rm::FORMAT_MATRIX_ONE : 0;

            append(writer, rm::WINDOW_SET_FORMAT_MATRIX + coefficient * 4, value);
        }
    }

    append(writer, rm::WINDOW_SET_INPUT_SCALER_CONTROL, rm::INPUT_SCALER_TWO_TAPS);
    append(writer, rm::WINDOW_SET_CSC11_CONTROL, 0);

    append(writer, rm::WINDOW_SET_INPUT_TABLE_CONTROL, INPUT_TABLE_CONTROL);
    append(writer, rm::WINDOW_SET_INPUT_TABLE_OFFSET, 0);
    append(writer, rm::WINDOW_SET_INPUT_TABLE_CONTEXT_DMA, look_up_table_context_dma);

    append(writer, rm::WINDOW_SET_COMPOSITION_CONTROL, MAIN_LAYER_DEPTH << rm::COMPOSITION_DEPTH_SHIFT);
    append(writer, rm::WINDOW_SET_COMPOSITION_CONSTANT_ALPHA, 0);
    append(writer, rm::WINDOW_SET_COMPOSITION_FACTORS, rm::COMPOSITION_FACTORS_OPAQUE);
}

// Appends no semaphore to wait on, then none to write, and the update with its interlock flags
static void append_update(method_writer& writer, uint8_t window, bool interlocks_core) {
    uint32_t core_interlock = interlocks_core ? rm::WINDOW_INTERLOCK_WITH_CORE : 0;

    append(writer, rm::WINDOW_SET_ACQUIRE_SEMAPHORE_CONTEXT_DMA, 0);
    append(writer, rm::WINDOW_SET_ACQUIRE_SEMAPHORE_CONTROL, 0);
    append(writer, rm::WINDOW_SET_ACQUIRE_SEMAPHORE_VALUE, 0);
    append(writer, rm::WINDOW_SET_SEMAPHORE_CONTEXT_DMA, 0);
    append(writer, rm::WINDOW_SET_SEMAPHORE_CONTROL, 0);
    append(writer, rm::WINDOW_SET_SEMAPHORE_RELEASE, 0);

    append(writer, rm::WINDOW_SET_INTERLOCK_FLAGS, core_interlock);
    append(writer, rm::WINDOW_SET_WINDOW_INTERLOCK_FLAGS, 1u << window);
    append(writer, rm::WINDOW_UPDATE, rm::WINDOW_UPDATE_RELEASE_ELV);
}

void build_window_image_methods(
    const window_image& image,
    const window_image_update& update,
    window_image_methods& out
) {
    uint32_t notifier_control = 0;
    if (update.notifier_context_dma != 0) {
        notifier_control = update.notifier_slot << rm::NOTIFIER_CONTROL_OFFSET_SHIFT;
    }

    method_writer writer = {.out = out, .count = 0};
    append(writer, rm::WINDOW_SET_SCAN_DIRECTION, rm::SCAN_FROM_TOP_LEFT);
    append(writer, rm::WINDOW_SET_NOTIFIER_CONTEXT_DMA, update.notifier_context_dma);
    append(writer, rm::WINDOW_SET_NOTIFIER_CONTROL, notifier_control);
    append(writer, rm::WINDOW_SET_PRESENT_CONTROL, rm::PRESENT_MIN_INTERVAL_ONE_FRAME);

    append_surface_slots(writer, image.surface_context_dma);
    append_surface_layout(writer, image);
    append_no_timestamp(writer);
    append(writer, rm::WINDOW_SET_PARAMS, rm::FORMAT_X8R8G8B8);

    append_color_pipeline(writer, image.look_up_table_context_dma);
    append_update(writer, image.window, update.interlocks_core);
}

} // namespace drivers::nvidia
