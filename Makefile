CC=gcc

# Reviewed warnings for gcc 6.2.1
CFLAGS = \
	-std=c11 -g -ggdb -pedantic -pedantic-errors \
	-Werror -Wall -Wextra \
	-Wformat=2 \
	-Wformat-signedness \
	-Wnull-dereference \
	-Winit-self \
	-Wmissing-include-dirs \
	-Wshift-overflow=2 \
	-Wswitch-default \
	-Wswitch-enum \
	-Wunused-const-variable=2 \
	-Wuninitialized \
	-Wunknown-pragmas \
	-Wstrict-overflow=5 \
	-Wsuggest-attribute=pure \
	-Wsuggest-attribute=const \
	-Wsuggest-attribute=noreturn \
	-Wsuggest-attribute=format \
	-Warray-bounds=2 \
	-Wduplicated-cond \
	-Wfloat-equal \
	-Wundef \
	-Wshadow \
	-Wbad-function-cast \
	-Wcast-qual \
	-Wcast-align \
	-Wwrite-strings \
	-Wconversion \
	-Wjump-misses-init \
	-Wlogical-op \
	-Waggregate-return \
	-Wcast-align \
	-Wstrict-prototypes \
	-Wold-style-definition \
	-Wmissing-prototypes \
	-Wmissing-declarations \
	-Wpacked \
	-Wredundant-decls \
	-Wnested-externs \
	-Winline \
	-Winvalid-pch \
	-Wstack-protector \
	-Wno-incompatible-pointer-types

# XXX: I'm using -Wno-incompatible-pointer types because of the
#   const uint8_t * * on swr_convert()'s input parameter. Probably there's a
#   way to resolve that.

TARGETS=audiostreamer

all: $(TARGETS)

audiostreamer: audiostreamer.c
	@# -lavutil for av_frame_free
	$(CC) $(CFLAGS) -o $@ $< -lavformat -lavdevice -lavcodec -lavutil -lswresample

clean:
	rm -f $(TARGETS)
