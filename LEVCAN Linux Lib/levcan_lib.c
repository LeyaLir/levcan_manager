#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include "levcan.h"
#include "levcan_address.h"
#include "levcan_filedef.h"
#include "mq.h"

#define LC_EXPORT __attribute__((visibility("default")))

// Внешние объявления функций HAL, которые теперь лежат в levcan_hal_linux.c
extern LC_Return_t LC_HAL_Send(LC_HeaderPacked_t header, uint32_t* data, uint8_t length);
extern LC_Return_t LC_HAL_CreateFilterMasks(LC_HeaderPacked_t* reg, LC_HeaderPacked_t* mask, uint16_t count);
extern LC_Return_t LC_HAL_HalfFull(void);

// Оставляем только функции регистрации коллбеков, если они вызываются извне библиотеки
extern void LC_HAL_SetCallbacks(void* snd, void* fltr);

LC_EXPORT void LC_Set_SendCallback(void* callback) {
    LC_HAL_SetCallbacks(callback, NULL);
}

LC_EXPORT void LC_Set_FilterCallback(void* callback) {
    LC_HAL_SetCallbacks(NULL, callback);
}

LC_EXPORT void LC_Set_AddressCallback(LC_RemoteNodeCallback_t address) { 
    lc_addressCallback = address; 
}

#ifdef LEVCAN_USE_RTOS_QUEUE
// Экспортируем функцию для внешних приложений, чтобы они могли подменить очереди
LC_EXPORT void LC_Set_QueueCallbacks(qCreate create, qDelete delete, qReceive receive, qSendBack toback) {
    wrapper_QueueCreate = create;
    wrapper_QueueDelete = delete;
    wrapper_QueueReceive = receive;
    wrapper_QueueSendToBack = toback;
}
#else
LC_EXPORT void LC_Set_QueueCallbacks(void) {
    // Пустышка для конфигурации без очередей
}
#endif

// Обертки для файловых операций (остаются без изменений)
fOpen lcfopen; fTell lcftell; fSeek lcflseek; fRead lcfread; fWrite lcfwrite; fClose lcfclose; fTruncate lcftruncate; fSize lcfsize; fOnReceive LC_FileServerOnReceive;

LC_EXPORT void LC_Set_FileCallbacks(fOpen fopen, fTell ftell, fSeek flseek, fRead fread, fWrite fwrite, fClose fclose, fTruncate ftruncate, fSize fsize, fOnReceive onrec) {
    lcfopen = fopen; lcftell = ftell; lcflseek = flseek; lcfread = fread; lcfwrite = fwrite; lcfclose = fclose; lcftruncate = ftruncate; lcfsize = fsize; LC_FileServerOnReceive = onrec;
}

int print_log(const char* format, ...) {
    va_list args; va_start(args, format);
    vfprintf(stdout, format, args); va_end(args); fflush(stdout);
    return 0;
}

// Инициализация дескриптора драйвера LEVCAN связывается с функциями из HAL слоя
LC_EXPORT intptr_t* LC_LibInit() {
    LC_NodeDescriptor_t* desc = lcmalloc(sizeof(LC_NodeDescriptor_t));
    LC_InitNodeDescriptor(desc);
    
    LC_DriverCalls_t* drv = lcmalloc(sizeof(LC_DriverCalls_t));
    drv->Filter = LC_HAL_CreateFilterMasks; 
    drv->Send = LC_HAL_Send; 
    drv->TxHalfFull = LC_HAL_HalfFull;
    desc->Driver = drv;
    
    LC_Set_QueueCallbacks(wrapper_QueueCreate, wrapper_QueueDelete, wrapper_QueueReceive, wrapper_QueueSendToBack);
    
    return (intptr_t*)desc;
}
