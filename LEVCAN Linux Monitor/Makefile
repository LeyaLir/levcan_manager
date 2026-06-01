# Настройки компилятора и флагов
CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
# -lncurses для GUI, -lrt для POSIX очередей, передаем путь к .so
LDFLAGS = -pthread -lrt -lncurses -L$(LIB_DIR) -Wl,-rpath,$(LIB_DIR)

# Автоопределение архитектуры хоста для линковки .so
ARCH ?= $(shell uname -m)
ifeq ($(ARCH),x86_64)
    LIB_DIR_NAME = linux-x64
else ifeq ($(ARCH),aarch64)
    LIB_DIR_NAME = linux-arm64
else
    LIB_DIR_NAME = linux-$(ARCH)
endif

# Относительный путь к каталогу с собранной библиотекой lblevcan.so
LIB_DIR = "../LEVCAN Linux Lib/bin/release/runtimes/$(LIB_DIR_NAME)/native"
LIBS = -llevcan

# Название исполняемого файла монитора и папка сборки
TARGET = bin/levcan-monitor
OBJ_DIR = build

# ЖЕСТКОЕ ОПРЕДЕЛЕНИЕ ПУТЕЙ (В обход foreach для защиты от разрыва строк пробелами)
CFLAGS += -I.
CFLAGS += -I"../LEVCAN/source"
CFLAGS += -I"../LEVCAN Linux Lib"

# Исходный файл
SRCS = main.c
OBJS = $(OBJ_DIR)/main.o

.PHONY: all clean check_lib

# Главная цель сборки
all: check_lib $(TARGET)

# Проверка, что общая библиотека .so уже была собрана в соседней папке
check_lib:
	@if [ ! -f $(LIB_DIR)/liblevcan.so ]; then \
		echo "Ошибка: Сначала соберите библиотеку lblevcan.so в папке 'LEVCAN Linux Lib'!"; \
		exit 1; \
	fi

# Сборка исполняемого файла монитора
$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)
	@echo "Успешно собран монитор: $@"

# Компиляция main.c с защищенными кавычками путями
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Очистка результатов сборки
clean:
	rm -rf $(OBJ_DIR) bin
	@echo "Монитор очищен"
