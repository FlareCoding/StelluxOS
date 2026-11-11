#ifndef SHEEN_KILN_H
#define SHEEN_KILN_H

/* The look of Sheen's text area and the languages it colors. Kiln is the syntax palette, and each Lexilla style
 * takes its color from the tags the lexer publishes for it, or from the language's own table. */
#include "settings.h"

#include <QByteArrayView>
#include <QList>
#include <QString>

#include <Scintilla.h>

#include <cstdint>

class ScintillaEditBase;

struct language;

/* The text's em size in pixels for every view made after it is set */
void set_text_size(float em_px);

void apply_text_area_look(ScintillaEditBase* view);

/* Widens the line numbers for the document's line count and zoom, never below the gutter's design width */
void fit_line_numbers(ScintillaEditBase* view);

/* The language of a file by its name, then by the program its first line names after #!, null for plain text */
const language* find_language(const QString& file_name, QByteArrayView first_line);

/* Sets the lexer, keywords and colors of `lang` on `view`, null clearing them for plain text */
void apply_language(ScintillaEditBase* view, const language* lang);

QString language_name(const language* lang);

/* A theme color as Scintilla takes it, 0xAABBGGRR */
sptr_t scintilla_rgba(uint32_t argb);

/* Defines `marker` to tint the lines it marks with `argb`, under their text */
void define_line_tint(ScintillaEditBase* view, int marker, uint32_t argb);

/* Shows or hides the fold chevrons, which Code mode has and Text mode and large files go without */
void set_folding_shown(ScintillaEditBase* view, bool is_shown);

/* Shows or hides the gutter's line numbers. The unsaved bars stay either way */
void set_line_numbers_shown(ScintillaEditBase* view, bool is_shown);
bool are_line_numbers_shown(ScintillaEditBase* view);

/* Sets the numbers, guides, edge, wrap and reading width a mode starts a tab with */
void apply_mode_defaults(ScintillaEditBase* view, const mode_defaults& defaults);
void fit_reading_width(ScintillaEditBase* view, int wrap_column);

void mark_function_names(ScintillaEditBase* view, const language* lang);
void clear_function_names(ScintillaEditBase* view);

/* Whether a language's files open in Code mode, every lexed language but Markdown */
bool is_code_language(const language* lang);

/* Every language Sheen colors, in the order a menu lists them */
QList<const language*> all_languages();

#endif
