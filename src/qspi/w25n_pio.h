// W25N02KV SPI-NAND を RP1 PIO で制御するドライバ (SPI-NAND HAT ver.4)
// Step 5: x1 SPI + x4(QSPI)。書き込みデータ相はQuad Data Load(32h)、
// 読み出しはFast Read Quad Output(6Bh)にも対応 (w25n_quad_read_buf)。
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// BCM GPIO番号。io0=DI, io1=DO, io2=/WP, io3=/HOLD
// sm: 使用するPIOステートマシン番号 (固定割当)。x4送信はIO0..3が飛び飛びの
// ため「ワイドOUT+ホスト側スウィズル」で行い、U3のワイドOUTスパン(GPIO2..23)
// がU2のCLK(GPIO12)を跨ぐ。同一サイクルに複数SMが同じピンへ書くとSM番号の
// 大きい方が勝つので、U2のSM番号 > U3のSM番号 を維持すること。
typedef struct {
    unsigned cs, clk, io0, io1, io2, io3;
    unsigned sm;
} w25n_pins_t;

// SPI-NAND HAT ver.4 の配線 (U2/U3で完全に独立したバス)
#define W25N_U2_PINS ((w25n_pins_t){ .cs = 21, .clk = 12, .io0 = 19, .io1 = 26, .io2 = 20, .io3 = 5, .sm = 1 })
#define W25N_U3_PINS ((w25n_pins_t){ .cs = 27, .clk = 4,  .io0 = 15, .io1 = 17, .io2 = 18, .io3 = 2, .sm = 0 })

typedef struct w25n_dev w25n_dev_t;

// clkdiv: PIOクロック分周。SPIクロック = 200MHz / clkdiv / 2
// (例: clkdiv=100 → 1MHz)。失敗時はNULLを返しerrnoを設定。
w25n_dev_t *w25n_open(const w25n_pins_t *pins, float clkdiv);
void w25n_close(w25n_dev_t *d);

// CSをアサートし、txlenバイト送信(x1)→rxlenバイト受信(x1)し、CSを戻す
int w25n_cmd(w25n_dev_t *d, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen);

// JEDEC ID (9Fh)。W25N02KVなら id = {0xEF, 0xAA, 0x22}
int w25n_jedec_id(w25n_dev_t *d, uint8_t id[3]);

// ステータスレジスタ読み出し (0Fh)。addr: 0xA0=保護, 0xB0=設定, 0xC0=状態
int w25n_read_status(w25n_dev_t *d, uint8_t addr, uint8_t *val);

// ステータスレジスタ書き込み (1Fh)
int w25n_write_status(w25n_dev_t *d, uint8_t addr, uint8_t val);

// 全ブロックの書き込み保護を解除 (SR-1=0x00、読み戻し検証付き)
int w25n_unprotect(w25n_dev_t *d);

// ECCエンジンの有効/無効 (SR-2のECC-Eビット)。
// NANDセルを論理素子に使う(二重書き込み=AND)場合は無効必須。
int w25n_set_ecc(w25n_dev_t *d, int enable);

// BUSYが下がるまでポーリング。sr3_outに最終SR-3を返す(NULL可)
int w25n_wait_busy(w25n_dev_t *d, unsigned timeout_us, uint8_t *sr3_out);

// 128KBブロック消去 (D8h)。pageはブロック内任意のページ番号(17bit)
int w25n_block_erase(w25n_dev_t *d, uint32_t page);

// データバッファへロード(02h)→Program Execute(10h)→完了待ち。
// 02hはバッファ全体をFFhにリセットしてからcol位置にlenバイト置く
int w25n_program(w25n_dev_t *d, uint32_t page, uint16_t col, const uint8_t *data, size_t len);

// Page Data Read (13h): ページをデータバッファへ読み込み、完了を待つ
int w25n_page_read(w25n_dev_t *d, uint32_t page);

// データバッファからcol位置をlenバイト読む (03h)。
// len が4の倍数(8〜2176)なら専用SMプログラムでのバルク転送、それ以外はバイト毎転送
int w25n_read_buf(w25n_dev_t *d, uint16_t col, uint8_t *buf, size_t len);

// データバッファからcol位置をlenバイト読む (6Bh, Fast Read Quad Output)。
// IO0..3の4本を受信に使う。ホスト側スウィズルのためFIFO 1ワード=4bitで
// x1バルク(w25n_read_buf)よりFIFO効率は悪い。検証・将来のRX DMA用。
int w25n_quad_read_buf(w25n_dev_t *d, uint16_t col, uint8_t *buf, size_t len);

// SPIクロックの変更。SPIクロック = 200MHz / clkdiv / 2 (全転送モード共通)
void w25n_set_clkdiv(w25n_dev_t *d, float clkdiv);

#ifdef __cplusplus
}
#endif
