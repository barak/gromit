all: gromit

CPPFLAGS += -DG_DISABLE_DEPRECATED
CPPFLAGS += -DGDK_PIXBUF_DISABLE_DEPRECATED
# CPPFLAGS += -DGDK_DISABLE_DEPRECATED
CPPFLAGS += -DPANGO_DISABLE_DEPRECATED
CPPFLAGS += -DGDK_MULTIHEAD_SAFE -DGTK_MULTIHEAD_SAFE

CPPFLAGS += $(shell pkg-config --cflags-only-I gtk+-2.0 x11)

CFLAGS += -Wall -Wno-pointer-sign
CFLAGS += -O2
CFLAGS += -g

CFLAGS += $(shell pkg-config --cflags-only-other gtk+-2.0 x11)

LOADLIBES += $(shell pkg-config --libs gtk+-2.0 x11)
LOADLIBES += -lm
