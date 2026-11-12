#ifndef STLXQPA_DROP_H
#define STLXQPA_DROP_H

#include <QString>

class QWindow;

namespace stlxqpa {

// Words for the chip of the drag over `window`, such as "Open in Sheen", set from the drag events of the widget that
// accepts it. Each motion starts without words, so a widget that sets none shows the action's name.
void set_drop_words(QWindow* window, const QString& words);

} // namespace stlxqpa

#endif
