#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

static const char *words[] = {
    "crane", "slate", "trace", "crate", "stare",
    "adore", "arose", "atone", "sauce", "raise",
    "snare", "arise", "ocean", "bloat", "chair",
    "clash", "dream", "feast", "ghost", "haven",
    "ivory", "joker", "knelt", "lemon", "mango",
    "nerve", "olive", "pearl", "queen", "robin",
    "steam", "torch", "ultra", "vivid", "waltz",
    "youth", "zebra", "blaze", "cliff", "dodge",
    "eagle", "flame", "grind", "haste", "image",
    "jolly", "knack", "lunar", "magic", "noble",
    "opera", "plumb", "quest", "ridge", "shine",
    "tidal", "unity", "vigor", "whirl", "xenon",
    "abort", "angel", "badge", "baker", "cabin",
    "candy", "dance", "decay", "elder", "elbow",
    "fairy", "fancy", "gauge", "glyph", "happy",
    "heart", "index", "irony", "jelly", "judge",
    "kayak", "label", "lance", "marsh", "medal",
    "night", "nylon", "onset", "outer", "panic",
    "patch", "quote", "radar", "ranch", "saint",
    "scale", "table", "taste", "udder", "upset",
    "valve", "vault", "waste", "watch", "yacht",
    "acute", "blown", "brace", "charm", "chess",
    "climb", "coral", "craft", "drift", "dwarf",
    "earth", "event", "fiber", "flesh", "flora",
    "forge", "frost", "globe", "grain", "grape",
    "guild", "hazel", "honor", "human", "humor",
    "juice", "kneel", "knife", "leapt", "light",
    "linen", "manor", "maple", "marsh", "minor",
    "model", "month", "moose", "nerve", "omega",
    "orbit", "oxide", "paint", "panel", "pearl",
    "phase", "piano", "pilot", "pixel", "plant",
    "plaza", "plume", "point", "polar", "pound",
    "power", "press", "price", "pride", "prime",
    "print", "prize", "proof", "prose", "proud",
    "proxy", "pulse", "pupil", "quail", "queen",
    "quiet", "raven", "rebel", "reign", "rinse",
    "rival", "roast", "royal", "ruler", "rural",
    "salad", "satin", "scope", "scout", "shark",
    "sheep", "shelf", "shell", "shirt", "shock",
    "shore", "shown", "siege", "sight", "since",
    "sixth", "skate", "skull", "slang", "sleep",
    "slice", "slide", "slope", "smart", "smell",
    "smile", "smoke", "snake", "solar", "solve",
    "sound", "south", "space", "spare", "spark",
    "speak", "speed", "spend", "spice", "spine",
    "spoke", "spoon", "sport", "spray", "squad",
    "staff", "stage", "stain", "stair", "stake",
    "stalk", "stand", "stark", "state", "steal",
    "steep", "stern", "stick", "still", "stock",
    "stone", "store", "storm", "story", "stove",
    "strap", "straw", "strip", "stuck", "study",
    "stuff", "stung", "style", "sugar", "suite",
    "super", "surge", "swamp", "swear", "sweep",
    "sweet", "swept", "swing", "syrup", "taste",
    "teach", "theft", "theme", "thick", "thing",
    "think", "thorn", "those", "three", "threw",
    "throw", "thumb", "tiger", "tired", "title",
    "token", "total", "touch", "tough", "towel",
    "tower", "toxic", "track", "trade", "trail",
    "train", "trait", "trash", "treat", "trend",
    "tribe", "trick", "tried", "troop", "trout",
    "truck", "truly", "trump", "trunk", "trust",
    "truth", "tulip", "tumor", "tuner", "twice",
    "under", "unfit", "union", "upper", "urban",
    "usage", "usual", "utter", "vague", "valid",
    "value", "verse", "video", "vinyl", "viral",
    "virus", "visit", "vital", "vivid", "vocal",
    "voice", "voter", "weary", "weave", "wedge",
    "weird", "wheat", "wheel", "where", "which",
    "while", "white", "whole", "widen", "woman",
    "world", "worry", "worse", "worst", "worth",
    "would", "wound", "wrist", "wrote", "yearn",
};

#define WORD_COUNT (int)(sizeof(words) / sizeof(words[0]))
#define WORD_LEN 5
#define MAX_GUESSES 6

#define CLR_RESET  "\x1b[0m"
#define CLR_GREEN  "\x1b[30;42m"
#define CLR_YELLOW "\x1b[30;43m"
#define CLR_GRAY   "\x1b[37;100m"

static void seed_rng(void) {
    unsigned int seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, &seed, sizeof(seed));
        close(fd);
    }
    if (seed == 0) seed = 54321;
    srand(seed);
}

static void to_lower(char *s) {
    for (int i = 0; s[i]; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32;
    }
}

static void print_guess(const char *guess, const char *answer) {
    int answer_used[WORD_LEN] = {0};
    int result[WORD_LEN];

    for (int i = 0; i < WORD_LEN; i++) {
        if (guess[i] == answer[i]) {
            result[i] = 2;
            answer_used[i] = 1;
        } else {
            result[i] = 0;
        }
    }

    for (int i = 0; i < WORD_LEN; i++) {
        if (result[i] == 2) continue;
        for (int j = 0; j < WORD_LEN; j++) {
            if (!answer_used[j] && guess[i] == answer[j]) {
                result[i] = 1;
                answer_used[j] = 1;
                break;
            }
        }
    }

    printf("  ");
    for (int i = 0; i < WORD_LEN; i++) {
        const char *clr = CLR_GRAY;
        if (result[i] == 2) clr = CLR_GREEN;
        else if (result[i] == 1) clr = CLR_YELLOW;
        printf("%s %c %s", clr, guess[i] - 32, CLR_RESET);
    }
    printf("\n");
}

static void show_alphabet(const char guesses[][WORD_LEN + 1], int nguesses,
                          const char *answer) {
    int status[26];
    memset(status, -1, sizeof(status));

    for (int g = 0; g < nguesses; g++) {
        int used[WORD_LEN] = {0};
        for (int i = 0; i < WORD_LEN; i++) {
            int idx = guesses[g][i] - 'a';
            if (guesses[g][i] == answer[i]) {
                status[idx] = 2;
                used[i] = 1;
            }
        }
        for (int i = 0; i < WORD_LEN; i++) {
            int idx = guesses[g][i] - 'a';
            if (status[idx] == 2) continue;
            int found = 0;
            for (int j = 0; j < WORD_LEN; j++) {
                if (!used[j] && guesses[g][i] == answer[j]) {
                    found = 1;
                    used[j] = 1;
                    break;
                }
            }
            if (found && status[idx] < 1) status[idx] = 1;
            else if (!found && status[idx] < 0) status[idx] = 0;
        }
    }

    printf("  ");
    for (int i = 0; i < 26; i++) {
        char c = 'A' + i;
        if (status[i] == 2) printf("%s%c%s", CLR_GREEN, c, CLR_RESET);
        else if (status[i] == 1) printf("%s%c%s", CLR_YELLOW, c, CLR_RESET);
        else if (status[i] == 0) printf("%s%c%s", CLR_GRAY, c, CLR_RESET);
        else printf("%c", c);
    }
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    seed_rng();

    char input[64];

    for (;;) {
        const char *answer = words[rand() % WORD_COUNT];
        char guesses[MAX_GUESSES][WORD_LEN + 1];
        int nguesses = 0;
        int won = 0;

        printf("\n  === WORDLE ===\n");
        printf("  Guess the 5-letter word (%d attempts)\n\n", MAX_GUESSES);

        while (nguesses < MAX_GUESSES) {
            printf("  Guess %d/%d: ", nguesses + 1, MAX_GUESSES);
            if (!fgets(input, sizeof(input), stdin)) return 0;

            size_t len = strlen(input);
            while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r'))
                input[--len] = '\0';

            to_lower(input);

            if ((int)len != WORD_LEN) {
                printf("  Must be %d letters.\n", WORD_LEN);
                continue;
            }

            int valid = 1;
            for (int i = 0; i < WORD_LEN; i++) {
                if (input[i] < 'a' || input[i] > 'z') { valid = 0; break; }
            }
            if (!valid) {
                printf("  Letters only.\n");
                continue;
            }

            memcpy(guesses[nguesses], input, WORD_LEN + 1);
            nguesses++;

            printf("\n");
            for (int i = 0; i < nguesses; i++) {
                print_guess(guesses[i], answer);
            }
            printf("\n");
            show_alphabet(guesses, nguesses, answer);
            printf("\n");

            if (strcmp(input, answer) == 0) {
                printf("  You got it in %d!\n", nguesses);
                won = 1;
                break;
            }
        }

        if (!won) {
            printf("  The word was: %s\n", answer);
        }

        printf("\n  Play again? (y/n) ");
        if (!fgets(input, sizeof(input), stdin)) break;
        if (input[0] != 'y' && input[0] != 'Y') break;
    }

    printf("\n  Thanks for playing!\n\n");
    return 0;
}
