# Host unit tests for the pure-C parts of the firmware (no ESP-IDF needed).
#   make -C firmware/tests/host        build and run
SRC_DIR := ../../main
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I$(SRC_DIR)
LIBS    := -lm
CORE    := $(SRC_DIR)/ot_frame.c $(SRC_DIR)/plant_sim.c $(SRC_DIR)/ot_sim.c $(SRC_DIR)/otb_core.c $(SRC_DIR)/cfgtable.c $(SRC_DIR)/control.c $(SRC_DIR)/schedule.c $(SRC_DIR)/datalog.c $(SRC_DIR)/alerts.c $(SRC_DIR)/service.c $(SRC_DIR)/cfgtext.c $(SRC_DIR)/sensor_filter.c $(SRC_DIR)/histring.c $(SRC_DIR)/textparse.c
TESTS   := test_ot test_control test_sched test_datalog test_service test_cfgtext test_sensor test_manchester test_histring test_textparse test_cfgblob test_awaykeys

all: run

build/%: %.c $(CORE) $(wildcard $(SRC_DIR)/*.h) test.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< $(CORE) $(LIBS)

run: $(addprefix build/,$(TESTS))
	@for t in $^; do echo "== $$t"; ./$$t || exit 1; done

clean:
	rm -rf build

.PHONY: all run clean
