# 変数定義
CXX = g++
CXXFLAGS = -Wall -Iinclude -std=c++14 -Ofast -I/usr/local/include -O3
LIBS = `pkg-config --libs gtk+-3.0` -lpthread
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
$(SERVER): $(SERVER_OBJ_FILES)
	$(CXX) $(SERVER_OBJ_FILES) -o $(SERVER) $(LIBS)

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

# クリーンアップ
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET)

# フォースターゲット指定
.PHONY: all clean
