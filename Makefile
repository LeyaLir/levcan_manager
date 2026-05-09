# Makefile для can-browser — LEVCAN обозреватель для Linux
# Использует ncurses и socketCAN

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS = -lncursesw
# Для отладки: раскомментируйте следующую строку
# CFLAGS += -DDEBUG -O0 -fsanitize=address

TARGET  = can-browser
SRC     = main.c
OBJ     = $(SRC:.c=.o)

# CAN интерфейс по умолчанию (можно переопределить: make run CAN_IF=vcan0)
CAN_IF ?= can0

.PHONY: all clean run debug gdb valgrind setup-vcan help

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Сборка завершена: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Очистка
clean:
	rm -f $(TARGET) $(OBJ)
	@echo "Очищено."

# Запуск (требует прав root для CAN)
run: $(TARGET)
	@echo "Запуск на интерфейсе $(CAN_IF)..."
	sudo ./$(TARGET) $(CAN_IF)

# Отладка с GDB
gdb: $(TARGET)
	@echo "Запуск GDB для $(TARGET) на $(CAN_IF)..."
	@echo "Команды GDB: run $(CAN_IF), bt, info locals, etc."
	sudo gdb --args ./$(TARGET) $(CAN_IF)

# Valgrind для проверки утечек памяти
valgrind: $(TARGET)
	@echo "Проверка Valgrind..."
	sudo valgrind --leak-check=full --show-leak-kinds=all \
		--track-origins=yes --log-file=valgrind.log \
		./$(TARGET) $(CAN_IF) &
	@echo "Запущен в фоне. Дождитесь выхода и проверьте valgrind.log"
	@echo "Для остановки: sudo pkill -f './$(TARGET)'"

# Создание виртуального CAN для тестирования
setup-vcan:
	@echo "Настройка виртуального CAN vcan0..."
	sudo modprobe vcan
	sudo ip link add dev vcan0 type vcan
	sudo ip link set up vcan0
	@echo "vcan0 создан и поднят. Для проверки:"
	@echo "  candump vcan0          # слушать трафик"
	@echo "  cansend vcan0 123#DEAD  # отправить кадр"
	@echo "  make run CAN_IF=vcan0   # запустить браузер"

# Удаление виртуального CAN
teardown-vcan:
	@echo "Удаление vcan0..."
	sudo ip link delete vcan0

# Анализ кода
cppcheck:
	cppcheck --enable=all --inconclusive --std=c11 $(SRC)

# Форматирование кода (требуется clang-format)
format:
	clang-format -i $(SRC)

# Просмотр CAN трафика
candump:
	candump $(CAN_IF)

# Информация о сборке
info:
	@echo "Target:       $(TARGET)"
	@echo "Source:       $(SRC)"
	@echo "CAN IF:       $(CAN_IF)"
	@echo "Compiler:     $(CC)"
	@echo "CFLAGS:       $(CFLAGS)"
	@echo "LDFLAGS:      $(LDFLAGS)"

# Справка
help:
	@echo "Доступные цели:"
	@echo "  make             — сборка"
	@echo "  make run         — запуск (CAN_IF=can0 по умолчанию)"
	@echo "  make gdb         — отладка с GDB"
	@echo "  make valgrind    — проверка утечек памяти"
	@echo "  make setup-vcan  — создать vcan0 для тестирования"
	@echo "  make teardown-vcan — удалить vcan0"
	@echo "  make clean       — удалить собранные файлы"
	@echo "  make cppcheck    — статический анализ"
	@echo "  make format      — форматировать код (clang-format)"
	@echo "  make candump     — просмотр трафика CAN"
	@echo "  make info        — информация о сборке"
	@echo ""
	@echo "Примеры:"
	@echo "  make run CAN_IF=vcan0    — запуск на виртуальном CAN"
	@echo "  make setup-vcan && make run CAN_IF=vcan0"
	@echo "                            — быстрый тестовый запуск"