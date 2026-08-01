LIBNAME=tftpd.a
LIBDIRNAME=tftpd

EXTRA_INCLUDE=-I$(SWITCH_BASE)/apps/$(SWITCH_NAME) -I$(SWITCH_BASE)/include 

SUBDIRS	=
OBJS    = tftpd.o tftpd_task.o tftpd_cmd.o

include $(SWITCH_BASE)/configs/rules.library

ifeq ($(findstring $(SWITCH_NAME),2026 2224), $(SWITCH_NAME))
CFLAGS += -funaligned-pointers 
endif

CC_OPTIM=	-O0 -g 
