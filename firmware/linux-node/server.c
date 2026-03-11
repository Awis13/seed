/*
 * seed.c — AI-growable firmware (grown node)
 * Компиляция: gcc -O2 -o seed server.c -lpthread
 * Запуск: ./seed [port] [serial_device] [baudrate]
 *   По умолчанию: порт 8080, serial /dev/ttyGS0, 115200 baud
 *
 * Эндпоинты (KVM):
 *   GET  /status            — состояние serial, keyboard, USB и host
 *   GET  /health            — быстрый health check (usb, host, ip, uptime)
 *   POST /exec              — отправить команду, получить вывод
 *   POST /read              — прочитать текущий буфер
 *   POST /type              — напечатать текст в serial
 *   GET  /serial/list       — список доступных serial портов
 *   POST /serial/switch     — переключить serial (body: device:baudrate)
 *   POST /serial/enable     — включить PL011 UART на GPIO 14/15, отключить BT
 *   POST /keyboard/type     — напечатать текст через USB HID клавиатуру
 *   POST /keyboard/keys     — комбинации клавиш (body: "ctrl+c", "alt+tab", etc.)
 *   GET  /keyboard/status   — статус HID устройства
 *   GET  /hosts             — список спаренных хостов + статус подключения
 *   GET  /host/info         — информация о хосте (с проверкой связности)
 *
 * Эндпоинты (Seed):
 *   POST /gpio/{pin}/mode   — задать режим пина (input/output) через sysfs
 *   POST /gpio/{pin}/write  — установить значение пина (high/low/1/0)
 *   GET  /gpio/{pin}/read   — прочитать значение пина
 *   GET  /gpio/status       — статус всех сконфигурированных пинов
 *   POST /i2c/scan          — сканировать I2C шину
 *   POST /i2c/{addr}/write  — записать байты на I2C устройство
 *   GET  /i2c/{addr}/read/{count} — прочитать N байт с I2C устройства
 *   GET  /config.md         — прочитать конфиг-файл ноды
 *   POST /config.md         — записать конфиг-файл ноды
 *   GET  /capabilities      — возможности ноды (JSON)
 *   POST /deploy            — загрузить и запустить скрипт как systemd-сервис
 *   GET  /deploy            — статус задеплоенного скрипта
 *   DELETE /deploy          — остановить и удалить задеплоенный скрипт
 *   GET  /deploy/logs       — логи задеплоенного скрипта (ring buffer)
 *   GET  /live.md           — runtime state markdown
 *   POST /live.md           — записать runtime state
 *   GET  /events            — event log с timestamps (?since=<unix_ts>)
 *   GET  /mesh              — mDNS discovery соседних Seed нод
 *   POST /system/exec       — выполнить команду на самой Pi (не на хосте)
 *   POST /system/reboot     — перезагрузить Pi
 *
 * Эндпоинты (WireGuard):
 *   GET  /wg/status          — статус интерфейса wg0 (up/down, public key, listen port)
 *   POST /wg/config          — настроить wg0 (address, listen_port)
 *   POST /wg/peer            — добавить/обновить пира
 *   DELETE /wg/peer           — удалить пира (body: raw public key)
 *   GET  /wg/peers           — список пиров с трафиком и handshake
 *
 * Эндпоинты (Network Toolbox):
 *   GET  /net/scan           — ARP сканирование локальной сети (arp-scan / arp -an fallback)
 *   GET  /net/mdns           — Browse всех mDNS сервисов (avahi-browse)
 *   GET  /net/wifi           — сканирование WiFi сетей (iwlist wlan0 scan)
 *   GET  /net/ping/{host}    — Ping хоста (3 пакета, rtt stats)
 *   GET  /net/probe/{host}/{port} — HTTP probe сервиса (status, latency, headers)
 *   GET  /net/interfaces     — Список сетевых интерфейсов (ip addr + sysfs stats)
 *   GET  /net/ports          — Слушающие порты (ss -tlnp)
 *
 * Эндпоинты (System & Files):
 *   GET  /proc/stats          — Системная статистика (CPU, RAM, disk, temp, load, uptime)
 *   GET  /proc/list           — Список процессов (ps aux, отсортированные по CPU)
 *   GET  /fs/ls/{path}        — Список файлов в директории (path-restricted)
 *   GET  /fs/read/{path}      — Прочитать файл (path-restricted, max 1MB)
 *   POST /fs/write/{path}     — Записать файл (path-restricted, max 1MB)
 *
 * Эндпоинты (Firmware Self-Update):
 *   GET  /firmware/version   — версия прошивки, дата сборки, uptime
 *   GET  /firmware/source    — прочитать исходный код server.c (plain text, streaming)
 *   POST /firmware/source    — записать новый исходный код server.c
 *   POST /firmware/build     — скомпилировать из исходника новый бинарник
 *   GET  /firmware/build/logs — прочитать лог последней сборки
 *   POST /firmware/apply     — применить новый бинарник (с watchdog для безопасности)
 *
 * Эндпоинты (Firmware Vault):
 *   POST   /vault/store          — Сохранить прошивку в vault (body=binary, X-Node-Type, X-Version, X-Description)
 *   GET    /vault/list           — Список прошивок в vault (manifest)
 *   GET    /vault/{id}           — Скачать прошивку (binary, application/octet-stream)
 *   POST   /vault/push/{node}    — Распределить прошивку на ноду (curl forward)
 *   DELETE /vault/{id}           — Удалить прошивку из vault
 *
 * Эндпоинты (Fleet Management):
 *   GET  /fleet/status     — Агрегированный статус всех нод в mesh (health + version + rtt)
 *   POST /fleet/exec       — Broadcast HTTP запрос на все ноды (body: {path, method, body?, token?})
 *   POST /fetch            — Скачать файл по URL (body: {url, save_to})
 *
 * Эндпоинты (Agent Notes — межагентная коммуникация):
 *   POST   /notes            — Оставить заметку (body: {from, body, ttl_hours?})
 *   GET    /notes             — Прочитать все заметки (auto-expire, ?since=UNIX_TS)
 *   DELETE /notes/{id}        — Удалить заметку по ID
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <strings.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <limits.h>
#include <sys/statvfs.h>

/* I2C — linux-специфичные заголовки. На не-Linux (macOS cross-compile) — заглушки */
#ifdef __linux__
#include <linux/i2c-dev.h>
#ifndef I2C_SLAVE
#define I2C_SLAVE 0x0703
#endif
#else
#define I2C_SLAVE 0x0703
#endif

#define BUF_SIZE      65536
#define HTTP_BUF      8192
#define MAX_BODY      16384
#define DEFAULT_PORT  8080
#define SERIAL_DEV    "/dev/ttyGS0"
#define HID_DEV       "/dev/hidg0"
#define MAX_PORTS     16
#define KEY_DELAY_US  30000  /* 30ms между нажатиями */
#define TOKEN_FILE    "/etc/seed/token"
#define HOSTS_DIR     "/etc/seed/hosts"
#define SSH_KEY       "/root/.ssh/id_ed25519"
#define USB_SUBNET    "10.55.0."
/* BT_SUBNET removed — trust only loopback */
#define USB_IF        "usb0"
#define USB_OPERSTATE "/sys/class/net/" USB_IF "/operstate"
#define WG_KEY_LEN    44
#define WG_PUBKEY_FILE  "/etc/seed/wg/publickey"
#define WG_PRIVKEY_FILE "/etc/seed/wg/privatekey"
#define DNSMASQ_LEASES "/var/lib/misc/dnsmasq.leases"
#define MONITOR_INTERVAL 5  /* секунд между проверками */

/* Seed — GPIO, I2C, конфиг, деплой */
#define SEED_DIR        "/etc/seed"
#define SEED_CONFIG     "/etc/seed/config.md"
#define SEED_SCRIPTS    "/etc/seed/scripts"
#define SEED_DEPLOY_SH  "/etc/seed/scripts/deploy.sh"
#define SEED_SERVICE    "seed-deploy"
#define SEED_UNIT_FILE  "/etc/systemd/system/seed-deploy.service"
#define SEED_LIVE_MD    "/etc/seed/live.md"
#define SEED_DEPLOY_LOG "/etc/seed/scripts/deploy.log"
#define I2C_BUS_DEV         "/dev/i2c-1"
#define GPIO_SYSFS          "/sys/class/gpio"
#define MAX_GPIO_PINS       32

/* Firmware Self-Update — пути и версия */
#define FIRMWARE_VERSION    "1.0.0"
#define FIRMWARE_SOURCE     "/opt/seed/server.c"
#define FIRMWARE_BINARY     "/opt/seed/seed"
#define FIRMWARE_NEW        "/opt/seed/seed-new"
#define FIRMWARE_BACKUP     "/opt/seed/seed.bak"
#define FIRMWARE_BUILD_LOG  "/opt/seed/build.log"

/* Firmware Vault — хранение и распределение прошивок */
#define VAULT_DIR           "/var/seed/vault"
#define VAULT_MANIFEST      "/var/seed/vault/manifest.json"
#define VAULT_MAX_FW_SIZE   (2 * 1024 * 1024)  /* 2MB max firmware */

/* Agent Notes — межагентная коммуникация (bulletin board) */
#define NOTES_FILE          "/etc/seed/notes.json"
#define NOTES_MAX_BODY      4096
#define NOTES_MAX_FROM      128
#define NOTES_MAX_TTL       8760   /* 1 год в часах */
#define NOTES_DEFAULT_TTL   168    /* 1 неделя в часах */

/* ========== Events Ring Buffer ========== */
#define MAX_EVENTS          256
#define EVENT_MSG_LEN       128

typedef struct {
    time_t timestamp;
    char message[EVENT_MSG_LEN];
} event_entry_t;

static event_entry_t events_buf[MAX_EVENTS];
static int events_head = 0;   /* next write position */
static int events_count = 0;  /* total entries (max MAX_EVENTS) */
static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;

/* Добавить событие в кольцевой буфер (thread-safe) */
static void event_add(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&events_lock);
    event_entry_t *e = &events_buf[events_head];
    e->timestamp = time(NULL);
    vsnprintf(e->message, EVENT_MSG_LEN, fmt, ap);
    events_head = (events_head + 1) % MAX_EVENTS;
    if (events_count < MAX_EVENTS) events_count++;
    pthread_mutex_unlock(&events_lock);
    va_end(ap);
}
static int gpio_base_offset = 0;  /* gpiochip base: 0 на старых Pi, 512 на новых */

/* Безопасное ограничение pos после snprintf — предотвращает UB при переполнении буфера.
 * snprintf возвращает сколько ХОТЕЛ записать, pos может превысить max_len. */
#define CLAMP_POS(pos, max_len) do { if ((pos) >= (max_len)) (pos) = (max_len) - 1; } while(0)

/* BCM2835 GPIO пины, доступные на Pi Zero W header */
static const int gpio_valid_pins[] = {
    2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27
};
#define NUM_VALID_PINS (sizeof(gpio_valid_pins)/sizeof(gpio_valid_pins[0]))

/* ========== HID Keyboard Maps ========== */

/* USB HID Keycodes (USB HID Usage Tables, Section 10) */
typedef struct { const char *name; unsigned char keycode; } keymap_entry_t;

static const keymap_entry_t keycode_map[] = {
    {"a", 0x04}, {"b", 0x05}, {"c", 0x06}, {"d", 0x07}, {"e", 0x08},
    {"f", 0x09}, {"g", 0x0A}, {"h", 0x0B}, {"i", 0x0C}, {"j", 0x0D},
    {"k", 0x0E}, {"l", 0x0F}, {"m", 0x10}, {"n", 0x11}, {"o", 0x12},
    {"p", 0x13}, {"q", 0x14}, {"r", 0x15}, {"s", 0x16}, {"t", 0x17},
    {"u", 0x18}, {"v", 0x19}, {"w", 0x1A}, {"x", 0x1B}, {"y", 0x1C},
    {"z", 0x1D},
    {"1", 0x1E}, {"2", 0x1F}, {"3", 0x20}, {"4", 0x21}, {"5", 0x22},
    {"6", 0x23}, {"7", 0x24}, {"8", 0x25}, {"9", 0x26}, {"0", 0x27},
    {"enter", 0x28}, {"escape", 0x29}, {"backspace", 0x2A},
    {"tab", 0x2B}, {"space", 0x2C},
    {"-", 0x2D}, {"=", 0x2E}, {"[", 0x2F}, {"]", 0x30},
    {"\\", 0x31}, {";", 0x33}, {"'", 0x34}, {"`", 0x35},
    {",", 0x36}, {".", 0x37}, {"/", 0x38},
    {"capslock", 0x39},
    {"f1", 0x3A}, {"f2", 0x3B}, {"f3", 0x3C}, {"f4", 0x3D},
    {"f5", 0x3E}, {"f6", 0x3F}, {"f7", 0x40}, {"f8", 0x41},
    {"f9", 0x42}, {"f10", 0x43}, {"f11", 0x44}, {"f12", 0x45},
    {"printscreen", 0x46}, {"scrolllock", 0x47}, {"pause", 0x48},
    {"insert", 0x49}, {"home", 0x4A}, {"pageup", 0x4B},
    {"delete", 0x4C}, {"end", 0x4D}, {"pagedown", 0x4E},
    {"right", 0x4F}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52},
    {NULL, 0}
};

/* Символы, требующие Shift → базовый символ (US layout) */
typedef struct { char shifted; char base; } shift_entry_t;

static const shift_entry_t shift_map[] = {
    {'!', '1'}, {'@', '2'}, {'#', '3'}, {'$', '4'}, {'%', '5'},
    {'^', '6'}, {'&', '7'}, {'*', '8'}, {'(', '9'}, {')', '0'},
    {'_', '-'}, {'+', '='}, {'{', '['}, {'}', ']'}, {'|', '\\'},
    {':', ';'}, {'"', '\''}, {'~', '`'}, {'<', ','}, {'>', '.'},
    {'?', '/'},
    {0, 0}
};

/* Модификаторы — битовые маски */
typedef struct { const char *name; unsigned char mask; } modifier_entry_t;

static const modifier_entry_t modifier_map[] = {
    {"ctrl", 0x01}, {"lctrl", 0x01}, {"rctrl", 0x10},
    {"shift", 0x02}, {"lshift", 0x02}, {"rshift", 0x20},
    {"alt", 0x04}, {"lalt", 0x04}, {"ralt", 0x40},
    {"super", 0x08}, {"lsuper", 0x08}, {"rsuper", 0x80},
    {"win", 0x08}, {"meta", 0x08}, {"cmd", 0x08},
    {NULL, 0}
};

/* Кольцевой буфер для serial вывода */
static char serial_buf[BUF_SIZE];
static int  serial_len = 0;
static pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;

/* Serial port */
static int serial_fd = -1;
static char serial_device[256] = SERIAL_DEV;
static int serial_baudrate = 115200;

/* Известные baud rates */
typedef struct { int rate; speed_t constant; } baud_entry_t;
static const baud_entry_t baud_table[] = {
    {300,    B300},    {1200,   B1200},   {2400,   B2400},
    {4800,   B4800},   {9600,   B9600},   {19200,  B19200},
    {38400,  B38400},  {57600,  B57600},  {115200, B115200},
    {230400, B230400},
#ifdef B460800
    {460800, B460800},
#endif
#ifdef B921600
    {921600, B921600},
#endif
    {0, 0}
};

speed_t baud_to_constant(int rate) {
    for (int i = 0; baud_table[i].rate; i++)
        if (baud_table[i].rate == rate) return baud_table[i].constant;
    return B115200;
}

/* ========== Serial ========== */

int serial_open(void) {
    struct termios tty;

    if (serial_fd >= 0) {
        close(serial_fd);
        serial_fd = -1;
    }

    serial_fd = open(serial_device, O_RDWR | O_NOCTTY);
    if (serial_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", serial_device, strerror(errno));
        return -1;
    }

    if (tcgetattr(serial_fd, &tty) != 0) {
        fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
        close(serial_fd);
        serial_fd = -1;
        return -1;
    }

    speed_t speed = baud_to_constant(serial_baudrate);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;

    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        close(serial_fd);
        serial_fd = -1;
        return -1;
    }

    fprintf(stderr, "Serial %s opened at %d baud\n", serial_device, serial_baudrate);
    return 0;
}

/* Переключить serial port на лету */
int serial_switch(const char *device, int baudrate) {
    pthread_mutex_lock(&serial_lock);

    if (serial_fd >= 0) {
        close(serial_fd);
        serial_fd = -1;
    }
    serial_len = 0;

    strncpy(serial_device, device, sizeof(serial_device) - 1);
    serial_device[sizeof(serial_device) - 1] = '\0';
    if (baudrate > 0) serial_baudrate = baudrate;

    pthread_mutex_unlock(&serial_lock);

    return serial_open();
}

/* Сканировать доступные serial порты */
int serial_list_ports(char *out, int max_len) {
    int pos = 0;
    pos += snprintf(out + pos, max_len - pos, "[");

    const char *dirs[] = {"/dev", NULL};
    const char *prefixes[] = {"ttyGS", "ttyACM", "ttyUSB", "ttyAMA", "ttyS", NULL};

    int first = 1;
    for (int d = 0; dirs[d]; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *ep;
        while ((ep = readdir(dp)) != NULL) {
            for (int p = 0; prefixes[p]; p++) {
                if (strncmp(ep->d_name, prefixes[p], strlen(prefixes[p])) == 0) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/%s", dirs[d], ep->d_name);

                    /* Проверяем что это char device */
                    struct stat st;
                    if (stat(path, &st) == 0 && S_ISCHR(st.st_mode)) {
                        const char *type = "unknown";
                        if (strncmp(ep->d_name, "ttyGS", 5) == 0) type = "usb-gadget";
                        else if (strncmp(ep->d_name, "ttyACM", 6) == 0) type = "usb-acm";
                        else if (strncmp(ep->d_name, "ttyUSB", 6) == 0) type = "usb-serial";
                        else if (strncmp(ep->d_name, "ttyAMA", 6) == 0) type = "gpio-uart";
                        else if (strncmp(ep->d_name, "ttyS", 4) == 0) type = "hardware-uart";

                        int active = (strcmp(path, serial_device) == 0 && serial_fd >= 0);

                        if (!first) pos += snprintf(out + pos, max_len - pos, ",");
                        pos += snprintf(out + pos, max_len - pos,
                            "{\"device\":\"%s\",\"type\":\"%s\",\"active\":%s}",
                            path, type, active ? "true" : "false");
                        first = 0;
                    }
                }
            }
        }
        closedir(dp);
    }

    pos += snprintf(out + pos, max_len - pos, "]");
    return pos;
}

/* Фоновый поток — читает serial и кладёт в буфер */
void *serial_reader(void *arg) {
    (void)arg;
    char tmp[1024];
    while (1) {
        if (serial_fd < 0) {
            sleep(1);
            serial_open();
            continue;
        }
        int n = read(serial_fd, tmp, sizeof(tmp));
        if (n > 0) {
            pthread_mutex_lock(&serial_lock);
            if (serial_len + n > BUF_SIZE) {
                int shift = serial_len + n - BUF_SIZE;
                memmove(serial_buf, serial_buf + shift, serial_len - shift);
                serial_len -= shift;
            }
            memcpy(serial_buf + serial_len, tmp, n);
            serial_len += n;
            pthread_mutex_unlock(&serial_lock);
        } else if (n < 0 && errno != EAGAIN) {
            fprintf(stderr, "Serial read error: %s\n", strerror(errno));
            close(serial_fd);
            serial_fd = -1;
        }
    }
    return NULL;
}

int serial_flush(char *out, int max_len) {
    pthread_mutex_lock(&serial_lock);
    int len = serial_len < max_len ? serial_len : max_len - 1;
    memcpy(out, serial_buf, len);
    out[len] = '\0';
    serial_len = 0;
    pthread_mutex_unlock(&serial_lock);
    return len;
}

int serial_exec(const char *cmd, char *out, int max_len, int timeout_ms) {
    if (serial_fd < 0) {
        snprintf(out, max_len, "{\"error\": \"serial not available\"}");
        return strlen(out);
    }

    char marker[64];
    snprintf(marker, sizeof(marker), "__DONE_%ld__", (long)time(NULL));

    pthread_mutex_lock(&serial_lock);
    serial_len = 0;
    pthread_mutex_unlock(&serial_lock);

    char full_cmd[MAX_BODY + 128];
    snprintf(full_cmd, sizeof(full_cmd), "%s; echo %s\n", cmd, marker);
    write(serial_fd, full_cmd, strlen(full_cmd));

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        usleep(50000);
        elapsed += 50;

        pthread_mutex_lock(&serial_lock);
        serial_buf[serial_len] = '\0';
        char *found = strstr(serial_buf, marker);
        if (found) {
            int out_len = found - serial_buf;
            char *start = serial_buf;
            char *nl = memchr(serial_buf, '\n', out_len);
            if (nl) start = nl + 1;

            int result_len = found - start;
            if (result_len < 0) result_len = 0;
            if (result_len >= max_len) result_len = max_len - 1;
            memcpy(out, start, result_len);
            out[result_len] = '\0';

            char *marker_line = out;
            while ((marker_line = strstr(marker_line, marker)) != NULL) {
                char *line_start = marker_line;
                while (line_start > out && *(line_start - 1) != '\n') line_start--;
                char *line_end = marker_line + strlen(marker);
                if (*line_end == '\n') line_end++;
                memmove(line_start, line_end, strlen(line_end) + 1);
                marker_line = line_start;
            }

            serial_len = 0;
            pthread_mutex_unlock(&serial_lock);

            result_len = strlen(out);
            while (result_len > 0 && (out[result_len-1] == '\n' || out[result_len-1] == '\r' || out[result_len-1] == ' '))
                out[--result_len] = '\0';

            return result_len;
        }
        pthread_mutex_unlock(&serial_lock);
    }

    int len = serial_flush(out, max_len);
    return len;
}

/* ========== HID Keyboard ========== */

static unsigned char hid_keycode_for_char(char c) {
    /* Строчная буква */
    char lower[2] = {c, 0};
    if (c >= 'a' && c <= 'z') lower[0] = c;
    else if (c >= 'A' && c <= 'Z') lower[0] = c + 32;
    else lower[0] = c;

    for (int i = 0; keycode_map[i].name; i++) {
        if (strlen(keycode_map[i].name) == 1 && keycode_map[i].name[0] == lower[0])
            return keycode_map[i].keycode;
    }
    return 0;
}

static unsigned char hid_keycode_for_name(const char *name) {
    for (int i = 0; keycode_map[i].name; i++) {
        if (strcasecmp(keycode_map[i].name, name) == 0)
            return keycode_map[i].keycode;
    }
    return 0;
}

static unsigned char hid_modifier_for_name(const char *name) {
    for (int i = 0; modifier_map[i].name; i++) {
        if (strcasecmp(modifier_map[i].name, name) == 0)
            return modifier_map[i].mask;
    }
    return 0;
}

static char hid_shift_base(char c) {
    for (int i = 0; shift_map[i].shifted; i++) {
        if (shift_map[i].shifted == c)
            return shift_map[i].base;
    }
    return 0;
}

/* Отправить HID report в уже открытый fd */
static int hid_write_report(int fd, unsigned char modifier, unsigned char keycode) {
    unsigned char report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    unsigned char release[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    if (write(fd, report, 8) != 8) return -1;
    usleep(KEY_DELAY_US);
    if (write(fd, release, 8) != 8) return -1;
    usleep(KEY_DELAY_US);
    return 0;
}

/* Обёртка: открывает, пишет, закрывает — для одиночных нажатий */
static int hid_send_report(unsigned char modifier, unsigned char keycode) {
    int fd = open(HID_DEV, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", HID_DEV, strerror(errno));
        return -1;
    }
    int rc = hid_write_report(fd, modifier, keycode);
    close(fd);
    return rc;
}

static int hid_send_combo(unsigned char modifier, unsigned char *keycodes, int nkeys) {
    /* Для комбинаций: нажать все клавиши одновременно */
    unsigned char report[8] = {modifier, 0, 0, 0, 0, 0, 0, 0};
    unsigned char release[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    for (int i = 0; i < nkeys && i < 6; i++)
        report[2 + i] = keycodes[i];

    int fd = open(HID_DEV, O_WRONLY);
    if (fd < 0) return -1;

    int ok = 0;
    if (write(fd, report, 8) == 8) {
        usleep(50000); /* 50ms для комбинаций */
        if (write(fd, release, 8) == 8) ok = 1;
    }

    close(fd);
    return ok ? 0 : -1;
}

static int hid_type_text(const char *text, int len) {
    /* Открываем HID один раз на весь текст — иначе хост теряет modifier */
    int fd = open(HID_DEV, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", HID_DEV, strerror(errno));
        return 0;
    }

    int typed = 0;
    for (int i = 0; i < len; i++) {
        char c = text[i];
        unsigned char modifier = 0;
        unsigned char keycode = 0;

        if (c == '\n') {
            keycode = 0x28; /* enter */
        } else if (c == '\t') {
            keycode = 0x2B; /* tab */
        } else if (c == ' ') {
            keycode = 0x2C; /* space */
        } else if (c >= 'A' && c <= 'Z') {
            modifier = 0x02; /* shift */
            keycode = hid_keycode_for_char(c);
        } else {
            char base = hid_shift_base(c);
            if (base) {
                modifier = 0x02;
                keycode = hid_keycode_for_char(base);
            } else {
                keycode = hid_keycode_for_char(c);
            }
        }

        if (keycode) {
            if (hid_write_report(fd, modifier, keycode) == 0)
                typed++;
            else
                break;
        }
    }

    close(fd);
    return typed;
}

/* Парсит "ctrl+c", "alt+tab", "enter" и отправляет комбинацию */
static int hid_send_keys(const char *keys_str, char *result, int max_len) {
    char buf[256];
    strncpy(buf, keys_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Trim whitespace */
    int slen = strlen(buf);
    while (slen > 0 && (buf[slen-1] == '\n' || buf[slen-1] == '\r' || buf[slen-1] == ' '))
        buf[--slen] = '\0';

    unsigned char modifier = 0;
    unsigned char keycodes[6];
    int nkeys = 0;

    /* Разбираем по + */
    char *saveptr;
    char *token = strtok_r(buf, "+", &saveptr);
    while (token) {
        /* Trim пробелы */
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        unsigned char mod = hid_modifier_for_name(token);
        if (mod) {
            modifier |= mod;
        } else {
            unsigned char kc = hid_keycode_for_name(token);
            if (kc) {
                if (nkeys < 6) keycodes[nkeys++] = kc;
            } else if (strlen(token) == 1) {
                /* Одиночный символ */
                unsigned char kc2 = hid_keycode_for_char(token[0]);
                if (kc2) {
                    if (nkeys < 6) keycodes[nkeys++] = kc2;
                } else {
                    snprintf(result, max_len, "{\"error\":\"unknown key: %s\"}", token);
                    return -1;
                }
            } else {
                snprintf(result, max_len, "{\"error\":\"unknown key: %s\"}", token);
                return -1;
            }
        }
        token = strtok_r(NULL, "+", &saveptr);
    }

    if (hid_send_combo(modifier, keycodes, nkeys) == 0) {
        snprintf(result, max_len, "{\"ok\":true,\"keys\":\"%s\"}", keys_str);
        return 0;
    } else {
        snprintf(result, max_len, "{\"error\":\"failed to send HID report\"}");
        return -1;
    }
}

static int hid_is_available(void) {
    return access(HID_DEV, W_OK) == 0;
}

/* ========== Auth Token ========== */

static char api_token[65] = "";

static void token_load(void) {
    FILE *fp = fopen(TOKEN_FILE, "r");
    if (fp) {
        if (fgets(api_token, sizeof(api_token), fp)) {
            int l = strlen(api_token);
            while (l > 0 && (api_token[l-1] == '\n' || api_token[l-1] == '\r')) api_token[--l] = '\0';
        }
        fclose(fp);
        size_t tlen = strlen(api_token);
        if (tlen >= 8)
            fprintf(stderr, "Token loaded from %s (%.4s...%s)\n", TOKEN_FILE, api_token, api_token + tlen - 4);
        else
            fprintf(stderr, "Token loaded from %s (***)\n", TOKEN_FILE);
        return;
    }
    /* Генерируем новый токен */
    fp = fopen("/dev/urandom", "r");
    if (fp) {
        unsigned char raw[16];
        fread(raw, 1, 16, fp);
        fclose(fp);
        for (int i = 0; i < 16; i++)
            sprintf(api_token + i*2, "%02x", raw[i]);
        api_token[32] = '\0';
    }
    mkdir("/etc/seed", 0700);
    fp = fopen(TOKEN_FILE, "w");
    if (fp) {
        fprintf(fp, "%s\n", api_token);
        fclose(fp);
        chmod(TOKEN_FILE, 0600);
    }
    {
        size_t tlen = strlen(api_token);
        if (tlen >= 8)
            fprintf(stderr, "Token generated: %.4s...%s\n", api_token, api_token + tlen - 4);
        else
            fprintf(stderr, "Token generated: (***)\n");
    }
}

/* Проверяем, доверенный ли источник (USB/BT subnet) */
static int is_trusted_ip(const char *ip) {
    return strcmp(ip, "127.0.0.1") == 0;
}

/* ========== SSH Host Exec ========== */

static char host_user[64] = "";
static char host_ip[64] = "10.55.0.2";
static int host_paired = 0;

/* Состояние USB и хоста (обновляется фоновым потоком host_monitor) */
static volatile int usb_up = 0;       /* 1 = usb0 interface up */
static volatile int host_connected = 0; /* 1 = SSH до хоста работает */
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t server_start_time = 0;

static void host_load_profile(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/current.json", HOSTS_DIR);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char buf[1024];
    if (fgets(buf, sizeof(buf), fp)) {
        /* Простой парсинг "user":"xxx", "ip":"xxx" */
        char *u = strstr(buf, "\"user\":\"");
        if (u) {
            u += 8;
            char *end = strchr(u, '"');
            if (end) {
                int len = end - u;
                if (len < (int)sizeof(host_user)) {
                    memcpy(host_user, u, len);
                    host_user[len] = '\0';
                    host_paired = 1;
                }
            }
        }
        char *ip = strstr(buf, "\"ip\":\"");
        if (ip) {
            ip += 6;
            char *end = strchr(ip, '"');
            if (end) {
                int len = end - ip;
                if (len < (int)sizeof(host_ip)) {
                    memcpy(host_ip, ip, len);
                    host_ip[len] = '\0';
                }
            }
        }
    }
    fclose(fp);
    if (host_paired)
        fprintf(stderr, "Host profile loaded: user=%s\n", host_user);
}

static int host_exec(const char *cmd, char *out, int max_len) {
    if (!host_paired || host_user[0] == '\0') {
        snprintf(out, max_len, "not paired");
        return -1;
    }
    char ssh_cmd[4096 + 256];
    snprintf(ssh_cmd, sizeof(ssh_cmd),
        "ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "
        "-o BatchMode=yes -i %s %s@%s '%s' 2>&1",
        SSH_KEY, host_user, host_ip, cmd);
    FILE *fp = popen(ssh_cmd, "r");
    if (!fp) {
        snprintf(out, max_len, "popen failed");
        return -1;
    }
    int total = 0;
    while (total < max_len - 1) {
        int n = fread(out + total, 1, max_len - 1 - total, fp);
        if (n <= 0) break;
        total += n;
    }
    out[total] = '\0';
    int status = pclose(fp);
    return WEXITSTATUS(status);
}

/* ========== Host Monitor ========== */

/* Проверить состояние usb0 интерфейса */
static int check_usb_state(void) {
    FILE *fp = fopen(USB_OPERSTATE, "r");
    if (!fp) return 0;
    char state[32] = "";
    if (fgets(state, sizeof(state), fp)) {
        int l = strlen(state);
        while (l > 0 && (state[l-1] == '\n' || state[l-1] == '\r')) state[--l] = '\0';
    }
    fclose(fp);
    return (strcmp(state, "up") == 0);
}

/* Прочитать IP хоста из lease-файла dnsmasq */
static int read_dnsmasq_lease(char *ip_out, int max_len) {
    FILE *fp = fopen(DNSMASQ_LEASES, "r");
    if (!fp) return 0;

    char line[512];
    char best_ip[64] = "";
    time_t best_ts = 0;

    /* Формат: timestamp mac ip hostname client_id */
    while (fgets(line, sizeof(line), fp)) {
        long ts_long;
        char mac[32], ip[64], hostname[128], client_id[128];
        if (sscanf(line, "%ld %31s %63s %127s %127s", &ts_long, mac, ip, hostname, client_id) >= 3) {
            time_t ts = (time_t)ts_long;
            /* Берём только адреса из USB-подсети */
            if (strncmp(ip, USB_SUBNET, strlen(USB_SUBNET)) == 0) {
                if (ts > best_ts) {
                    best_ts = ts;
                    strncpy(best_ip, ip, sizeof(best_ip) - 1);
                    best_ip[sizeof(best_ip) - 1] = '\0';
                }
            }
        }
    }
    fclose(fp);

    if (best_ip[0]) {
        strncpy(ip_out, best_ip, max_len - 1);
        ip_out[max_len - 1] = '\0';
        return 1;
    }
    return 0;
}

/* Быстрая проверка SSH-связности (ssh ... 'true', hard timeout 5 сек) */
static int check_ssh_connectivity(void) {
    if (!host_paired || host_user[0] == '\0') return 0;

    /* Копируем host_ip/host_user под локом — thread safety */
    char ip_copy[64], user_copy[64];
    pthread_mutex_lock(&host_lock);
    strncpy(ip_copy, host_ip, sizeof(ip_copy) - 1);
    ip_copy[sizeof(ip_copy) - 1] = '\0';
    strncpy(user_copy, host_user, sizeof(user_copy) - 1);
    user_copy[sizeof(user_copy) - 1] = '\0';
    pthread_mutex_unlock(&host_lock);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "timeout 5 ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 "
        "-o BatchMode=yes -i %s %s@%s 'true' >/dev/null 2>&1",
        SSH_KEY, user_copy, ip_copy);
    int rc = system(cmd);
    return (rc == 0);
}

/* Фоновый поток — мониторит USB-подключение и SSH-связность.
 * USB network recovery делает usb_watchdog.sh — сервер только читает состояние. */
void *host_monitor(void *arg) {
    (void)arg;
    int prev_usb = -1;
    int prev_connected = -1;

    while (1) {
        sleep(MONITOR_INTERVAL);

        /* 1. Проверяем usb0 */
        int usb_now = check_usb_state();

        if (usb_now != prev_usb) {
            fprintf(stderr, "[monitor] usb0: %s -> %s\n",
                prev_usb < 0 ? "unknown" : (prev_usb ? "up" : "down"),
                usb_now ? "up" : "down");
            if (prev_usb >= 0) /* не логируем начальное состояние */
                event_add("usb0 %s", usb_now ? "up" : "down");
            prev_usb = usb_now;
        }
        usb_up = usb_now;

        /* 2. Читаем IP хоста из dnsmasq lease */
        if (usb_now) {
            char lease_ip[64] = "";
            if (read_dnsmasq_lease(lease_ip, sizeof(lease_ip))) {
                pthread_mutex_lock(&host_lock);
                if (strcmp(lease_ip, host_ip) != 0) {
                    fprintf(stderr, "[monitor] host IP changed: %s -> %s\n", host_ip, lease_ip);
                    strncpy(host_ip, lease_ip, sizeof(host_ip) - 1);
                    host_ip[sizeof(host_ip) - 1] = '\0';
                }
                pthread_mutex_unlock(&host_lock);
            }
        }

        /* 3. Проверяем SSH-связность (только если USB up и есть паринг) */
        int connected_now = 0;
        if (usb_now && host_paired) {
            connected_now = check_ssh_connectivity();
        }

        if (connected_now != prev_connected) {
            fprintf(stderr, "[monitor] host: %s -> %s\n",
                prev_connected < 0 ? "unknown" : (prev_connected ? "connected" : "disconnected"),
                connected_now ? "connected" : "disconnected");
            if (prev_connected >= 0) { /* не логируем начальное состояние */
                if (connected_now) {
                    char ip_copy[64];
                    pthread_mutex_lock(&host_lock);
                    strncpy(ip_copy, host_ip, sizeof(ip_copy) - 1);
                    ip_copy[sizeof(ip_copy) - 1] = '\0';
                    pthread_mutex_unlock(&host_lock);
                    event_add("host connected: %s", ip_copy);
                } else {
                    event_add("host disconnected");
                }
            }
            prev_connected = connected_now;
        }
        host_connected = connected_now;
    }
    return NULL;
}

/* ========== Seed GPIO ========== */

/* Трекинг сконфигурированных пинов */
typedef struct {
    int pin;
    int configured; /* 1 = экспортирован через sysfs */
} gpio_pin_t;

static gpio_pin_t gpio_pins[MAX_GPIO_PINS];
static int gpio_pin_count = 0;
static pthread_mutex_t gpio_lock = PTHREAD_MUTEX_INITIALIZER;

/* Проверить, валидный ли BCM пин */
static int gpio_is_valid_pin(int pin) {
    for (int i = 0; i < (int)NUM_VALID_PINS; i++)
        if (gpio_valid_pins[i] == pin) return 1;
    return 0;
}

/* Найти пин в трекере, вернуть индекс или -1.
 * single-threaded: no lock needed — вызывается из accept loop (handle_request)
 * или из gpio_track_pin, который уже держит gpio_lock. */
static int gpio_find_pin(int pin) {
    for (int i = 0; i < gpio_pin_count; i++)
        if (gpio_pins[i].pin == pin && gpio_pins[i].configured) return i;
    return -1;
}

/* Определить base offset для gpiochip (512 на новых Pi, 0 на старых) */
static void gpio_detect_base(void) {
    DIR *dp = opendir(GPIO_SYSFS);
    if (!dp) return;
    struct dirent *ep;
    while ((ep = readdir(dp)) != NULL) {
        if (strncmp(ep->d_name, "gpiochip", 8) == 0) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s/base", GPIO_SYSFS, ep->d_name);
            FILE *fp = fopen(path, "r");
            if (fp) {
                int base = 0;
                if (fscanf(fp, "%d", &base) == 1 && base > 0) {
                    gpio_base_offset = base;
                    fprintf(stderr, "GPIO base offset: %d (chip %s)\n", base, ep->d_name);
                }
                fclose(fp);
                break;  /* берём первый чип с ненулевым base */
            }
        }
    }
    closedir(dp);
}

/* Экспортировать пин через sysfs (если ещё не экспортирован) */
static int gpio_export(int pin) {
    int sysfs_pin = pin + gpio_base_offset;
    char path[128];
    snprintf(path, sizeof(path), "%s/gpio%d", GPIO_SYSFS, sysfs_pin);
    struct stat st;
    if (stat(path, &st) == 0) return 0; /* уже экспортирован */

    snprintf(path, sizeof(path), "%s/export", GPIO_SYSFS);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%d", sysfs_pin);
    fclose(fp);

    /* Даём sysfs время на создание файлов */
    usleep(100000);
    return 0;
}

/* Задать направление пина: "in" или "out" */
static int gpio_set_direction(int pin, const char *dir) {
    int sysfs_pin = pin + gpio_base_offset;
    char path[128];
    snprintf(path, sizeof(path), "%s/gpio%d/direction", GPIO_SYSFS, sysfs_pin);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s", dir);
    fclose(fp);
    return 0;
}

/* Записать значение пина: 0 или 1 */
static int gpio_write_value(int pin, int value) {
    int sysfs_pin = pin + gpio_base_offset;
    char path[128];
    snprintf(path, sizeof(path), "%s/gpio%d/value", GPIO_SYSFS, sysfs_pin);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%d", value ? 1 : 0);
    fclose(fp);
    return 0;
}

/* Прочитать значение пина, вернуть 0/1 или -1 при ошибке */
static int gpio_read_value(int pin) {
    int sysfs_pin = pin + gpio_base_offset;
    char path[128];
    snprintf(path, sizeof(path), "%s/gpio%d/value", GPIO_SYSFS, sysfs_pin);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int val = -1;
    fscanf(fp, "%d", &val);
    fclose(fp);
    return val;
}

/* Прочитать направление пина: "in"/"out" */
static int gpio_read_direction(int pin, char *dir, int max_len) {
    int sysfs_pin = pin + gpio_base_offset;
    char path[128];
    snprintf(path, sizeof(path), "%s/gpio%d/direction", GPIO_SYSFS, sysfs_pin);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        strncpy(dir, "unknown", max_len - 1);
        dir[max_len - 1] = '\0';
        return -1;
    }
    if (fgets(dir, max_len, fp)) {
        int l = strlen(dir);
        while (l > 0 && (dir[l-1] == '\n' || dir[l-1] == '\r')) dir[--l] = '\0';
    }
    fclose(fp);
    return 0;
}

/* Добавить пин в трекер */
static void gpio_track_pin(int pin) {
    pthread_mutex_lock(&gpio_lock);
    if (gpio_find_pin(pin) < 0 && gpio_pin_count < MAX_GPIO_PINS) {
        gpio_pins[gpio_pin_count].pin = pin;
        gpio_pins[gpio_pin_count].configured = 1;
        gpio_pin_count++;
    }
    pthread_mutex_unlock(&gpio_lock);
}

/* Сканировать sysfs при старте — найти уже экспортированные пины */
static void gpio_scan_configured(void) {
    for (int i = 0; i < (int)NUM_VALID_PINS; i++) {
        int pin = gpio_valid_pins[i];
        char path[128];
        int sysfs_pin = pin + gpio_base_offset;
        snprintf(path, sizeof(path), "%s/gpio%d", GPIO_SYSFS, sysfs_pin);
        struct stat st;
        if (stat(path, &st) == 0) {
            gpio_track_pin(pin);
        }
    }
    if (gpio_pin_count > 0)
        fprintf(stderr, "GPIO: %d pins already configured\n", gpio_pin_count);
}

/* ========== Seed I2C ========== */

/* Сканировать I2C шину, вернуть JSON массив адресов */
static int i2c_scan(char *out, int max_len) {
    int pos = 0;
    pos += snprintf(out + pos, max_len - pos, "{\"devices\":[");
    CLAMP_POS(pos, max_len);

    int fd = open(I2C_BUS_DEV, O_RDWR);
    if (fd < 0) {
        pos += snprintf(out + pos, max_len - pos, "],\"error\":\"cannot open %s: %s\"}", I2C_BUS_DEV, strerror(errno));
        CLAMP_POS(pos, max_len);
        return pos;
    }

    int first = 1;
    for (int addr = 0x03; addr <= 0x77; addr++) {
        if (ioctl(fd, I2C_SLAVE, addr) < 0) continue;
        /* Попытка прочитать 1 байт — если ACK, устройство есть */
        unsigned char byte;
        if (read(fd, &byte, 1) >= 0) {
            if (!first) { pos += snprintf(out + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }
            pos += snprintf(out + pos, max_len - pos, "\"0x%02X\"", addr);
            CLAMP_POS(pos, max_len);
            first = 0;
        }
    }

    close(fd);
    pos += snprintf(out + pos, max_len - pos, "]}");
    CLAMP_POS(pos, max_len);
    return pos;
}

/* Записать байты на I2C устройство. data — hex строка "0A FF 01" */
static int i2c_write_bytes(int addr, const char *hex_data, char *out, int max_len) {
    unsigned char bytes[128];
    int nbytes = 0;

    /* Парсим hex строку */
    const char *p = hex_data;
    while (*p && nbytes < (int)sizeof(bytes)) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        unsigned int val;
        if (sscanf(p, "%2x", &val) != 1) break;
        bytes[nbytes++] = (unsigned char)val;
        p += 2;
    }

    if (nbytes == 0) {
        snprintf(out, max_len, "{\"error\":\"no valid hex bytes\"}");
        return -1;
    }

    int fd = open(I2C_BUS_DEV, O_RDWR);
    if (fd < 0) {
        snprintf(out, max_len, "{\"error\":\"cannot open %s: %s\"}", I2C_BUS_DEV, strerror(errno));
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        close(fd);
        snprintf(out, max_len, "{\"error\":\"ioctl I2C_SLAVE 0x%02X: %s\"}", addr, strerror(errno));
        return -1;
    }

    int written = write(fd, bytes, nbytes);
    close(fd);

    if (written < 0) {
        snprintf(out, max_len, "{\"error\":\"write failed: %s\"}", strerror(errno));
        return -1;
    }

    snprintf(out, max_len, "{\"ok\":true,\"addr\":\"0x%02X\",\"bytes_written\":%d}", addr, written);
    return 0;
}

/* Прочитать N байт с I2C устройства, вернуть hex строку */
static int i2c_read_bytes(int addr, int count, char *out, int max_len) {
    if (count <= 0 || count > 256) {
        snprintf(out, max_len, "{\"error\":\"count must be 1-256\"}");
        return -1;
    }

    int fd = open(I2C_BUS_DEV, O_RDWR);
    if (fd < 0) {
        snprintf(out, max_len, "{\"error\":\"cannot open %s: %s\"}", I2C_BUS_DEV, strerror(errno));
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        close(fd);
        snprintf(out, max_len, "{\"error\":\"ioctl I2C_SLAVE 0x%02X: %s\"}", addr, strerror(errno));
        return -1;
    }

    unsigned char bytes[256];
    int nread = read(fd, bytes, count);
    close(fd);

    if (nread < 0) {
        snprintf(out, max_len, "{\"error\":\"read failed: %s\"}", strerror(errno));
        return -1;
    }

    int pos = 0;
    pos += snprintf(out + pos, max_len - pos, "{\"addr\":\"0x%02X\",\"count\":%d,\"data\":\"", addr, nread);
    CLAMP_POS(pos, max_len);
    for (int i = 0; i < nread && pos < max_len - 4; i++) {
        if (i > 0) { pos += snprintf(out + pos, max_len - pos, " "); CLAMP_POS(pos, max_len); }
        pos += snprintf(out + pos, max_len - pos, "%02X", bytes[i]);
        CLAMP_POS(pos, max_len);
    }
    pos += snprintf(out + pos, max_len - pos, "\"}");
    CLAMP_POS(pos, max_len);
    return pos;
}

/* Предварительное объявление json_escape (определена ниже, в секции HTTP) */
int json_escape(const char *in, char *out, int max_len);

/* ========== Seed Deploy ========== */

/* Получить статус systemd-сервиса seed-deploy */
static int deploy_get_status(char *out, int max_len) {
    /* Проверяем, есть ли скрипт */
    struct stat st;
    if (stat(SEED_DEPLOY_SH, &st) != 0) {
        snprintf(out, max_len, "{\"status\":\"none\",\"script\":null,\"pid\":null}");
        return 0;
    }

    /* Читаем скрипт (static — иначе 48KB на стеке поверх handle_request) */
    static char script[MAX_BODY];
    script[0] = '\0';
    FILE *fp = fopen(SEED_DEPLOY_SH, "r");
    if (fp) {
        int n = fread(script, 1, sizeof(script) - 1, fp);
        script[n] = '\0';
        fclose(fp);
    }

    /* Проверяем статус сервиса */
    char status[32] = "unknown";
    int pid = 0;
    fp = popen("systemctl is-active " SEED_SERVICE " 2>/dev/null", "r");
    if (fp) {
        if (fgets(status, sizeof(status), fp)) {
            int l = strlen(status);
            while (l > 0 && (status[l-1] == '\n' || status[l-1] == '\r')) status[--l] = '\0';
        }
        pclose(fp);
    }

    /* PID */
    fp = popen("systemctl show -p MainPID --value " SEED_SERVICE " 2>/dev/null", "r");
    if (fp) {
        char pid_buf[32] = "";
        if (fgets(pid_buf, sizeof(pid_buf), fp))
            pid = atoi(pid_buf);
        pclose(fp);
    }

    /* Маппим статус.
     * Для oneshot сервисов с RemainAfterExit=yes: active + pid==0 = скрипт завершился.
     * Показываем "finished" вместо "running" чтобы не путать пользователя. */
    const char *st_str = "stopped";
    if (strcmp(status, "active") == 0 && pid == 0) st_str = "finished";
    else if (strcmp(status, "active") == 0) st_str = "running";
    else if (strcmp(status, "activating") == 0) st_str = "running";
    else if (strcmp(status, "inactive") == 0) st_str = "stopped";
    else if (strcmp(status, "failed") == 0) st_str = "stopped";

    /* JSON-escape скрипта (static — экономим стек) */
    static char escaped_script[MAX_BODY * 2];
    json_escape(script, escaped_script, sizeof(escaped_script));

    snprintf(out, max_len,
        "{\"status\":\"%s\",\"script\":\"%s\",\"pid\":%d}",
        st_str, escaped_script, pid > 0 ? pid : 0);
    return 0;
}

/* ========== HTTP ========== */

typedef struct {
    char method[8];
    char path[256];
    char body[MAX_BODY];
    int  body_len;
    int  content_length;
    char auth_token[128];
    int  large_body_fd;       /* fd для чтения большого тела (>MAX_BODY) */
    int  body_remaining;      /* сколько байт тела ещё не прочитано из fd */
    /* Custom headers для vault */
    char x_node_type[32];     /* X-Node-Type: esp32/pi */
    char x_version[64];       /* X-Version: version string */
    char x_description[256];  /* X-Description: firmware description */
} http_req_t;

int parse_request(int fd, http_req_t *req) {
    char buf[HTTP_BUF];
    memset(req, 0, sizeof(*req));

    int total = 0;
    int header_end = 0;

    while (total < HTTP_BUF - 1) {
        int n = read(fd, buf + total, HTTP_BUF - 1 - total);
        if (n <= 0) return -1;
        total += n;
        buf[total] = '\0';

        char *end = strstr(buf, "\r\n\r\n");
        if (end) {
            header_end = (end - buf) + 4;
            break;
        }
    }

    if (!header_end) return -1;

    sscanf(buf, "%7s %255s", req->method, req->path);

    char *cl = strcasestr(buf, "Content-Length:");
    if (cl) req->content_length = atoi(cl + 15);

    /* Authorization: Bearer <token> */
    char *auth = strcasestr(buf, "Authorization: Bearer ");
    if (auth) {
        auth += 22;
        char *end = strstr(auth, "\r\n");
        if (end) {
            int len = end - auth;
            if (len > 0 && len < (int)sizeof(req->auth_token)) {
                memcpy(req->auth_token, auth, len);
                req->auth_token[len] = '\0';
            }
        }
    }

    /* Custom headers: X-Node-Type, X-Version, X-Description */
    {
        char *xnt = strcasestr(buf, "\r\nX-Node-Type:");
        if (xnt) {
            xnt += 14; /* strlen("\r\nX-Node-Type:") */
            while (*xnt == ' ') xnt++;
            char *end = strstr(xnt, "\r\n");
            if (end) {
                int len = end - xnt;
                if (len > 0 && len < (int)sizeof(req->x_node_type))
                    { memcpy(req->x_node_type, xnt, len); req->x_node_type[len] = '\0'; }
            }
        }
        char *xv = strcasestr(buf, "\r\nX-Version:");
        if (xv) {
            xv += 12; /* strlen("\r\nX-Version:") */
            while (*xv == ' ') xv++;
            char *end = strstr(xv, "\r\n");
            if (end) {
                int len = end - xv;
                if (len > 0 && len < (int)sizeof(req->x_version))
                    { memcpy(req->x_version, xv, len); req->x_version[len] = '\0'; }
            }
        }
        char *xd = strcasestr(buf, "\r\nX-Description:");
        if (xd) {
            xd += 16; /* strlen("\r\nX-Description:") */
            while (*xd == ' ') xd++;
            char *end = strstr(xd, "\r\n");
            if (end) {
                int len = end - xd;
                if (len > 0 && len < (int)sizeof(req->x_description))
                    { memcpy(req->x_description, xd, len); req->x_description[len] = '\0'; }
            }
        }
    }

    /* Тело слишком большое — проверяем, можно ли обработать как large body */
    if (req->content_length > MAX_BODY) {
        /* Разрешаем large body для POST /firmware/source и POST /vault/store */
        if (strcmp(req->method, "POST") == 0 &&
            (strcmp(req->path, "/firmware/source") == 0 || strcmp(req->path, "/vault/store") == 0)) {
            /* Сохраняем то, что уже прочитали из тела, в body[]
             * HTTP_BUF=8192, body_so_far максимум ~7KB — всегда влезет в MAX_BODY */
            int body_so_far = total - header_end;
            if (body_so_far > 0) {
                int copy = body_so_far < MAX_BODY - 1 ? body_so_far : MAX_BODY - 1;
                memcpy(req->body, buf + header_end, copy);
                req->body_len = copy;
            }
            /* Остаток тела хэндлер будет читать из fd напрямую */
            req->large_body_fd = fd;
            req->body_remaining = req->content_length - (body_so_far > 0 ? body_so_far : 0);
            if (req->body_remaining < 0) req->body_remaining = 0;
            return 0;
        }
        /* Остальные эндпоинты — 413 */
        return -2;
    }

    int body_so_far = total - header_end;
    if (body_so_far > 0) {
        int copy = body_so_far < MAX_BODY - 1 ? body_so_far : MAX_BODY - 1;
        memcpy(req->body, buf + header_end, copy);
        req->body_len = copy;
    }

    while (req->body_len < req->content_length && req->body_len < MAX_BODY - 1) {
        int n = read(fd, req->body + req->body_len, MAX_BODY - 1 - req->body_len);
        if (n <= 0) break;
        req->body_len += n;
    }
    req->body[req->body_len] = '\0';

    return 0;
}

void send_response(int fd, int status, const char *status_text, const char *body) {
    char header[512];
    int body_len = body ? strlen(body) : 0;
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, body_len);
    write(fd, header, hlen);
    if (body && body_len > 0)
        write(fd, body, body_len);
}

void send_html_response(int fd, int status, const char *status_text, const char *html) {
    char header[512];
    int body_len = html ? strlen(html) : 0;
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, body_len);
    write(fd, header, hlen);
    if (html && body_len > 0)
        write(fd, html, body_len);
}

void send_text_response(int fd, int status, const char *status_text, const char *text, const char *content_type) {
    char header[512];
    int body_len = text ? strlen(text) : 0;
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    write(fd, header, hlen);
    if (text && body_len > 0)
        write(fd, text, body_len);
}

int json_escape(const char *in, char *out, int max_len) {
    int j = 0;
    for (int i = 0; in[i] && j < max_len - 7; i++) {
        switch (in[i]) {
            case '"':  if (j+2<max_len) { out[j++]='\\'; out[j++]='"'; } break;
            case '\\': if (j+2<max_len) { out[j++]='\\'; out[j++]='\\'; } break;
            case '\n': if (j+2<max_len) { out[j++]='\\'; out[j++]='n'; } break;
            case '\r': if (j+2<max_len) { out[j++]='\\'; out[j++]='r'; } break;
            case '\t': if (j+2<max_len) { out[j++]='\\'; out[j++]='t'; } break;
            case '\b': if (j+2<max_len) { out[j++]='\\'; out[j++]='b'; } break;
            default:
                if ((unsigned char)in[i] < 0x20) {
                    /* Control chars → \uXXXX */
                    if (j+6<max_len) {
                        j += snprintf(out + j, max_len - j, "\\u%04x", (unsigned char)in[i]);
                    }
                } else {
                    out[j++] = in[i];
                }
                break;
        }
    }
    out[j] = '\0';
    return j;
}

void handle_request(int fd, const char *client_ip) {
    http_req_t req;
    int parse_rc = parse_request(fd, &req);
    if (parse_rc == -2) {
        /* Тело запроса превышает MAX_BODY — 413 */
        send_response(fd, 413, "Payload Too Large", "{\"error\":\"Payload Too Large\"}");
        return;
    }
    if (parse_rc < 0) {
        send_response(fd, 400, "Bad Request", "{\"error\": \"bad request\"}");
        return;
    }

    char output[BUF_SIZE];
    char escaped[BUF_SIZE * 2];
    char response[BUF_SIZE * 2 + 256];

    if (strcmp(req.method, "OPTIONS") == 0) {
        char h[512];
        int n = snprintf(h, sizeof(h),
            "HTTP/1.1 204 No Content\r\n"
            "Connection: close\r\n\r\n");
        write(fd, h, n);
        return;
    }

    /* Pairing page — всегда доступна */
    if (strcmp(req.path, "/") == 0 && strcmp(req.method, "GET") == 0) {
        static const char pairing_html[] =
            "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
            "<title>ClawKVM Setup</title>"
            "<style>"
            "body{font-family:system-ui;max-width:500px;margin:40px auto;padding:20px;background:#0d1117;color:#e6edf3}"
            "h1{color:#58a6ff}input,button{width:100%;padding:12px;margin:8px 0;border-radius:8px;border:1px solid #30363d;"
            "background:#161b22;color:#e6edf3;font-size:16px;box-sizing:border-box}"
            "button{background:#238636;border:none;cursor:pointer;font-weight:bold}"
            "button:hover{background:#2ea043}.ok{color:#3fb950}.err{color:#f85149}"
            ".token{background:#161b22;padding:12px;border-radius:8px;font-family:monospace;word-break:break-all;margin:8px 0}"
            ".section{margin:20px 0;padding:16px;border:1px solid #30363d;border-radius:8px}"
            "</style></head><body>"
            "<h1>ClawKVM Setup</h1>"
            "<div class='section'>"
            "<h3>1. Pair with host computer</h3>"
            "<p>Enter the username on the computer this dongle is plugged into:</p>"
            "<input id='user' placeholder='Username' autofocus>"
            "<input id='pass' type='password' placeholder='Password (for SSH key setup)'>"
            "<button onclick='doPair()'>Pair</button>"
            "<div id='pair-result'></div>"
            "</div>"
            "<div class='section'>"
            "<h3>2. Download Claude skill</h3>"
            "<p>After pairing, download the skill file and place it in <code>~/.claude/skills/</code></p>"
            "<button onclick='location.href=\"/skill\"'>Download Skill</button>"
            "</div>"
            "<div class='section'>"
            "<h3>API Token</h3>"
            "<p>For WiFi access, use this token in the Authorization header:</p>"
            "<div class='token' id='token'>Loading...</div>"
            "</div>"
            "<script>"
            "fetch('/token').then(r=>r.json()).then(d=>document.getElementById('token').textContent=d.token);"
            "function doPair(){"
            "let u=document.getElementById('user').value,p=document.getElementById('pass').value,r=document.getElementById('pair-result');"
            "r.innerHTML='Pairing...';"
            "fetch('/pair',{method:'POST',headers:{'Content-Type':'application/json'},"
            "body:JSON.stringify({user:u,pass:p})})"
            ".then(r=>r.json()).then(d=>{"
            "document.getElementById('pair-result').innerHTML=d.ok?"
            "'<p class=\"ok\">Paired! Host: '+d.hostname+'</p>'"
            ":'<p class=\"err\">Error: '+d.error+'</p>';"
            "}).catch(e=>r.innerHTML='<p class=\"err\">'+e+'</p>')}"
            "</script></body></html>";
        send_html_response(fd, 200, "OK", pairing_html);
        return;
    }

    /* GET /token — показать токен (только USB/BT) */
    if (strcmp(req.path, "/token") == 0 && strcmp(req.method, "GET") == 0) {
        if (!is_trusted_ip(client_ip)) {
            send_response(fd, 403, "Forbidden", "{\"error\":\"token only visible via USB/BT\"}");
            return;
        }
        snprintf(response, sizeof(response), "{\"token\":\"%s\"}", api_token);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /pair — SSH key exchange */
    if (strcmp(req.path, "/pair") == 0 && strcmp(req.method, "POST") == 0) {
        /* Парсим JSON {user, pass} */
        char user[64] = "", pass[128] = "";
        char *u = strstr(req.body, "\"user\":\"");
        char *p = strstr(req.body, "\"pass\":\"");
        if (u) { u += 8; char *e = strchr(u, '"'); if (e) { int l = e-u; if (l<64) { memcpy(user,u,l); user[l]=0; } } }
        if (p) { p += 8; char *e = strchr(p, '"'); if (e) { int l = e-p; if (l<128) { memcpy(pass,p,l); pass[l]=0; } } }

        if (user[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"username required\"}");
            return;
        }

        fprintf(stderr, "Pairing with user=%s\n", user);

        /* Читаем наш public key */
        char pubkey[512] = "";
        FILE *fp = fopen(SSH_KEY ".pub", "r");
        if (fp) { fgets(pubkey, sizeof(pubkey), fp); fclose(fp); }
        int pkl = strlen(pubkey);
        while (pkl > 0 && (pubkey[pkl-1] == '\n' || pubkey[pkl-1] == '\r')) pubkey[--pkl] = '\0';

        /* Определяем IP хоста — берём из client_ip если на USB-подсети */
        char pair_ip[64];
        if (strncmp(client_ip, USB_SUBNET, strlen(USB_SUBNET)) == 0)
            strncpy(pair_ip, client_ip, sizeof(pair_ip) - 1);
        else
            strncpy(pair_ip, host_ip, sizeof(pair_ip) - 1);
        pair_ip[sizeof(pair_ip)-1] = '\0';

        /* Копируем ключ через sshpass + ssh */
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "sshpass -p '%s' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "
            "%s@%s 'mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
            "grep -qF \"%s\" ~/.ssh/authorized_keys 2>/dev/null || echo \"%s\" >> ~/.ssh/authorized_keys && "
            "chmod 600 ~/.ssh/authorized_keys && hostname' 2>&1",
            pass, user, pair_ip, pubkey, pubkey);
        fp = popen(cmd, "r");
        char result[512] = "";
        if (fp) {
            fgets(result, sizeof(result), fp);
            int l = strlen(result);
            while (l > 0 && (result[l-1] == '\n' || result[l-1] == '\r')) result[--l] = '\0';
            pclose(fp);
        }

        /* Проверяем SSH без пароля */
        snprintf(cmd, sizeof(cmd),
            "ssh -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=5 "
            "-i %s %s@%s 'hostname' 2>&1", SSH_KEY, user, pair_ip);
        fp = popen(cmd, "r");
        char hostname[256] = "";
        int ssh_ok = 0;
        if (fp) {
            if (fgets(hostname, sizeof(hostname), fp)) {
                int l = strlen(hostname);
                while (l > 0 && (hostname[l-1] == '\n' || hostname[l-1] == '\r')) hostname[--l] = '\0';
            }
            ssh_ok = (pclose(fp) == 0);
        }

        if (ssh_ok) {
            /* Сохраняем профиль (под локом — thread safety с monitor) */
            pthread_mutex_lock(&host_lock);
            strncpy(host_user, user, sizeof(host_user) - 1);
            strncpy(host_ip, pair_ip, sizeof(host_ip) - 1);
            host_paired = 1;
            pthread_mutex_unlock(&host_lock);

            mkdir(HOSTS_DIR, 0700);
            char profile_path[512];
            snprintf(profile_path, sizeof(profile_path), "%s/current.json", HOSTS_DIR);
            fp = fopen(profile_path, "w");
            if (fp) {
                fprintf(fp, "{\"user\":\"%s\",\"hostname\":\"%s\",\"ip\":\"%s\"}\n", user, hostname, pair_ip);
                fclose(fp);
            }

            snprintf(response, sizeof(response),
                "{\"ok\":true,\"hostname\":\"%s\",\"user\":\"%s\"}", hostname, user);
            send_response(fd, 200, "OK", response);
        } else {
            snprintf(response, sizeof(response),
                "{\"ok\":false,\"error\":\"SSH key auth failed: %s\"}", result);
            send_response(fd, 200, "OK", response);
        }
        return;
    }

    /* POST /host/exec — выполнить команду на хосте через SSH */
    if (strcmp(req.path, "/host/exec") == 0 && strcmp(req.method, "POST") == 0) {
        if (!host_paired) {
            send_response(fd, 503, "Service Unavailable", "{\"error\":\"not paired with host\"}");
            return;
        }
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty command\"}");
            return;
        }
        while (req.body_len > 0 && (req.body[req.body_len-1] == '\n' || req.body[req.body_len-1] == '\r'))
            req.body[--req.body_len] = '\0';

        {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);
            fprintf(stderr, "[AUDIT] %s %s /host/exec: %.200s\n", ts, client_ip, req.body);
        }
        int exit_code = host_exec(req.body, output, sizeof(output));
        json_escape(output, escaped, sizeof(escaped));
        snprintf(response, sizeof(response),
            "{\"output\":\"%s\",\"exit_code\":%d}", escaped, exit_code);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /host/info — информация о хосте */
    if (strcmp(req.path, "/host/info") == 0) {
        if (!host_paired) {
            send_response(fd, 503, "Service Unavailable", "{\"error\":\"not paired with host\"}");
            return;
        }
        if (!host_connected) {
            snprintf(response, sizeof(response),
                "{\"error\":\"host not connected\",\"user\":\"%s\",\"host_ip\":\"%s\","
                "\"usb\":\"%s\",\"paired\":true,\"connected\":false}",
                host_user, host_ip, usb_up ? "up" : "down");
            send_response(fd, 200, "OK", response);
            return;
        }
        int rc = host_exec("hostname; uname -s; uptime -p 2>/dev/null || uptime", output, sizeof(output));
        json_escape(output, escaped, sizeof(escaped));
        if (rc != 0) {
            /* SSH команда упала — хост отключился между проверками монитора */
            snprintf(response, sizeof(response),
                "{\"output\":\"%s\",\"user\":\"%s\",\"paired\":true,\"connected\":false}", escaped, host_user);
        } else {
            snprintf(response, sizeof(response),
                "{\"output\":\"%s\",\"user\":\"%s\",\"paired\":true,\"connected\":true}", escaped, host_user);
        }
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /hosts — список спаренных хостов + статус подключения */
    if (strcmp(req.path, "/hosts") == 0) {
        snprintf(response, sizeof(response),
            "{\"paired\":%s,\"user\":\"%s\",\"host\":\"%s\",\"connected\":%s}",
            host_paired ? "true" : "false", host_user, host_ip,
            host_connected ? "true" : "false");
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /skill — скилл-файл для Claude (содержит token, требует auth) */
    if (strcmp(req.path, "/skill") == 0) {
        if (!is_trusted_ip(client_ip) && (api_token[0] && strcmp(req.auth_token, api_token) != 0)) {
            send_response(fd, 401, "Unauthorized", "{\"error\":\"Authorization: Bearer <token> required\"}");
            return;
        }
        char hostname[256] = "unknown";
        gethostname(hostname, sizeof(hostname));
        char skill[8192];
        snprintf(skill, sizeof(skill),
            "# Seed — Universal Pi Node\n\n"
            "Seed node connected. API: http://seed.local:8080\n"
            "Token: %s\n\n"
            "## KVM — Execute command on host\n"
            "```bash\n"
            "curl -s -X POST http://seed.local:8080/host/exec \\\n"
            "  -H 'Authorization: Bearer %s' \\\n"
            "  -d 'ls -la'\n"
            "```\n\n"
            "## GPIO — Configure and control pins\n"
            "```bash\n"
            "# Set pin mode\n"
            "curl -s -X POST http://seed.local:8080/gpio/12/mode \\\n"
            "  -H 'Authorization: Bearer %s' -d 'output'\n"
            "# Write pin value\n"
            "curl -s -X POST http://seed.local:8080/gpio/12/write \\\n"
            "  -H 'Authorization: Bearer %s' -d 'high'\n"
            "# Read pin\n"
            "curl -s http://seed.local:8080/gpio/12/read \\\n"
            "  -H 'Authorization: Bearer %s'\n"
            "# All pins status\n"
            "curl -s http://seed.local:8080/gpio/status \\\n"
            "  -H 'Authorization: Bearer %s'\n"
            "```\n\n"
            "## I2C — Scan and communicate\n"
            "```bash\n"
            "curl -s -X POST http://seed.local:8080/i2c/scan \\\n"
            "  -H 'Authorization: Bearer %s'\n"
            "# Write hex bytes\n"
            "curl -s -X POST http://seed.local:8080/i2c/0x27/write \\\n"
            "  -H 'Authorization: Bearer %s' -d '0A FF 01'\n"
            "# Read 4 bytes\n"
            "curl -s http://seed.local:8080/i2c/0x27/read/4 \\\n"
            "  -H 'Authorization: Bearer %s'\n"
            "```\n\n"
            "## Config, Capabilities, Deploy\n"
            "```bash\n"
            "curl -s http://seed.local:8080/capabilities \\\n"
            "  -H 'Authorization: Bearer %s'\n"
            "curl -s http://seed.local:8080/config.md \\\n"
            "  -H 'Authorization: Bearer %s'\n"
            "curl -s -X POST http://seed.local:8080/deploy \\\n"
            "  -H 'Authorization: Bearer %s' -d '#!/bin/bash\\nwhile true; do echo ok; sleep 60; done'\n"
            "```\n",
            api_token, api_token, api_token, api_token, api_token, api_token,
            api_token, api_token, api_token, api_token, api_token, api_token);
        send_text_response(fd, 200, "OK", skill, "text/markdown; charset=utf-8");
        return;
    }

    /* GET /notes — прочитать заметки AI */
    /* /notes — перенесены после auth check (см. секцию Agent Notes ниже) */

    /* GET /health — без auth, как на ESP32 (для мониторинга и watchdog) */
    if (strcmp(req.path, "/health") == 0 && strcmp(req.method, "GET") == 0) {
        time_t uptime_sec = time(NULL) - server_start_time;
        snprintf(response, sizeof(response),
            "{\"ok\":true,\"uptime_sec\":%ld,\"type\":\"pi-zero-w\"}",
            (long)uptime_sec);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* === Auth check для остальных эндпоинтов === */
    if (!is_trusted_ip(client_ip)) {
        /* WiFi — нужен токен */
        if (api_token[0] && strcmp(req.auth_token, api_token) != 0) {
            send_response(fd, 401, "Unauthorized",
                "{\"error\":\"Authorization: Bearer <token> required\"}");
            return;
        }
    }

    /* GET /status */
    if (strcmp(req.path, "/status") == 0) {
        const char *host_status = !host_paired ? "not_paired"
            : (host_connected ? "connected" : "disconnected");
        snprintf(response, sizeof(response),
            "{\"serial\":\"%s\",\"device\":\"%s\",\"baudrate\":%d,"
            "\"keyboard\":\"%s\",\"hid_device\":\"%s\","
            "\"usb\":\"%s\","
            "\"host\":\"%s\",\"host_ip\":\"%s\",\"host_user\":\"%s\"}",
            serial_fd >= 0 ? "ready" : "not available",
            serial_device, serial_baudrate,
            hid_is_available() ? "ready" : "not available",
            HID_DEV,
            usb_up ? "up" : "down",
            host_status, host_ip, host_user);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /serial/list — список доступных serial портов */
    if (strcmp(req.path, "/serial/list") == 0) {
        char ports[4096];
        serial_list_ports(ports, sizeof(ports));
        snprintf(response, sizeof(response),
            "{\"ports\":%s,\"current\":\"%s\",\"baudrate\":%d}",
            ports, serial_device, serial_baudrate);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /serial/switch — переключить serial порт
     * Body: "/dev/ttyUSB0:9600" или "/dev/ttyAMA0" (baud по умолчанию) */
    if (strcmp(req.path, "/serial/switch") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request",
                "{\"error\":\"body: /dev/ttyXXX[:baudrate]\"}");
            return;
        }

        /* Trim */
        while (req.body_len > 0 && (req.body[req.body_len-1] == '\n' || req.body[req.body_len-1] == '\r'))
            req.body[--req.body_len] = '\0';

        char new_dev[256] = {0};
        int new_baud = serial_baudrate;

        char *colon = strchr(req.body, ':');
        if (colon) {
            *colon = '\0';
            new_baud = atoi(colon + 1);
            if (new_baud <= 0) new_baud = serial_baudrate;
        }
        strncpy(new_dev, req.body, sizeof(new_dev) - 1);

        fprintf(stderr, "Switching serial to %s at %d\n", new_dev, new_baud);

        if (serial_switch(new_dev, new_baud) == 0) {
            snprintf(response, sizeof(response),
                "{\"ok\":true,\"device\":\"%s\",\"baudrate\":%d}",
                serial_device, serial_baudrate);
            send_response(fd, 200, "OK", response);
        } else {
            snprintf(response, sizeof(response),
                "{\"ok\":false,\"error\":\"cannot open %s\"}",
                new_dev);
            send_response(fd, 500, "Error", response);
        }
        return;
    }

    /* POST /serial/enable — настроить PL011 UART на GPIO 14/15
     * Отключает BT (который занимает ttyAMA0), включает UART, убирает serial console.
     * Требует перезагрузки. */
    if (strcmp(req.path, "/serial/enable") == 0 && strcmp(req.method, "POST") == 0) {
        /* Определяем boot config */
        const char *boot_cfg = "/boot/firmware/config.txt";
        if (access(boot_cfg, F_OK) != 0) boot_cfg = "/boot/config.txt";

        int changed = 0;
        char check_cmd[512];

        /* enable_uart=1 */
        snprintf(check_cmd, sizeof(check_cmd),
            "grep -q '^enable_uart=1' %s 2>/dev/null || { echo 'enable_uart=1' >> %s && echo added; }",
            boot_cfg, boot_cfg);
        FILE *fp = popen(check_cmd, "r");
        if (fp) { char tmp[32]; if (fgets(tmp, sizeof(tmp), fp) && strstr(tmp, "added")) changed++; pclose(fp); }

        /* dtoverlay=disable-bt */
        snprintf(check_cmd, sizeof(check_cmd),
            "grep -q '^dtoverlay=disable-bt' %s 2>/dev/null || { echo 'dtoverlay=disable-bt' >> %s && echo added; }",
            boot_cfg, boot_cfg);
        fp = popen(check_cmd, "r");
        if (fp) { char tmp[32]; if (fgets(tmp, sizeof(tmp), fp) && strstr(tmp, "added")) changed++; pclose(fp); }

        /* Убираем serial console из cmdline.txt */
        system("sed -i 's/console=serial0,[0-9]* //g' /boot/cmdline.txt 2>/dev/null");
        system("sed -i 's/console=ttyAMA0,[0-9]* //g' /boot/cmdline.txt 2>/dev/null");
        system("sed -i 's/console=serial0,[0-9]* //g' /boot/firmware/cmdline.txt 2>/dev/null");
        system("sed -i 's/console=ttyAMA0,[0-9]* //g' /boot/firmware/cmdline.txt 2>/dev/null");

        /* Отключаем hciuart */
        system("systemctl disable hciuart.service 2>/dev/null");
        system("systemctl stop hciuart.service 2>/dev/null");

        if (changed > 0) {
            snprintf(response, sizeof(response),
                "{\"ok\":true,\"message\":\"UART enabled on GPIO 14/15 (PL011). Bluetooth disabled. Reboot required.\","
                "\"device\":\"/dev/ttyAMA0\",\"reboot_required\":true,\"changes\":%d}", changed);
        } else {
            snprintf(response, sizeof(response),
                "{\"ok\":true,\"message\":\"UART already configured.\","
                "\"device\":\"/dev/ttyAMA0\",\"reboot_required\":false,\"changes\":0}");
        }
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /system/exec — выполнить команду на самой Pi (не на хосте!)
     * Аварийный эндпоинт: remount rw, fsck, перезагрузка, обновление. */
    if (strcmp(req.path, "/system/exec") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty command\"}");
            return;
        }

        while (req.body_len > 0 && (req.body[req.body_len-1] == '\n' || req.body[req.body_len-1] == '\r'))
            req.body[--req.body_len] = '\0';

        {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);
            fprintf(stderr, "[AUDIT] %s %s /system/exec: %.200s\n", ts, client_ip, req.body);
        }

        char cmd_buf[4096 + 32];
        snprintf(cmd_buf, sizeof(cmd_buf), "%s 2>&1", req.body);

        /* SIGCHLD=SIG_IGN ломает pclose() — waitpid возвращает ECHILD.
         * Временно ставим SIG_DFL чтобы получить корректный exit code. */
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen(cmd_buf, "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"popen failed\"}");
            return;
        }

        char result[8192] = "";
        int total = 0;
        char tmp[256];
        while (fgets(tmp, sizeof(tmp), fp) && total < (int)sizeof(result) - 256) {
            int n = strlen(tmp);
            memcpy(result + total, tmp, n);
            total += n;
        }
        result[total] = '\0';
        int rc = pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        json_escape(result, escaped, sizeof(escaped));
        snprintf(response, sizeof(response),
            "{\"output\":\"%s\",\"exit_code\":%d}", escaped, exit_code);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /system/reboot — перезагрузить Pi */
    if (strcmp(req.path, "/system/reboot") == 0 && strcmp(req.method, "POST") == 0) {
        {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);
            fprintf(stderr, "[AUDIT] %s %s /system/reboot\n", ts, client_ip);
        }
        send_response(fd, 200, "OK", "{\"ok\":true,\"message\":\"rebooting in 2 seconds\"}");
        event_add("system reboot requested");
        sleep(2);
        system("reboot");
        return;
    }

    /* POST /exec */
    if (strcmp(req.path, "/exec") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\": \"empty command\"}");
            return;
        }

        while (req.body_len > 0 && (req.body[req.body_len-1] == '\n' || req.body[req.body_len-1] == '\r'))
            req.body[--req.body_len] = '\0';

        fprintf(stderr, "POST /exec: %s\n", req.body);

        int len = serial_exec(req.body, output, sizeof(output), 10000);
        json_escape(output, escaped, sizeof(escaped));
        snprintf(response, sizeof(response), "{\"output\":\"%s\"}", escaped);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /read */
    if (strcmp(req.path, "/read") == 0) {
        int len = serial_flush(output, sizeof(output));
        json_escape(output, escaped, sizeof(escaped));
        snprintf(response, sizeof(response), "{\"output\":\"%s\"}", escaped);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /type */
    if (strcmp(req.path, "/type") == 0 && strcmp(req.method, "POST") == 0) {
        if (serial_fd < 0) {
            send_response(fd, 503, "Service Unavailable", "{\"error\":\"serial not available\"}");
            return;
        }
        write(serial_fd, req.body, req.body_len);
        snprintf(response, sizeof(response), "{\"typed\":%d}", req.body_len);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /info — полная информация о донгле */
    if (strcmp(req.path, "/info") == 0) {
        char hostname[256] = "unknown";
        gethostname(hostname, sizeof(hostname));

        /* Uptime */
        FILE *fp = fopen("/proc/uptime", "r");
        double uptime_sec = 0;
        if (fp) { fscanf(fp, "%lf", &uptime_sec); fclose(fp); }

        /* IP адреса */
        char ip_buf[512] = "";
        fp = popen("hostname -I 2>/dev/null", "r");
        if (fp) {
            if (fgets(ip_buf, sizeof(ip_buf), fp)) {
                int l = strlen(ip_buf);
                while (l > 0 && (ip_buf[l-1] == '\n' || ip_buf[l-1] == ' ')) ip_buf[--l] = '\0';
            }
            pclose(fp);
        }

        snprintf(response, sizeof(response),
            "{\"hostname\":\"%s\",\"uptime_sec\":%.0f,"
            "\"serial\":{\"device\":\"%s\",\"baudrate\":%d,\"status\":\"%s\"},"
            "\"keyboard\":{\"device\":\"%s\",\"status\":\"%s\"},"
            "\"ip\":\"%s\","
            "\"endpoints\":["
            "\"GET /status\",\"GET /health\",\"GET /info\","
            "\"GET /hosts\",\"GET /host/info\",\"POST /host/exec\","
            "\"POST /exec\",\"POST /read\",\"POST /type\","
            "\"POST /system/exec\",\"POST /system/reboot\","
            "\"GET /serial/list\",\"POST /serial/switch\",\"POST /serial/enable\","
            "\"POST /keyboard/type\",\"POST /keyboard/keys\","
            "\"POST /keyboard/command\",\"GET /keyboard/status\","
            "\"GET /notes\",\"POST /notes\",\"DELETE /notes/{id}\","
            "\"POST /gpio/{pin}/mode\",\"POST /gpio/{pin}/write\","
            "\"GET /gpio/{pin}/read\",\"GET /gpio/status\","
            "\"POST /i2c/scan\",\"POST /i2c/{addr}/write\","
            "\"GET /i2c/{addr}/read/{count}\","
            "\"GET /config.md\",\"POST /config.md\","
            "\"GET /capabilities\","
            "\"POST /deploy\",\"GET /deploy\",\"DELETE /deploy\","
            "\"GET /wg/status\",\"POST /wg/config\","
            "\"POST /wg/peer\",\"DELETE /wg/peer\",\"GET /wg/peers\","
            "\"GET /firmware/version\",\"GET /firmware/source\",\"POST /firmware/source\","
            "\"POST /firmware/build\",\"GET /firmware/build/logs\",\"POST /firmware/apply\","
            "\"POST /vault/store\",\"GET /vault/list\",\"GET /vault/{id}\","
            "\"POST /vault/push/{node}\",\"DELETE /vault/{id}\""
            "]}",
            hostname, uptime_sec,
            serial_device, serial_baudrate,
            serial_fd >= 0 ? "ready" : "not available",
            HID_DEV,
            hid_is_available() ? "ready" : "not available",
            ip_buf);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /keyboard/type — печатаем текст через USB HID клавиатуру */
    if (strcmp(req.path, "/keyboard/type") == 0 && strcmp(req.method, "POST") == 0) {
        if (!hid_is_available()) {
            send_response(fd, 503, "Service Unavailable",
                "{\"error\":\"HID keyboard not available (/dev/hidg0)\"}");
            return;
        }
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty text\"}");
            return;
        }

        fprintf(stderr, "POST /keyboard/type: %d chars\n", req.body_len);
        int typed = hid_type_text(req.body, req.body_len);
        snprintf(response, sizeof(response), "{\"typed\":%d,\"total\":%d}", typed, req.body_len);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /keyboard/keys — комбинации клавиш (body: "ctrl+c", "enter", "alt+tab") */
    if (strcmp(req.path, "/keyboard/keys") == 0 && strcmp(req.method, "POST") == 0) {
        if (!hid_is_available()) {
            send_response(fd, 503, "Service Unavailable",
                "{\"error\":\"HID keyboard not available (/dev/hidg0)\"}");
            return;
        }
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty keys\"}");
            return;
        }

        fprintf(stderr, "POST /keyboard/keys: %s\n", req.body);
        hid_send_keys(req.body, response, sizeof(response));
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /keyboard/status */
    if (strcmp(req.path, "/keyboard/status") == 0) {
        snprintf(response, sizeof(response),
            "{\"available\":%s,\"device\":\"%s\"}",
            hid_is_available() ? "true" : "false",
            HID_DEV);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /keyboard/command — набрать команду и нажать Enter */
    if (strcmp(req.path, "/keyboard/command") == 0 && strcmp(req.method, "POST") == 0) {
        if (!hid_is_available()) {
            send_response(fd, 503, "Service Unavailable",
                "{\"error\":\"HID keyboard not available (/dev/hidg0)\"}");
            return;
        }
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty command\"}");
            return;
        }

        /* Trim */
        while (req.body_len > 0 && (req.body[req.body_len-1] == '\n' || req.body[req.body_len-1] == '\r'))
            req.body[--req.body_len] = '\0';

        fprintf(stderr, "POST /keyboard/command: %s\n", req.body);
        int typed = hid_type_text(req.body, req.body_len);
        /* Нажимаем Enter */
        usleep(50000);
        hid_send_report(0, 0x28); /* enter */
        snprintf(response, sizeof(response),
            "{\"typed\":%d,\"enter\":true,\"command\":\"%s\"}", typed, req.body);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* ========== Seed GPIO API ========== */

    /* POST /gpio/{pin}/mode — задать режим пина (body: "input" | "output") */
    if (strncmp(req.path, "/gpio/", 6) == 0 && strcmp(req.method, "POST") == 0) {
        /* Парсим /gpio/12/mode или /gpio/12/write */
        int pin = 0;
        char action[16] = "";
        if (sscanf(req.path, "/gpio/%d/%15s", &pin, action) < 2) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"use /gpio/{pin}/mode or /gpio/{pin}/write\"}");
            return;
        }

        if (!gpio_is_valid_pin(pin)) {
            snprintf(response, sizeof(response), "{\"error\":\"invalid GPIO pin %d\"}", pin);
            send_response(fd, 400, "Bad Request", response);
            return;
        }

        if (strcmp(action, "mode") == 0) {
            /* Bug #2: GPIO 14/15 зарезервированы для UART serial — блокируем если serial активен */
            if ((pin == 14 || pin == 15) && serial_fd != -1) {
                send_response(fd, 400, "Bad Request",
                    "{\"error\":\"GPIO 14/15 reserved for UART serial\"}");
                return;
            }

            /* Trim body */
            while (req.body_len > 0 && (req.body[req.body_len-1] == '\n' || req.body[req.body_len-1] == '\r' || req.body[req.body_len-1] == ' '))
                req.body[--req.body_len] = '\0';

            const char *dir = NULL;
            if (strcmp(req.body, "input") == 0 || strcmp(req.body, "in") == 0)
                dir = "in";
            else if (strcmp(req.body, "output") == 0 || strcmp(req.body, "out") == 0)
                dir = "out";
            else {
                send_response(fd, 400, "Bad Request", "{\"error\":\"body must be 'input' or 'output'\"}");
                return;
            }

            if (gpio_export(pin) < 0) {
                snprintf(response, sizeof(response), "{\"error\":\"cannot export GPIO %d: %s\"}", pin, strerror(errno));
                send_response(fd, 500, "Error", response);
                return;
            }

            if (gpio_set_direction(pin, dir) < 0) {
                snprintf(response, sizeof(response), "{\"error\":\"cannot set direction GPIO %d: %s\"}", pin, strerror(errno));
                send_response(fd, 500, "Error", response);
                return;
            }

            gpio_track_pin(pin);
            event_add("gpio %d mode set to %s", pin, dir);
            snprintf(response, sizeof(response), "{\"ok\":true,\"pin\":%d,\"direction\":\"%s\"}", pin, dir);
            send_response(fd, 200, "OK", response);
            return;
        }

        if (strcmp(action, "write") == 0) {
            /* Trim body */
            while (req.body_len > 0 && (req.body[req.body_len-1] == '\n' || req.body[req.body_len-1] == '\r' || req.body[req.body_len-1] == ' '))
                req.body[--req.body_len] = '\0';

            int val = -1;
            if (strcmp(req.body, "high") == 0 || strcmp(req.body, "1") == 0)
                val = 1;
            else if (strcmp(req.body, "low") == 0 || strcmp(req.body, "0") == 0)
                val = 0;
            else {
                send_response(fd, 400, "Bad Request", "{\"error\":\"body must be 'high', 'low', '1', or '0'\"}");
                return;
            }

            if (gpio_find_pin(pin) < 0) {
                snprintf(response, sizeof(response), "{\"error\":\"GPIO %d not configured. POST /gpio/%d/mode first\"}", pin, pin);
                send_response(fd, 400, "Bad Request", response);
                return;
            }

            /* Bug #1: проверяем что пин сконфигурирован как output перед записью */
            {
                char dir[8] = "";
                gpio_read_direction(pin, dir, sizeof(dir));
                if (strcmp(dir, "in") == 0) {
                    snprintf(response, sizeof(response),
                        "{\"error\":\"pin %d is configured as input, set mode to output first\"}", pin);
                    send_response(fd, 400, "Bad Request", response);
                    return;
                }
            }

            if (gpio_write_value(pin, val) < 0) {
                snprintf(response, sizeof(response), "{\"error\":\"cannot write GPIO %d: %s\"}", pin, strerror(errno));
                send_response(fd, 500, "Error", response);
                return;
            }

            event_add("gpio %d set to %d", pin, val);
            snprintf(response, sizeof(response), "{\"ok\":true,\"pin\":%d,\"value\":%d}", pin, val);
            send_response(fd, 200, "OK", response);
            return;
        }

        /* Неизвестное действие для POST /gpio/{pin}/... */
        send_response(fd, 400, "Bad Request", "{\"error\":\"use /gpio/{pin}/mode or /gpio/{pin}/write\"}");
        return;
    }

    /* GET /gpio/{pin}/read — прочитать значение пина */
    if (strncmp(req.path, "/gpio/", 6) == 0 && strcmp(req.method, "GET") == 0) {
        /* Парсим /gpio/status или /gpio/12/read */
        int pin = 0;
        char action[16] = "";

        if (strcmp(req.path, "/gpio/status") == 0) {
            /* GET /gpio/status — все сконфигурированные пины */
            int pos = 0;
            int max_len = (int)sizeof(response);
            pos += snprintf(response + pos, max_len - pos, "[");
            CLAMP_POS(pos, max_len);

            pthread_mutex_lock(&gpio_lock);
            int first = 1;
            for (int i = 0; i < gpio_pin_count; i++) {
                if (!gpio_pins[i].configured) continue;
                int p = gpio_pins[i].pin;
                int val = gpio_read_value(p);
                char dir[8] = "";
                gpio_read_direction(p, dir, sizeof(dir));

                if (!first) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }
                pos += snprintf(response + pos, max_len - pos,
                    "{\"pin\":%d,\"value\":%d,\"direction\":\"%s\"}", p, val, dir);
                CLAMP_POS(pos, max_len);
                first = 0;
            }
            pthread_mutex_unlock(&gpio_lock);

            pos += snprintf(response + pos, max_len - pos, "]");
            CLAMP_POS(pos, max_len);
            send_response(fd, 200, "OK", response);
            return;
        }

        if (sscanf(req.path, "/gpio/%d/%15s", &pin, action) >= 1) {
            if (!gpio_is_valid_pin(pin)) {
                snprintf(response, sizeof(response), "{\"error\":\"invalid GPIO pin %d\"}", pin);
                send_response(fd, 400, "Bad Request", response);
                return;
            }

            /* /gpio/{pin}/read или просто /gpio/{pin} */
            if (action[0] == '\0' || strcmp(action, "read") == 0) {
                if (gpio_find_pin(pin) < 0) {
                    snprintf(response, sizeof(response), "{\"error\":\"GPIO %d not configured. POST /gpio/%d/mode first\"}", pin, pin);
                    send_response(fd, 400, "Bad Request", response);
                    return;
                }

                int val = gpio_read_value(pin);
                char dir[8] = "";
                gpio_read_direction(pin, dir, sizeof(dir));

                snprintf(response, sizeof(response), "{\"pin\":%d,\"value\":%d,\"direction\":\"%s\"}", pin, val, dir);
                send_response(fd, 200, "OK", response);
                return;
            }
        }

        /* Fallthrough — неизвестный GPIO GET */
        send_response(fd, 400, "Bad Request", "{\"error\":\"use GET /gpio/{pin}/read or GET /gpio/status\"}");
        return;
    }

    /* ========== Seed I2C API ========== */

    /* POST /i2c/scan — сканировать I2C шину */
    if (strcmp(req.path, "/i2c/scan") == 0 && strcmp(req.method, "POST") == 0) {
        i2c_scan(response, sizeof(response));
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /i2c/{addr}/write — записать байты */
    if (strncmp(req.path, "/i2c/", 5) == 0 && strcmp(req.method, "POST") == 0) {
        int addr = 0;
        char action[16] = "";
        if (sscanf(req.path, "/i2c/%i/%15s", &addr, action) < 2 || strcmp(action, "write") != 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"use POST /i2c/{addr}/write\"}");
            return;
        }
        if (addr < 0x03 || addr > 0x77) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"I2C address must be 0x03-0x77\"}");
            return;
        }

        /* Trim body */
        while (req.body_len > 0 && (req.body[req.body_len-1] == '\n' || req.body[req.body_len-1] == '\r'))
            req.body[--req.body_len] = '\0';

        i2c_write_bytes(addr, req.body, response, sizeof(response));
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /i2c/{addr}/read/{count} — прочитать байты */
    if (strncmp(req.path, "/i2c/", 5) == 0 && strcmp(req.method, "GET") == 0) {
        int addr = 0, count = 0;
        char action[16] = "";
        if (sscanf(req.path, "/i2c/%i/%15[^/]/%d", &addr, action, &count) < 3 || strcmp(action, "read") != 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"use GET /i2c/{addr}/read/{count}\"}");
            return;
        }
        if (addr < 0x03 || addr > 0x77) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"I2C address must be 0x03-0x77\"}");
            return;
        }

        i2c_read_bytes(addr, count, response, sizeof(response));
        send_response(fd, 200, "OK", response);
        return;
    }

    /* ========== Seed Config ========== */

    /* GET /config.md — прочитать конфиг ноды */
    if (strcmp(req.path, "/config.md") == 0 && strcmp(req.method, "GET") == 0) {
        char config[BUF_SIZE] = "";
        FILE *fp = fopen(SEED_CONFIG, "r");
        if (fp) {
            int n = fread(config, 1, sizeof(config) - 1, fp);
            config[n] = '\0';
            fclose(fp);
        }
        send_text_response(fd, 200, "OK", config, "text/markdown; charset=utf-8");
        return;
    }

    /* POST /config.md — записать конфиг ноды */
    if (strcmp(req.path, "/config.md") == 0 && strcmp(req.method, "POST") == 0) {
        /* Создаём директорию если нет */
        mkdir(SEED_DIR, 0755);
        FILE *fp = fopen(SEED_CONFIG, "w");
        if (fp) {
            fwrite(req.body, 1, req.body_len, fp);
            fclose(fp);
            snprintf(response, sizeof(response), "{\"ok\":true,\"bytes\":%d}", req.body_len);
            send_response(fd, 200, "OK", response);
        } else {
            snprintf(response, sizeof(response), "{\"error\":\"cannot write %s: %s\"}", SEED_CONFIG, strerror(errno));
            send_response(fd, 500, "Error", response);
        }
        return;
    }

    /* ========== Seed Capabilities ========== */

    /* GET /capabilities — описание возможностей ноды */
    if (strcmp(req.path, "/capabilities") == 0 && strcmp(req.method, "GET") == 0) {
        int pos = 0;
        int max_len = (int)sizeof(response);
        pos += snprintf(response + pos, max_len - pos,
            "{\"node\":\"seed\",\"type\":\"pi-zero-w\","
            "\"modules\":[\"gpio\",\"i2c\",\"kvm\",\"keyboard\",\"serial\",\"logs\",\"live\",\"events\",\"mesh\",\"wireguard\",\"net_scan\",\"net_tools\",\"system\",\"filesystem\",\"vault\",\"fleet\",\"notes\"],"
            "\"gpio_pins\":[");
        CLAMP_POS(pos, max_len);

        /* Список доступных GPIO пинов */
        for (int i = 0; i < (int)NUM_VALID_PINS; i++) {
            if (i > 0) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }
            pos += snprintf(response + pos, max_len - pos, "%d", gpio_valid_pins[i]);
            CLAMP_POS(pos, max_len);
        }

        pos += snprintf(response + pos, max_len - pos, "],\"gpio_configured\":[");
        CLAMP_POS(pos, max_len);

        /* Сконфигурированные пины с текущим состоянием */
        pthread_mutex_lock(&gpio_lock);
        int first = 1;
        for (int i = 0; i < gpio_pin_count; i++) {
            if (!gpio_pins[i].configured) continue;
            int p = gpio_pins[i].pin;
            int val = gpio_read_value(p);
            char dir[8] = "";
            gpio_read_direction(p, dir, sizeof(dir));

            if (!first) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }
            pos += snprintf(response + pos, max_len - pos,
                "{\"pin\":%d,\"direction\":\"%s\",\"value\":%d}", p, dir, val);
            CLAMP_POS(pos, max_len);
            first = 0;
        }
        pthread_mutex_unlock(&gpio_lock);

        pos += snprintf(response + pos, max_len - pos,
            "],\"i2c_bus\":\"%s\",\"has_kvm\":%s,\"has_keyboard\":%s}",
            I2C_BUS_DEV,
            host_paired ? "true" : "false",
            hid_is_available() ? "true" : "false");
        CLAMP_POS(pos, max_len);

        send_response(fd, 200, "OK", response);
        return;
    }

    /* ========== Seed Deploy ========== */

    /* POST /deploy — загрузить и запустить скрипт */
    if (strcmp(req.path, "/deploy") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty script body\"}");
            return;
        }

        /* Bug #6: очищаем лог предыдущего деплоя перед новым запуском */
        {
            FILE *lf = fopen(SEED_DEPLOY_LOG, "w");
            if (lf) fclose(lf);
        }

        /* Создаём директории */
        mkdir(SEED_DIR, 0755);
        mkdir(SEED_SCRIPTS, 0755);

        /* Определяем режим: oneshot (#!oneshot в первой строке) или daemon (по умолчанию) */
        int is_oneshot = 0;
        const char *script_body = req.body;
        size_t script_len = req.body_len;
        const char oneshot_marker[] = "#!oneshot";
        size_t marker_len = sizeof(oneshot_marker) - 1;

        if (script_len >= marker_len &&
            strncmp(script_body, oneshot_marker, marker_len) == 0 &&
            (script_len == marker_len || script_body[marker_len] == '\n' || script_body[marker_len] == '\r')) {
            is_oneshot = 1;
            /* Пропускаем маркер-строку — она не валидный bash */
            const char *nl = memchr(script_body, '\n', script_len);
            if (nl) {
                size_t skip = (size_t)(nl - script_body) + 1;
                script_body += skip;
                script_len -= skip;
            } else {
                /* Скрипт состоит только из маркера — пустое тело */
                script_body += script_len;
                script_len = 0;
            }
        }

        /* Записываем скрипт (без маркера #!oneshot если был) */
        FILE *fp = fopen(SEED_DEPLOY_SH, "w");
        if (!fp) {
            snprintf(response, sizeof(response), "{\"error\":\"cannot write %s: %s\"}", SEED_DEPLOY_SH, strerror(errno));
            send_response(fd, 500, "Error", response);
            return;
        }
        fwrite(script_body, 1, script_len, fp);
        fclose(fp);
        chmod(SEED_DEPLOY_SH, 0755);

        /* Создаём systemd unit с учётом режима oneshot/daemon */
        fp = fopen(SEED_UNIT_FILE, "w");
        if (!fp) {
            snprintf(response, sizeof(response), "{\"error\":\"cannot write %s: %s\"}", SEED_UNIT_FILE, strerror(errno));
            send_response(fd, 500, "Error", response);
            return;
        }
        if (is_oneshot) {
            /* Oneshot: скрипт выполнится один раз и остановится без рестарта */
            fprintf(fp,
                "[Unit]\n"
                "Description=Seed deployed script (oneshot)\n"
                "After=seed.service\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart=/bin/bash %s\n"
                "Restart=no\n"
                "\n"
                "[Install]\n"
                "WantedBy=multi-user.target\n",
                SEED_DEPLOY_SH);
        } else {
            /* Daemon: скрипт перезапускается при падении */
            fprintf(fp,
                "[Unit]\n"
                "Description=Seed deployed script\n"
                "After=seed.service\n"
                "StartLimitBurst=5\n"
                "StartLimitIntervalSec=60\n"
                "\n"
                "[Service]\n"
                "ExecStart=/bin/bash %s\n"
                "Restart=always\n"
                "RestartSec=5\n"
                "\n"
                "[Install]\n"
                "WantedBy=multi-user.target\n",
                SEED_DEPLOY_SH);
        }
        fclose(fp);

        const char *mode_str = is_oneshot ? "oneshot" : "daemon";

        /* Перезагрузить и запустить */
        event_add("deploy started");
        int rc = system("systemctl daemon-reload && systemctl restart " SEED_SERVICE " 2>&1");
        if (rc != 0) {
            snprintf(response, sizeof(response),
                "{\"ok\":true,\"mode\":\"%s\",\"warning\":\"script saved but systemctl failed (rc=%d). May need root.\"}", mode_str, WEXITSTATUS(rc));
        } else {
            snprintf(response, sizeof(response),
                "{\"ok\":true,\"mode\":\"%s\",\"script_bytes\":%zu,\"service\":\"%s\",\"status\":\"started\"}", mode_str, script_len, SEED_SERVICE);
        }
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /deploy — статус задеплоенного скрипта */
    if (strcmp(req.path, "/deploy") == 0 && strcmp(req.method, "GET") == 0) {
        deploy_get_status(response, sizeof(response));
        send_response(fd, 200, "OK", response);
        return;
    }

    /* DELETE /deploy — остановить и удалить скрипт */
    if (strcmp(req.path, "/deploy") == 0 && strcmp(req.method, "DELETE") == 0) {
        /* Останавливаем сервис */
        event_add("deploy stopped");
        system("systemctl stop " SEED_SERVICE " 2>/dev/null");
        system("systemctl disable " SEED_SERVICE " 2>/dev/null");

        /* Удаляем файлы */
        unlink(SEED_DEPLOY_SH);
        unlink(SEED_UNIT_FILE);
        system("systemctl daemon-reload 2>/dev/null");

        send_response(fd, 200, "OK", "{\"ok\":true,\"status\":\"removed\"}");
        return;
    }

    /* ========== Observability: deploy/logs, live.md, events ========== */

    /* GET /deploy/logs — логи задеплоенного скрипта */
    if (strcmp(req.path, "/deploy/logs") == 0 && strcmp(req.method, "GET") == 0) {
        /* Читаем лог-файл (или journalctl fallback) */
        static char log_buf[BUF_SIZE];
        log_buf[0] = '\0';
        int log_len = 0;

        FILE *fp = fopen(SEED_DEPLOY_LOG, "r");
        if (fp) {
            log_len = fread(log_buf, 1, sizeof(log_buf) - 1, fp);
            log_buf[log_len] = '\0';
            fclose(fp);
        } else {
            /* Fallback: journalctl */
            fp = popen("journalctl -u " SEED_SERVICE " --no-pager -n 100 --output=cat 2>/dev/null", "r");
            if (fp) {
                log_len = fread(log_buf, 1, sizeof(log_buf) - 1, fp);
                log_buf[log_len] = '\0';
                pclose(fp);
            }
        }

        /* Разбиваем на строки, берём последние 100 */
        char *lines[1024];
        int nlines = 0;
        char *p = log_buf;
        while (*p && nlines < 1024) {
            lines[nlines] = p;
            char *nl = strchr(p, '\n');
            if (nl) { *nl = '\0'; p = nl + 1; }
            else p += strlen(p);
            if (lines[nlines][0] != '\0')  /* пропускаем пустые */
                nlines++;
        }

        /* Берём последние 100 строк */
        int start = 0;
        if (nlines > 100) start = nlines - 100;

        int pos = 0;
        int max_len = (int)sizeof(response);
        pos += snprintf(response + pos, max_len - pos, "{\"lines\":[");
        CLAMP_POS(pos, max_len);
        for (int i = start; i < nlines; i++) {
            static char esc_line[1024];
            json_escape(lines[i], esc_line, sizeof(esc_line));
            if (i > start) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }
            pos += snprintf(response + pos, max_len - pos, "\"%s\"", esc_line);
            CLAMP_POS(pos, max_len);
        }
        pos += snprintf(response + pos, max_len - pos, "],\"count\":%d}", nlines - start);
        CLAMP_POS(pos, max_len);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /live.md — runtime state markdown */
    if (strcmp(req.path, "/live.md") == 0 && strcmp(req.method, "GET") == 0) {
        char live[BUF_SIZE] = "";
        FILE *fp = fopen(SEED_LIVE_MD, "r");
        if (fp) {
            int n = fread(live, 1, sizeof(live) - 1, fp);
            live[n] = '\0';
            fclose(fp);
        }
        send_text_response(fd, 200, "OK", live, "text/markdown; charset=utf-8");
        return;
    }

    /* POST /live.md — записать runtime state */
    if (strcmp(req.path, "/live.md") == 0 && strcmp(req.method, "POST") == 0) {
        mkdir(SEED_DIR, 0755);
        FILE *fp = fopen(SEED_LIVE_MD, "w");
        if (fp) {
            fwrite(req.body, 1, req.body_len, fp);
            fclose(fp);
            snprintf(response, sizeof(response), "{\"ok\":true,\"bytes\":%d}", req.body_len);
            send_response(fd, 200, "OK", response);
        } else {
            snprintf(response, sizeof(response), "{\"error\":\"cannot write %s: %s\"}", SEED_LIVE_MD, strerror(errno));
            send_response(fd, 500, "Error", response);
        }
        return;
    }

    /* GET /events — event log with timestamps */
    if (strncmp(req.path, "/events", 7) == 0 && strcmp(req.method, "GET") == 0) {
        /* Парсим ?since=<timestamp> */
        time_t since = 0;
        char *q = strchr(req.path, '?');
        if (q) {
            char *sp = strstr(q, "since=");
            if (sp) {
                char *endptr = NULL;
                since = (time_t)strtol(sp + 6, &endptr, 10);
                /* Проверяем, что строка содержит хотя бы одну цифру */
                if (endptr == sp + 6) {
                    send_response(fd, 400, "Bad Request",
                        "{\"error\":\"since must be a unix timestamp\"}");
                    return;
                }
            }
        }

        pthread_mutex_lock(&events_lock);

        /* Определяем начало кольцевого буфера */
        int start_idx = (events_count < MAX_EVENTS) ? 0 : events_head;
        int total = events_count;

        int pos = 0;
        int max_len = (int)sizeof(response);
        pos += snprintf(response + pos, max_len - pos, "{\"events\":[");
        CLAMP_POS(pos, max_len);

        int emitted = 0;
        for (int i = 0; i < total; i++) {
            int idx = (start_idx + i) % MAX_EVENTS;
            event_entry_t *e = &events_buf[idx];
            if (since > 0 && e->timestamp <= since) continue;
            /* Ограничиваем вывод: последние 100 если без since */
            if (since == 0 && total - i > 100) continue;

            static char esc_msg[EVENT_MSG_LEN * 2];
            json_escape(e->message, esc_msg, sizeof(esc_msg));
            if (emitted > 0) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }
            pos += snprintf(response + pos, max_len - pos,
                "{\"t\":%ld,\"event\":\"%s\"}", (long)e->timestamp, esc_msg);
            CLAMP_POS(pos, max_len);
            emitted++;
        }

        pthread_mutex_unlock(&events_lock);

        pos += snprintf(response + pos, max_len - pos, "],\"count\":%d}", emitted);
        CLAMP_POS(pos, max_len);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /mesh — mDNS discovery соседних Seed нод через avahi-browse */
    if (strcmp(req.path, "/mesh") == 0 && strcmp(req.method, "GET") == 0) {
        char hostname[256] = "unknown";
        gethostname(hostname, sizeof(hostname));

        /* Определяем собственный IP (первый не-loopback IPv4) */
        char self_ip[64] = "0.0.0.0";
        struct ifaddrs *ifap, *ifa;
        if (getifaddrs(&ifap) == 0) {
            for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr) continue;
                if (ifa->ifa_addr->sa_family != AF_INET) continue;
                if (ifa->ifa_flags & IFF_LOOPBACK) continue;
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &sa->sin_addr, self_ip, sizeof(self_ip));
                break;  /* берём первый подходящий */
            }
            freeifaddrs(ifap);
        }

        int pos = 0;
        int max_len = (int)sizeof(response);

        /* Self info — экранируем hostname и IP для безопасного JSON */
        static char esc_hostname[512];
        json_escape(hostname, esc_hostname, sizeof(esc_hostname));
        static char esc_self_ip[128];
        json_escape(self_ip, esc_self_ip, sizeof(esc_self_ip));
        pos += snprintf(response + pos, max_len - pos,
            "{\"self\":{\"name\":\"%s\",\"ip\":\"%s\",\"port\":%d},\"nodes\":[",
            esc_hostname, esc_self_ip, DEFAULT_PORT);
        CLAMP_POS(pos, max_len);

        /* Bug #3: avahi-browse -t только читает кэш и сразу выходит — ESP32 может
         * не попасть в кэш. Используем timeout 3s вместо -t, чтобы дождаться
         * реальных mDNS ответов из сети. */
        FILE *fp = popen("/usr/bin/timeout 3 /usr/bin/avahi-browse -r -p _seed._tcp 2>/dev/null", "r");
        int count = 0;
        time_t now = time(NULL);

        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                /* Нас интересуют только resolved записи (начинаются с '=') */
                if (line[0] != '=') continue;

                /* Формат: =;interface;protocol;name;type;domain;hostname;address;port;txt
                 * Разделитель — ';' */
                char *fields[10];
                int nfields = 0;
                char *p = line;
                while (nfields < 10) {
                    fields[nfields++] = p;
                    char *sep = strchr(p, ';');
                    if (!sep) break;
                    *sep = '\0';
                    p = sep + 1;
                }

                if (nfields < 9) continue;

                char *node_name = fields[3];   /* name */
                char *node_ip   = fields[7];   /* address */
                char *node_port = fields[8];    /* port */

                /* Trim trailing whitespace from port (may have \n) */
                int plen = strlen(node_port);
                while (plen > 0 && (node_port[plen-1] == '\n' || node_port[plen-1] == '\r' || node_port[plen-1] == ' '))
                    node_port[--plen] = '\0';

                /* Фильтруем себя — по hostname (надёжнее чем IP, т.к. нода видна на нескольких интерфейсах) */
                if (strcmp(node_name, hostname) == 0) continue;
                /* Пропускаем IPv6 (оставляем только IPv4) */
                if (strchr(node_ip, ':') != NULL) continue;
                /* Пропускаем loopback и link-local */
                if (strncmp(node_ip, "127.", 4) == 0) continue;

                if (count > 0) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }

                /* Экранируем имя ноды и IP */
                static char esc_name[256];
                json_escape(node_name, esc_name, sizeof(esc_name));
                static char esc_ip[128];
                json_escape(node_ip, esc_ip, sizeof(esc_ip));

                pos += snprintf(response + pos, max_len - pos,
                    "{\"name\":\"%s\",\"ip\":\"%s\",\"port\":%d,\"last_seen\":%ld}",
                    esc_name, esc_ip, atoi(node_port), (long)now);
                CLAMP_POS(pos, max_len);
                count++;

                event_add("mesh: discovered node %s at %s", node_name, node_ip);
            }
            pclose(fp);
        }

        pos += snprintf(response + pos, max_len - pos, "],\"count\":%d}", count);
        CLAMP_POS(pos, max_len);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* ========== WireGuard API ========== */

    /* GET /wg/status — статус интерфейса wg0 */
    if (strcmp(req.path, "/wg/status") == 0 && strcmp(req.method, "GET") == 0) {
        /* Проверяем, существует ли интерфейс wg0 */
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("/sbin/ip link show wg0 2>/dev/null", "r");
        int iface_exists = 0;
        if (fp) {
            char tmp[256];
            if (fgets(tmp, sizeof(tmp), fp)) iface_exists = 1;
            pclose(fp);
        }
        signal(SIGCHLD, SIG_IGN);

        if (!iface_exists) {
            /* Интерфейс не существует — читаем pubkey из файла */
            char pubkey[64] = "";
            FILE *pkf = fopen(WG_PUBKEY_FILE, "r");
            if (pkf) {
                if (fgets(pubkey, sizeof(pubkey), pkf)) {
                    /* Убираем \n */
                    int plen = strlen(pubkey);
                    while (plen > 0 && (pubkey[plen-1] == '\n' || pubkey[plen-1] == '\r'))
                        pubkey[--plen] = '\0';
                }
                fclose(pkf);
            }
            char esc_pk[128];
            json_escape(pubkey, esc_pk, sizeof(esc_pk));
            snprintf(response, sizeof(response),
                "{\"interface\":\"wg0\",\"up\":false,\"public_key\":\"%s\"}", esc_pk);
            send_response(fd, 200, "OK", response);
            return;
        }

        /* Интерфейс существует — парсим wg show wg0 dump */
        signal(SIGCHLD, SIG_DFL);
        fp = popen("/usr/bin/wg show wg0 dump 2>/dev/null", "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"popen failed\"}");
            return;
        }

        char line[512] = "";
        int peers_count = 0;
        char wg_pubkey[64] = "";
        int wg_port = 0;

        /* Первая строка — интерфейс: private-key, public-key, listen-port, fwmark */
        if (fgets(line, sizeof(line), fp)) {
            char *fields[4];
            int nf = 0;
            char *p = line;
            while (nf < 4) {
                fields[nf++] = p;
                char *tab = strchr(p, '\t');
                if (!tab) break;
                *tab = '\0';
                p = tab + 1;
            }
            if (nf >= 3) {
                snprintf(wg_pubkey, sizeof(wg_pubkey), "%s", fields[1]);
                wg_port = atoi(fields[2]);
            }
        }
        /* Считаем оставшиеся строки — каждая это пир */
        while (fgets(line, sizeof(line), fp)) {
            if (line[0] != '\0' && line[0] != '\n') peers_count++;
        }
        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        /* Убираем trailing whitespace из pubkey */
        int pklen = strlen(wg_pubkey);
        while (pklen > 0 && (wg_pubkey[pklen-1] == '\n' || wg_pubkey[pklen-1] == '\r'))
            wg_pubkey[--pklen] = '\0';

        char esc_pk[128];
        json_escape(wg_pubkey, esc_pk, sizeof(esc_pk));
        snprintf(response, sizeof(response),
            "{\"interface\":\"wg0\",\"public_key\":\"%s\",\"listen_port\":%d,\"peers_count\":%d,\"up\":true}",
            esc_pk, wg_port, peers_count);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /wg/config — настроить интерфейс wg0 */
    if (strcmp(req.path, "/wg/config") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty body\"}");
            return;
        }

        /* Парсим address */
        char address[64] = "";
        {
            char *p = strstr(req.body, "\"address\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(address)) {
                            memcpy(address, p, end - p);
                            address[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим listen_port */
        int listen_port = 51820;
        {
            char *p = strstr(req.body, "\"listen_port\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    listen_port = atoi(p + 1);
                }
            }
        }

        /* Валидация address: разрешаем только [0-9./] */
        if (address[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"address required\"}");
            return;
        }
        for (int i = 0; address[i]; i++) {
            char c = address[i];
            if (!((c >= '0' && c <= '9') || c == '.' || c == '/')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid address format\"}");
                return;
            }
        }

        /* Валидация порта */
        if (listen_port < 1 || listen_port > 65535) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid port (1-65535)\"}");
            return;
        }

        /* Выполняем последовательность команд для настройки wg0 */
        char cmd[512];

        /* Создаём интерфейс (ошибку игнорируем — может уже существовать) */
        signal(SIGCHLD, SIG_DFL);
        system("/sbin/ip link add wg0 type wireguard 2>/dev/null");
        signal(SIGCHLD, SIG_IGN);

        /* Задаём приватный ключ и порт */
        snprintf(cmd, sizeof(cmd),
            "/usr/bin/wg set wg0 private-key %s listen-port %d",
            WG_PRIVKEY_FILE, listen_port);
        signal(SIGCHLD, SIG_DFL);
        int rc = system(cmd);
        signal(SIGCHLD, SIG_IGN);
        if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) {
            snprintf(response, sizeof(response),
                "{\"error\":\"wg set failed\",\"exit_code\":%d}", WEXITSTATUS(rc));
            send_response(fd, 500, "Error", response);
            return;
        }

        /* Сбрасываем и задаём адрес */
        signal(SIGCHLD, SIG_DFL);
        system("/sbin/ip addr flush dev wg0 2>/dev/null");
        signal(SIGCHLD, SIG_IGN);

        snprintf(cmd, sizeof(cmd), "/sbin/ip addr add %s dev wg0", address);
        signal(SIGCHLD, SIG_DFL);
        rc = system(cmd);
        signal(SIGCHLD, SIG_IGN);
        if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) {
            snprintf(response, sizeof(response),
                "{\"error\":\"ip addr add failed\",\"exit_code\":%d}", WEXITSTATUS(rc));
            send_response(fd, 500, "Error", response);
            return;
        }

        /* Поднимаем интерфейс */
        signal(SIGCHLD, SIG_DFL);
        system("/sbin/ip link set wg0 up");
        signal(SIGCHLD, SIG_IGN);

        event_add("wg: configured wg0 address=%s port=%d", address, listen_port);
        char esc_addr[128];
        json_escape(address, esc_addr, sizeof(esc_addr));
        snprintf(response, sizeof(response),
            "{\"ok\":true,\"address\":\"%s\",\"listen_port\":%d}", esc_addr, listen_port);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /wg/peer — добавить/обновить пира */
    if (strcmp(req.path, "/wg/peer") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty body\"}");
            return;
        }

        /* Парсим public_key */
        char peer_key[64] = "";
        {
            char *p = strstr(req.body, "\"public_key\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(peer_key)) {
                            memcpy(peer_key, p, end - p);
                            peer_key[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим allowed_ips */
        char allowed_ips[128] = "";
        {
            char *p = strstr(req.body, "\"allowed_ips\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(allowed_ips)) {
                            memcpy(allowed_ips, p, end - p);
                            allowed_ips[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим endpoint (опционально) */
        char endpoint[128] = "";
        {
            char *p = strstr(req.body, "\"endpoint\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(endpoint)) {
                            memcpy(endpoint, p, end - p);
                            endpoint[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим persistent_keepalive (опционально) */
        int keepalive = 0;
        {
            char *p = strstr(req.body, "\"persistent_keepalive\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    keepalive = atoi(p + 1);
                }
            }
        }

        /* Валидация public_key: ровно 44 символа, только [A-Za-z0-9+/=] */
        if (strlen(peer_key) != WG_KEY_LEN) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid public_key (must be 44 chars base64)\"}");
            return;
        }
        for (int i = 0; i < WG_KEY_LEN; i++) {
            char c = peer_key[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid public_key charset\"}");
                return;
            }
        }

        /* Валидация allowed_ips: разрешаем [0-9./,] (IPv4 CIDR, через запятую) */
        if (allowed_ips[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"allowed_ips required\"}");
            return;
        }
        for (int i = 0; allowed_ips[i]; i++) {
            char c = allowed_ips[i];
            if (!((c >= '0' && c <= '9') || c == '.' || c == '/' || c == ',')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid allowed_ips format (IPv4 CIDR only)\"}");
                return;
            }
        }

        /* Валидация endpoint: если задан, формат IP:port — разрешаем [0-9.:] */
        if (endpoint[0] != '\0') {
            for (int i = 0; endpoint[i]; i++) {
                char c = endpoint[i];
                if (!((c >= '0' && c <= '9') || c == '.' || c == ':')) {
                    send_response(fd, 400, "Bad Request", "{\"error\":\"invalid endpoint format (IP:port)\"}");
                    return;
                }
            }
            /* Должен содержать хотя бы один ':' (разделитель IP:port) */
            if (!strchr(endpoint, ':')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"endpoint must be IP:port\"}");
                return;
            }
        }

        /* Валидация keepalive */
        if (keepalive < 0 || keepalive > 65535) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid persistent_keepalive (0-65535)\"}");
            return;
        }

        /* Собираем команду */
        char cmd[512];
        int cpos = 0;
        int cmax = (int)sizeof(cmd);
        cpos += snprintf(cmd + cpos, cmax - cpos,
            "/usr/bin/wg set wg0 peer %s allowed-ips %s", peer_key, allowed_ips);
        if (cpos >= cmax) cpos = cmax - 1;

        if (endpoint[0] != '\0') {
            cpos += snprintf(cmd + cpos, cmax - cpos, " endpoint %s", endpoint);
            if (cpos >= cmax) cpos = cmax - 1;
        }
        if (keepalive > 0) {
            cpos += snprintf(cmd + cpos, cmax - cpos, " persistent-keepalive %d", keepalive);
            if (cpos >= cmax) cpos = cmax - 1;
        }

        signal(SIGCHLD, SIG_DFL);
        int rc = system(cmd);
        signal(SIGCHLD, SIG_IGN);
        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

        if (exit_code != 0) {
            snprintf(response, sizeof(response),
                "{\"error\":\"wg set peer failed\",\"exit_code\":%d}", exit_code);
            send_response(fd, 500, "Error", response);
            return;
        }

        event_add("wg: peer added %s allowed_ips=%s", peer_key, allowed_ips);
        char esc_pk[128];
        json_escape(peer_key, esc_pk, sizeof(esc_pk));
        snprintf(response, sizeof(response), "{\"ok\":true,\"public_key\":\"%s\"}", esc_pk);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* DELETE /wg/peer — удалить пира (body: raw public key) */
    if (strcmp(req.path, "/wg/peer") == 0 && strcmp(req.method, "DELETE") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty body (send public key)\"}");
            return;
        }

        /* Копируем и тримим ключ */
        char peer_key[64] = "";
        int klen = req.body_len < (int)sizeof(peer_key) - 1 ? req.body_len : (int)sizeof(peer_key) - 1;
        memcpy(peer_key, req.body, klen);
        peer_key[klen] = '\0';
        while (klen > 0 && (peer_key[klen-1] == '\n' || peer_key[klen-1] == '\r' || peer_key[klen-1] == ' '))
            peer_key[--klen] = '\0';

        /* Валидация: ровно 44 символа, только base64 */
        if (klen != WG_KEY_LEN) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid key (must be 44 chars base64)\"}");
            return;
        }
        for (int i = 0; i < WG_KEY_LEN; i++) {
            char c = peer_key[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid key charset\"}");
                return;
            }
        }

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "/usr/bin/wg set wg0 peer %s remove", peer_key);

        signal(SIGCHLD, SIG_DFL);
        int rc = system(cmd);
        signal(SIGCHLD, SIG_IGN);
        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

        if (exit_code != 0) {
            snprintf(response, sizeof(response),
                "{\"error\":\"wg peer remove failed\",\"exit_code\":%d}", exit_code);
            send_response(fd, 500, "Error", response);
            return;
        }

        event_add("wg: peer removed %s", peer_key);
        char esc_pk[128];
        json_escape(peer_key, esc_pk, sizeof(esc_pk));
        snprintf(response, sizeof(response), "{\"ok\":true,\"removed\":\"%s\"}", esc_pk);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /wg/peers — список пиров с деталями */
    if (strcmp(req.path, "/wg/peers") == 0 && strcmp(req.method, "GET") == 0) {
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("/usr/bin/wg show wg0 dump 2>/dev/null", "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"popen failed\"}");
            return;
        }

        int pos = 0;
        int max_len = (int)sizeof(response);
        pos += snprintf(response + pos, max_len - pos, "{\"peers\":[");
        CLAMP_POS(pos, max_len);

        char line[512];
        int count = 0;
        int first_line = 1;

        while (fgets(line, sizeof(line), fp)) {
            /* Убираем \n */
            int llen = strlen(line);
            while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
                line[--llen] = '\0';

            /* Первая строка — интерфейс, пропускаем */
            if (first_line) { first_line = 0; continue; }
            if (llen == 0) continue;

            /* Парсим tab-separated поля пира:
             * public-key, preshared-key, endpoint, allowed-ips,
             * latest-handshake, transfer-rx, transfer-tx, persistent-keepalive */
            char *fields[8];
            int nf = 0;
            char *p = line;
            while (nf < 8) {
                fields[nf++] = p;
                char *tab = strchr(p, '\t');
                if (!tab) break;
                *tab = '\0';
                p = tab + 1;
            }
            if (nf < 8) continue;

            char *pk        = fields[0];
            /* fields[1] = preshared-key (не выводим) */
            char *ep        = fields[2];
            char *aips      = fields[3];
            char *handshake = fields[4];
            char *rx        = fields[5];
            char *tx        = fields[6];
            char *ka        = fields[7];

            if (count > 0) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }

            /* Экранируем строки для JSON */
            char esc_pk[128], esc_ep[128], esc_aips[256];
            json_escape(pk, esc_pk, sizeof(esc_pk));
            json_escape(strcmp(ep, "(none)") == 0 ? "" : ep, esc_ep, sizeof(esc_ep));
            json_escape(strcmp(aips, "(none)") == 0 ? "" : aips, esc_aips, sizeof(esc_aips));

            long long hs = atoll(handshake);
            long long trx = atoll(rx);
            long long ttx = atoll(tx);
            int pka = (strcmp(ka, "off") == 0 || strcmp(ka, "(none)") == 0) ? 0 : atoi(ka);

            pos += snprintf(response + pos, max_len - pos,
                "{\"public_key\":\"%s\",\"endpoint\":\"%s\",\"allowed_ips\":\"%s\","
                "\"latest_handshake\":%lld,\"transfer_rx\":%lld,\"transfer_tx\":%lld,"
                "\"persistent_keepalive\":%d}",
                esc_pk, esc_ep, esc_aips, hs, trx, ttx, pka);
            CLAMP_POS(pos, max_len);
            count++;
        }
        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        pos += snprintf(response + pos, max_len - pos, "],\"count\":%d}", count);
        CLAMP_POS(pos, max_len);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* ========== Firmware Self-Update API ========== */

    /* GET /firmware/version — версия, дата сборки, uptime */
    if (strcmp(req.path, "/firmware/version") == 0 && strcmp(req.method, "GET") == 0) {
        time_t uptime_sec = time(NULL) - server_start_time;
        snprintf(response, sizeof(response),
            "{\"version\":\"%s\",\"build_date\":\"%s\",\"build_time\":\"%s\",\"uptime_sec\":%ld}",
            FIRMWARE_VERSION, __DATE__, __TIME__, (long)uptime_sec);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /firmware/source — стриминг исходного кода server.c (plain text)
     * Файл ~112KB, не влезает в BUF_SIZE — шлём напрямую через write() */
    if (strcmp(req.path, "/firmware/source") == 0 && strcmp(req.method, "GET") == 0) {
        FILE *fp = fopen(FIRMWARE_SOURCE, "r");
        if (!fp) {
            snprintf(response, sizeof(response),
                "{\"error\":\"cannot open %s: %s\"}", FIRMWARE_SOURCE, strerror(errno));
            send_response(fd, 500, "Error", response);
            return;
        }

        /* Определяем размер файла */
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        /* Отправляем HTTP заголовки */
        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "\r\n",
            file_size);
        write(fd, header, hlen);

        /* Стримим содержимое файла чанками */
        char chunk[8192];
        size_t nread;
        while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
            size_t written = 0;
            while (written < nread) {
                ssize_t w = write(fd, chunk + written, nread - written);
                if (w <= 0) { fclose(fp); return; }
                written += w;
            }
        }
        fclose(fp);
        return;
    }

    /* POST /firmware/source — записать новый исходный код
     * Поддерживает large body (>MAX_BODY) через потоковое чтение из fd */
    if (strcmp(req.path, "/firmware/source") == 0 && strcmp(req.method, "POST") == 0) {
        FILE *fp = fopen(FIRMWARE_SOURCE, "w");
        if (!fp) {
            snprintf(response, sizeof(response),
                "{\"error\":\"cannot write %s: %s\"}", FIRMWARE_SOURCE, strerror(errno));
            send_response(fd, 500, "Error", response);
            return;
        }

        long total_written = 0;

        /* Записываем то, что уже прочитано в body[] (parse_request) */
        if (req.body_len > 0) {
            fwrite(req.body, 1, req.body_len, fp);
            total_written += req.body_len;
        }

        /* Если large body — дочитываем остаток из сокета */
        if (req.large_body_fd > 0 && req.body_remaining > 0) {
            char chunk[8192];
            int remaining = req.body_remaining;
            while (remaining > 0) {
                int to_read = remaining < (int)sizeof(chunk) ? remaining : (int)sizeof(chunk);
                int n = read(req.large_body_fd, chunk, to_read);
                if (n <= 0) break;
                fwrite(chunk, 1, n, fp);
                total_written += n;
                remaining -= n;
            }
        }

        fclose(fp);
        event_add("firmware source updated (%ld bytes)", total_written);
        snprintf(response, sizeof(response),
            "{\"ok\":true,\"bytes_written\":%ld}", total_written);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /firmware/build — скомпилировать новый бинарник */
    if (strcmp(req.path, "/firmware/build") == 0 && strcmp(req.method, "POST") == 0) {
        /* Проверяем что исходник существует */
        struct stat st;
        if (stat(FIRMWARE_SOURCE, &st) != 0) {
            send_response(fd, 400, "Bad Request",
                "{\"ok\":false,\"error\":\"source file not found\"}");
            return;
        }

        /* Компилируем с перенаправлением stderr в build.log */
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "gcc -O2 -o %s %s -lpthread > %s 2>&1",
            FIRMWARE_NEW, FIRMWARE_SOURCE, FIRMWARE_BUILD_LOG);

        /* SIGCHLD=SIG_IGN ломает system() — waitpid возвращает ECHILD.
         * Временно восстанавливаем обработчик на время сборки. */
        signal(SIGCHLD, SIG_DFL);
        int rc = system(cmd);
        signal(SIGCHLD, SIG_IGN);
        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

        /* Читаем build log */
        static char build_output[BUF_SIZE];
        build_output[0] = '\0';
        FILE *fp = fopen(FIRMWARE_BUILD_LOG, "r");
        if (fp) {
            int n = fread(build_output, 1, sizeof(build_output) - 1, fp);
            build_output[n] = '\0';
            fclose(fp);
        }

        if (exit_code == 0) {
            /* Успешная сборка — получаем размер бинарника */
            long bin_size = 0;
            if (stat(FIRMWARE_NEW, &st) == 0)
                bin_size = st.st_size;

            event_add("firmware build OK (%ld bytes)", bin_size);
            snprintf(response, sizeof(response),
                "{\"ok\":true,\"binary\":\"%s\",\"size_bytes\":%ld}",
                FIRMWARE_NEW, bin_size);
        } else {
            /* Ошибка сборки — возвращаем вывод компилятора */
            static char esc_output[BUF_SIZE * 2];
            json_escape(build_output, esc_output, sizeof(esc_output));
            event_add("firmware build FAILED (exit %d)", exit_code);
            snprintf(response, sizeof(response),
                "{\"ok\":false,\"error\":\"compilation failed\",\"output\":\"%s\"}",
                esc_output);
        }
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /firmware/build/logs — прочитать лог последней сборки */
    if (strcmp(req.path, "/firmware/build/logs") == 0 && strcmp(req.method, "GET") == 0) {
        static char log_buf[BUF_SIZE];
        log_buf[0] = '\0';
        FILE *fp = fopen(FIRMWARE_BUILD_LOG, "r");
        if (fp) {
            int n = fread(log_buf, 1, sizeof(log_buf) - 1, fp);
            log_buf[n] = '\0';
            fclose(fp);
        }
        send_text_response(fd, 200, "OK", log_buf, "text/plain; charset=utf-8");
        return;
    }

    /* POST /firmware/apply — применить новый бинарник с watchdog
     * 1. Проверяет что seed-new существует
     * 2. Бэкапит текущий бинарник
     * 3. Заменяет бинарник
     * 4. Форкает watchdog: ждёт 10с, проверяет /health, откатывает при неудаче
     * 5. Рестартит сервис */
    if (strcmp(req.path, "/firmware/apply") == 0 && strcmp(req.method, "POST") == 0) {
        /* Проверяем что новый бинарник существует */
        struct stat st;
        if (stat(FIRMWARE_NEW, &st) != 0) {
            send_response(fd, 400, "Bad Request",
                "{\"ok\":false,\"error\":\"no built binary found, run POST /firmware/build first\"}");
            return;
        }

        /* Бэкап текущего бинарника (cp, не rename — нужен fallback) */
        {
            FILE *src = fopen(FIRMWARE_BINARY, "rb");
            FILE *dst = fopen(FIRMWARE_BACKUP, "wb");
            if (!src || !dst) {
                if (src) fclose(src);
                if (dst) fclose(dst);
                send_response(fd, 500, "Error",
                    "{\"ok\":false,\"error\":\"failed to backup current binary\"}");
                return;
            }
            char cpbuf[8192];
            size_t n;
            while ((n = fread(cpbuf, 1, sizeof(cpbuf), src)) > 0)
                fwrite(cpbuf, 1, n, dst);
            fclose(src);
            fclose(dst);
            chmod(FIRMWARE_BACKUP, 0755);
        }

        /* Заменяем бинарник */
        if (rename(FIRMWARE_NEW, FIRMWARE_BINARY) != 0) {
            /* Откатываем бэкап */
            rename(FIRMWARE_BACKUP, FIRMWARE_BINARY);
            snprintf(response, sizeof(response),
                "{\"ok\":false,\"error\":\"failed to replace binary: %s\"}", strerror(errno));
            send_response(fd, 500, "Error", response);
            return;
        }
        chmod(FIRMWARE_BINARY, 0755);

        event_add("firmware apply: new binary installed, restarting with watchdog");

        /* Отправляем ответ ДО рестарта */
        send_response(fd, 200, "OK",
            "{\"ok\":true,\"warning\":\"service restarting, watchdog active for 10s\"}");

        /* Форкаем дочерний процесс для рестарта + watchdog */
        pid_t pid = fork();
        if (pid == 0) {
            /* Дочерний процесс: закрываем унаследованные fd */
            for (int i = 3; i < 1024; i++) close(i);

            /* Даём время на отправку ответа клиенту */
            sleep(1);

            /* Рестарт сервиса */
            system("systemctl restart seed");

            /* Ждём пока новый процесс стартанёт */
            sleep(10);

            /* Проверяем health нового бинарника */
            int rc = system("curl -s --max-time 5 http://localhost:8080/health > /dev/null 2>&1");
            if (rc != 0) {
                /* Новый бинарник не работает — откатываем! */
                char rollback[256];
                snprintf(rollback, sizeof(rollback),
                    "cp %s %s && systemctl restart seed",
                    FIRMWARE_BACKUP, FIRMWARE_BINARY);
                system(rollback);
            } else {
                /* Всё ок — удаляем бэкап */
                unlink(FIRMWARE_BACKUP);
            }
            _exit(0);
        }
        if (pid < 0) {
            /* fork() не удался — откатываем бинарник, так как watchdog не запустится */
            char rollback_cmd[256];
            snprintf(rollback_cmd, sizeof(rollback_cmd), "cp %s %s", FIRMWARE_BACKUP, FIRMWARE_BINARY);
            system(rollback_cmd);
            event_add("firmware apply: fork failed, rolled back");
            /* Ответ уже отправлен — логируем в stderr */
            fprintf(stderr, "FIRMWARE APPLY: fork() failed, binary rolled back\n");
        }
        /* Родительский процесс: fd будет закрыт main loop */
        return;
    }

    /* ========== Network Toolbox ========== */

    /* GET /net/scan — ARP сканирование локальной сети */
    if (strcmp(req.path, "/net/scan") == 0 && strcmp(req.method, "GET") == 0) {
        /* Определяем активный сетевой интерфейс: usb0 → wlan0 → eth0 */
        const char *ifaces[] = {"usb0", "wlan0", "eth0", NULL};
        const char *active_iface = NULL;
        for (int i = 0; ifaces[i]; i++) {
            char check[64];
            snprintf(check, sizeof(check), "/sys/class/net/%s", ifaces[i]);
            struct stat st;
            if (stat(check, &st) == 0) {
                active_iface = ifaces[i];
                break;
            }
        }
        if (!active_iface) active_iface = "eth0";

        /* Пробуем arp-scan, fallback на arp -an */
        int use_arp_scan = 0;
        {
            struct stat st;
            if (stat("/usr/sbin/arp-scan", &st) == 0) use_arp_scan = 1;
        }

        char cmd[256];
        if (use_arp_scan) {
            snprintf(cmd, sizeof(cmd),
                "/usr/sbin/arp-scan --localnet --interface=%s 2>/dev/null", active_iface);
        } else {
            snprintf(cmd, sizeof(cmd), "/usr/sbin/arp -an 2>/dev/null");
        }

        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"popen failed\"}");
            return;
        }

        /* Динамический буфер — может быть много хостов */
        int cap = 8192;
        char *buf = malloc(cap);
        if (!buf) {
            pclose(fp);
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int pos = 0;
        pos += snprintf(buf + pos, cap - pos, "{\"interface\":\"%s\",\"method\":\"%s\",\"hosts\":[",
            active_iface, use_arp_scan ? "arp-scan" : "arp");

        char line[512];
        int count = 0;

        if (use_arp_scan) {
            /* arp-scan формат: "10.55.0.2\taa:bb:cc:dd:ee:ff\tHostname" */
            while (fgets(line, sizeof(line), fp)) {
                /* Пропускаем заголовки и итоговые строки */
                if (line[0] == '\n' || line[0] == '\r') continue;
                if (strncmp(line, "Interface:", 10) == 0) continue;
                if (strncmp(line, "Starting", 8) == 0) continue;
                if (strncmp(line, "Ending", 6) == 0) continue;
                if (strchr(line, '\t') == NULL) continue;
                /* Первое поле: не IP — пропускаем */
                if (line[0] < '0' || line[0] > '9') continue;

                char ip[64] = "", mac[32] = "", hostname[256] = "";
                char *tab1 = strchr(line, '\t');
                if (!tab1) continue;
                int ip_len = tab1 - line;
                if (ip_len >= (int)sizeof(ip)) ip_len = sizeof(ip) - 1;
                memcpy(ip, line, ip_len);
                ip[ip_len] = '\0';

                char *tab2 = strchr(tab1 + 1, '\t');
                if (tab2) {
                    int mac_len = tab2 - (tab1 + 1);
                    if (mac_len >= (int)sizeof(mac)) mac_len = sizeof(mac) - 1;
                    memcpy(mac, tab1 + 1, mac_len);
                    mac[mac_len] = '\0';
                    /* hostname — остаток строки */
                    strncpy(hostname, tab2 + 1, sizeof(hostname) - 1);
                    hostname[sizeof(hostname) - 1] = '\0';
                } else {
                    strncpy(mac, tab1 + 1, sizeof(mac) - 1);
                    mac[sizeof(mac) - 1] = '\0';
                }
                /* Trim trailing whitespace */
                int l = strlen(hostname);
                while (l > 0 && (hostname[l-1] == '\n' || hostname[l-1] == '\r' || hostname[l-1] == ' '))
                    hostname[--l] = '\0';
                l = strlen(mac);
                while (l > 0 && (mac[l-1] == '\n' || mac[l-1] == '\r' || mac[l-1] == ' '))
                    mac[--l] = '\0';

                /* Расширяем буфер при необходимости */
                if (pos + 512 > cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) break;
                    buf = nb;
                }

                static char esc_ip[128], esc_mac[64], esc_host[512];
                json_escape(ip, esc_ip, sizeof(esc_ip));
                json_escape(mac, esc_mac, sizeof(esc_mac));
                json_escape(hostname, esc_host, sizeof(esc_host));

                if (count > 0) {
                    if (pos + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
                    buf[pos++] = ',';
                }
                pos += snprintf(buf + pos, cap - pos,
                    "{\"ip\":\"%s\",\"mac\":\"%s\",\"hostname\":\"%s\"}",
                    esc_ip, esc_mac, esc_host);
                count++;
            }
        } else {
            /* arp -an формат: "? (10.55.0.2) at aa:bb:cc:dd:ee:ff [ether] on usb0" */
            while (fgets(line, sizeof(line), fp)) {
                char ip[64] = "", mac[32] = "";
                /* Парсим IP из скобок */
                char *lp = strchr(line, '(');
                char *rp = strchr(line, ')');
                if (!lp || !rp || rp <= lp) continue;
                int ip_len = rp - lp - 1;
                if (ip_len >= (int)sizeof(ip)) ip_len = sizeof(ip) - 1;
                memcpy(ip, lp + 1, ip_len);
                ip[ip_len] = '\0';

                /* Парсим MAC после "at " */
                char *at_ptr = strstr(line, " at ");
                if (!at_ptr) continue;
                at_ptr += 4; /* skip " at " */
                /* MAC до следующего пробела */
                char *sp = strchr(at_ptr, ' ');
                int mac_len = sp ? (sp - at_ptr) : (int)strlen(at_ptr);
                if (mac_len >= (int)sizeof(mac)) mac_len = sizeof(mac) - 1;
                memcpy(mac, at_ptr, mac_len);
                mac[mac_len] = '\0';
                /* Trim */
                int l = strlen(mac);
                while (l > 0 && (mac[l-1] == '\n' || mac[l-1] == '\r'))
                    mac[--l] = '\0';

                /* Пропускаем incomplete entries */
                if (strcmp(mac, "<incomplete>") == 0) continue;

                if (pos + 512 > cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) break;
                    buf = nb;
                }

                static char esc_ip2[128], esc_mac2[64];
                json_escape(ip, esc_ip2, sizeof(esc_ip2));
                json_escape(mac, esc_mac2, sizeof(esc_mac2));

                if (count > 0) {
                    if (pos + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
                    buf[pos++] = ',';
                }
                pos += snprintf(buf + pos, cap - pos,
                    "{\"ip\":\"%s\",\"mac\":\"%s\",\"hostname\":\"\"}",
                    esc_ip2, esc_mac2);
                count++;
            }
        }

        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        if (pos + 64 > cap) {
            cap += 128;
            char *nb = realloc(buf, cap);
            if (nb) buf = nb;
        }
        pos += snprintf(buf + pos, cap - pos, "],\"count\":%d}", count);

        send_response(fd, 200, "OK", buf);
        free(buf);
        return;
    }

    /* GET /net/mdns — Browse всех mDNS сервисов в локальной сети */
    if (strcmp(req.path, "/net/mdns") == 0 && strcmp(req.method, "GET") == 0) {
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("/usr/bin/timeout 5 /usr/bin/avahi-browse -a -t -r -p 2>/dev/null", "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"avahi-browse failed\"}");
            return;
        }

        /* Дедупликация по name+type — до 256 уникальных сервисов */
        #define MDNS_MAX_SERVICES 256
        typedef struct {
            char name[128];
            char type[128];
            char host[256];
            char address[64];
            int port;
            char txt[512];
        } mdns_svc_t;

        mdns_svc_t *services = calloc(MDNS_MAX_SERVICES, sizeof(mdns_svc_t));
        if (!services) {
            pclose(fp);
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }
        int svc_count = 0;

        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            /* Только resolved записи (начинаются с '=') */
            if (line[0] != '=') continue;

            /* Формат: =;iface;protocol;name;type;domain;hostname;address;port;txt */
            char *fields[11];
            int nfields = 0;
            char *p = line;
            while (nfields < 11) {
                fields[nfields++] = p;
                char *sep = strchr(p, ';');
                if (!sep) break;
                *sep = '\0';
                p = sep + 1;
            }
            if (nfields < 9) continue;

            char *svc_name = fields[3];
            char *svc_type = fields[4];
            char *svc_host = fields[6];
            char *svc_addr = fields[7];
            char *svc_port = fields[8];
            char *svc_txt = (nfields >= 10) ? fields[9] : "";

            /* Trim trailing whitespace */
            int l = strlen(svc_txt);
            while (l > 0 && (svc_txt[l-1] == '\n' || svc_txt[l-1] == '\r'))
                svc_txt[--l] = '\0';

            /* Дедупликация по name+type (IPv4/IPv6 дублируются) */
            int dup = 0;
            for (int i = 0; i < svc_count; i++) {
                if (strcmp(services[i].name, svc_name) == 0 &&
                    strcmp(services[i].type, svc_type) == 0) {
                    /* Если текущий — IPv4 (без ':'), предпочитаем его */
                    if (strchr(svc_addr, ':') == NULL) {
                        strncpy(services[i].address, svc_addr, sizeof(services[i].address) - 1);
                        strncpy(services[i].host, svc_host, sizeof(services[i].host) - 1);
                        services[i].port = atoi(svc_port);
                    }
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
            if (svc_count >= MDNS_MAX_SERVICES) continue;

            strncpy(services[svc_count].name, svc_name, sizeof(services[svc_count].name) - 1);
            strncpy(services[svc_count].type, svc_type, sizeof(services[svc_count].type) - 1);
            strncpy(services[svc_count].host, svc_host, sizeof(services[svc_count].host) - 1);
            strncpy(services[svc_count].address, svc_addr, sizeof(services[svc_count].address) - 1);
            services[svc_count].port = atoi(svc_port);
            strncpy(services[svc_count].txt, svc_txt, sizeof(services[svc_count].txt) - 1);
            svc_count++;
        }

        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        /* Формируем JSON */
        int cap = 4096 + svc_count * 1024;
        char *buf = malloc(cap);
        if (!buf) {
            free(services);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int bpos = 0;
        bpos += snprintf(buf + bpos, cap - bpos, "{\"services\":[");

        static char esc_name[256], esc_type[256], esc_host[512];
        static char esc_addr[128], esc_txt[1024];

        for (int i = 0; i < svc_count; i++) {
            json_escape(services[i].name, esc_name, sizeof(esc_name));
            json_escape(services[i].type, esc_type, sizeof(esc_type));
            json_escape(services[i].host, esc_host, sizeof(esc_host));
            json_escape(services[i].address, esc_addr, sizeof(esc_addr));
            json_escape(services[i].txt, esc_txt, sizeof(esc_txt));

            /* Расширяем буфер при необходимости (включая запятую) */
            if (bpos + 1024 > cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }

            if (i > 0) buf[bpos++] = ',';
            bpos += snprintf(buf + bpos, cap - bpos,
                "{\"name\":\"%s\",\"type\":\"%s\",\"host\":\"%s\","
                "\"address\":\"%s\",\"port\":%d,\"txt\":\"%s\"}",
                esc_name, esc_type, esc_host,
                esc_addr, services[i].port, esc_txt);
        }

        bpos += snprintf(buf + bpos, cap - bpos, "],\"count\":%d}", svc_count);

        send_response(fd, 200, "OK", buf);
        free(buf);
        free(services);
        return;
    }

    /* GET /net/wifi — сканирование WiFi сетей */
    if (strcmp(req.path, "/net/wifi") == 0 && strcmp(req.method, "GET") == 0) {
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("/sbin/iwlist wlan0 scan 2>/dev/null", "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"iwlist scan failed\"}");
            return;
        }

        /* Парсим блоки Cell XX */
        typedef struct {
            char ssid[128];
            int channel;
            int signal_dbm;
            char encryption[32];
        } wifi_net_t;

        #define WIFI_MAX_NETS 128
        wifi_net_t *nets = calloc(WIFI_MAX_NETS, sizeof(wifi_net_t));
        if (!nets) {
            pclose(fp);
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }
        int net_count = 0;
        int cur = -1; /* текущий индекс сети */

        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            /* Новая ячейка: "Cell XX - Address: ..." */
            if (strstr(line, "Cell ") && strstr(line, "Address:")) {
                if (net_count < WIFI_MAX_NETS) {
                    cur = net_count++;
                    nets[cur].ssid[0] = '\0';
                    nets[cur].channel = 0;
                    nets[cur].signal_dbm = -100;
                    strcpy(nets[cur].encryption, "open");
                }
                continue;
            }
            if (cur < 0) continue;

            /* Channel:XX */
            char *ch_ptr = strstr(line, "Channel:");
            if (ch_ptr) {
                nets[cur].channel = atoi(ch_ptr + 8);
                continue;
            }

            /* ESSID:"name" */
            char *essid_ptr = strstr(line, "ESSID:\"");
            if (essid_ptr) {
                essid_ptr += 7; /* skip ESSID:" */
                char *end = strchr(essid_ptr, '"');
                if (end) {
                    int len = end - essid_ptr;
                    if (len >= (int)sizeof(nets[cur].ssid)) len = sizeof(nets[cur].ssid) - 1;
                    memcpy(nets[cur].ssid, essid_ptr, len);
                    nets[cur].ssid[len] = '\0';
                }
                continue;
            }

            /* Signal level=-XX dBm */
            char *sig_ptr = strstr(line, "Signal level=");
            if (sig_ptr) {
                nets[cur].signal_dbm = atoi(sig_ptr + 13);
                continue;
            }
            /* Альтернативный формат: Signal level:XX/100 */
            sig_ptr = strstr(line, "Signal level:");
            if (sig_ptr && !strstr(line, "Signal level=")) {
                nets[cur].signal_dbm = atoi(sig_ptr + 13);
                continue;
            }

            /* Encryption key:on */
            char *enc_ptr = strstr(line, "Encryption key:on");
            if (enc_ptr) {
                strcpy(nets[cur].encryption, "on");
                continue;
            }

            /* IE: IEEE 802.11i/WPA2 Version 1 или IE: WPA Version 1 */
            char *ie_ptr = strstr(line, "IE:");
            if (ie_ptr && cur >= 0) {
                if (strstr(line, "WPA2")) {
                    strcpy(nets[cur].encryption, "WPA2");
                } else if (strstr(line, "WPA")) {
                    /* Не перезаписываем WPA2 если уже установлен */
                    if (strcmp(nets[cur].encryption, "WPA2") != 0)
                        strcpy(nets[cur].encryption, "WPA");
                }
                continue;
            }
        }

        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        /* Сортировка по signal_dbm (убывание — сильнейшие первые) */
        for (int i = 0; i < net_count - 1; i++) {
            for (int j = i + 1; j < net_count; j++) {
                if (nets[j].signal_dbm > nets[i].signal_dbm) {
                    wifi_net_t tmp = nets[i];
                    nets[i] = nets[j];
                    nets[j] = tmp;
                }
            }
        }

        /* Формируем JSON */
        int cap = 4096 + net_count * 256;
        char *buf = malloc(cap);
        if (!buf) {
            free(nets);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int bpos = 0;
        int visible = 0; /* счётчик видимых (не скрытых) сетей */
        bpos += snprintf(buf + bpos, cap - bpos, "{\"networks\":[");

        static char esc_ssid[256], esc_enc[64];

        for (int i = 0; i < net_count; i++) {
            /* Пропускаем сети без SSID (скрытые) */
            if (nets[i].ssid[0] == '\0') continue;

            json_escape(nets[i].ssid, esc_ssid, sizeof(esc_ssid));
            json_escape(nets[i].encryption, esc_enc, sizeof(esc_enc));

            if (bpos + 256 > cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }

            if (visible > 0) buf[bpos++] = ',';
            bpos += snprintf(buf + bpos, cap - bpos,
                "{\"ssid\":\"%s\",\"channel\":%d,\"signal_dbm\":%d,\"encryption\":\"%s\"}",
                esc_ssid, nets[i].channel, nets[i].signal_dbm, esc_enc);
            visible++;
        }

        bpos += snprintf(buf + bpos, cap - bpos, "],\"count\":%d}", visible);

        send_response(fd, 200, "OK", buf);
        free(buf);
        free(nets);
        return;
    }

    /* ========== Network Tools (ping, probe, interfaces, ports) ========== */

    /* GET /net/ping/{host} — Ping хоста */
    if (strncmp(req.path, "/net/ping/", 10) == 0 && strcmp(req.method, "GET") == 0) {
        const char *host = req.path + 10;

        /* Проверяем что host не пустой */
        if (host[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"host is required\"}");
            return;
        }

        /* Длина host: max 253 символа (DNS limit) */
        int host_len = strlen(host);
        if (host_len > 253) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"host too long (max 253)\"}");
            return;
        }

        /* Input validation: только [a-zA-Z0-9._-] — shell injection protection */
        for (int i = 0; i < host_len; i++) {
            char c = host[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid host: only [a-zA-Z0-9._-] allowed\"}");
                return;
            }
        }

        char cmd[512];
        snprintf(cmd, sizeof(cmd), "ping -c 3 -W 2 %s 2>/dev/null", host);

        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"popen failed\"}");
            return;
        }

        char line[512];
        int packets_sent = 0, packets_received = 0;
        double loss_percent = 100.0;
        double rtt_min = 0, rtt_avg = 0, rtt_max = 0;
        int got_stats = 0;

        while (fgets(line, sizeof(line), fp)) {
            /* "3 packets transmitted, 3 received, 0% packet loss, time ..." */
            if (strstr(line, "packets transmitted")) {
                sscanf(line, "%d packets transmitted, %d received", &packets_sent, &packets_received);
                char *loss_ptr = strstr(line, "% packet loss");
                if (loss_ptr) {
                    /* Ищем число перед '% packet loss' */
                    char *p = loss_ptr - 1;
                    while (p > line && ((*p >= '0' && *p <= '9') || *p == '.')) p--;
                    loss_percent = atof(p + 1);
                }
            }
            /* "rtt min/avg/max/mdev = 0.123/0.456/0.789/0.012 ms" */
            if (strstr(line, "min/avg/max")) {
                char *eq = strchr(line, '=');
                if (eq) {
                    sscanf(eq + 1, " %lf/%lf/%lf", &rtt_min, &rtt_avg, &rtt_max);
                    got_stats = 1;
                }
            }
        }

        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        static char esc_host_ping[512];
        json_escape(host, esc_host_ping, sizeof(esc_host_ping));

        if (got_stats) {
            snprintf(response, sizeof(response),
                "{\"host\":\"%s\",\"packets_sent\":%d,\"packets_received\":%d,"
                "\"loss_percent\":%.1f,\"rtt_min\":%.3f,\"rtt_avg\":%.3f,\"rtt_max\":%.3f}",
                esc_host_ping, packets_sent, packets_received,
                loss_percent, rtt_min, rtt_avg, rtt_max);
        } else {
            snprintf(response, sizeof(response),
                "{\"host\":\"%s\",\"packets_sent\":%d,\"packets_received\":%d,"
                "\"loss_percent\":%.1f,\"rtt_min\":0,\"rtt_avg\":0,\"rtt_max\":0}",
                esc_host_ping, packets_sent, packets_received, loss_percent);
        }

        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /net/probe/{host}/{port} — HTTP probe сервиса */
    if (strncmp(req.path, "/net/probe/", 11) == 0 && strcmp(req.method, "GET") == 0) {
        const char *rest = req.path + 11;

        /* Парсим host и port из пути: host/port */
        char probe_host[256] = "";
        int probe_port = 0;

        const char *last_slash = strrchr(rest, '/');
        if (!last_slash || last_slash == rest) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"format: /net/probe/{host}/{port}\"}");
            return;
        }

        int ph_len = last_slash - rest;
        if (ph_len <= 0 || ph_len >= (int)sizeof(probe_host)) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"host too long or empty\"}");
            return;
        }
        memcpy(probe_host, rest, ph_len);
        probe_host[ph_len] = '\0';

        const char *port_str = last_slash + 1;
        if (port_str[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"port is required\"}");
            return;
        }
        /* Digit-only validation — atoi("1;rm -rf /") returns 1, so check chars first */
        for (int i = 0; port_str[i]; i++) {
            if (port_str[i] < '0' || port_str[i] > '9') {
                send_response(fd, 400, "Bad Request", "{\"error\":\"port must be numeric\"}");
                return;
            }
        }
        probe_port = atoi(port_str);
        if (probe_port < 1 || probe_port > 65535) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"port must be 1-65535\"}");
            return;
        }

        /* Длина host: max 253 символа (DNS limit) */
        if (ph_len > 253) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"host too long (max 253)\"}");
            return;
        }

        /* Input validation на host: только [a-zA-Z0-9._-] */
        for (int i = 0; i < ph_len; i++) {
            char c = probe_host[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid host: only [a-zA-Z0-9._-] allowed\"}");
                return;
            }
        }

        /* Curl для status, latency, size */
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "curl -s -o /dev/null -w '%%{http_code}\\n%%{time_total}\\n%%{size_download}\\n' -m 5 http://%s:%d/ 2>/dev/null",
            probe_host, probe_port);

        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"popen failed\"}");
            return;
        }

        char line[512];
        int status_code = 0;
        double latency_s = 0;
        long size_bytes = 0;
        int line_num = 0;

        while (fgets(line, sizeof(line), fp)) {
            /* Trim newline */
            int l = strlen(line);
            while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';

            if (line_num == 0) status_code = atoi(line);
            else if (line_num == 1) latency_s = atof(line);
            else if (line_num == 2) size_bytes = atol(line);
            line_num++;
        }
        pclose(fp);

        /* Если status_code == 0 — хост не ответил */
        if (status_code == 0) {
            signal(SIGCHLD, SIG_IGN);
            static char esc_ph[512];
            json_escape(probe_host, esc_ph, sizeof(esc_ph));
            snprintf(response, sizeof(response),
                "{\"host\":\"%s\",\"port\":%d,\"status_code\":0,\"error\":\"connection refused\"}",
                esc_ph, probe_port);
            send_response(fd, 200, "OK", response);
            return;
        }

        /* Curl для headers */
        snprintf(cmd, sizeof(cmd),
            "curl -s -I -m 5 http://%s:%d/ 2>/dev/null", probe_host, probe_port);

        fp = popen(cmd, "r");

        /* Динамический буфер для headers JSON */
        int cap = 4096;
        char *buf = malloc(cap);
        if (!buf) {
            if (fp) pclose(fp);
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int bpos = 0;
        static char esc_ph2[512];
        json_escape(probe_host, esc_ph2, sizeof(esc_ph2));

        bpos += snprintf(buf + bpos, cap - bpos,
            "{\"host\":\"%s\",\"port\":%d,\"status_code\":%d,"
            "\"latency_ms\":%.1f,\"size_bytes\":%ld,\"headers\":{",
            esc_ph2, probe_port, status_code,
            latency_s * 1000.0, size_bytes);

        int hdr_count = 0;
        if (fp) {
            while (fgets(line, sizeof(line), fp)) {
                /* Trim trailing whitespace */
                int l = strlen(line);
                while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r' || line[l-1] == ' '))
                    line[--l] = '\0';

                /* Пропускаем строку статуса HTTP/1.x ... */
                if (strncmp(line, "HTTP/", 5) == 0) continue;
                /* Пропускаем пустые строки */
                if (line[0] == '\0') continue;

                /* Парсим "Header-Name: value" */
                char *colon = strchr(line, ':');
                if (!colon) continue;
                *colon = '\0';
                char *val = colon + 1;
                while (*val == ' ') val++;

                static char esc_key[256], esc_val[1024];
                json_escape(line, esc_key, sizeof(esc_key));
                json_escape(val, esc_val, sizeof(esc_val));

                /* Расширяем буфер при необходимости */
                int needed = strlen(esc_key) + strlen(esc_val) + 16;
                if (bpos + needed > cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) break;
                    buf = nb;
                }

                if (hdr_count > 0) {
                    if (bpos + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
                    buf[bpos++] = ',';
                }
                bpos += snprintf(buf + bpos, cap - bpos,
                    "\"%s\":\"%s\"", esc_key, esc_val);
                hdr_count++;
            }
            pclose(fp);
        }
        signal(SIGCHLD, SIG_IGN);

        /* Закрываем JSON */
        if (bpos + 4 > cap) {
            cap += 16;
            char *nb = realloc(buf, cap);
            if (nb) buf = nb;
        }
        bpos += snprintf(buf + bpos, cap - bpos, "}}");

        send_response(fd, 200, "OK", buf);
        free(buf);
        return;
    }

    /* GET /net/interfaces — Список сетевых интерфейсов */
    if (strcmp(req.path, "/net/interfaces") == 0 && strcmp(req.method, "GET") == 0) {
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("ip -j addr show 2>/dev/null", "r");

        /* Флаг: удалось ли использовать ip -j */
        int use_json = 0;
        char *ip_json_buf = NULL;

        if (fp) {
            /* Читаем весь вывод ip -j addr show */
            int jcap = 16384;
            ip_json_buf = malloc(jcap);
            if (ip_json_buf) {
                int jpos = 0;
                char chunk[4096];
                while (fgets(chunk, sizeof(chunk), fp)) {
                    int clen = strlen(chunk);
                    if (jpos + clen >= jcap) {
                        jcap *= 2;
                        char *nb = realloc(ip_json_buf, jcap);
                        if (!nb) break;
                        ip_json_buf = nb;
                    }
                    memcpy(ip_json_buf + jpos, chunk, clen);
                    jpos += clen;
                }
                ip_json_buf[jpos] = '\0';
                /* Проверяем что вывод начинается с '[' — валидный JSON */
                if (jpos > 0 && ip_json_buf[0] == '[') {
                    use_json = 1;
                }
            }
            pclose(fp);
        }

        /* Результирующий JSON с интерфейсами */
        int cap = 8192;
        char *buf = malloc(cap);
        if (!buf) {
            if (ip_json_buf) free(ip_json_buf);
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int bpos = 0;
        bpos += snprintf(buf + bpos, cap - bpos, "{\"interfaces\":[");
        int iface_count = 0;

        if (use_json && ip_json_buf) {
            /* Парсим ip -j вручную — ищем ifname, operstate, address, addr_info */
            /* Формат: [{"ifname":"eth0","operstate":"UP","address":"aa:bb:...",
               "addr_info":[{"local":"10.0.0.1","prefixlen":24}]},...] */
            char *p = ip_json_buf;

            while ((p = strstr(p, "\"ifname\"")) != NULL) {
                char ifname[64] = "", state[32] = "", mac[32] = "";
                char ipaddr[64] = "";
                int mask = 0;

                /* Извлекаем ifname */
                char *q = strchr(p + 8, '"');
                if (!q) break;
                q++; /* начало значения */
                char *end = strchr(q, '"');
                if (!end) break;
                int len = end - q;
                if (len >= (int)sizeof(ifname)) len = sizeof(ifname) - 1;
                memcpy(ifname, q, len);
                ifname[len] = '\0';
                p = end + 1;

                /* operstate */
                char *os = strstr(p, "\"operstate\"");
                /* Ограничиваем поиск следующим ifname */
                char *next_if = strstr(p, "\"ifname\"");
                if (os && (!next_if || os < next_if)) {
                    q = strchr(os + 11, '"');
                    if (q) {
                        q++;
                        end = strchr(q, '"');
                        if (end) {
                            len = end - q;
                            if (len >= (int)sizeof(state)) len = sizeof(state) - 1;
                            memcpy(state, q, len);
                            state[len] = '\0';
                        }
                    }
                }

                /* address (MAC) */
                char *ad = strstr(p, "\"address\"");
                if (ad && (!next_if || ad < next_if)) {
                    q = strchr(ad + 9, '"');
                    if (q) {
                        q++;
                        end = strchr(q, '"');
                        if (end) {
                            len = end - q;
                            if (len >= (int)sizeof(mac)) len = sizeof(mac) - 1;
                            memcpy(mac, q, len);
                            mac[len] = '\0';
                        }
                    }
                }

                /* addr_info → first "local" with "family":"inet" */
                char *ai = strstr(p, "\"addr_info\"");
                if (ai && (!next_if || ai < next_if)) {
                    /* Ищем "family":"inet" и затем "local" */
                    char *inet = strstr(ai, "\"inet\"");
                    if (inet && (!next_if || inet < next_if)) {
                        /* Ищем "local" перед или после "inet" в рамках addr_info */
                        char *local = strstr(ai, "\"local\"");
                        if (local && (!next_if || local < next_if)) {
                            q = strchr(local + 7, '"');
                            if (q) {
                                q++;
                                end = strchr(q, '"');
                                if (end) {
                                    len = end - q;
                                    if (len >= (int)sizeof(ipaddr)) len = sizeof(ipaddr) - 1;
                                    memcpy(ipaddr, q, len);
                                    ipaddr[len] = '\0';
                                }
                            }
                        }
                        /* prefixlen */
                        char *pl = strstr(ai, "\"prefixlen\"");
                        if (pl && (!next_if || pl < next_if)) {
                            pl += 11;
                            while (*pl && (*pl == ':' || *pl == ' ')) pl++;
                            mask = atoi(pl);
                        }
                    }
                }

                /* Читаем rx/tx bytes из sysfs */
                long long rx_bytes = 0, tx_bytes = 0;
                char stat_path[128];
                snprintf(stat_path, sizeof(stat_path), "/sys/class/net/%s/statistics/rx_bytes", ifname);
                FILE *sf = fopen(stat_path, "r");
                if (sf) { fscanf(sf, "%lld", &rx_bytes); fclose(sf); }
                snprintf(stat_path, sizeof(stat_path), "/sys/class/net/%s/statistics/tx_bytes", ifname);
                sf = fopen(stat_path, "r");
                if (sf) { fscanf(sf, "%lld", &tx_bytes); fclose(sf); }

                /* Расширяем буфер */
                if (bpos + 512 > cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) break;
                    buf = nb;
                }

                static char esc_ifn[128], esc_st[64], esc_mc[64], esc_ip[128];
                json_escape(ifname, esc_ifn, sizeof(esc_ifn));
                json_escape(state, esc_st, sizeof(esc_st));
                json_escape(mac, esc_mc, sizeof(esc_mc));
                json_escape(ipaddr, esc_ip, sizeof(esc_ip));

                if (iface_count > 0) {
                    if (bpos + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
                    buf[bpos++] = ',';
                }
                bpos += snprintf(buf + bpos, cap - bpos,
                    "{\"name\":\"%s\",\"state\":\"%s\",\"mac\":\"%s\","
                    "\"ip\":\"%s\",\"mask\":%d,\"rx_bytes\":%lld,\"tx_bytes\":%lld}",
                    esc_ifn, esc_st, esc_mc, esc_ip, mask, rx_bytes, tx_bytes);
                iface_count++;
            }
        } else {
            /* Fallback: ip addr show (текстовый формат) */
            fp = popen("ip addr show 2>/dev/null", "r");
            if (fp) {
                char ifname[64] = "", state[32] = "", mac_addr[32] = "", ip_addr[64] = "";
                int cur_mask = 0;
                int in_iface = 0;

                char line[512];
                while (fgets(line, sizeof(line), fp)) {
                    /* Новый интерфейс: "2: eth0: <...> state UP ..." */
                    if (line[0] >= '0' && line[0] <= '9') {
                        /* Сохраняем предыдущий интерфейс */
                        if (in_iface && ifname[0]) {
                            long long rx_bytes = 0, tx_bytes = 0;
                            char stat_path[128];
                            snprintf(stat_path, sizeof(stat_path), "/sys/class/net/%s/statistics/rx_bytes", ifname);
                            FILE *sf = fopen(stat_path, "r");
                            if (sf) { fscanf(sf, "%lld", &rx_bytes); fclose(sf); }
                            snprintf(stat_path, sizeof(stat_path), "/sys/class/net/%s/statistics/tx_bytes", ifname);
                            sf = fopen(stat_path, "r");
                            if (sf) { fscanf(sf, "%lld", &tx_bytes); fclose(sf); }

                            if (bpos + 512 > cap) {
                                cap *= 2;
                                char *nb = realloc(buf, cap);
                                if (!nb) break;
                                buf = nb;
                            }

                            static char e_ifn[128], e_st[64], e_mc[64], e_ip2[128];
                            json_escape(ifname, e_ifn, sizeof(e_ifn));
                            json_escape(state, e_st, sizeof(e_st));
                            json_escape(mac_addr, e_mc, sizeof(e_mc));
                            json_escape(ip_addr, e_ip2, sizeof(e_ip2));

                            if (iface_count > 0) {
                                if (bpos + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
                                buf[bpos++] = ',';
                            }
                            bpos += snprintf(buf + bpos, cap - bpos,
                                "{\"name\":\"%s\",\"state\":\"%s\",\"mac\":\"%s\","
                                "\"ip\":\"%s\",\"mask\":%d,\"rx_bytes\":%lld,\"tx_bytes\":%lld}",
                                e_ifn, e_st, e_mc, e_ip2, cur_mask, rx_bytes, tx_bytes);
                            iface_count++;
                        }

                        /* Парсим имя интерфейса и state */
                        ifname[0] = '\0'; state[0] = '\0'; mac_addr[0] = '\0'; ip_addr[0] = '\0'; cur_mask = 0;
                        char *colon = strchr(line, ':');
                        if (colon) {
                            colon++; /* skip first ':' */
                            while (*colon == ' ') colon++;
                            char *colon2 = strchr(colon, ':');
                            if (colon2) {
                                int nlen = colon2 - colon;
                                if (nlen >= (int)sizeof(ifname)) nlen = sizeof(ifname) - 1;
                                memcpy(ifname, colon, nlen);
                                ifname[nlen] = '\0';
                                /* Trim trailing spaces */
                                int tl = strlen(ifname);
                                while (tl > 0 && ifname[tl-1] == ' ') ifname[--tl] = '\0';
                            }
                        }
                        char *st_ptr = strstr(line, "state ");
                        if (st_ptr) {
                            st_ptr += 6;
                            char *sp = strchr(st_ptr, ' ');
                            int slen = sp ? (sp - st_ptr) : (int)strlen(st_ptr);
                            if (slen >= (int)sizeof(state)) slen = sizeof(state) - 1;
                            memcpy(state, st_ptr, slen);
                            state[slen] = '\0';
                            /* Trim newline */
                            int tl = strlen(state);
                            while (tl > 0 && (state[tl-1] == '\n' || state[tl-1] == '\r')) state[--tl] = '\0';
                        }
                        in_iface = 1;
                        continue;
                    }

                    if (!in_iface) continue;

                    /* "    link/ether aa:bb:cc:dd:ee:ff brd ..." */
                    char *le = strstr(line, "link/ether ");
                    if (le) {
                        le += 11;
                        char *sp = strchr(le, ' ');
                        int mlen = sp ? (sp - le) : (int)strlen(le);
                        if (mlen >= (int)sizeof(mac_addr)) mlen = sizeof(mac_addr) - 1;
                        memcpy(mac_addr, le, mlen);
                        mac_addr[mlen] = '\0';
                        continue;
                    }

                    /* "    inet 10.0.0.1/24 ..." */
                    char *inet = strstr(line, "inet ");
                    if (inet && !strstr(line, "inet6 ")) {
                        inet += 5;
                        while (*inet == ' ') inet++;
                        char *sl = strchr(inet, '/');
                        if (sl) {
                            int ilen = sl - inet;
                            if (ilen >= (int)sizeof(ip_addr)) ilen = sizeof(ip_addr) - 1;
                            memcpy(ip_addr, inet, ilen);
                            ip_addr[ilen] = '\0';
                            cur_mask = atoi(sl + 1);
                        }
                        continue;
                    }
                }

                /* Последний интерфейс */
                if (in_iface && ifname[0]) {
                    long long rx_bytes = 0, tx_bytes = 0;
                    char stat_path[128];
                    snprintf(stat_path, sizeof(stat_path), "/sys/class/net/%s/statistics/rx_bytes", ifname);
                    FILE *sf = fopen(stat_path, "r");
                    if (sf) { fscanf(sf, "%lld", &rx_bytes); fclose(sf); }
                    snprintf(stat_path, sizeof(stat_path), "/sys/class/net/%s/statistics/tx_bytes", ifname);
                    sf = fopen(stat_path, "r");
                    if (sf) { fscanf(sf, "%lld", &tx_bytes); fclose(sf); }

                    if (bpos + 512 > cap) {
                        cap *= 2;
                        char *nb = realloc(buf, cap);
                        if (!nb) { /* no-op, snprintf will truncate */ }
                        else buf = nb;
                    }

                    static char e_ifn2[128], e_st2[64], e_mc2[64], e_ip3[128];
                    json_escape(ifname, e_ifn2, sizeof(e_ifn2));
                    json_escape(state, e_st2, sizeof(e_st2));
                    json_escape(mac_addr, e_mc2, sizeof(e_mc2));
                    json_escape(ip_addr, e_ip3, sizeof(e_ip3));

                    if (iface_count > 0) {
                        if (bpos + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) { /* truncate */ } else buf = nb; }
                        buf[bpos++] = ',';
                    }
                    bpos += snprintf(buf + bpos, cap - bpos,
                        "{\"name\":\"%s\",\"state\":\"%s\",\"mac\":\"%s\","
                        "\"ip\":\"%s\",\"mask\":%d,\"rx_bytes\":%lld,\"tx_bytes\":%lld}",
                        e_ifn2, e_st2, e_mc2, e_ip3, cur_mask, rx_bytes, tx_bytes);
                    iface_count++;
                }

                pclose(fp);
            }
        }

        if (ip_json_buf) free(ip_json_buf);
        signal(SIGCHLD, SIG_IGN);

        /* Закрываем JSON */
        if (bpos + 32 > cap) {
            cap += 64;
            char *nb = realloc(buf, cap);
            if (nb) buf = nb;
        }
        bpos += snprintf(buf + bpos, cap - bpos, "],\"count\":%d}", iface_count);

        send_response(fd, 200, "OK", buf);
        free(buf);
        return;
    }

    /* GET /net/ports — Слушающие порты (ss -tlnp) */
    if (strcmp(req.path, "/net/ports") == 0 && strcmp(req.method, "GET") == 0) {
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("ss -tlnp 2>/dev/null", "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"ss command failed\"}");
            return;
        }

        int cap = 8192;
        char *buf = malloc(cap);
        if (!buf) {
            pclose(fp);
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int bpos = 0;
        bpos += snprintf(buf + bpos, cap - bpos, "{\"ports\":[");

        char line[1024];
        int port_count = 0;
        int first_line = 1; /* Пропускаем заголовочную строку */

        while (fgets(line, sizeof(line), fp)) {
            /* Пропускаем заголовок "State  Recv-Q  Send-Q  Local Address:Port ..." */
            if (first_line) { first_line = 0; continue; }

            /* Trim trailing whitespace */
            int l = strlen(line);
            while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r' || line[l-1] == ' '))
                line[--l] = '\0';
            if (l == 0) continue;

            /* Формат ss -tlnp:
               LISTEN  0  128  0.0.0.0:8080  0.0.0.0:*  users:(("seed",pid=123,fd=4))
               Поля разделены пробелами (multiple spaces) */

            /* Парсим поля, пропуская множественные пробелы */
            char state_str[32] = "";
            char local_addr_port[128] = "";
            char process_info[512] = "";

            char *p = line;
            int field = 0;
            while (*p && field < 5) {
                while (*p == ' ') p++;
                if (*p == '\0') break;

                char *start = p;
                while (*p && *p != ' ') p++;
                int flen = p - start;

                switch (field) {
                    case 0: /* State */
                        if (flen >= (int)sizeof(state_str)) flen = sizeof(state_str) - 1;
                        memcpy(state_str, start, flen);
                        state_str[flen] = '\0';
                        break;
                    case 3: /* Local Address:Port */
                        if (flen >= (int)sizeof(local_addr_port)) flen = sizeof(local_addr_port) - 1;
                        memcpy(local_addr_port, start, flen);
                        local_addr_port[flen] = '\0';
                        break;
                    case 4: /* Peer Address:Port (skip), but after this comes process */
                        break;
                }
                field++;
            }

            /* Process info — всё после 5-го поля */
            while (*p == ' ') p++;
            if (*p) {
                strncpy(process_info, p, sizeof(process_info) - 1);
                process_info[sizeof(process_info) - 1] = '\0';
            }

            /* Разделяем local_addr и port — порт после последнего ':' */
            char address[128] = "";
            int listen_port = 0;
            char *last_colon = strrchr(local_addr_port, ':');
            if (last_colon) {
                int alen = last_colon - local_addr_port;
                if (alen >= (int)sizeof(address)) alen = sizeof(address) - 1;
                memcpy(address, local_addr_port, alen);
                address[alen] = '\0';
                listen_port = atoi(last_colon + 1);
            }

            /* Извлекаем PID и process name из users:(("name",pid=123,fd=4)) */
            int pid = 0;
            char proc_name[128] = "";
            char *users = strstr(process_info, "users:((");
            if (users) {
                users += 8; /* skip 'users:((' */
                /* "name",pid=NNN */
                if (*users == '"') {
                    users++;
                    char *qend = strchr(users, '"');
                    if (qend) {
                        int nlen = qend - users;
                        if (nlen >= (int)sizeof(proc_name)) nlen = sizeof(proc_name) - 1;
                        memcpy(proc_name, users, nlen);
                        proc_name[nlen] = '\0';
                    }
                }
                char *pid_ptr = strstr(process_info, "pid=");
                if (pid_ptr) {
                    pid = atoi(pid_ptr + 4);
                }
            }

            /* Расширяем буфер */
            if (bpos + 512 > cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }

            static char esc_addr[256], esc_proc[256];
            json_escape(address, esc_addr, sizeof(esc_addr));
            json_escape(proc_name, esc_proc, sizeof(esc_proc));

            if (port_count > 0) {
                if (bpos + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
                buf[bpos++] = ',';
            }
            bpos += snprintf(buf + bpos, cap - bpos,
                "{\"proto\":\"tcp\",\"address\":\"%s\",\"port\":%d,\"pid\":%d,\"process\":\"%s\"}",
                esc_addr, listen_port, pid, esc_proc);
            port_count++;
        }

        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        /* Закрываем JSON */
        if (bpos + 32 > cap) {
            cap += 64;
            char *nb = realloc(buf, cap);
            if (nb) buf = nb;
        }
        bpos += snprintf(buf + bpos, cap - bpos, "],\"count\":%d}", port_count);

        send_response(fd, 200, "OK", buf);
        free(buf);
        return;
    }

    /* ========== System & Files Toolbox ========== */

    /* GET /proc/stats — Системная статистика (CPU, RAM, disk, temp, load, uptime) */
    if (strcmp(req.path, "/proc/stats") == 0 && strcmp(req.method, "GET") == 0) {
        /* CPU — читаем /proc/stat дважды с интервалом ~100ms, вычисляем дельту */
        double cpu_percent = 0.0;
        {
            FILE *fp = fopen("/proc/stat", "r");
            long long user1=0, nice1=0, sys1=0, idle1=0, iow1=0, irq1=0, sirq1=0, steal1=0;
            long long user2=0, nice2=0, sys2=0, idle2=0, iow2=0, irq2=0, sirq2=0, steal2=0;
            if (fp) {
                char line[256];
                if (fgets(line, sizeof(line), fp))
                    sscanf(line, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                        &user1, &nice1, &sys1, &idle1, &iow1, &irq1, &sirq1, &steal1);
                fclose(fp);
            }
            usleep(100000); /* 100ms */
            fp = fopen("/proc/stat", "r");
            if (fp) {
                char line[256];
                if (fgets(line, sizeof(line), fp))
                    sscanf(line, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                        &user2, &nice2, &sys2, &idle2, &iow2, &irq2, &sirq2, &steal2);
                fclose(fp);
            }
            long long total1 = user1+nice1+sys1+idle1+iow1+irq1+sirq1+steal1;
            long long total2 = user2+nice2+sys2+idle2+iow2+irq2+sirq2+steal2;
            long long dtotal = total2 - total1;
            long long didle = (idle2+iow2) - (idle1+iow1);
            if (dtotal > 0)
                cpu_percent = 100.0 * (double)(dtotal - didle) / (double)dtotal;
        }

        /* RAM — /proc/meminfo */
        long long mem_total_kb = 0, mem_available_kb = 0, mem_free_kb = 0;
        {
            FILE *fp = fopen("/proc/meminfo", "r");
            if (fp) {
                char line[128];
                while (fgets(line, sizeof(line), fp)) {
                    if (strncmp(line, "MemTotal:", 9) == 0)
                        sscanf(line, "MemTotal: %lld kB", &mem_total_kb);
                    else if (strncmp(line, "MemAvailable:", 13) == 0)
                        sscanf(line, "MemAvailable: %lld kB", &mem_available_kb);
                    else if (strncmp(line, "MemFree:", 8) == 0)
                        sscanf(line, "MemFree: %lld kB", &mem_free_kb);
                }
                fclose(fp);
            }
        }
        double mem_used_percent = 0.0;
        if (mem_total_kb > 0)
            mem_used_percent = 100.0 * (double)(mem_total_kb - mem_available_kb) / (double)mem_total_kb;

        /* Disk — statfs("/") */
        long long disk_total_mb = 0, disk_available_mb = 0;
        double disk_used_percent = 0.0;
        {
            struct statvfs svfs;
            if (statvfs("/", &svfs) == 0) {
                long long total_bytes = (long long)svfs.f_blocks * svfs.f_frsize;
                long long avail_bytes = (long long)svfs.f_bavail * svfs.f_frsize;
                long long used_bytes = total_bytes - (long long)svfs.f_bfree * svfs.f_frsize;
                disk_total_mb = total_bytes / (1024*1024);
                disk_available_mb = avail_bytes / (1024*1024);
                if (total_bytes > 0)
                    disk_used_percent = 100.0 * (double)used_bytes / (double)total_bytes;
            }
        }

        /* Temp — /sys/class/thermal/thermal_zone0/temp */
        double temp_c = 0.0;
        {
            FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
            if (fp) {
                long raw = 0;
                if (fscanf(fp, "%ld", &raw) == 1)
                    temp_c = (double)raw / 1000.0;
                fclose(fp);
            }
        }

        /* Load — /proc/loadavg */
        double load_1 = 0, load_5 = 0, load_15 = 0;
        {
            FILE *fp = fopen("/proc/loadavg", "r");
            if (fp) {
                fscanf(fp, "%lf %lf %lf", &load_1, &load_5, &load_15);
                fclose(fp);
            }
        }

        /* Uptime — /proc/uptime */
        double uptime_sec = 0;
        {
            FILE *fp = fopen("/proc/uptime", "r");
            if (fp) {
                fscanf(fp, "%lf", &uptime_sec);
                fclose(fp);
            }
        }

        snprintf(response, sizeof(response),
            "{\"cpu_percent\":%.1f,"
            "\"mem_total_mb\":%lld,\"mem_available_mb\":%lld,\"mem_used_percent\":%.1f,"
            "\"disk_total_mb\":%lld,\"disk_available_mb\":%lld,\"disk_used_percent\":%.1f,"
            "\"temp_c\":%.1f,"
            "\"load_1\":%.2f,\"load_5\":%.2f,\"load_15\":%.2f,"
            "\"uptime_seconds\":%.0f}",
            cpu_percent,
            mem_total_kb / 1024, mem_available_kb / 1024, mem_used_percent,
            disk_total_mb, disk_available_mb, disk_used_percent,
            temp_c,
            load_1, load_5, load_15,
            uptime_sec);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /proc/list — Список процессов (ps aux, отсортированные по CPU) */
    if (strcmp(req.path, "/proc/list") == 0 && strcmp(req.method, "GET") == 0) {
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("ps aux --no-header 2>/dev/null", "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"popen failed\"}");
            return;
        }

        /* Парсим процессы во временный массив */
        typedef struct {
            int pid;
            char user[32];
            double cpu_pct;
            double mem_pct;
            long rss_kb;
            char command[129];
        } proc_entry_t;

        int proc_cap = 256;
        proc_entry_t *procs = malloc(proc_cap * sizeof(proc_entry_t));
        if (!procs) {
            pclose(fp);
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }
        int proc_count = 0;

        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            /* ps aux: USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND */
            char user[32], tty[16], stat[8], start[16], ptime[16];
            int pid;
            double cpu_pct, mem_pct;
            long vsz, rss;
            char cmd_buf[256] = "";
            int parsed = sscanf(line, "%31s %d %lf %lf %ld %ld %15s %7s %15s %15s %255[^\n]",
                user, &pid, &cpu_pct, &mem_pct, &vsz, &rss, tty, stat, start, ptime, cmd_buf);
            if (parsed < 10) continue;

            if (proc_count >= proc_cap) {
                proc_cap *= 2;
                proc_entry_t *np = realloc(procs, proc_cap * sizeof(proc_entry_t));
                if (!np) break;
                procs = np;
            }

            proc_entry_t *e = &procs[proc_count++];
            e->pid = pid;
            strncpy(e->user, user, sizeof(e->user)-1); e->user[sizeof(e->user)-1] = '\0';
            e->cpu_pct = cpu_pct;
            e->mem_pct = mem_pct;
            e->rss_kb = rss;
            strncpy(e->command, cmd_buf, 128); e->command[128] = '\0';
        }
        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        /* Сортировка по cpu_percent descending (bubble sort — для сотен процессов ОК) */
        for (int i = 0; i < proc_count - 1; i++) {
            for (int j = i + 1; j < proc_count; j++) {
                if (procs[j].cpu_pct > procs[i].cpu_pct) {
                    proc_entry_t tmp = procs[i];
                    procs[i] = procs[j];
                    procs[j] = tmp;
                }
            }
        }

        /* Формируем JSON */
        int cap = 4096 + proc_count * 512;
        char *buf = malloc(cap);
        if (!buf) {
            free(procs);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int bpos = 0;
        bpos += snprintf(buf + bpos, cap - bpos, "{\"processes\":[");

        char esc_user[64], esc_cmd[300];
        for (int i = 0; i < proc_count; i++) {
            /* Realloc check BEFORE comma write */
            int needed = 512;
            if (bpos + needed > cap) {
                cap += needed + 4096;
                char *nb = realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }

            if (i > 0) buf[bpos++] = ',';

            json_escape(procs[i].user, esc_user, sizeof(esc_user));
            json_escape(procs[i].command, esc_cmd, sizeof(esc_cmd));

            bpos += snprintf(buf + bpos, cap - bpos,
                "{\"pid\":%d,\"user\":\"%s\",\"cpu_percent\":%.1f,"
                "\"mem_percent\":%.1f,\"rss_kb\":%ld,\"command\":\"%s\"}",
                procs[i].pid, esc_user, procs[i].cpu_pct,
                procs[i].mem_pct, procs[i].rss_kb, esc_cmd);
        }

        free(procs);

        /* Закрываем JSON */
        if (bpos + 64 > cap) {
            cap += 128;
            char *nb = realloc(buf, cap);
            if (nb) buf = nb;
        }
        bpos += snprintf(buf + bpos, cap - bpos, "],\"count\":%d}", proc_count);

        send_response(fd, 200, "OK", buf);
        free(buf);
        return;
    }

    /* ========== Filesystem Toolbox (path-restricted) ========== */

    // Проверка разрешённости пути для fs эндпоинтов.
    // Разрешены ТОЛЬКО: /etc/seed/, /var/seed/, /tmp/, /opt/seed/
    // Использует realpath() для нормализации (symlinks, ../ traversal).
    // fs_path_allowed() defined above handles all validation.

    /* GET /fs/ls/{path} — Список файлов в директории */
    if (strncmp(req.path, "/fs/ls/", 7) == 0 && strcmp(req.method, "GET") == 0) {
        const char *raw_path = req.path + 6;  /* включаем начальный / */
        if (strlen(raw_path) < 2) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty path\"}");
            return;
        }

        /* Path validation через realpath + whitelist (realpath нормализует ../ и symlinks) */
        char real[PATH_MAX];
        if (!realpath(raw_path, real)) {
            send_response(fd, 404, "Not Found",
                "{\"error\":\"path not found\"}");
            return;
        }
        if (strncmp(real, "/etc/seed/", 14) != 0 &&
            strncmp(real, "/var/seed/", 14) != 0 &&
            strncmp(real, "/tmp/", 5) != 0 &&
            strncmp(real, "/opt/seed/", 16) != 0 &&
            strcmp(real, "/etc/seed") != 0 &&
            strcmp(real, "/var/seed") != 0 &&
            strcmp(real, "/tmp") != 0 &&
            strcmp(real, "/opt/seed") != 0) {
            send_response(fd, 403, "Forbidden",
                "{\"error\":\"path not allowed\"}");
            return;
        }

        DIR *d = opendir(real);
        if (!d) {
            send_response(fd, 404, "Not Found",
                "{\"error\":\"cannot open directory\"}");
            return;
        }

        int cap = 4096;
        char *buf = malloc(cap);
        if (!buf) {
            closedir(d);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        char esc_path[PATH_MAX * 2];
        json_escape(real, esc_path, sizeof(esc_path));
        int bpos = 0;
        bpos += snprintf(buf + bpos, cap - bpos,
            "{\"path\":\"%s\",\"entries\":[", esc_path);

        int count = 0;
        struct dirent *entry;
        char fullpath[PATH_MAX];
        char esc_name[512];
        while ((entry = readdir(d)) != NULL) {
            /* Пропускаем . и .. */
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            snprintf(fullpath, sizeof(fullpath), "%s/%s", real, entry->d_name);
            struct stat st;
            if (lstat(fullpath, &st) != 0) continue;

            const char *type = "file";
            if (S_ISDIR(st.st_mode)) type = "dir";
            else if (S_ISLNK(st.st_mode)) type = "link";

            /* Realloc check BEFORE comma write */
            int needed = 512;
            if (bpos + needed > cap) {
                cap += needed + 4096;
                char *nb = realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }

            if (count > 0) buf[bpos++] = ',';

            json_escape(entry->d_name, esc_name, sizeof(esc_name));
            bpos += snprintf(buf + bpos, cap - bpos,
                "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lld,\"mtime\":%lld}",
                esc_name, type, (long long)st.st_size, (long long)st.st_mtime);
            count++;
        }
        closedir(d);

        /* Закрываем JSON */
        if (bpos + 64 > cap) {
            cap += 128;
            char *nb = realloc(buf, cap);
            if (nb) buf = nb;
        }
        bpos += snprintf(buf + bpos, cap - bpos, "],\"count\":%d}", count);

        send_response(fd, 200, "OK", buf);
        free(buf);
        return;
    }

    /* GET /fs/read/{path} — Прочитать файл (max 1MB) */
    if (strncmp(req.path, "/fs/read/", 9) == 0 && strcmp(req.method, "GET") == 0) {
        const char *raw_path = req.path + 8;  /* включаем начальный / */
        if (strlen(raw_path) < 2) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty path\"}");
            return;
        }

        /* Path validation через realpath + whitelist */
        char real[PATH_MAX];
        if (!realpath(raw_path, real)) {
            send_response(fd, 404, "Not Found",
                "{\"error\":\"path not found\"}");
            return;
        }
        if (strncmp(real, "/etc/seed/", 14) != 0 &&
            strncmp(real, "/var/seed/", 14) != 0 &&
            strncmp(real, "/tmp/", 5) != 0 &&
            strncmp(real, "/opt/seed/", 16) != 0) {
            send_response(fd, 403, "Forbidden",
                "{\"error\":\"path not allowed\"}");
            return;
        }

        struct stat st;
        if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) {
            send_response(fd, 404, "Not Found",
                "{\"error\":\"not a regular file\"}");
            return;
        }

        if (st.st_size > 1024 * 1024) {
            send_response(fd, 413, "Payload Too Large",
                "{\"error\":\"file too large (max 1MB)\"}");
            return;
        }

        FILE *fp = fopen(real, "r");
        if (!fp) {
            send_response(fd, 500, "Error", "{\"error\":\"cannot open file\"}");
            return;
        }

        char *content = malloc(st.st_size + 1);
        if (!content) {
            fclose(fp);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }
        int n = fread(content, 1, st.st_size, fp);
        content[n] = '\0';
        fclose(fp);

        /* Проверка на бинарный файл (содержит \0) */
        int is_binary = 0;
        for (int i = 0; i < n; i++) {
            if (content[i] == '\0') { is_binary = 1; break; }
        }

        char esc_path[PATH_MAX * 2];
        json_escape(real, esc_path, sizeof(esc_path));

        if (is_binary) {
            free(content);
            snprintf(response, sizeof(response),
                "{\"path\":\"%s\",\"binary\":true,\"size\":%lld,\"mtime\":%lld}",
                esc_path, (long long)st.st_size, (long long)st.st_mtime);
            send_response(fd, 200, "OK", response);
            return;
        }

        /* JSON-escape содержимого */
        int esc_cap = n * 2 + 256;
        char *esc_content = malloc(esc_cap);
        if (!esc_content) {
            free(content);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }
        json_escape(content, esc_content, esc_cap);
        free(content);

        /* Формируем JSON-ответ */
        int resp_len = strlen(esc_content) + PATH_MAX * 2 + 256;
        char *buf = malloc(resp_len);
        if (!buf) {
            free(esc_content);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }
        snprintf(buf, resp_len,
            "{\"path\":\"%s\",\"content\":\"%s\",\"size\":%lld,\"mtime\":%lld}",
            esc_path, esc_content, (long long)st.st_size, (long long)st.st_mtime);
        free(esc_content);

        send_response(fd, 200, "OK", buf);
        free(buf);
        return;
    }

    /* POST /fs/write/{path} — Записать файл (body = raw content, max 1MB) */
    if (strncmp(req.path, "/fs/write/", 10) == 0 && strcmp(req.method, "POST") == 0) {
        const char *raw_path = req.path + 9;  /* включаем начальный / */
        if (strlen(raw_path) < 2) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty path\"}");
            return;
        }

        if (req.body_len > 1024 * 1024) {
            send_response(fd, 413, "Payload Too Large",
                "{\"error\":\"body too large (max 1MB)\"}");
            return;
        }

        /* Path validation — для записи файл может не существовать,
         * поэтому проверяем директорию через realpath */
        char dir_buf[PATH_MAX];
        strncpy(dir_buf, raw_path, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *last_slash = strrchr(dir_buf, '/');
        const char *filename = "";
        if (last_slash && last_slash != dir_buf) {
            *last_slash = '\0';
            filename = last_slash + 1;
        } else {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid path\"}");
            return;
        }

        /* Проверяем что filename не пустой и безопасен */
        if (strlen(filename) == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty filename\"}");
            return;
        }
        /* Filename не должен содержать / или .. — иначе можно выйти за real_dir */
        if (strchr(filename, '/') != NULL || strstr(filename, "..") != NULL) {
            send_response(fd, 400, "Bad Request",
                "{\"error\":\"filename must not contain / or ..\"}");
            return;
        }

        char real_dir[PATH_MAX];
        /* Попробуем resolve директорию, если не получается — создаём */
        if (!realpath(dir_buf, real_dir)) {
            /* Директория не существует — проверяем что она в разрешённом префиксе.
             * Для этого проверяем сам raw_path (до нормализации).
             * Безопасно, потому что потом realpath ещё раз проверит. */
            if (strncmp(dir_buf, "/etc/seed/", 14) != 0 &&
                strncmp(dir_buf, "/etc/seed", 13) != 0 &&
                strncmp(dir_buf, "/var/seed/", 14) != 0 &&
                strncmp(dir_buf, "/var/seed", 13) != 0 &&
                strncmp(dir_buf, "/tmp/", 5) != 0 &&
                strncmp(dir_buf, "/tmp", 4) != 0 &&
                strncmp(dir_buf, "/opt/seed/", 16) != 0 &&
                strncmp(dir_buf, "/opt/seed", 15) != 0) {
                send_response(fd, 403, "Forbidden",
                    "{\"error\":\"path not allowed\"}");
                return;
            }

            /* Создаём директорию рекурсивно (pure C, без system()) */
            {
                char tmp[PATH_MAX];
                strncpy(tmp, dir_buf, sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                for (char *p = tmp + 1; *p; p++) {
                    if (*p == '/') {
                        *p = '\0';
                        mkdir(tmp, 0755);
                        *p = '/';
                    }
                }
                mkdir(tmp, 0755);
            }

            /* Пробуем realpath ещё раз */
            if (!realpath(dir_buf, real_dir)) {
                send_response(fd, 500, "Error",
                    "{\"error\":\"cannot create directory\"}");
                return;
            }
        }

        /* Финальная проверка нормализованного пути */
        if (strncmp(real_dir, "/etc/seed/", 14) != 0 &&
            strncmp(real_dir, "/var/seed/", 14) != 0 &&
            strncmp(real_dir, "/tmp/", 5) != 0 &&
            strncmp(real_dir, "/opt/seed/", 16) != 0 &&
            strcmp(real_dir, "/etc/seed") != 0 &&
            strcmp(real_dir, "/var/seed") != 0 &&
            strcmp(real_dir, "/tmp") != 0 &&
            strcmp(real_dir, "/opt/seed") != 0) {
            send_response(fd, 403, "Forbidden",
                "{\"error\":\"path not allowed\"}");
            return;
        }

        /* Собираем полный путь */
        char final_path[PATH_MAX];
        snprintf(final_path, sizeof(final_path), "%s/%s", real_dir, filename);

        /* Дополнительные запреты на запись */
        if (strcmp(final_path, "/etc/seed/token") == 0) {
            send_response(fd, 403, "Forbidden",
                "{\"error\":\"cannot write token file\"}");
            return;
        }
        if (strcmp(final_path, "/opt/seed/server.c") == 0) {
            send_response(fd, 403, "Forbidden",
                "{\"error\":\"use /firmware/source to update server.c\"}");
            return;
        }

        FILE *fp = fopen(final_path, "w");
        if (!fp) {
            send_response(fd, 500, "Error", "{\"error\":\"cannot write file\"}");
            return;
        }
        fwrite(req.body, 1, req.body_len, fp);
        fclose(fp);

        char esc_path[PATH_MAX * 2];
        json_escape(final_path, esc_path, sizeof(esc_path));
        snprintf(response, sizeof(response),
            "{\"path\":\"%s\",\"size\":%d,\"ok\":true}",
            esc_path, req.body_len);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* ========== Firmware Vault ========== */

    /* POST /vault/store — Сохранить прошивку в vault
     * Body = raw binary firmware data
     * Headers: X-Node-Type (esp32/pi), X-Version, X-Description
     * Поддерживает large body через потоковое чтение (до 2MB) */
    if (strcmp(req.path, "/vault/store") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.content_length <= 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty firmware body\"}");
            return;
        }
        if (req.content_length > VAULT_MAX_FW_SIZE) {
            snprintf(response, sizeof(response),
                "{\"error\":\"firmware too large (max %d bytes)\"}", VAULT_MAX_FW_SIZE);
            send_response(fd, 413, "Payload Too Large", response);
            return;
        }
        if (req.x_node_type[0] == '\0' || req.x_version[0] == '\0') {
            send_response(fd, 400, "Bad Request",
                "{\"error\":\"X-Node-Type and X-Version headers required\"}");
            return;
        }
        /* X-Node-Type: только esp32 или pi */
        if (strcmp(req.x_node_type, "esp32") != 0 && strcmp(req.x_node_type, "pi") != 0) {
            send_response(fd, 400, "Bad Request",
                "{\"error\":\"X-Node-Type must be 'esp32' or 'pi'\"}");
            return;
        }
        /* X-Version: только [a-zA-Z0-9._-] */
        for (const char *vp = req.x_version; *vp; vp++) {
            if (!((*vp >= 'a' && *vp <= 'z') || (*vp >= 'A' && *vp <= 'Z') ||
                  (*vp >= '0' && *vp <= '9') || *vp == '.' || *vp == '_' || *vp == '-')) {
                send_response(fd, 400, "Bad Request",
                    "{\"error\":\"X-Version: only [a-zA-Z0-9._-] allowed\"}");
                return;
            }
        }

        /* Создаём vault директорию (pure C, без system()) */
        {
            char tmp[PATH_MAX];
            strncpy(tmp, VAULT_DIR, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            for (char *p = tmp + 1; *p; p++) {
                if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
            }
            mkdir(tmp, 0755);
        }

        /* Генерируем ID: timestamp_random (urandom для уникальности) */
        char fw_id[64];
        {
            time_t now = time(NULL);
            unsigned int rnd = 0;
            FILE *urand = fopen("/dev/urandom", "r");
            if (urand) { fread(&rnd, sizeof(rnd), 1, urand); fclose(urand); }
            else { rnd = (unsigned int)(now ^ getpid()); }
            snprintf(fw_id, sizeof(fw_id), "%ld_%08x", (long)now, rnd);
        }

        /* Записываем binary файл */
        char fw_path[PATH_MAX];
        snprintf(fw_path, sizeof(fw_path), "%s/%s.bin", VAULT_DIR, fw_id);
        FILE *fp = fopen(fw_path, "wb");
        if (!fp) {
            snprintf(response, sizeof(response),
                "{\"error\":\"cannot create %s: %s\"}", fw_path, strerror(errno));
            send_response(fd, 500, "Error", response);
            return;
        }

        long total_written = 0;

        /* Записываем уже прочитанное в body[] */
        int write_err = 0;
        if (req.body_len > 0) {
            size_t w = fwrite(req.body, 1, req.body_len, fp);
            if ((int)w != req.body_len) write_err = 1;
            total_written += w;
        }

        /* Large body — дочитываем остаток из сокета */
        if (!write_err && req.large_body_fd > 0 && req.body_remaining > 0) {
            char chunk[8192];
            int remaining = req.body_remaining;
            while (remaining > 0) {
                int to_read = remaining < (int)sizeof(chunk) ? remaining : (int)sizeof(chunk);
                int n = read(req.large_body_fd, chunk, to_read);
                if (n <= 0) break;
                size_t w = fwrite(chunk, 1, n, fp);
                if ((int)w != n) { write_err = 1; break; }
                total_written += n;
                remaining -= n;
            }
        }
        fclose(fp);

        if (write_err) {
            unlink(fw_path);
            send_response(fd, 500, "Error", "{\"error\":\"disk write failed\"}");
            return;
        }

        /* Обновляем manifest.json — читаем существующий, добавляем запись */
        /* Формат даты ISO 8601 */
        char date_buf[32];
        {
            time_t now = time(NULL);
            struct tm *t = gmtime(&now);
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%SZ", t);
        }

        /* Escape строковых полей */
        char esc_id[128], esc_type[64], esc_ver[128], esc_desc[512], esc_date[64];
        json_escape(fw_id, esc_id, sizeof(esc_id));
        json_escape(req.x_node_type, esc_type, sizeof(esc_type));
        json_escape(req.x_version, esc_ver, sizeof(esc_ver));
        json_escape(req.x_description, esc_desc, sizeof(esc_desc));
        json_escape(date_buf, esc_date, sizeof(esc_date));

        /* Новая запись JSON */
        char new_entry[1024];
        snprintf(new_entry, sizeof(new_entry),
            "{\"id\":\"%s\",\"node_type\":\"%s\",\"version\":\"%s\","
            "\"description\":\"%s\",\"size\":%ld,\"date\":\"%s\"}",
            esc_id, esc_type, esc_ver, esc_desc, total_written, esc_date);

        /* Читаем существующий manifest */
        char *manifest = NULL;
        long mlen = 0;
        FILE *mf = fopen(VAULT_MANIFEST, "r");
        if (mf) {
            fseek(mf, 0, SEEK_END);
            mlen = ftell(mf);
            fseek(mf, 0, SEEK_SET);
            if (mlen > 0) {
                manifest = malloc(mlen + 1);
                if (manifest) {
                    int n = fread(manifest, 1, mlen, mf);
                    manifest[n] = '\0';
                    mlen = n;
                }
            }
            fclose(mf);
        }

        /* Записываем обновлённый manifest */
        mf = fopen(VAULT_MANIFEST, "w");
        if (mf) {
            if (manifest && mlen > 2) {
                /* Ищем последний ] в существующем массиве */
                char *last_bracket = strrchr(manifest, ']');
                if (last_bracket) {
                    /* Записываем всё до ] */
                    fwrite(manifest, 1, last_bracket - manifest, mf);
                    fprintf(mf, ",%s]", new_entry);
                } else {
                    /* Невалидный manifest — перезаписываем */
                    fprintf(mf, "[%s]", new_entry);
                }
            } else {
                /* Нет manifest или пустой — создаём новый */
                fprintf(mf, "[%s]", new_entry);
            }
            fclose(mf);
        }
        if (manifest) free(manifest);

        event_add("vault: stored firmware %s (%ld bytes)", fw_id, total_written);
        snprintf(response, sizeof(response),
            "{\"ok\":true,\"id\":\"%s\",\"size\":%ld}", esc_id, total_written);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /vault/list — Список прошивок из manifest.json */
    if (strcmp(req.path, "/vault/list") == 0 && strcmp(req.method, "GET") == 0) {
        char *manifest = NULL;
        long mlen = 0;
        FILE *mf = fopen(VAULT_MANIFEST, "r");
        if (mf) {
            fseek(mf, 0, SEEK_END);
            mlen = ftell(mf);
            fseek(mf, 0, SEEK_SET);
            if (mlen > 0) {
                manifest = malloc(mlen + 1);
                if (manifest) {
                    int n = fread(manifest, 1, mlen, mf);
                    manifest[n] = '\0';
                    mlen = n;
                }
            }
            fclose(mf);
        }

        /* Считаем количество записей по числу "id" вхождений */
        int count = 0;
        if (manifest) {
            const char *p = manifest;
            while ((p = strstr(p, "\"id\"")) != NULL) { count++; p += 4; }
        }

        /* Формируем ответ */
        int resp_cap = (manifest ? (int)mlen : 2) + 128;
        char *buf = malloc(resp_cap);
        if (!buf) {
            if (manifest) free(manifest);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }
        snprintf(buf, resp_cap, "{\"firmware\":%s,\"count\":%d}",
            manifest && mlen > 0 ? manifest : "[]", count);

        send_response(fd, 200, "OK", buf);
        free(buf);
        if (manifest) free(manifest);
        return;
    }

    /* DELETE /vault/{id} — Удалить прошивку из vault */
    if (strncmp(req.path, "/vault/", 7) == 0 && strcmp(req.method, "DELETE") == 0) {
        const char *fw_id = req.path + 7;

        /* ID validation: только [a-zA-Z0-9_] */
        if (strlen(fw_id) == 0 || strlen(fw_id) > 60) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid firmware ID\"}");
            return;
        }
        for (const char *p = fw_id; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in ID\"}");
                return;
            }
        }

        /* Обновляем manifest ПЕРЕД удалением файла (crash safety) */
        char *manifest = NULL;
        long mlen = 0;
        FILE *mf = fopen(VAULT_MANIFEST, "r");
        if (mf) {
            fseek(mf, 0, SEEK_END);
            mlen = ftell(mf);
            fseek(mf, 0, SEEK_SET);
            if (mlen > 0) {
                manifest = malloc(mlen + 1);
                if (manifest) {
                    int n = fread(manifest, 1, mlen, mf);
                    manifest[n] = '\0';
                    mlen = n;
                }
            }
            fclose(mf);
        }

        if (manifest && mlen > 2) {
            /* Находим запись с данным id и удаляем её из текста.
             * Ищем паттерн {"id":"<fw_id>" ... } и вырезаем.
             * Простой подход: ищем "id":"fw_id", берём { до и } после */
            char search[128];
            snprintf(search, sizeof(search), "\"id\":\"%s\"", fw_id);
            char *found = strstr(manifest, search);
            if (found) {
                /* Ищем начало объекта { перед найденным id */
                char *obj_start = found;
                while (obj_start > manifest && *obj_start != '{') obj_start--;

                /* Ищем конец объекта } после найденного id */
                char *obj_end = found;
                int depth = 0;
                /* Переходим к { */
                char *scan = obj_start;
                for (; *scan; scan++) {
                    if (*scan == '{') depth++;
                    else if (*scan == '}') { depth--; if (depth == 0) { obj_end = scan; break; } }
                }

                /* Удаляем объект + запятую (до или после) */
                char *del_start = obj_start;
                char *del_end = obj_end + 1;

                /* Убираем запятую: перед объектом или после */
                if (*del_end == ',') {
                    del_end++; /* удаляем запятую после */
                } else if (del_start > manifest) {
                    char *before = del_start - 1;
                    while (before > manifest && (*before == ' ' || *before == '\n' || *before == '\r' || *before == '\t')) before--;
                    if (*before == ',') del_start = before; /* удаляем запятую перед */
                }

                /* Записываем manifest без удалённого объекта */
                mf = fopen(VAULT_MANIFEST, "w");
                if (mf) {
                    fwrite(manifest, 1, del_start - manifest, mf);
                    fwrite(del_end, 1, strlen(del_end), mf);
                    fclose(mf);
                }
            }
        }
        if (manifest) free(manifest);

        /* Удаляем binary файл ПОСЛЕ обновления manifest */
        char fw_path[PATH_MAX];
        snprintf(fw_path, sizeof(fw_path), "%s/%s.bin", VAULT_DIR, fw_id);
        unlink(fw_path); /* OK if already gone */

        char esc_id[128];
        json_escape(fw_id, esc_id, sizeof(esc_id));
        event_add("vault: deleted firmware %s", fw_id);
        snprintf(response, sizeof(response), "{\"ok\":true,\"id\":\"%s\"}", esc_id);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /vault/push/{node_url_encoded} — Распределить прошивку на ноду */
    if (strncmp(req.path, "/vault/push/", 12) == 0 && strcmp(req.method, "POST") == 0) {
        const char *node_url = req.path + 12;

        /* Node URL validation: только [a-zA-Z0-9.:] (IP:port) */
        if (strlen(node_url) == 0 || strlen(node_url) > 64) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid node URL\"}");
            return;
        }
        for (const char *p = node_url; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '.' || *p == ':')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in node URL\"}");
                return;
            }
        }

        /* Парсим body JSON: {id: "vault_id", token: "auth_token"} */
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty body, need {id, token}\"}");
            return;
        }

        /* Простой парсинг JSON — ищем "id" и "token" */
        char vault_id[64] = "";
        char target_token[128] = "";
        {
            char *id_pos = strstr(req.body, "\"id\"");
            if (id_pos) {
                char *colon = strchr(id_pos + 4, ':');
                if (colon) {
                    char *q1 = strchr(colon, '"');
                    if (q1) {
                        q1++;
                        char *q2 = strchr(q1, '"');
                        if (q2 && (q2 - q1) < (int)sizeof(vault_id)) {
                            memcpy(vault_id, q1, q2 - q1);
                            vault_id[q2 - q1] = '\0';
                        }
                    }
                }
            }
            char *tok_pos = strstr(req.body, "\"token\"");
            if (tok_pos) {
                char *colon = strchr(tok_pos + 7, ':');
                if (colon) {
                    char *q1 = strchr(colon, '"');
                    if (q1) {
                        q1++;
                        char *q2 = strchr(q1, '"');
                        if (q2 && (q2 - q1) < (int)sizeof(target_token)) {
                            memcpy(target_token, q1, q2 - q1);
                            target_token[q2 - q1] = '\0';
                        }
                    }
                }
            }
        }

        if (vault_id[0] == '\0' || target_token[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"id and token required in JSON body\"}");
            return;
        }

        /* Vault ID validation: [a-zA-Z0-9_] */
        for (const char *p = vault_id; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in vault ID\"}");
                return;
            }
        }

        /* Token validation: [a-zA-Z0-9] */
        for (const char *p = target_token; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9'))) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in token\"}");
                return;
            }
        }

        /* Проверяем что firmware существует */
        char fw_path[PATH_MAX];
        snprintf(fw_path, sizeof(fw_path), "%s/%s.bin", VAULT_DIR, vault_id);
        struct stat fw_stat;
        if (stat(fw_path, &fw_stat) != 0) {
            send_response(fd, 404, "Not Found", "{\"error\":\"firmware not found in vault\"}");
            return;
        }

        /* Формируем curl команду — все входные данные валидированы */
        char curl_cmd[1024];
        snprintf(curl_cmd, sizeof(curl_cmd),
            "curl -s -w '\\n%%{http_code}' -X POST "
            "\"http://%s/firmware/upload\" "
            "-H \"Authorization: Bearer %s\" "
            "-H \"Content-Type: application/octet-stream\" "
            "--data-binary @\"%s\" "
            "--max-time 30 2>&1",
            node_url, target_token, fw_path);

        /* SIGCHLD wrapping для popen */
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen(curl_cmd, "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"popen curl failed\"}");
            return;
        }

        char curl_output[4096] = "";
        int curl_total = 0;
        char tmp[256];
        while (fgets(tmp, sizeof(tmp), fp) && curl_total < (int)sizeof(curl_output) - 256) {
            int n = strlen(tmp);
            memcpy(curl_output + curl_total, tmp, n);
            curl_total += n;
        }
        curl_output[curl_total] = '\0';
        int rc = pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        char esc_output[8192];
        json_escape(curl_output, esc_output, sizeof(esc_output));
        char esc_node[128];
        json_escape(node_url, esc_node, sizeof(esc_node));

        if (exit_code == 0) {
            event_add("vault: pushed %s to %s (%ld bytes)", vault_id, node_url, (long)fw_stat.st_size);
            snprintf(response, sizeof(response),
                "{\"ok\":true,\"target\":\"%s\",\"size\":%ld,\"response\":\"%s\"}",
                esc_node, (long)fw_stat.st_size, esc_output);
            send_response(fd, 200, "OK", response);
        } else {
            snprintf(response, sizeof(response),
                "{\"ok\":false,\"target\":\"%s\",\"error\":\"curl exit %d\",\"response\":\"%s\"}",
                esc_node, exit_code, esc_output);
            send_response(fd, 502, "Bad Gateway", response);
        }
        return;
    }

    /* GET /vault/{id} — Скачать прошивку (binary, octet-stream) */
    if (strncmp(req.path, "/vault/", 7) == 0 && strcmp(req.method, "GET") == 0 &&
        strcmp(req.path, "/vault/list") != 0) {
        const char *fw_id = req.path + 7;

        /* ID validation: только [a-zA-Z0-9_] */
        if (strlen(fw_id) == 0 || strlen(fw_id) > 60) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid firmware ID\"}");
            return;
        }
        for (const char *p = fw_id; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in ID\"}");
                return;
            }
        }

        char fw_path[PATH_MAX];
        snprintf(fw_path, sizeof(fw_path), "%s/%s.bin", VAULT_DIR, fw_id);

        FILE *fp = fopen(fw_path, "rb");
        if (!fp) {
            send_response(fd, 404, "Not Found", "{\"error\":\"firmware not found\"}");
            return;
        }

        /* Размер файла */
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        /* HTTP заголовки для binary */
        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %ld\r\n"
            "Content-Disposition: attachment; filename=\"%s.bin\"\r\n"
            "Connection: close\r\n"
            "\r\n",
            file_size, fw_id);
        write(fd, header, hlen);

        /* Стримим содержимое файла */
        char chunk[8192];
        size_t nread;
        while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
            size_t written = 0;
            while (written < nread) {
                ssize_t w = write(fd, chunk + written, nread - written);
                if (w <= 0) { fclose(fp); return; }
                written += w;
            }
        }
        fclose(fp);
        return;
    }

    /* ========== Fleet Management ========== */

    /* GET /fleet/status — Агрегированный статус всех нод в mesh */
    if (strcmp(req.path, "/fleet/status") == 0 && strcmp(req.method, "GET") == 0) {
        char hostname[256] = "unknown";
        gethostname(hostname, sizeof(hostname));

        /* mDNS discovery — avahi-browse (та же логика что /mesh) */
        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("/usr/bin/timeout 3 /usr/bin/avahi-browse -r -p _seed._tcp 2>/dev/null", "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"avahi-browse failed\"}");
            return;
        }

        /* Собираем ноды из mDNS */
        typedef struct { char name[128]; char ip[64]; int port; } fleet_node_t;
        fleet_node_t nodes[32];
        int node_count = 0;

        char line[512];
        while (fgets(line, sizeof(line), fp) && node_count < 32) {
            if (line[0] != '=') continue;

            char *fields[10];
            int nfields = 0;
            char *p = line;
            while (nfields < 10) {
                fields[nfields++] = p;
                char *sep = strchr(p, ';');
                if (!sep) break;
                *sep = '\0';
                p = sep + 1;
            }
            if (nfields < 9) continue;

            char *node_name = fields[3];
            char *node_ip   = fields[7];
            char *node_port = fields[8];

            /* Trim trailing whitespace */
            int plen = strlen(node_port);
            while (plen > 0 && (node_port[plen-1] == '\n' || node_port[plen-1] == '\r' || node_port[plen-1] == ' '))
                node_port[--plen] = '\0';

            /* Фильтруем self, IPv6, loopback */
            if (strcmp(node_name, hostname) == 0) continue;
            if (strchr(node_ip, ':') != NULL) continue;
            if (strncmp(node_ip, "127.", 4) == 0) continue;

            /* Валидация IP: только [0-9.], max 15 chars, структурная проверка */
            {
                int ip_len = strlen(node_ip);
                int valid_ip = 1;
                int dots = 0;
                if (ip_len < 7 || ip_len > 15) valid_ip = 0;
                for (int c = 0; valid_ip && c < ip_len; c++) {
                    if (node_ip[c] == '.') dots++;
                    else if (node_ip[c] < '0' || node_ip[c] > '9') valid_ip = 0;
                }
                if (dots != 3) valid_ip = 0;
                if (!valid_ip) continue; /* skip malicious mDNS entries */
            }

            /* Проверяем дубликаты (avahi может вернуть несколько записей) */
            int dup = 0;
            for (int d = 0; d < node_count; d++) {
                if (strcmp(nodes[d].ip, node_ip) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            strncpy(nodes[node_count].name, node_name, sizeof(nodes[0].name) - 1);
            nodes[node_count].name[sizeof(nodes[0].name) - 1] = '\0';
            strncpy(nodes[node_count].ip, node_ip, sizeof(nodes[0].ip) - 1);
            nodes[node_count].ip[sizeof(nodes[0].ip) - 1] = '\0';
            nodes[node_count].port = atoi(node_port);
            node_count++;
        }
        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        /* Для каждой ноды — запрашиваем /health и /firmware/version */
        int pos = 0;
        int max_len = (int)sizeof(response);
        pos += snprintf(response + pos, max_len - pos, "{\"nodes\":[");
        CLAMP_POS(pos, max_len);

        for (int i = 0; i < node_count; i++) {
            if (i > 0) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }

            char esc_name[256], esc_ip[128];
            json_escape(nodes[i].name, esc_name, sizeof(esc_name));
            json_escape(nodes[i].ip, esc_ip, sizeof(esc_ip));

            /* GET /health — с замером RTT */
            char curl_cmd[512];
            snprintf(curl_cmd, sizeof(curl_cmd),
                "curl -s -w '\\n%%{time_total}' --max-time 3 "
                "\"http://%s:%d/health\" 2>/dev/null",
                nodes[i].ip, nodes[i].port);

            struct timespec t_start, t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_start);

            signal(SIGCHLD, SIG_DFL);
            FILE *hfp = popen(curl_cmd, "r");
            char health_buf[2048] = "";
            int health_total = 0;
            if (hfp) {
                char tmp[256];
                while (fgets(tmp, sizeof(tmp), hfp) && health_total < (int)sizeof(health_buf) - 256) {
                    int n = strlen(tmp);
                    memcpy(health_buf + health_total, tmp, n);
                    health_total += n;
                }
                health_buf[health_total] = '\0';
                pclose(hfp);
            }
            signal(SIGCHLD, SIG_IGN);

            clock_gettime(CLOCK_MONOTONIC, &t_end);
            long rtt_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                          (t_end.tv_nsec - t_start.tv_nsec) / 1000000;

            /* Если нода не ответила */
            if (health_total == 0) {
                pos += snprintf(response + pos, max_len - pos,
                    "{\"name\":\"%s\",\"ip\":\"%s\",\"port\":%d,\"error\":\"timeout\"}",
                    esc_name, esc_ip, nodes[i].port);
                CLAMP_POS(pos, max_len);
                continue;
            }

            /* Парсим type из /health ответа */
            char node_type[64] = "unknown";
            {
                char *tp = strstr(health_buf, "\"type\"");
                if (tp) {
                    tp = strchr(tp, ':');
                    if (tp) {
                        tp = strchr(tp, '"');
                        if (tp) {
                            tp++;
                            char *end = strchr(tp, '"');
                            if (end && (end - tp) < (int)sizeof(node_type)) {
                                memcpy(node_type, tp, end - tp);
                                node_type[end - tp] = '\0';
                            }
                        }
                    }
                }
            }

            /* Парсим uptime из /health ответа */
            long node_uptime = 0;
            {
                char *up = strstr(health_buf, "\"uptime_sec\"");
                if (up) {
                    up = strchr(up + 11, ':');
                    if (up) node_uptime = atol(up + 1);
                }
            }

            /* GET /firmware/version */
            snprintf(curl_cmd, sizeof(curl_cmd),
                "curl -s --max-time 3 "
                "\"http://%s:%d/firmware/version\" 2>/dev/null",
                nodes[i].ip, nodes[i].port);

            signal(SIGCHLD, SIG_DFL);
            FILE *vfp = popen(curl_cmd, "r");
            char ver_buf[1024] = "";
            int ver_total = 0;
            if (vfp) {
                char tmp[256];
                while (fgets(tmp, sizeof(tmp), vfp) && ver_total < (int)sizeof(ver_buf) - 256) {
                    int n = strlen(tmp);
                    memcpy(ver_buf + ver_total, tmp, n);
                    ver_total += n;
                }
                ver_buf[ver_total] = '\0';
                pclose(vfp);
            }
            signal(SIGCHLD, SIG_IGN);

            /* Парсим version */
            char node_version[64] = "unknown";
            {
                char *vp = strstr(ver_buf, "\"version\"");
                if (vp) {
                    vp = strchr(vp, ':');
                    if (vp) {
                        vp = strchr(vp, '"');
                        if (vp) {
                            vp++;
                            char *end = strchr(vp, '"');
                            if (end && (end - vp) < (int)sizeof(node_version)) {
                                memcpy(node_version, vp, end - vp);
                                node_version[end - vp] = '\0';
                            }
                        }
                    }
                }
            }

            char esc_type[128], esc_ver[128];
            json_escape(node_type, esc_type, sizeof(esc_type));
            json_escape(node_version, esc_ver, sizeof(esc_ver));

            pos += snprintf(response + pos, max_len - pos,
                "{\"name\":\"%s\",\"ip\":\"%s\",\"port\":%d,"
                "\"type\":\"%s\",\"uptime\":%ld,\"version\":\"%s\",\"rtt_ms\":%ld}",
                esc_name, esc_ip, nodes[i].port,
                esc_type, node_uptime, esc_ver, rtt_ms);
            CLAMP_POS(pos, max_len);
        }

        pos += snprintf(response + pos, max_len - pos, "],\"count\":%d}", node_count);
        CLAMP_POS(pos, max_len);

        event_add("fleet: status query, %d nodes found", node_count);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /fleet/exec — Broadcast HTTP запрос на все ноды в mesh */
    if (strcmp(req.path, "/fleet/exec") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty body, need {path, method}\"}");
            return;
        }

        /* Парсим path */
        char exec_path[256] = "";
        {
            char *p = strstr(req.body, "\"path\"");
            if (p) {
                p = strchr(p + 6, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(exec_path)) {
                            memcpy(exec_path, p, end - p);
                            exec_path[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим method */
        char exec_method[16] = "";
        {
            char *p = strstr(req.body, "\"method\"");
            if (p) {
                p = strchr(p + 8, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(exec_method)) {
                            memcpy(exec_method, p, end - p);
                            exec_method[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим body (опционально) */
        char exec_body[4096] = "";
        {
            char *p = strstr(req.body, "\"body\"");
            if (p) {
                p = strchr(p + 6, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(exec_body)) {
                            memcpy(exec_body, p, end - p);
                            exec_body[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим token (опционально) */
        char exec_token[256] = "";
        {
            char *p = strstr(req.body, "\"token\"");
            if (p) {
                p = strchr(p + 7, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(exec_token)) {
                            memcpy(exec_token, p, end - p);
                            exec_token[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Валидация path — должен начинаться с / */
        if (exec_path[0] != '/') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"path must start with /\"}");
            return;
        }

        /* Валидация method — только GET, POST, DELETE */
        if (strcmp(exec_method, "GET") != 0 && strcmp(exec_method, "POST") != 0 &&
            strcmp(exec_method, "DELETE") != 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"method must be GET, POST, or DELETE\"}");
            return;
        }

        /* Валидация token — только [a-zA-Z0-9] (если предоставлен) */
        if (exec_token[0] != '\0') {
            for (const char *tp = exec_token; *tp; tp++) {
                if (!((*tp >= 'a' && *tp <= 'z') || (*tp >= 'A' && *tp <= 'Z') ||
                      (*tp >= '0' && *tp <= '9'))) {
                    send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in token\"}");
                    return;
                }
            }
        }

        /* Валидация path chars — [a-zA-Z0-9/_.-] */
        for (const char *pp = exec_path; *pp; pp++) {
            if (!((*pp >= 'a' && *pp <= 'z') || (*pp >= 'A' && *pp <= 'Z') ||
                  (*pp >= '0' && *pp <= '9') || *pp == '/' || *pp == '_' ||
                  *pp == '.' || *pp == '-')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in path\"}");
                return;
            }
        }

        /* mDNS discovery */
        char hostname[256] = "unknown";
        gethostname(hostname, sizeof(hostname));

        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen("/usr/bin/timeout 3 /usr/bin/avahi-browse -r -p _seed._tcp 2>/dev/null", "r");
        if (!fp) {
            signal(SIGCHLD, SIG_IGN);
            send_response(fd, 500, "Error", "{\"error\":\"avahi-browse failed\"}");
            return;
        }

        typedef struct { char name[128]; char ip[64]; int port; } fleet_exec_node_t;
        fleet_exec_node_t nodes[32];
        int node_count = 0;

        char line[512];
        while (fgets(line, sizeof(line), fp) && node_count < 32) {
            if (line[0] != '=') continue;

            char *fields[10];
            int nfields = 0;
            char *p = line;
            while (nfields < 10) {
                fields[nfields++] = p;
                char *sep = strchr(p, ';');
                if (!sep) break;
                *sep = '\0';
                p = sep + 1;
            }
            if (nfields < 9) continue;

            char *node_name = fields[3];
            char *node_ip   = fields[7];
            char *node_port = fields[8];

            int plen = strlen(node_port);
            while (plen > 0 && (node_port[plen-1] == '\n' || node_port[plen-1] == '\r' || node_port[plen-1] == ' '))
                node_port[--plen] = '\0';

            if (strcmp(node_name, hostname) == 0) continue;
            if (strchr(node_ip, ':') != NULL) continue;
            if (strncmp(node_ip, "127.", 4) == 0) continue;

            /* Валидация IP: только [0-9.], max 15 chars, 3 dots */
            {
                int ip_len = strlen(node_ip);
                int valid_ip = 1;
                int dots = 0;
                if (ip_len < 7 || ip_len > 15) valid_ip = 0;
                for (int c = 0; valid_ip && c < ip_len; c++) {
                    if (node_ip[c] == '.') dots++;
                    else if (node_ip[c] < '0' || node_ip[c] > '9') valid_ip = 0;
                }
                if (dots != 3) valid_ip = 0;
                if (!valid_ip) continue;
            }

            /* Проверяем дубликаты */
            int dup = 0;
            for (int d = 0; d < node_count; d++) {
                if (strcmp(nodes[d].ip, node_ip) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            strncpy(nodes[node_count].name, node_name, sizeof(nodes[0].name) - 1);
            nodes[node_count].name[sizeof(nodes[0].name) - 1] = '\0';
            strncpy(nodes[node_count].ip, node_ip, sizeof(nodes[0].ip) - 1);
            nodes[node_count].ip[sizeof(nodes[0].ip) - 1] = '\0';
            nodes[node_count].port = atoi(node_port);
            node_count++;
        }
        pclose(fp);
        signal(SIGCHLD, SIG_IGN);

        /* Для каждой ноды — отправляем HTTP запрос */
        int pos = 0;
        int max_len = (int)sizeof(response);
        pos += snprintf(response + pos, max_len - pos, "{\"results\":[");
        CLAMP_POS(pos, max_len);

        for (int i = 0; i < node_count; i++) {
            if (i > 0) { pos += snprintf(response + pos, max_len - pos, ","); CLAMP_POS(pos, max_len); }

            char esc_name[256], esc_ip[128];
            json_escape(nodes[i].name, esc_name, sizeof(esc_name));
            json_escape(nodes[i].ip, esc_ip, sizeof(esc_ip));

            /* Формируем curl команду */
            char curl_cmd[8192];
            int cpos = 0;
            cpos += snprintf(curl_cmd + cpos, sizeof(curl_cmd) - cpos,
                "curl -s -w '\\n%%{http_code}' --max-time 5 -X %s ", exec_method);

            /* Authorization header (если token предоставлен) */
            if (exec_token[0] != '\0') {
                cpos += snprintf(curl_cmd + cpos, sizeof(curl_cmd) - cpos,
                    "-H \"Authorization: Bearer %s\" ", exec_token);
            }

            /* Body (если предоставлен и метод POST) */
            char body_tmpfile[64] = "";
            if (exec_body[0] != '\0' && strcmp(exec_method, "POST") == 0) {
                /* Экранируем body для shell — записываем во временный файл */
                strcpy(body_tmpfile, "/tmp/fleet_exec_XXXXXX");
                int tmpfd = mkstemp(body_tmpfile);
                FILE *tf = tmpfd >= 0 ? fdopen(tmpfd, "w") : NULL;
                if (tf) {
                    /* Декодируем JSON escape sequences в body */
                    for (const char *bp = exec_body; *bp; bp++) {
                        if (*bp == '\\' && *(bp+1) == 'n') { fputc('\n', tf); bp++; }
                        else if (*bp == '\\' && *(bp+1) == 't') { fputc('\t', tf); bp++; }
                        else if (*bp == '\\' && *(bp+1) == '"') { fputc('"', tf); bp++; }
                        else if (*bp == '\\' && *(bp+1) == '\\') { fputc('\\', tf); bp++; }
                        else fputc(*bp, tf);
                    }
                    fclose(tf);
                    cpos += snprintf(curl_cmd + cpos, sizeof(curl_cmd) - cpos,
                        "-H \"Content-Type: application/octet-stream\" --data-binary @\"%s\" ", body_tmpfile);
                }
            }

            cpos += snprintf(curl_cmd + cpos, sizeof(curl_cmd) - cpos,
                "\"http://%s:%d%s\" 2>/dev/null",
                nodes[i].ip, nodes[i].port, exec_path);

            struct timespec t_start, t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_start);

            signal(SIGCHLD, SIG_DFL);
            FILE *cfp = popen(curl_cmd, "r");
            char curl_output[4096] = "";
            int curl_total = 0;
            if (cfp) {
                char tmp[256];
                while (fgets(tmp, sizeof(tmp), cfp) && curl_total < (int)sizeof(curl_output) - 256) {
                    int n = strlen(tmp);
                    memcpy(curl_output + curl_total, tmp, n);
                    curl_total += n;
                }
                curl_output[curl_total] = '\0';
                pclose(cfp);
            }
            signal(SIGCHLD, SIG_IGN);

            clock_gettime(CLOCK_MONOTONIC, &t_end);
            long rtt_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                          (t_end.tv_nsec - t_start.tv_nsec) / 1000000;

            /* Удаляем временный файл */
            if (body_tmpfile[0] != '\0') unlink(body_tmpfile);

            /* Извлекаем HTTP status code (последняя строка после curl -w) */
            int status_code = 0;
            char *last_nl = strrchr(curl_output, '\n');
            if (last_nl && last_nl > curl_output) {
                *last_nl = '\0';
                char *prev_nl = strrchr(curl_output, '\n');
                if (prev_nl) {
                    /* body\nstatus_code\n — status code между двумя \n */
                    status_code = atoi(prev_nl + 1);
                    *prev_nl = '\0';  /* Отрезаем status code из response */
                } else {
                    /* body\nstatus_code (без trailing \n) — status code после last_nl */
                    status_code = atoi(last_nl + 1);
                    /* curl_output уже обрезан до body (last_nl = '\0') */
                }
            } else if (curl_total > 0) {
                /* Только status code, без newline (пустой body) */
                status_code = atoi(curl_output);
                curl_output[0] = '\0';
            }

            char esc_resp[8192];
            json_escape(curl_output, esc_resp, sizeof(esc_resp));

            pos += snprintf(response + pos, max_len - pos,
                "{\"node\":\"%s\",\"ip\":\"%s\",\"status_code\":%d,"
                "\"response\":\"%s\",\"rtt_ms\":%ld}",
                esc_name, esc_ip, status_code, esc_resp, rtt_ms);
            CLAMP_POS(pos, max_len);
        }

        pos += snprintf(response + pos, max_len - pos, "],\"count\":%d}", node_count);
        CLAMP_POS(pos, max_len);

        event_add("fleet: exec %s %s on %d nodes", exec_method, exec_path, node_count);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* POST /fetch — Скачать файл по URL */
    if (strcmp(req.path, "/fetch") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty body, need {url, save_to}\"}");
            return;
        }

        /* Парсим url */
        char fetch_url[2048] = "";
        {
            char *p = strstr(req.body, "\"url\"");
            if (p) {
                p = strchr(p + 4, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(fetch_url)) {
                            memcpy(fetch_url, p, end - p);
                            fetch_url[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим save_to */
        char save_to[512] = "";
        {
            char *p = strstr(req.body, "\"save_to\"");
            if (p) {
                p = strchr(p + 9, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(save_to)) {
                            memcpy(save_to, p, end - p);
                            save_to[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Валидация url — должен начинаться с http:// или https:// */
        if (strncmp(fetch_url, "http://", 7) != 0 && strncmp(fetch_url, "https://", 8) != 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"url must start with http:// or https://\"}");
            return;
        }

        /* SSRF protection — блокируем loopback и metadata endpoints */
        {
            /* Извлекаем host часть из URL (после ://) */
            const char *scheme_end = strstr(fetch_url, "://") + 3;
            const char *host_start = strchr(scheme_end, '/');
            /* host_part = от :// до первого / или конца строки */
            char host_part[512] = "";
            int host_len = host_start ? (int)(host_start - scheme_end) : (int)strlen(scheme_end);
            if (host_len > 0 && host_len < (int)sizeof(host_part)) {
                memcpy(host_part, scheme_end, host_len);
                host_part[host_len] = '\0';
            }
            /* Убираем порт (:XXXX) для проверки */
            char host_only[512];
            strncpy(host_only, host_part, sizeof(host_only) - 1);
            host_only[sizeof(host_only) - 1] = '\0';
            char *colon = strrchr(host_only, ':');
            if (colon) *colon = '\0';

            /* Блокируемые хосты */
            const char *blocked[] = {
                "127.0.0.1", "localhost", "::1", "[::1]",
                "0.0.0.0", "169.254.169.254", NULL
            };
            for (int i = 0; blocked[i]; i++) {
                if (strcasecmp(host_only, blocked[i]) == 0) {
                    send_response(fd, 403, "Forbidden",
                        "{\"error\":\"SSRF blocked: loopback/metadata addresses not allowed\"}");
                    return;
                }
            }
        }

        /* Валидация url chars — [a-zA-Z0-9._:/-?&=%+~#@!] */
        for (const char *up = fetch_url; *up; up++) {
            if (!((*up >= 'a' && *up <= 'z') || (*up >= 'A' && *up <= 'Z') ||
                  (*up >= '0' && *up <= '9') || *up == '.' || *up == '_' ||
                  *up == ':' || *up == '/' || *up == '-' || *up == '?' ||
                  *up == '&' || *up == '=' || *up == '%' || *up == '+' ||
                  *up == '~' || *up == '#' || *up == '@' || *up == '!')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in url\"}");
                return;
            }
        }

        /* Валидация save_to — не пусто */
        if (save_to[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"save_to required\"}");
            return;
        }

        /* Валидация save_to path chars — [a-zA-Z0-9/_.-] */
        for (const char *sp = save_to; *sp; sp++) {
            if (!((*sp >= 'a' && *sp <= 'z') || (*sp >= 'A' && *sp <= 'Z') ||
                  (*sp >= '0' && *sp <= '9') || *sp == '/' || *sp == '_' ||
                  *sp == '.' || *sp == '-')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in save_to\"}");
                return;
            }
        }

        /* Path validation: whitelist check BEFORE any directory creation */
        {
            char dir_buf[512];
            strncpy(dir_buf, save_to, sizeof(dir_buf) - 1);
            dir_buf[sizeof(dir_buf) - 1] = '\0';
            char *last_slash = strrchr(dir_buf, '/');
            if (!last_slash || last_slash == dir_buf) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"save_to must be absolute path with directory\"}");
                return;
            }
            *last_slash = '\0';

            /* Check raw path against whitelist BEFORE creating anything */
            if (strncmp(dir_buf, "/etc/seed", 13) != 0 &&
                strncmp(dir_buf, "/var/seed", 13) != 0 &&
                strncmp(dir_buf, "/tmp", 4) != 0 &&
                strncmp(dir_buf, "/opt/seed", 15) != 0) {
                send_response(fd, 403, "Forbidden", "{\"error\":\"save_to path not allowed\"}");
                return;
            }

            /* Now safe to create directory (pure C, no system()) */
            {
                char tmp[PATH_MAX];
                strncpy(tmp, dir_buf, sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                for (char *p = tmp + 1; *p; p++) {
                    if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
                }
                mkdir(tmp, 0755);
            }

            /* Verify with realpath after creation */
            char real_dir[PATH_MAX];
            if (!realpath(dir_buf, real_dir)) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid save_to directory\"}");
                return;
            }
            if (strncmp(real_dir, "/etc/seed/", 14) != 0 &&
                strncmp(real_dir, "/var/seed/", 14) != 0 &&
                strncmp(real_dir, "/tmp/", 5) != 0 &&
                strncmp(real_dir, "/opt/seed/", 16) != 0 &&
                strcmp(real_dir, "/etc/seed") != 0 &&
                strcmp(real_dir, "/var/seed") != 0 &&
                strcmp(real_dir, "/tmp") != 0 &&
                strcmp(real_dir, "/opt/seed") != 0) {
                send_response(fd, 403, "Forbidden", "{\"error\":\"save_to path not allowed\"}");
                return;
            }
        }

        /* Выполняем curl для скачивания */
        char curl_cmd[4096];
        snprintf(curl_cmd, sizeof(curl_cmd),
            "curl -s --max-redirs 0 -o \"%s\" -w '%%{http_code}' "
            "--max-filesize 10485760 --max-time 60 "
            "\"%s\" 2>/dev/null",
            save_to, fetch_url);

        signal(SIGCHLD, SIG_DFL);
        FILE *fp = popen(curl_cmd, "r");
        char status_buf[32] = "";
        if (fp) {
            if (fgets(status_buf, sizeof(status_buf), fp) == NULL)
                status_buf[0] = '\0';
            pclose(fp);
        }
        signal(SIGCHLD, SIG_IGN);

        int http_status = atoi(status_buf);

        /* Проверяем что файл скачан */
        struct stat st;
        if (stat(save_to, &st) != 0) {
            snprintf(response, sizeof(response),
                "{\"ok\":false,\"error\":\"download failed\",\"status_code\":%d}", http_status);
            send_response(fd, 502, "Bad Gateway", response);
            return;
        }

        /* Проверяем размер (max 10MB) */
        if (st.st_size > 10 * 1024 * 1024) {
            unlink(save_to);
            send_response(fd, 413, "Payload Too Large", "{\"error\":\"file exceeds 10MB limit\"}");
            return;
        }

        char esc_path[1024];
        json_escape(save_to, esc_path, sizeof(esc_path));

        snprintf(response, sizeof(response),
            "{\"ok\":true,\"path\":\"%s\",\"size\":%ld,\"status_code\":%d}",
            esc_path, (long)st.st_size, http_status);

        event_add("fetch: downloaded %ld bytes to %s", (long)st.st_size, save_to);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* ========== Agent Notes — межагентная коммуникация (bulletin board) ========== */

    /* POST /notes — оставить заметку
     * Body JSON: {from: "agent-session-abc", body: "text", ttl_hours: 168}
     * from — кто оставил (max 128 chars), body — текст (max 4096 chars)
     * ttl_hours — время жизни в часах (default 168 = 1 неделя, max 8760 = 1 год) */
    if (strcmp(req.path, "/notes") == 0 && strcmp(req.method, "POST") == 0) {
        if (req.body_len == 0) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"empty body, need {from, body}\"}");
            return;
        }

        /* Парсим "from" */
        char note_from[NOTES_MAX_FROM + 1] = "";
        {
            char *p = strstr(req.body, "\"from\"");
            if (p) {
                p = strchr(p + 6, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        char *end = strchr(p, '"');
                        if (end && (end - p) < (int)sizeof(note_from)) {
                            memcpy(note_from, p, end - p);
                            note_from[end - p] = '\0';
                        }
                    }
                }
            }
        }

        /* Парсим "body" — может содержать escaped quotes */
        char note_body[NOTES_MAX_BODY + 1] = "";
        {
            char *p = strstr(req.body, "\"body\"");
            if (p) {
                p = strchr(p + 6, ':');
                if (p) {
                    p++; /* пропускаем ':' */
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == '"') {
                        p++;
                        /* Ищем закрывающую кавычку, учитывая escape */
                        int i = 0;
                        while (*p && i < NOTES_MAX_BODY) {
                            if (*p == '\\' && *(p + 1)) {
                                /* Копируем escape-последовательность как есть */
                                note_body[i++] = *p++;
                                if (i < NOTES_MAX_BODY) note_body[i++] = *p++;
                            } else if (*p == '"') {
                                break;
                            } else {
                                note_body[i++] = *p++;
                            }
                        }
                        note_body[i] = '\0';
                    }
                }
            }
        }

        /* Парсим "ttl_hours" (опционально, default 168) */
        int ttl_hours = NOTES_DEFAULT_TTL;
        {
            char *p = strstr(req.body, "\"ttl_hours\"");
            if (p) {
                p = strchr(p + 11, ':');
                if (p) {
                    p++;
                    while (*p == ' ' || *p == '\t') p++;
                    ttl_hours = atoi(p);
                    if (ttl_hours <= 0) ttl_hours = NOTES_DEFAULT_TTL;
                    if (ttl_hours > NOTES_MAX_TTL) ttl_hours = NOTES_MAX_TTL;
                }
            }
        }

        /* Валидация */
        if (note_from[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"'from' is required\"}");
            return;
        }
        if (note_body[0] == '\0') {
            send_response(fd, 400, "Bad Request", "{\"error\":\"'body' is required\"}");
            return;
        }

        /* Создаём /etc/seed директорию (pure C, без system()) */
        {
            char tmp[256];
            strncpy(tmp, SEED_DIR, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            for (char *p = tmp + 1; *p; p++) {
                if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
            }
            mkdir(tmp, 0755);
        }

        /* Генерируем ID: timestamp + 4 hex random (из /dev/urandom) */
        char note_id[64];
        time_t now = time(NULL);
        {
            unsigned short rnd = 0;
            FILE *urand = fopen("/dev/urandom", "r");
            if (urand) { fread(&rnd, sizeof(rnd), 1, urand); fclose(urand); }
            else { rnd = (unsigned short)(now ^ getpid()); }
            snprintf(note_id, sizeof(note_id), "%ld_%04x", (long)now, rnd);
        }

        /* Escape строковых полей для JSON */
        char esc_id[128], esc_from[NOTES_MAX_FROM * 2], esc_body[NOTES_MAX_BODY * 2];
        json_escape(note_id, esc_id, sizeof(esc_id));
        json_escape(note_from, esc_from, sizeof(esc_from));
        json_escape(note_body, esc_body, sizeof(esc_body));

        /* Новая запись JSON */
        int entry_cap = (int)(sizeof(esc_id) + sizeof(esc_from) + sizeof(esc_body) + 256);
        char *new_entry = malloc(entry_cap);
        if (!new_entry) {
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }
        snprintf(new_entry, entry_cap,
            "{\"id\":\"%s\",\"from\":\"%s\",\"body\":\"%s\","
            "\"created\":%ld,\"ttl_hours\":%d}",
            esc_id, esc_from, esc_body, (long)now, ttl_hours);

        /* Читаем существующий notes.json */
        char *notes_data = NULL;
        long nlen = 0;
        FILE *nf = fopen(NOTES_FILE, "r");
        if (nf) {
            fseek(nf, 0, SEEK_END);
            nlen = ftell(nf);
            fseek(nf, 0, SEEK_SET);
            if (nlen > 0) {
                notes_data = malloc(nlen + 1);
                if (notes_data) {
                    int n = fread(notes_data, 1, nlen, nf);
                    notes_data[n] = '\0';
                    nlen = n;
                }
            }
            fclose(nf);
        }

        /* Записываем обновлённый notes.json */
        nf = fopen(NOTES_FILE, "w");
        if (nf) {
            if (notes_data && nlen > 2) {
                /* Ищем последний ] в существующем массиве */
                char *last_bracket = strrchr(notes_data, ']');
                if (last_bracket) {
                    fwrite(notes_data, 1, last_bracket - notes_data, nf);
                    fprintf(nf, ",%s]", new_entry);
                } else {
                    fprintf(nf, "[%s]", new_entry);
                }
            } else {
                fprintf(nf, "[%s]", new_entry);
            }
            fclose(nf);
        } else {
            if (notes_data) free(notes_data);
            free(new_entry);
            send_response(fd, 500, "Error", "{\"error\":\"cannot write notes.json\"}");
            return;
        }
        if (notes_data) free(notes_data);
        free(new_entry);

        event_add("notes: new note %s from %s", note_id, note_from);
        snprintf(response, sizeof(response), "{\"ok\":true,\"id\":\"%s\"}", esc_id);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* GET /notes — прочитать все заметки (с auto-expire и ?since= фильтром)
     * Auto-expire: удаляем заметки с истёкшим TTL, перезаписываем файл
     * ?since=UNIX_TIMESTAMP — вернуть только заметки новее указанного времени */
    if (strncmp(req.path, "/notes", 6) == 0 &&
        (req.path[6] == '\0' || req.path[6] == '?') &&
        strcmp(req.method, "GET") == 0) {

        /* Парсим ?since= параметр */
        time_t since = 0;
        char *q = strchr(req.path, '?');
        if (q) {
            char *sp = strstr(q, "since=");
            if (sp) {
                char *endptr = NULL;
                since = (time_t)strtol(sp + 6, &endptr, 10);
                if (endptr == sp + 6) {
                    send_response(fd, 400, "Bad Request",
                        "{\"error\":\"since must be a unix timestamp\"}");
                    return;
                }
            }
        }

        time_t now = time(NULL);

        /* Читаем notes.json */
        char *notes_data = NULL;
        long nlen = 0;
        FILE *nf = fopen(NOTES_FILE, "r");
        if (nf) {
            fseek(nf, 0, SEEK_END);
            nlen = ftell(nf);
            fseek(nf, 0, SEEK_SET);
            if (nlen > 0) {
                notes_data = malloc(nlen + 1);
                if (notes_data) {
                    int n = fread(notes_data, 1, nlen, nf);
                    notes_data[n] = '\0';
                    nlen = n;
                }
            }
            fclose(nf);
        }

        if (!notes_data || nlen < 2) {
            if (notes_data) free(notes_data);
            send_response(fd, 200, "OK", "{\"notes\":[],\"count\":0}");
            return;
        }

        /* Первый проход: собираем все объекты, фильтруем expired.
         * Храним указатели на start/end + created/expires для сортировки newest first. */
        typedef struct { char *start; char *end; long created; long expires; } note_ref_t;
        int max_notes = 0;
        { char *s = notes_data; while ((s = strchr(s, '{')) != NULL) { max_notes++; s++; } }
        if (max_notes == 0) {
            free(notes_data);
            send_response(fd, 200, "OK", "{\"notes\":[],\"count\":0}");
            return;
        }

        note_ref_t *refs = malloc(max_notes * sizeof(note_ref_t));
        if (!refs) {
            free(notes_data);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int valid_count = 0;
        int had_expired = 0;
        char *p = notes_data;
        while ((p = strchr(p, '{')) != NULL) {
            char *obj_start = p;
            int depth = 0;
            char *obj_end = NULL;
            for (char *scan = p; *scan; scan++) {
                if (*scan == '{') depth++;
                else if (*scan == '}') {
                    depth--;
                    if (depth == 0) { obj_end = scan; break; }
                }
            }
            if (!obj_end) break;

            /* Извлекаем created */
            long created = 0;
            {
                char *cp = strstr(obj_start, "\"created\"");
                if (cp && cp < obj_end) {
                    cp = strchr(cp + 9, ':');
                    if (cp) { cp++; while (*cp == ' ') cp++; created = strtol(cp, NULL, 10); }
                }
            }

            /* Извлекаем ttl_hours */
            int ttl = NOTES_DEFAULT_TTL;
            {
                char *tp = strstr(obj_start, "\"ttl_hours\"");
                if (tp && tp < obj_end) {
                    tp = strchr(tp + 11, ':');
                    if (tp) { tp++; while (*tp == ' ') tp++; ttl = atoi(tp); }
                }
            }

            long expires = created + (long)ttl * 3600;

            if (expires <= (long)now) {
                had_expired = 1;
            } else {
                refs[valid_count].start = obj_start;
                refs[valid_count].end = obj_end;
                refs[valid_count].created = created;
                refs[valid_count].expires = expires;
                valid_count++;
            }
            p = obj_end + 1;
        }

        /* Если были expired — перезаписываем файл без них */
        if (had_expired) {
            nf = fopen(NOTES_FILE, "w");
            if (nf) {
                fprintf(nf, "[");
                for (int i = 0; i < valid_count; i++) {
                    if (i > 0) fprintf(nf, ",");
                    int obj_len = (int)(refs[i].end - refs[i].start + 1);
                    fwrite(refs[i].start, 1, obj_len, nf);
                }
                fprintf(nf, "]");
                fclose(nf);
            }
        }

        /* Формируем ответ — newest first (обратный порядок) */
        int resp_cap = (int)nlen * 2 + 256;
        char *resp_buf = malloc(resp_cap);
        if (!resp_buf) {
            free(refs);
            free(notes_data);
            send_response(fd, 500, "Error", "{\"error\":\"malloc failed\"}");
            return;
        }

        int resp_pos = 0;
        int count = 0;
        resp_pos += snprintf(resp_buf + resp_pos, resp_cap - resp_pos, "{\"notes\":[");

        for (int i = valid_count - 1; i >= 0; i--) {
            /* Фильтр ?since= */
            if (since > 0 && refs[i].created <= (long)since) continue;

            int obj_len = (int)(refs[i].end - refs[i].start + 1);
            if (count > 0)
                resp_pos += snprintf(resp_buf + resp_pos, resp_cap - resp_pos, ",");
            /* Вставляем объект, добавляя expires перед закрывающей } */
            resp_pos += snprintf(resp_buf + resp_pos, resp_cap - resp_pos,
                "%.*s,\"expires\":%ld}", obj_len - 1, refs[i].start, refs[i].expires);
            count++;
        }

        resp_pos += snprintf(resp_buf + resp_pos, resp_cap - resp_pos, "],\"count\":%d}", count);

        send_response(fd, 200, "OK", resp_buf);
        free(resp_buf);
        free(refs);
        free(notes_data);
        return;
    }

    /* DELETE /notes/{id} — удалить заметку */
    if (strncmp(req.path, "/notes/", 7) == 0 && strcmp(req.method, "DELETE") == 0) {
        const char *note_id = req.path + 7;

        /* ID validation: только [a-zA-Z0-9_] */
        if (strlen(note_id) == 0 || strlen(note_id) > 60) {
            send_response(fd, 400, "Bad Request", "{\"error\":\"invalid note ID\"}");
            return;
        }
        for (const char *p = note_id; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) {
                send_response(fd, 400, "Bad Request", "{\"error\":\"invalid chars in ID\"}");
                return;
            }
        }

        /* Читаем notes.json */
        char *notes_data = NULL;
        long nlen = 0;
        FILE *nf = fopen(NOTES_FILE, "r");
        if (nf) {
            fseek(nf, 0, SEEK_END);
            nlen = ftell(nf);
            fseek(nf, 0, SEEK_SET);
            if (nlen > 0) {
                notes_data = malloc(nlen + 1);
                if (notes_data) {
                    int n = fread(notes_data, 1, nlen, nf);
                    notes_data[n] = '\0';
                    nlen = n;
                }
            }
            fclose(nf);
        }

        if (!notes_data || nlen < 2) {
            if (notes_data) free(notes_data);
            send_response(fd, 404, "Not Found", "{\"error\":\"note not found\"}");
            return;
        }

        /* Ищем объект с данным id */
        char search[128];
        snprintf(search, sizeof(search), "\"id\":\"%s\"", note_id);
        char *found = strstr(notes_data, search);
        if (!found) {
            free(notes_data);
            send_response(fd, 404, "Not Found", "{\"error\":\"note not found\"}");
            return;
        }

        /* Ищем начало объекта { перед найденным id */
        char *obj_start = found;
        while (obj_start > notes_data && *obj_start != '{') obj_start--;

        /* Ищем конец объекта } после найденного id */
        char *obj_end = found;
        int depth = 0;
        char *scan = obj_start;
        for (; *scan; scan++) {
            if (*scan == '{') depth++;
            else if (*scan == '}') { depth--; if (depth == 0) { obj_end = scan; break; } }
        }

        /* Удаляем объект + запятую (до или после) */
        char *del_start = obj_start;
        char *del_end = obj_end + 1;

        /* Убираем запятую: перед объектом или после */
        if (*del_end == ',') {
            del_end++; /* удаляем запятую после */
        } else if (del_start > notes_data) {
            char *before = del_start - 1;
            while (before > notes_data && (*before == ' ' || *before == '\n' || *before == '\r' || *before == '\t')) before--;
            if (*before == ',') del_start = before; /* удаляем запятую перед */
        }

        /* Записываем notes.json без удалённого объекта */
        nf = fopen(NOTES_FILE, "w");
        if (nf) {
            fwrite(notes_data, 1, del_start - notes_data, nf);
            fwrite(del_end, 1, strlen(del_end), nf);
            fclose(nf);
        }
        free(notes_data);

        char esc_id[128];
        json_escape(note_id, esc_id, sizeof(esc_id));
        event_add("notes: deleted note %s", note_id);
        snprintf(response, sizeof(response), "{\"ok\":true,\"id\":\"%s\"}", esc_id);
        send_response(fd, 200, "OK", response);
        return;
    }

    /* Известные пути — если путь совпадает, но метод неверный → 405 */
    static const char *known_paths[] = {
        "/health", "/status", "/exec", "/read", "/type", "/info",
        "/serial/list", "/serial/switch", "/serial/enable",
        "/keyboard/type", "/keyboard/keys", "/keyboard/status", "/keyboard/command",
        "/gpio/", "/i2c/", "/config.md", "/capabilities",
        "/deploy", "/deploy/logs", "/live.md", "/events", "/mesh",
        "/system/exec", "/system/reboot", "/notes", "/notes/", "/hosts", "/host/info",
        "/host/exec", "/skill", "/pair", "/token",
        "/wg/status", "/wg/config", "/wg/peer", "/wg/peers",
        "/firmware/version", "/firmware/source", "/firmware/build",
        "/firmware/build/logs", "/firmware/apply",
        "/net/scan", "/net/mdns", "/net/wifi",
        "/net/ping/", "/net/probe/", "/net/interfaces", "/net/ports",
        "/proc/stats", "/proc/list",
        "/fs/ls/", "/fs/read/", "/fs/write/",
        "/vault/store", "/vault/list", "/vault/push/", "/vault/",
        "/fleet/status", "/fleet/exec", "/fetch",
        NULL
    };
    for (int i = 0; known_paths[i]; i++) {
        int plen = strlen(known_paths[i]);
        /* Для путей с '/' на конце — проверяем префикс, иначе точное совпадение */
        if (known_paths[i][plen - 1] == '/') {
            if (strncmp(req.path, known_paths[i], plen) == 0) {
                send_response(fd, 405, "Method Not Allowed", "{\"error\":\"method not allowed\"}");
                return;
            }
        } else {
            if (strcmp(req.path, known_paths[i]) == 0) {
                send_response(fd, 405, "Method Not Allowed", "{\"error\":\"method not allowed\"}");
                return;
            }
        }
    }

    send_response(fd, 404, "Not Found", "{\"error\":\"not found\"}");
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;

    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) strncpy(serial_device, argv[2], sizeof(serial_device) - 1);
    if (argc > 3) serial_baudrate = atoi(argv[3]);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);  /* Не создаём зомби-процессы от fork() в firmware/apply */
    server_start_time = time(NULL);

    token_load();
    host_load_profile();
    serial_open();

    /* Seed — создаём директории, определяем GPIO base, сканируем */
    mkdir(SEED_DIR, 0755);
    mkdir(SEED_SCRIPTS, 0755);
    gpio_detect_base();
    gpio_scan_configured();

    /* Начальная проверка USB */
    usb_up = check_usb_state();

    pthread_t reader;
    pthread_create(&reader, NULL, serial_reader, NULL);

    pthread_t monitor;
    pthread_create(&monitor, NULL, host_monitor, NULL);
    pthread_detach(monitor);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    listen(srv, 8);
    fprintf(stderr, "seed (Seed) listening on port %d\n", port);
    fprintf(stderr, "Serial: %s at %d baud (%s)\n", serial_device, serial_baudrate,
            serial_fd >= 0 ? "connected" : "waiting");
    fprintf(stderr, "Keyboard: %s (%s)\n", HID_DEV,
            hid_is_available() ? "ready" : "not available");
    fprintf(stderr, "USB: %s (%s)\n", USB_IF, usb_up ? "up" : "down");
    fprintf(stderr, "Host monitor: started (interval %ds)\n", MONITOR_INTERVAL);
    if (host_paired)
        fprintf(stderr, "Host: %s@%s (paired)\n", host_user, host_ip);
    fprintf(stderr, "Seed: GPIO (%d pins tracked), I2C (%s), config (%s)\n",
            gpio_pin_count, I2C_BUS_DEV, SEED_CONFIG);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(srv, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) continue;

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        struct timeval tv = { .tv_sec = 5 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_request(client, client_ip);
        close(client);
    }

    return 0;
}
