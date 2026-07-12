#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>

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
    socklen_t addr_len = sizeof(serv_addr);

    if (sock < 0) {
        // UDPソケット作成
        if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            std::cout << "Socket creation error" << std::endl;
            exit(1);
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
    
    // UDPでデータ送信
    ssize_t bytes_sent = sendto(sock, &req, sizeof(req), 0,
                               (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if (bytes_sent != sizeof(req)) {
        std::cout << "データ送信に失敗しました: 送信サイズ " << bytes_sent << " bytes" << std::endl;
        close(sock);
        exit(1);
    }
    //std::cout << "データを送信しました: a=" << req.a << ", b=" << req.b << std::endl;
    
    // UDPで結果受信
    ssize_t bytes_received = recvfrom(sock, &resp, sizeof(resp), 0,
                                     (struct sockaddr*)&serv_addr, &addr_len);

    if (bytes_received == sizeof(resp)) {
        #if 0
        std::cout << "結果を受信しました:" << std::endl;
        std::cout << "c = b - a = " << resp.c << std::endl;
        std::cout << "c <= 0 ? " << (resp.is_jmp ? "true" : "false") << std::endl;
        #endif
    } else {
        std::cout << "結果の受信に失敗しました: 受信サイズ " << bytes_received << " bytes" << std::endl;
    }

    return resp;
}