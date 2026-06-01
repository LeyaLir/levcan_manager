/*******************************************************************************
 * LEVCAN: Light Electric Vehicle CAN protocol [LC]
 * Copyright (C) 2020 Vasiliy Sukhoparov
 ******************************************************************************/

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "levcan_filedef.h"

// Функции пользователя для критических секций
extern void lc_enable_irq(void);
extern void lc_disable_irq(void);

// Упаковка памяти и платформозависимые типы
#define LEVCAN_PACKED __attribute__((packed))
#define LEVCAN_MIN_BYTE_SIZE 1
#define LC_EXPORT __attribute__((visibility("default")))
#define CALLBACK

// Вывод отладочных сообщений
#define LEVCAN_TRACE
#define trace_printf(format, ...) print_log("LEVCAN:" format, ##__VA_ARGS__)
extern int print_log(const char* format, ...);

// Конфигурация файловых операций
#define LEVCAN_FILECLIENT
#define LEVCAN_FILESERVER
#define LEVCAN_BUFFER_FILEPRINTF
#define LEVCAN_FILE_TIMEOUT 500

// Конфигурация сервера параметров
#define LEVCAN_PARAMETERS_SERVER
#define LEVCAN_USE_FLOAT
#define LEVCAN_USE_INT64
#define LEVCAN_USE_DOUBLE
#define LEVCAN_PARAM_QUEUE_SIZE 5

// Настройки узлов и очередей
#define LEVCAN_MAX_OWN_NODES 1
#define LEVCAN_PARAMETERS_CLIENT
#define LEVCAN_MAX_TABLE_NODES 120
#define LEVCAN_RX_SIZE 100
#define LEVCAN_NO_TX_QUEUE

// Размер данных для объектов и ввода-вывода
#define LEVCAN_OBJECT_DATASIZE 64
#define LEVCAN_FILE_DATASIZE 512

// Динамическое управление памятью POSIX
//#define LEVCAN_MEM_STATIC
#ifndef LEVCAN_MEM_STATIC
#define lcmalloc(bytes) malloc(bytes)
#define lcfree free 
#define lcdelay(ms) usleep((ms) * 1000)

// Использование очередей RTOS / оберток
#define LEVCAN_USE_RTOS_QUEUE
#ifdef LEVCAN_USE_RTOS_QUEUE

#define S1(x) #x
#define S2(x) S1(x)
#define LOCATION __FILE__ " : " S2(__LINE__)

// Функции очередей
#define LC_QueueCreate(length, itemSize) wrapper_QueueCreate(length, itemSize)
#define LC_QueueDelete(queue) wrapper_QueueDelete(queue)
#define LC_QueueReset(queue) linux_QueueReset(queue) // Добавьте эту строку
#define LC_QueueSendToBack(queue, buffer, ttwait) wrapper_QueueSendToBack(queue, buffer, ttwait)
#define LC_QueueSendToBackISR(queue, item, yieldNeeded) LC_QueueSendToBack(queue, item, 0)
#define LC_QueueReceive(queue, buffer, ttwait) wrapper_QueueReceive(queue, buffer, ttwait)

#define LC_SemaphoreCreate xSemaphoreCreateBinary
#define LC_SemaphoreDelete(sem) vSemaphoreDelete(sem)
#define LC_SemaphoreGive(sem) xSemaphoreGive(sem)
#define LC_SemaphoreGiveISR(sem, yieldNeeded) xSemaphoreGiveFromISR(sem, yieldNeeded)
#define LC_SemaphoreTake(sem, ttwait) xSemaphoreTake(sem, ttwait)

#define LC_RTOSYieldISR(yield) 
#define YieldNeeded_t uint32_t

// Прототипы функций очередей
typedef intptr_t*(CALLBACK* qCreate)(uint32_t length, uint32_t itemSize);
typedef void(CALLBACK* qDelete)(void* queue);
typedef int32_t(CALLBACK* qReceive)(void* queue, void* buffer, int32_t ttwait);
typedef int32_t(CALLBACK* qSendBack)(void* queue, void* buffer, int32_t ttwait);

extern qCreate wrapper_QueueCreate;
extern qDelete wrapper_QueueDelete;
extern qReceive wrapper_QueueReceive;
extern qSendBack wrapper_QueueSendToBack;

extern void linux_QueueReset(void* queue);

// Прототипы файловых операций
typedef LC_FileResult_t(CALLBACK* fOpen)(void** fileObject, char* name, LC_FileAccess_t mode);
typedef uint32_t(CALLBACK* fTell)(void *fileObject);
typedef LC_FileResult_t(CALLBACK* fSeek)(void* fileObject, uint32_t pointer);
typedef LC_FileResult_t(CALLBACK* fRead)(void* fileObject, char* buffer, uint32_t bytesToRead, uint32_t* bytesReaded);
typedef LC_FileResult_t(CALLBACK* fWrite)(void* fileObject, char* buffer, uint32_t bytesToWrite, uint32_t* bytesWritten);
typedef LC_FileResult_t(CALLBACK* fClose)(void* fileObject);
typedef uint32_t(CALLBACK* fSize)(void* fileObject);
typedef LC_FileResult_t(CALLBACK* fTruncate)(void* fileObject);
typedef void(CALLBACK* fOnReceive)();

extern fOpen lcfopen;
extern fTell lcftell;
extern fSeek lcflseek;
extern fRead lcfread;
extern fWrite lcfwrite;
extern fClose lcfclose;
extern fTruncate lcftruncate;
extern fSize lcfsize;
extern fOnReceive LC_FileServerOnReceive;

#endif // LEVCAN_USE_RTOS_QUEUE
#endif // LEVCAN_MEM_STATIC
