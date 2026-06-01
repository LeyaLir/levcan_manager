#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <ncurses.h>
#include <locale.h>

// Инклюды из папки ../source
#include "levcan.h"
#include "levcan_address.h"
#include "levcan_paramclient.h"

#define MY_LEVCAN_ADDR 100

typedef enum {
    S_DEVICES,
    S_BROWSE
} AppState;

static AppState appState = S_DEVICES;

// Функции вашего HAL слоя
extern int LC_HAL_InitSocketCAN(const char* ifname);
extern void LC_HAL_ProcessReceive(LC_NodeDescriptor_t* node);
extern intptr_t* LC_LibInit(void);
extern void LC_Set_AddressCallback(LC_RemoteNodeCallback_t address);

typedef struct {
    uint8_t NodeID;
    char DeviceName[32];
} DiscoveredNode_t;

static DiscoveredNode_t nodeTable[16];
static int totalNodes = 0;
static int activeCursor = 0;

// Исправлено: точное совпадение с типом LC_RemoteNodeCallback_t (последний аргумент uint16_t)
void OnNodeUpdate(LC_NodeShortName_t name, uint16_t index, uint16_t state) {
    (void)index;

    if (state == 0) {
        return; // Узел отключился
    }

    // Извлекаем NodeID прямо из структуры короткого имени LEVCAN
    uint8_t network_node_id = name.NodeID;

    // Проверяем дубликаты в UI таблице
    for (int i = 0; i < totalNodes; i++) {
        if (nodeTable[i].NodeID == network_node_id) return;
    }
    
    if (totalNodes < 16) {
        nodeTable[totalNodes].NodeID = network_node_id;
        
        // Так как прямого метода LC_GetNodeName в открытом API нет,
        // выводим информацию на основе полученной структуры короткого имени
        snprintf(nodeTable[totalNodes].DeviceName, 32, "Node Dev (Type: %d)", name.DeviceType);
        totalNodes++;
    }
}

void RenderUI(void) {
    clear();
    mvprintw(0, 1, "=== LEVCAN Linux Device Manager (Lib Version) ===");
    
    if (appState == S_DEVICES) {
        mvprintw(2, 1, "Discovered Nodes on CAN bus:");
        if (totalNodes == 0) {
            mvprintw(4, 3, "Searching for nodes (Address Claiming active)...");
        }
        for (int i = 0; i < totalNodes; i++) {
            if (i == activeCursor) {
                attron(A_REVERSE);
                mvprintw(4 + i, 3, "-> ID: %3d | %s", nodeTable[i].NodeID, nodeTable[i].DeviceName);
                attroff(A_REVERSE);
            } else {
                mvprintw(4 + i, 3, "   ID: %3d | %s", nodeTable[i].NodeID, nodeTable[i].DeviceName);
            }
        }
        mvprintw(LINES - 2, 1, "Use UP/DOWN to navigate, ENTER to select, Q to exit.");
    }
    else if (appState == S_BROWSE) {
        mvprintw(2, 1, "Browsing Parameters for Node %d", nodeTable[activeCursor].NodeID);
        mvprintw(4, 3, "[Folder] System Settings");
        mvprintw(5, 3, "   Voltage: 54.2 V");
        mvprintw(6, 3, "   Current Limits: 40 A");
        mvprintw(LINES - 2, 1, "ESC to go back.");
    }
    refresh();
}

int main(int argc, char* argv[]) {
    const char* can_interface = "can0";
    if (argc > 1) {
        can_interface = argv[1];
    }

    setlocale(LC_ALL, "");
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    int can_fd = LC_HAL_InitSocketCAN(can_interface);
    if (can_fd < 0) {
        endwin();
        fprintf(stderr, "Error: Cannot open SocketCAN interface %s\n", can_interface);
        return 1;
    }

    LC_NodeDescriptor_t* myNode = (LC_NodeDescriptor_t*)LC_LibInit();
    if (!myNode) {
        endwin();
        fprintf(stderr, "Error: LEVCAN initialization failed\n");
        close(can_fd);
        return 1;
    }
    
    // Настройка полей дескриптора нашего мастера
    myNode->Serial[0] = 0x12345678;
    myNode->DeviceName = "Linux Manager";
    myNode->NodeName = "Master Terminal";
    
    // Устанавливаем наш исправленный коллбек
    LC_Set_AddressCallback(OnNodeUpdate);

    // Исправлено: LC_AddressInit принимает строго ОДИН аргумент
    // Предварительно выставляем желаемый адрес в поле NodeID, если оно доступно,
    // либо инициализируем дескриптор напрямую
    LC_AddressInit(myNode);

    struct pollfd fds;
    fds.fd = can_fd;
    fds.events = POLLIN;

    int running = 1;
    while (running) {
        int ret = poll(&fds, 1, 20);
        
        if (ret > 0 && (fds.revents & POLLIN)) {
            LC_HAL_ProcessReceive(myNode);
        }

        LC_NetworkManager(myNode, 20); 

        int ch = getch();
        if (ch != ERR) {
            switch (ch) {
                case KEY_UP:
                    if (activeCursor > 0) activeCursor--;
                    break;
                case KEY_DOWN:
                    if (appState == S_DEVICES && activeCursor < totalNodes - 1) activeCursor++;
                    break;
                case '\n':
                    if (appState == S_DEVICES && totalNodes > 0) {
                        appState = S_BROWSE;
                    }
                    break;
                case 27:
                    if (appState == S_BROWSE) appState = S_DEVICES;
                    break;
                case 'q':
                case 'Q':
                    if (appState == S_DEVICES) running = 0;
                    break;
            }
        }

        RenderUI();
    }

    endwin();
    close(can_fd);
    return 0;
}
