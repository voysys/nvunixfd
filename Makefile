###############################################################################
#
# SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA Corporation and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA Corporation is strictly prohibited.
#
###############################################################################

SO_NAME := libgstnvunixfd.so

CC := gcc

GST_INSTALL_DIR?=/usr/lib/aarch64-linux-gnu/gstreamer-1.0/
LIB_INSTALL_DIR?=/usr/lib/aarch64-linux-gnu/tegra/
CFLAGS:=
LIBS:= -lnvbufsurface -lnvbufsurftransform -lpthread

SRCS := $(wildcard *.c)

INCLUDES += -I./ -I../

PKGS := gstreamer-1.0 \
	gstreamer-base-1.0 \
	gstreamer-video-1.0 \
	gstreamer-allocators-1.0 \
	glib-2.0 \
	gio-2.0 \
	gio-unix-2.0

OBJS := $(SRCS:.c=.o)

CFLAGS += -fPIC \
	-DEXPLICITLY_ADDED=1 \
	-DGETTEXT_PACKAGE=1 \
	-DHAVE_IPC_TARGET_NV=1 \

CFLAGS += `pkg-config --cflags $(PKGS)`

LDFLAGS = -Wl,--no-undefined -L$(LIB_INSTALL_DIR) -Wl,-rpath,$(LIB_INSTALL_DIR)

LIBS += `pkg-config --libs $(PKGS)`

all: $(SO_NAME)

%.o: %.c
	@echo "Compiling: $(CC) $(CFLAGS) $<"
	$(CC) -c $< $(CFLAGS) $(INCLUDES) -o $@

$(SO_NAME): $(OBJS)
	$(CC) -shared -o $(SO_NAME) $(OBJS) $(LIBS) $(LDFLAGS)

.PHONY: install
install: $(SO_NAME)
	cp -vp $(SO_NAME) $(GST_INSTALL_DIR)

.PHONY: clean
clean:
	rm -rf $(OBJS) $(SO_NAME)
