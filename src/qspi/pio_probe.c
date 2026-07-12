#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
typedef uint32_t uint32_t_alias;
#include "rp1_pio_if.h"

int main(void) {
    int fd = open("/dev/pio0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }

    struct rp1_pio_sm_claim_args a;
    for (int sm = 0; sm < 4; sm++) {
        a.mask = 1u << sm;
        errno = 0;
        int r = ioctl(fd, PIO_IOC_SM_IS_CLAIMED, &a);
        printf("sm%d IS_CLAIMED -> ret=%d errno=%s\n", sm, r, errno ? strerror(errno) : "0");
    }
    // 個別クレームを試す
    for (int sm = 0; sm < 4; sm++) {
        a.mask = 1u << sm;
        errno = 0;
        int r = ioctl(fd, PIO_IOC_SM_CLAIM, &a);
        printf("sm%d CLAIM       -> ret=%d errno=%s\n", sm, r, errno ? strerror(errno) : "0");
        if (r >= 0) {
            a.mask = 1u << sm;
            ioctl(fd, PIO_IOC_SM_UNCLAIM, &a);
        }
    }
    // 任意クレーム(mask=0)
    a.mask = 0;
    errno = 0;
    int r = ioctl(fd, PIO_IOC_SM_CLAIM, &a);
    printf("any CLAIM        -> ret=%d errno=%s\n", r, errno ? strerror(errno) : "0");
    close(fd);
    return 0;
}
