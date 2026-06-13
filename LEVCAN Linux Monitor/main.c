#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>
#include <locale.h>
#include <stdarg.h>
#include <iconv.h>
#include <errno.h>
#include <ncurses.h>

#include "levcan.h"
#include "levcan_paramclient.h"

#define MY_LEVCAN_ADDR 33
#define MAX_NODES 128
#define MAX_DIRS 32
#define MAX_PARAMS 64

typedef enum { S_DEVICES, S_PARAMS, S_LOADING } AppState;

typedef struct {
    uint8_t  NodeID;
    char     Name[96];
    uint16_t DeviceType;
    uint16_t ManufacturerCode;
    uint16_t SerialNumber;
    bool     active;
    bool     confirmed;
} DiscoveredNode_t;

typedef struct {
    char     name[96];
    uint8_t  type;
    uint16_t childDir;
    bool     isFolder;
    bool     hasValue;
    char     value[64];
} ParamInfo_t;

typedef struct {
    uint16_t index;
    char     name[96];
    uint16_t entryCount;
    ParamInfo_t entries[MAX_PARAMS];
    bool     loaded;
} DirInfo_t;

static DiscoveredNode_t nodeTable[MAX_NODES];
static int totalNodes = 0;
static int activeCursor = 0;
static int selectedNodeId = -1;
static AppState appState = S_DEVICES;
static LC_NodeDescriptor_t* myNode = NULL;
static volatile int running = 1;

static DirInfo_t dirs[MAX_DIRS];
static int dirCount = 0;
static int currentDir = 0;
static int paramCursor = 0;
static uint16_t dirStack[16];
static int dirStackPos = 0;

static FILE* logFile = NULL;
static iconv_t iconvCtx = (iconv_t)-1;

static void logMsg(const char* format, ...) {
    if (!logFile) return;
    va_list args; va_start(args, format);
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    fprintf(logFile, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
    vfprintf(logFile, format, args); fflush(logFile);
    va_end(args);
}

static void cp1251_to_utf8(const char* src, int srcLen, char* dst, int dstSize) {
    if (!src || srcLen <= 0 || !dst || dstSize <= 0) { if (dst) dst[0] = '\0'; return; }
    if (iconvCtx == (iconv_t)-1) {
        int di = 0;
        for (int si = 0; si < srcLen && di+1 < dstSize; si++)
            dst[di++] = ((uint8_t)src[si] < 0x80) ? src[si] : '?';
        dst[di] = '\0'; return;
    }
    char* in = (char*)src; size_t il = srcLen; char* out = dst; size_t ol = dstSize - 1;
    iconv(iconvCtx, NULL, NULL, NULL, NULL); iconv(iconvCtx, &in, &il, &out, &ol);
    *out = '\0';
}

extern intptr_t* LC_LibInit(void);
extern int LC_HAL_InitSocketCAN(const char* ifname);
extern void LC_HAL_ProcessReceive(LC_NodeDescriptor_t* node);
extern void LC_Set_AddressCallback(void (*)(LC_NodeShortName_t, uint16_t, uint16_t));
extern LC_Return_t LC_HAL_Send(LC_HeaderPacked_t header, uint32_t* data, uint8_t length);

static int findNodeById(uint8_t id) { for (int i = 0; i < totalNodes; i++) if (nodeTable[i].NodeID == id) return i; return -1; }

static void sendRTR(uint8_t target, uint16_t msgId) {
    LC_HeaderPacked_t h; memset(&h, 0, sizeof(h));
    h.Source = MY_LEVCAN_ADDR; h.Target = target; h.MsgID = msgId; h.Request = 1;
    uint32_t d[2] = {0}; LC_HAL_Send(h, d, 0);
}

// ====== ОБРАБОТЧИКИ С ЛОГАМИ ======
static void onNodeName(LC_NodeDescriptor_t* node, LC_Header_t h, void* data, int32_t size) {
    logMsg(">>> onNodeName called: src=%d size=%d\n", h.Source, size);
    (void)node; if (size <= 0 || !data) return;
    int idx = findNodeById(h.Source);
    if (idx >= 0) { cp1251_to_utf8(data, size<47?size:47, nodeTable[idx].Name, 96); nodeTable[idx].confirmed = true; }
}

static void onDeviceName(LC_NodeDescriptor_t* node, LC_Header_t h, void* data, int32_t size) {
    logMsg(">>> onDeviceName called: src=%d size=%d\n", h.Source, size);
    (void)node; if (size <= 0 || !data) return;
    int idx = findNodeById(h.Source);
    if (idx >= 0 && !nodeTable[idx].confirmed) { cp1251_to_utf8(data, size<47?size:47, nodeTable[idx].Name, 96); nodeTable[idx].confirmed = true; }
}

static void onParamRequest(LC_NodeDescriptor_t* node, LC_Header_t h, void* data, int32_t size) {
    logMsg(">>> onParamRequest called: src=%d msgID=0x%03X size=%d\n", h.Source, h.MsgID, size);
    (void)node; (void)h; (void)data; (void)size;
}

static void onParamData(LC_NodeDescriptor_t* node, LC_Header_t h, void* data, int32_t size) {
    logMsg(">>> onParamData called: src=%d msgID=0x%03X size=%d data=", h.Source, h.MsgID, size);
    if (size > 0 && data) { for (int i = 0; i < size && i < 8; i++) logMsg("%02X ", ((uint8_t*)data)[i]); }
    logMsg("\n");
    (void)node;
}

static void onParamName(LC_NodeDescriptor_t* node, LC_Header_t h, void* data, int32_t size) {
    logMsg(">>> onParamName called: src=%d msgID=0x%03X size=%d str=", h.Source, h.MsgID, size);
    if (size > 0 && data) { char buf[96]; cp1251_to_utf8(data, size<95?size:95, buf, 96); logMsg("\"%s\"", buf); }
    logMsg("\n");
    (void)node;
}

static void onParamValue(LC_NodeDescriptor_t* node, LC_Header_t h, void* data, int32_t size) {
    logMsg(">>> onParamValue called: src=%d msgID=0x%03X size=%d\n", h.Source, h.MsgID, size);
    (void)node; (void)data; (void)size;
}

static void sysHandler(LC_NodeDescriptor_t* node, LC_Header_t h, void* data, int32_t size) {
    logMsg(">>> sysHandler called: src=%d msgID=0x%03X size=%d\n", h.Source, h.MsgID, size);
    (void)node; (void)h; (void)data; (void)size;
}

static LC_Object_t myObjects[24];

static void AddressCallback(LC_NodeShortName_t sn, uint16_t index, uint16_t state) {
    logMsg(">>> AddressCallback: node=%d state=%d type=%d\n", sn.NodeID, state, sn.DeviceType);
    (void)index;
    if (state == 3) { int i = findNodeById(sn.NodeID); if (i >= 0) nodeTable[i].active = false; return; }
    if (state == 0 || sn.NodeID < 64 || sn.NodeID > 125) return;
    if (sn.NodeID == MY_LEVCAN_ADDR) return;
    int i = findNodeById(sn.NodeID); if (i >= 0) { nodeTable[i].active = true; return; }
    if (totalNodes < MAX_NODES) {
        memset(&nodeTable[totalNodes], 0, sizeof(DiscoveredNode_t));
        nodeTable[totalNodes].NodeID = sn.NodeID;
        nodeTable[totalNodes].DeviceType = sn.DeviceType;
        nodeTable[totalNodes].ManufacturerCode = sn.ManufacturerCode;
        nodeTable[totalNodes].SerialNumber = sn.SerialNumber;
        nodeTable[totalNodes].active = true; totalNodes++;
        sendRTR(sn.NodeID, LC_SYS_NodeName);
    }
}

static void loadAllDirs(uint8_t target) {
    logMsg("=== loadAllDirs: node %d ===\n", target);
    static bool pcInit = false;
    if (!pcInit) { LCP_ParameterClientInit(myNode); pcInit = true; }
    memset(dirs, 0, sizeof(dirs)); dirCount = 0;
    LC_Return_t state = LC_Ok; uint16_t index = 0; int outofrange = 0;
    while (state != LC_Timeout && index < MAX_DIRS) {
        LCPC_Directory_t dirInfo; memset(&dirInfo, 0, sizeof(dirInfo));
        state = LCP_RequestDirectory(myNode, target, index, &dirInfo);
        logMsg("  RequestDirectory(%d) = %d\n", index, state);
        if (state == LC_Ok) {
            dirs[dirCount].index = index;
            if (dirInfo.Name && dirInfo.Name[0]) {
                cp1251_to_utf8(dirInfo.Name, strlen(dirInfo.Name), dirs[dirCount].name, 96);
            } else {
                snprintf(dirs[dirCount].name, 95, "Dir %d", index);
            }
            dirs[dirCount].entryCount = dirInfo.Size; dirCount++;
            LCP_CleanDirectory(&dirInfo);
        } else if (state == LC_OutOfRange) { outofrange++; if (outofrange > 1) state = LC_Timeout; LCP_CleanDirectory(&dirInfo); }
        else LCP_CleanDirectory(&dirInfo);
        index++;
    }
    logMsg("=== Found %d directories ===\n", dirCount);
}

static bool loadDirEntries(uint8_t target, int dirIdx) {
    if (dirIdx >= dirCount) return false;
    DirInfo_t* d = &dirs[dirIdx];
    logMsg("=== loadDirEntries: dir %d (%d entries) ===\n", d->index, d->entryCount);
    memset(d->entries, 0, sizeof(d->entries));
    for (uint16_t i = 0; i < d->entryCount && i < MAX_PARAMS; i++) {
        LCPC_Entry_t entry; memset(&entry, 0, sizeof(entry));
        LC_Return_t ret = LCP_RequestEntry(myNode, target, d->index, i, &entry);
        logMsg("  RequestEntry(%d, %d) = %d\n", d->index, i, ret);
        if (ret != LC_Ok) { LCP_CleanEntry(&entry); if (ret == LC_Timeout || ret == LC_OutOfRange) break; continue; }
        d->entries[i].type = entry.EntryType; d->entries[i].isFolder = (entry.EntryType == 0); d->entries[i].childDir = entry.DirectoryIndex;
        if (entry.Name && entry.Name[0]) cp1251_to_utf8(entry.Name, strlen(entry.Name), d->entries[i].name, 96);
        else snprintf(d->entries[i].name, 95, "[%d]", i);
        logMsg("    name=\"%s\" type=%d\n", d->entries[i].name, d->entries[i].type);
        if (!d->entries[i].isFolder && entry.EntryType != 1 && entry.VarSize > 0 && entry.Variable) {
            switch (entry.EntryType) {
                case 2: snprintf(d->entries[i].value,63,"%s",*(uint8_t*)entry.Variable?"ON":"OFF"); break;
                case 5: snprintf(d->entries[i].value,63,"%d",*(int32_t*)entry.Variable); break;
                case 6: snprintf(d->entries[i].value,63,"%u",*(uint32_t*)entry.Variable); break;
                case 12: snprintf(d->entries[i].value,63,"%d",*(int32_t*)entry.Variable); break;
            }
            d->entries[i].hasValue = true;
        }
        LCP_CleanEntry(&entry);
    }
    d->loaded = true;
    return true;
}

static void* networkManagerThread(void* arg) { LC_NodeDescriptor_t* n = arg; uint32_t t = 0; while (running) { LC_NetworkManager(n, t); t++; usleep(1000); } return NULL; }
static void* receiveManagerThread(void* arg) { while (running) { LC_ReceiveManager(arg); usleep(1000); } return NULL; }
static void* canReceiveThread(void* arg) { while (running) { LC_HAL_ProcessReceive(arg); usleep(1000); } return NULL; }

static void RenderUI(void) {
    erase(); int my, mx; getmaxyx(stdscr, my, mx);
    attron(A_REVERSE); mvhline(0, 0, ' ', mx); mvprintw(0, 1, "LEVCAN [%d]", MY_LEVCAN_ADDR); attroff(A_REVERSE);
    if (appState == S_DEVICES) {
        mvprintw(2, 1, "Devices:"); int row = 4, vi = 0;
        for (int i = 0; i < totalNodes && row < my - 2; i++) {
            if (!nodeTable[i].active && !nodeTable[i].confirmed) continue;
            bool sel = (vi == activeCursor);
            if (sel) attron(A_REVERSE);
            mvhline(row, 0, ' ', mx);
            mvprintw(row, 1, "%s %3d | %s", sel?">":" ", nodeTable[i].NodeID, nodeTable[i].Name);
            if (sel) attroff(A_REVERSE);
            row++; vi++;
        }
        if (activeCursor >= vi) activeCursor = vi > 0 ? vi - 1 : 0;
        mvprintw(my - 1, 1, "j/k:Nav ENTER:Params Q:Exit (%d)", vi);
    }
    else if (appState == S_LOADING) { mvprintw(my/2, (mx-20)/2, "Loading..."); }
    else if (appState == S_PARAMS) {
        int i = findNodeById(selectedNodeId);
        mvprintw(2, 1, "%s [%d]  %s", i>=0?nodeTable[i].Name:"?", selectedNodeId, dirs[currentDir].name);
        mvhline(3, 0, '-', mx);
        if (dirs[currentDir].entryCount == 0) { mvprintw(5, 3, "No entries"); }
        else {
            int row = 4;
            for (int j = 0; j < dirs[currentDir].entryCount && row < my - 2; j++) {
                bool sel = (j == paramCursor);
                if (sel) attron(A_REVERSE);
                mvhline(row, 0, ' ', mx);
                mvprintw(row, 1, "%s %s %s %s", sel?">":" ", dirs[currentDir].entries[j].isFolder?"[+]":" ",
                         dirs[currentDir].entries[j].name, dirs[currentDir].entries[j].value);
                if (sel) attroff(A_REVERSE);
                row++;
            }
        }
        mvprintw(my - 1, 1, "j/k:Nav ENTER:Open BS:Back ESC:Exit");
    }
    refresh();
}

static void sigHandler(int sig) { (void)sig; running = 0; }

int main(int argc, char* argv[]) {
    const char* iface = "can0";
    const char* logPath = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) { logPath = argv[++i]; }
        else if (argv[i][0] != '-') iface = argv[i];
    }
    if (logPath) { logFile = fopen(logPath, "a"); time_t t = time(NULL); fprintf(logFile, "\n=== %s", ctime(&t)); fflush(logFile); }
    iconvCtx = iconv_open("UTF-8", "CP1251"); if (iconvCtx == (iconv_t)-1) iconvCtx = iconv_open("UTF-8", "WINDOWS-1251");
    
    signal(SIGINT, sigHandler);
    if (LC_HAL_InitSocketCAN(iface) < 0) { fprintf(stderr, "CAN failed\n"); return 1; }

    myNode = (LC_NodeDescriptor_t*)LC_LibInit();
    if (!myNode) { fprintf(stderr, "LibInit failed\n"); return 1; }
    myNode->ShortName.NodeID = MY_LEVCAN_ADDR;
    myNode->ShortName.DeviceType = 100; myNode->ShortName.ManufacturerCode = 1; myNode->ShortName.SerialNumber = 1;
    myNode->DeviceName = "Scanner"; myNode->NodeName = "Scanner"; myNode->VendorName = "LEVCAN"; myNode->Serial[0] = 1;

    // Все обработчики с логами
    memset(myObjects, 0, sizeof(myObjects));
    for (int i = 0; i < 24; i++) {
        myObjects[i].MsgID = LC_SYS_NodeName + i;
        myObjects[i].Attributes.Function = 1; myObjects[i].Attributes.Writable = 1;
        myObjects[i].Size = (i <= 1) ? -64 : 8;
        if (i == 0)      myObjects[i].Address = onNodeName;
        else if (i == 1) myObjects[i].Address = onDeviceName;
        else if (i == 17) myObjects[i].Address = onParamRequest;
        else if (i == 18) myObjects[i].Address = onParamData;
        else if (i == 20) myObjects[i].Address = onParamName;
        else if (i == 22) myObjects[i].Address = onParamValue;
        else myObjects[i].Address = sysHandler;
    }
    myNode->Objects = myObjects; myNode->ObjectsSize = 24;
    LC_Set_AddressCallback(AddressCallback);
    if (LC_CreateNode(myNode) != LC_Ok) { fprintf(stderr, "CreateNode failed\n"); return 1; }

    pthread_t t1, t2, t3;
    pthread_create(&t1, NULL, networkManagerThread, myNode);
    pthread_create(&t2, NULL, receiveManagerThread, myNode);
    pthread_create(&t3, NULL, canReceiveThread, myNode);
    pthread_detach(t1); pthread_detach(t2); pthread_detach(t3);

    setlocale(LC_ALL, "");
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); nodelay(stdscr, TRUE); curs_set(0);
    
    while (running) {
        usleep(50000); RenderUI();
        int ch = getch(); if (ch == ERR) continue;
        if (appState == S_DEVICES) {
            switch (ch) {
                case KEY_UP: case 'k': if (activeCursor > 0) activeCursor--; break;
                case KEY_DOWN: case 'j': activeCursor++; break;
                case '\n': case KEY_ENTER: {
                    int vi = 0;
                    for (int i = 0; i < totalNodes; i++)
                        if (nodeTable[i].active || nodeTable[i].confirmed)
                            { if (vi == activeCursor) { selectedNodeId = nodeTable[i].NodeID; appState = S_LOADING; RenderUI(); loadAllDirs(selectedNodeId); if (dirCount > 0) loadDirEntries(selectedNodeId, 0); currentDir = 0; dirStackPos = 0; paramCursor = 0; appState = S_PARAMS; break; } vi++; }
                    break;
                }
                case 'q': running = 0; break;
            }
        }
        else if (appState == S_PARAMS) {
            switch (ch) {
                case 27: appState = S_DEVICES; break;
                case 'q': running = 0; break;
                case KEY_UP: case 'k': if (paramCursor > 0) paramCursor--; break;
                case KEY_DOWN: case 'j': if (paramCursor < dirs[currentDir].entryCount - 1) paramCursor++; break;
                case '\n': case KEY_ENTER:
                    if (paramCursor < dirs[currentDir].entryCount && dirs[currentDir].entries[paramCursor].isFolder) {
                        int foundIdx = -1;
                        for (int i = 0; i < dirCount; i++) if (dirs[i].index == dirs[currentDir].entries[paramCursor].childDir) { foundIdx = i; break; }
                        if (foundIdx >= 0) { dirStack[dirStackPos++] = currentDir; currentDir = foundIdx; paramCursor = 0; if (!dirs[currentDir].loaded) { appState = S_LOADING; RenderUI(); loadDirEntries(selectedNodeId, currentDir); appState = S_PARAMS; } }
                    }
                    break;
                case KEY_BACKSPACE: case 127:
                    if (dirStackPos > 0) { currentDir = dirStack[--dirStackPos]; paramCursor = 0; } else appState = S_DEVICES;
                    break;
            }
        }
    }
    endwin();
    if (iconvCtx != (iconv_t)-1) iconv_close(iconvCtx);
    if (logFile) fclose(logFile);
    return 0;
}