#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>
#include <sys/socket.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "http_parser.h"
#include "http_header.h"
#include "aespl_http_client.h"

#define HTTP_VERSION "HTTP/1.0"
#define USER_AGENT "aespl/1.0"

static const char *LOG_TAG = "aespl_http_client";

static aespl_http_response *current_resp = NULL;
static char *current_header_f = NULL;
static char *current_header_v = NULL;

static int http_parser_on_headers_complete(http_parser *parser) {
    if (current_header_f) {
        free(current_header_f);
    }

    if (current_header_v) {
        free(current_header_v);
    }

    return 0;
}

static int http_parser_on_message_complete(http_parser *parser) {
    printf("on_message_complete\n");
    return 0;
}

static int http_parser_on_header_field(http_parser *parser, const char *at, size_t length) {
    if (current_header_f) {
        free(current_header_f);
    }

    current_header_f = malloc(length + 1);
    bzero(current_header_f, length + 1);
    strncpy(current_header_f, at, length);

    return 0;
}

static int http_parser_on_header_value(http_parser *parser, const char *at, size_t length) {
    if (!current_header_f) {
        return 0;
    }

    if (current_header_v) {
        free(current_header_v);
    }

    current_header_v = malloc(length + 1);
    bzero(current_header_v, length + 1);
    strncpy(current_header_v, at, length);

    http_header_set(current_resp->headers, current_header_f, current_header_v);

    return 0;
}

static int http_parser_on_body(http_parser *parser, const char *at, size_t length) {
    current_resp->body = malloc(length + 1);
    bzero(current_resp->body, length + 1);
    strncpy(current_resp->body, at, length);

    return 0;
}

esp_err_t aespl_http_client_request(aespl_http_response *resp, enum http_method method, const char *url,
                                    http_header_handle_t headers, const char *body) {
    int err_i;

    // Parse the URL
    struct http_parser_url parsed_url;
    http_parser_url_init(&parsed_url);
    err_i = http_parser_parse_url(url, strlen(url), false, &parsed_url);
    if (err_i) {
        ESP_LOGE("Error parsing URL: %s", url);
        return err_i;
    }

    // Parse hostname
    if (!(parsed_url.field_set & (1 << UF_HOST))) {
        ESP_LOGE("Host not found in URL: %s", url);
        return err_i;
    }
    char *host = malloc(parsed_url.field_data[UF_HOST].len + 1);
    bzero(host, parsed_url.field_data[UF_HOST].len + 1);
    strncpy(host, url + parsed_url.field_data[UF_HOST].off, parsed_url.field_data[UF_HOST].len);

    // Parse port
    char *port;
    if (parsed_url.field_set & (1 << UF_PORT)) {
        port = malloc(parsed_url.field_data[UF_PORT].len + 1);
        bzero(port, parsed_url.field_data[UF_PORT].len + 1);
        strncpy(port, url + parsed_url.field_data[UF_PORT].off, parsed_url.field_data[UF_PORT].len);
    } else {
        port = malloc(3);
        strcpy(port, "80");
    }

    // Resolve address
    struct addrinfo *addr_info;
    const struct addrinfo addr_info_hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    err_i = getaddrinfo(host, port, &addr_info_hints, &addr_info);
    if (err_i != 0 || addr_info == NULL) {
        ESP_LOGE(LOG_TAG, "getaddrinfo(\"%s\", \"%s\") failed, error %d", host, port, err_i);
        free(host);
        free(port);
        return err_i;
    }
    struct in_addr *addr = &((struct sockaddr_in *)addr_info->ai_addr)->sin_addr;
    ESP_LOGI(LOG_TAG, "IP address of \"%s\": %s", host, inet_ntoa(*addr));

    // Allocate socket
    int sock = socket(addr_info->ai_family, addr_info->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGE(LOG_TAG, "Failed to allocate socket");
        freeaddrinfo(addr_info);
        free(host);
        free(port);
        return sock;
    }

    // Open connection
    err_i = connect(sock, addr_info->ai_addr, addr_info->ai_addrlen);
    if (err_i != 0) {
        ESP_LOGE(LOG_TAG, "Socket connect failed, error %d", errno);
        close(sock);
        freeaddrinfo(addr_info);
        free(host);
        free(port);
        return err_i;
    }
    freeaddrinfo(addr_info);

    // Parse path
    char *path;
    if (parsed_url.field_set & (1 << UF_PATH)) {
        path = malloc(parsed_url.field_data[UF_PATH].len + 1);
        bzero(path, parsed_url.field_data[UF_PATH].len + 1);
        strncpy(path, url + parsed_url.field_data[UF_PATH].off, parsed_url.field_data[UF_PATH].len);
    } else {
        path = malloc(2);
        strcpy(path, "/");
    }

    // Parse query
    char *query;
    if (parsed_url.field_set & (1 << UF_QUERY)) {
        query = malloc(parsed_url.field_data[UF_QUERY].len + 1);
        bzero(query, parsed_url.field_data[UF_QUERY].len + 1);
        strncpy(query, url + parsed_url.field_data[UF_QUERY].off, parsed_url.field_data[UF_QUERY].len);
    } else {
        query = malloc(1);
        strcpy(query, "");
    }

    // Build headers string
    char *headers_str = NULL;
    int headers_str_len = HTTP_MAX_HEADER_SIZE;
    if (headers) {
        headers_str = malloc(headers_str_len);
        http_header_generate_string(headers, 0, headers_str, &headers_str_len);
    }

    // Calculate request length
    uint16_t req_len = strlen(http_method_str(method)) + 1 + strlen(path) + 1 + strlen(HTTP_VERSION) + 2;
    if (query) {
        req_len += 1 + strlen(query);
    }
    req_len += strlen("Host: ") + strlen(host) + 2;
    req_len += strlen("User-Agent: ") + strlen(USER_AGENT) + 2;
    if (headers_str) {
        req_len += headers_str_len;
    }
    req_len += 2;  // "\r\n"
    if (method != HTTP_GET && body) {
        req_len += strlen(body);
    }
    req_len += 1;  // '\0'

    // Build request string
    char *req = malloc(req_len);
    sprintf(req, "%s %s", http_method_str(method), path);
    if (strlen(query)) {
        sprintf(req, "%s?%s", req, query);
    }
    sprintf(req, "%s %s\r\n", req, HTTP_VERSION);
    sprintf(req, "%sHost: %s\r\n", req, host);
    sprintf(req, "%sUser-Agent: %s\r\n", req, USER_AGENT);
    if (headers_str) {
        sprintf(req, "%s%s", req, headers_str);
    }
    sprintf(req, "%s\r\n", req);
    if (method != HTTP_GET && body) {
        strcat(req, body);
    }

    // Free URL
    free(host);
    free(port);
    free(path);
    free(query);

    printf("--- REQUEST\n");
    printf("%s", req);
    printf("---\n");

    // Send request
    err_i = write(sock, req, req_len);
    if (err_i < 0) {
        ESP_LOGE(LOG_TAG, "Socket write failed");
        free(req);
        close(sock);
        return err_i;
    }
    free(req);

    // Set socket receive timeout
    struct timeval rcv_timeout = {
        .tv_sec = 5,
        .tv_usec = 0,
    };
    err_i = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    if (err_i < 0) {
        ESP_LOGE(LOG_TAG, "Failed to set socket receiving timeout");
        close(sock);
        return err_i;
    }

    // Read HTTP response
    size_t resp_len = 0;
    char *resp_str = malloc(1);
    resp_str[0] = 0;
    ssize_t bytes_read;
    do {
        // Read a piece of data
        char buf[64];
        bytes_read = read(sock, buf, sizeof(buf) - 1);
        if (bytes_read < 0) {
            ESP_LOGW(LOG_TAG, "Error while reading from the socket: %d", bytes_read);
            free(resp_str);
            close(sock);
            return bytes_read;
        }

        // Concatenate read bytes
        if (bytes_read > 0) {
            resp_len += bytes_read + 1;
            resp_str = realloc(resp_str, resp_len);
            if (!resp_str) {
                ESP_LOGE(LOG_TAG, "Error while allocating memory for the response");
                free(resp_str);
                close(sock);
                return ESP_ERR_NO_MEM;
            }
            buf[bytes_read] = 0;  // ensure end of the string marker
            strcat(resp_str, buf);
        }
    } while (bytes_read > 0);

    // Free socket
    close(sock);

    printf("--- RESPONSE\n");
    printf("%s", resp_str);
    printf("---\n");

    // Setup HTTP parser
    http_parser parser;
    http_parser_settings parser_settings;
    http_parser_init(&parser, HTTP_RESPONSE);
    http_parser_settings_init(&parser_settings);

    // Setup parser callbacks
    parser_settings.on_header_field = &http_parser_on_header_field;
    parser_settings.on_header_value = &http_parser_on_header_value;
    parser_settings.on_headers_complete = &http_parser_on_headers_complete;
    parser_settings.on_body = &http_parser_on_body;
    parser_settings.on_message_complete = &http_parser_on_message_complete;

    // Initialize response struct
    memset(resp, 0, sizeof(*resp));
    resp->headers = http_header_init();

    // Parse the response
    current_resp = resp;
    http_parser_execute(&parser, &parser_settings, resp_str, resp_len);

    // Free response string
    free(resp_str);

    resp->status_code = parser.status_code;
    resp->content_length = parser.content_length;

    return ESP_OK;
}

esp_err_t aespl_http_client_get(aespl_http_response *resp, const char *url, http_header_handle_t headers) {
    return aespl_http_client_request(resp, HTTP_GET, url, headers, NULL);
}

void aespl_http_client_free(aespl_http_response *resp) {
    if (resp->body) {
        free(resp->body);
    }

    if (current_header_f) {
        free(current_header_f);
    }

    if (current_header_v) {
        free(current_header_v);
    }

    if (resp->headers) {
        http_header_destroy(resp->headers);
    }
}
