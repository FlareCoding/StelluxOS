#ifndef XHCI_DEVICE_H
#define XHCI_DEVICE_H

#include "xhci_device_ctx.h"

class xhci_device_endpoint_descriptor {
public:
    xhci_device_endpoint_descriptor() = default;
    xhci_device_endpoint_descriptor(uint8_t slot_id, usb_endpoint_descriptor* desc);
    ~xhci_device_endpoint_descriptor();

    uint8_t             slot_id;
    uint8_t             endpoint_num;
    uint8_t             endpoint_type;
    uint16_t            max_packet_size;
    uint8_t             interval;
    uint8_t*            data_buffer;

    kstl::shared_ptr<xhci_transfer_ring>  transfer_ring;
};

class xhci_device {
public:
    xhci_device() = default;
    ~xhci_device() = default;

    uint8_t port_reg_set;   // Port index of the port register sets (0-based)
    uint8_t port_number;    // Port number (1-based)
    uint8_t speed;          // Speed of the port to which device is connected
    uint8_t slot_id;        // Slot index into device context base address array

    // Primary configuration interface
    uint8_t primary_interface;

    // Device type identification fields
    uint8_t interface_class;
    uint8_t interface_sub_class;
    uint8_t interface_protocol;
    
    // Device-specific endpoints specified in the configuration/endpoint descriptors
    kstl::vector<xhci_device_endpoint_descriptor*> endpoints;
    
    // Driver responsible for handling this device
    // iusb_device_driver* usb_device_driver = nullptr;

    void allocate_input_context(bool use64byte_contexts);
    uint64_t get_input_context_physical_base();

    void allocate_control_endpoint_transfer_ring();

    __force_inline__ xhci_transfer_ring* get_control_endpoint_transfer_ring() { 
        return m_control_endpoint_transfer_ring.get();
    }

    xhci_input_control_context32* get_input_control_context(bool use_64byte_contexts);
    xhci_slot_context32* get_input_slot_context(bool use_64byte_contexts);
    xhci_endpoint_context32* get_input_control_endpoint_context(bool use_64byte_contexts);
    xhci_endpoint_context32* get_input_endpoint_context(bool use_64byte_contexts, uint8_t endpoint_id);

    void copy_output_device_context_to_input_device_context(bool use_64byte_contexts, void* output_device_context);

private:
    void* m_input_context = nullptr;
    kstl::shared_ptr<xhci_transfer_ring> m_control_endpoint_transfer_ring;
};

#endif // XHCI_DEVICE_H
