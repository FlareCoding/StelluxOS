#include "plugin.h"
#include "integration.h"

#include <QtPlugin>

// Registers the plugin at startup, kept in every Qt program by the whole-archive link
Q_IMPORT_PLUGIN(QStelluxIntegrationPlugin)

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
