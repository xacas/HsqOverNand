# 変数定義
CXX = g++
# -MMD -MP: ヘッダ依存(.d)を生成し、ヘッダ変更時に.oを再ビルドさせる
CXXFLAGS = -Wall -Iinclude -Isrc/qspi -Isrc/spi -std=c++14 -Ofast -I/usr/local/include -O3 -MMD -MP
LIBS = `pkg-config --libs gtk+-3.0` -lpthread

# SPI-NAND HAT用のPIOドライバ(C)。nandServerにリンクする
QSPI_OBJ_DIR = build/qspi
QSPI_OBJS = $(QSPI_OBJ_DIR)/w25n_pio.o $(QSPI_OBJ_DIR)/piolib.o \
            $(QSPI_OBJ_DIR)/library_piochips.o $(QSPI_OBJ_DIR)/pio_rp1.o

# HW SPI(spidev)用の非同期W25N02KVドライバ(C)。nandServerにリンクする
SPI_OBJ_DIR = build/spi
SPI_OBJS = $(SPI_OBJ_DIR)/w25n_spidev.o
SUBLEQ_SRC_DIR = src/subleq
SERVER_SRC_DIR = src/server
CLIENT_SRC_DIR = src/client
SUBLEQ_BUILD_DIR = build/subleq
SERVER_BUILD_DIR = build/server
CLIENT_BUILD_DIR = build/client
BUILD_DIR = build
SUBLEQ = subleq
SERVER = nandServer
CLIENT = simpleClient
HSQ = hsq
TARGET = $(SUBLEQ) $(SERVER) $(CLIENT) $(HSQ)

# ソースファイルとオブジェクトファイル
SUBLEQ_SRC_FILES = $(wildcard $(SUBLEQ_SRC_DIR)/*.cpp)
SERVER_SRC_FILES = $(wildcard $(SERVER_SRC_DIR)/*.cpp)
CLIENT_SRC_FILES = $(wildcard $(CLIENT_SRC_DIR)/*.cpp)
SUBLEQ_OBJ_FILES = $(patsubst $(SUBLEQ_SRC_DIR)/%.cpp, $(SUBLEQ_BUILD_DIR)/%.o, $(SUBLEQ_SRC_FILES))
SERVER_OBJ_FILES = $(patsubst $(SERVER_SRC_DIR)/%.cpp, $(SERVER_BUILD_DIR)/%.o, $(SERVER_SRC_FILES))
CLIENT_OBJ_FILES = $(patsubst $(CLIENT_SRC_DIR)/%.cpp, $(CLIENT_BUILD_DIR)/%.o, $(CLIENT_SRC_FILES))

# デフォルトターゲット
all: $(TARGET)

# ターゲットのリンク
$(SERVER): $(SERVER_OBJ_FILES) $(QSPI_OBJS) $(SPI_OBJS)
	$(CXX) $(SERVER_OBJ_FILES) $(QSPI_OBJS) $(SPI_OBJS) -o $(SERVER) $(LIBS)

$(SUBLEQ): $(SUBLEQ_OBJ_FILES)
	$(CXX) $(SUBLEQ_OBJ_FILES) -o $(SUBLEQ)

$(CLIENT): $(CLIENT_OBJ_FILES)
	$(CXX) $(CLIENT_OBJ_FILES) -o $(CLIENT)

# hsqのオリジナル(Oleg Mazonka氏作、ライセンス未公表)は再配布しない。
# ビルド時に取得してチェックサム検証し、64bit化パッチ(hsq64.patch)を適用する。
HSQ_ORIG_URL = https://web.archive.org/web/20211207033118/http://mazonka.com/subleq/hsq.cpp
HSQ_ORIG_URL2 = https://raw.githubusercontent.com/8l/hsq/master/hsq.cpp
HSQ_ORIG_SHA256 = 462c210ae200fbe7780ad5b8b8adced28d37ff5697a8af806e12bbe23c32d751

hsq_orig.cpp:
	curl -fsSL "$(HSQ_ORIG_URL)" -o $@ || curl -fsSL "$(HSQ_ORIG_URL2)" -o $@
	echo "$(HSQ_ORIG_SHA256)  $@" | sha256sum -c -

hsq.cpp: hsq_orig.cpp hsq64.patch
	tr -d '\r' < hsq_orig.cpp > $@
	patch $@ hsq64.patch

$(HSQ): hsq.cpp
	$(CXX) $(CXXFLAGS) hsq.cpp -o $(HSQ)

# 各オブジェクトファイルのビルドルール
$(SUBLEQ_BUILD_DIR)/%.o: $(SUBLEQ_SRC_DIR)/%.cpp | $(SUBLEQ_BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SERVER_BUILD_DIR)/%.o: $(SERVER_SRC_DIR)/%.cpp | $(SERVER_BUILD_DIR)
	$(CXX) $(CXXFLAGS) `pkg-config --cflags gtk+-3.0` -c $< -o $@

$(CLIENT_BUILD_DIR)/%.o: $(CLIENT_SRC_DIR)/%.cpp | $(CLIENT_BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# buildディレクトリが存在しない場合に作成
$(SUBLEQ_BUILD_DIR):
	mkdir -p $(SUBLEQ_BUILD_DIR)

$(SERVER_BUILD_DIR):
	mkdir -p $(SERVER_BUILD_DIR)

$(CLIENT_BUILD_DIR):
	mkdir -p $(CLIENT_BUILD_DIR)

# RP1 PIOによるW25N02KV直接制御 (SPI-NAND HAT ver.4)
CC = gcc
PIOLIB_DIR = vendor/piolib
PIOLIB_SRC = $(PIOLIB_DIR)/piolib.c $(PIOLIB_DIR)/library_piochips.c $(PIOLIB_DIR)/pio_rp1.c
QSPI_CFLAGS = -Wall -O2 -I$(PIOLIB_DIR)/include -Isrc/qspi -DLIBRARY_BUILD=1

# nandServerにリンクするオブジェクト
$(QSPI_OBJ_DIR):
	mkdir -p $(QSPI_OBJ_DIR)

$(QSPI_OBJ_DIR)/w25n_pio.o: src/qspi/w25n_pio.c | $(QSPI_OBJ_DIR)
	$(CC) $(QSPI_CFLAGS) -c $< -o $@

$(QSPI_OBJ_DIR)/%.o: $(PIOLIB_DIR)/%.c | $(QSPI_OBJ_DIR)
	$(CC) $(QSPI_CFLAGS) -c $< -o $@

qspiTest: src/qspi/test_jedec.c src/qspi/w25n_pio.c $(PIOLIB_SRC)
	$(CC) $(QSPI_CFLAGS) $^ -o $@

qspiPageTest: src/qspi/test_page.c src/qspi/w25n_pio.c $(PIOLIB_SRC)
	$(CC) $(QSPI_CFLAGS) $^ -o $@

qspiSpeedTest: src/qspi/test_speed.c src/qspi/w25n_pio.c $(PIOLIB_SRC)
	$(CC) $(QSPI_CFLAGS) $^ -o $@

qspiQuadTest: src/qspi/test_quad.c src/qspi/w25n_pio.c $(PIOLIB_SRC)
	$(CC) $(QSPI_CFLAGS) $^ -o $@

# HW SPI(spidev)による非同期W25N02KVドライバ
$(SPI_OBJ_DIR):
	mkdir -p $(SPI_OBJ_DIR)

$(SPI_OBJ_DIR)/%.o: src/spi/%.c | $(SPI_OBJ_DIR)
	$(CC) -Wall -O2 -Isrc/spi -c $< -o $@

# HW SPIの疎通・往復・非同期パイプラインのベンチマーク
# (実チップを破壊的に書き換える。不良ブロックマークをスキャンして
# 良品ブロックを自動選択してから使用する)
# w25n_spidev.cはC(calloc等の暗黙変換がC++では通らない)なのでgccでオブジェクト化してからリンクする
# 例: ./spiPipelineTest /dev/spidev0.0 /dev/spidev1.0
spiPipelineTest: src/spi/test_pipeline.cpp $(SPI_OBJS)
	$(CXX) -Wall -O2 -std=c++14 -Iinclude -Isrc/spi $^ -o $@

# SPI-NAND HAT実チップでのCPU統合テスト (sudo不要)
testHw: test/test_hw.cpp src/server/ROM.cpp src/server/cpu.cpp \
        src/server/gpiolib.cpp src/server/gpiochip_rp1.cpp src/server/util.cpp $(QSPI_OBJS) $(SPI_OBJS)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) $(QSPI_OBJS) $(SPI_OBJS) -o $@ -lpthread

# デバッグ用: PIOファームウェアの生存確認(全ioctlがETIMEDOUTなら要再起動)
pioProbe: src/qspi/pio_probe.c
	$(CC) $(QSPI_CFLAGS) $^ -o $@

# デバッグ用: シフト閾値切り替えがSM_INITで効くかの実験
dbgShiftCfg: src/qspi/dbg_shiftcfg.c $(PIOLIB_SRC)
	$(CC) $(QSPI_CFLAGS) $^ -o $@

# デバッグ用: バルクRXのFIFOポーリング/DMA比較 (ループバック、フラッシュ非使用)
dbgRxBulk: src/qspi/dbg_rxbulk.c $(PIOLIB_SRC)
	$(CC) $(QSPI_CFLAGS) $^ -o $@

# ヘッダ依存(.d)の取り込み (初回ビルド時は存在しないので-include)
-include $(SUBLEQ_OBJ_FILES:.o=.d) $(SERVER_OBJ_FILES:.o=.d) $(CLIENT_OBJ_FILES:.o=.d)

# クリーンアップ
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET) qspiTest qspiPageTest qspiSpeedTest qspiQuadTest testHw pioProbe dbgShiftCfg dbgRxBulk spiPipelineTest

# フォースターゲット指定
.PHONY: all clean
