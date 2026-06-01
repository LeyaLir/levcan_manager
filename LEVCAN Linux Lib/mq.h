#pragma once

#include <stdint.h>
#include "levcan_filedef.h"

#ifndef CALLBACK
#define CALLBACK
#endif

// Определение типов указателей на функции очередей, которые требует LEVCAN
typedef intptr_t*(CALLBACK* qCreate)(uint32_t length, uint32_t itemSize);
typedef void(CALLBACK* qDelete)(void* queue);
typedef int32_t(CALLBACK* qReceive)(void* queue, void* buffer, int32_t ttwait);
typedef int32_t(CALLBACK* qSendBack)(void* queue, void* buffer, int32_t ttwait);

// Переменные указателей, используемые макросами LEVCAN
extern qCreate wrapper_QueueCreate;
extern qDelete wrapper_QueueDelete;
extern qReceive wrapper_QueueReceive;
extern qSendBack wrapper_QueueSendToBack;

// Функция для внутренней инициализации указателей (вызывается при старте)
void mq_init_wrappers(void);
