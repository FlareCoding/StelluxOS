#include "kiln.h"

#include <QFileInfo>
#include <QList>
#include <QRegularExpression>

#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>
#include <Scintilla.h>
#include <ScintillaEditBase.h>

#include <stlxtheme/theme.h>

#include <cstdint>

enum class syntax_role { IDENTIFIER, KEYWORD, TYPE, STRING, NUMBER, PREPROCESSOR, COMMENT, OPERATOR };

struct style_role {
    int style;
    syntax_role role;
};

struct language_property {
    const char* key;
    const char* value;
};

struct language {
    const char* name;                    // shown in the status bar
    const char* lexer;                   // Lexilla's name for the lexer that styles it
    const char* file_patterns;           // space separated wildcards matched against the file name
    const char* interpreters;            // space separated programs a #! first line may name
    const char* keywords[4];             // Lexilla's keyword sets, in the order the lexer numbers them
    QList<style_role> roles;             // where the lexer's own tags give no role or too broad a one
    QList<language_property> properties;
};

// Kiln, the syntax palette
static constexpr uint32_t KILN_CLAY = 0xFFE8917C;
static constexpr uint32_t KILN_ORCHID = 0xFFD3A5CC;
static constexpr uint32_t KILN_SAGE = 0xFFB4C38A;
static constexpr uint32_t KILN_VERDIGRIS = 0xFF8FBFAE;
static constexpr uint32_t KILN_BRONZE = 0xFFC4A88E;
static constexpr uint32_t KILN_ASH = 0xFF857B6E;

static constexpr uint32_t BRASS = 0xFFE2C496;
static constexpr uint32_t CREAM = 0xFFFFF0D7;
static constexpr double CARET_LINE_ALPHA = 0.045;
static constexpr double INDENT_GUIDE_ALPHA = 0.10;
static constexpr double EDGE_ALPHA = 0.07;

static float s_text_size = STLXTHEME_TERMINAL_SIZE;

static constexpr double POINTS_PER_INCH = 72;
static constexpr double SCREEN_DPI = 96;
static constexpr int CARET_WIDTH = 2;
static constexpr int CARET_BLINK_MS = 500;
static constexpr int TAB_WIDTH = 4;
static constexpr int EDGE_COLUMN = 120;

// The 64 px gutter: right aligned numbers, a gap, the 2 px unsaved bar and 7 px before the text's own 8 px inset
static constexpr int GUTTER_WIDTH = 64;
static constexpr int GUTTER_BAR_GAP = 7;
static constexpr int GUTTER_NUMBER_GAP = 11;
static constexpr int TEXT_INSET = 8;

// Scintilla's room right of a line number, and the room the design leaves left of a four digit one
static constexpr int NUMBER_PADDING = 3;
static constexpr int NUMBER_LEFT_ROOM = 6;

enum gutter_margin { NUMBER_MARGIN, NUMBER_GAP_MARGIN, CHANGE_BAR_MARGIN, BAR_GAP_MARGIN, GUTTER_MARGIN_COUNT };

static const char* const C_KEYWORDS =
    "alignas alignof asm auto break case catch class concept const consteval constexpr constinit const_cast "
    "continue co_await co_return co_yield decltype default delete do dynamic_cast else enum explicit export extern "
    "for friend goto if inline mutable namespace new noexcept operator private protected public register "
    "reinterpret_cast requires return sizeof static static_assert static_cast struct switch template this "
    "thread_local throw try typedef typeid typename union using virtual volatile while";

static const char* const C_TYPES =
    "bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t ssize_t "
    "ptrdiff_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t uintptr_t intptr_t";

static const char* const C_CONSTANTS = "true false nullptr NULL EOF";

static const char* const JS_KEYWORDS =
    "async await break case catch class const continue debugger default delete do else export extends finally for "
    "from function get if import in instanceof let new of return set static super switch this throw try typeof var "
    "void while with yield";

static const char* const JS_TYPES =
    "any bigint boolean declare enum implements interface keyof namespace never number object readonly string "
    "symbol type unknown";

static const char* const JS_CONSTANTS = "true false null undefined NaN Infinity";

static const char* const PYTHON_KEYWORDS =
    "False None True and as assert async await break case class continue def del elif else except finally for from "
    "global if import in is lambda match nonlocal not or pass raise return try while with yield";

static const char* const PYTHON_BUILTINS =
    "bool bytearray bytes dict float frozenset int list object set str tuple type abs all any enumerate isinstance "
    "len max min open print range repr sorted sum super zip self cls";

static const char* const SHELL_KEYWORDS =
    "if then else elif fi case esac for select while until do done in function time return break continue exit "
    "export local readonly declare set unset shift source eval exec trap wait echo printf read cd test true false";

static const char* const RUST_KEYWORDS =
    "as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move mut "
    "pub ref return self Self static struct super trait type unsafe use where while";

static const char* const RUST_TYPES =
    "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize Box Option Result String Vec";

static const char* const LUA_KEYWORDS =
    "and break do else elseif end false for function goto if in local nil not or repeat return then true until while";

static const QList<style_role> C_ROLES = {
    { SCE_C_WORD2, syntax_role::TYPE },
    { SCE_C_GLOBALCLASS, syntax_role::NUMBER },
};

static const QList<style_role> PYTHON_ROLES = {
    { SCE_P_WORD2, syntax_role::TYPE },
    { SCE_P_DECORATOR, syntax_role::PREPROCESSOR },
};

static const QList<style_role> RUST_ROLES = {
    { SCE_RUST_WORD2, syntax_role::TYPE },
    { SCE_RUST_MACRO, syntax_role::PREPROCESSOR },
    { SCE_RUST_LIFETIME, syntax_role::TYPE },
};

static const QList<style_role> JSON_ROLES = {
    { SCE_JSON_NUMBER, syntax_role::NUMBER },
    { SCE_JSON_STRING, syntax_role::STRING },
    { SCE_JSON_STRINGEOL, syntax_role::STRING },
    { SCE_JSON_PROPERTYNAME, syntax_role::KEYWORD },
    { SCE_JSON_ESCAPESEQUENCE, syntax_role::STRING },
    { SCE_JSON_LINECOMMENT, syntax_role::COMMENT },
    { SCE_JSON_BLOCKCOMMENT, syntax_role::COMMENT },
    { SCE_JSON_OPERATOR, syntax_role::OPERATOR },
    { SCE_JSON_URI, syntax_role::STRING },
    { SCE_JSON_COMPACTIRI, syntax_role::STRING },
    { SCE_JSON_KEYWORD, syntax_role::NUMBER },
    { SCE_JSON_LDKEYWORD, syntax_role::KEYWORD },
};

static const QList<style_role> YAML_ROLES = {
    { SCE_YAML_COMMENT, syntax_role::COMMENT },
    { SCE_YAML_IDENTIFIER, syntax_role::KEYWORD },
    { SCE_YAML_KEYWORD, syntax_role::NUMBER },
    { SCE_YAML_NUMBER, syntax_role::NUMBER },
    { SCE_YAML_REFERENCE, syntax_role::PREPROCESSOR },
    { SCE_YAML_DOCUMENT, syntax_role::OPERATOR },
    { SCE_YAML_TEXT, syntax_role::STRING },
    { SCE_YAML_OPERATOR, syntax_role::OPERATOR },
};

static const QList<style_role> INI_ROLES = {
    { SCE_PROPS_COMMENT, syntax_role::COMMENT },
    { SCE_PROPS_SECTION, syntax_role::PREPROCESSOR },
    { SCE_PROPS_ASSIGNMENT, syntax_role::OPERATOR },
    { SCE_PROPS_DEFVAL, syntax_role::NUMBER },
    { SCE_PROPS_KEY, syntax_role::KEYWORD },
};

static const QList<style_role> DIFF_ROLES = {
    { SCE_DIFF_COMMENT, syntax_role::COMMENT },
    { SCE_DIFF_COMMAND, syntax_role::PREPROCESSOR },
    { SCE_DIFF_HEADER, syntax_role::TYPE },
    { SCE_DIFF_POSITION, syntax_role::NUMBER },
    { SCE_DIFF_DELETED, syntax_role::KEYWORD },
    { SCE_DIFF_ADDED, syntax_role::STRING },
    { SCE_DIFF_CHANGED, syntax_role::TYPE },
};

// Preprocessor tracking would dim inactive branches, which reads as commented out code
static const QList<language_property> C_PROPERTIES = {
    { "lexer.cpp.track.preprocessor", "0" },
    { "styling.within.preprocessor", "1" },
};

static const language LANGUAGES[] = {
    { "C", "cpp", "*.c *.h", "", { C_KEYWORDS, C_TYPES, "", C_CONSTANTS }, C_ROLES, C_PROPERTIES },
    { "C++", "cpp", "*.cpp *.cc *.cxx *.hpp *.hh *.hxx *.ipp *.inl", "", { C_KEYWORDS, C_TYPES, "", C_CONSTANTS },
      C_ROLES, C_PROPERTIES },
    { "JavaScript", "cpp", "*.js *.mjs *.cjs *.ts *.tsx *.jsx", "node",
      { JS_KEYWORDS, JS_TYPES, "", JS_CONSTANTS }, C_ROLES, {} },
    { "Python", "python", "*.py *.pyw", "python python3", { PYTHON_KEYWORDS, PYTHON_BUILTINS, "", "" },
      PYTHON_ROLES, {} },
    { "Shell", "bash", "*.sh *.bash *.zsh", "sh bash zsh dash ash", { SHELL_KEYWORDS, "", "", "" }, {}, {} },
    { "Makefile", "makefile", "Makefile makefile GNUmakefile *.mk *.mak", "make", { "", "", "", "" }, {}, {} },
    { "Rust", "rust", "*.rs", "", { RUST_KEYWORDS, RUST_TYPES, "", "" }, RUST_ROLES, {} },
    { "Lua", "lua", "*.lua", "lua", { LUA_KEYWORDS, "", "", "" }, {}, {} },
    { "Markdown", "markdown", "*.md *.markdown", "", { "", "", "", "" }, {}, {} },
    { "JSON", "json", "*.json", "", { "true false null", "", "", "" }, JSON_ROLES, {} },
    { "YAML", "yaml", "*.yml *.yaml", "", { "true false null yes no on off", "", "", "" }, YAML_ROLES, {} },
    { "INI", "props", "*.ini *.conf *.cfg *.properties", "", { "", "", "", "" }, INI_ROLES, {} },
    { "HTML", "hypertext", "*.html *.htm", "", { "", "", "", "" }, {}, {} },
    { "XML", "xml", "*.xml *.svg", "", { "", "", "", "" }, {}, {} },
    { "Diff", "diff", "*.diff *.patch", "", { "", "", "", "" }, DIFF_ROLES, {} },
};

static constexpr int KEYWORD_SET_COUNT = sizeof(language::keywords) / sizeof(language::keywords[0]);

static sptr_t scintilla_rgb(uint32_t argb) {
    return static_cast<sptr_t>(((argb & 0xFF) << 16) | (argb & 0xFF00) | ((argb >> 16) & 0xFF));
}

static sptr_t scintilla_rgba(uint32_t argb) {
    return static_cast<sptr_t>((argb & 0xFF000000u) | static_cast<uint32_t>(scintilla_rgb(argb)));
}

static uint32_t with_alpha(uint32_t argb, double alpha) {
    return (static_cast<uint32_t>(alpha * 255 + 0.5) << 24) | (argb & 0xFFFFFF);
}

// Scintilla draws some parts, such as indent guides and the edge, in opaque colors only
static uint32_t blend_over_pocket(uint32_t argb, double alpha) {
    auto channel = [alpha](uint32_t over, uint32_t under) {
        return static_cast<uint32_t>(under + (static_cast<double>(over) - under) * alpha + 0.5);
    };

    uint32_t red = channel((argb >> 16) & 0xFF, (STLXTHEME_OPAQUE_POCKET >> 16) & 0xFF);
    uint32_t green = channel((argb >> 8) & 0xFF, (STLXTHEME_OPAQUE_POCKET >> 8) & 0xFF);
    uint32_t blue = channel(argb & 0xFF, STLXTHEME_OPAQUE_POCKET & 0xFF);
    return 0xFF000000u | (red << 16) | (green << 8) | blue;
}

static uint32_t role_color(syntax_role role) {
    switch (role) {
        case syntax_role::KEYWORD:
            return KILN_CLAY;
        case syntax_role::TYPE:
            return KILN_ORCHID;
        case syntax_role::STRING:
            return KILN_SAGE;
        case syntax_role::NUMBER:
            return KILN_VERDIGRIS;
        case syntax_role::PREPROCESSOR:
            return KILN_BRONZE;
        case syntax_role::COMMENT:
            return KILN_ASH;
        case syntax_role::OPERATOR:
            return STLXTHEME_TEXT_DIM;
        case syntax_role::IDENTIFIER:
            break;
    }

    return STLXTHEME_TERMINAL_TEXT;
}

static syntax_role role_of_tags(const QByteArray& tags) {
    QList<QByteArray> words = tags.split(' ');
    if (words.contains("comment")) {
        return syntax_role::COMMENT;
    }

    if (words.contains("preprocessor")) {
        return syntax_role::PREPROCESSOR;
    }

    if (words.contains("literal")) {
        return words.contains("numeric") ? syntax_role::NUMBER : syntax_role::STRING;
    }

    if (words.contains("keyword")) {
        return syntax_role::KEYWORD;
    }

    return words.contains("operator") ? syntax_role::OPERATOR : syntax_role::IDENTIFIER;
}

// Scintilla writes a terminating NUL after the tags, so the buffer holds one byte more than they take
static QByteArray tags_of_style(ScintillaEditBase* view, int style) {
    sptr_t length = view->send(SCI_TAGSOFSTYLE, style, 0);
    QByteArray tags(length + 1, '\0');
    view->send(SCI_TAGSOFSTYLE, style, reinterpret_cast<sptr_t>(tags.data()));
    tags.resize(length);
    return tags;
}

static void set_property(ScintillaEditBase* view, const char* key, const char* value) {
    view->send(SCI_SETPROPERTY, reinterpret_cast<uptr_t>(key), reinterpret_cast<sptr_t>(value));
}

static double points_from_pixels(double em_px) {
    return em_px * POINTS_PER_INCH / SCREEN_DPI;
}

// Every style starts as plain text in Plex Mono on the pocket, then the lexer's styles take their colors
static void reset_styles(ScintillaEditBase* view) {
    QByteArray family = QByteArrayLiteral(STLXTHEME_MONO_FONT_FAMILY);
    int size = static_cast<int>(points_from_pixels(s_text_size) * SC_FONT_SIZE_MULTIPLIER + 0.5);

    view->send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(family.constData()));
    view->send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, size);
    view->send(SCI_STYLESETFORE, STYLE_DEFAULT, scintilla_rgb(STLXTHEME_TERMINAL_TEXT));
    view->send(SCI_STYLESETBACK, STYLE_DEFAULT, scintilla_rgb(STLXTHEME_OPAQUE_POCKET));
    view->send(SCI_STYLECLEARALL);

    view->send(SCI_STYLESETFORE, STYLE_LINENUMBER, scintilla_rgb(STLXTHEME_TEXT_DISABLED));
    view->send(SCI_STYLESETBACK, STYLE_LINENUMBER, scintilla_rgb(STLXTHEME_OPAQUE_POCKET));
    view->send(SCI_STYLESETFORE, STYLE_INDENTGUIDE, scintilla_rgb(blend_over_pocket(BRASS, INDENT_GUIDE_ALPHA)));
}

static void apply_gutter(ScintillaEditBase* view) {
    int bar_left = GUTTER_WIDTH - GUTTER_BAR_GAP - STLXTHEME_MARKER_WIDTH;

    view->send(SCI_SETMARGINS, GUTTER_MARGIN_COUNT);
    view->send(SCI_SETMARGINTYPEN, NUMBER_MARGIN, SC_MARGIN_NUMBER);
    view->send(SCI_SETMARGINWIDTHN, NUMBER_MARGIN, bar_left - GUTTER_NUMBER_GAP);
    view->send(SCI_SETMARGINTYPEN, NUMBER_GAP_MARGIN, SC_MARGIN_COLOUR);
    view->send(SCI_SETMARGINWIDTHN, NUMBER_GAP_MARGIN, GUTTER_NUMBER_GAP);

    view->send(SCI_SETMARGINTYPEN, CHANGE_BAR_MARGIN, SC_MARGIN_SYMBOL);
    view->send(SCI_SETMARGINWIDTHN, CHANGE_BAR_MARGIN, STLXTHEME_MARKER_WIDTH);
    view->send(SCI_SETMARGINMASKN, CHANGE_BAR_MARGIN, SC_MASK_HISTORY);
    view->send(SCI_SETMARGINTYPEN, BAR_GAP_MARGIN, SC_MARGIN_COLOUR);
    view->send(SCI_SETMARGINWIDTHN, BAR_GAP_MARGIN, GUTTER_BAR_GAP);

    for (int margin = 0; margin < GUTTER_MARGIN_COUNT; margin++) {
        view->send(SCI_SETMARGINBACKN, margin, scintilla_rgb(STLXTHEME_OPAQUE_POCKET));
        if (margin != CHANGE_BAR_MARGIN) {
            view->send(SCI_SETMARGINMASKN, margin, 0);
        }
    }

    view->send(SCI_SETMARGINLEFT, 0, TEXT_INSET);

    // Only lines changed since the last save carry the amber bar, and undoing back to the saved text clears it
    view->send(SCI_MARKERDEFINE, SC_MARKNUM_HISTORY_MODIFIED, SC_MARK_FULLRECT);
    view->send(SCI_MARKERSETBACK, SC_MARKNUM_HISTORY_MODIFIED, scintilla_rgb(STLXTHEME_ACCENT));
    view->send(SCI_MARKERDEFINE, SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED, SC_MARK_FULLRECT);
    view->send(SCI_MARKERSETBACK, SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED, scintilla_rgb(STLXTHEME_ACCENT));
    view->send(SCI_MARKERDEFINE, SC_MARKNUM_HISTORY_SAVED, SC_MARK_EMPTY);
    view->send(SCI_MARKERDEFINE, SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN, SC_MARK_EMPTY);
}

// Lines take the terminal's 22 px height, the extra split between ascent and descent
static void apply_line_height(ScintillaEditBase* view) {
    view->send(SCI_SETEXTRAASCENT, 0);
    view->send(SCI_SETEXTRADESCENT, 0);

    int extra = STLXTHEME_TERMINAL_LINE_HEIGHT - static_cast<int>(view->send(SCI_TEXTHEIGHT, 0));
    view->send(SCI_SETEXTRAASCENT, extra - extra / 2);
    view->send(SCI_SETEXTRADESCENT, extra / 2);
}

static bool matches_any(const QString& text, const char* patterns, bool is_wildcard) {
    for (const QString& pattern : QString::fromLatin1(patterns).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        bool is_match = is_wildcard
                      ? QRegularExpression::fromWildcard(pattern, Qt::CaseInsensitive).match(text).hasMatch()
                      : text == pattern;
        if (is_match) {
            return true;
        }
    }

    return false;
}

// `#!/usr/bin/env python3` names python3, `#!/bin/sh -e` names sh
static QString interpreter_of(QByteArrayView first_line) {
    if (!first_line.startsWith("#!")) {
        return QString();
    }

    QStringList words = QString::fromUtf8(first_line.sliced(2)).simplified().split(QLatin1Char(' '));
    QString program = QFileInfo(words.value(0)).fileName();
    if (program == QLatin1String("env") && words.size() > 1) {
        program = words.at(1);
    }

    return program;
}

void set_text_size(float em_px) {
    s_text_size = em_px;
}

void apply_text_area_look(ScintillaEditBase* view) {
    reset_styles(view);
    apply_gutter(view);
    apply_line_height(view);

    view->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, scintilla_rgba(STLXTHEME_TEXT));
    view->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_ADDITIONAL, scintilla_rgba(STLXTHEME_TEXT_SECONDARY));
    view->send(SCI_SETCARETWIDTH, CARET_WIDTH);
    view->send(SCI_SETCARETPERIOD, CARET_BLINK_MS);

    view->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, scintilla_rgba(STLXTHEME_SURFACE_SELECTED));
    view->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_ADDITIONAL_BACK, scintilla_rgba(STLXTHEME_SURFACE_SELECTED));
    view->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, scintilla_rgba(STLXTHEME_SURFACE_SELECTED));
    view->send(SCI_SETSELECTIONLAYER, SC_LAYER_UNDER_TEXT);

    view->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, scintilla_rgba(with_alpha(CREAM, CARET_LINE_ALPHA)));
    view->send(SCI_SETCARETLINELAYER, SC_LAYER_UNDER_TEXT);
    view->send(SCI_SETCARETLINEVISIBLEALWAYS, 1);

    view->send(SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH);
    view->send(SCI_SETEDGEMODE, EDGE_LINE);
    view->send(SCI_SETEDGECOLUMN, EDGE_COLUMN);
    view->send(SCI_SETEDGECOLOUR, scintilla_rgb(blend_over_pocket(BRASS, EDGE_ALPHA)));

    view->send(SCI_SETTABWIDTH, TAB_WIDTH);
    view->send(SCI_SETINDENT, TAB_WIDTH);
    view->send(SCI_SETUSETABS, 0);
    view->send(SCI_SETSCROLLWIDTHTRACKING, 1);
    view->send(SCI_SETSCROLLWIDTH, 1);
}

// Past the gutter's design width once the last line's number needs more room, as from line 100,000 or zoomed in
void fit_line_numbers(ScintillaEditBase* view) {
    if (view->send(SCI_GETMARGINWIDTHN, NUMBER_MARGIN) == 0) {
        return;
    }

    int design = GUTTER_WIDTH - GUTTER_BAR_GAP - STLXTHEME_MARKER_WIDTH - GUTTER_NUMBER_GAP;
    QByteArray widest(QByteArray::number(view->send(SCI_GETLINECOUNT)).size(), '9');
    sptr_t needed = view->send(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<sptr_t>(widest.constData()));
    view->send(SCI_SETMARGINWIDTHN, NUMBER_MARGIN, qMax<sptr_t>(design, needed + NUMBER_PADDING + NUMBER_LEFT_ROOM));
}

const language* find_language(const QString& file_name, QByteArrayView first_line) {
    for (const language& lang : LANGUAGES) {
        if (matches_any(file_name, lang.file_patterns, true)) {
            return &lang;
        }
    }

    QString interpreter = interpreter_of(first_line);
    if (interpreter.isEmpty()) {
        return nullptr;
    }

    for (const language& lang : LANGUAGES) {
        if (matches_any(interpreter, lang.interpreters, false)) {
            return &lang;
        }
    }

    return nullptr;
}

void apply_language(ScintillaEditBase* view, const language* lang) {
    reset_styles(view);
    if (!lang) {
        view->send(SCI_SETILEXER, 0, 0);
        view->send(SCI_COLOURISE, 0, -1);
        return;
    }

    view->send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(CreateLexer(lang->lexer)));
    for (int set = 0; set < KEYWORD_SET_COUNT; set++) {
        view->send(SCI_SETKEYWORDS, set, reinterpret_cast<sptr_t>(lang->keywords[set]));
    }

    for (const language_property& property : lang->properties) {
        set_property(view, property.key, property.value);
    }

    int named_styles = static_cast<int>(view->send(SCI_GETNAMEDSTYLES));
    for (int style = 0; style < named_styles; style++) {
        if (style < STYLE_DEFAULT || style > STYLE_LASTPREDEFINED) {
            view->send(SCI_STYLESETFORE, style, scintilla_rgb(role_color(role_of_tags(tags_of_style(view, style)))));
        }
    }

    for (const style_role& entry : lang->roles) {
        view->send(SCI_STYLESETFORE, entry.style, scintilla_rgb(role_color(entry.role)));
    }

    view->send(SCI_COLOURISE, 0, -1);
}

QString language_name(const language* lang) {
    return lang ? QString::fromLatin1(lang->name) : QStringLiteral("Plain Text");
}

void set_line_numbers_shown(ScintillaEditBase* view, bool is_shown) {
    int bar_left = GUTTER_WIDTH - GUTTER_BAR_GAP - STLXTHEME_MARKER_WIDTH;
    view->send(SCI_SETMARGINWIDTHN, NUMBER_MARGIN, is_shown ? bar_left - GUTTER_NUMBER_GAP : 0);
    view->send(SCI_SETMARGINWIDTHN, NUMBER_GAP_MARGIN, is_shown ? GUTTER_NUMBER_GAP : 0);
    fit_line_numbers(view);
}

bool are_line_numbers_shown(ScintillaEditBase* view) {
    return view->send(SCI_GETMARGINWIDTHN, NUMBER_MARGIN) > 0;
}

QList<const language*> all_languages() {
    QList<const language*> languages;
    for (const language& lang : LANGUAGES) {
        languages.append(&lang);
    }

    return languages;
}
