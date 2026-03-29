#ifndef NETWORK_H
#define NETWORK_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

typedef struct
{
    int port;
    char webroot[512];
    int max_connections;
} ServerConfig;

static inline char *trim_whitespace(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;

    if (*s == '\0')
        return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;
    end[1] = '\0';

    return s;
}

static inline void strip_inline_comment(char *s)
{
    int in_quotes = 0;
    for (char *p = s; *p != '\0'; p++) {
        if (*p == '"')
            in_quotes = !in_quotes;
        else if (*p == '#' && !in_quotes) {
            *p = '\0';
            return;
        }
    }
}

static inline int parse_int_in_range(const char *value, int min, int max, int *out)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *trim_whitespace(end) != '\0')
        return 0;
    if (parsed < min || parsed > max)
        return 0;

    *out = (int)parsed;
    return 1;
}

static inline int parse_key_value(char *line, char *key, size_t key_sz,
                                  char *value, size_t value_sz)
{
    char *sep = strchr(line, '=');
    if (sep != NULL) {
        *sep = '\0';
        char *k = trim_whitespace(line);
        char *v = trim_whitespace(sep + 1);
        if (*k == '\0' || *v == '\0')
            return 0;
        snprintf(key, key_sz, "%s", k);
        snprintf(value, value_sz, "%s", v);
        return 1;
    }

    return sscanf(line, "%63s %447[^\n]", key, value) == 2;
}

int load_config(const char *path, ServerConfig *cfg)
{
    /* Valores por defecto */
    cfg->port            = 80;
    cfg->max_connections = 5;
    strncpy(cfg->webroot, "./www", sizeof(cfg->webroot) - 1);
    cfg->webroot[sizeof(cfg->webroot) - 1] = '\0';

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), f))
    {
        line_no++;

        line[strcspn(line, "\r\n")] = '\0';
        strip_inline_comment(line);
        char *clean = trim_whitespace(line);
        if (clean[0] == '\0')
            continue;

        char key[64], value[448];
        if (!parse_key_value(clean, key, sizeof(key), value, sizeof(value)))
        {
            fprintf(stderr, "[WARN] %s:%d linea invalida, ignorada\n", path, line_no);
            continue;
        }

        char *v = trim_whitespace(value);
        size_t len = strlen(v);
        if (len >= 2 && v[0] == '"' && v[len - 1] == '"') {
            v[len - 1] = '\0';
            v++;
            v = trim_whitespace(v);
        }

        if (strcmp(key, "port") == 0) {
            int port_value;
            if (parse_int_in_range(v, 1, 65535, &port_value)) {
                cfg->port = port_value;
            } else {
                fprintf(stderr, "[WARN] %s:%d port invalido '%s' (1..65535)\n",
                        path, line_no, v);
            }
        }
        else if (strcmp(key, "webroot") == 0) {
            struct stat st;
            char candidate[sizeof(cfg->webroot)];
            snprintf(candidate, sizeof(candidate), "%s", v);
            if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncpy(cfg->webroot, candidate, sizeof(cfg->webroot) - 1);
                cfg->webroot[sizeof(cfg->webroot) - 1] = '\0';
            } else {
                fprintf(stderr, "[WARN] %s:%d webroot invalido '%s' (no existe o no es directorio)\n",
                        path, line_no, v);
            }
        }
        else if (strcmp(key, "max_connections") == 0) {
            int max_conn;
            if (parse_int_in_range(v, 1, 10000, &max_conn)) {
                cfg->max_connections = max_conn;
            } else {
                fprintf(stderr, "[WARN] %s:%d max_connections invalido '%s' (1..10000)\n",
                        path, line_no, v);
            }
        } else {
            fprintf(stderr, "[WARN] %s:%d clave desconocida '%s', ignorada\n",
                    path, line_no, key);
        }
    }

    fclose(f);
    return 1;
}




void fatal(const char *message)
{
    char error_message[100];
    snprintf(error_message, sizeof(error_message), "[!!] Fatal Error: %s", message);
    perror(error_message);
    exit(-1);
}


int send_string(int sockfd, const unsigned char *buffer)
{
    ssize_t sent_bytes;
    size_t bytes_to_send;
    bytes_to_send = strlen((const char *)buffer);

    while (bytes_to_send > 0)
    {
        sent_bytes = send(sockfd, buffer, bytes_to_send, 0);
        if (sent_bytes < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (sent_bytes == 0)
            return 0;
        bytes_to_send -= (size_t)sent_bytes;
        buffer += sent_bytes;
    }

    return 1;
}


int recv_line(int sockfd, unsigned char *dest_buffer, size_t dest_cap)
{
#define EOL "\r\n"
#define EOL_SIZE 2
    unsigned char *ptr;
    int eol_matched = 0;
    size_t used = 0;

    if (dest_cap == 0)
        return 0;
    ptr = dest_buffer;
    while (used + 1 < dest_cap)
    {
        ssize_t n = recv(sockfd, ptr, 1, 0);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }

        if (*ptr == EOL[eol_matched])
        {
            eol_matched++;
            if (eol_matched == EOL_SIZE)
            {
                *(ptr + 1 - EOL_SIZE) = '\0';
                return strlen((char *)dest_buffer);
            }
        }
        else
        {
            eol_matched = 0;
        }
        ptr++;
        used++;
    }
    *ptr = '\0';
    return 0;
}

#endif /*NETWORK.H */
