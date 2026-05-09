# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -I.
LDFLAGS := -lpthread
TARGET  := main
BUILD   := build
SRCS    := main.c workqueue.c
OBJS    := $(SRCS:%.c=$(BUILD)/%.o)

.PHONY: all run clean dirs

all: dirs $(BUILD)/$(TARGET)

dirs:
	@mkdir -p $(BUILD)

$(BUILD)/$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: %.c list.h workqueue.h | dirs
	$(CC) $(CFLAGS) -c -o $@ $<

run: all
	$(BUILD)/$(TARGET)

clean:
	rm -rf $(BUILD)
