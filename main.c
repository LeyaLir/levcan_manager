// can-browser.c — интерактивный обозреватель LEVCAN-меню для Linux
//
// Использует ncurses вместо SSD1306 и сокет CAN (socketCAN) вместо TWAI.
// Интерфейс CAN задаётся параметром командной строки (по умолчанию "can0").
//
// Сборка:
//   gcc -O2 -Wall -o can-browser can-browser.c -lncurses -liconv
//
// Запуск:
//   ./can-browser [-d] [-c кодировка] [can0]
//   -d            — включить отладочный журнал в can.log
//   -c кодировка  — кодировка LEVCAN-строк (по умолчанию "cp1251")
//
// Клавиши:
//   ESC/q     = Back / Выход
//   ↑/k       = Вверх
//   ↓/j       = Вниз
//   Enter     = Select (войти / применить)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <iconv.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>
#include <ncurses.h>
#include <locale.h>

// ====== LEVCAN константы ======
#define MY_ADDR              100
#define BROADCAST            127
#define MSG_ADDRESS_CLAIMED  0x380
#define MSG_NODE_NAME        0x388
#define MSG_DEVICE_NAME      0x389
#define MSG_PARAM_REQUEST    0x399
#define MSG_PARAM_DATA       0x39A
#define MSG_PARAM_DESCRIPTOR 0x39B
#define MSG_PARAM_NAME       0x39C
#define MSG_PARAM_TEXT       0x39D
#define MSG_PARAM_VALUE      0x39E

#define LCP_REQ_DATA          0x01
#define LCP_REQ_NAME          0x02
#define LCP_REQ_TEXT          0x04
#define LCP_REQ_DESCRIPTOR    0x08
#define LCP_REQ_VARIABLE      0x10
#define LCP_REQ_DATA_NAME     0x03
#define LCP_REQ_FULL_ENTRY    0x1F
#define LCP_REQ_DIRECTORY_INFO 0x20
#define LCP_REQ_VALUE_SET     0x40

enum LCP_Type {
    LCP_Folder=0, LCP_Label, LCP_Bool, LCP_Enum, LCP_Bitfield32,
    LCP_Int32, LCP_Uint32, LCP_Int64, LCP_Uint64, LCP_Float,
    LCP_Double, LCP_Decimal32, LCP_String, LCP_End=0xFF
};

#define LCP_MODE_RO       0x01
#define LCP_MODE_WO       0x02
#define LCP_MODE_LIVE_UPD 0x04
#define LCP_MODE_LIVE_CHG 0x08

// ====== Пределы ======
#define MAX_NODES        8
#define MAX_ENTRIES_DIR  64
#define NAME_BUF         48
#define TEXT_BUF         96
#define DESC_BUF         48
#define ASM_BUF          128
#define DEPTH_STACK      8
#define UTF8_BUF         256  // Буфер для UTF-8 (4 байта на символ — с запасом)

// ====== Временные константы (мс) ======
#define BROADCAST_PERIOD 5000
#define LIVE_PERIOD      700
#define DISCOVER_MIN_MS  2500

// ====== Состояния приложения ======
typedef enum {
    S_DISCOVER,
    S_DEVICES,
    S_LOAD_DIR,
    S_BROWSE,
    S_EDIT
} AppState;

// ====== Структуры данных ======
typedef struct {
    bool     present;
    uint8_t  addr;
    char     deviceName[NAME_BUF];
    char     nodeName[NAME_BUF];
    bool     hasDeviceName;
    bool     hasNodeName;
    uint64_t lastSeenMs;
} Node;

typedef struct {
    uint8_t  type;
    uint8_t  mode;
    uint16_t entryIdx;
    uint16_t varSize;
    uint16_t descSize;
    uint16_t textSize;
    bool     hasData, hasName, hasDesc, hasText, hasValue;
    char     name[NAME_BUF];
    char     text[TEXT_BUF];
    uint8_t  desc[DESC_BUF];
    uint8_t  descLen;
    uint8_t  value[16];
    uint8_t  valueLen;
} EntryInfo;

typedef struct {
    bool     known;
    uint16_t entrySize;
    uint16_t nameSize;
    char     name[NAME_BUF];
    EntryInfo entries[MAX_ENTRIES_DIR];
} DirInfo;

typedef struct {
    uint16_t dirIndex;
    uint8_t  cursor;
    uint8_t  scrollTop;
} StackFrame;

// ====== Заголовок LEVCAN ======
typedef struct {
    uint8_t  source;
    uint8_t  target;
    uint16_t msgId;
    uint8_t  eom;
    uint8_t  parity;
    uint8_t  rts;
    uint8_t  prio;
} LcHeader;

// ====== Глобальные переменные ======
static int can_socket = -1;
static AppState appState = S_DISCOVER;
static FILE* log_file = NULL;
static iconv_t iconv_cd = (iconv_t)-1;  // Дескриптор конвертера

static Node nodes[MAX_NODES];
static uint8_t nodeCount = 0;
static int selectedNode = -1;
static uint8_t targetAddr = 0;

static DirInfo curDir;
static uint16_t curDirIndex = 0;
static uint8_t cursor = 0;
static uint8_t scrollTop = 0;

static StackFrame dirStack[DEPTH_STACK];
static uint8_t dirStackPos = 0;

static uint64_t lastBroadcastMs = 0;
static uint64_t lastLiveUpdMs = 0;
static uint64_t discoverStartMs = 0;

// Редактор
static struct {
    uint16_t entryIdx;
    uint8_t  type;
    uint16_t varSize;
    bool     isSigned;
    int32_t  vmin, vmax, vstep;
    uint16_t count;
    uint16_t cursor;
    uint16_t scrollTop;
    uint8_t  decimals;
} editor;

// Сборщик multi-frame
typedef struct {
    bool     active;
    uint16_t msgId;
    uint16_t pos;
    uint8_t  buf[ASM_BUF];
    uint8_t  srcAddr;
} Channel;

static const uint16_t CHAN_IDS[] = {
    MSG_PARAM_DATA, MSG_PARAM_DESCRIPTOR, MSG_PARAM_NAME,
    MSG_PARAM_TEXT, MSG_PARAM_VALUE, MSG_NODE_NAME, MSG_DEVICE_NAME
};
#define CHAN_COUNT (sizeof(CHAN_IDS)/sizeof(CHAN_IDS[0]))

static Channel channels[CHAN_COUNT];

// Ожидание запроса
typedef enum { WK_NONE, WK_DIR, WK_ENTRY, WK_VALUE, WK_SET } WaitKind;
static struct {
    WaitKind kind;
    uint8_t  target;
    uint16_t dirIndex;
    uint16_t entryIndex;
    uint64_t startMs;
} pending;

// Результат установки значения
static struct {
    bool    received;
    uint8_t errorCode;
} setResult;

// ====== Логирование ======
#define LOG(fmt, ...) do { \
    if (log_file) { \
        uint64_t ts = getMs(); \
        fprintf(log_file, "[%6lu.%03lu] " fmt, \
                (unsigned long)(ts / 1000), \
                (unsigned long)(ts % 1000), \
                ##__VA_ARGS__); \
        fflush(log_file); \
    } \
} while(0)

// ====== Утилиты ======
static uint64_t getMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t packId(const LcHeader* h) {
    uint32_t id = 0;
    id |= (uint32_t)(h->source & 0x7F);
    id |= (uint32_t)(h->target & 0x7F) << 7;
    id |= (uint32_t)(h->msgId  & 0x3FF) << 14;
    id |= (uint32_t)(h->eom    & 0x1)   << 24;
    id |= (uint32_t)(h->parity & 0x1)   << 25;
    id |= (uint32_t)(h->rts    & 0x1)   << 26;
    id |= (uint32_t)(h->prio   & 0x3)   << 27;
    return id;
}

static LcHeader unpackId(uint32_t id) {
    LcHeader h;
    h.source = id & 0x7F;
    h.target = (id >> 7) & 0x7F;
    h.msgId  = (id >> 14) & 0x3FF;
    h.eom    = (id >> 24) & 0x1;
    h.parity = (id >> 25) & 0x1;
    h.rts    = (id >> 26) & 0x1;
    h.prio   = (id >> 27) & 0x3;
    return h;
}

// ====== Конвертация через libiconv ======
// Преобразует srcLen байт из src (в исходной кодировке) в UTF-8.
// Результат помещается в dst размера dstSize, всегда терминируется '\0'.
// Возвращает количество байт в dst (без терминатора), либо 0 при ошибке.
static size_t convertToUtf8(const uint8_t* src, uint16_t srcLen, char* dst, size_t dstSize) {
    if (!src || srcLen == 0 || !dst || dstSize == 0) {
        if (dst && dstSize > 0) dst[0] = '\0';
        return 0;
    }

    // Если конвертер не инициализирован (не нашли кодировку), делаем простую замену:
    // все байты >= 0x80 заменяем на '?'
    if (iconv_cd == (iconv_t)-1) {
        size_t di = 0;
        for (uint16_t si = 0; si < srcLen && di + 1 < dstSize; si++) {
            if (src[si] < 0x80) dst[di++] = (char)src[si];
            else dst[di++] = '?';
        }
        dst[di] = '\0';
        return di;
    }

    // Подготавливаем буферы для iconv
    char* in_buf = (char*)src;
    size_t in_left = srcLen;
    char* out_buf = dst;
    size_t out_left = dstSize - 1;  // оставляем место для '\0'

    // Сбрасываем состояние конвертера
    iconv(iconv_cd, NULL, NULL, NULL, NULL);

    size_t ret = iconv(iconv_cd, &in_buf, &in_left, &out_buf, &out_left);
    if (ret == (size_t)-1) {
        // При ошибке — заполняем остаток '?'
        if (errno == EILSEQ || errno == EINVAL) {
            // Пропускаем проблемный байт и продолжаем
            size_t converted = out_buf - dst;
            size_t remaining = dstSize - converted - 1;
            if (remaining > 0) {
                memset(out_buf, '?', 1);
                out_buf++;
                remaining--;
            }
            // Копируем остаток как '?'
            while (in_left > 0 && remaining > 0) {
                *out_buf++ = '?';
                in_buf++;
                in_left--;
                remaining--;
            }
        }
    }

    *out_buf = '\0';
    return out_buf - dst;
}

// ====== Работа со списком узлов ======
static int findOrAddNode(uint8_t addr) {
    for (int i = 0; i < nodeCount; i++)
        if (nodes[i].addr == addr) return i;
    if (nodeCount >= MAX_NODES) return -1;
    int idx = nodeCount++;
    memset(&nodes[idx], 0, sizeof(Node));
    nodes[idx].present = true;
    nodes[idx].addr = addr;
    snprintf(nodes[idx].deviceName, NAME_BUF, "node%u", addr);
    nodes[idx].lastSeenMs = getMs();
    return idx;
}

static const char* nodeDisplayName(const Node* n) {
    if (n->hasNodeName   && n->nodeName[0]   != 0) return n->nodeName;
    if (n->hasDeviceName && n->deviceName[0] != 0) return n->deviceName;
    return n->deviceName;
}

// ====== CAN сокет ======
static bool initCAN(const char* ifname) {
    LOG("Opening CAN interface: %s\n", ifname);

    can_socket = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (can_socket < 0) {
        perror("socket");
        LOG("ERROR: socket() failed: %s\n", strerror(errno));
        return false;
    }

    struct ifreq ifr;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(can_socket, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl");
        LOG("ERROR: ioctl() failed: %s\n", strerror(errno));
        close(can_socket);
        return false;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(can_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        LOG("ERROR: bind() failed: %s\n", strerror(errno));
        close(can_socket);
        return false;
    }

    // Неблокирующий режим
    int flags = fcntl(can_socket, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        LOG("ERROR: fcntl(F_GETFL) failed: %s\n", strerror(errno));
        close(can_socket);
        return false;
    }
    if (fcntl(can_socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        LOG("ERROR: fcntl(F_SETFL) failed: %s\n", strerror(errno));
        close(can_socket);
        return false;
    }

    LOG("CAN interface %s opened successfully\n", ifname);
    return true;
}

static bool sendFrame(uint32_t id, const uint8_t* data, uint8_t len, bool rtr) {
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id = id | CAN_EFF_FLAG;
    if (rtr) frame.can_id |= CAN_RTR_FLAG;
    frame.can_dlc = len > 8 ? 8 : len;
    if (data && !rtr) memcpy(frame.data, data, frame.can_dlc);

    ssize_t n = write(can_socket, &frame, sizeof(frame));
    bool ret = (n == sizeof(frame));

    if (log_file) {
        LcHeader h = unpackId(id);
        LOG("TX: src=%u dst=%u msg=0x%03X eom=%u par=%u rts=%u prio=%u len=%u %s %s\n",
            h.source, h.target, h.msgId, h.eom, h.parity, h.rts, h.prio,
            len, rtr ? "RTR" : "DATA",
            ret ? "OK" : "FAIL");
    }

    return ret;
}

// ====== Запросы ======
static bool reqDir(uint8_t target, uint16_t dirIndex) {
    LcHeader h = {0};
    h.source = MY_ADDR; h.target = target; h.msgId = MSG_PARAM_REQUEST;
    h.eom = 1; h.parity = 1; h.rts = 1; h.prio = 0;
    uint8_t pl[4] = {
        (uint8_t)(LCP_REQ_DIRECTORY_INFO & 0xFF),
        (uint8_t)(LCP_REQ_DIRECTORY_INFO >> 8),
        (uint8_t)(dirIndex & 0xFF),
        (uint8_t)(dirIndex >> 8)
    };
    LOG("Requesting directory info: target=%u dir=%u\n", target, dirIndex);
    return sendFrame(packId(&h), pl, 4, false);
}

static bool reqEntryCmd(uint8_t target, uint16_t dirIndex, uint16_t entryIndex, uint16_t cmdMask) {
    LcHeader h = {0};
    h.source = MY_ADDR; h.target = target; h.msgId = MSG_PARAM_REQUEST;
    h.eom = 1; h.parity = 1; h.rts = 1; h.prio = 0;
    uint8_t pl[6] = {
        (uint8_t)(cmdMask & 0xFF),
        (uint8_t)(cmdMask >> 8),
        (uint8_t)(dirIndex & 0xFF),
        (uint8_t)(dirIndex >> 8),
        (uint8_t)(entryIndex & 0xFF),
        (uint8_t)(entryIndex >> 8)
    };
    return sendFrame(packId(&h), pl, 6, false);
}

static bool reqEntry(uint8_t target, uint16_t dir, uint16_t e) {
    LOG("Requesting entry data+name: target=%u dir=%u entry=%u\n", target, dir, e);
    return reqEntryCmd(target, dir, e, LCP_REQ_DATA_NAME);
}
static bool reqEntryVariable(uint8_t target, uint16_t dir, uint16_t e) {
    LOG("Requesting entry value: target=%u dir=%u entry=%u\n", target, dir, e);
    return reqEntryCmd(target, dir, e, LCP_REQ_VARIABLE);
}
static bool reqEntryDescriptor(uint8_t target, uint16_t dir, uint16_t e) {
    LOG("Requesting entry descriptor: target=%u dir=%u entry=%u\n", target, dir, e);
    return reqEntryCmd(target, dir, e, LCP_REQ_DESCRIPTOR);
}
static bool reqEntryText(uint8_t target, uint16_t dir, uint16_t e) {
    LOG("Requesting entry text: target=%u dir=%u entry=%u\n", target, dir, e);
    return reqEntryCmd(target, dir, e, LCP_REQ_TEXT);
}

static bool reqSysName(uint8_t target, uint16_t msgId) {
    LcHeader h = {0};
    h.source = MY_ADDR; h.target = target; h.msgId = msgId;
    h.eom = 0; h.parity = 0; h.rts = 0; h.prio = 0;
    LOG("Requesting system name: target=%u msg=0x%03X\n", target, msgId);
    return sendFrame(packId(&h), NULL, 0, true);
}

static bool reqDeviceName(uint8_t target) { return reqSysName(target, MSG_DEVICE_NAME); }
static bool reqNodeName(uint8_t target)   { return reqSysName(target, MSG_NODE_NAME); }
static bool broadcastDeviceNameQuery(void) {
    LOG("Broadcasting device name query\n");
    return reqDeviceName(BROADCAST);
}

// ====== Сборка multi-frame ======
static int channelIndexFor(uint16_t msgId) {
    for (uint8_t i = 0; i < CHAN_COUNT; i++)
        if (CHAN_IDS[i] == msgId) return i;
    return -1;
}

static void resetChannel(int i) {
    memset(&channels[i], 0, sizeof(Channel));
    channels[i].msgId = CHAN_IDS[i];
}

static void resetAllChannels(void) {
    for (uint8_t i = 0; i < CHAN_COUNT; i++)
        resetChannel(i);
}

static bool sendCTS(uint8_t target, uint16_t msgId, uint8_t parity, uint8_t rts, uint8_t eom) {
    LcHeader h = {0};
    h.source = MY_ADDR; h.target = target; h.msgId = msgId;
    h.parity = parity; h.rts = rts; h.eom = eom; h.prio = 0;
    LOG("  <- CTS to %u msg=0x%X par=%u rts=%u eom=%u\n", target, msgId, parity, rts, eom);
    return sendFrame(packId(&h), NULL, 0, true);
}

// ====== Обработчики данных ======
static void onDeviceName(uint8_t srcAddr, const uint8_t* d, uint16_t len) {
    int i = findOrAddNode(srcAddr);
    if (i < 0) return;
    uint16_t n = len;
    for (uint16_t k = 0; k < n; k++)
        if (d[k] == 0) { n = k; break; }
    convertToUtf8(d, n, nodes[i].deviceName, NAME_BUF);
    nodes[i].hasDeviceName = true;
    nodes[i].lastSeenMs = getMs();
    LOG("  Node %u DeviceName=\"%s\"\n", srcAddr, nodes[i].deviceName);
    if (!nodes[i].hasNodeName) reqNodeName(srcAddr);
}

static void onNodeName(uint8_t srcAddr, const uint8_t* d, uint16_t len) {
    int i = findOrAddNode(srcAddr);
    if (i < 0) return;
    uint16_t n = len;
    for (uint16_t k = 0; k < n; k++)
        if (d[k] == 0) { n = k; break; }
    convertToUtf8(d, n, nodes[i].nodeName, NAME_BUF);
    nodes[i].hasNodeName = true;
    nodes[i].lastSeenMs = getMs();
    LOG("  Node %u NodeName=\"%s\"\n", srcAddr, nodes[i].nodeName);
}

static void onDirData(uint16_t dirIdx, const uint8_t* d, uint16_t len) {
    if (len < 6) return;
    uint16_t entrySize = d[0] | (d[1] << 8);
    uint16_t nameSize  = d[2] | (d[3] << 8);
    uint16_t actualDir = d[4] | (d[5] << 8);
    if (actualDir != dirIdx) {
        LOG("  DirData dir mismatch %u/%u\n", actualDir, dirIdx);
        return;
    }
    if (entrySize > MAX_ENTRIES_DIR || nameSize == 0 || nameSize > NAME_BUF) {
        LOG("  DirData bogus size es=%u ns=%u\n", entrySize, nameSize);
        return;
    }
    curDir.known = true;
    curDir.entrySize = entrySize;
    curDir.nameSize = nameSize;
    LOG("  DirData[%u]: entries=%u\n", dirIdx, entrySize);
}

static void onEntryData(uint16_t expectedEntry, const uint8_t* d, uint16_t len) {
    if (len < 4 || expectedEntry >= MAX_ENTRIES_DIR) return;
    uint16_t respEntry = d[2] | (d[3] << 8);
    if (respEntry != expectedEntry) {
        LOG("  Entry idx mismatch %u/%u\n", respEntry, expectedEntry);
        return;
    }
    EntryInfo* e = &curDir.entries[expectedEntry];
    e->type = d[0]; e->mode = d[1]; e->entryIdx = respEntry;
    if (len >= 6)  e->varSize  = d[4] | (d[5] << 8);
    if (len >= 8)  e->descSize = d[6] | (d[7] << 8);
    if (len >= 10) e->textSize = d[8] | (d[9] << 8);
    e->hasData = true;
#ifdef DEBUG
    LOG("  EntryData[%u]: type=%u mode=0x%02X varSz=%u\n",
        expectedEntry, e->type, e->mode, e->varSize);
#endif
}

static void onName(int16_t entryIdx, const uint8_t* d, uint16_t len) {
    uint16_t n = len;
    for (uint16_t i = 0; i < n; i++) if (d[i]==0) { n = i; break; }
    char buf[NAME_BUF];
    convertToUtf8(d, n, buf, sizeof(buf));
    if (entryIdx < 0) {
        strncpy(curDir.name, buf, NAME_BUF - 1);
        curDir.name[NAME_BUF - 1] = 0;
        LOG("  DirName: \"%s\"\n", curDir.name);
    } else if (entryIdx < MAX_ENTRIES_DIR) {
        EntryInfo* e = &curDir.entries[entryIdx];
        strncpy(e->name, buf, NAME_BUF - 1);
        e->name[NAME_BUF - 1] = 0;
        e->hasName = true;
        LOG("  EntryName[%u]: \"%s\"\n", (unsigned)entryIdx, e->name);
    }
}

static void onText(uint16_t entryIdx, const uint8_t* d, uint16_t len) {
    if (entryIdx >= MAX_ENTRIES_DIR) return;
    EntryInfo* e = &curDir.entries[entryIdx];
    uint16_t n = len;
    for (uint16_t i = 0; i < n; i++) if (d[i]==0) { n = i; break; }
    convertToUtf8(d, n, e->text, TEXT_BUF);
    e->hasText = true;
}

static void onDesc(uint16_t entryIdx, const uint8_t* d, uint16_t len) {
    if (entryIdx >= MAX_ENTRIES_DIR) return;
    EntryInfo* e = &curDir.entries[entryIdx];
    uint16_t n = (len < DESC_BUF) ? len : DESC_BUF;
    memcpy(e->desc, d, n);
    e->descLen = n;
    e->hasDesc = true;
}

static void onValue(uint16_t entryIdx, const uint8_t* d, uint16_t len) {
    LOG("  EntryValue[%u]: len=%u\n", (unsigned)entryIdx, len);
    if (entryIdx >= MAX_ENTRIES_DIR) return;
    EntryInfo* e = &curDir.entries[entryIdx];
    uint16_t n = (len < sizeof(e->value)) ? len : sizeof(e->value);
    memcpy(e->value, d, n);
    e->valueLen = n;
    e->hasValue = true;
}

// ====== Диспетчеризация каналов ======
static void dispatchChannel(int idx) {
    Channel* c = &channels[idx];
    uint16_t msgId = c->msgId;

    if (msgId == MSG_DEVICE_NAME) { onDeviceName(c->srcAddr, c->buf, c->pos); resetChannel(idx); return; }
    if (msgId == MSG_NODE_NAME)   { onNodeName(c->srcAddr, c->buf, c->pos); resetChannel(idx); return; }

    if (pending.kind == WK_NONE) { resetChannel(idx); return; }
    if (c->srcAddr != pending.target) { resetChannel(idx); return; }

    switch (pending.kind) {
        case WK_DIR:
            if (msgId == MSG_PARAM_DATA) onDirData(pending.dirIndex, c->buf, c->pos);
            else if (msgId == MSG_PARAM_NAME) onName(-1, c->buf, c->pos);
            break;
        case WK_ENTRY:
            if (msgId == MSG_PARAM_DATA) onEntryData(pending.entryIndex, c->buf, c->pos);
            else if (msgId == MSG_PARAM_NAME) onName((int16_t)pending.entryIndex, c->buf, c->pos);
            else if (msgId == MSG_PARAM_DESCRIPTOR) onDesc(pending.entryIndex, c->buf, c->pos);
            else if (msgId == MSG_PARAM_TEXT) onText(pending.entryIndex, c->buf, c->pos);
            else if (msgId == MSG_PARAM_VALUE) onValue(pending.entryIndex, c->buf, c->pos);
            break;
        case WK_VALUE:
            if (msgId == MSG_PARAM_VALUE) onValue(pending.entryIndex, c->buf, c->pos);
            break;
        case WK_SET:
            if (msgId == MSG_PARAM_DATA && c->pos >= 1) {
                setResult.errorCode = c->buf[0];
                setResult.received = true;
                LOG("  ValueSet response: errorCode=%u\n", c->buf[0]);
            }
            break;
        default:
            break;
    }
    resetChannel(idx);
}

// ====== Обработка кадра ======
static void handleFrame(void) {
    struct can_frame frame;
    ssize_t n = read(can_socket, &frame, sizeof(frame));
    if (n <= 0) return;

    if (!(frame.can_id & CAN_EFF_FLAG)) return;

    bool rtr = (frame.can_id & CAN_RTR_FLAG) != 0;
    uint32_t id = frame.can_id & CAN_EFF_MASK;
    LcHeader h = unpackId(id);

    if (h.source == MY_ADDR) return;
    if (h.target != MY_ADDR && h.target != BROADCAST) return;

    if (h.msgId == MSG_ADDRESS_CLAIMED) {
        LOG("  AddressClaimed from addr %u\n", h.source);
        int i = findOrAddNode(h.source);
        if (i >= 0) {
            nodes[i].lastSeenMs = getMs();
            if (!nodes[i].hasDeviceName) reqDeviceName(h.source);
            if (!nodes[i].hasNodeName) reqNodeName(h.source);
        }
        return;
    }

    if (rtr) {
        LOG("  RX RTR src=%u msg=0x%X rts=%u eom=%u par=%u\n",
            h.source, h.msgId, h.rts, h.eom, h.parity);
        return;
    }

    LOG("  RX 0x%X src=%u msg=0x%X eom=%u par=%u rts=%u dlc=%u\n",
        id, h.source, h.msgId, h.eom, h.parity, h.rts, frame.can_dlc);

    int chIdx = channelIndexFor(h.msgId);
    if (chIdx < 0) return;
    Channel* c = &channels[chIdx];

    if (c->active && c->srcAddr != h.source)
        resetChannel(chIdx);

    if (h.rts == 1 && h.eom == 1) {
        // single-frame
        resetChannel(chIdx);
        c->active = true;
        c->srcAddr = h.source;
        uint8_t dlc = frame.can_dlc;
        if (dlc > ASM_BUF) dlc = ASM_BUF;
        memcpy(c->buf, frame.data, dlc);
        c->pos = dlc;
        uint8_t ackParity = (~(((c->pos + 7) / 8))) & 1;
        sendCTS(h.source, h.msgId, ackParity, 0, 1);
        dispatchChannel(chIdx);
    } else if (h.rts == 1 && h.eom == 0) {
        // начало multi-frame
        resetChannel(chIdx);
        c->active = true;
        c->srcAddr = h.source;
        uint8_t dlc = frame.can_dlc;
        if (dlc > ASM_BUF) dlc = ASM_BUF;
        memcpy(c->buf, frame.data, dlc);
        c->pos = dlc;
        uint8_t nextParity = (~(((c->pos + 7) / 8))) & 1;
        sendCTS(h.source, h.msgId, nextParity, 1, 0);
    } else if (h.rts == 0) {
        // продолжение
        if (!c->active) return;
        uint16_t take = frame.can_dlc;
        if (c->pos + take > ASM_BUF) take = ASM_BUF - c->pos;
        memcpy(c->buf + c->pos, frame.data, take);
        c->pos += take;
        if (h.eom == 1) {
            uint8_t lastParity = (~(((c->pos + 7) / 8))) & 1;
            sendCTS(h.source, h.msgId, lastParity, 0, 1);
            dispatchChannel(chIdx);
        } else {
            uint8_t nextParity = (~(((c->pos + 7) / 8))) & 1;
            sendCTS(h.source, h.msgId, nextParity, 1, 0);
        }
    }
}

// ====== Ожидание с условием ======
static bool waitFor(uint64_t timeoutMs, bool (*cond)(void)) {
    uint64_t start = getMs();
    while (getMs() - start < timeoutMs) {
        struct pollfd pfd = { .fd = can_socket, .events = POLLIN };
        int ret = poll(&pfd, 1, 2);
        if (ret > 0) handleFrame();
        if (cond()) return true;
    }
    return false;
}

static bool dirNameReady(void) { return curDir.known && curDir.name[0] != 0; }
static bool entryNameReady(void) {
    EntryInfo* e = &curDir.entries[pending.entryIndex];
    return e->hasData && e->hasName;
}
static bool entryValueReady(void) {
    EntryInfo* e = &curDir.entries[pending.entryIndex];
    return e->hasValue;
}
static bool entryDescReady(void) {
    return curDir.entries[pending.entryIndex].hasDesc;
}
static bool entryTextReady(void) {
    return curDir.entries[pending.entryIndex].hasText;
}

// ====== Загрузка директории ======
static void clearDir(void) {
    memset(&curDir, 0, sizeof(curDir));
}

static void clearEntryFlags(uint8_t i) {
    if (i >= MAX_ENTRIES_DIR) return;
    EntryInfo* e = &curDir.entries[i];
    e->hasData = e->hasName = e->hasDesc = e->hasText = e->hasValue = false;
}

static bool loadDir(uint8_t target, uint16_t dirIndex) {
    LOG("[loadDir target=%u dir=%u]\n", target, dirIndex);
    resetAllChannels();
    clearDir();

    pending.kind = WK_DIR;
    pending.target = target;
    pending.dirIndex = dirIndex;
    pending.entryIndex = 0;
    pending.startMs = getMs();

    if (!reqDir(target, dirIndex)) {
        LOG("  reqDir TX fail\n");
        pending.kind = WK_NONE;
        return false;
    }
    if (!waitFor(1500, dirNameReady)) {
        LOG("  dir timeout\n");
        pending.kind = WK_NONE;
        return false;
    }
    pending.kind = WK_NONE;

    for (uint16_t i = 0; i < curDir.entrySize; i++) {
        clearEntryFlags(i);
        pending.kind = WK_ENTRY;
        pending.target = target;
        pending.dirIndex = dirIndex;
        pending.entryIndex = i;
        pending.startMs = getMs();

        bool ok = false;
        for (int retry = 0; retry < 2 && !ok; retry++) {
            if (retry > 0) resetAllChannels();
            reqEntry(target, dirIndex, i);
            ok = waitFor(800, entryNameReady);
        }
        if (!ok) {
            LOG("  entry %u: skip after retries\n", i);
            pending.kind = WK_NONE;
            continue;
        }

        EntryInfo* e = &curDir.entries[i];

        bool needDesc = (e->type == LCP_Decimal32) || (e->type == LCP_Enum)
                     || (e->type == LCP_Int32) || (e->type == LCP_Uint32)
                     || (e->type == LCP_Int64) || (e->type == LCP_Uint64);
        if (needDesc) {
            usleep(5000);
            for (int retry = 0; retry < 2; retry++) {
                if (retry > 0) resetAllChannels();
                reqEntryDescriptor(target, dirIndex, i);
                if (waitFor(800, entryDescReady)) break;
            }
        }

        uint16_t nameLen = strlen(e->name);
        bool hasRealText = (e->textSize > nameLen);
        bool needText = hasRealText && (
            e->type == LCP_Label || e->type == LCP_Enum || e->type == LCP_Bool ||
            e->type == LCP_Int32 || e->type == LCP_Uint32 ||
            e->type == LCP_Int64 || e->type == LCP_Uint64 || e->type == LCP_Decimal32);
        if (needText) {
            usleep(5000);
            reqEntryText(target, dirIndex, i);
            waitFor(600, entryTextReady);
        }

        bool needValue = (e->type != LCP_Folder) && (e->type != LCP_Label)
                      && ((e->mode & 0x02) == 0) && (e->varSize > 0);
        if (needValue) {
            usleep(5000);
            for (int retry = 0; retry < 2; retry++) {
                if (retry > 0) resetAllChannels();
                reqEntryVariable(target, dirIndex, i);
                if (waitFor(800, entryValueReady)) break;
            }
        }
        pending.kind = WK_NONE;
        usleep(5000);
    }
    return true;
}

// ====== Живое обновление ======
static bool refreshLiveValues(uint8_t target, uint16_t dirIndex) {
    for (uint16_t i = 0; i < curDir.entrySize; i++) {
        EntryInfo* e = &curDir.entries[i];
        if (!e->hasData) continue;
        if (!(e->mode & LCP_MODE_LIVE_UPD)) continue;
        if (e->type == LCP_Folder || e->type == LCP_Label) continue;
        e->hasValue = false;
        pending.kind = WK_VALUE;
        pending.target = target;
        pending.dirIndex = dirIndex;
        pending.entryIndex = i;
        pending.startMs = getMs();
        reqEntryVariable(target, dirIndex, i);
        waitFor(400, entryValueReady);
        pending.kind = WK_NONE;
    }
    return true;
}

// ====== Формат значения ======
static bool pickEnumLabel(const char* src, uint32_t idx, char* out, size_t outSize) {
    if (!src || !src[0]) return false;
    const char* p = src;
    for (uint32_t k = 0; k < idx; k++) {
        const char* nl = strchr(p, '\n');
        if (!nl) return false;
        p = nl + 1;
    }
    const char* end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= outSize) len = outSize - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

static bool checkSafeFormat(const char* fmt, char* modeOut) {
    if (!fmt || !fmt[0]) return false;
    int converters = 0;
    char mode = 0;
    for (const char* p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) continue;
        if (p[1] == '%') { p++; continue; }
        const char* q = p + 1;
        while (*q == '-' || *q == '+' || *q == ' ' || *q == '0' || *q == '#') q++;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '.') { q++; while (*q >= '0' && *q <= '9') q++; }
        while (*q == 'l' || *q == 'h') q++;
        char c = *q;
        if (c == 'd' || c == 'i')      mode = 'd';
        else if (c == 'u')             mode = 'u';
        else if (c == 's')             mode = 's';
        else                            return false;
        converters++;
        p = q;
    }
    if (converters != 1) return false;
    if (modeOut) *modeOut = mode;
    return true;
}

static bool applyTextFormat(const char* fmt, long ival, const char* sval, char* out, size_t outSize) {
    char mode = 0;
    if (!checkSafeFormat(fmt, &mode)) return false;
    if (mode == 's') snprintf(out, outSize, fmt, sval ? sval : "");
    else             snprintf(out, outSize, fmt, ival);
    return true;
}

static void formatValue(const EntryInfo* e, char* out, size_t outSize) {
    if (e->type == LCP_Label) {
        if (e->hasText) snprintf(out, outSize, "%s", e->text);
        else { out[0] = 0; }
        return;
    }
    if (!e->hasValue) { out[0] = '?'; out[1] = 0; return; }

    switch (e->type) {
        case LCP_Bool: {
            uint32_t idx = e->value[0] ? 1 : 0;
            if (e->hasText && pickEnumLabel(e->text, idx, out, outSize)) break;
            snprintf(out, outSize, "%s", idx ? "ON" : "off");
            break;
        }
        case LCP_Int32: {
            int32_t v = (int32_t)(e->value[0] | (e->value[1]<<8) | (e->value[2]<<16) | (e->value[3]<<24));
            char raw[16];
            snprintf(raw, sizeof(raw), "%ld", (long)v);
            if (e->hasText && applyTextFormat(e->text, (long)v, raw, out, outSize)) break;
            snprintf(out, outSize, "%s", raw);
            break;
        }
        case LCP_Uint32: {
            uint32_t v = (uint32_t)(e->value[0] | (e->value[1]<<8) | (e->value[2]<<16) | (e->value[3]<<24));
            char raw[16];
            snprintf(raw, sizeof(raw), "%lu", (unsigned long)v);
            if (e->hasText && applyTextFormat(e->text, (long)v, raw, out, outSize)) break;
            snprintf(out, outSize, "%s", raw);
            break;
        }
        case LCP_Decimal32: {
            int32_t v = 0;
            if      (e->valueLen >= 4) { v = (int32_t)((uint32_t)e->value[0] | ((uint32_t)e->value[1]<<8) |
                                                         ((uint32_t)e->value[2]<<16) | ((uint32_t)e->value[3]<<24)); }
            else if (e->valueLen >= 2) { int16_t s = (int16_t)((uint16_t)e->value[0] | ((uint16_t)e->value[1]<<8)); v = s; }
            else if (e->valueLen >= 1) { v = (int8_t)e->value[0]; }
            uint8_t decimals = 0;
            if (e->hasDesc && e->descLen >= 13) decimals = e->desc[12];
            char raw[20];
            if (decimals == 0) {
                snprintf(raw, sizeof(raw), "%ld", (long)v);
            } else {
                int32_t div = 1;
                for (uint8_t k = 0; k < decimals; k++) div *= 10;
                int32_t whole = v / div;
                int32_t frac  = v % div;
                if (frac < 0) frac = -frac;
                if (decimals > 6) decimals = 6;
                if (v < 0 && whole == 0)
                    snprintf(raw, sizeof(raw), "-0.%0*ld", (int)decimals, (long)frac);
                else
                    snprintf(raw, sizeof(raw), "%ld.%0*ld", (long)whole, (int)decimals, (long)frac);
            }
            if (e->hasText && applyTextFormat(e->text, (long)v, raw, out, outSize)) break;
            snprintf(out, outSize, "%s", raw);
            break;
        }
        case LCP_Float: {
            float f;
            memcpy(&f, e->value, 4);
            snprintf(out, outSize, "%.2f", f);
            break;
        }
        case LCP_Enum: {
            uint32_t v = 0;
            if      (e->valueLen >= 4) v = (uint32_t)e->value[0] | ((uint32_t)e->value[1]<<8) | ((uint32_t)e->value[2]<<16) | ((uint32_t)e->value[3]<<24);
            else if (e->valueLen >= 2) v = (uint32_t)e->value[0] | ((uint32_t)e->value[1]<<8);
            else if (e->valueLen >= 1) v = e->value[0];
            uint32_t minv = 0;
            if (e->hasDesc && e->descLen >= 4) {
                minv = (uint32_t)e->desc[0] | ((uint32_t)e->desc[1]<<8) | ((uint32_t)e->desc[2]<<16) | ((uint32_t)e->desc[3]<<24);
            }
            uint32_t idx = (v >= minv) ? (v - minv) : 0;
            if (e->hasText && pickEnumLabel(e->text, idx, out, outSize)) break;
            snprintf(out, outSize, "%lu", (unsigned long)v);
            break;
        }
        default:
            out[0] = 0;
            for (uint8_t k = 0; k < e->valueLen && k < 4; k++) {
                char tmp[4];
                snprintf(tmp, sizeof(tmp), "%02X", e->value[k]);
                if (strlen(out) + 2 < outSize) strcat(out, tmp);
            }
    }
}

// ====== Действия UI ======
static void enterDevice(int nodeIdx) {
    selectedNode = nodeIdx;
    targetAddr = nodes[nodeIdx].addr;
    dirStackPos = 0;
    curDirIndex = 0;
    cursor = 0;
    scrollTop = 0;
    appState = S_LOAD_DIR;
}

static void enterFolder(uint16_t newDirIndex) {
    if (dirStackPos < DEPTH_STACK) {
        dirStack[dirStackPos].dirIndex = curDirIndex;
        dirStack[dirStackPos].cursor = cursor;
        dirStack[dirStackPos].scrollTop = scrollTop;
        dirStackPos++;
    }
    curDirIndex = newDirIndex;
    cursor = 0;
    scrollTop = 0;
    appState = S_LOAD_DIR;
}

static void goBack(void) {
    if (dirStackPos == 0) {
        appState = S_DEVICES;
        return;
    }
    dirStackPos--;
    curDirIndex = dirStack[dirStackPos].dirIndex;
    cursor = dirStack[dirStackPos].cursor;
    scrollTop = dirStack[dirStackPos].scrollTop;
    appState = S_LOAD_DIR;
}

static bool isEditable(const EntryInfo* e) {
    if (e->type == LCP_Folder || e->type == LCP_Label || e->type == LCP_String)
        return false;
    if (e->type == LCP_Float || e->type == LCP_Double || e->type == LCP_Bitfield32)
        return false;
    if (e->mode & LCP_MODE_RO) return false;
    if (e->mode & LCP_MODE_WO) return false;
    if (e->varSize == 0) return false;
    return true;
}

static bool prepareEditor(uint16_t entryIdx) {
    EntryInfo* e = &curDir.entries[entryIdx];
    memset(&editor, 0, sizeof(editor));
    editor.entryIdx = entryIdx;
    editor.type = e->type;
    editor.varSize = e->varSize;

    #define RD_I32(off) ((int32_t)((uint32_t)e->desc[off] | ((uint32_t)e->desc[off+1] << 8) \
                    | ((uint32_t)e->desc[off+2] << 16) | ((uint32_t)e->desc[off+3] << 24)))

    switch (e->type) {
        case LCP_Bool:
            editor.vmin = 0; editor.vmax = 1; editor.vstep = 1; editor.count = 2;
            break;
        case LCP_Enum: {
            if (e->descLen < 8) return false;
            int32_t mn = RD_I32(0);
            int32_t sz = RD_I32(4);
            if (sz <= 0) sz = 1;
            if (sz > 256) sz = 256;
            editor.vmin = mn;
            editor.vmax = mn + sz - 1;
            editor.vstep = 1;
            editor.count = (uint16_t)sz;
            break;
        }
        case LCP_Int32: case LCP_Int64:
            editor.isSigned = true;
            if (e->descLen < 12) return false;
            editor.vmin = RD_I32(0);
            editor.vmax = RD_I32(4);
            editor.vstep = RD_I32(8);
            break;
        case LCP_Uint32: case LCP_Uint64:
            if (e->descLen < 12) return false;
            editor.vmin = RD_I32(0);
            editor.vmax = RD_I32(4);
            editor.vstep = RD_I32(8);
            break;
        case LCP_Decimal32:
            editor.isSigned = true;
            if (e->descLen < 13) return false;
            editor.vmin = RD_I32(0);
            editor.vmax = RD_I32(4);
            editor.vstep = RD_I32(8);
            editor.decimals = e->desc[12];
            break;
        default:
            return false;
    }
    #undef RD_I32

    if (editor.vstep <= 0) editor.vstep = 1;
    int64_t cnt = ((int64_t)editor.vmax - (int64_t)editor.vmin) / editor.vstep + 1;
    if (cnt < 1) cnt = 1;
    if (cnt > 256) cnt = 256;
    editor.count = (uint16_t)cnt;

    if (e->hasValue) {
        int32_t cur = 0;
        if      (e->valueLen >= 4) cur = (int32_t)((uint32_t)e->value[0] | ((uint32_t)e->value[1]<<8) | ((uint32_t)e->value[2]<<16) | ((uint32_t)e->value[3]<<24));
        else if (e->valueLen >= 2) cur = editor.isSigned ? (int32_t)(int16_t)((uint16_t)e->value[0] | ((uint16_t)e->value[1]<<8))
                                                         : (int32_t)((uint16_t)e->value[0] | ((uint16_t)e->value[1]<<8));
        else if (e->valueLen >= 1) cur = editor.isSigned ? (int32_t)(int8_t)e->value[0] : (int32_t)e->value[0];
        if (editor.vstep > 0 && cur >= editor.vmin && cur <= editor.vmax) {
            uint32_t idx = (uint32_t)((cur - editor.vmin) / editor.vstep);
            if (idx < editor.count) editor.cursor = (uint16_t)idx;
        }
    }
    return true;
}

static void formatEditorOption(uint16_t i, char* out, size_t outSize) {
    EntryInfo* e = &curDir.entries[editor.entryIdx];
    int32_t v = editor.vmin + (int32_t)i * editor.vstep;

    switch (editor.type) {
        case LCP_Bool: {
            uint32_t idx = v ? 1 : 0;
            if (e->hasText && pickEnumLabel(e->text, idx, out, outSize)) return;
            snprintf(out, outSize, "%s", idx ? "ON" : "off");
            return;
        }
        case LCP_Enum: {
            if (e->hasText && pickEnumLabel(e->text, i, out, outSize)) return;
            snprintf(out, outSize, "%ld", (long)v);
            return;
        }
        case LCP_Decimal32: {
            uint8_t d = editor.decimals;
            if (d > 6) d = 6;
            char raw[20];
            if (d == 0) {
                snprintf(raw, sizeof(raw), "%ld", (long)v);
            } else {
                int32_t div = 1;
                for (uint8_t k = 0; k < d; k++) div *= 10;
                int32_t whole = v / div, frac = v % div;
                if (frac < 0) frac = -frac;
                if (v < 0 && whole == 0)
                    snprintf(raw, sizeof(raw), "-0.%0*ld", (int)d, (long)frac);
                else
                    snprintf(raw, sizeof(raw), "%ld.%0*ld", (long)whole, (int)d, (long)frac);
            }
            if (e->hasText && applyTextFormat(e->text, (long)v, raw, out, outSize)) return;
            snprintf(out, outSize, "%s", raw);
            return;
        }
        default: {
            char raw[20];
            if (editor.isSigned) snprintf(raw, sizeof(raw), "%ld", (long)v);
            else                 snprintf(raw, sizeof(raw), "%lu", (unsigned long)(uint32_t)v);
            if (e->hasText && applyTextFormat(e->text, (long)v, raw, out, outSize)) return;
            snprintf(out, outSize, "%s", raw);
            return;
        }
    }
}

static bool applyEditorValue(void) {
    EntryInfo* e = &curDir.entries[editor.entryIdx];
    int32_t v = editor.vmin + (int32_t)editor.cursor * editor.vstep;
    uint8_t bytes[8] = {0};
    uint16_t sz = editor.varSize;
    if (sz > 8) sz = 8;

    uint32_t u = (uint32_t)v;
    for (uint16_t k = 0; k < sz; k++)
        bytes[k] = (uint8_t)(u >> (8*k));
    if (sz > 4) {
        uint8_t pad = (editor.isSigned && (v < 0)) ? 0xFF : 0x00;
        for (uint16_t k = 4; k < sz; k++) bytes[k] = pad;
    }

    uint16_t total = 6 + sz;
    uint8_t buf[64];
    buf[0] = (uint8_t)(LCP_REQ_VALUE_SET & 0xFF);
    buf[1] = (uint8_t)(LCP_REQ_VALUE_SET >> 8);
    buf[2] = (uint8_t)(curDirIndex & 0xFF);
    buf[3] = (uint8_t)(curDirIndex >> 8);
    buf[4] = (uint8_t)(editor.entryIdx & 0xFF);
    buf[5] = (uint8_t)(editor.entryIdx >> 8);
    memcpy(buf + 6, bytes, sz);

    LcHeader h = {0};
    h.source = MY_ADDR; h.target = targetAddr; h.msgId = MSG_PARAM_REQUEST;
    h.rts = 1; h.eom = 1; h.parity = 1; h.prio = 0;

    setResult.received = false;
    pending.kind = WK_SET;
    pending.target = targetAddr;
    pending.dirIndex = curDirIndex;
    pending.entryIndex = editor.entryIdx;
    pending.startMs = getMs();

    if (!sendFrame(packId(&h), buf, (uint8_t)total, false)) {
        pending.kind = WK_NONE;
        return false;
    }

    uint64_t t0 = getMs();
    while (getMs() - t0 < 1000 && !setResult.received) {
        struct pollfd pfd = { .fd = can_socket, .events = POLLIN };
        poll(&pfd, 1, 2);
        handleFrame();
    }
    pending.kind = WK_NONE;

    if (!setResult.received) return false;
    if (setResult.errorCode == 0) {
        e->valueLen = (uint8_t)sz;
        memcpy(e->value, bytes, sz);
        e->hasValue = true;
    }
    return setResult.errorCode == 0;
}

static void handleSelect(void) {
    if (cursor >= curDir.entrySize) return;
    EntryInfo* e = &curDir.entries[cursor];
    if (e->type == LCP_Folder) {
        uint16_t childDir = e->varSize;
        enterFolder(childDir);
    } else if (isEditable(e)) {
        if (prepareEditor(cursor)) {
            appState = S_EDIT;
        }
    }
}

// ====== NCURSES отрисовка ======
static void initNcurses(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    start_color();
    init_pair(1, COLOR_BLACK, COLOR_WHITE);
    init_pair(2, COLOR_WHITE, COLOR_BLACK);
    init_pair(3, COLOR_BLACK, COLOR_WHITE);
}

static bool pollKey(int* key) {
    int c = getch();
    if (c == ERR) return false;
    *key = c;
    return true;
}

static void drawDiscover(void) {
    erase();
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    attron(COLOR_PAIR(3));
    mvhline(0, 0, ' ', max_x);
    mvprintw(0, 1, "Searching CAN...");
    attroff(COLOR_PAIR(3));

    mvprintw(2, 1, "Found: %u", nodeCount);
    for (uint8_t i = 0; i < nodeCount && i < (uint8_t)(max_y - 5); i++) {
        mvprintw(4 + i, 1, " %u %.32s", nodes[i].addr, nodeDisplayName(&nodes[i]));
    }

    if (getMs() - discoverStartMs > DISCOVER_MIN_MS && nodeCount > 0) {
        attron(COLOR_PAIR(3));
        mvhline(max_y - 1, 0, ' ', max_x);
        mvprintw(max_y - 1, 1, "Enter=Continue");
        attroff(COLOR_PAIR(3));
    }
    refresh();
}

static void drawDevices(void) {
    erase();
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    attron(COLOR_PAIR(3));
    mvhline(0, 0, ' ', max_x);
    mvprintw(0, 1, "Devices");
    attroff(COLOR_PAIR(3));

    for (uint8_t i = 0; i < nodeCount && i < (uint8_t)(max_y - 3); i++) {
        bool sel = ((int)i == selectedNode);
        if (sel) attron(COLOR_PAIR(1));
        else     attron(COLOR_PAIR(2));
        mvhline(2 + i, 0, ' ', max_x);
        mvprintw(2 + i, 1, "%u %.48s", nodes[i].addr, nodeDisplayName(&nodes[i]));
        if (sel) attroff(COLOR_PAIR(1));
        else     attroff(COLOR_PAIR(2));
    }

    attron(COLOR_PAIR(3));
    mvhline(max_y - 1, 0, ' ', max_x);
    mvprintw(max_y - 1, 1, "Up/Dn Sel=Open Esc=Scan");
    attroff(COLOR_PAIR(3));
    refresh();
}

static void drawLoading(const char* what) {
    erase();
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    attron(COLOR_PAIR(3));
    mvhline(0, 0, ' ', max_x);
    mvprintw(0, 1, "Loading...");
    attroff(COLOR_PAIR(3));

    mvprintw(max_y / 2, (max_x - (int)strlen(what)) / 2, "%s", what);
    refresh();
}

static void drawBrowse(void) {
    erase();
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int vis_rows = max_y - 3;

    attron(COLOR_PAIR(3));
    mvhline(0, 0, ' ', max_x);
    mvprintw(0, 1, "%.*s", max_x - 2, curDir.name);
    attroff(COLOR_PAIR(3));

    if ((int)cursor < scrollTop) scrollTop = cursor;
    if ((int)cursor >= scrollTop + vis_rows) scrollTop = cursor - vis_rows + 1;
    if (curDir.entrySize <= (uint16_t)vis_rows) scrollTop = 0;

    for (int row = 0; row < vis_rows; row++) {
        uint8_t i = scrollTop + row;
        if (i >= curDir.entrySize) break;
        EntryInfo* e = &curDir.entries[i];
        bool sel = (i == cursor);

        if (sel) attron(COLOR_PAIR(1));
        else attron(COLOR_PAIR(2));

        mvhline(1 + row, 0, ' ', max_x);

        char val[32] = "";
        if (e->hasValue || e->type == LCP_Label) formatValue(e, val, sizeof(val));
        char vs[17];
        size_t L = strlen(val);
        if (L <= 16) strcpy(vs, val);
        else { memcpy(vs, val, 15); vs[15] = '~'; vs[16] = 0; }

        bool hasPrefix = (e->type == LCP_Folder) || (e->type == LCP_Label);
        char prefix = (e->type == LCP_Folder) ? '>' : (e->type == LCP_Label ? '#' : ' ');

        int prefixW = hasPrefix ? 2 : 0;
        int valW = (int)strlen(vs) + (strlen(vs) > 0 ? 1 : 0);
        int nameW = max_x - prefixW - valW - 3;
        if (nameW < 8) nameW = 8;

        char nm[64];
        L = strlen(e->name);
        if ((int)L <= nameW) strcpy(nm, e->name);
        else { memcpy(nm, e->name, nameW - 1); nm[nameW - 1] = '~'; nm[nameW] = 0; }

        if (hasPrefix) mvprintw(1 + row, 1, "%c %-*s%*s", prefix, nameW, nm, valW, vs);
        else           mvprintw(1 + row, 1, "  %-*s%*s",          nameW, nm, valW, vs);

        if (sel) attroff(COLOR_PAIR(1));
        else attroff(COLOR_PAIR(2));
    }

    const char* selHint = "";
    if (cursor < curDir.entrySize) {
        EntryInfo* ec = &curDir.entries[cursor];
        if (ec->type == LCP_Folder) selHint = " Enter=open";
        else if (isEditable(ec))    selHint = " Enter=edit";
    }
    attron(COLOR_PAIR(3));
    mvhline(max_y - 1, 0, ' ', max_x);
    mvprintw(max_y - 1, 1, "%u/%u %s%s", cursor + 1, curDir.entrySize,
             dirStackPos > 0 ? "Esc=back" : "Esc=devs", selHint);
    attroff(COLOR_PAIR(3));
    refresh();
}

static void drawEditor(void) {
    erase();
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int vis_rows = max_y - 3;

    EntryInfo* e = &curDir.entries[editor.entryIdx];

    attron(COLOR_PAIR(3));
    mvhline(0, 0, ' ', max_x);
    mvprintw(0, 1, "%.*s", max_x - 2, e->name);
    attroff(COLOR_PAIR(3));

    if ((int)editor.cursor < editor.scrollTop) editor.scrollTop = editor.cursor;
    if ((int)editor.cursor >= editor.scrollTop + vis_rows) editor.scrollTop = editor.cursor - vis_rows + 1;
    if (editor.count <= (uint16_t)vis_rows) editor.scrollTop = 0;

    for (int row = 0; row < vis_rows; row++) {
        uint16_t i = editor.scrollTop + row;
        if (i >= editor.count) break;
        bool sel = (i == editor.cursor);

        if (sel) attron(COLOR_PAIR(1));
        else attron(COLOR_PAIR(2));

        mvhline(1 + row, 0, ' ', max_x);
        char buf[64];
        formatEditorOption(i, buf, sizeof(buf));
        mvprintw(1 + row, 2, "%s", buf);

        if (sel) attroff(COLOR_PAIR(1));
        else attroff(COLOR_PAIR(2));
    }

    attron(COLOR_PAIR(3));
    mvhline(max_y - 1, 0, ' ', max_x);
    mvprintw(max_y - 1, 1, "%u/%u  Enter=apply Esc=cancel", editor.cursor + 1, editor.count);
    attroff(COLOR_PAIR(3));
    refresh();
}

// ====== Главный цикл ======
int main(int argc, char** argv) {
    const char* ifname = "can0";
    const char* charset = "cp1251";  // Кодировка по умолчанию
    bool debug = false;

    // Разбор аргументов
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            debug = true;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            charset = argv[++i];
        } else {
            ifname = argv[i];
        }
    }

    // Открываем лог-файл если нужен
    if (debug) {
        log_file = fopen("can.log", "a");
        if (!log_file) {
            fprintf(stderr, "Warning: cannot open can.log for writing\n");
        } else {
            time_t now = time(NULL);
            fprintf(log_file, "\n=== Started at %s", ctime(&now));
            fflush(log_file);
        }
    }

    // Инициализируем конвертер кодировки
    iconv_cd = iconv_open("UTF-8", charset);
    if (iconv_cd == (iconv_t)-1) {
        fprintf(stderr, "Warning: cannot initialize iconv from '%s' to UTF-8: %s\n",
                charset, strerror(errno));
        fprintf(stderr, "Non-ASCII characters will be replaced with '?'\n");
    } else {
        printf("Using charset conversion: %s -> UTF-8\n", charset);
    }

    if (!initCAN(ifname)) {
        fprintf(stderr, "Failed to open CAN interface %s\n", ifname);
        if (log_file) fclose(log_file);
        if (iconv_cd != (iconv_t)-1) iconv_close(iconv_cd);
        return 1;
    }
    printf("LEVCAN browser on %s (ncurses UI, press q to quit)\n", ifname);

    initNcurses();
    atexit((void(*)(void))endwin);

    resetAllChannels();
    discoverStartMs = getMs();
    broadcastDeviceNameQuery();
    lastBroadcastMs = discoverStartMs;

    while (true) {
        struct pollfd pfd = { .fd = can_socket, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            handleFrame();
        }

        int key;
        bool pressed = pollKey(&key);
        uint64_t now = getMs();

        if (pressed) {
            if (key == 'q' || key == 'Q') break;

            switch (appState) {
                case S_DISCOVER:
                    if (key == '\n' && nodeCount > 0 && (now - discoverStartMs) > DISCOVER_MIN_MS) {
                        selectedNode = 0;
                        appState = S_DEVICES;
                    }
                    break;

                case S_DEVICES:
                    if (key == KEY_UP || key == 'k') {
                        if (selectedNode > 0) selectedNode--;
                    } else if (key == KEY_DOWN || key == 'j') {
                        if (selectedNode + 1 < (int)nodeCount) selectedNode++;
                    } else if (key == 27 || key == 'q') {
                        nodeCount = 0;
                        selectedNode = -1;
                        appState = S_DISCOVER;
                        discoverStartMs = now;
                        broadcastDeviceNameQuery();
                        lastBroadcastMs = now;
                    } else if (key == '\n' && selectedNode >= 0) {
                        enterDevice(selectedNode);
                    }
                    break;

                case S_LOAD_DIR:
                    break;

                case S_BROWSE:
                    if (key == KEY_UP || key == 'k') {
                        if (cursor > 0) cursor--;
                    } else if (key == KEY_DOWN || key == 'j') {
                        if (cursor + 1 < curDir.entrySize) cursor++;
                    } else if (key == 27 || key == 'q') {
                        goBack();
                    } else if (key == '\n') {
                        handleSelect();
                    }
                    break;

                case S_EDIT:
                    if (key == KEY_UP || key == 'k') {
                        if (editor.cursor > 0) editor.cursor--;
                    } else if (key == KEY_DOWN || key == 'j') {
                        if (editor.cursor + 1 < editor.count) editor.cursor++;
                    } else if (key == 27 || key == 'q') {
                        appState = S_BROWSE;
                    } else if (key == '\n') {
                        applyEditorValue();
                        appState = S_BROWSE;
                    }
                    break;
            }
        }

        if (appState == S_DISCOVER && now - lastBroadcastMs > BROADCAST_PERIOD) {
            broadcastDeviceNameQuery();
            for (uint8_t k = 0; k < nodeCount; k++) {
                if (!nodes[k].hasNodeName) reqNodeName(nodes[k].addr);
            }
            lastBroadcastMs = now;
        }

        if (appState == S_LOAD_DIR) {
            bool ok = loadDir(targetAddr, curDirIndex);
            if (ok) {
                appState = S_BROWSE;
                lastLiveUpdMs = now;
            } else {
                appState = S_DEVICES;
            }
        }

        if (appState == S_BROWSE && now - lastLiveUpdMs > LIVE_PERIOD) {
            lastLiveUpdMs = now;
            refreshLiveValues(targetAddr, curDirIndex);
        }

        switch (appState) {
            case S_DISCOVER:    drawDiscover();    break;
            case S_DEVICES:     drawDevices();     break;
            case S_LOAD_DIR:    drawLoading("Loading menu..."); break;
            case S_BROWSE:      drawBrowse();      break;
            case S_EDIT:        drawEditor();      break;
        }

        usleep(5000);
    }

    endwin();
    close(can_socket);

    if (iconv_cd != (iconv_t)-1) {
        iconv_close(iconv_cd);
    }

    if (log_file) {
        LOG("Exiting.\n");
        fclose(log_file);
    }

    return 0;
}