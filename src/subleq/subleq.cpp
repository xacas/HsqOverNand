#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <setjmp.h>
#include <inttypes.h>
#include "client.hpp"

// 語幅は64bit、アドレス空間は24bit(2^24語)のまま
#define MAX_CODE_SIZE (0xFFFFFF + 1 )
int64_t code[MAX_CODE_SIZE];
volatile bool die;
volatile bool ctrlc;
static int ctrlc_count = 0;
sigjmp_buf jmpbuf;

#define RING_BUFFER_SIZE 100000

typedef struct {
    volatile uint8_t buffer[RING_BUFFER_SIZE];
    volatile int head;
    volatile int tail;
} RingBuffer;

RingBuffer keypressBuffer;

// Function to initialize the ring buffer
void ring_buffer_init(RingBuffer *rb) {
    rb->head = rb->tail = 0;
}


// Function to pop a character from the ring buffer, returns -1 if buffer is empty
int ring_buffer_pop(RingBuffer *rb) {
    if (rb->head == rb->tail) {
        return -1; // Buffer is empty
    } else {
        int nextChar = rb->buffer[rb->tail];
        rb->tail = (rb->tail + 1) % RING_BUFFER_SIZE;
        return nextChar;
    }
}

struct termios orig_termios;

// Function to reset the terminal to its original state
void reset_terminal_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

// Function to set the terminal to raw mode
void set_raw_mode() {
    struct termios raw;

    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(reset_terminal_mode);

    raw = orig_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

// Signal handler for SIGIO
void handle_keypress(int sig) {
    static int cnt=0;
    char c;
    while (read(STDIN_FILENO, &c, 1) > 0) {
        if (die) return;

        // Check for CTRL-C and set the flag if detected. The flag is read by the 
        // subleq program to check if CTRL-C was pressed. 
        // If pressed 3 times in a row, the subleq interpreter exits to shell/monitor
        if (c==3) {
            ctrlc=1;
            keypressBuffer.head=0;
            keypressBuffer.tail=0;
            cnt++;
            if (cnt>2) die=true; 
        } else cnt=0;

        if (c==4) { // Dump memory from 0x00BE00 to 0x00c000 formatted as a normal hexdump.
            printf("\r\n");
            for (int i=0x00ba00; i<0x00bb00; i+=16) {
                printf("%06X: ", i);
                fflush(stdout);
                for (int j=0; j<16; j++) {
                    printf("%016" PRIX64 " ", code[i+j]);
                fflush(stdout);
                }
                printf(" ");
                fflush(stdout);
                for (int j=0; j<16; j++) {
                    char ch = code[i+j];
                    if (ch>31 && ch<127) printf("%c", ch);
                    else printf(".");
                fflush(stdout);
                }
                printf("\r\n");
                fflush(stdout);
             }
                fflush(stdout);
        }


        int next = (keypressBuffer.head + 1) % RING_BUFFER_SIZE;
        keypressBuffer.buffer[keypressBuffer.head] = c;
        keypressBuffer.head = next;
    }
    // If read returns -1 and errno is set to EAGAIN or EWOULDBLOCK, the read would block, indicating no more data
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No more data to read
    }
}


// Set up asynchronous keypress handling
void setup_async_keypress() {
    struct sigaction sa;

    ring_buffer_init(&keypressBuffer);

    // Set up the signal handler for SIGIO
    sa.sa_handler = handle_keypress;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGIO, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    // Make stdin non-blocking and set it to generate SIGIO on input
    fcntl(STDIN_FILENO, F_SETFL, O_ASYNC | O_NONBLOCK);
    fcntl(STDIN_FILENO, F_SETOWN, getpid());
}

static int read_stdin_blocking(void) {
    fd_set rfds;
    unsigned char c;

    while (!die) {
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int rv = select(STDIN_FILENO + 1, &rfds, NULL, NULL, NULL);
        if (rv == -1) {
            if (errno == EINTR) continue;
            perror("select");
            return -1;
        }
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n == 1) return (int)c;
            if (n == 0) return -1; // EOF
            if (n == -1) {
                if (errno == EINTR) continue;
                return -1;
            }
        }
    }
    return -1;
}

void my_handler(int sig)
{
    siglongjmp(jmpbuf, 1);
}

void set_raw_mode_keep_signals(void) {
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("tcgetattr");
        exit(1);
    }
    atexit(reset_terminal_mode);

    raw = orig_termios;
    cfmakeraw(&raw);        // ほとんどの "raw" 設定を適用
    raw.c_lflag |= ISIG;    // だがシグナル生成だけは有効に戻す

    // 必要ならノンカノニカル最小読み取りなどを調整:
    // raw.c_cc[VMIN] = 1;
    // raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
        perror("tcsetattr");
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    FILE *file;
    char* filename;
    bool trace = false;
    
    if (argc < 2) {
        printf("Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    if (argc==3) trace=true;

    filename = argv[1];
    uint32_t i = 0;

    file = fopen(filename, "r");
    if (file == NULL) {
        printf("Failed to open the file.\n");
        return 1;
    }

    // Skip the first line in the file
    char line[100];
    if (fgets(line, sizeof(line), file) == NULL) {
        printf("Failed to read the first line.\n");
        return 1;
    }

    while (fscanf(file, "%" SCNx64, (uint64_t*)&code[i]) == 1) {
        i++;
        if (i >= MAX_CODE_SIZE) {
            printf("Exceeded maximum code size.\n");
            break;
        }
    }

    fclose(file);
    // printf("Loaded %d instructions.\n", i);

    //set_raw_mode();    // Set terminal to raw mode
    //setup_async_keypress();    // Set up asynchronous keypress handling
    set_raw_mode_keep_signals();
    signal(SIGINT, my_handler);

    for (int i=0; i<256; i++) code[0x600000+i]=i;

    int64_t pc = 0;
    int64_t a, b, c;
    ctrlc=0;
    while (!die) {
        if (pc>=0x1000000 || pc<0) {
            fflush(stdout);
            printf("\r\n[pc] Out of bounds error. Exiting.\r\n");
            fflush(stdout);
            break;
        }
        a = code[pc];
        b = code[pc + 1];
        c = code[pc + 2];

        if (trace) {
            printf("PC: %06" PRIX64 " A: %" PRIX64 " (%" PRId64 ") B: %" PRIX64 " (%" PRId64 ") C: %" PRIX64 " (%" PRId64 ")\r\n", pc, a,a, b,b, c,c);
            fflush(stdout);
        }


        if (a>=0x1000000) {
        fflush(stdout);
            printf("\r\n[a]Out of bounds error. Exiting.\r\n");
        fflush(stdout);
            exit(1);
        }
        if (b>=0x1000000) {
        fflush(stdout);
            printf("\r\n[b]Out of bounds error. Exiting.\r\n");
        fflush(stdout);
            exit(1);
        }
        if (pc>=0x1000000) {
        fflush(stdout);
            printf("\r\n[c]Out of bounds error. Exiting.\r\n");
        fflush(stdout);
            exit(1);
        }



        if (b==-1) {      // -1 Write to console
            ssize_t tmp = write(STDOUT_FILENO, &code[a], 1); // Write the character to stdou  t
            fflush(stdout); // Ensure all buffered output is written to stdout            
            pc += 3;
            (void)tmp;
            continue;
        }
#if 0
        if (a==-1) {      // -1 Read from keyboard
            ctrlc = 0;
            int ch = ring_buffer_pop(&keypressBuffer);
            code[b] = ch;
            pc += 3;
            continue;
        }
#endif

        if (a==-1) {      // -1 Read from keyboard (blocking with select)
            ctrlc = 0;
            int ch = read_stdin_blocking();
            if (sigsetjmp(jmpbuf, 1) != 0) {
                // Ctrl-C シグナルでここに戻ってくる
                ch = 3;
            }
            if (ch == -1) {
                // EOF/エラーは -1 のまま格納
            } else {
                if (ch == 3) { // Ctrl-C
                    ctrlc = 1;
                    ctrlc_count++;
                    keypressBuffer.head=0;
                    keypressBuffer.tail=0;
                    if (ctrlc_count > 2) die = true;
                } else {
                    ctrlc_count = 0;
                }

                if (ch == 4) { // Ctrl-D: メモリダンプ
                    printf("\r\n");
                    for (int i=0x00ba00; i<0x00bb00; i+=16) {
                        printf("%06X: ", i);
                        fflush(stdout);
                        for (int j=0; j<16; j++) {
                            printf("%016" PRIX64 " ", code[i+j]);
                            fflush(stdout);
                        }
                        printf(" ");
                        fflush(stdout);
                        for (int j=0; j<16; j++) {
                            char ch2 = code[i+j];
                            if (ch2>31 && ch2<127) printf("%c", ch2);
                            else printf(".");
                            fflush(stdout);
                        }
                        printf("\r\n");
                        fflush(stdout);
                    }
                    fflush(stdout);
                }
            }
            code[b] = ch;
            pc += 3;
            continue;
        }

        if (a==-2) {      // -2 CTRL-C Check
            code[b] = ctrlc;
            ctrlc = 0;
            pc += 3;
            continue;
        }


        if (a<0) {
        fflush(stdout);
            printf("\r\n[a] Negative error. Exiting.\r\n");
        fflush(stdout);
            exit(1);
        }

        if (b<0) {
        fflush(stdout);
            printf("\r\n[b] Negative error. Exiting.\r\n");
        fflush(stdout);
            exit(1);
        }


        // Perform subtraction (64-bit wrap-around) on the NAND flash server
#if 0
        int64_t result = (int64_t)((uint64_t)code[b] - (uint64_t)code[a]);
#else
        Request req = {code[a], code[b]};
        Response resp = flashClient(req);
#endif
        if (b<0x600000) code[b] = resp.c;
#if 0
        if (code[b] <= 0) {
#else
        if (resp.is_jmp) {
#endif
            if (c==-1) {
                // printf("\r\nHALT\r\n");
                break;
                }
            pc = c;
        } else {
            pc += 3;
        }
    }
    
    reset_terminal_mode();
    // printf("\r\nExiting the subleq interpreter.\r\n");
    return 0;
}