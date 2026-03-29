#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <semaphore.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
 
#include "../lib/network.h"
 
/* ── Colores ANSI — paleta Nord/Aurora ───────────────────────────
 * 2xx Success   aurora4  #a3be8c  → verde
 * 3xx Redirect  aurora5  #b48ead  → violeta
 * 4xx Client    aurora2  #d08770  → naranja
 * 5xx Server    aurora1  #bf616a  → rojo
 * INFO          frost2   #88c0d0  → celeste
 * WARN          aurora3  #ebcb8b  → amarillo
 * ---------------------------------------------------------------- */

#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_2XX     "\033[38;2;163;190;140m"   /* aurora4 — verde   */
#define ANSI_3XX     "\033[38;2;180;142;173m"   /* aurora5 — violeta */
#define ANSI_4XX     "\033[38;2;208;135;112m"   /* aurora2 — naranja */
#define ANSI_5XX     "\033[38;2;191;97;106m"    /* aurora1 — rojo    */
#define ANSI_INFO    "\033[38;2;136;192;208m"   /* frost2  — celeste */
#define ANSI_WARN    "\033[38;2;235;203;139m"   /* aurora3 — amarillo*/
#define ANSI_DIM     "\033[2m"
 
static const char *http_color(int code)
{
    if (code >= 200 && code < 300) return ANSI_2XX;
    if (code >= 300 && code < 400) return ANSI_3XX;
    if (code >= 400 && code < 500) return ANSI_4XX;
    if (code >= 500)               return ANSI_5XX;
    return ANSI_RESET;
}
 
static void log_response(int code, const char *method,
                          const char *path, const char *client_ip,
                          long long bytes)
{
    const char *col = http_color(code);
    if (bytes >= 0)
        printf("%s[%d]%s %s%-5s%s %s  %s%s  %s(%lld bytes)%s\n",
               col, code, ANSI_RESET,
               ANSI_DIM, method, ANSI_RESET,
               path,
               ANSI_DIM, client_ip,
               ANSI_DIM, bytes, ANSI_RESET);
    else
        printf("%s[%d]%s %s%-5s%s %s  %s%s%s\n",
               col, code, ANSI_RESET,
               ANSI_DIM, method, ANSI_RESET,
               path,
               ANSI_DIM, client_ip, ANSI_RESET);
}
 
#define SEM_NAME "/arcd_connections"
 
static sem_t *conn_sem = NULL;
 
void handle_connection(int sockfd, struct sockaddr_in *client_addr_ptr,
                       const ServerConfig *cfg);
off_t get_file_size(int fd);
void sigchld_handler(int sig);
static int send_all(int sockfd, const unsigned char *buf, size_t len);
static int send_file_chunked(int sockfd, int fd);
static const char *get_content_type(const char *path);
static int path_is_within_webroot(const char *webroot, const char *candidate);
static int is_cgi_python(const char *uri_path, const char *resource_path);
static int run_cgi(int sockfd, const char *script_fs_path, const char *script_uri_path,
                   const char *query_string, const char *client_ip);
 
int main(int argc, char *argv[])
{
    const char *conf_path = (argc > 1) ? argv[1] : "./arcd.conf";
    ServerConfig cfg;
 
    if (!load_config(conf_path, &cfg))
    {
        fprintf(stderr, "[!] No se pudo abrir '%s'. "
                        "Se usan valores por defecto.\n",
                conf_path);
    }
 
    printf(ANSI_INFO "[*]" ANSI_RESET " Configuración del servidor:\n");
    printf("    Puerto                       : " ANSI_BOLD "%d\n" ANSI_RESET, cfg.port);
    printf("    Raíz web                     : " ANSI_BOLD "%s\n" ANSI_RESET, cfg.webroot);
    printf("    Máximo de conexiones simult. : " ANSI_BOLD "%d\n" ANSI_RESET, cfg.max_connections);
 
    sem_unlink(SEM_NAME);
    conn_sem = sem_open(SEM_NAME, O_CREAT | O_EXCL, 0600, cfg.max_connections);
    if (conn_sem == SEM_FAILED)
        fatal("creando semáforo");
 
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
 
    int sockfd, yes = 1;
    struct sockaddr_in host_addr, client_addr;
    socklen_t sin_size;
 
    if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
        fatal("creando socket");
 
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
        fatal("configurando SO_REUSEADDR");
 
    host_addr.sin_family = AF_INET;
    host_addr.sin_port = htons(cfg.port);
    host_addr.sin_addr.s_addr = INADDR_ANY;
    memset(&(host_addr.sin_zero), '\0', 8);
 
    if (bind(sockfd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr)) == -1)
        fatal("haciendo bind en el puerto");
 
    if (listen(sockfd, cfg.max_connections) == -1)
        fatal("escuchando en el socket");
 
    printf(ANSI_INFO "[*]" ANSI_RESET " Servidor activo en el puerto: " ANSI_BOLD "%d\n" ANSI_RESET, cfg.port);
    printf(ANSI_INFO "[*]" ANSI_RESET " CGI Python: URI bajo /cgi-bin/ con extensión .py (python3)\n");
 
    while (1)
    {
        sin_size = sizeof(struct sockaddr_in);
        int new_fd = accept(sockfd, (struct sockaddr *)&client_addr, &sin_size);
 
        if (new_fd == -1)
        {
            perror(ANSI_4XX "[!] aceptando conexión" ANSI_RESET);
            continue;
        }
 
        while (sem_wait(conn_sem) == -1)
        {
            if (errno != EINTR)
            {
                perror(ANSI_5XX "[!] sem_wait" ANSI_RESET);
                close(new_fd);
                new_fd = -1;
                break;
            }
        }
        if (new_fd == -1)
            continue;
 
        pid_t pid = fork();
        if (pid == -1)
        {
            perror(ANSI_5XX "[!] fork" ANSI_RESET);
            sem_post(conn_sem);
            close(new_fd);
            continue;
        }
 
        if (pid == 0)
        {
            close(sockfd);
            handle_connection(new_fd, &client_addr, &cfg);
            sem_post(conn_sem);
            exit(0);
        }
 
        close(new_fd);
    }
 
    sem_close(conn_sem);
    sem_unlink(SEM_NAME);
    return 0;
}
 
void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}
 
void handle_connection(int sockfd,
                       struct sockaddr_in *client_addr_ptr,
                       const ServerConfig *cfg)
{
    char request[1024], resource[1024 + 512];
    char client_ip[INET_ADDRSTRLEN];
 
    int req_len;
    off_t file_size;
 
    char header[256];
 
    unsigned char *ptr;
    int fd;
 
    {
        const char *ia = inet_ntoa(client_addr_ptr->sin_addr);
        snprintf(client_ip, sizeof(client_ip), "%s", ia ? ia : "");
    }
 
    req_len = recv_line(sockfd, (unsigned char *)request, sizeof(request));
    if (req_len <= 0)
    {
        close(sockfd);
        return;
    }
 
    printf(ANSI_INFO "[>]" ANSI_RESET " %s%s%s - %s\n",
           ANSI_DIM, client_ip, ANSI_RESET, request);
 
    ptr = (unsigned char *)strstr(request, " HTTP/");
    if (ptr == NULL)
    {
        printf(ANSI_WARN "[!] Solicitud no HTTP, ignorada.\n" ANSI_RESET);
        close(sockfd);
        return;
    }
    *ptr = '\0';
 
    char *path = NULL;
    int head_only = 0;
 
    if (strncmp(request, "GET ", 4) == 0)
        path = request + 4;
    else if (strncmp(request, "HEAD ", 5) == 0)
    {
        path = request + 5;
        head_only = 1;
    }
 
    if (path == NULL)
    {
        send_string(sockfd,
                    (unsigned char *)"HTTP/1.0 501 Not Implemented\r\n\r\n");
        close(sockfd);
        return;
    }
 
    if (strstr(path, "..") != NULL)
    {
        printf(ANSI_4XX "[!] Intento de path traversal: %s\n" ANSI_RESET, path);
        send_string(sockfd,
                    (unsigned char *)"HTTP/1.0 403 Forbidden\r\n\r\n");
        close(sockfd);
        return;
    }
 
    const char *query_string = "";
    char *qmark = strchr(path, '?');
    if (qmark != NULL)
    {
        *qmark = '\0';
        query_string = qmark + 1;
    }
 
    if (path[0] == '\0')
    {
        send_string(sockfd,
                    (unsigned char *)"HTTP/1.0 400 Bad Request\r\n\r\n");
        close(sockfd);
        return;
    }
 
    if (path[strlen(path) - 1] == '/')
        snprintf(resource, sizeof(resource),
                 "%s%sindex.html", cfg->webroot, path);
    else
        snprintf(resource, sizeof(resource),
                 "%s%s", cfg->webroot, path);
 
    if (!path_is_within_webroot(cfg->webroot, resource))
    {
        printf(ANSI_4XX "[!] Recurso fuera de webroot: %s\n" ANSI_RESET, resource);
        send_string(sockfd,
                    (unsigned char *)"HTTP/1.0 403 Forbidden\r\n\r\n");
        close(sockfd);
        return;
    }
 
    const char *method = head_only ? "HEAD" : "GET";
 
    struct stat st;
    if (stat(resource, &st) == -1)
    {
        log_response(404, method, resource, client_ip, -1);
        send_string(sockfd,
                    (unsigned char *)"HTTP/1.0 404 NOT FOUND\r\n");
        send_string(sockfd,
                    (unsigned char *)"Content-Type: text/html\r\n\r\n");
 
        if (!head_only)
            send_string(sockfd,
                        (unsigned char *)"<html><body>"
                                         "<h1>404 Not Found</h1>"
                                         "</body></html>");
    }
    else if (S_ISREG(st.st_mode) && is_cgi_python(path, resource))
    {
        if (head_only)
        {
            log_response(501, "HEAD", resource, client_ip, -1);
            send_string(sockfd,
                        (unsigned char *)"HTTP/1.0 501 Not Implemented\r\n"
                                        "Content-Type: text/plain\r\n\r\n"
                                        "HEAD no está soportado para CGI.\r\n");
        }
        else
        {
            log_response(200, "CGI ", resource, client_ip, -1);
            if (run_cgi(sockfd, resource, path, query_string, client_ip) == -1)
                perror(ANSI_5XX "[!] CGI" ANSI_RESET);
        }
    }
    else
    {
        fd = open(resource, O_RDONLY);
        if (fd == -1)
        {
            log_response(404, method, resource, client_ip, -1);
            send_string(sockfd,
                        (unsigned char *)"HTTP/1.0 404 NOT FOUND\r\n");
            send_string(sockfd,
                        (unsigned char *)"Content-Type: text/html\r\n\r\n");
 
            if (!head_only)
                send_string(sockfd,
                            (unsigned char *)"<html><body>"
                                             "<h1>404 Not Found</h1>"
                                             "</body></html>");
        }
        else
        {
            file_size = get_file_size(fd);
            if (file_size < 0)
            {
                perror(ANSI_5XX "[!] fstat" ANSI_RESET);
                close(fd);
                close(sockfd);
                return;
            }
 
            log_response(200, method, resource, client_ip, (long long)file_size);
 
            send_string(sockfd, (unsigned char *)"HTTP/1.0 200 OK\r\n");
            send_string(sockfd, (unsigned char *)"Server: Arcd Server\r\n");
            send_string(sockfd, (unsigned char *)"Connection: close\r\n");
 
            snprintf(header, sizeof(header),
                     "Content-Length: %lld\r\n", (long long)file_size);
            send_string(sockfd, (unsigned char *)header);
 
            snprintf(header, sizeof(header),
                     "Content-Type: %s\r\n\r\n", get_content_type(resource));
            send_string(sockfd, (unsigned char *)header);
 
            if (!head_only)
            {
                if (send_file_chunked(sockfd, fd) == -1)
                    perror(ANSI_5XX "[!] enviando archivo" ANSI_RESET);
            }
            close(fd);
        }
    }
 
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
}
 
off_t get_file_size(int fd)
{
    struct stat stat_struct;
    if (fstat(fd, &stat_struct) == -1)
        return -1;
    return stat_struct.st_size;
}
 
static int send_all(int sockfd, const unsigned char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(sockfd, buf + sent, len - sent, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}
 
static int send_file_chunked(int sockfd, int fd)
{
    unsigned char buffer[8192];
    while (1)
    {
        ssize_t r = read(fd, buffer, sizeof(buffer));
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return 0;
        if (send_all(sockfd, buffer, (size_t)r) == -1)
            return -1;
    }
}
 
static const char *get_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";
 
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";
    if (strcmp(ext, ".css") == 0)
        return "text/css";
    if (strcmp(ext, ".js") == 0)
        return "application/javascript";
    if (strcmp(ext, ".json") == 0)
        return "application/json";
    if (strcmp(ext, ".txt") == 0)
        return "text/plain";
    if (strcmp(ext, ".png") == 0)
        return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".gif") == 0)
        return "image/gif";
    if (strcmp(ext, ".svg") == 0)
        return "image/svg+xml";
    if (strcmp(ext, ".pdf") == 0)
        return "application/pdf";
    if (strcmp(ext, ".ico") == 0)
        return "image/x-icon";
 
    return "application/octet-stream";
}
 
static int path_is_within_webroot(const char *webroot, const char *candidate)
{
    char root_real[PATH_MAX];
    char cand_real[PATH_MAX];
 
    if (realpath(webroot, root_real) == NULL)
        return 0;
 
    if (realpath(candidate, cand_real) == NULL)
        return 0;
 
    size_t root_len = strlen(root_real);
    if (strncmp(root_real, cand_real, root_len) != 0)
        return 0;
 
    if (cand_real[root_len] != '\0' && cand_real[root_len] != '/')
        return 0;
 
    return 1;
}
 
static int is_cgi_python(const char *uri_path, const char *resource_path)
{
    if (strncmp(uri_path, "/cgi-bin/", 9) != 0)
        return 0;
    size_t len = strlen(resource_path);
    if (len < 3)
        return 0;
    return strcmp(resource_path + len - 3, ".py") == 0;
}
 
static int run_cgi(int sockfd, const char *script_fs_path, const char *script_uri_path,
                   const char *query_string, const char *client_ip)
{
    int pipefd[2];
    if (pipe(pipefd) == -1)
        return -1;
 
    pid_t pid = fork();
    if (pid == -1)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
 
    if (pid == 0)
    {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) == -1)
            _exit(126);
        close(pipefd[1]);
 
        /*
         * Resolver ruta absoluta ANTES de chdir: si webroot es relativo (p.ej. ./www),
         * tras chdir al cgi-bin una ruta relativa apunta mal (./www/cgi-bin/... duplicado).
         */
        char script_abs[PATH_MAX];
        if (realpath(script_fs_path, script_abs) == NULL)
            _exit(126);
 
        char *dir_copy = strdup(script_abs);
        if (dir_copy)
        {
            char *slash = strrchr(dir_copy, '/');
            if (slash)
            {
                *slash = '\0';
                (void)chdir(dir_copy);
            }
            free(dir_copy);
        }
 
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.0", 1);
        setenv("REQUEST_METHOD", "GET", 1);
        setenv("QUERY_STRING", query_string ? query_string : "", 1);
        setenv("REMOTE_ADDR", client_ip ? client_ip : "", 1);
        setenv("SCRIPT_NAME", script_uri_path ? script_uri_path : "", 1);
        setenv("SERVER_SOFTWARE", "Arcd/0.1", 1);
        setenv("PATH_TRANSLATED", script_abs, 1);
        setenv("PATH_INFO", "", 1);
 
        execlp("python3", "python3", script_abs, (char *)NULL);
        _exit(127);
    }
 
    close(pipefd[1]);
 
    send_string(sockfd, (unsigned char *)"HTTP/1.0 200 OK\r\n");
    send_string(sockfd, (unsigned char *)"Connection: close\r\n");
 
    unsigned char buff[8192];
    ssize_t n;
    while ((n = read(pipefd[0], buff, sizeof(buff))) != 0)
    {
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (send_all(sockfd, buff, (size_t)n) == -1)
            break;
    }
    close(pipefd[0]);
 
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
        ;
    (void)status;
    return 0;
}