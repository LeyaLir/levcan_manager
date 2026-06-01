# Настройки компилятора и флагов
CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -pthread
LDFLAGS = -shared -pthread -lrt

# Определение архитектуры (по умолчанию текущая хост-система)
ARCH ?= $(shell uname -m)
ifeq ($(ARCH),x86_64)
    TARGET_DIR = bin/release/runtimes/linux-x64/native
else ifeq ($(ARCH),aarch64)
    TARGET_DIR = bin/release/runtimes/linux-arm64/native
else
    TARGET_DIR = bin/release/runtimes/linux-$(ARCH)/native
endif

# Название выходного файла библиотеки
TARGET = $(TARGET_DIR)/liblevcan.so
OBJ_DIR = build/$(ARCH)

# Пути к исходникам и заголовочным файлам
SRC_DIRS = . ../LEVCAN/source
INC_DIRS = . ../LEVCAN/source

# Поиск всех файлов .c в указанных директориях
SRCS = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
# Преобразование путей .c файлов в пути .o в директории сборки
OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(SRCS)))

# Добавление путей для поиска заголовочных файлов
CFLAGS += $(foreach dir,$(INC_DIRS),-I$(dir))

# Настройка путей поиска файлов (VPATH) для make
vpath %.c $(SRC_DIRS)

.PHONY: all clean

# Основная цель сборки
all: $(TARGET)

# Сборка динамической библиотеки
$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Успешно собрано: $@"

# Компиляция объектных файлов
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Очистка результатов сборки
clean:
	rm -rf build bin
	@echo "Проект очищен"
