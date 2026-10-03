#ifndef STLXQPA_PLUGIN_H
#define STLXQPA_PLUGIN_H

#include <qpa/qplatformintegrationplugin.h>

// The stellux platform, the default platform of every program linked against the Qt kit
class QStelluxIntegrationPlugin : public QPlatformIntegrationPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "stellux.json")

public:
    QPlatformIntegration* create(const QString& key, const QStringList& parameters) override;
};

#endif
