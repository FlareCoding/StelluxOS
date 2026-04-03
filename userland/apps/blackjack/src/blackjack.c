#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int deck[52];
static int deck_pos;

static void seed_rng(void) {
    unsigned int seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, &seed, sizeof(seed));
        close(fd);
    }
    if (seed == 0) seed = 12345;
    srand(seed);
}

static void shuffle(void) {
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 51; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }
    deck_pos = 0;
}

static int deal(void) {
    if (deck_pos >= 52) shuffle();
    return deck[deck_pos++];
}

static int card_value(int card) {
    int rank = card % 13;
    if (rank >= 10) return 10;
    if (rank == 0) return 11;
    return rank + 1;
}

static const char *rank_str(int card) {
    static const char *ranks[] = {
        "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"
    };
    return ranks[card % 13];
}

static const char *suit_str(int card) {
    static const char *suits[] = { "S", "H", "D", "C" };
    return suits[card / 13];
}

static int hand_score(const int *hand, int n) {
    int total = 0, aces = 0;
    for (int i = 0; i < n; i++) {
        total += card_value(hand[i]);
        if (hand[i] % 13 == 0) aces++;
    }
    while (total > 21 && aces > 0) {
        total -= 10;
        aces--;
    }
    return total;
}

static void show_hand(const char *name, const int *hand, int n, int hide_first) {
    printf("  %s: ", name);
    for (int i = 0; i < n; i++) {
        if (i == 0 && hide_first) {
            printf("[??] ");
        } else {
            printf("[%s%s] ", rank_str(hand[i]), suit_str(hand[i]));
        }
    }
    if (!hide_first) {
        printf("(%d)", hand_score(hand, n));
    }
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    seed_rng();

    int wins = 0, losses = 0, pushes = 0;
    char input[32];

    printf("\n  === BLACKJACK ===\n\n");

    for (;;) {
        shuffle();

        int player[12], dealer[12];
        int pn = 0, dn = 0;

        player[pn++] = deal();
        dealer[dn++] = deal();
        player[pn++] = deal();
        dealer[dn++] = deal();

        int player_bust = 0;

        for (;;) {
            printf("\n");
            show_hand("Dealer", dealer, dn, 1);
            show_hand("You   ", player, pn, 0);

            int ps = hand_score(player, pn);
            if (ps == 21) {
                printf("  Blackjack!\n");
                break;
            }

            printf("\n  (h)it or (s)tand? ");
            if (!fgets(input, sizeof(input), stdin)) return 0;

            if (input[0] == 'h' || input[0] == 'H') {
                player[pn++] = deal();
                ps = hand_score(player, pn);
                if (ps > 21) {
                    printf("\n");
                    show_hand("Dealer", dealer, dn, 1);
                    show_hand("You   ", player, pn, 0);
                    printf("  BUST!\n");
                    player_bust = 1;
                    break;
                }
            } else {
                break;
            }
        }

        if (!player_bust) {
            while (hand_score(dealer, dn) < 17) {
                dealer[dn++] = deal();
            }
        }

        printf("\n  --- Result ---\n");
        show_hand("Dealer", dealer, dn, 0);
        show_hand("You   ", player, pn, 0);

        int ps = hand_score(player, pn);
        int ds = hand_score(dealer, dn);

        if (player_bust) {
            printf("  You lose.\n");
            losses++;
        } else if (ds > 21) {
            printf("  Dealer busts! You win!\n");
            wins++;
        } else if (ps > ds) {
            printf("  You win!\n");
            wins++;
        } else if (ps < ds) {
            printf("  You lose.\n");
            losses++;
        } else {
            printf("  Push.\n");
            pushes++;
        }

        printf("  Score: %dW-%dL-%dP\n", wins, losses, pushes);
        printf("\n  Play again? (y/n) ");
        if (!fgets(input, sizeof(input), stdin)) break;
        if (input[0] != 'y' && input[0] != 'Y') break;
    }

    printf("\n  Thanks for playing!\n\n");
    return 0;
}
