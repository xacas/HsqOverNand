#include "w25n_spidev.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

struct w25n_spi_dev {
    int fd;
    uint32_t speed_hz;
};

w25n_spi_dev_t *w25n_spi_open(const char *path, uint32_t speed_hz)
{
    w25n_spi_dev_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;

    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        fprintf(stderr, "w25nSpi: %sを開けない: %s\n", path, strerror(errno));
        free(d);
        return NULL;
    }

    uint8_t mode = SPI_MODE_0;   /* W25N02KVはCS立下り時のCLKでmode0/3自動判別。0固定でよい */
    uint8_t bits = 8;
    if (ioctl(d->fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(d->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(d->fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
        fprintf(stderr, "w25nSpi: %sのSPIモード設定に失敗: %s\n", path, strerror(errno));
        close(d->fd);
        free(d);
        return NULL;
    }
    d->speed_hz = speed_hz;
    return d;
}

void w25n_spi_close(w25n_spi_dev_t *d)
{
    if (!d)
        return;
    if (d->fd >= 0)
        close(d->fd);
    free(d);
}

int w25n_spi_set_speed(w25n_spi_dev_t *d, uint32_t speed_hz)
{
    if (ioctl(d->fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0)
        return -1;
    d->speed_hz = speed_hz;
    return 0;
}

int w25n_spi_cmd(w25n_spi_dev_t *d, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen)
{
    struct spi_ioc_transfer xfer[2];
    memset(xfer, 0, sizeof(xfer));
    int n = 0;

    if (txlen > 0) {
        xfer[n].tx_buf = (uintptr_t)tx;
        xfer[n].len = (uint32_t)txlen;
        xfer[n].speed_hz = d->speed_hz;
        xfer[n].bits_per_word = 8;
        n++;
    }
    if (rxlen > 0) {
        xfer[n].rx_buf = (uintptr_t)rx;
        xfer[n].len = (uint32_t)rxlen;
        xfer[n].speed_hz = d->speed_hz;
        xfer[n].bits_per_word = 8;
        n++;
    }
    if (n == 0)
        return 0;
    /* 1回のioctlに収める = CSは全セグメントを通じて連続してLow */
    if (ioctl(d->fd, SPI_IOC_MESSAGE(n), xfer) < 0) {
        fprintf(stderr, "w25nSpi: SPI転送に失敗: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int w25n_spi_jedec_id(w25n_spi_dev_t *d, uint8_t id[3])
{
    const uint8_t tx[2] = { 0x9F, 0x00 };  /* コマンド + ダミー8クロック */
    return w25n_spi_cmd(d, tx, sizeof(tx), id, 3);
}

int w25n_spi_read_status(w25n_spi_dev_t *d, uint8_t addr, uint8_t *val)
{
    const uint8_t tx[2] = { 0x0F, addr };
    return w25n_spi_cmd(d, tx, sizeof(tx), val, 1);
}

int w25n_spi_write_status(w25n_spi_dev_t *d, uint8_t addr, uint8_t val)
{
    const uint8_t tx[3] = { 0x1F, addr, val };
    return w25n_spi_cmd(d, tx, sizeof(tx), NULL, 0);
}

int w25n_spi_unprotect(w25n_spi_dev_t *d)
{
    uint8_t sr;
    w25n_spi_write_status(d, 0xA0, 0x00);
    w25n_spi_read_status(d, 0xA0, &sr);
    if (sr != 0x00) {
        fprintf(stderr, "w25nSpi: 保護解除に失敗 (SR-1=0x%02X)\n", sr);
        return -1;
    }
    return 0;
}

int w25n_spi_set_ecc(w25n_spi_dev_t *d, int enable)
{
    uint8_t sr;
    w25n_spi_read_status(d, 0xB0, &sr);
    sr = enable ? (sr | 0x10) : (sr & ~0x10);
    w25n_spi_write_status(d, 0xB0, sr);
    uint8_t chk;
    w25n_spi_read_status(d, 0xB0, &chk);
    return chk == sr ? 0 : -1;
}

static int write_enable(w25n_spi_dev_t *d)
{
    const uint8_t tx[1] = { 0x06 };
    return w25n_spi_cmd(d, tx, sizeof(tx), NULL, 0);
}

int w25n_spi_wait_busy(w25n_spi_dev_t *d, unsigned timeout_us,
                        unsigned poll_interval_us, uint8_t *sr3_out)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint8_t sr3;
        w25n_spi_read_status(d, 0xC0, &sr3);
        if (!(sr3 & 0x01)) {
            if (sr3_out)
                *sr3_out = sr3;
            return 0;
        }
        if (poll_interval_us > 0)
            usleep(poll_interval_us);
        clock_gettime(CLOCK_MONOTONIC, &t);
        unsigned long el = (t.tv_sec - t0.tv_sec) * 1000000ul +
                           (t.tv_nsec - t0.tv_nsec) / 1000;
        if (el > timeout_us) {
            if (sr3_out)
                *sr3_out = sr3;
            fprintf(stderr, "w25nSpi: BUSYタイムアウト (%uus)\n", timeout_us);
            return -1;
        }
    }
}

int w25n_spi_block_erase(w25n_spi_dev_t *d, uint32_t page)
{
    write_enable(d);
    const uint8_t tx[4] = { 0xD8, (uint8_t)(page >> 16), (uint8_t)(page >> 8), (uint8_t)page };
    w25n_spi_cmd(d, tx, sizeof(tx), NULL, 0);
    uint8_t sr3;
    if (w25n_spi_wait_busy(d, 500000, 200, &sr3) < 0)   /* tBEmax=10ms */
        return -1;
    if (sr3 & 0x04) {   /* E-FAIL */
        fprintf(stderr, "w25nSpi: Erase失敗 page=%u (SR-3=0x%02X)\n", page, sr3);
        return -1;
    }
    return 0;
}

int w25n_spi_program_load(w25n_spi_dev_t *d, uint16_t col, const uint8_t *data, size_t len)
{
    write_enable(d);
    /* コマンド+アドレスとデータ本体を1回のCSアサートで送る */
    uint8_t hdr[3] = { 0x02, (uint8_t)(col >> 8), (uint8_t)col };
    struct spi_ioc_transfer xfer[2];
    memset(xfer, 0, sizeof(xfer));
    xfer[0].tx_buf = (uintptr_t)hdr;
    xfer[0].len = sizeof(hdr);
    xfer[0].speed_hz = d->speed_hz;
    xfer[0].bits_per_word = 8;
    xfer[1].tx_buf = (uintptr_t)data;
    xfer[1].len = (uint32_t)len;
    xfer[1].speed_hz = d->speed_hz;
    xfer[1].bits_per_word = 8;
    if (ioctl(d->fd, SPI_IOC_MESSAGE(2), xfer) < 0) {
        fprintf(stderr, "w25nSpi: Data Loadに失敗: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int w25n_spi_program_issue(w25n_spi_dev_t *d, uint32_t page)
{
    const uint8_t exec[4] = { 0x10, (uint8_t)(page >> 16), (uint8_t)(page >> 8), (uint8_t)page };
    return w25n_spi_cmd(d, exec, sizeof(exec), NULL, 0);
}

int w25n_spi_program_wait(w25n_spi_dev_t *d, unsigned timeout_us, uint8_t *sr3_out)
{
    uint8_t sr3;
    if (w25n_spi_wait_busy(d, timeout_us, 50, &sr3) < 0)   /* tPPmax=700us */
        return -1;
    if (sr3_out)
        *sr3_out = sr3;
    if (sr3 & 0x08) {   /* P-FAIL */
        fprintf(stderr, "w25nSpi: Program失敗 (SR-3=0x%02X)\n", sr3);
        return -1;
    }
    return 0;
}

int w25n_spi_program(w25n_spi_dev_t *d, uint32_t page, uint16_t col, const uint8_t *data, size_t len)
{
    if (w25n_spi_program_load(d, col, data, len) < 0)
        return -1;
    if (w25n_spi_program_issue(d, page) < 0)
        return -1;
    return w25n_spi_program_wait(d, 100000, NULL);
}

int w25n_spi_page_read(w25n_spi_dev_t *d, uint32_t page)
{
    const uint8_t tx[4] = { 0x13, (uint8_t)(page >> 16), (uint8_t)(page >> 8), (uint8_t)page };
    w25n_spi_cmd(d, tx, sizeof(tx), NULL, 0);
    return w25n_spi_wait_busy(d, 100000, 20, NULL);    /* tRDmax=60us */
}

int w25n_spi_read_buf(w25n_spi_dev_t *d, uint16_t col, uint8_t *buf, size_t len)
{
    const uint8_t tx[4] = { 0x03, (uint8_t)(col >> 8), (uint8_t)col, 0x00 };  /* +8ダミークロック */
    return w25n_spi_cmd(d, tx, sizeof(tx), buf, len);
}
