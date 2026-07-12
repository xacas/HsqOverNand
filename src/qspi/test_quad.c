// x4(QSPI)の検証: Quad Data Load(32h)とFast Read Quad Output(6Bh)。
// 判定はすべて実績のあるx1読み出し(03h)を基準に行う。
//   1. クアッド書き込み → x1読み出しで一致 (スウィズル送信の検証)
//   2. クアッド読み出し → x1読み出しと一致 (スウィズル受信の検証)
//   3. 二重書き込み=AND特性がクアッド書き込みでも成立
//   4. U2/U3交互動作 (ワイドOUTによる他チップのラッチ破壊がないこと)
//   5. 速度計測 (クアッド書き込み・読み出し vs x1)
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "w25n_pio.h"

#define CLKDIV 10.0f
#define PAGE 256          /* ブロック4。他テストと衝突しない消去単位 */
#define LEN 2048

static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t x = seed | 1;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;   /* xorshift32 */
        buf[i] = (uint8_t)x;
    }
}

static int check(const char *name, const char *what,
                 const uint8_t *got, const uint8_t *want, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (got[i] != want[i]) {
            printf("%s: %s NG: [%zu]=%02X (期待 %02X)\n",
                   name, what, i, got[i], want[i]);
            return -1;
        }
    }
    printf("%s: %s OK (%zuバイト一致)\n", name, what, len);
    return 0;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static uint8_t pat1[LEN], pat2[LEN], rd[LEN], rd2[LEN], exp2[LEN];

static int test_chip(const char *name, w25n_dev_t *d)
{
    uint8_t id[3];
    w25n_jedec_id(d, id);
    if (id[0] != 0xEF || id[1] != 0xAA || id[2] != 0x22) {
        printf("%s: JEDEC NG (%02X %02X %02X)\n", name, id[0], id[1], id[2]);
        return -1;
    }
    if (w25n_unprotect(d) < 0 || w25n_set_ecc(d, 0) < 0) {
        printf("%s: 保護解除/ECCオフに失敗\n", name);
        return -1;
    }

    // 1. クアッド書き込み → x1読み出し
    fill_pattern(pat1, LEN, 0x1234);
    if (w25n_block_erase(d, PAGE) < 0 || w25n_program(d, PAGE, 0, pat1, LEN) < 0)
        return -1;
    if (w25n_page_read(d, PAGE) < 0 || w25n_read_buf(d, 0, rd, LEN) < 0)
        return -1;
    if (check(name, "クアッド書き込み(32h)→x1読み出し", rd, pat1, LEN) < 0)
        return -1;

    // 2. クアッド読み出し → x1読み出しと一致
    if (w25n_quad_read_buf(d, 0, rd2, LEN) < 0)
        return -1;
    if (check(name, "クアッド読み出し(6Bh)", rd2, rd, LEN) < 0)
        return -1;

    // 3. AND特性 (消去→pat1書き込み→pat2上書き→pat1&pat2)
    fill_pattern(pat2, LEN, 0xBEEF);
    for (size_t i = 0; i < LEN; i++)
        exp2[i] = pat1[i] & pat2[i];
    if (w25n_program(d, PAGE, 0, pat2, LEN) < 0)
        return -1;
    if (w25n_page_read(d, PAGE) < 0 || w25n_read_buf(d, 0, rd, LEN) < 0)
        return -1;
    if (check(name, "クアッド二重書き込み=AND", rd, exp2, LEN) < 0)
        return -1;
    return 0;
}

int main(void)
{
    // U3=SM0を先に、U2=SM1を後に開く (どの順でも固定SM割当なので同じ)
    w25n_dev_t *u3 = w25n_open(&W25N_U3_PINS, CLKDIV);
    w25n_dev_t *u2 = w25n_open(&W25N_U2_PINS, CLKDIV);
    if (!u2 || !u3) {
        fprintf(stderr, "open失敗\n");
        return 1;
    }
    int rc = 0;
    if (test_chip("U2", u2) < 0 || test_chip("U3", u3) < 0)
        rc = 1;

    // 4. 交互動作: U2のワイドOUTスパンはU3のIO群を、U3のスパンはU2のCLKを
    //    跨ぐ。交互にクアッド書き込みして相互破壊がないことを確認
    if (rc == 0) {
        fill_pattern(pat1, LEN, 0xAAAA);
        fill_pattern(pat2, LEN, 0x5555);
        w25n_dev_t *devs[2] = { u2, u3 };
        const char *names[2] = { "U2", "U3" };
        uint8_t *pats[2] = { pat1, pat2 };
        for (int i = 0; i < 2 && rc == 0; i++)
            if (w25n_block_erase(devs[i], PAGE) < 0)
                rc = 1;
        for (int i = 0; i < 2 && rc == 0; i++)
            if (w25n_program(devs[i], PAGE, 0, pats[i], LEN) < 0)
                rc = 1;
        for (int i = 0; i < 2 && rc == 0; i++) {
            if (w25n_page_read(devs[i], PAGE) < 0 ||
                w25n_read_buf(devs[i], 0, rd, LEN) < 0 ||
                check(names[i], "交互クアッド書き込み", rd, pats[i], LEN) < 0)
                rc = 1;
        }
    }

    // 5. 速度計測 (U2、20ページ)
    if (rc == 0) {
        const int n = 20;
        double t0 = now();
        for (int i = 0; i < n; i++)
            w25n_program(u2, PAGE, 0, pat1, LEN);   /* AND上書き=消去不要 */
        double t1 = now();
        for (int i = 0; i < n; i++) {
            w25n_page_read(u2, PAGE);
            w25n_read_buf(u2, 0, rd, LEN);
        }
        double t2 = now();
        for (int i = 0; i < n; i++) {
            w25n_page_read(u2, PAGE);
            w25n_quad_read_buf(u2, 0, rd, LEN);
        }
        double t3 = now();
        printf("U2: クアッド書き込み %.1f回/秒 (%.1f KB/s)\n",
               n / (t1 - t0), n * LEN / (t1 - t0) / 1024);
        printf("U2: x1読み出し       %.1f回/秒 (%.1f KB/s)\n",
               n / (t2 - t1), n * LEN / (t2 - t1) / 1024);
        printf("U2: クアッド読み出し %.1f回/秒 (%.1f KB/s)\n",
               n / (t3 - t2), n * LEN / (t3 - t2) / 1024);
    }

    w25n_close(u2);
    w25n_close(u3);
    puts(rc == 0 ? "ALL OK" : "NG");
    return rc;
}
