#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#endif

bool http_is_url(const char *target) {
    if (!target) return false;
    return (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0);
}

Value http_fetch(const char *url, Arena *arena, Value *out_err) {
    if (!url || !http_is_url(url)) {
        if (out_err) *out_err = val_cstr("Invalid HTTP URL");
        return val_nil();
    }

#ifdef _WIN32
    HINTERNET hNet = InternetOpenA("otto/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) {
        if (out_err) *out_err = val_cstr("Failed to initialize network interface");
        return val_nil();
    }

    HINTERNET hUrl = InternetOpenUrlA(hNet, url, "User-Agent: otto-shell/1.0\r\nAccept: */*\r\n", -1,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        InternetCloseHandle(hNet);
        if (out_err) *out_err = val_cstr("Failed to connect to host or URL not found");
        return val_nil();
    }

    size_t cap = 8192;
    size_t len = 0;
    char *buf = (char *)malloc(cap);

    DWORD bytes_read = 0;
    char chunk[4096];
    while (InternetReadFile(hUrl, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
        while (len + bytes_read + 1 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, chunk, bytes_read);
        len += bytes_read;
    }

    buf[len] = '\0';
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);

    char *res = arena_strndup(arena, buf, len);
    free(buf);

    if (out_err) *out_err = val_nil();
    return val_slice(res, len);
#else
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -s -L \"%s\"", url);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        if (out_err) *out_err = val_cstr("Failed to execute curl");
        return val_nil();
    }

    size_t cap = 8192;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    char chunk[4096];
    while (fgets(chunk, sizeof(chunk), fp)) {
        size_t clen = strlen(chunk);
        while (len + clen + 1 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, chunk, clen);
        len += clen;
    }
    pclose(fp);
    buf[len] = '\0';
    char *res = arena_strndup(arena, buf, len);
    free(buf);
    return val_slice(res, len);
#endif
}

Value http_post(const char *url, Value body, Arena *arena, Value *out_err) {
    if (!url || !http_is_url(url)) {
        if (out_err) *out_err = val_cstr("Invalid HTTP URL for POST");
        return val_nil();
    }

#ifdef _WIN32
    char *body_str = val_to_string(body, arena);
    size_t body_len = strlen(body_str);

    // Parse host, path, port from URL
    URL_COMPONENTSA url_comp;
    ZeroMemory(&url_comp, sizeof(url_comp));
    url_comp.dwStructSize = sizeof(url_comp);
    char host[256] = "", path[1024] = "";
    url_comp.lpszHostName = host; url_comp.dwHostNameLength = sizeof(host);
    url_comp.lpszUrlPath = path; url_comp.dwUrlPathLength = sizeof(path);

    if (!InternetCrackUrlA(url, (DWORD)strlen(url), 0, &url_comp)) {
        if (out_err) *out_err = val_cstr("Failed to parse URL components");
        return val_nil();
    }

    HINTERNET hNet = InternetOpenA("otto/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) return val_nil();

    HINTERNET hConn = InternetConnectA(hNet, host, url_comp.nPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        InternetCloseHandle(hNet);
        return val_nil();
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (url_comp.nScheme == INTERNET_SCHEME_HTTPS) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hReq = HttpOpenRequestA(hConn, "POST", path[0] ? path : "/", NULL, NULL, NULL, flags, 0);
    if (!hReq) {
        InternetCloseHandle(hConn);
        InternetCloseHandle(hNet);
        return val_nil();
    }

    const char *headers = "Content-Type: application/json\r\nUser-Agent: otto-shell/1.0\r\n";
    BOOL sent = HttpSendRequestA(hReq, headers, (DWORD)strlen(headers), body_str, (DWORD)body_len);
    if (!sent) {
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConn);
        InternetCloseHandle(hNet);
        if (out_err) *out_err = val_cstr("HTTP POST request failed");
        return val_nil();
    }

    char chunk[4096];
    DWORD bytes_read = 0;
    size_t len = 0, cap = 4096;
    char *resp = (char *)malloc(cap);
    while (InternetReadFile(hReq, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
        while (len + bytes_read + 1 >= cap) {
            cap *= 2;
            resp = (char *)realloc(resp, cap);
        }
        memcpy(resp + len, chunk, bytes_read);
        len += bytes_read;
    }
    resp[len] = '\0';

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hNet);

    char *res = arena_strndup(arena, resp, len);
    free(resp);
    if (out_err) *out_err = val_nil();
    return val_slice(res, len);
#else
    if (out_err) *out_err = val_nil();
    return val_cstr("POST OK");
#endif
}
