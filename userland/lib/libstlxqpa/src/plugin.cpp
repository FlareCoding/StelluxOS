#include "plugin.h"
#include "integration.h"

#include <QtPlugin>

// Registers the plugins at startup, kept in every Qt program by the whole-archive link
Q_IMPORT_PLUGIN(QStelluxIntegrationPlugin)
Q_IMPORT_PLUGIN(QStelluxStylePlugin)

QPlatformIntegration* QStelluxIntegrationPlugin::create(const QString& key, const QStringList& parameters) {
    Q_UNUSED(parameters);

    if (key.compare(QLatin1String("stellux"), Qt::CaseInsensitive) != 0) {
        return nullptr;
    }

    auto* integration = new QStelluxIntegration;
    if (!integration->is_connected()) {
        delete integration;
        return nullptr;
    }

    return integration;
}
