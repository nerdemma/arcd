# arcd

<img src="docs/logo.png">

Servidor HTTP ligero escrito en C, diseñado para ser simple, auditable y fácil de extender. Pensado para entornos embebidos, laboratorios de redes y aprendizaje de programación de sistemas.

---

## Características

- Servidor HTTP/1.0 con soporte para `GET` y `HEAD`
- Configuración externa mediante `arcd.conf` — sin recompilar para cambiar puerto o directorio
- Control de concurrencia con semáforos POSIX — límite configurable de conexiones simultáneas
- Cada conexión se atiende en un proceso hijo independiente (`fork`)
- Protección básica contra path traversal (`../`)
- Sin dependencias externas salvo `libc` y `libpthread`

---

## Estructura del proyecto

```
arcd/
├── arcd.conf       # Archivo de configuración
├── src/
│   └──main.c       # Servidor principal
├── lib/
│   └── network.h   # Funciones de red y parser de configuración
├── www/            # Directorio raíz de archivos web (configurable)
│   └── index.html
└── cgi-bin/            # Directorio de Scripts CGI
```

---

## Compilación

```bash
gcc -Wall -Wextra -o arcd main.c -lpthread
```

Requiere GCC y las cabeceras de desarrollo estándar de Linux. Probado en Debian 12 y Ubuntu 22.04.

---

## Configuración

El servidor lee `arcd.conf` al iniciar. Si no encuentra el archivo usa valores por defecto.

```ini
# arcd.conf

# Puerto en el que escucha el servidor
port 8080

# Directorio raíz de los archivos web
www ./www

# Máximo de conexiones simultáneas
max_connections 10
```

| Clave            | Por defecto | Descripción                                 |
|------------------|-------------|---------------------------------------------|
| `port`           | `80`        | Puerto TCP donde escucha el servidor        |
| `www`            | `./www`     | Ruta al directorio raíz de archivos web     |
| `max_connections`| `5`         | Límite de conexiones concurrentes           |

---

## Uso

```bash
# Iniciar con la configuración por defecto (./arcd.conf)
sudo ./arcd

# Especificar un archivo de configuración alternativo
sudo ./arcd /etc/arcd/arcd.conf
```

Al iniciar, el servidor imprime la configuración activa:

```
[*] Configuración del servidor:
    Puerto                       : 8080
    Raíz web                     : ./www
    Máximo de conexiones simult. : 10
[*] Servidor activo en el puerto: 8080
```

---

## Cómo funciona

```
Cliente TCP
    │
    ▼
accept()  ←── bucle principal (proceso padre)
    │
    ├── sem_wait()          espera un slot libre
    │
    └── fork()
          │
          └── proceso hijo
                │
                ├── recv_line()       lee la petición HTTP
                ├── valida método     GET / HEAD
                ├── verifica path     bloquea ".."
                ├── open()            abre el archivo
                └── send()            envía respuesta
```

Cuando el hijo termina, libera el slot del semáforo y el padre lo recolecta con `SIGCHLD` para evitar procesos zombie.

---

## Respuestas HTTP

| Situación                     | Código           |
|-------------------------------|------------------|
| Archivo encontrado            | `200 OK`         |
| Archivo no encontrado         | `404 Not Found`  |
| Método no soportado           | `501 Not Implemented` |
| Intento de path traversal     | `403 Forbidden`  |

---

## Roadmap

- [ ] MIME types — servir CSS, JS, imágenes correctamente
- [ ] Logging a archivo con timestamp
- [ ] Soporte CGI para scripts Python y Bash
- [ ] Recarga de configuración con `SIGHUP` sin reiniciar
- [ ] HTTPS mediante `mbedTLS` o `OpenSSL`

---

## Limitaciones conocidas

- HTTP/1.0 únicamente — no hay keep-alive ni pipelining
- Todo el archivo se carga en memoria antes de enviarlo — no apto para archivos grandes
- El `Content-Type` es siempre `text/html` hasta implementar la tabla de MIME types
- Soporte para Perl, Php y Motores de base de datos no disponible

---

## Licencia

GPL — ver `LICENSE` para más detalles.