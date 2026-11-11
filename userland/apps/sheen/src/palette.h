#ifndef SHEEN_PALETTE_H
#define SHEEN_PALETTE_H

/* The command palette's matching: a query's letters found in order in each row's label, the best matches first */
#include "picker.h"

#include <QList>
#include <QString>

#include <utility>

/* Marks where `query`'s letters fall in `text`, in order and ignoring case, preferring one unbroken run and then
 * letters that start words. Returns the match's score, higher for a better match, or -1 when a letter is missing */
int match_letters(const QString& text, const QString& query, QList<int>* marks);

/* The rows whose labels hold `query`'s letters, each with its index in `rows` and its marks, the best match first
 * and rows of equal score in their own order */
QList<std::pair<int, picker_row>> rank_rows(const QList<picker_row>& rows, const QString& query);

#endif
