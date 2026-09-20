/* SPDX-License-Identifier: Apache-2.0 */

#include "smart_home_miloco_client.h"

#include <arpa/inet.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MILOCO_HTTP_CONNECT_TIMEOUT_MS 3000
#define MILOCO_HTTP_IO_TIMEOUT_MS      5000

/* 解析 RFC 7231 日期 "Thu, 20 Sep 2026 19:30:00 GMT" 为 epoch 秒。
 * 失败时 *out 保持调用方预置值。纯计算无时区依赖（GMT 即 UTC）。 */
static void parse_http_date(const char *text, time_t *out)
{
    static const char months[12][4] =
    {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char mon[8] = "";
    int mon_index = -1;
    int i;

    if (!text || !out) {
        return;
    }

    if (sscanf(text, "%*[^,], %d %7s %d %d:%d:%d",
               &day, mon, &year, &hour, &minute, &second) < 6) {
        return;
    }

    for (i = 0; i < 12; i++) {
        if (strcmp(mon, months[i]) == 0) {
            mon_index = i;
            break;
        }
    }
    if (mon_index < 0 || day < 1 || day > 31 || year < 2020 ||
        year > 2100 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59) {
        return;
    }

    /* days_from_civil（Howard Hinnant 算法）：1970-01-01 = 0。 */
    {
        long y = year;
        long m = mon_index + 1;
        long era;
        long yoe;
        long doy;
        long doe;
        long days;

        y -= (m <= 2) ? 1 : 0;
        era = (y >= 0 ? y : y - 399) / 400;
        yoe = y - era * 400;
        doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        days = era * 146097 + doe - 719468;

        *out = (time_t)(days * 86400L + hour * 3600L + minute * 60L +
                        second);
    }
}

static int http_request(const smart_home_miloco_client_config_t *config,
                        const char *method,
                        const char *path,
                        const char *body,
                        char *response,
                        size_t response_size,
                        int *http_status_out,
                        time_t *date_out)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct timeval timeout;
    struct pollfd pfd;
    socklen_t optlen;
    char request[512];
    char port_text[8];
    int sockfd = -1;
    int flags;
    int error_code = 0;
    int http_status = 0;
    size_t sent = 0;
    size_t request_len;
    bool header_done = false;
    bool have_status = false;
    int ret;

    if (!config || !path || !response || response_size == 0) {
        return -EINVAL;
    }
    response[0] = '\0';
    if (config->host[0] == '\0' || config->port == 0) {
        return -EINVAL;
    }

    syslog(LOG_INFO, "[milo] http: begin %s\n", path);
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)config->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ret = getaddrinfo(config->host, port_text, &hints, &result);
    if (ret != 0 || !result) {
        return -EHOSTUNREACH;
    }

    syslog(LOG_INFO, "[milo] http: addrinfo ok\n");
    sockfd = socket(result->ai_family, result->ai_socktype, 0);
    if (sockfd < 0) {
        ret = -errno;
        goto out_freeaddr;
    }

    /* 非阻塞连接 + poll 限时，避免不可达地址把 worker 卡在 connect。 */
    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }
    if (connect(sockfd, result->ai_addr, result->ai_addrlen) == 0) {
        /* 立即成功（本机环回等场景）。 */
    } else if (errno == EINPROGRESS) {
        pfd.fd = sockfd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, MILOCO_HTTP_CONNECT_TIMEOUT_MS) <= 0 ||
            (pfd.revents & POLLOUT) == 0) {
            syslog(LOG_WARNING, "[milo] http: connect timeout %ums\n",
                   MILOCO_HTTP_CONNECT_TIMEOUT_MS);
            ret = -ETIMEDOUT;
            goto out_close;
        }
        optlen = sizeof(error_code);
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error_code, &optlen) < 0 ||
            error_code != 0) {
            syslog(LOG_WARNING, "[milo] http: connect error=%d\n",
                   error_code);
            ret = error_code != 0 ? -error_code : -EIO;
            goto out_close;
        }
    } else {
        ret = -errno;
        goto out_close;
    }

    syslog(LOG_INFO, "[milo] http: connected fd=%d\n", sockfd);
    /* 收发阶段恢复阻塞并施加超时。 */
    if (flags >= 0) {
        fcntl(sockfd, F_SETFL, flags);
    }
    timeout.tv_sec = MILOCO_HTTP_IO_TIMEOUT_MS / 1000;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    /* HTTP/1.0 请求行强制服务器以 Content-Length 或连接关闭应答，
     * 永远不会出现 chunked，客户端因此无需分块解码。 */
    if (body) {
        request_len = (size_t)snprintf(
            request, sizeof(request),
            "%s %s HTTP/1.0\r\n"
            "Host: %s:%u\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, config->host, (unsigned)config->port,
            config->token[0] ? config->token : "-", strlen(body));
    } else {
        request_len = (size_t)snprintf(
            request, sizeof(request),
            "%s %s HTTP/1.0\r\n"
            "Host: %s:%u\r\n"
            "Authorization: Bearer %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, config->host, (unsigned)config->port,
            config->token[0] ? config->token : "-");
    }
    if (request_len >= sizeof(request)) {
        ret = -ENOMEM;
        goto out_close;
    }

    while (sent < request_len) {
        ret = send(sockfd, request + sent, request_len - sent, 0);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            ret = -errno;
            goto out_close;
        }
        sent += (size_t)ret;
    }
    if (body) {
        sent = 0;
        while (sent < strlen(body)) {
            ret = send(sockfd, body + sent, strlen(body) - sent, 0);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ret = -errno;
                goto out_close;
            }
            sent += (size_t)ret;
        }
    }

    /*
     * 响应解析状态机：头部阶段逐字节扫描，行缓冲仅保留第一行（状态
     * 行）；"\r\n\r\n" 之后进入 body，后续字节全部写入 response。
     * line_pending 标记"上一字节是行尾 \n"，其后紧跟 "\r\n"（空行）
     * 即头部结束；该判定不依赖 chunk 边界。
     */
    {
        char chunk[256];
        char status_line[40];
        char header_line[64];   /* 逐行捕获头部，命中 Date: 解析时间 */
        size_t header_len = 0;
        size_t used = 0;
        size_t line_len = 0;
        bool line_pending = false;
        size_t i;

        status_line[0] = '\0';
        header_line[0] = '\0';
        while (1) {
            ret = recv(sockfd, chunk, sizeof(chunk), 0);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ret = -errno;
                goto out_close;
            }
            if (ret == 0) {
                break; /* 连接关闭：HTTP/1.0 body 结束 */
            }
            for (i = 0; i < (size_t)ret; i++) {
                char byte = chunk[i];

                if (header_done) {
                    if (used + 1 < response_size) {
                        response[used++] = byte;
                        response[used] = '\0';
                    } else {
                        ret = -ENOMEM;
                        goto out_close;
                    }
                    continue;
                }
                if (byte == '\n') {
                    if (line_pending) {
                        /* 空行：头部结束，同 chunk 余下字节即 body。 */
                        header_done = true;
                        continue;
                    }
                    line_pending = true;
                    if (!have_status) {
                        if (sscanf(status_line, "HTTP/%*d.%*d %d",
                                   &http_status) == 1) {
                            have_status = true;
                        }
                        line_len = 0;
                    }
                    /* Date: Thu, 20 Sep 2026 19:30:00 GMT */
                    if (date_out && header_len > 6 &&
                        strncmp(header_line, "Date: ", 6) == 0) {
                        parse_http_date(header_line + 6, date_out);
                    }
                    header_len = 0;
                    header_line[0] = '\0';
                    continue;
                }
                if (byte != '\r') {
                    line_pending = false;
                    if (!have_status && line_len + 1 < sizeof(status_line)) {
                        status_line[line_len++] = byte;
                        status_line[line_len] = '\0';
                    }
                    if (header_len + 1 < sizeof(header_line)) {
                        header_line[header_len++] = byte;
                        header_line[header_len] = '\0';
                    }
                }
                /* '\r'：可能是行尾或空行组成，保持状态。 */
            }
        }
    }

    syslog(LOG_INFO, "[milo] http: done status=%d len=%u\n",
           http_status, (unsigned)strlen(response));
    if (http_status_out) {
        *http_status_out = http_status;
    }
    ret = 0;

out_close:
    close(sockfd);
out_freeaddr:
    freeaddrinfo(result);
    return ret;
}



int smart_home_miloco_http_get_date(
    const smart_home_miloco_client_config_t *config,
    const char *path,
    char *response,
    size_t response_size,
    int *http_status_out,
    time_t *date_out)
{
    if (date_out) {
        *date_out = (time_t)-1;
    }
    return http_request(config, "GET", path, NULL, response, response_size,
                        http_status_out, date_out);
}

int smart_home_miloco_http_get(const smart_home_miloco_client_config_t *config,
                               const char *path,
                               char *response,
                               size_t response_size,
                               int *http_status_out)
{
    return http_request(config, "GET", path, NULL, response, response_size,
                        http_status_out, NULL);
}

int smart_home_miloco_http_post(const smart_home_miloco_client_config_t *config,
                                const char *path,
                                const char *body,
                                char *response,
                                size_t response_size,
                                int *http_status_out)
{
    return http_request(config, "POST", path, body, response, response_size,
                        http_status_out, NULL);
}
