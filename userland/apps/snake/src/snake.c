#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>

#define STLX_TCSETS_RAW    0x5401
#define STLX_TCSETS_COOKED 0x5402

#define W 40
#define H 20
#define MAX_LEN (W * H)

static int sx[MAX_LEN], sy[MAX_LEN];
static int slen, dir, fx, fy, score, alive;

static void seed_rng(void) {
    unsigned int seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { read(fd, &seed, sizeof(seed)); close(fd); }
    if (seed == 0) seed = 11111;
    srand(seed);
}

static void place_food(void) {
    int ok;
    do {
        fx = 1 + rand() % (W - 2);
        fy = 1 + rand() % (H - 2);
        ok = 1;
        for (int i = 0; i < slen; i++) {
            if (sx[i] == fx && sy[i] == fy) { ok = 0; break; }
        }
    } while (!ok);
}

static void draw(void) {
    write(1, "\x1b[H", 3);

    char buf[4096];
    int pos = 0;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            char ch = ' ';
            if (y == 0 || y == H - 1) ch = '-';
            else if (x == 0 || x == W - 1) ch = '|';
            else if (x == fx && y == fy) ch = '*';
            else {
                for (int i = 0; i < slen; i++) {
                    if (sx[i] == x && sy[i] == y) {
                        ch = (i == 0) ? '@' : 'o';
                        break;
                    }
                }
            }
            buf[pos++] = ch;
        }
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }
    write(1, buf, pos);

    char info[64];
    int n = snprintf(info, sizeof(info), "  Score: %d  |  WASD to move, Q to quit\r\n", score);
    write(1, info, n);
}

static void step(void) {
    int nx = sx[0], ny = sy[0];
    switch (dir) {
        case 0: ny--; break;
        case 1: ny++; break;
        case 2: nx--; break;
        case 3: nx++; break;
    }

    if (nx <= 0 || nx >= W - 1 || ny <= 0 || ny >= H - 1) {
        alive = 0;
        return;
    }

    for (int i = 1; i < slen; i++) {
        if (sx[i] == nx && sy[i] == ny) {
            alive = 0;
            return;
        }
    }

    int ate = (nx == fx && ny == fy);

    if (!ate) {
        for (int i = slen - 1; i > 0; i--) {
            sx[i] = sx[i - 1];
            sy[i] = sy[i - 1];
        }
    } else {
        for (int i = slen; i > 0; i--) {
            sx[i] = sx[i - 1];
            sy[i] = sy[i - 1];
        }
        slen++;
        score += 10;
    }

    sx[0] = nx;
    sy[0] = ny;

    if (ate) {
        place_food();
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    seed_rng();

    ioctl(0, STLX_TCSETS_RAW, 0);
    fcntl(0, F_SETFL, O_NONBLOCK);

    char input[32];

    for (;;) {
        write(1, "\x1b[2J\x1b[H", 7);
        write(1, "\r\n  === SNAKE ===\r\n\r\n", 21);
        write(1, "  Press any key to start (Q to quit)...\r\n", 41);

        for (;;) {
            char c;
            if (read(0, &c, 1) == 1) {
                if (c == 'q' || c == 'Q') goto done;
                break;
            }
            struct timespec ts = {0, 50000000};
            nanosleep(&ts, NULL);
        }

        sx[0] = W / 2; sy[0] = H / 2;
        slen = 3;
        for (int i = 1; i < slen; i++) { sx[i] = sx[0] - i; sy[i] = sy[0]; }
        dir = 3;
        score = 0;
        alive = 1;
        place_food();

        while (alive) {
            char c;
            while (read(0, &c, 1) == 1) {
                switch (c) {
                    case 'w': case 'W': if (dir != 1) dir = 0; break;
                    case 's': case 'S': if (dir != 0) dir = 1; break;
                    case 'a': case 'A': if (dir != 3) dir = 2; break;
                    case 'd': case 'D': if (dir != 2) dir = 3; break;
                    case 'q': case 'Q': alive = 0; goto gameover;
                }
            }

            step();
            if (alive) draw();

            struct timespec ts = {0, 120000000};
            nanosleep(&ts, NULL);
        }

gameover:
        write(1, "\x1b[2J\x1b[H", 7);
        {
            char msg[80];
            int n = snprintf(msg, sizeof(msg),
                "\r\n  GAME OVER!  Score: %d\r\n\r\n  Play again? (y/n) ", score);
            write(1, msg, n);
        }

        fcntl(0, F_SETFL, 0);
        ioctl(0, STLX_TCSETS_COOKED, 0);
        if (!fgets(input, sizeof(input), stdin)) break;
        if (input[0] != 'y' && input[0] != 'Y') break;
        ioctl(0, STLX_TCSETS_RAW, 0);
        fcntl(0, F_SETFL, O_NONBLOCK);
    }

done:
    fcntl(0, F_SETFL, 0);
    ioctl(0, STLX_TCSETS_COOKED, 0);
    write(1, "\x1b[2J\x1b[H", 7);
    write(1, "\r\n  Thanks for playing!\r\n\r\n", 26);
    return 0;
}
