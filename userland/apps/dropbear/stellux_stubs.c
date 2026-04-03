/*
 * Stellux stubs for functions referenced by Dropbear but not
 * available or not needed on Stellux.
 */

/* ttyslot() - legacy BSD function for utmp, not in musl */
int ttyslot(void) {
    return 0;
}

/* dropbear_ed25519_verify - normally behind DROPBEAR_SIGNKEY_VERIFY
 * which is disabled since we have no pubkey auth or client mode,
 * but sk-ed25519.c references it unconditionally. */
int dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen,
                            const unsigned char *s, unsigned long slen,
                            const unsigned char *pk) {
    (void)m; (void)mlen; (void)s; (void)slen; (void)pk;
    return -1;
}
