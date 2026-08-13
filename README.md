# HsqOverNand

NAND型フラッシュメモリを論理素子として使う 64bit SUBLEQ 計算機と、その上で動く
C風言語コンパイラ (Higher Subleq) の実装。

NANDフラッシュのプログラム動作は「ビットが 1→0 にしか変化しない」ため、
セル(1バイト)は書き込まれた値の AND を蓄積する。これを論理ゲートとして利用し、
SUBLEQ の減算 `b - a` をフラッシュのプログラムシーケンスだけで実行する。

## 構成

```
HSQソース(.hsq) → hsq → SUBLEQコード(.sq) → subleq(インタプリタ)
                                               │ 減算をUDP:8080でオフロード
                                               ▼
                                          nandServer → /dev/mtd0 (NANDフラッシュ)
```

- **語幅 64bit / アドレス空間 24bit(2^24語)**
- 減算回路は階層型キャリー先見(4bit×4グループ×4スーパーグループ)のボロー版。
  `src/server/ROM.cpp` がネットリストを自動生成し、書き込みステップを自動スケジューリングする
- 1回路 874 セルなので 2048B のフラッシュ 1 ページに 2 回路が載る。
  **2レーン並列**: 複数クライアントのリクエストを 1 ラウンドに相乗りさせ、
  36 回のプログラムシーケンスを 2 つの減算で共有する
- **マルチフラッシュ並列**: nandServer は起動時に /dev/mtd0..3 を検出し、
  1 デバイスにつき 1 CPU(+専用ワーカースレッド)を立ち上げる。全ワーカーが
  同一 UDP ソケットを共有するためクライアントは無変更。4 デバイス × 2 レーン
  で最大 8 並列減算。相乗り待ちはアクティブクライアント数が CPU 数を超えた
  ときだけ行う
- **HW SPI(spidev)ドライバ**: W25N02KV を Linux の spidev(RP1 の実 SPI
  ペリフェラル)経由で直接制御する。RP1 PIO による自前 QSPI ビットバング
  (`src/qspi/`)は実測で HW 単体 SPI より遅かったため、新規チップは
  `src/spi/`(spidev + 非同期 Program)を使う。Program 実行(10h)はコマンド
  送信後 BUSY 待ちせずに戻る(`issue`/`wait` 分割)ため、同一物理バスを
  共有する複数チップに対して「あるチップの Program 中(最大 tPP=700us)に
  別チップへコマンドを送る」ことでバスのアイドルを減らせる
  (`include/W25NSpiFlashDevice.hpp`)。ただし SUBLEQ の 1 ラウンドは同一
  ページへの依存した AND の積み重ねなのでチップ内ではパイプライン化でき
  ず、恩恵が出るのは独立チップ間(既存のマルチフラッシュ並列と同じ形)
- **ウェアレベリング**: ラウンド毎に物理ページを 1 つ進め、全容量
  (2Gbit なら 130,752 ページ)を使い切ったときだけブロックを一括消去する

## ビルドと実行

```sh
make            # subleq, nandServer, simpleClient
make hsq        # Higher Subleq コンパイラ
                # (オリジナルのhsq.cppを取得→SHA256検証→64bitパッチ適用→ビルド。要ネットワーク)

./run.sh        # nandsim をロードして nandServer を起動 (要 sudo)
                # 実機 NAND の場合は /dev/mtd0 をそのまま使う

./hsq test/small64.hsq -o small64.sq   # コンパイル
./subleq small64.sq                    # NANDフラッシュ上で実行
./hsq -e test/small64.hsq              # ソフトウェアエミュレータで実行
./hsq -f test/small64.hsq              # hsq内蔵エミュレータから減算だけ実機に委譲
```

注意: ROM のレイアウトを変更した場合は、フラッシュに旧 ROM が残っていると
AND 合成で壊れるため nandsim の再ロード(run.sh)が必要。nandServer は起動時に
ROM を読み戻して検証し、不一致なら `ROM verify FAILED` で終了する。

### HW SPI(spidev)でのNANDデバイス指定

W25N02KV(等)を Linux の spidev 経由で使うには、チップを RP1 の実 SPI
ペリフェラル固定ピンに配線する(SPI0: GPIO7-11 の CE1/CE0/MISO/MOSI/SCLK、
SPI1: GPIO16-21 の CE0-2/MISO/MOSI/SCLK)。spidev のデバイスノードが
`/dev/spidevB.C` として現れたら、環境変数で nandServer に伝える:

```sh
NAND_DEV=spi NAND_SPI_DEVS=/dev/spidev0.0,/dev/spidev0.1 ./nandServer
```

チップ 1 個ごとに 1 nandInterface(=1 CPU ワーカー)を立ち上げる(HAT の
`hat:u2`/`hat:u3` と同じ粒度)。SPI クロックは `W25N_SPI_HZ`(既定
20MHz、要実機での実測調整)で指定する。`spiPipelineTest` (`make
spiPipelineTest`) で疎通確認・AND 特性検証・非同期 Program の効果測定が
できる:

```sh
./spiPipelineTest /dev/spidev0.0 /dev/spidev0.1
```

## 検証

ハードウェア無しでネットリストを検証できる(インメモリNANDスタブ):

```sh
g++ -O2 -DNAND_STUB -Iinclude src/server/ROM.cpp src/server/cpu.cpp \
    test/test_sim.cpp -o test_sim && ./test_sim
```

実機に対しては `./simpleClient` が全数・境界値・乱数の回帰テストを行う。

## ディレクトリ

| パス | 内容 |
|---|---|
| `src/server/` | nandServer: マイクロコードROM生成(CLA)、CPU、UDPサーバ |
| `src/subleq/` | SUBLEQインタプリタ(減算をnandServerへオフロード) |
| `src/client/` | simpleClient: 減算の回帰テスト |
| `src/qspi/` | SPI-NAND HAT ver.4向けRP1 PIOドライバ(x1+x4、参考実装) |
| `src/spi/` | HW SPI(spidev)向け非同期W25N02KVドライバ(現行) |
| `include/` | 共有ヘッダ(ROM/CPU/NANDインタフェース/MTD/W25N) |
| `hsq64.patch` | Higher Subleq コンパイラを64bit化するパッチ(本体はビルド時取得) |
| `hsq_src/`, `test/` | HSQサンプルとテストハーネス |
| `hw/` | 回路図(キャリー先見加算器) |

## 実行速度の目安

1 命令 ≈ 1ms (UDP往復 + 36回のページプログラム)。`printf("%d")` の除算ループは
命令数が膨大になるため、大きな数の印字は実機では分〜時間単位かかる。

## ライセンスとクレジット

このリポジトリのコードは [MITライセンス](LICENSE)。

Higher Subleq コンパイラ (hsq.cpp) は Oleg Mazonka 氏の著作物
([オリジナルサイト](http://mazonka.com/subleq/)、2009-2011)で、ライセンスが
公表されていないため**このリポジトリでは再配布していない**。リポジトリに
含まれるのは当方の64bit化改変分 `hsq64.patch`(MIT)のみで、`make hsq` が
オリジナルを公開アーカイブ([Wayback Machine](https://web.archive.org/web/20211207033118/http://mazonka.com/subleq/hsq.cpp)
/ [GitHubミラー](https://github.com/8l/hsq))から取得し、SHA256検証のうえ
ローカルでパッチを適用する。
