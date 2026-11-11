#ifndef STLXQPA_SERVICES_H
#define STLXQPA_SERVICES_H

#include <qpa/qplatformservices.h>

/**
 * Opens URLs in the program the desktop configuration names for their
 * scheme, a url_handler section of stlxdm.conf or of a drop-in file, and
 * local files in the one it names for their MIME type or a type it derives
 * from, a file_handler section.
 */
class QStelluxServices : public QPlatformServices {
public:
    bool openUrl(const QUrl& url) override;
    bool openDocument(const QUrl& url) override;
};

#endif
