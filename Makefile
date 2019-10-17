CC=gcc
CFLAGS=-O2 -Wall

SRCC=jcp.c jcp_handler.c
SRCH=dumpver.h flash_cof.h flashstub.h 
SRCH+=romdump.h turbow.h univbin.h upgrade101.h winusb.h
SRCH+=jcp_handler.h 
OBJS=$(SRCC:.c=.o) 

PROJECT=rmvjcp
PROJECT_NUMBER=1.0.0

TARFILE=$(PROJECT)-$(PROJECT_NUMBER).tar


DISTFILES=Makefile ReadMe.txt libusb.lib
DISTFILES+=$(SRCC) $(SRCH)

all: .depend $(PROJECT) 

$(PROJECT): $(OBJS) $(SRCH)
	gcc -o $(PROJECT) $(OBJS) -lusb

%.o: %.c
	$(CC) $(CFLAGS) -c $<

dist:
	tar cfv $(TARFILE) $(DISTFILES)
	gzip	$(TARFILE)

clean:
	rm -f *~ $(OBJS) $(PROJECT)

.depend: $(SRCC)
	$(CC) -MM $(SRCC) > .depend

-include .depend
