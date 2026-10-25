/* Prints the Adreno chip id KGSL reports (e.g. 0x43050a01 Adreno 740, 0x44050001 Adreno 830),
 * the id Turnip matches in freedreno_devices.py. run-gport.sh picks the Turnip variant with it.
 * The structures are KGSL's ioctl ABI (msm_kgsl.h), declared here to need no kernel headers. */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct kgsl_devinfo {
    unsigned int device_id;
    unsigned int chip_id;
    unsigned int mmu_enabled;
    unsigned long gmem_gpubaseaddr;
    unsigned int gpu_id;
    size_t gmem_sizebytes;
};

struct kgsl_device_getproperty {
    unsigned int type;
    void *value;
    size_t sizebytes;
};

#define KGSL_PROP_DEVICE_INFO 0x1
#define IOCTL_KGSL_DEVICE_GETPROPERTY _IOWR(0x09, 0x2, struct kgsl_device_getproperty)

int main(void) {
    const int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("kgsl-chip-id: /dev/kgsl-3d0");
        return 1;
    }
    struct kgsl_devinfo info = {0};
    struct kgsl_device_getproperty property = {KGSL_PROP_DEVICE_INFO, &info, sizeof(info)};
    if (ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &property) != 0) {
        perror("kgsl-chip-id: device info");
        close(fd);
        return 1;
    }
    close(fd);
    printf("0x%08x\n", info.chip_id);
    return 0;
}
