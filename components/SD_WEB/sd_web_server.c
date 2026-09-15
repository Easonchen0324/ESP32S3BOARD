#include "sd_web_server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#define SD_WEB_MAX_PATH       512
#define SD_WEB_QUERY_SIZE     1024
#define SD_WEB_IO_BUFFER_SIZE 4096

static const char *TAG = "sd_web";
static char s_base_path[32];
static httpd_handle_t s_http_server;
static bool s_wifi_started;

static const char s_index_html[] =
    "<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>SD卡文件管理</title><style>"
    "body{font-family:sans-serif;max-width:900px;margin:24px auto;padding:0 14px;color:#222}"
    "h2{margin-bottom:8px}.bar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:14px 0}"
    "button,input{font-size:15px;padding:7px 10px}#path{font-family:monospace;word-break:break-all}"
    "table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:9px;border-bottom:1px solid #ddd}"
    "th:nth-child(2),td:nth-child(2){text-align:right}.actions{white-space:nowrap}.actions button{margin-left:6px}"
    "#msg{min-height:24px;color:#176b2c}.error{color:#b00020!important}</style></head><body>"
    "<h2>SD卡文件管理</h2><div id='path'>/</div><div class='bar'>"
    "<button id='back'>返回上级</button><button id='refresh'>刷新</button>"
    "<input id='file' type='file'><button id='upload'>上传到当前目录</button></div>"
    "<div id='msg'></div><table><thead><tr><th>名称</th><th>大小</th><th>操作</th></tr></thead>"
    "<tbody id='items'></tbody></table><script>"
    "let current='/';const el=id=>document.getElementById(id);"
    "function join(n){return(current==='/'?'/':current+'/')+n}"
    "function message(t,bad=false){el('msg').textContent=t;el('msg').className=bad?'error':''}"
    "function size(n){if(n<1024)return n+' B';if(n<1048576)return(n/1024).toFixed(1)+' KB';return(n/1048576).toFixed(1)+' MB'}"
    "async function refresh(){message('正在读取...');try{const r=await fetch('/api/list?path='+encodeURIComponent(current));"
    "if(!r.ok)throw new Error(await r.text());const data=await r.json();el('path').textContent=current;const body=el('items');"
    "body.replaceChildren();data.entries.forEach(e=>{const tr=document.createElement('tr'),name=document.createElement('td'),"
    "sz=document.createElement('td'),act=document.createElement('td'),a=document.createElement('a');const p=join(e.name);"
    "a.textContent=(e.dir?'📁 ':'📄 ')+e.name;a.href=e.dir?'#':'/api/download?path='+encodeURIComponent(p);"
    "if(e.dir)a.onclick=x=>{x.preventDefault();current=p;refresh()};name.append(a);sz.textContent=e.dir?'':size(e.size);"
    "act.className='actions';if(!e.dir){const d=document.createElement('button');d.textContent='删除';d.onclick=async()=>{"
    "if(!confirm('删除 '+e.name+'？'))return;const q=await fetch('/api/file?path='+encodeURIComponent(p),{method:'DELETE'});"
    "if(!q.ok)message(await q.text(),true);else{message('已删除 '+e.name);refresh()}};act.append(d)}"
    "tr.append(name,sz,act);body.append(tr)});message('共 '+data.entries.length+' 项')}catch(e){message(e.message,true)}}"
    "el('back').onclick=()=>{if(current!=='/'){current=current.substring(0,current.lastIndexOf('/'))||'/';refresh()}};"
    "el('refresh').onclick=refresh;el('upload').onclick=async()=>{const f=el('file').files[0];if(!f){message('请先选择文件',true);return}"
    "message('正在上传 '+f.name+'...');try{const r=await fetch('/api/upload?path='+encodeURIComponent(join(f.name)),{method:'POST',body:f});"
    "if(!r.ok)throw new Error(await r.text());message('上传完成：'+f.name);el('file').value='';refresh()}catch(e){message(e.message,true)}};refresh();"
    "</script></body></html>";

static int hex_value(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    return -1;
}

static esp_err_t url_decode(const char *input, char *output, size_t output_size)
{
    size_t out = 0;

    while (*input != '\0')
    {
        unsigned char value;
        if (*input == '%')
        {
            if (input[1] == '\0' || input[2] == '\0')
            {
                return ESP_ERR_INVALID_ARG;
            }
            int high = hex_value(input[1]);
            int low = hex_value(input[2]);
            if (high < 0 || low < 0)
            {
                return ESP_ERR_INVALID_ARG;
            }
            value = (unsigned char)((high << 4) | low);
            input += 3;
        }
        else
        {
            value = (*input == '+') ? ' ' : (unsigned char)*input;
            input++;
        }

        if (value == '\0' || out + 1 >= output_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        output[out++] = (char)value;
    }

    output[out] = '\0';
    return ESP_OK;
}

static bool path_is_safe(const char *path)
{
    if (path[0] != '/')
    {
        return false;
    }

    const char *segment = path + 1;
    for (const unsigned char *pos = (const unsigned char *)path; *pos != '\0'; pos++)
    {
        if (*pos < 0x20 || *pos == '\\' || *pos == ':')
        {
            return false;
        }
    }

    while (*segment != '\0')
    {
        const char *end = strchr(segment, '/');
        size_t length = end == NULL ? strlen(segment) : (size_t)(end - segment);
        if (length == 2 && segment[0] == '.' && segment[1] == '.')
        {
            return false;
        }
        if (end == NULL)
        {
            break;
        }
        segment = end + 1;
    }

    return true;
}

static esp_err_t get_request_path(httpd_req_t *req, char *virtual_path,
                                  size_t virtual_size, char *full_path,
                                  size_t full_size)
{
    char query[SD_WEB_QUERY_SIZE];
    char encoded[SD_WEB_QUERY_SIZE];
    size_t query_length = httpd_req_get_url_query_len(req);

    if (query_length == 0 || query_length >= sizeof(query))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", encoded, sizeof(encoded)) != ESP_OK ||
        url_decode(encoded, virtual_path, virtual_size) != ESP_OK ||
        !path_is_safe(virtual_path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(full_path, full_size, "%s%s", s_base_path, virtual_path);
    if (written < 0 || (size_t)written >= full_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static size_t json_escape(const char *input, char *output, size_t output_size)
{
    size_t out = 0;
    while (*input != '\0')
    {
        unsigned char value = (unsigned char)*input++;
        if (value == '"' || value == '\\')
        {
            if (out + 2 >= output_size)
            {
                return 0;
            }
            output[out++] = '\\';
            output[out++] = (char)value;
        }
        else if (value < 0x20)
        {
            if (out + 6 >= output_size)
            {
                return 0;
            }
            int written = snprintf(output + out, output_size - out, "\\u%04x", value);
            if (written != 6)
            {
                return 0;
            }
            out += 6;
        }
        else
        {
            if (out + 1 >= output_size)
            {
                return 0;
            }
            output[out++] = (char)value;
        }
    }
    output[out] = '\0';
    return out;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t list_handler(httpd_req_t *req)
{
    char virtual_path[SD_WEB_MAX_PATH];
    char full_path[SD_WEB_MAX_PATH];
    if (get_request_path(req, virtual_path, sizeof(virtual_path),
                         full_path, sizeof(full_path)) != ESP_OK)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    DIR *directory = opendir(full_path);
    if (directory == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory not found");
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (httpd_resp_send_chunk(req, "{\"entries\":[", HTTPD_RESP_USE_STRLEN) != ESP_OK)
    {
        closedir(directory);
        return ESP_FAIL;
    }

    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        size_t name_length = strlen(entry->d_name);
        if (name_length >= 5 && strcmp(entry->d_name + name_length - 5, ".part") == 0)
        {
            continue;
        }

        char entry_path[SD_WEB_MAX_PATH];
        int written = snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(entry_path))
        {
            continue;
        }

        struct stat info;
        if (stat(entry_path, &info) != 0)
        {
            continue;
        }

        char escaped_name[1600];
        if (json_escape(entry->d_name, escaped_name, sizeof(escaped_name)) == 0)
        {
            continue;
        }

        char item[1800];
        written = snprintf(item, sizeof(item),
                           "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu}",
                           first ? "" : ",", escaped_name,
                           S_ISDIR(info.st_mode) ? "true" : "false",
                           (unsigned long long)info.st_size);
        if (written < 0 || (size_t)written >= sizeof(item) ||
            httpd_resp_send_chunk(req, item, written) != ESP_OK)
        {
            closedir(directory);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        first = false;
    }

    closedir(directory);
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    char virtual_path[SD_WEB_MAX_PATH];
    char full_path[SD_WEB_MAX_PATH];
    char temporary_path[SD_WEB_MAX_PATH];
    if (get_request_path(req, virtual_path, sizeof(virtual_path),
                         full_path, sizeof(full_path)) != ESP_OK ||
        strcmp(virtual_path, "/") == 0)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    int written = snprintf(temporary_path, sizeof(temporary_path), "%s.part", full_path);
    if (written < 0 || (size_t)written >= sizeof(temporary_path))
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path is too long");
    }

    unlink(temporary_path);
    FILE *file = fopen(temporary_path, "wb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Cannot create %s: errno=%d (%s)", temporary_path, errno,
                 strerror(errno));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
    }

    char *buffer = malloc(SD_WEB_IO_BUFFER_SIZE);
    if (buffer == NULL)
    {
        fclose(file);
        unlink(temporary_path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    size_t remaining = req->content_len;
    bool failed = false;
    while (remaining > 0)
    {
        size_t receive_size = remaining < SD_WEB_IO_BUFFER_SIZE ? remaining : SD_WEB_IO_BUFFER_SIZE;
        int received = httpd_req_recv(req, buffer, receive_size);
        if (received == HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue;
        }
        if (received <= 0 || fwrite(buffer, 1, received, file) != (size_t)received)
        {
            failed = true;
            break;
        }
        remaining -= received;
    }

    free(buffer);
    if (fclose(file) != 0)
    {
        failed = true;
    }
    if (failed)
    {
        unlink(temporary_path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    }

    struct stat existing;
    if (stat(full_path, &existing) == 0 && S_ISDIR(existing.st_mode))
    {
        unlink(temporary_path);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Target is a directory");
    }
    unlink(full_path);
    if (rename(temporary_path, full_path) != 0)
    {
        unlink(temporary_path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot finish upload");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char virtual_path[SD_WEB_MAX_PATH];
    char full_path[SD_WEB_MAX_PATH];
    if (get_request_path(req, virtual_path, sizeof(virtual_path),
                         full_path, sizeof(full_path)) != ESP_OK ||
        strcmp(virtual_path, "/") == 0)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    struct stat info;
    if (stat(full_path, &info) != 0)
    {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }
    if (!S_ISREG(info.st_mode))
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Only files can be deleted");
    }
    if (unlink(full_path) != 0)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
    }
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t download_handler(httpd_req_t *req)
{
    char virtual_path[SD_WEB_MAX_PATH];
    char full_path[SD_WEB_MAX_PATH];
    if (get_request_path(req, virtual_path, sizeof(virtual_path),
                         full_path, sizeof(full_path)) != ESP_OK)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    struct stat info;
    if (stat(full_path, &info) != 0 || !S_ISREG(info.st_mode))
    {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    FILE *file = fopen(full_path, "rb");
    if (file == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
    }
    char *buffer = malloc(SD_WEB_IO_BUFFER_SIZE);
    if (buffer == NULL)
    {
        fclose(file);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
    size_t count;
    esp_err_t result = ESP_OK;
    while ((count = fread(buffer, 1, SD_WEB_IO_BUFFER_SIZE, file)) > 0)
    {
        if (httpd_resp_send_chunk(req, buffer, count) != ESP_OK)
        {
            result = ESP_FAIL;
            break;
        }
    }
    if (ferror(file))
    {
        result = ESP_FAIL;
    }
    fclose(file);
    free(buffer);

    if (result == ESP_OK)
    {
        result = httpd_resp_send_chunk(req, NULL, 0);
    }
    return result;
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL)
    {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_http_server, &config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/list", .method = HTTP_GET, .handler = list_handler},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = upload_handler},
        {.uri = "/api/file", .method = HTTP_DELETE, .handler = delete_handler},
        {.uri = "/api/download", .method = HTTP_GET, .handler = download_handler},
    };

    for (size_t index = 0; index < sizeof(handlers) / sizeof(handlers[0]); index++)
    {
        ret = httpd_register_uri_handler(s_http_server, &handlers[index]);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "HTTP route registration failed: %s", esp_err_to_name(ret));
            httpd_stop(s_http_server);
            s_http_server = NULL;
            return ret;
        }
    }
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "SD file manager: http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        if (start_http_server() != ESP_OK)
        {
            ESP_LOGE(TAG, "SD file manager is unavailable");
        }
    }
}

esp_err_t sd_web_start(const char *base_path)
{
    if (base_path == NULL || base_path[0] != '/' || strlen(base_path) >= sizeof(s_base_path))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi_started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_base_path, base_path, sizeof(s_base_path));

    esp_err_t ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL);
    if (ret != ESP_OK)
    {
        return ret;
    }

    s_wifi_started = true;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        if (start_http_server() != ESP_OK)
        {
            ESP_LOGE(TAG, "SD file manager is unavailable");
        }
    }
    else
    {
        ESP_LOGI(TAG, "Waiting for Wi-Fi connection before starting SD file manager");
    }
    return ESP_OK;
}
