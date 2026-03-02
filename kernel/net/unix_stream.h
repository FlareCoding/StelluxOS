#ifndef STELLUX_NET_UNIX_STREAM_H
#define STELLUX_NET_UNIX_STREAM_H

#include "common/types.h"

namespace net::unix_stream {

class stream_socket;

constexpr uint32_t SOCKET_PATH_MAX = 108;

constexpr int32_t OK               = 0;
constexpr int32_t ERR_INVAL        = -1;
constexpr int32_t ERR_NOMEM        = -2;
constexpr int32_t ERR_ADDRINUSE    = -3;
constexpr int32_t ERR_AGAIN        = -4;
constexpr int32_t ERR_NOTCONN      = -5;
constexpr int32_t ERR_CONNREFUSED  = -6;
constexpr int32_t ERR_PIPE         = -7;
constexpr int32_t ERR_OPNOTSUPP    = -8;
constexpr int32_t ERR_AFNOSUPPORT  = -9;
constexpr int32_t ERR_PROTONOSUPPORT = -10;
constexpr int32_t ERR_BADF         = -11;

struct socket_path {
    uint16_t len;
    char bytes[SOCKET_PATH_MAX];
};

/**
 * @brief Initialize the AF_UNIX stream socket subsystem.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Build a validated socket path from raw bytes.
 * Rejects empty paths and abstract namespace paths.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t make_path(
    const char* path_bytes,
    size_t path_len,
    socket_path* out
);

/**
 * @brief Build a validated socket path from a NUL-terminated C string.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t make_path_cstr(
    const char* cstr,
    socket_path* out
);

/**
 * @brief Create a new AF_UNIX stream socket object.
 * On success returns one owned socket reference in out_socket.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket(bool nonblocking, stream_socket** out_socket);

/**
 * @brief Bind a created socket to a unique path.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t bind(stream_socket* socket, const socket_path& path);

/**
 * @brief Mark a bound socket as listening with a bounded backlog.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t listen(stream_socket* socket, uint32_t backlog);

/**
 * @brief Connect a created socket to a listening path.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t connect(stream_socket* socket, const socket_path& path);

/**
 * @brief Accept one pending incoming connection from a listener.
 * On success returns one owned socket reference in out_socket.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t accept(stream_socket* listener, stream_socket** out_socket);

/**
 * @brief Send bytes through a connected stream socket.
 * @return Positive byte count on success, negative error otherwise.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t send(stream_socket* socket, const void* ksrc, size_t count);

/**
 * @brief Receive bytes from a connected stream socket.
 * @return Positive byte count, 0 on EOF, or negative error.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t recv(stream_socket* socket, void* kdst, size_t count);

/**
 * @brief Mark a socket closed and wake all waiters.
 * Safe to call multiple times.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t close(stream_socket* socket);

/**
 * @brief Set or clear O_NONBLOCK behavior.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_nonblocking(stream_socket* socket, bool nonblocking);

/**
 * @brief Query current O_NONBLOCK behavior.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t get_nonblocking(stream_socket* socket, bool* out_nonblocking);

/**
 * @brief Add one strong reference to a socket object.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void add_ref(stream_socket* socket);

/**
 * @brief Release one strong reference to a socket object.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void release(stream_socket* socket);

} // namespace net::unix_stream

#endif // STELLUX_NET_UNIX_STREAM_H
