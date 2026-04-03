/*
 * Stellux-specific Dropbear options.
 * Overrides default_options.h for minimal Stellux build.
 */

#ifndef DROPBEAR_LOCALOPTIONS_H_
#define DROPBEAR_LOCALOPTIONS_H_

/* Server only, no client */
#define DROPBEAR_SERVER 1
#define DROPBEAR_CLIENT 0

/* Both modes: listener uses non-inetd, spawned children use inetd */
#define NON_INETD_MODE 1
#define INETD_MODE 1

/* Disable features we don't need */
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_CLI_LOCALTCPFWD 0
#define DROPBEAR_CLI_REMOTETCPFWD 0
#define DROPBEAR_CLI_PROXYCMD 0
#define DROPBEAR_CLI_NETCAT 0

/* Simple password auth only */
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 0
#define DROPBEAR_SVR_PUBKEY_OPTIONS 0
#define DROPBEAR_SVR_MULTIUSER 0

/* Disable re-exec (no fexecve) */
#define DROPBEAR_REEXEC 0

/* Multi-process mode: spawn child process per connection via proc_create */
#define DEBUG_NOFORK 0

/* Enable verbose trace output (run with -v) */
#define DEBUG_TRACE 1

/* Disable utmp/wtmp/lastlog/syslog */
#define DROPBEAR_SVR_LOG_COMMANDS 0

/* Minimal ciphers -- keep it simple */
#define DROPBEAR_AES128_CTR 1
#define DROPBEAR_AES256_CTR 1
#define DROPBEAR_CHACHA20POLY1305 1

/* Key exchange */
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_ECDH 0
#define DROPBEAR_DH_GROUP14_SHA256 1
#define DROPBEAR_DH_GROUP16 0
#define DROPBEAR_DH_GROUP1 0

/* Host key types */
#define DROPBEAR_ED25519 1
#define DROPBEAR_RSA 1
#define DROPBEAR_DSS 0
#define DROPBEAR_ECDSA 0

/* Use /dev/urandom as fallback for random */
#define DROPBEAR_URANDOM_DEV "/dev/urandom"

/* Disable zlib compression (not available on Stellux) */
#define DISABLE_ZLIB 1

/* Disable IPv6 (not available on Stellux) */
#define DROPBEAR_IPV6 0

/* Delay host key generation until first connection */
#define DROPBEAR_DELAY_HOSTKEY 1

/* Host key paths */
#undef ED25519_PRIV_FILENAME
#define ED25519_PRIV_FILENAME "/etc/dropbear/dropbear_ed25519_host_key"
#undef RSA_PRIV_FILENAME
#define RSA_PRIV_FILENAME "/etc/dropbear/dropbear_rsa_host_key"

#endif /* DROPBEAR_LOCALOPTIONS_H_ */
