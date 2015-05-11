CC=gcc
LD=gcc

CFLAGS=-Wall -Wno-format -g -I/opt/vc/include/IL -I/opt/vc/include -I/opt/vc/include/interface/vcos/pthreads -I/opt/vc/include/interface/vmcs_host/linux -DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -D_LINUX -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -U_FORTIFY_SOURCE -DHAVE_LIBOPENMAX=2 -DOMX -DOMX_SKIP64BIT -ftree-vectorize -pipe -DUSE_EXTERNAL_OMX -DHAVE_LIBBCM_HOST -DUSE_EXTERNAL_LIBBCM_HOST -DUSE_VCHIQ_ARM -L/usr/local/lib -I/usr/local/include -O3
LDFLAGS=-Xlinker -R/opt/vc/lib -L/opt/vc/lib/ -Xlinker -L/usr/local/lib -Xlinker -R/usr/local/lib # -Xlinker --verbose
LIBS=-lavformat -lavcodec -lavutil -lopenmaxil -lbcm_host -lvcos -lpthread -lpng -lm -lx264
OFILES=omxmotion.o motion.o

.PHONY: all clean install dist

all: omxmotion

.c.o:
	$(CC) $(CFLAGS) -c $<

omxmotion: $(OFILES)
	$(CC) $(LDFLAGS) $(LIBS) -o omxmotion $(OFILES)

plotraw: plotraw.o
	$(CC) $(LDFLAGS) $(LIBS) -o plotraw plotraw.o

clean:
	rm -f *.o omxmotion
	rm -rf dist

# rm -f vo/*; valgrind --leak-check=full --undef-value-errors=no  ./omxmotion -b 16 -m heatmap.png -d vo -t 1 -o 100

dist: clean
	mkdir dist
	cp omxmotion.c Makefile dist
	FILE=omxmotion-`date +%Y%m%dT%H%M%S`.tar.bz2 && tar cvf - --exclude='.*.sw[ponml]' dist | bzip2 > $$FILE && echo && echo $$FILE
