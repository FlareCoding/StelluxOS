#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static const char *words[] = {
    "kernel", "memory", "socket", "thread", "signal",
    "buffer", "driver", "process", "system", "binary",
    "cipher", "stream", "packet", "bridge", "switch",
    "router", "server", "client", "daemon", "module",
    "python", "rustic", "clojure", "golang", "erlang",
    "quartz", "zephyr", "enigma", "vortex", "galaxy",
    "pirate", "knight", "wizard", "dragon", "castle",
    "forest", "jungle", "desert", "island", "cavern",
    "potion", "scroll", "shield", "falcon", "hunter",
    "shadow", "riddle", "legend", "oracle", "throne",
};

#define WORD_COUNT (int)(sizeof(words) / sizeof(words[0]))
#define MAX_WRONG 6

static const char *hangman_art[] = {
    "  +---+\n"
    "  |   |\n"
    "      |\n"
    "      |\n"
    "      |\n"
    "      |\n"
    "=========",

    "  +---+\n"
    "  |   |\n"
    "  O   |\n"
    "      |\n"
    "      |\n"
    "      |\n"
    "=========",

    "  +---+\n"
    "  |   |\n"
    "  O   |\n"
    "  |   |\n"
    "      |\n"
    "      |\n"
    "=========",

    "  +---+\n"
    "  |   |\n"
    "  O   |\n"
    " /|   |\n"
    "      |\n"
    "      |\n"
    "=========",

    "  +---+\n"
    "  |   |\n"
    "  O   |\n"
    " /|\\  |\n"
    "      |\n"
    "      |\n"
    "=========",

    "  +---+\n"
    "  |   |\n"
    "  O   |\n"
    " /|\\  |\n"
    " /    |\n"
    "      |\n"
    "=========",

    "  +---+\n"
    "  |   |\n"
    "  O   |\n"
    " /|\\  |\n"
    " / \\  |\n"
    "      |\n"
    "=========",
};

static void seed_rng(void) {
    unsigned int seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, &seed, sizeof(seed));
        close(fd);
    }
    if (seed == 0) seed = 67890;
    srand(seed);
}

static void show_word(const char *word, const int *revealed) {
    int len = (int)strlen(word);
    printf("  Word: ");
    for (int i = 0; i < len; i++) {
        if (revealed[i]) printf("%c ", word[i]);
        else printf("_ ");
    }
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    seed_rng();

    char input[32];
    int wins = 0, losses = 0;

    for (;;) {
        const char *word = words[rand() % WORD_COUNT];
        int len = (int)strlen(word);
        int revealed[32] = {0};
        char guessed[27] = {0};
        int nguessed = 0;
        int wrong = 0;
        int remaining = len;

        printf("\n  === HANGMAN ===\n\n");

        while (wrong < MAX_WRONG && remaining > 0) {
            printf("\n%s\n\n", hangman_art[wrong]);
            show_word(word, revealed);
            printf("  Guessed: ");
            for (int i = 0; i < nguessed; i++) printf("%c ", guessed[i]);
            printf("\n");
            printf("  (%d wrong, %d left)\n", wrong, MAX_WRONG - wrong);
            printf("\n  Guess a letter: ");

            if (!fgets(input, sizeof(input), stdin)) return 0;

            char c = input[0];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c < 'a' || c > 'z') {
                printf("  Enter a letter.\n");
                continue;
            }

            int already = 0;
            for (int i = 0; i < nguessed; i++) {
                if (guessed[i] == c) { already = 1; break; }
            }
            if (already) {
                printf("  Already guessed '%c'.\n", c);
                continue;
            }

            guessed[nguessed++] = c;

            int hit = 0;
            for (int i = 0; i < len; i++) {
                if (word[i] == c && !revealed[i]) {
                    revealed[i] = 1;
                    remaining--;
                    hit = 1;
                }
            }

            if (!hit) {
                wrong++;
                printf("  '%c' is not in the word.\n", c);
            }
        }

        printf("\n%s\n\n", hangman_art[wrong]);
        show_word(word, revealed);

        if (remaining == 0) {
            printf("\n  You saved them! The word was: %s\n", word);
            wins++;
        } else {
            printf("\n  Hanged! The word was: %s\n", word);
            losses++;
        }

        printf("  Score: %dW-%dL\n", wins, losses);
        printf("\n  Play again? (y/n) ");
        if (!fgets(input, sizeof(input), stdin)) break;
        if (input[0] != 'y' && input[0] != 'Y') break;
    }

    printf("\n  Thanks for playing!\n\n");
    return 0;
}
