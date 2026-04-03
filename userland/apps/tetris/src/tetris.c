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

#define BW 10
#define BH 20
#define CELL_EMPTY 0

static int board[BH][BW];
static int score, lines_cleared, level, alive;

static const int pieces[7][4][4][2] = {
    /* I */ {{{0,0},{1,0},{2,0},{3,0}}, {{0,0},{0,1},{0,2},{0,3}}, {{0,0},{1,0},{2,0},{3,0}}, {{0,0},{0,1},{0,2},{0,3}}},
    /* O */ {{{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}},
    /* T */ {{{0,0},{1,0},{2,0},{1,1}}, {{0,0},{0,1},{0,2},{1,1}}, {{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{0,1}}},
    /* S */ {{{1,0},{2,0},{0,1},{1,1}}, {{0,0},{0,1},{1,1},{1,2}}, {{1,0},{2,0},{0,1},{1,1}}, {{0,0},{0,1},{1,1},{1,2}}},
    /* Z */ {{{0,0},{1,0},{1,1},{2,1}}, {{1,0},{0,1},{1,1},{0,2}}, {{0,0},{1,0},{1,1},{2,1}}, {{1,0},{0,1},{1,1},{0,2}}},
    /* L */ {{{0,0},{0,1},{1,1},{2,1}}, {{0,0},{1,0},{0,1},{0,2}}, {{0,0},{1,0},{2,0},{2,1}}, {{1,0},{1,1},{0,2},{1,2}}},
    /* J */ {{{2,0},{0,1},{1,1},{2,1}}, {{0,0},{0,1},{0,2},{1,2}}, {{0,0},{1,0},{2,0},{0,1}}, {{0,0},{1,0},{1,1},{1,2}}},
};

static const char piece_chars[] = "IOTSZ LJ";

static int cur_piece, cur_rot, cur_x, cur_y;
static int next_piece;

static void seed_rng(void) {
    unsigned int seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { read(fd, &seed, sizeof(seed)); close(fd); }
    if (seed == 0) seed = 77777;
    srand(seed);
}

static int fits(int piece, int rot, int px, int py) {
    for (int i = 0; i < 4; i++) {
        int x = px + pieces[piece][rot][i][0];
        int y = py + pieces[piece][rot][i][1];
        if (x < 0 || x >= BW || y < 0 || y >= BH) return 0;
        if (board[y][x] != CELL_EMPTY) return 0;
    }
    return 1;
}

static void lock_piece(void) {
    for (int i = 0; i < 4; i++) {
        int x = cur_x + pieces[cur_piece][cur_rot][i][0];
        int y = cur_y + pieces[cur_piece][cur_rot][i][1];
        if (y >= 0 && y < BH && x >= 0 && x < BW) {
            board[y][x] = cur_piece + 1;
        }
    }
}

static void clear_lines(void) {
    int cleared = 0;
    for (int y = BH - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < BW; x++) {
            if (board[y][x] == CELL_EMPTY) { full = 0; break; }
        }
        if (full) {
            cleared++;
            for (int yy = y; yy > 0; yy--) {
                memcpy(board[yy], board[yy - 1], sizeof(board[0]));
            }
            memset(board[0], 0, sizeof(board[0]));
            y++;
        }
    }
    if (cleared > 0) {
        static const int scores[] = {0, 100, 300, 500, 800};
        score += scores[cleared] * (level + 1);
        lines_cleared += cleared;
        level = lines_cleared / 10;
    }
}

static void spawn_piece(void) {
    cur_piece = next_piece;
    next_piece = rand() % 7;
    cur_rot = 0;
    cur_x = BW / 2 - 1;
    cur_y = 0;
    if (!fits(cur_piece, cur_rot, cur_x, cur_y)) {
        alive = 0;
    }
}

static void draw(void) {
    write(1, "\x1b[H", 3);

    char buf[4096];
    int pos = 0;

    for (int y = 0; y < BH; y++) {
        buf[pos++] = ' '; buf[pos++] = '|';
        for (int x = 0; x < BW; x++) {
            char ch = ' ';

            if (board[y][x] != CELL_EMPTY) {
                ch = '#';
            } else {
                for (int i = 0; i < 4; i++) {
                    int px = cur_x + pieces[cur_piece][cur_rot][i][0];
                    int py = cur_y + pieces[cur_piece][cur_rot][i][1];
                    if (px == x && py == y) {
                        ch = piece_chars[cur_piece];
                        break;
                    }
                }
            }
            buf[pos++] = ch;
        }
        buf[pos++] = '|';

        if (y == 1) {
            int n = snprintf(buf + pos, 30, "  Score: %d", score);
            pos += n;
        } else if (y == 3) {
            int n = snprintf(buf + pos, 30, "  Level: %d", level);
            pos += n;
        } else if (y == 5) {
            int n = snprintf(buf + pos, 30, "  Lines: %d", lines_cleared);
            pos += n;
        } else if (y == 8) {
            int n = snprintf(buf + pos, 30, "  Next: %c", piece_chars[next_piece]);
            pos += n;
        } else if (y == 11) {
            int n = snprintf(buf + pos, 30, "  A/D  Move");
            pos += n;
        } else if (y == 12) {
            int n = snprintf(buf + pos, 30, "  W    Rotate");
            pos += n;
        } else if (y == 13) {
            int n = snprintf(buf + pos, 30, "  S    Drop");
            pos += n;
        } else if (y == 14) {
            int n = snprintf(buf + pos, 30, "  Q    Quit");
            pos += n;
        }

        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }

    buf[pos++] = ' '; buf[pos++] = '+';
    for (int x = 0; x < BW; x++) buf[pos++] = '-';
    buf[pos++] = '+'; buf[pos++] = '\r'; buf[pos++] = '\n';

    write(1, buf, pos);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    seed_rng();

    ioctl(0, STLX_TCSETS_RAW, 0);
    fcntl(0, F_SETFL, O_NONBLOCK);

    char input[32];

    for (;;) {
        write(1, "\x1b[2J\x1b[H", 7);
        write(1, "\r\n  === TETRIS ===\r\n\r\n", 22);
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

        memset(board, 0, sizeof(board));
        score = 0;
        lines_cleared = 0;
        level = 0;
        alive = 1;
        next_piece = rand() % 7;
        spawn_piece();

        int tick = 0;
        int drop_interval = 8;

        while (alive) {
            char c;
            while (read(0, &c, 1) == 1) {
                switch (c) {
                    case 'a': case 'A':
                        if (fits(cur_piece, cur_rot, cur_x - 1, cur_y)) cur_x--;
                        break;
                    case 'd': case 'D':
                        if (fits(cur_piece, cur_rot, cur_x + 1, cur_y)) cur_x++;
                        break;
                    case 'w': case 'W': {
                        int nr = (cur_rot + 1) % 4;
                        if (fits(cur_piece, nr, cur_x, cur_y)) cur_rot = nr;
                        break;
                    }
                    case 's': case 'S':
                        while (fits(cur_piece, cur_rot, cur_x, cur_y + 1)) cur_y++;
                        lock_piece();
                        clear_lines();
                        spawn_piece();
                        tick = 0;
                        break;
                    case 'q': case 'Q':
                        alive = 0;
                        goto gameover;
                }
            }

            tick++;
            drop_interval = (level < 9) ? (8 - level) : 1;
            if (drop_interval < 1) drop_interval = 1;

            if (tick >= drop_interval) {
                tick = 0;
                if (fits(cur_piece, cur_rot, cur_x, cur_y + 1)) {
                    cur_y++;
                } else {
                    lock_piece();
                    clear_lines();
                    spawn_piece();
                }
            }

            if (alive) draw();

            struct timespec ts = {0, 80000000};
            nanosleep(&ts, NULL);
        }

gameover:
        write(1, "\x1b[2J\x1b[H", 7);
        {
            char msg[128];
            int n = snprintf(msg, sizeof(msg),
                "\r\n  GAME OVER!\r\n  Score: %d  Level: %d  Lines: %d\r\n\r\n  Play again? (y/n) ",
                score, level, lines_cleared);
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
