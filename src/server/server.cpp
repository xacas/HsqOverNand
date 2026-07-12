#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <memory>
#include <mutex>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "cpu.hpp"

#undef EN_GUI

// データ構造体
struct Request {
    int64_t a;
    int64_t b;
};

struct Response {
    int64_t c;
    bool is_non_positive;
};

// グローバル状態管理
class ServerState {
public:
    std::atomic<bool> isboost{false};  // 初期状態をBoost OFFに変更
    std::atomic<bool> server_running{false};
    std::atomic<bool> image_update_running{false};
    
    // GUI要素
    GtkWidget *status_label = nullptr;
    GtkWidget *toggle_button = nullptr;
    GtkWidget *image_widget = nullptr;
    
    // スレッド管理
    std::thread server_thread;
    std::thread image_update_thread;
    
    // MTDデータ
    static constexpr size_t MTD_DATA_SIZE = MTD_SHADOW_SIZE;
    static constexpr int IMAGE_WIDTH = 500;
    static constexpr int IMAGE_HEIGHT = 500;
    static constexpr int UPDATE_INTERVAL_MS = 100;
    
    uint8_t mtd_data[MTD_DATA_SIZE];
    
    ServerState() {
        memset(mtd_data, 0xff, MTD_DATA_SIZE);
    }
    
    ~ServerState() {
        cleanup();
    }
    
    void cleanup() {
        server_running.store(false);
        stop_image_update_thread();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
    
    void stop_image_update_thread() {
        if (image_update_running.load()) {
            image_update_running.store(false);
            if (image_update_thread.joinable()) {
                image_update_thread.join();
            }
        }
    }
    
    void restart_image_update_thread() {
        stop_image_update_thread();
        // 少し待機してからスレッドを再起動
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
};

// グローバルインスタンス
ServerState g_state;
uint8_t *mtd_data = g_state.mtd_data;

// GUI更新関数群
namespace GUI {
    void update_status_label() {
        gchar *status_text = g_strdup_printf("NAND型フラッシュメモリサーバ: %s\nBoost モード: %s", 
                                            g_state.server_running.load() ? "稼働中" : "停止中",
                                            g_state.isboost.load() ? "有効" : "無効");
        gtk_label_set_text(GTK_LABEL(g_state.status_label), status_text);
        g_free(status_text);
    }
    
    void free_rgb_data(guchar *pixels, gpointer data) {
        delete[] pixels;
    }
    
    GdkPixbuf* create_pixbuf_from_mtd_data() {
        const int channels = 3;
        const int rowstride = ServerState::IMAGE_WIDTH * channels;
        
        guchar *rgb_data = new guchar[ServerState::IMAGE_WIDTH * ServerState::IMAGE_HEIGHT * channels];
        
        for (int i = 0; i < ServerState::IMAGE_WIDTH * ServerState::IMAGE_HEIGHT; i++) {
            uint8_t gray_value = g_state.mtd_data[i];
            rgb_data[i * 3 + 0] = gray_value; // R
            rgb_data[i * 3 + 1] = gray_value; // G
            rgb_data[i * 3 + 2] = gray_value; // B
        }
        
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(
            rgb_data,
            GDK_COLORSPACE_RGB,
            FALSE, // has_alpha
            8,     // bits_per_sample
            ServerState::IMAGE_WIDTH,
            ServerState::IMAGE_HEIGHT,
            rowstride,
            free_rgb_data,
            NULL
        );
        
        if (!pixbuf) {
            delete[] rgb_data;
        }
        
        return pixbuf;
    }
    
    void update_image_display() {
        if (g_state.isboost.load()) {
            gtk_image_clear(GTK_IMAGE(g_state.image_widget));
            return;
        }
        
        GdkPixbuf *pixbuf = create_pixbuf_from_mtd_data();
        if (pixbuf) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(g_state.image_widget), pixbuf);
            g_object_unref(pixbuf);
            gtk_widget_show(g_state.image_widget);
        } else {
            std::cerr << "Pixbufの作成に失敗しました" << std::endl;
        }
    }
    
    gboolean update_image_on_gui_thread(gpointer data) {
        GdkPixbuf *pixbuf = static_cast<GdkPixbuf*>(data);
        
        if (pixbuf && g_state.image_widget) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(g_state.image_widget), pixbuf);
            gtk_widget_show(g_state.image_widget);
            g_object_unref(pixbuf);
        }
        
        return G_SOURCE_REMOVE;
    }
}

// 画像更新スレッド管理
namespace ImageUpdater {
    void thread_func() {
        std::cout << "画像更新スレッドを開始しました（" << ServerState::UPDATE_INTERVAL_MS << "ms間隔）" << std::endl;
        
        while (g_state.image_update_running.load()) {
            if (!g_state.isboost.load()) {
                GdkPixbuf *pixbuf = GUI::create_pixbuf_from_mtd_data();
                if (pixbuf) {
                    g_idle_add(GUI::update_image_on_gui_thread, pixbuf);
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(ServerState::UPDATE_INTERVAL_MS));
        }
        
        std::cout << "画像更新スレッドを停止しました" << std::endl;
    }
    
    void start() {
        if (!g_state.image_update_running.load()) {
            g_state.image_update_running.store(true);
            // 既存のスレッドが残っている場合は適切に処理
            if (g_state.image_update_thread.joinable()) {
                g_state.image_update_thread.join();
            }
            g_state.image_update_thread = std::thread(thread_func);
        }
    }
    
    void stop() {
        if (g_state.image_update_running.load()) {
            g_state.image_update_running.store(false);
            if (g_state.image_update_thread.joinable()) {
                g_state.image_update_thread.join();
            }
        }
    }
}

// サーバー処理（UDP版）
namespace Server {
    // 1ラウンド(1回のNANDプログラムシーケンス)にLANES件まで相乗りさせる。
    // 相乗り相手を待つ最大時間。1ラウンドのフラッシュ時間より十分小さくする
    constexpr int BATCH_WAIT_US = 500;
    // 接続するフラッシュデバイス(/dev/mtd0..3)の最大数。1デバイス=1CPU=1ワーカー
    constexpr int MAX_MTD = 4;

    struct Pending {
        Request req;
        struct sockaddr_in addr;
        socklen_t len;
    };

    // ワーカー = 1個のフラッシュデバイスを占有するCPU
    struct Worker {
        int id = 0;
        CPU* cpu = nullptr;
    };

    // 直近1秒のアクティブクライアント数を全ワーカー共有で数える。
    // クライアント数が「CPU数」を超えるときだけ相乗り待ちする
    // (超えていなければ各クライアントは別CPUで並列処理でき、待ちは純粋な損)
    class ClientTracker {
        static constexpr int MAXC = 32;
        std::mutex mu;
        struct Ent {
            uint64_t key;
            std::chrono::steady_clock::time_point seen;
        } ent[MAXC] = {};
        int n = 0;
    public:
        // クライアントの活動を記録し、現在のアクティブ数を返す
        int touch(const sockaddr_in& a) {
            const uint64_t key = ((uint64_t)a.sin_addr.s_addr << 16) | a.sin_port;
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(mu);
            int active = 0;
            int slot = -1;
            bool found = false;
            for (int i = 0; i < n; i++) {
                if (ent[i].key == key) {
                    ent[i].seen = now;
                    found = true;
                }
                if (now - ent[i].seen < std::chrono::seconds(1)) {
                    active++;
                } else if (slot < 0) {
                    slot = i;   // 期限切れスロットは再利用できる
                }
            }
            if (!found) {
                if (slot < 0 && n < MAXC) {
                    slot = n++;
                }
                if (slot >= 0) {
                    ent[slot] = {key, now};
                }
                active++;
            }
            return active;
        }
    };

    ClientTracker g_clients;
    int g_num_cpus = 1;

    void process_requests(int server_fd, Worker& w) {
        CPU& cpu = *w.cpu;

        Pending p[LANES];
        p[0].len = sizeof(p[0].addr);
        // 全ワーカーが同一ソケットを共有するため非ブロッキングで取る
        // (selectで起きても他ワーカーが先に取っていることがある)
        ssize_t bytes_received = recvfrom(server_fd, &p[0].req, sizeof(Request), MSG_DONTWAIT,
                                         (struct sockaddr*)&p[0].addr, &p[0].len);
        if (bytes_received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (bytes_received != sizeof(Request)) {
            std::cout << "データ受信エラー: 受信サイズ " << bytes_received << " bytes" << std::endl;
            return;
        }
        int n = 1;
        int active = g_clients.touch(p[0].addr);

        bool current_isboost = g_state.isboost.load();

        if (!current_isboost) {
            // 既に到着済みのリクエストを非ブロッキングで取り込む
            while (n < LANES) {
                p[n].len = sizeof(p[n].addr);
                ssize_t r = recvfrom(server_fd, &p[n].req, sizeof(Request), MSG_DONTWAIT,
                                     (struct sockaddr*)&p[n].addr, &p[n].len);
                if (r != sizeof(Request)) break;
                active = g_clients.touch(p[n].addr);
                n++;
            }
            // クライアント数がCPU数を超えている(=CPUを分け合うしかない)ときだけ、
            // 少しだけ相乗り相手を待つ。CPU数以下なら別CPUで並列処理できるので待たない
            auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::microseconds(BATCH_WAIT_US);
            while (n < LANES && active > g_num_cpus) {
                auto remain = std::chrono::duration_cast<std::chrono::microseconds>(
                                deadline - std::chrono::steady_clock::now()).count();
                if (remain <= 0) break;
                fd_set rf;
                struct timeval tv;
                FD_ZERO(&rf);
                FD_SET(server_fd, &rf);
                tv.tv_sec = 0;
                tv.tv_usec = remain;
                if (select(server_fd + 1, &rf, NULL, NULL, &tv) <= 0) break;
                p[n].len = sizeof(p[n].addr);
                ssize_t r = recvfrom(server_fd, &p[n].req, sizeof(Request), MSG_DONTWAIT,
                                     (struct sockaddr*)&p[n].addr, &p[n].len);
                if (r == sizeof(Request)) {
                    active = g_clients.touch(p[n].addr);
                    n++;
                }
            }
        }

        Response resp[LANES];

        if (current_isboost) {
            // Boostモード: 直接計算
            for (int i = 0; i < n; i++) {
                resp[i].c = (int64_t)((uint64_t)p[i].req.b - (uint64_t)p[i].req.a);
                resp[i].is_non_positive = (resp[i].c <= 0);
            }
        } else {
            // CPUモード: n件をレーンに載せて1ラウンドで並列実行
            for (int i = 0; i < LANES; i++) {
                cpu.inA[i] = (i < n) ? (uint64_t)p[i].req.a : 0;
                cpu.inB[i] = (i < n) ? (uint64_t)p[i].req.b : 0;
            }
            for (size_t k = 0; k < cpu.getRomSize(); k++) {
                cpu.execute(cpu.fetch());
                if (cpu.signal) {
                    break;
                }
            }
            for (int i = 0; i < n; i++) {
                resp[i].c = (int64_t)cpu.out[i];
                resp[i].is_non_positive = cpu.is_jmp[i];
            }
        }

        for (int i = 0; i < n; i++) {
            printf("CPU%d 受信[%d/%d]: a = %20ld, b = %20ld, 計算: c = %20ld, c <= 0 ? : %s\n",
                   w.id, i + 1, n, p[i].req.a, p[i].req.b, resp[i].c,
                   (resp[i].is_non_positive) ? "true" : "false");
            ssize_t bytes_sent = sendto(server_fd, &resp[i], sizeof(Response), 0,
                                       (struct sockaddr*)&p[i].addr, p[i].len);
            if (bytes_sent != sizeof(Response)) {
                std::cout << "応答送信エラー: 送信サイズ " << bytes_sent << " bytes" << std::endl;
            }
        }
    }
    
    // 1ワーカー = 1フラッシュデバイス。全員が同じUDPソケットからリクエストを取る
    void worker_func(int server_fd, Worker* w) {
        while (g_state.server_running.load()) {
            fd_set readfds;
            struct timeval timeout;
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            int activity = select(server_fd + 1, &readfds, NULL, NULL, &timeout);
            if (activity < 0) {
                if (g_state.server_running.load()) {
                    perror("select error");
                }
                break;
            } else if (activity == 0) {
                continue; // タイムアウト
            }
            if (FD_ISSET(server_fd, &readfds)) {
                process_requests(server_fd, *w);
            }
        }
    }

    void thread_func() {
        int server_fd;
        struct sockaddr_in server_addr;

        // 接続されているフラッシュデバイス(/dev/mtd0..3)を検出し、
        // 1デバイスにつき1CPUを立ち上げる(初期化はROM生成の共有状態のため逐次)
        std::vector<std::unique_ptr<CPU>> cpus;
        for (int i = 0; i < MAX_MTD; i++) {
            char dev[16];
            snprintf(dev, sizeof(dev), "/dev/mtd%d", i);
            if (access(dev, F_OK) != 0) {
                break;
            }
            printf("CPU%d: %s を初期化中...\n", i, dev);
            cpus.emplace_back(new CPU(dev));
        }
        if (cpus.empty()) {
            printf("フラッシュデバイスが見つかりません\n");
            return;
        }
        g_num_cpus = (int)cpus.size();
        printf("%zu CPU (最大 %zu 並列減算) でサーバを開始します\n",
               cpus.size(), cpus.size() * LANES);

        // UDPソケット初期化
        if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) == 0) {
            perror("UDP socket failed");
            return;
        }
        
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
            perror("setsockopt");
            close(server_fd);
            return;
        }
        
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(8080);
        
        if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("UDP bind failed");
            close(server_fd);
            return;
        }
        
        g_state.server_running.store(true);
        std::cout << "UDPサーバーがポート8080で起動しました..." << std::endl;
        
        g_idle_add([](gpointer data) -> gboolean {
            GUI::update_status_label();
            return G_SOURCE_REMOVE;
        }, nullptr);
        
        // CPU毎のワーカースレッドを起動して待つ
        std::vector<Worker> workers(cpus.size());
        std::vector<std::thread> worker_threads;
        for (size_t i = 0; i < cpus.size(); i++) {
            workers[i].id = (int)i;
            workers[i].cpu = cpus[i].get();
            worker_threads.emplace_back(worker_func, server_fd, &workers[i]);
        }
        for (auto& t : worker_threads) {
            t.join();
        }

        close(server_fd);
        g_state.server_running.store(false);
        
        g_idle_add([](gpointer data) -> gboolean {
            GUI::update_status_label();
            return G_SOURCE_REMOVE;
        }, nullptr);
        
        std::cout << "UDPサーバーを停止しました" << std::endl;
    }
    
    void start() {
        if (!g_state.server_running.load()) {
            g_state.server_thread = std::thread(thread_func);
        }
    }
}

// イベントハンドラー
namespace EventHandlers {
    void on_toggle_button_toggled(GtkToggleButton *button, gpointer data) {
        gboolean active = gtk_toggle_button_get_active(button);
        
        std::cout << "Debug: ボタンクリック - active = " << active << std::endl;
        std::cout << "Debug: 変更前 isboost = " << g_state.isboost.load() << std::endl;
        
        g_state.isboost.store(active);
        
        std::cout << "Debug: 変更後 isboost = " << g_state.isboost.load() << std::endl;
        
        gtk_button_set_label(GTK_BUTTON(button), active ? "Boost: ON" : "Boost: OFF");
        GUI::update_status_label();
        
        if (active) {
            // Boost ON: 画像更新停止、画像クリア
            std::cout << "Boost ON に切り替え - 画像更新停止" << std::endl;
            ImageUpdater::stop();
            gtk_image_clear(GTK_IMAGE(g_state.image_widget));
        } else {
            // Boost OFF: 画像表示、画像更新開始
            std::cout << "Boost OFF に切り替え - 画像更新開始" << std::endl;
            GUI::update_image_display();
            ImageUpdater::start();
        }
        
        std::cout << "GUI: Boost モードが" << (active ? "有効" : "無効") << "に変更されました" << std::endl;
    }
    
    void on_window_destroy(GtkWidget *widget, gpointer data) {
        g_state.cleanup();
        gtk_main_quit();
    }
}

// GUI初期化
namespace GUISetup {
    GtkWidget* create_main_window() {
        GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Subleq CPU");
        gtk_window_set_default_size(GTK_WINDOW(window), 600, 600);
        gtk_container_set_border_width(GTK_CONTAINER(window), 10);
        
        g_signal_connect(window, "destroy", G_CALLBACK(EventHandlers::on_window_destroy), NULL);
        
        return window;
    }
    
    void setup_widgets(GtkWidget *window) {
        // メインレイアウト
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_container_add(GTK_CONTAINER(window), vbox);
        
        // ステータスラベル
        g_state.status_label = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(vbox), g_state.status_label, FALSE, FALSE, 0);
        
        // トグルボタン（初期状態をBoost OFFに設定）
        g_state.toggle_button = gtk_toggle_button_new_with_label("Boost: OFF");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_state.toggle_button), FALSE);
        g_signal_connect(g_state.toggle_button, "toggled", 
                        G_CALLBACK(EventHandlers::on_toggle_button_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(vbox), g_state.toggle_button, FALSE, FALSE, 0);
        
        // 画像表示エリア
        g_state.image_widget = gtk_image_new();
        gtk_widget_set_size_request(g_state.image_widget, ServerState::IMAGE_WIDTH, ServerState::IMAGE_HEIGHT);
        
        GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                      GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scrolled_window), g_state.image_widget);
        gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);
    }
}

int main(int argc, char *argv[]) {
#ifdef EN_GUI
    gtk_init(&argc, &argv);
       
    // GUI初期化
    GtkWidget *window = GUISetup::create_main_window();
    GUISetup::setup_widgets(window);
#endif    
    // サーバー自動開始
    Server::start();
#ifdef EN_GUI    
    // 初期状態更新
    GUI::update_status_label();

    // 初期状態がBoost OFFなので、画像表示とスレッド開始
    GUI::update_image_display();
    ImageUpdater::start();
    
    // GUI表示開始
    gtk_widget_show_all(window);
    gtk_main();
#else
    while(1){};
#endif
    return 0;
}
