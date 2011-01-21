all: gromit

proptest: proptest.c
	gcc -o proptest proptest.c `gtk-config --libs --cflags`

propertywatch: propertywatch.c
	gcc -o propertywatch propertywatch.c `gtk-config --libs --cflags`

gromit: gromit.c Makefile
	gcc -DG_DISABLE_DEPRECATED -DGDK_PIXBUF_DISABLE_DEPRECATED -DGDK_DISABLE_DEPRECATED -DPANGO_DISABLE_DEPRECATED -DGDK_MULTIHEAD_SAFE -DGTK_MULTIHEAD_SAFE -o gromit gromit.c -Wall `pkg-config --libs --cflags gtk+-2.0 x11`
