#include "palette.h"

#include <algorithm>

static constexpr int RUN_SCORE = 1000;
static constexpr int WORD_START_SCORE = 100;
static constexpr int LETTER_SCORE = 10;

static bool starts_word(const QString& text, qsizetype at) {
    return at == 0 || !text.at(at - 1).isLetterOrNumber();
}

// Each letter takes the next start of a word holding it when `prefers_words`, else its next occurrence
static int match_scattered(const QString& text, const QString& query, bool prefers_words, QList<int>* marks) {
    int score = 0;
    qsizetype from = 0;
    for (QChar letter : query) {
        qsizetype at = -1;
        for (qsizetype index = from; prefers_words && index < text.size(); index++) {
            if (text.at(index).toCaseFolded() == letter.toCaseFolded() && starts_word(text, index)) {
                at = index;
                break;
            }
        }

        if (at < 0) {
            at = text.indexOf(letter, from, Qt::CaseInsensitive);
        }

        if (at < 0) {
            marks->clear();
            return -1;
        }

        score += starts_word(text, at) ? WORD_START_SCORE : LETTER_SCORE;
        marks->append(static_cast<int>(at));
        from = at + 1;
    }

    return score;
}

int match_letters(const QString& text, const QString& query, QList<int>* marks) {
    marks->clear();
    if (query.isEmpty()) {
        return 0;
    }

    qsizetype run = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (run >= 0) {
        for (qsizetype letter = 0; letter < query.size(); letter++) {
            marks->append(static_cast<int>(run + letter));
        }

        return RUN_SCORE + (starts_word(text, run) ? WORD_START_SCORE : 0) - static_cast<int>(run);
    }

    int score = match_scattered(text, query, true, marks);
    return score >= 0 ? score : match_scattered(text, query, false, marks);
}

QList<std::pair<int, picker_row>> rank_rows(const QList<picker_row>& rows, const QString& query) {
    QList<std::pair<int, picker_row>> ranked;
    QList<int> scores;
    for (int index = 0; index < rows.size(); index++) {
        picker_row row = rows.at(index);
        int score = match_letters(row.label, query, &row.marks);
        if (score >= 0) {
            ranked.append({ index, row });
            scores.append(score);
        }
    }

    QList<int> order;
    for (int position = 0; position < ranked.size(); position++) {
        order.append(position);
    }

    std::stable_sort(order.begin(), order.end(), [&scores](int left, int right) {
        return scores.at(left) > scores.at(right);
    });

    QList<std::pair<int, picker_row>> sorted;
    for (int position : order) {
        sorted.append(ranked.at(position));
    }

    return sorted;
}
