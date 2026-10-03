#ifndef STLXQPA_FONT_DATABASE_H
#define STLXQPA_FONT_DATABASE_H

#include <QtGui/private/qfreetypefontdatabase_p.h>

// Stellux keeps every font in one directory, which the database reads whole
class QStelluxFontDatabase : public QFreeTypeFontDatabase {
public:
    void populateFontDatabase() override;
};

#endif
