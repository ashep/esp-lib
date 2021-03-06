/**
 * @brief     AESPL HTTP Server
 *
 * @author    Alexander Shepetko <a@shepetko.com>
 * @copyright MIT License
 */

#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "aespl_httpd.h"

static char *log_tag = "aespl_http_server";

esp_err_t aespl_httpd_handle(aespl_httpd_t *server, httpd_method_t method, const char *uri,
                             esp_err_t (*handler)(httpd_req_t *r)) {
    if (!server->server) {
        ESP_LOGE(log_tag, "server is not started");
        return AESPL_ERR_HTTPD_NOT_STARTED;
    }

    httpd_uri_t h = {
            .uri = uri,
            .method = method,
            .handler = handler,
    };

    esp_err_t err = httpd_register_uri_handler(server->server, &h);
    if (err == ESP_OK) {
        ESP_LOGI(log_tag, "URI handler registered: %s %s", http_method_str(method), uri);
    } else {
        ESP_LOGE(log_tag, "URI handler not registered (error %d): %s %s", err, http_method_str(method), uri);
    }

    return err;
}

esp_err_t aespl_httpd_send(httpd_req_t *req, const char *status, const char *body) {
    esp_err_t err = httpd_resp_send(req, body, strlen(body));

    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "unable to send response");
        return err;
    } else {
        ESP_LOGI(log_tag, "%s %s %s", http_method_str(req->method), req->uri, status);
    }

    return err;
}

esp_err_t aespl_httpd_send_json(httpd_req_t *req, const char *status, cJSON *json) {
    esp_err_t err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "unable to set response content type");
        return err;
    }

    err = httpd_resp_set_status(req, status);
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "unable to set response status");
        return err;
    }

    char *body = cJSON_PrintUnformatted(json);
    if (!body) {
        ESP_LOGE(log_tag, "unable to format JSON");
        return ESP_FAIL;
    }

    err = aespl_httpd_send(req, status, body);
    free(body);

    return err;
}

esp_err_t aespl_httpd_send_json_result(httpd_req_t *req, const char *status, cJSON *result) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "result", result);
    esp_err_t err = aespl_httpd_send_json(req, status, root);
    cJSON_Delete(root);

    return err;
}

esp_err_t aespl_httpd_send_json_error(httpd_req_t *req, const char *status, int code) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "error", code);
    esp_err_t err = aespl_httpd_send_json(req, status, root);
    cJSON_Delete(root);

    return err;
}

esp_err_t aespl_httpd_stop(aespl_httpd_t *server) {
    esp_err_t err = httpd_stop(server->server);
    if (err) {
        return err;
    }

    server->server = NULL;
    server->config = NULL;

    return ESP_OK;
}

esp_err_t aespl_httpd_start(aespl_httpd_t *server, const httpd_config_t *config) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    server->config = &cfg;
    if (config) {
        server->config = config;
    }

    esp_err_t err = httpd_start(&server->server, server->config);
    if (err == ESP_OK) {
        ESP_LOGI(log_tag, "server started on port %d", server->config->server_port);
        return ESP_OK;
    } else {
        ESP_LOGE(log_tag, "error starting server: 0x%x", err);
        return err;
    }
}
