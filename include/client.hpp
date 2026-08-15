#include <iostream>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cerrno>

struct Request {
    int64_t a;
    int64_t b;
};

struct Response {
    int64_t c;
    bool is_jmp;
};

Response flashClient(Request req) {
    // ソケットは再利用する。送信元ポートを固定することで、サーバ側が
    // クライアントを同定でき、複数VM実行時の相乗りバッチングが機能する
    static int sock = -1;
    static struct sockaddr_in serv_addr;

    // UDPはパケットロスがあり得る(特に別マシン越しの実運用)ため、
    // 応答が一定時間来なければ同じリクエストを再送する。
    // 環境変数で調整可能(既定: 3秒タイムアウト、最大5回リトライ)
    static int timeout_ms = [] {
        const char* e = getenv("NAND_CLIENT_TIMEOUT_MS");
        return e ? atoi(e) : 3000;
    }();
    static int max_retries = [] {
        const char* e = getenv("NAND_CLIENT_RETRIES");
        return e ? atoi(e) : 5;
    }();

    if (sock < 0) {
        // UDPソケット作成
        if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            std::cout << "Socket creation error" << std::endl;
            exit(1);
        }

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            std::cout << "setsockopt(SO_RCVTIMEO)に失敗しました" << std::endl;
        }

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(8080);

        // IPv4アドレス変換
        if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
            std::cout << "Invalid address/ Address not supported" << std::endl;
            close(sock);
            exit(1);
        }
    }

    Response resp;
    socklen_t addr_len = sizeof(serv_addr);

    for (int attempt = 0; ; attempt++) {
        // UDPでデータ送信(リトライ時は同一リクエストを再送。サーバ側の計算は
        // a,bのみに依存する純粋関数なので再送しても結果は変わらない)
        ssize_t bytes_sent = sendto(sock, &req, sizeof(req), 0,
                                   (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (bytes_sent != sizeof(req)) {
            if (attempt < max_retries) {
                std::cout << "データ送信に失敗しました(試行" << (attempt + 1) << "/" << (max_retries + 1)
                           << "): 送信サイズ " << bytes_sent << " bytes、再送します" << std::endl;
                continue;
            }
            std::cout << "データ送信に失敗しました: 送信サイズ " << bytes_sent << " bytes" << std::endl;
            close(sock);
            exit(1);
        }
        //std::cout << "データを送信しました: a=" << req.a << ", b=" << req.b << std::endl;

        // UDPで結果受信(SO_RCVTIMEOによりtimeout_msでタイムアウトする)
        ssize_t bytes_received = recvfrom(sock, &resp, sizeof(resp), 0,
                                         (struct sockaddr*)&serv_addr, &addr_len);

        if (bytes_received == sizeof(resp)) {
            #if 0
            std::cout << "結果を受信しました:" << std::endl;
            std::cout << "c = b - a = " << resp.c << std::endl;
            std::cout << "c <= 0 ? " << (resp.is_jmp ? "true" : "false") << std::endl;
            #endif
            break;
        }

        bool is_timeout = (bytes_received < 0) && (errno == EAGAIN || errno == EWOULDBLOCK);
        if (attempt < max_retries) {
            std::cout << (is_timeout ? "応答タイムアウト" : "結果の受信に失敗しました")
                       << "(試行" << (attempt + 1) << "/" << (max_retries + 1)
                       << "): 受信サイズ " << bytes_received << " bytes、再送します" << std::endl;
            continue;
        }
        std::cout << "結果の受信に失敗しました(リトライ上限到達): 受信サイズ " << bytes_received << " bytes" << std::endl;
        close(sock);
        exit(1);
    }

    // タイムアウト後の再送で二重に届いた古い応答が残っていれば読み捨てる。
    // 残したまま次回呼び出しに入ると、次のリクエストへの応答としてこの
    // 古いデータを誤って受け取ってしまう
    for (;;) {
        struct timeval zero_tv = {0, 0};
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sock, &rf);
        if (select(sock + 1, &rf, NULL, NULL, &zero_tv) <= 0) break;
        Response stale;
        if (recvfrom(sock, &stale, sizeof(stale), MSG_DONTWAIT, NULL, NULL) <= 0) break;
        std::cout << "古い応答を読み捨てました" << std::endl;
    }

    return resp;
}