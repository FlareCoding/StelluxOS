#ifndef STLXQPA_STYLE_H
#define STLXQPA_STYLE_H

#include <QProxyStyle>
#include <QStylePlugin>

// Qt's widgets drawn the way stlxui draws its controls, with Fusion for every widget stlxui lacks
class QStelluxStyle : public QProxyStyle {
public:
    static constexpr const char* NAME = "stellux";

    QStelluxStyle();

    using QProxyStyle::polish;
    using QProxyStyle::unpolish;
    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;

    int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contents,
                           const QWidget* widget) const override;
    QRect subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option, SubControl sub,
                         const QWidget* widget) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                            const QWidget* widget) const override;
};

// The stellux style, which the stellux platform theme asks every Qt program for
class QStelluxStylePlugin : public QStylePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QStyleFactoryInterface_iid FILE "style.json")

public:
    QStyle* create(const QString& key) override;
};

#endif
