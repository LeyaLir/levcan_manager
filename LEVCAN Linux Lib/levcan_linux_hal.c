#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "levcan.h"
#include "mq.h"

static int sktCAN = -1;
static pthread_mutex_t ghMutex = PTHREAD_MUTEX_INITIALIZER;

// Контекст для хранения POSIX очереди
typedef struct {
    mqd_t mqdes;
    char name[32];
    size_t item_size;
} LinuxQueue_t;

static int queue_counter = 0;

// === РЕАЛИЗАЦИЯ ФУНКЦИЙ ОЧЕРЕДЕЙ POSIX ===

intptr_t* linux_QueueCreate(uint32_t length, uint32_t itemSize) {
    LinuxQueue_t* q = (LinuxQueue_t*)malloc(sizeof(LinuxQueue_t));
    if (!q) return NULL;

    q->item_size = itemSize;
    snprintf(q->name, sizeof(q->name), "/lc_mq_%d_%d", getpid(), queue_counter++);

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = (length > 10) ? 10 : length;
    attr.mq_msgsize = itemSize;
    attr.mq_curmsgs = 0;

    q->mqdes = mq_open(q->name, O_RDWR | O_CREAT, 0666, &attr);
    if (q->mqdes == (mqd_t)-1) {
        free(q);
        return NULL;
    }
    return (intptr_t*)q;
}

void linux_QueueDelete(void* queue) {
    if (!queue) return;
    LinuxQueue_t* q = (LinuxQueue_t*)queue;
    mq_close(q->mqdes);
    mq_unlink(q->name);
    free(q);
}

int32_t linux_QueueSendToBack(void* queue, void* buffer, int32_t ttwait) {
    if (!queue || !buffer) return 0;
    LinuxQueue_t* q = (LinuxQueue_t*)queue;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (ttwait > 0) {
        ts.tv_sec += ttwait / 1000;
        ts.tv_nsec += (ttwait % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    }

    if (mq_timedsend(q->mqdes, (const char*)buffer, q->item_size, 0, &ts) == 0) {
        return 1;
    }
    return 0;
}

int32_t linux_QueueReceive(void* queue, void* buffer, int32_t ttwait) {
    if (!queue || !buffer) return 0;
    LinuxQueue_t* q = (LinuxQueue_t*)queue;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (ttwait > 0) {
        ts.tv_sec += ttwait / 1000;
        ts.tv_nsec += (ttwait % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    }

    ssize_t bytes_read = mq_timedreceive(q->mqdes, (char*)buffer, q->item_size, NULL, &ts);
    if (bytes_read >= 0) return 1;
    return 0;
}

void linux_QueueReset(void* queue) {
    if (!queue) return;
    LinuxQueue_t* q = (LinuxQueue_t*)queue;
    struct mq_attr attr;
    if (mq_getattr(q->mqdes, &attr) == 0) {
        char* buf = malloc(q->item_size);
        if (!buf) return;
        while (attr.mq_curmsgs > 0) {
            struct timespec ts = {0, 0};
            if (mq_timedreceive(q->mqdes, buf, q->item_size, NULL, &ts) < 0) break;
            if (mq_getattr(q->mqdes, &attr) != 0) break;
        }
        free(buf);
    }
}

qCreate wrapper_QueueCreate = linux_QueueCreate;
qDelete wrapper_QueueDelete = linux_QueueDelete;
qReceive wrapper_QueueReceive = linux_QueueReceive;
qSendBack wrapper_QueueSendToBack = linux_QueueSendToBack;

// === ОСТАЛЬНОЙ НАШ HAL КОД SOCKETCAN ===

typedef LC_Return_t(*extSndCallback)(uint32_t header, uint32_t* data, uint8_t length);
typedef LC_Return_t(*extFltrCallback)(uint32_t* reg, uint32_t* mask, uint8_t count);
static extSndCallback external_send = NULL;
static extFltrCallback external_filter = NULL;

void LC_HAL_SetCallbacks(void* snd, void* fltr) {
    if (snd) external_send = (extSndCallback)snd;
    if (fltr) external_filter = (extFltrCallback)fltr;
}

int LC_HAL_InitSocketCAN(const char* ifname) {
    struct sockaddr_can addr;
    struct ifreq ifr;
    sktCAN = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sktCAN < 0) return -1;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sktCAN, SIOCGIFINDEX, &ifr) < 0) { close(sktCAN); return -1; }
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(sktCAN, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sktCAN); return -1; }
    
    int recv_own_msgs = 0;
    setsockopt(sktCAN, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &recv_own_msgs, sizeof(recv_own_msgs));
    
    // printf("[HAL] SocketCAN initialized on %s (fd=%d)\n", ifname, sktCAN);
    return sktCAN;
}

LC_Return_t LC_HAL_Send(LC_HeaderPacked_t header, uint32_t* data, uint8_t length) {
    if (external_send != NULL) return (LC_Return_t)external_send(header.ToUint32, data, length);
    if (sktCAN < 0) return LC_ObjectError;
    
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    
    uint32_t can_id = header.ToUint32 & 0x1FFFFFFF;
    frame.can_id = can_id | CAN_EFF_FLAG;
    
    if (header.ToUint32 & (1u << 29)) {
        frame.can_id |= CAN_RTR_FLAG;
        frame.can_dlc = length;
    } else {
        frame.can_dlc = length > 8 ? 8 : length;
        if (data && length > 0) memcpy(frame.data, data, frame.can_dlc);
    }
    
    if (write(sktCAN, &frame, sizeof(struct can_frame)) == sizeof(struct can_frame)) 
        return LC_Ok;
    return LC_BufferFull;
}

void LC_HAL_ProcessReceive(LC_NodeDescriptor_t* node) {
    if (!node || sktCAN < 0) return;
    
    struct can_frame frame;
    ssize_t n = read(sktCAN, &frame, sizeof(struct can_frame));
    
    if (n == sizeof(struct can_frame)) {
        if (frame.can_id & CAN_EFF_FLAG) {
            LC_HeaderPacked_t header;
            header.ToUint32 = frame.can_id & CAN_EFF_MASK;
            if (frame.can_id & CAN_RTR_FLAG) {
                header.ToUint32 |= (1u << 29);
            }
            LC_ReceiveHandler(node, header, (uint32_t*)frame.data, frame.can_dlc);
        }
    }
}

LC_Return_t LC_HAL_CreateFilterMasks(LC_HeaderPacked_t* reg, LC_HeaderPacked_t* mask, uint16_t count) {
    if (external_filter != NULL) return (LC_Return_t)external_filter((uint32_t*)reg, (uint32_t*)mask, count);
    return LC_Ok;
}

LC_Return_t LC_HAL_HalfFull(void) { return LC_BufferEmpty; }
void lc_enable_irq(void) { pthread_mutex_unlock(&ghMutex); }
void lc_disable_irq(void) { pthread_mutex_lock(&ghMutex); }
