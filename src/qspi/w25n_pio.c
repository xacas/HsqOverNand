// W25N02KV を RP1 PIO で叩く実装。
//
// PIOプログラムは4種類を固定オフセットに常駐させる (.side_set 1 = CLK):
//
//   fdx @0 (全二重バイトモード、コマンド/ステータス用、2サイクル/bit):
//     out pins, 1   side 0   ; 立ち下がりでDIを更新
//     in  pins, 1   side 1   ; 立ち上がりでDOを取り込み
//
//   rx  @2 (x1受信専用バルク、32bitパック、8bit/ループのフリーラン。
//           必要ワード数はホストが数え、回収し終えたらSMを止める。
//           ※旧実装のjmp x--カウント方式はプログラム配置によっては
//           「7ループに1回Xが余分に減る」RP1のクセを踏む (下の制約6)):
//     { in pins,1 side 1 ; nop side 0 } ×8   (wrapで先頭へ)
//
//   qtx @18 (x4送信、Quad Data Load 32hのデータ相。1クロック=1ワード。
//            IO0..3が飛び飛びGPIOなのでout pins,22のワイドOUT+ホスト側
//            スウィズルで4bit/クロックを作る。out_base=io3=スパン最小ピン):
//     { out pins,22 side 0 ; nop side 1 } ×5   (wrapで先頭へ)
//
//   qrx @28 (x4受信、Fast Read Quad Output 6Bhのデータ相。
//            in pins,32 (in_base=0) で全GPIOを1クロック=1ワード取り込み、
//            ホスト側で4bit抽出。rx同様フリーランでホストが止める):
//     { in pins,32 side 1 ; nop side 0 }       (wrapで先頭へ)
//
// 【x4の排他制約】ワイドOUTは自チップのIO0..3以外のスパン内ピンの出力
// ラッチも上書きする (U2スパン=GPIO5..26、U3スパン=GPIO2..23)。
//   - 他チップのCLKを跨ぐ組合せ(U3→U2のCLK12)は「同一サイクルに複数SMが
//     同じピンへ書くとSM番号の大きい方が勝つ」でU2のside-setを勝たせる
//     (U2=SM1 > U3=SM0 固定割当)。
//   - qtx中に他チップへ別コマンドを流すとDI/CLKのラッチが衝突するため、
//     qtxはプロセス内で全デバイス直列 (シングルスレッド前提。将来の
//     マルチプロセス化では外部mutexが必要)。
//   - qtx後は全デバイスの/WP,/HOLDラッチをHighへ復元する (g_devsレジストリ)。
//
// 【RP1 PIOの実装上の制約 (実測、2026-07-13)】
// 1. side-set値が同じ命令が連続すると、命令境界でCLKに余分なエッジが出る
//    (例: nop side0 / in side1 / jmp side0 の3命令ループはフラッシュが
//     ループ2周あたり3ビット進む)。全プログラムで side 0/1 を毎命令交互に
//    することで回避する。fdx/tx/rxすべてこの構成。
// 2. カーネルDMA (pio_sm_xfer_data) はDREQ閾値(既定TX=4/RX=1)がDMAバースト長
//    (8要求、DMAC実力4)と不整合で、そのままでは両方向とも化ける
//    (FROM_SM: FIFO空読み、TO_SM: 溢れて4ワード欠落)。pio_sm_set_dmactrlで
//    TX=閾値0にすると送信は安定するが、受信はRX=閾値8+DWELL最大でも
//    SMが速い(div<50)とバースト境界でまれに化けるため使用しない。
//    既定は両方向ともFIFOレベルポーリング+非ブロッキングput/get。
//    環境変数W25N_TXDMA/W25N_RXDMAでDMAをopt-inできる(実験用)。
// 3. シフト閾値などの設定はpio_sm_init経由なら正しく適用されるが、
//    pio_sm_set_config単独では反映されない。設定変更は必ずsm_initで行う。
// 4. ループ一周(後方jmp/wrap)には命令数と無関係に約11サイクル(分周後)かかる。
//    ループ内の命令自体は実質コストゼロなので、tx/rxはアンロールして
//    1周あたりのビット数を稼ぐ(rx: 8bit/周、qtx: 5クロック/周)。
// 5. 信頼動作域は clkdiv >= 8 (バルクはdiv=4以下でビット化け、fdxのJEDECは
//    div=6でNG)。推奨はマージンを取って clkdiv=10。
// 6. 後方jmp x--によるループはプログラム配置依存で「7ループに1回Xが余分に
//    デクリメントされる」(実測: ループ@2..17+jmp17→2で発症、@14..29+jmp29→14
//    では無症状。div=10/100共通)。Xでの回数制御は使わず、wrapでフリーラン
//    させてホスト側でワード数を数えること (wrapループは旧tx@2..13で実績あり)。
//
// FIFOワードはビッグエンディアン詰め(先頭バイト=最上位、左シフトでMSBファースト)。
// CLK/IO0/IO1 のみPIOにミュックスし、CS#/WP#/HOLD# はGPIO cdev
// (/dev/gpiochip0のioctl)で制御する(CSのタイミングにサイクル精度は不要)。
// FIFOが空くとSMは out で停止しCLKはその時のside値で止まる。W25NはCS
// 立ち下がり時のCLKレベルでmode0/3を自動判別するのでどちらでも正しく動く。
#include "w25n_pio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "piolib.h"

#define GPIOCHIP "/dev/gpiochip0"   /* RP1 (pinctrl-rp1) */
#define CONSUMER "w25n_pio"

// プログラム配置 (固定オフセット)。命令メモリは32スロット (30/32使用)
#define OFF_FDX 0            /* 2命令 */
#define OFF_RX  2            /* 16命令 (8bit/ループ、フリーラン) */
#define RX_LEN  16
#define OFF_QTX 18           /* 10命令 (5クロック=20bit/ループ) */
#define QTX_LEN 10
#define QTX_SPAN 22          /* ワイドOUTの幅 (U2: GPIO5..26が最大) */
#define OFF_QRX 28           /* 2命令 (1クロック=1ワード、フリーラン) */
#define QRX_LEN 2

enum sm_mode { MODE_NONE, MODE_FDX, MODE_QTX };

#define BULK_MAX 2176            /* ページ+スペア領域 */

struct w25n_dev {
    PIO pio;
    int sm;
    w25n_pins_t pins;
    float clkdiv;
    enum sm_mode mode;
    int use_rxdma;                 /* DREQ閾値を上書きしてカーネルDMAを使う */
    int no_quad_dma;               /* qtxをFIFOポーリングで行う (実験用) */
    int quad_dma_ready;
    pio_sm_config cfg_fdx, cfg_rx, cfg_qtx, cfg_qrx;
    uint32_t qtab[16];             /* ニブル→ワイドOUTワードのスウィズル表 */
    uint32_t words[2 * BULK_MAX];  /* バルク転送ワードバッファ (x4は2ワード/バイト) */
    int cs_fd;                     /* GPIO cdevラインリクエストfd (CS#のみ) */
};

// PIOインスタンスとプログラムはデバイス間で共有
// (RP1のPIOは1ブロック・命令メモリ32スロットしかない)
static PIO g_pio;
static int g_pio_refs;
static int g_progs_loaded;
static w25n_dev_t *g_devs[4];      /* qtx後の/WP,/HOLDラッチ復元用レジストリ */

static void load_spi_programs(PIO pio)
{
    if (g_progs_loaded)
        return;
    static uint16_t fdx[2], rx[RX_LEN], qtx[QTX_LEN], qrx[QRX_LEN];
    fdx[0] = pio_encode_out(pio_pins, 1) | pio_encode_sideset(1, 0);
    fdx[1] = pio_encode_in(pio_pins, 1) | pio_encode_sideset(1, 1);
    for (int i = 0; i < RX_LEN; i += 2) {
        rx[i] = pio_encode_in(pio_pins, 1) | pio_encode_sideset(1, 1);
        rx[i + 1] = pio_encode_nop() | pio_encode_sideset(1, 0);
    }
    for (int i = 0; i < QTX_LEN; i += 2) {
        qtx[i] = pio_encode_out(pio_pins, QTX_SPAN) | pio_encode_sideset(1, 0);
        qtx[i + 1] = pio_encode_nop() | pio_encode_sideset(1, 1);
    }
    qrx[0] = pio_encode_in(pio_pins, 32) | pio_encode_sideset(1, 1);
    qrx[1] = pio_encode_nop() | pio_encode_sideset(1, 0);
    pio_program_t p_fdx = { .instructions = fdx, .length = 2, .origin = OFF_FDX };
    pio_program_t p_rx = { .instructions = rx, .length = RX_LEN, .origin = OFF_RX };
    pio_program_t p_qtx = { .instructions = qtx, .length = QTX_LEN, .origin = OFF_QTX };
    pio_program_t p_qrx = { .instructions = qrx, .length = QRX_LEN, .origin = OFF_QRX };
    pio_add_program_at_offset(pio, &p_fdx, OFF_FDX);
    pio_add_program_at_offset(pio, &p_rx, OFF_RX);
    pio_add_program_at_offset(pio, &p_qtx, OFF_QTX);
    pio_add_program_at_offset(pio, &p_qrx, OFF_QRX);
    g_progs_loaded = 1;
}

// 4モード分のSM設定を組み立てる(clkdiv変更時にも呼ぶ)
static void build_configs(w25n_dev_t *d)
{
    const w25n_pins_t *p = &d->pins;
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, p->clk);
    sm_config_set_out_pins(&c, p->io0, 1);
    sm_config_set_in_pins(&c, p->io1);
    sm_config_set_clkdiv(&c, d->clkdiv);

    d->cfg_fdx = c;
    sm_config_set_wrap(&d->cfg_fdx, OFF_FDX, OFF_FDX + 1);
    sm_config_set_out_shift(&d->cfg_fdx, false, true, 8);
    sm_config_set_in_shift(&d->cfg_fdx, false, true, 8);

    d->cfg_rx = c;
    sm_config_set_wrap(&d->cfg_rx, OFF_RX, OFF_RX + RX_LEN - 1);
    sm_config_set_out_shift(&d->cfg_rx, false, false, 32);
    sm_config_set_in_shift(&d->cfg_rx, false, true, 32);

    // qtx: ワイドOUT。out_base=io3(スパン最小ピン)、右シフトでワードの
    // bit k → GPIO(io3+k)。autopull閾値=QTX_SPANで1ワード=1クロック
    d->cfg_qtx = c;
    sm_config_set_wrap(&d->cfg_qtx, OFF_QTX, OFF_QTX + QTX_LEN - 1);
    sm_config_set_out_pins(&d->cfg_qtx, p->io3, QTX_SPAN);
    sm_config_set_out_shift(&d->cfg_qtx, true, true, QTX_SPAN);
    sm_config_set_in_shift(&d->cfg_qtx, false, true, 32);

    // qrx: 全GPIO取り込み。in pins,32はin_base=0でワードのbit k = GPIO k
    d->cfg_qrx = c;
    sm_config_set_wrap(&d->cfg_qrx, OFF_QRX, OFF_QRX + QRX_LEN - 1);
    sm_config_set_in_pins(&d->cfg_qrx, 0);
    sm_config_set_out_shift(&d->cfg_qrx, false, false, 32);
    sm_config_set_in_shift(&d->cfg_qrx, false, true, 32);
}

// SMを指定モードのプログラム/設定で再初期化して走らせる
static void enter_mode(w25n_dev_t *d, enum sm_mode mode)
{
    if (d->mode == mode)
        return;
    const pio_sm_config *c = (mode == MODE_QTX) ? &d->cfg_qtx : &d->cfg_fdx;
    uint pc = (mode == MODE_QTX) ? OFF_QTX : OFF_FDX;
    pio_sm_set_enabled(d->pio, d->sm, false);
    pio_sm_init(d->pio, d->sm, pc, c);   /* FIFOクリア+restart+設定+jmp */
    pio_sm_set_enabled(d->pio, d->sm, true);
    d->mode = mode;
}

// GPIO 1本を出力・初期値付きで確保し、ラインリクエストfdを返す
static int claim_output(int chip_fd, unsigned n, int value)
{
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = n;
    req.num_lines = 1;
    strncpy(req.consumer, CONSUMER, sizeof(req.consumer) - 1);
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = value ? 1 : 0;
    req.config.attrs[0].mask = 1;
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
        return -1;
    return req.fd;
}

static void set_line(int fd, int value)
{
    struct gpio_v2_line_values v = { .bits = value ? 1 : 0, .mask = 1 };
    ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

w25n_dev_t *w25n_open(const w25n_pins_t *pins, float clkdiv)
{
    w25n_dev_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->pins = *pins;
    d->sm = -1;
    d->cs_fd = -1;

    int chip_fd = open(GPIOCHIP, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) {
        fprintf(stderr, "w25n: %sを開けない\n", GPIOCHIP);
        goto fail;
    }
    // CSはHigh(非選択)で確保。/WPと/HOLDはx4時にIO2/IO3として使うため
    // cdevではなくPIOの出力ラッチで駆動する(下)
    d->cs_fd = claim_output(chip_fd, pins->cs, 1);
    close(chip_fd);  /* ラインfd確保後はチップfd不要 */
    if (d->cs_fd < 0) {
        fprintf(stderr, "w25n: GPIO %u を確保できない\n", pins->cs);
        goto fail;
    }

    if (!g_pio) {
        PIO pio = pio_open(0);
        if (PIO_IS_ERR(pio)) {
            fprintf(stderr, "w25n: /dev/pio0 を開けない (%ld)\n", -(long)(uintptr_t)pio);
            goto fail;
        }
        g_pio = pio;
    }
    d->pio = g_pio;
    g_pio_refs++;
    // SMは固定割当 (U2=SM1 > U3=SM0。ヘッダのw25n_pins_t参照)
    if (pio_sm_is_claimed(d->pio, pins->sm)) {
        fprintf(stderr, "w25n: SM%uは使用中\n", pins->sm);
        goto fail;
    }
    pio_sm_claim(d->pio, pins->sm);
    d->sm = pins->sm;

    load_spi_programs(d->pio);
    d->clkdiv = clkdiv;
    build_configs(d);

    // ニブル→ワイドOUTワードのスウィズル表 (x4: IO0=bit0 .. IO3=bit3)
    for (int v = 0; v < 16; v++)
        d->qtab[v] = ((uint32_t)(v & 1) << (pins->io0 - pins->io3)) |
                     ((uint32_t)((v >> 1) & 1) << (pins->io1 - pins->io3)) |
                     ((uint32_t)((v >> 2) & 1) << (pins->io2 - pins->io3)) |
                     ((uint32_t)((v >> 3) & 1) << 0);

    // カーネルDMA: qtx(x4送信)はDREQ閾値0で安定するため既定で使用
    // (W25N_QUAD_NODMAでポーリングに切替)。RXはdiv<50で稀に化けるため
    // W25N_RXDMAでのopt-inのみ (ファイル冒頭の制約2参照)。
    d->no_quad_dma = getenv("W25N_QUAD_NODMA") != NULL;
    d->use_rxdma = getenv("W25N_RXDMA") != NULL;
    if (d->use_rxdma) {
        if (pio_sm_config_xfer(d->pio, d->sm, PIO_DIR_FROM_SM, 32, 4)) {
            fprintf(stderr, "w25n: RX DMAバッファ確保に失敗\n");
            goto fail;
        }
        // RX: 閾値8(FIFO満杯時のみDREQ) + DWELL最大31(バースト直後の
        // DREQ再発行による空読みを防ぐ)
        pio_sm_set_dmactrl(d->pio, d->sm, false, 0x80000000u | (31u << 7) | 8);
    }

    pio_gpio_init(d->pio, pins->clk);
    pio_gpio_init(d->pio, pins->io0);
    pio_gpio_init(d->pio, pins->io1);
    pio_gpio_init(d->pio, pins->io2);
    pio_gpio_init(d->pio, pins->io3);

    pio_sm_init(d->pio, d->sm, OFF_FDX, &d->cfg_fdx);
    // /WPと/HOLDはHigh(無効)のラッチを出力。x1中はこの値が保持される
    pio_sm_set_pins_with_mask(d->pio, d->sm,
                              (1u << pins->io2) | (1u << pins->io3),
                              (1u << pins->io2) | (1u << pins->io3));
    pio_sm_set_consecutive_pindirs(d->pio, d->sm, pins->clk, 1, true);
    pio_sm_set_consecutive_pindirs(d->pio, d->sm, pins->io0, 1, true);
    pio_sm_set_consecutive_pindirs(d->pio, d->sm, pins->io1, 1, false);
    pio_sm_set_consecutive_pindirs(d->pio, d->sm, pins->io2, 1, true);
    pio_sm_set_consecutive_pindirs(d->pio, d->sm, pins->io3, 1, true);
    pio_sm_set_enabled(d->pio, d->sm, true);
    d->mode = MODE_FDX;

    for (int i = 0; i < 4; i++) {
        if (!g_devs[i]) {
            g_devs[i] = d;
            break;
        }
    }
    return d;

fail:
    w25n_close(d);
    return NULL;
}

void w25n_close(w25n_dev_t *d)
{
    if (!d)
        return;
    for (int i = 0; i < 4; i++) {
        if (g_devs[i] == d)
            g_devs[i] = NULL;
    }
    if (d->pio && d->sm >= 0) {
        pio_sm_set_enabled(d->pio, d->sm, false);
        pio_sm_unclaim(d->pio, d->sm);
    }
    if (d->pio && --g_pio_refs == 0) {
        pio_close(g_pio);
        g_pio = NULL;
        g_progs_loaded = 0;
    }
    if (d->cs_fd >= 0)
        close(d->cs_fd);
    free(d);
}

// 全二重1バイト転送 (fdxモード前提)。TX 1バイトにつき必ずRX 1バイト返る。
static uint8_t xfer_byte(w25n_dev_t *d, uint8_t b)
{
    pio_sm_put_blocking(d->pio, d->sm, (uint32_t)b << 24);
    return (uint8_t)pio_sm_get_blocking(d->pio, d->sm);
}

void w25n_set_clkdiv(w25n_dev_t *d, float clkdiv)
{
    d->clkdiv = clkdiv;
    build_configs(d);
    d->mode = MODE_NONE;   /* 次の転送でSMを再初期化させる */
}

// TX FIFOとOSRが完全に掃けるまで待つ (CSを上げる前に必須)
static int drain_tx(w25n_dev_t *d)
{
    for (int i = 0; i < 1000000; i++) {
        if (pio_sm_get_tx_fifo_level(d->pio, d->sm) == 0) {
            /* OSR内の残り最大32bit(2サイクル/bit)が出きるのを待つ */
            unsigned us = (unsigned)(80.0f * d->clkdiv / 200.0f) + 1;
            usleep(us);
            return 0;
        }
    }
    fprintf(stderr, "w25n: TXドレインタイムアウト\n");
    return -1;
}

// x4送信中に上書きされる全デバイスの/WP,/HOLDラッチをHighへ復元する
static void restore_wp_hold(w25n_dev_t *d)
{
    uint32_t mask = 0;
    for (int i = 0; i < 4; i++) {
        if (g_devs[i])
            mask |= (1u << g_devs[i]->pins.io2) | (1u << g_devs[i]->pins.io3);
    }
    pio_sm_set_pins_with_mask(d->pio, d->sm, mask, mask);
}

// x4送信専用バルク (CSアサート中、32h/34hのコマンド+アドレス送信後に呼ぶ)。
// 1バイト=2クロック=2ワードにスウィズルして送る。TO_SM DMAはDREQ閾値0で
// 安定するため既定で使用 (W25N_QUAD_NODMAでポーリングに切替)。
// 呼び出し側でIO1..3を出力に切り替えてから呼び、終了後に復元すること。
static int qtx_bulk(w25n_dev_t *d, const uint8_t *data, size_t len)
{
    // スパン内にある他デバイスの/WP,/HOLDは全ワードでHighを書いて
    // 転送中の保護状態の揺れを防ぐ (最終的なラッチ復元は呼び出し側)
    uint32_t hi = 0;
    for (int i = 0; i < 4; i++) {
        if (!g_devs[i] || g_devs[i] == d)
            continue;
        unsigned wp = g_devs[i]->pins.io2, hold = g_devs[i]->pins.io3;
        if (wp >= d->pins.io3 && wp < d->pins.io3 + QTX_SPAN)
            hi |= 1u << (wp - d->pins.io3);
        if (hold >= d->pins.io3 && hold < d->pins.io3 + QTX_SPAN)
            hi |= 1u << (hold - d->pins.io3);
    }
    size_t nwords = 2 * len;
    for (size_t i = 0; i < len; i++) {
        d->words[2 * i] = d->qtab[data[i] >> 4] | hi;       /* 上位ニブルが先 */
        d->words[2 * i + 1] = d->qtab[data[i] & 15] | hi;
    }
    enter_mode(d, MODE_QTX);
    if (!d->no_quad_dma) {
        if (!d->quad_dma_ready) {
            if (pio_sm_config_xfer(d->pio, d->sm, PIO_DIR_TO_SM, 8192, 4)) {
                fprintf(stderr, "w25n: TX DMAバッファ確保に失敗、ポーリングへ切替\n");
                d->no_quad_dma = 1;
            } else {
                pio_sm_set_dmactrl(d->pio, d->sm, true, 0x80000100u);  /* TX: 閾値0 */
                d->quad_dma_ready = 1;
            }
        }
        if (!d->no_quad_dma) {
            if (pio_sm_xfer_data(d->pio, d->sm, PIO_DIR_TO_SM, nwords * 4, d->words)) {
                fprintf(stderr, "w25n: TX DMAに失敗\n");
                return -1;
            }
            return drain_tx(d);
        }
    }
    const uint depth = pio_get_fifo_depth(d->pio);
    size_t sent = 0;
    int idle = 0;
    while (sent < nwords) {
        uint lvl = pio_sm_get_tx_fifo_level(d->pio, d->sm);
        if (lvl >= depth) {
            // まずスピンで待つ(usleepは実際には~100µs寝るため多用すると遅い)
            if (++idle > 100000) {   /* 最大 ~1秒 */
                fprintf(stderr, "w25n: TXポーリングがword%zuでタイムアウト\n", sent);
                return -1;
            }
            if (idle > 2000)
                usleep(50);
            continue;
        }
        idle = 0;
        for (; lvl < depth && sent < nwords; lvl++)
            pio_sm_put(d->pio, d->sm, d->words[sent++]);
    }
    return drain_tx(d);
}

// 受信専用バルク (CSアサート中に呼ぶ)。len%4==0 かつ len<=BULK_MAX
// 回収はFIFOレベルポーリング (FROM_SM DMAはカーネル側の不具合で使えない。
// ファイル冒頭の制約2参照)。FIFOが満杯ならSMはin命令でストールし損失はない。
static int rx_bulk(w25n_dev_t *d, uint8_t *buf, size_t len)
{
    pio_sm_set_enabled(d->pio, d->sm, false);
    pio_sm_init(d->pio, d->sm, OFF_RX, &d->cfg_rx);
    pio_sm_set_enabled(d->pio, d->sm, true);
    // フリーラン: 必要ワード数を回収したらSMを止める。SMは回収中も
    // FIFO満杯で停止するまで走るが、余分なバイトは捨てられるだけ
    d->mode = MODE_NONE;   /* SMを途中で止めるため要再初期化 */
    // RXのDREQは8ワード貯まらないと出ないため、DMAは32バイト倍数のみ
    if (d->use_rxdma && len % 32 == 0) {
        if (pio_sm_xfer_data(d->pio, d->sm, PIO_DIR_FROM_SM, len, d->words)) {
            fprintf(stderr, "w25n: RX DMAに失敗\n");
            pio_sm_set_enabled(d->pio, d->sm, false);
            return -1;
        }
        pio_sm_set_enabled(d->pio, d->sm, false);
        goto unpack;
    }
    size_t got = 0;
    int idle = 0;
    while (got < len / 4) {
        uint lvl = pio_sm_get_rx_fifo_level(d->pio, d->sm);
        if (!lvl) {
            // まずスピンで待つ(usleepは実際には~100µs寝るため多用すると遅い)
            if (++idle > 100000) {   /* 最大 ~1秒 */
                fprintf(stderr, "w25n: RXポーリングがword%zuでタイムアウト\n", got);
                pio_sm_set_enabled(d->pio, d->sm, false);
                return -1;
            }
            if (idle > 2000)
                usleep(50);
            continue;
        }
        idle = 0;
        while (lvl-- && got < len / 4)
            d->words[got++] = pio_sm_get(d->pio, d->sm);
    }
    pio_sm_set_enabled(d->pio, d->sm, false);
unpack:
    for (size_t i = 0; i < len / 4; i++) {
        uint32_t w = d->words[i];
        buf[4 * i] = w >> 24;
        buf[4 * i + 1] = w >> 16;
        buf[4 * i + 2] = w >> 8;
        buf[4 * i + 3] = w;
    }
    return 0;
}

int w25n_cmd(w25n_dev_t *d, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen)
{
    enter_mode(d, MODE_FDX);
    set_line(d->cs_fd, 0);
    for (size_t i = 0; i < txlen; i++)
        xfer_byte(d, tx[i]);
    for (size_t i = 0; i < rxlen; i++)
        rx[i] = xfer_byte(d, 0x00);
    set_line(d->cs_fd, 1);
    return 0;
}

int w25n_jedec_id(w25n_dev_t *d, uint8_t id[3])
{
    const uint8_t tx[2] = { 0x9F, 0x00 };  /* コマンド + ダミー8クロック */
    return w25n_cmd(d, tx, sizeof(tx), id, 3);
}

int w25n_read_status(w25n_dev_t *d, uint8_t addr, uint8_t *val)
{
    const uint8_t tx[2] = { 0x0F, addr };
    return w25n_cmd(d, tx, sizeof(tx), val, 1);
}

int w25n_write_status(w25n_dev_t *d, uint8_t addr, uint8_t val)
{
    const uint8_t tx[3] = { 0x1F, addr, val };
    return w25n_cmd(d, tx, sizeof(tx), NULL, 0);
}

int w25n_unprotect(w25n_dev_t *d)
{
    uint8_t sr;
    w25n_write_status(d, 0xA0, 0x00);
    w25n_read_status(d, 0xA0, &sr);
    if (sr != 0x00) {
        fprintf(stderr, "w25n: 保護解除に失敗 (SR-1=0x%02X)\n", sr);
        return -1;
    }
    return 0;
}

int w25n_set_ecc(w25n_dev_t *d, int enable)
{
    uint8_t sr;
    w25n_read_status(d, 0xB0, &sr);
    sr = enable ? (sr | 0x10) : (sr & ~0x10);
    w25n_write_status(d, 0xB0, sr);
    uint8_t chk;
    w25n_read_status(d, 0xB0, &chk);
    return chk == sr ? 0 : -1;
}

static int write_enable(w25n_dev_t *d)
{
    const uint8_t tx[1] = { 0x06 };
    return w25n_cmd(d, tx, sizeof(tx), NULL, 0);
}

int w25n_wait_busy(w25n_dev_t *d, unsigned timeout_us, uint8_t *sr3_out)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint8_t sr3;
        w25n_read_status(d, 0xC0, &sr3);
        if (!(sr3 & 0x01)) {
            if (sr3_out)
                *sr3_out = sr3;
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        unsigned long el = (t.tv_sec - t0.tv_sec) * 1000000ul +
                           (t.tv_nsec - t0.tv_nsec) / 1000;
        if (el > timeout_us) {
            if (sr3_out)
                *sr3_out = sr3;
            fprintf(stderr, "w25n: BUSYタイムアウト (%uus)\n", timeout_us);
            return -1;
        }
    }
}

int w25n_block_erase(w25n_dev_t *d, uint32_t page)
{
    write_enable(d);
    const uint8_t tx[4] = { 0xD8, page >> 16, page >> 8, page };
    w25n_cmd(d, tx, sizeof(tx), NULL, 0);
    uint8_t sr3;
    if (w25n_wait_busy(d, 500000, &sr3) < 0)   /* tBEmax=10ms */
        return -1;
    if (sr3 & 0x04) {   /* E-FAIL */
        fprintf(stderr, "w25n: Erase失敗 page=%u (SR-3=0x%02X)\n", page, sr3);
        return -1;
    }
    return 0;
}

// データバッファへのロード。長さが十分ならQuad Data Load(32h)+x4送信、
// それ以外(またはW25N_NOQUAD)はStandard Data Load(02h)+バイト転送。
// どちらもバッファ全体をFFhにリセットしてからcol位置へ置く。
static int load_data(w25n_dev_t *d, uint16_t col, const uint8_t *data, size_t len)
{
    static int no_quad = -1;
    if (no_quad < 0)
        no_quad = getenv("W25N_NOQUAD") != NULL;
    int rc = 0;
    int quad = !no_quad && len >= 8 && len <= BULK_MAX;
    enter_mode(d, MODE_FDX);
    set_line(d->cs_fd, 0);
    const uint8_t hdr[3] = { quad ? 0x32 : 0x02, col >> 8, col };
    for (size_t i = 0; i < sizeof(hdr); i++)
        xfer_byte(d, hdr[i]);
    if (quad) {
        // IO1..3をPIO出力へ (io0は元々出力)。フラッシュは32hのデータ相で
        // IO0..3を入力扱いするので衝突しない
        uint32_t io123 = (1u << d->pins.io1) | (1u << d->pins.io2) | (1u << d->pins.io3);
        pio_sm_set_pindirs_with_mask(d->pio, d->sm, io123, io123);
        rc = qtx_bulk(d, data, len);
        set_line(d->cs_fd, 1);
        // IO1(DO)を入力へ戻し、全デバイスの/WP,/HOLDラッチをHighへ復元
        pio_sm_set_pindirs_with_mask(d->pio, d->sm, 0, 1u << d->pins.io1);
        restore_wp_hold(d);
    } else {
        for (size_t i = 0; i < len; i++)
            xfer_byte(d, data[i]);
        set_line(d->cs_fd, 1);
    }
    return rc;
}

int w25n_program_begin(w25n_dev_t *d, uint16_t col, const uint8_t *data, size_t len)
{
    write_enable(d);
    return load_data(d, col, data, len);
}

int w25n_program_commit(w25n_dev_t *d, uint32_t page)
{
    const uint8_t exec[4] = { 0x10, page >> 16, page >> 8, page };
    w25n_cmd(d, exec, sizeof(exec), NULL, 0);
    uint8_t sr3;
    if (w25n_wait_busy(d, 100000, &sr3) < 0)   /* tPPmax=700us */
        return -1;
    if (sr3 & 0x08) {   /* P-FAIL */
        fprintf(stderr, "w25n: Program失敗 page=%u (SR-3=0x%02X)\n", page, sr3);
        return -1;
    }
    return 0;
}

int w25n_program(w25n_dev_t *d, uint32_t page, uint16_t col, const uint8_t *data, size_t len)
{
    if (w25n_program_begin(d, col, data, len) < 0)
        return -1;
    return w25n_program_commit(d, page);
}

int w25n_page_read(w25n_dev_t *d, uint32_t page)
{
    const uint8_t tx[4] = { 0x13, page >> 16, page >> 8, page };
    w25n_cmd(d, tx, sizeof(tx), NULL, 0);
    return w25n_wait_busy(d, 100000, NULL);    /* tRDmax=60us */
}

int w25n_read_buf(w25n_dev_t *d, uint16_t col, uint8_t *buf, size_t len)
{
    const uint8_t tx[4] = { 0x03, col >> 8, col, 0x00 };  /* +8ダミークロック */
    if (len % 4 != 0 || len < 8 || len > BULK_MAX)
        return w25n_cmd(d, tx, sizeof(tx), buf, len);
    int rc;
    enter_mode(d, MODE_FDX);
    set_line(d->cs_fd, 0);
    for (size_t i = 0; i < sizeof(tx); i++)
        xfer_byte(d, tx[i]);
    rc = rx_bulk(d, buf, len);
    set_line(d->cs_fd, 1);
    return rc;
}

// Fast Read Quad Output (6Bh): コマンド+アドレスはx1、8ダミークロックの後
// フラッシュがIO0..3の4本でデータを出す。qrxで全GPIOを1クロック=1ワード
// 取り込み、ホスト側で4bitずつ抽出する (1バイト=2ワード)。
int w25n_quad_read_buf(w25n_dev_t *d, uint16_t col, uint8_t *buf, size_t len)
{
    if (len < 1 || len > BULK_MAX)
        return -1;
    enter_mode(d, MODE_FDX);
    set_line(d->cs_fd, 0);
    const uint8_t hdr[3] = { 0x6B, col >> 8, col };
    for (size_t i = 0; i < sizeof(hdr); i++)
        xfer_byte(d, hdr[i]);
    // データ相でフラッシュがIO0..3を駆動するため、ダミークロックの前に
    // IO0,/WP,/HOLDを入力へ切り替えて衝突を防ぐ (IO1は元々入力。
    // ダミー中のDIはdon't care)
    uint32_t io023 = (1u << d->pins.io0) | (1u << d->pins.io2) | (1u << d->pins.io3);
    pio_sm_set_pindirs_with_mask(d->pio, d->sm, 0, io023);
    xfer_byte(d, 0x00);   /* 8ダミークロック */
    // qrxへ切替。x1バルク同様、最初のin(side1)はエッジを作らず
    // 直前の立ち下がりでフラッシュが出した最初のニブルを取り込む
    pio_sm_set_enabled(d->pio, d->sm, false);
    pio_sm_init(d->pio, d->sm, OFF_QRX, &d->cfg_qrx);
    pio_sm_set_enabled(d->pio, d->sm, true);
    d->mode = MODE_NONE;
    // SMは回収完了後もFIFO満杯で停止するまで走るが、余分に読み出される
    // バイトは捨てられるだけなので害はない
    size_t nwords = 2 * len, got = 0;
    int idle = 0, rc = 0;
    while (got < nwords) {
        uint lvl = pio_sm_get_rx_fifo_level(d->pio, d->sm);
        if (!lvl) {
            if (++idle > 100000) {   /* 最大 ~1秒 */
                fprintf(stderr, "w25n: QRXポーリングがword%zuでタイムアウト\n", got);
                rc = -1;
                break;
            }
            if (idle > 2000)
                usleep(50);
            continue;
        }
        idle = 0;
        while (lvl-- && got < nwords)
            d->words[got++] = pio_sm_get(d->pio, d->sm);
    }
    pio_sm_set_enabled(d->pio, d->sm, false);
    set_line(d->cs_fd, 1);   /* フラッシュがIOを解放してからピンを戻す */
    pio_sm_set_pindirs_with_mask(d->pio, d->sm, io023, io023);
    restore_wp_hold(d);
    const w25n_pins_t *p = &d->pins;
    for (size_t i = 0; rc == 0 && i < len; i++) {
        uint32_t w0 = d->words[2 * i], w1 = d->words[2 * i + 1];
        unsigned hi = ((w0 >> p->io0) & 1) | ((w0 >> p->io1) & 1) << 1 |
                      ((w0 >> p->io2) & 1) << 2 | ((w0 >> p->io3) & 1) << 3;
        unsigned lo = ((w1 >> p->io0) & 1) | ((w1 >> p->io1) & 1) << 1 |
                      ((w1 >> p->io2) & 1) << 2 | ((w1 >> p->io3) & 1) << 3;
        buf[i] = hi << 4 | lo;
    }
    return rc;
}
