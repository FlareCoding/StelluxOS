#ifndef SHEEN_KILN_H
#define SHEEN_KILN_H

/* The look of Sheen's text area and the languages it colors. Kiln is the syntax palette, and each Lexilla style
 * takes its color from the tags the lexer publishes for it, or from the language's own table. */
#include <QByteArrayView>
#include <QList>
#include <QString>

class ScintillaEditBase;

struct language;

void apply_text_area_look(ScintillaEditBase* view);

/* Widens the line numbers for the document's line count and zoom, never below the gutter's design width */
void fit_line_numbers(ScintillaEditBase* view);

/* The language of a file by its name, then by the program its first line names after #!, null for plain text */
const language* find_language(const QString& file_name, QByteArrayView first_line);

/* Sets the lexer, keywords and colors of `lang` on `view`, null clearing them for plain text */
void apply_language(ScintillaEditBase* view, const language* lang);

QString language_name(const language* lang);

/* Shows or hides the gutter's line numbers. The unsaved bars stay either way */
void set_line_numbers_shown(ScintillaEditBase* view, bool is_shown);
bool are_line_numbers_shown(ScintillaEditBase* view);

/* Every language Sheen colors, in the order a menu lists them */
QList<const language*> all_languages();

#endif
