#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define KBASE_IOCTL_TYPE 0x80
struct kbase_ioctl_version_check { uint16_t major; uint16_t minor; };
#define KBASE_IOCTL_VERSION_CHECK _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
struct kbase_ioctl_set_flags { uint32_t create_flags; };
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
struct kbase_ioctl_get_gpuprops { uint64_t buffer; uint32_t size; uint32_t flags; };
#define KBASE_IOCTL_GET_GPUPROPS _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)

int main() {
    int fd = open("/dev/mali0", O_RDWR);
    struct kbase_ioctl_version_check ver = {0,0};
    ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver);
    struct kbase_ioctl_set_flags sf = {0};
    ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf);
    
    struct kbase_ioctl_get_gpuprops gp = {0, 0, 0};
    int size = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
    printf("gpuprops size: %d\n", size);
    if (size > 0) {
        uint8_t buf[size];
        gp.buffer = (uint64_t)(uintptr_t)buf;
        gp.size = size;
        ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
        /* Print first few props */
        uint32_t *p = (uint32_t*)buf;
        for (int i = 0; i < size/4 && i < 32; i += 2) {
            printf("prop 0x%04x = 0x%08x (%u)\n", p[i], p[i+1], p[i+1]);
        }
    }
    close(fd);
    return 0;
}
