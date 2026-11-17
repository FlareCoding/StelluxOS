#ifndef SHEEN_APP_SHARED_SERVICE_H
#define SHEEN_APP_SHARED_SERVICE_H

namespace sheen {

/* Base class for process-wide reference-counted background helpers shared across workspace windows. */
class shared_service {
public:
    virtual ~shared_service() = default;
};

} // namespace sheen

#endif
