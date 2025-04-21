#include <drivers/usb/xhci/xhci_usb_hid_kbd_driver.h>
#include <serial/serial.h>

// Global variables to track arrow key states
bool g_arrow_up_pressed = false;
bool g_arrow_down_pressed = false;
bool g_arrow_left_pressed = false;
bool g_arrow_right_pressed = false;

static constexpr uint8_t KEY_UP = 0x52;
static constexpr uint8_t KEY_DOWN = 0x51;
static constexpr uint8_t KEY_LEFT = 0x50;
static constexpr uint8_t KEY_RIGHT = 0x4F;

// Max keys tracked in standard HID boot protocol
constexpr size_t MAX_KEYS = 6;

// HID usage to ASCII map entry
struct hid_keymap_entry {
    uint8_t usage;
    char normal;
    char shifted;
};

static constexpr hid_keymap_entry HID_KEYMAP[] = {
    {0x04, 'a', 'A'}, {0x05, 'b', 'B'}, {0x06, 'c', 'C'}, {0x07, 'd', 'D'},
    {0x08, 'e', 'E'}, {0x09, 'f', 'F'}, {0x0A, 'g', 'G'}, {0x0B, 'h', 'H'},
    {0x0C, 'i', 'I'}, {0x0D, 'j', 'J'}, {0x0E, 'k', 'K'}, {0x0F, 'l', 'L'},
    {0x10, 'm', 'M'}, {0x11, 'n', 'N'}, {0x12, 'o', 'O'}, {0x13, 'p', 'P'},
    {0x14, 'q', 'Q'}, {0x15, 'r', 'R'}, {0x16, 's', 'S'}, {0x17, 't', 'T'},
    {0x18, 'u', 'U'}, {0x19, 'v', 'V'}, {0x1A, 'w', 'W'}, {0x1B, 'x', 'X'},
    {0x1C, 'y', 'Y'}, {0x1D, 'z', 'Z'},

    {0x1E, '1', '!'}, {0x1F, '2', '@'}, {0x20, '3', '#'}, {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '^'}, {0x24, '7', '&'}, {0x25, '8', '*'},
    {0x26, '9', '('}, {0x27, '0', ')'},

    {0x28, '\n', '\n'}, {0x2A, '\b', '\b'}, {0x2C, ' ', ' '}, {0x2D, '-', '_'},
    {0x2E, '=', '+'}, {0x2F, '[', '{'},  {0x30, ']', '}'}, {0x31, '\\', '|'},
    {0x33, ';', ':'}, {0x34, '\'', '"'}, {0x35, '`', '~'}, {0x36, ',', '<'},
    {0x37, '.', '>'}, {0x38, '/', '?'}
};

static char translate_hid_usage_to_ascii(uint8_t usage_id, uint32_t modifiers) {
    bool shift = (modifiers & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) != 0;

    for (const auto& entry : HID_KEYMAP) {
        if (entry.usage == usage_id) {
            return shift ? entry.shifted : entry.normal;
        }
    }

    return '?'; // Unknown
}

void xhci_usb_hid_kbd_driver::on_device_init() {
    memset(m_prev_keys, 0, sizeof(m_prev_keys));
}

void xhci_usb_hid_kbd_driver::on_device_event(uint8_t* data) {
    bool new_arrow_up_pressed = false;
    bool new_arrow_down_pressed = false;
    bool new_arrow_left_pressed = false;
    bool new_arrow_right_pressed = false;

    for (int i = 2; i < 8; i++) { // Check the key press array
        switch (data[i]) {
            case KEY_UP:    new_arrow_up_pressed = true; break;
            case KEY_DOWN:  new_arrow_down_pressed = true; break;
            case KEY_LEFT:  new_arrow_left_pressed = true; break;
            case KEY_RIGHT: new_arrow_right_pressed = true; break;
            default: break;
        }
    }

    // Update global states
    g_arrow_up_pressed = new_arrow_up_pressed;
    g_arrow_down_pressed = new_arrow_down_pressed;
    g_arrow_left_pressed = new_arrow_left_pressed;
    g_arrow_right_pressed = new_arrow_right_pressed;

    const uint8_t* current_keys = &data[2];
    uint8_t modifier_byte = data[0];

    _process_input_report(current_keys, modifier_byte);
}

void xhci_usb_hid_kbd_driver::_initialize_input_field(
    hid::hid_report_layout& layout, 
    uint16_t usage_page, uint16_t usage, 
    uint16_t& offset, uint16_t& size, 
    const char* field_name
) {
    __unused layout;
    __unused usage_page;
    __unused usage;
    __unused offset;
    __unused size;
    __unused field_name;
}

void xhci_usb_hid_kbd_driver::_process_input_report(
    const uint8_t* current_keys, uint8_t modifier_byte
) {
    uint32_t modifiers = static_cast<uint32_t>(modifier_byte);

    // --- Handle Key Presses ---
    for (size_t i = 0; i < MAX_KEYS; ++i) {
        uint8_t key = current_keys[i];
        if (key == 0) continue;

        bool was_previously_pressed = false;
        for (size_t j = 0; j < MAX_KEYS; ++j) {
            if (m_prev_keys[j] == key) {
                was_previously_pressed = true;
                break;
            }
        }

        if (!was_previously_pressed) {
            _emit_key_event(key, input::KBD_EVT_KEY_PRESSED, modifiers);
        }
    }

    // --- Handle Key Releases ---
    for (size_t i = 0; i < MAX_KEYS; ++i) {
        uint8_t key = m_prev_keys[i];
        if (key == 0) continue;

        bool is_still_pressed = false;
        for (size_t j = 0; j < MAX_KEYS; ++j) {
            if (current_keys[j] == key) {
                is_still_pressed = true;
                break;
            }
        }

        if (!is_still_pressed) {
            _emit_key_event(key, input::KBD_EVT_KEY_RELEASED, modifiers);
        }
    }

    // Update previous keys buffer
    memcpy(m_prev_keys, current_keys, MAX_KEYS);
}

void xhci_usb_hid_kbd_driver::_emit_key_event(
    uint8_t key, input::input_event_type type, uint32_t modifiers
) {
    input::input_event_t evt{};
    evt.id     = 0;
    evt.type   = type;
    evt.udata1 = modifiers;
    evt.sdata1 = translate_hid_usage_to_ascii(key, modifiers);

    auto& input_manager = input::system_input_manager::get();
    input_manager.push_event(INPUT_QUEUE_ID_KBD, evt);
}
