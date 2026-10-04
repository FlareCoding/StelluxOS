#ifndef STLXQPA_SERVICES_H
#define STLXQPA_SERVICES_H

#include <qpa/qplatformservices.h>

/**
 * Opens URLs in the program the desktop configuration names for their
 * scheme, a url_handler section of stlxdm.conf or of a drop-in file.
 */
class QStelluxServices : public QPlatformServices {
public:
    bool openUrl(const QUrl& url) override;
};

#endif
