#ifndef DROPBEAR_DEFAULT_OPTIONS_H_
#define DROPBEAR_DEFAULT_OPTIONS_H_
/*
                     > > > Read This < < <

default_options.h documents compile-time options, and provides default values.

Local customisation should be added to localoptions.h which is
used if it exists in the build directory. Options defined there will override
any options in this file.

Customisations will also be taken from src/distoptions.h if it exists.

Options can also be defined with -DDROPBEAR_XXX=[0,1] in Makefile CFLAGS

IMPORTANT: Some options will require "make clean" after changes */

#ifndef DROPBEAR_DEFPORT
#define DROPBEAR_DEFPORT "22"
#endif

/* Listen on all interfaces */
#ifndef DROPBEAR_DEFADDRESS
#define DROPBEAR_DEFADDRESS ""
#endif

/* Default hostkey paths - these can be specified on the command line.
 * Homedir is prepended if path begins with ~/
 */
#ifndef DSS_PRIV_FILENAME
#define DSS_PRIV_FILENAME "/etc/dropbear/dropbear_dss_host_key"
#endif
#ifndef RSA_PRIV_FILENAME
#define RSA_PRIV_FILENAME "/etc/dropbear/dropbear_rsa_host_key"
#endif
#ifndef ECDSA_PRIV_FILENAME
#define ECDSA_PRIV_FILENAME "/etc/dropbear/dropbear_ecdsa_host_key"
#endif
#ifndef ED25519_PRIV_FILENAME
#define ED25519_PRIV_FILENAME "/etc/dropbear/dropbear_ed25519_host_key"
#endif

/* Set NON_INETD_MODE if you require daemon functionality (ie Dropbear listens
 * on chosen ports and keeps accepting connections. This is the default.
 *
 * Set INETD_MODE if you want to be able to run Dropbear with inetd (or
 * similar), where it will use stdin/stdout for connections, and each process
 * lasts for a single connection. Dropbear should be invoked with the -i flag
 * for inetd, and can only accept IPv4 connections.
 *
 * Both of these flags can be defined at once, don't compile without at least
 * one of them. */
#ifndef NON_INETD_MODE
#define NON_INETD_MODE 1
#endif
#ifndef INETD_MODE
#define INETD_MODE 1
#endif

/* By default Dropbear will re-execute itself for each incoming connection so
   that memory layout may be re-randomised (ASLR) - exploiting
   vulnerabilities becomes harder. Re-exec causes slightly more memory use
   per connection.
   This option is ignored on non-Linux platforms at present */
#ifndef DROPBEAR_REEXEC
#define DROPBEAR_REEXEC 1
#endif

/* Include verbose debug output, enabled with -v at runtime (repeat to increase).
 * define which level of debug output you compile in
 * Level 0 = disabled
 * Level 1-3 = approx 4 Kb (connection, remote identity, algos, auth type info)
 * Level 4 = approx 17 Kb (detailed before connection)
 * Level 5 = approx 8 Kb (detailed after connection) */
#ifndef DEBUG_TRACE
#define DEBUG_TRACE 0
#endif

/* Set this if you want to use the DROPBEAR_SMALL_CODE option. This can save
 * several kB in binary size however will make the symmetrical ciphers and hashes
 * slower, perhaps by 50%. Recommended for small systems that aren't doing
 * much traffic. */
#ifndef DROPBEAR_SMALL_CODE
#define DROPBEAR_SMALL_CODE 1
#endif

/* Enable X11 Forwarding - server only */
#ifndef DROPBEAR_X11FWD
#define DROPBEAR_X11FWD 0
#endif

/* Enable TCP Fowarding */
/* 'Local' is "-L" style (client listening port forwarded via server)
 * 'Remote' is "-R" style (server listening port forwarded via client) */
#ifndef DROPBEAR_CLI_LOCALTCPFWD
#define DROPBEAR_CLI_LOCALTCPFWD 1
#endif
#ifndef DROPBEAR_CLI_REMOTETCPFWD
#define DROPBEAR_CLI_REMOTETCPFWD 1
#endif

#ifndef DROPBEAR_SVR_LOCALTCPFWD
#define DROPBEAR_SVR_LOCALTCPFWD 1
#endif
#ifndef DROPBEAR_SVR_REMOTETCPFWD
#define DROPBEAR_SVR_REMOTETCPFWD 1
#endif
#ifndef DROPBEAR_SVR_LOCALSTREAMFWD
#define DROPBEAR_SVR_LOCALSTREAMFWD 1
#endif

/* Enable Authentication Agent Forwarding */
#ifndef DROPBEAR_SVR_AGENTFWD
#define DROPBEAR_SVR_AGENTFWD 1
#endif
#ifndef DROPBEAR_CLI_AGENTFWD
#define DROPBEAR_CLI_AGENTFWD 1
#endif

/* Note: Both DROPBEAR_CLI_PROXYCMD and DROPBEAR_CLI_NETCAT must be set to
 * allow multihop dbclient connections */

/* Allow using -J <proxycommand> to run the connection through a
   pipe to a program, rather the normal TCP connection */
#ifndef DROPBEAR_CLI_PROXYCMD
#define DROPBEAR_CLI_PROXYCMD 1
#endif

/* Enable "Netcat mode" option. This will forward standard input/output
 * to a remote TCP-forwarded connection */
#ifndef DROPBEAR_CLI_NETCAT
#define DROPBEAR_CLI_NETCAT 1
#endif

/* Whether to support "-c" and "-m" flags to choose ciphers/MACs at runtime */
#ifndef DROPBEAR_USER_ALGO_LIST
#define DROPBEAR_USER_ALGO_LIST 1
#endif

/* Encryption - at least one required.
 * AES128 should be enabled, some very old implementations might only
 * support 3DES.
 * Including both AES keysize variants (128 and 256) will result in
 * a minimal size increase */
#ifndef DROPBEAR_AES128
#define DROPBEAR_AES128 1
#endif
#ifndef DROPBEAR_AES256
#define DROPBEAR_AES256 1
#endif
#ifndef DROPBEAR_3DES
#define DROPBEAR_3DES 0
#endif

/* Enable Chacha20-Poly1305 authenticated encryption mode. This is
 * generally faster than AES256 on CPU w/o dedicated AES instructions,
 * having the same key size. Recommended.
 * Compiling in will add ~5,5kB to binary size on x86-64 */
#ifndef DROPBEAR_CHACHA20POLY1305
#define DROPBEAR_CHACHA20POLY1305 1
#endif

/* Enable "Counter Mode" for ciphers. Recommended. */
#ifndef DROPBEAR_ENABLE_CTR_MODE
#define DROPBEAR_ENABLE_CTR_MODE 1
#endif

/* Enable CBC mode for ciphers. This has security issues though
   may be required for compatibility with old implementations */
#ifndef DROPBEAR_ENABLE_CBC_MODE
#define DROPBEAR_ENABLE_CBC_MODE 0
#endif

/* Enable "Galois/Counter Mode" for ciphers. This authenticated
 * encryption mode is combination of CTR mode and GHASH. Recommended
 * for security and forwards compatibility, but slower than CTR on
 * CPU w/o dedicated AES/GHASH instructions.
 * Compiling in will add ~6kB to binary size on x86-64 */
#ifndef DROPBEAR_ENABLE_GCM_MODE
#define DROPBEAR_ENABLE_GCM_MODE 0
#endif

/* Message integrity. sha2-256 is recommended as a default,
   sha1 for compatibility */
#ifndef DROPBEAR_SHA1_HMAC
#define DROPBEAR_SHA1_HMAC 1
#endif
#ifndef DROPBEAR_SHA2_256_HMAC
#define DROPBEAR_SHA2_256_HMAC 1
#endif
#ifndef DROPBEAR_SHA2_512_HMAC
#define DROPBEAR_SHA2_512_HMAC 0
#endif
#ifndef DROPBEAR_SHA1_96_HMAC
#define DROPBEAR_SHA1_96_HMAC 0
#endif

/* Hostkey/public key algorithms - at least one required, these are used
 * for hostkey as well as for verifying signatures with pubkey auth.
 * RSA is recommended.
 *
 * See: RSA_PRIV_FILENAME and DSS_PRIV_FILENAME */
#ifndef DROPBEAR_RSA
#define DROPBEAR_RSA 1
#endif
/* Newer SSH implementations use SHA256 for RSA signatures. SHA1
 * support is required to communicate with some older implementations.
 * It will be removed in future due to SHA1 insecurity, it can be
 * disabled with DROPBEAR_RSA_SHA1 set to 0 */
#ifndef DROPBEAR_RSA_SHA1
#define DROPBEAR_RSA_SHA1 1
#endif

/* DSS may be necessary to connect to some systems but is not
 * recommended for new keys (1024 bits is small, and it uses SHA1).
 * RSA key generation will be faster with bundled libtommath
 * if DROPBEAR_DSS is disabled.
 * https://github.com/mkj/dropbear/issues/174#issuecomment-1267374858 */
#ifndef DROPBEAR_DSS
#define DROPBEAR_DSS 0
#endif
/* ECDSA is significantly faster than RSA or DSS. Compiling in ECC
 * code (either ECDSA or ECDH) increases binary size - around 30kB
 * on x86-64.
 * See: ECDSA_PRIV_FILENAME  */
#ifndef DROPBEAR_ECDSA
#define DROPBEAR_ECDSA 1
#endif

/* Ed25519 is faster than ECDSA. Compiling in Ed25519 code increases
 * binary size - around 7,5kB on x86-64.
 * See: ED25519_PRIV_FILENAME  */
#ifndef DROPBEAR_ED25519
#define DROPBEAR_ED25519 1
#endif

/* Allow U2F security keys for public key auth, with
 * sk-ecdsa-sha2-nistp256@openssh.com or sk-ssh-ed25519@openssh.com keys.
 * The corresponding DROPBEAR_ECDSA or DROPBEAR_ED25519 also needs to be set.
 * This is currently server-only. */
#ifndef DROPBEAR_SK_KEYS
#define DROPBEAR_SK_KEYS 1
#endif

/* RSA must be >=1024 */
#ifndef DROPBEAR_DEFAULT_RSA_SIZE
#define DROPBEAR_DEFAULT_RSA_SIZE 2048
#endif
/* DSS is always 1024 */
/* ECDSA defaults to largest size configured, usually 521 */
/* Ed25519 is always 256 */

/* Add runtime flag "-R" to generate hostkeys as-needed when the first
   connection using that key type occurs.
   This avoids the need to otherwise run "dropbearkey" and avoids some problems
   with badly seeded /dev/urandom when systems first boot. */
#ifndef DROPBEAR_DELAY_HOSTKEY
#define DROPBEAR_DELAY_HOSTKEY 1
#endif


/* Key exchange algorithm.

 * group14_sha1 - 2048 bit, sha1
 * group14_sha256 - 2048 bit, sha2-256
 * group16 - 4096 bit, sha2-512
 * group1 - 1024 bit, sha1
 * curve25519 - elliptic curve DH
 * ecdh - NIST elliptic curve DH (256, 384, 521)
 *
 * group1 is too small for security though is necessary if you need
     compatibility with some implementations such as Dropbear versions < 0.53
 * group14 is supported by most implementations.
 * group16 provides a greater strength level but is slower and increases binary size
 * curve25519 and ecdh algorithms are faster than non-elliptic curve methods
 * curve25519 increases binary size by ~2,5kB on x86-64
 * including either ECDH or ECDSA increases binary size by ~30kB on x86-64

 * Small systems should generally include either curve25519 or ecdh for performance.
 * curve25519 is less widely supported but is faster
 */
#ifndef DROPBEAR_DH_GROUP14_SHA1
#define DROPBEAR_DH_GROUP14_SHA1 1
#endif
#ifndef DROPBEAR_DH_GROUP14_SHA256
#define DROPBEAR_DH_GROUP14_SHA256 1
#endif
#ifndef DROPBEAR_DH_GROUP16
#define DROPBEAR_DH_GROUP16 0
#endif
#ifndef DROPBEAR_CURVE25519
#define DROPBEAR_CURVE25519 1
#endif
#ifndef DROPBEAR_ECDH
#define DROPBEAR_ECDH 1
#endif
#ifndef DROPBEAR_DH_GROUP1
#define DROPBEAR_DH_GROUP1 0
#endif

/* When group1 is enabled it will only be allowed by Dropbear client
not as a server, due to concerns over its strength. Set to 0 to allow
group1 in Dropbear server too */
#ifndef DROPBEAR_DH_GROUP1_CLIENTONLY
#define DROPBEAR_DH_GROUP1_CLIENTONLY 1
#endif

/* Control the memory/performance/compression tradeoff for zlib.
 * Set windowBits=8 for least memory usage, see your system's
 * zlib.h for full details.
 * Default settings (windowBits=15) will use 256kB for compression
 * windowBits=8 will use 129kB for compression.
 * Both modes will use ~35kB for decompression (using windowBits=15 for
 * interoperability) */
#ifndef DROPBEAR_ZLIB_WINDOW_BITS
#define DROPBEAR_ZLIB_WINDOW_BITS 15
#endif

/* Whether to do reverse DNS lookups. */
#ifndef DO_HOST_LOOKUP
#define DO_HOST_LOOKUP 0
#endif

/* Whether to print the message of the day (MOTD). */
#ifndef DO_MOTD
#define DO_MOTD 1
#endif
#ifndef MOTD_FILENAME
#define MOTD_FILENAME "/etc/motd"
#endif
#ifndef MOTD_MAXSIZE
#define MOTD_MAXSIZE 2000
#endif

/* Authentication Types - at least one required.
   RFC Draft requires pubkey auth, and recommends password */
#ifndef DROPBEAR_SVR_PASSWORD_AUTH
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#endif

/* Note: PAM auth is quite simple and only works for PAM modules which just do
 * a simple "Login: " "Password: " (you can edit the strings in svr-authpam.c).
 * It's useful for systems like OS X where standard password crypts don't work
 * but there's an interface via a PAM module. It won't work for more complex
 * PAM challenge/response.
 * You can't enable both PASSWORD and PAM. */
#ifndef DROPBEAR_SVR_PAM_AUTH
#define DROPBEAR_SVR_PAM_AUTH 0
#endif

/* ~/.ssh/authorized_keys authentication.
 * You must define DROPBEAR_SVR_PUBKEY_AUTH in order to use plugins. */
#ifndef DROPBEAR_SVR_PUBKEY_AUTH
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#endif

/* Whether to take public key options in
 * authorized_keys file into account */
#ifndef DROPBEAR_SVR_PUBKEY_OPTIONS
#define DROPBEAR_SVR_PUBKEY_OPTIONS 1
#endif

/* Set this to 0 if your system does not have multiple user support.
   (Linux kernel CONFIG_MULTIUSER option)
   The resulting binary will not run on a normal system. */
#ifndef DROPBEAR_SVR_MULTIUSER
#define DROPBEAR_SVR_MULTIUSER 1
#endif

/* Client authentication options */
#ifndef DROPBEAR_CLI_PASSWORD_AUTH
#define DROPBEAR_CLI_PASSWORD_AUTH 1
#endif
#ifndef DROPBEAR_CLI_PUBKEY_AUTH
#define DROPBEAR_CLI_PUBKEY_AUTH 1
#endif

/* A default argument for dbclient -i <privatekey>.
 * Homedir is prepended if path begins with ~/
 */
#ifndef DROPBEAR_DEFAULT_CLI_AUTHKEY
#define DROPBEAR_DEFAULT_CLI_AUTHKEY "~/.ssh/id_dropbear"
#endif

/* Per client configuration file
*/
#ifndef DROPBEAR_USE_SSH_CONFIG
#define DROPBEAR_USE_SSH_CONFIG 0
#endif

/* Allow specifying the password for dbclient via the DROPBEAR_PASSWORD
 * environment variable. */
#ifndef DROPBEAR_USE_PASSWORD_ENV
#define DROPBEAR_USE_PASSWORD_ENV 1
#endif

/* Define this (as well as DROPBEAR_CLI_PASSWORD_AUTH) to allow the use of
 * a helper program for the ssh client. The helper program should be
 * specified in the SSH_ASKPASS environment variable, and dbclient
 * should be run with DISPLAY set and no tty. The program should
 * return the password on standard output */
#ifndef DROPBEAR_CLI_ASKPASS_HELPER
#define DROPBEAR_CLI_ASKPASS_HELPER 0
#endif

/* Save a network roundtrip by sendng a real auth request immediately after
 * sending a query for the available methods. This is not yet enabled by default
 since it could cause problems with non-compliant servers */
#ifndef DROPBEAR_CLI_IMMEDIATE_AUTH
#define DROPBEAR_CLI_IMMEDIATE_AUTH 0
#endif

/* Set this to use PRNGD or EGD instead of /dev/urandom */
#ifndef DROPBEAR_USE_PRNGD
#define DROPBEAR_USE_PRNGD 0
#endif
#ifndef DROPBEAR_PRNGD_SOCKET
#define DROPBEAR_PRNGD_SOCKET "/var/run/dropbear-rng"
#endif

/* Specify the number of clients we will allow to be connected but
 * not yet authenticated. After this limit, connections are rejected */
/* The first setting is per-IP, to avoid denial of service */
#ifndef MAX_UNAUTH_PER_IP
#define MAX_UNAUTH_PER_IP 5
#endif

/* And then a global limit to avoid chewing memory if connections
 * come from many IPs */
#ifndef MAX_UNAUTH_CLIENTS
#define MAX_UNAUTH_CLIENTS 30
#endif

/* Default maximum number of failed authentication tries (server option) */
/* -T server option overrides */
#ifndef MAX_AUTH_TRIES
#define MAX_AUTH_TRIES 10
#endif

/* Delay introduced before closing an unauthenticated session (seconds).
   Disabled by default, can be set to say 30 seconds to reduce the speed
   of password brute forcing. Note that there is a risk of denial of
   service by setting this */
#ifndef UNAUTH_CLOSE_DELAY
#define UNAUTH_CLOSE_DELAY 0
#endif

/* The default file to store the daemon's process ID, for shutdown
 * scripts etc. This can be overridden with the -P flag.
 * Homedir is prepended if path begins with ~/
 */
#ifndef DROPBEAR_PIDFILE
#define DROPBEAR_PIDFILE "/var/run/dropbear.pid"
#endif

/* The command to invoke for xauth when using X11 forwarding.
 * "-q" for quiet */
#ifndef XAUTH_COMMAND
#define XAUTH_COMMAND "/usr/bin/xauth -q"
#endif


/* If you want to enable running an sftp server (such as the one included with
 * OpenSSH), set the path below and set DROPBEAR_SFTPSERVER.
 * The sftp-server program is not provided by Dropbear itself.
 * Homedir is prepended if path begins with ~/
 */
#ifndef DROPBEAR_SFTPSERVER
#define DROPBEAR_SFTPSERVER 1
#endif
#ifndef SFTPSERVER_PATH
#define SFTPSERVER_PATH "/usr/libexec/sftp-server"
#endif

/* This is used by the scp binary when used as a client binary. If you're
 * not using the Dropbear client, you'll need to change it */
#ifndef DROPBEAR_PATH_SSH_PROGRAM
#define DROPBEAR_PATH_SSH_PROGRAM "/usr/bin/dbclient"
#endif

/* Whether to log commands executed by a client. This only logs the
 * (single) command sent to the server, not what a user did in a
 * shell/sftp session etc. */
#ifndef LOG_COMMANDS
#define LOG_COMMANDS 0
#endif

/* Window size limits. These tend to be a trade-off between memory
   usage and network performance: */
/* Size of the network receive window. This amount of memory is allocated
   as a per-channel receive buffer. Increasing this value can make a
   significant difference to network performance. 24kB was empirically
   chosen for a 100mbit ethernet network. The value can be altered at
   runtime with the -W argument. */
#ifndef DEFAULT_RECV_WINDOW
#define DEFAULT_RECV_WINDOW 24576
#endif
/* Maximum size of a received SSH data packet - this _MUST_ be >= 32768
   in order to interoperate with other implementations */
#ifndef RECV_MAX_PAYLOAD_LEN
#define RECV_MAX_PAYLOAD_LEN 32768
#endif
/* Maximum size of a transmitted data packet - this can be any value,
   though increasing it may not make a significant difference. */
#ifndef TRANS_MAX_PAYLOAD_LEN
#define TRANS_MAX_PAYLOAD_LEN 16384
#endif

/* Ensure that data is transmitted every KEEPALIVE seconds. This can
be overridden at runtime with -K. 0 disables keepalives */
#ifndef DEFAULT_KEEPALIVE
#define DEFAULT_KEEPALIVE 0
#endif

/* If this many KEEPALIVES are sent with no packets received from the
other side, exit. Not run-time configurable - if you have a need
for runtime configuration please mail the Dropbear list */
#ifndef DEFAULT_KEEPALIVE_LIMIT
#define DEFAULT_KEEPALIVE_LIMIT 3
#endif

/* Ensure that data is received within IDLE_TIMEOUT seconds. This can
be overridden at runtime with -I. 0 disables idle timeouts */
#ifndef DEFAULT_IDLE_TIMEOUT
#define DEFAULT_IDLE_TIMEOUT 0
#endif

/* The default path. This will often get replaced by the shell */
#ifndef DEFAULT_PATH
#define DEFAULT_PATH "/usr/bin:/bin"
#endif
#ifndef DEFAULT_ROOT_PATH
#define DEFAULT_ROOT_PATH "/usr/sbin:/usr/bin:/sbin:/bin"
#endif

#endif /* DROPBEAR_DEFAULT_OPTIONS_H_ */
