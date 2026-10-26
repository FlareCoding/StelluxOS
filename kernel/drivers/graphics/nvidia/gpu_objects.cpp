// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/nvidia.h"

namespace drivers::nvidia {

int32_t allocate_gpu_objects(firmware& fw, gpu_objects& out) {
    rm::device_params device_params = {};
    device_params.shared_client = fw.client();

    rm::handle device = fw.new_handle();
    int32_t rc = fw.allocate(fw.client(), device, rm::CLASS_DEVICE, &device_params, sizeof(device_params));
    if (rc != OK) {
        return rc;
    }

    rm::subdevice_params subdevice_params = {};

    rm::handle subdevice = fw.new_handle();
    rc = fw.allocate(device, subdevice, rm::CLASS_SUBDEVICE, &subdevice_params, sizeof(subdevice_params));
    if (rc != OK) {
        fw.release(device);
        return rc;
    }

    out.device = device;
    out.subdevice = subdevice;

    return OK;
}

int32_t release_gpu_objects(firmware& fw, const gpu_objects& objects) {
    int32_t subdevice_rc = fw.release(objects.subdevice);
    int32_t device_rc = fw.release(objects.device);

    return subdevice_rc != OK ? subdevice_rc : device_rc;
}

} // namespace drivers::nvidia
