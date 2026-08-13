// W25N02KV (SPI-NAND) を Linux spidev (RP1のハードウェアSPIペリフェラル)
// で叩くドライバ。src/qspi/w25n_pio.c (RP1 PIOによるGPIOビットバング)の
// 置き換え。x1のみ(quadは扱わない): PIO版のx1+x4は実測でHW単体SPI(x1)より
// 遅かったため、方針をHW SPIに一本化した。
//
// 【非同期Program】W25N02KVのProgram実行(10h)はコマンド送信後、内部で
// 最大tPP=700usビジーになるが、この間SPIバスは物理的に空く。
// w25n_spi_program_issue()は10hを送るだけでBUSY待ちをしない。複数チップが
// 同一物理バス(別CS)を共有する構成で、あるチップのissue直後に別チップへ
// load+issueを送れば、tPPがオーバーラップしバス利用率が上がる
// (呼び出し側のスケジューリングは include/W25NSpiFlashDevice.hpp 参照)。
// 単一チップしか無い場合もissue/waitを分けておくことで、将来の多チップ化や
// 「他の処理を挟んでからwaitする」用途に対応できる。
//
// 【複数チップとバス共有】spidevは1つの物理SPIバス(spi_master)に対し
// CSごとに/dev/spidevB.Cが生成される。同一バス上の複数chip fdへの
// ioctl(SPI_IOC_MESSAGE)はカーネルのspi_masterキューが自動的に直列化する
// ため、PIO版のようなホスト側mutexは不要(ワイドOUTでピンを共有する
// PIO特有の問題が存在しない)。
//
// 【BUSYポーリングとバス占有】w25n_spi_wait_busyは既定でポーリング間隔
// poll_interval_us(呼び出し側指定、推奨50〜100us)だけsleepしてから
// GET_FEATURESを送る。0を指定するとビジーポーリング(バスをほぼ独占)に
// なり、共有バス上の他チップの非同期処理を妨げるため非推奨。
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// W25N02KVのジオメトリ (2Gbit、単一ダイ、ページアドレスは17bit連続)
#define W25N_SPI_PAGE_SIZE   2048u
#define W25N_SPI_BLOCK_PAGES 64u
#define W25N_SPI_BLOCK_SIZE  (W25N_SPI_PAGE_SIZE * W25N_SPI_BLOCK_PAGES)      /* 128KB */
#define W25N_SPI_NUM_BLOCKS  2048u
#define W25N_SPI_TOTAL_SIZE  (W25N_SPI_BLOCK_SIZE * W25N_SPI_NUM_BLOCKS)      /* 256MB */

typedef struct w25n_spi_dev w25n_spi_dev_t;

// path: 例 "/dev/spidev0.0"。speed_hz: SPIクロック(要実測調整、まずは
// 保守的な値で確認してから上げること)。失敗時はNULLを返しerrnoを設定。
w25n_spi_dev_t *w25n_spi_open(const char *path, uint32_t speed_hz);
void w25n_spi_close(w25n_spi_dev_t *d);

// SPIクロックの変更(以後の転送に適用)
int w25n_spi_set_speed(w25n_spi_dev_t *d, uint32_t speed_hz);

// CSをアサートし、txlenバイト送信→rxlenバイト受信し、CSを戻す
// (1回のioctl呼び出しに収める=CSは送受信を通じて連続してLow)
int w25n_spi_cmd(w25n_spi_dev_t *d, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen);

// JEDEC ID (9Fh)。W25N02KVなら id = {0xEF, 0xAA, 0x22}
int w25n_spi_jedec_id(w25n_spi_dev_t *d, uint8_t id[3]);

// ステータスレジスタ読み出し (0Fh)。addr: 0xA0=保護, 0xB0=設定, 0xC0=状態
int w25n_spi_read_status(w25n_spi_dev_t *d, uint8_t addr, uint8_t *val);
// ステータスレジスタ書き込み (1Fh)
int w25n_spi_write_status(w25n_spi_dev_t *d, uint8_t addr, uint8_t val);

// 全ブロックの書き込み保護を解除 (SR-1=0x00、読み戻し検証付き)
int w25n_spi_unprotect(w25n_spi_dev_t *d);
// ECCエンジンの有効/無効 (SR-2のECC-Eビット)。セル=書込値のANDを使う
// (二重書き込み)場合は無効必須
int w25n_spi_set_ecc(w25n_spi_dev_t *d, int enable);

// BUSYが下がるまでポーリング。poll_interval_usごとにsleepしてから
// GET_FEATURESを送る(共有バスの占有を避けるため)。sr3_outに最終SR-3を
// 返す(NULL可)
int w25n_spi_wait_busy(w25n_spi_dev_t *d, unsigned timeout_us,
                        unsigned poll_interval_us, uint8_t *sr3_out);

// 128KBブロック消去 (D8h)。pageはブロック内任意のページ番号(17bit)。同期版
int w25n_spi_block_erase(w25n_spi_dev_t *d, uint32_t page);

// Program の3分割(いずれもx1、Standard Data Load 02h):
//   load  : Write Enable(06h) + Data Load(02h)。バッファ全体をFFhに
//           リセットしてからcol位置にlenバイト置く(チップの02h自体の仕様)
//   issue : Program Execute(10h)を送信するだけで戻る(BUSY待ちしない)
//   wait  : issueの完了を待つ
int w25n_spi_program_load(w25n_spi_dev_t *d, uint16_t col, const uint8_t *data, size_t len);
int w25n_spi_program_issue(w25n_spi_dev_t *d, uint32_t page);
int w25n_spi_program_wait(w25n_spi_dev_t *d, unsigned timeout_us, uint8_t *sr3_out);

// 同期版(load+issue+wait)。単一チップ運用や既存コードとの互換用
int w25n_spi_program(w25n_spi_dev_t *d, uint32_t page, uint16_t col, const uint8_t *data, size_t len);

// Page Data Read (13h): ページをデータバッファへ読み込み、完了を待つ
int w25n_spi_page_read(w25n_spi_dev_t *d, uint32_t page);
// データバッファからcol位置をlenバイト読む (03h)
int w25n_spi_read_buf(w25n_spi_dev_t *d, uint16_t col, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
