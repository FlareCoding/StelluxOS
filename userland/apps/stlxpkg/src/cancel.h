#ifndef STLXPKG_CANCEL_H
#define STLXPKG_CANCEL_H

/**
 * @brief Makes SIGTERM, SIGINT and SIGPIPE ask the run to stop instead of ending it, so it can stop where its work
 * stays whole.
 */
void cancel_watch(void);

/**
 * @brief Fails as cancelled once one of those signals arrived.
 * @return 0 while none did, -1 once one did.
 */
int cancel_check(void);

#endif /* STLXPKG_CANCEL_H */
