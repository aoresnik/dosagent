# DOSAGENT build.
#
#   make host    Linux/POSIX test build -> build/host/agent
#   make dos     DJGPP cross build      -> build/dos/DOSAGENT.EXE
#   make dist    release archive        -> build/dist/DAGNTxyq.ZIP
#                and release notes      -> build/dist/RELNOTES.md
#
# For the DOS build, set DJGPP_PREFIX to your cross-compiler prefix. Watt-32
# is compiled from its sources here, without its configur.sh; the errno files
# its build normally generates (by running a DOS program) are in dos/watt32/.

DJGPP_PREFIX ?= i586-pc-msdosdjgpp-
WATT_ROOT    ?= third_party/watt32
BEARSSL      := third_party/bearssl

SRCS := src/main.c src/api.c src/http.c src/net.c src/tools.c \
        src/json.c src/buf.c src/charset.c src/platform.c
BEARSSL_SRCS := $(shell find $(BEARSSL)/src -name '*.c')

WARN := -Wall -Wextra -Wno-unused-parameter

# Version and archive name from the git tag (see tools/version.sh).
# make dist TAG=v0.1alpha builds as if HEAD had that tag.
VERSION_INFO := $(shell sh tools/version.sh $(TAG))
ifeq ($(VERSION_INFO),)
$(error cannot determine the version)
endif
VERSION  := $(word 1,$(VERSION_INFO))
DIST_ZIP := $(word 2,$(VERSION_INFO))
# build/version.h is rewritten only when the version changes.
$(shell mkdir -p build && \
        echo '#define VERSION "$(VERSION)"' > build/version.h.tmp && \
        { cmp -s build/version.h.tmp build/version.h || \
          mv build/version.h.tmp build/version.h; } ; rm -f build/version.h.tmp)

# ---------------------------------------------------------------- host
HOST_CC     ?= gcc
HOST_CFLAGS := -std=gnu99 -O2 -g $(WARN) -Ibuild -I$(BEARSSL)/inc
HOST_OBJ    := build/host
HOST_APP_OBJS := $(SRCS:src/%.c=$(HOST_OBJ)/%.o)
HOST_BR_OBJS  := $(BEARSSL_SRCS:$(BEARSSL)/src/%.c=$(HOST_OBJ)/bearssl/%.o)

host: $(HOST_OBJ)/agent

$(HOST_OBJ)/agent: $(HOST_APP_OBJS) $(HOST_OBJ)/libbearssl.a
	$(HOST_CC) -o $@ $(HOST_APP_OBJS) $(HOST_OBJ)/libbearssl.a

$(HOST_OBJ)/libbearssl.a: $(HOST_BR_OBJS)
	rm -f $@
	ar rcs $@ $^

$(HOST_OBJ)/%.o: src/%.c src/*.h build/version.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

$(HOST_OBJ)/bearssl/%.o: $(BEARSSL)/src/%.c
	@mkdir -p $(dir $@)
	@$(HOST_CC) -std=gnu99 -O2 -I$(BEARSSL)/inc -I$(BEARSSL)/src -c -o $@ $<

# ---------------------------------------------------------------- DOS
DOS_CC     := $(DJGPP_PREFIX)gcc
DOS_AR     := $(DJGPP_PREFIX)ar
# -march=i386 keeps the binary runnable on a 386; tune for 486-class caches.
DOS_ARCH   := -march=i386 -mtune=i486
DOS_CFLAGS := -std=gnu99 -O2 $(DOS_ARCH) $(WARN) -Ibuild -I$(BEARSSL)/inc \
              -I$(WATT_ROOT)/inc -Idos/watt32
# BearSSL can't find an OS RNG or clock on DOS; we seed it and set the time.
# The x86 extensions (AES-NI, SSE2, RDRAND) are disabled so no code path
# depends on CPUID, which a 386 does not have.
DOS_BR_DEFS := -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 \
               -DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 \
               -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_RDRAND=0
DOS_OBJ    := build/dos
DOS_APP_OBJS := $(SRCS:src/%.c=$(DOS_OBJ)/%.o)
DOS_BR_OBJS  := $(BEARSSL_SRCS:$(BEARSSL)/src/%.c=$(DOS_OBJ)/bearssl/%.o)

# -lemu links in the 387 emulator, so a 386 or 486SX without an FPU does
# not need EMU387.DXE (without either, the program dies with SIGNOFP).
dos: $(DOS_OBJ)/DOSAGENT.EXE $(DOS_OBJ)/CWSDPMI.EXE

$(DOS_OBJ)/DOSAGENT.EXE: $(DOS_APP_OBJS) $(DOS_OBJ)/libbearssl.a \
                      $(DOS_OBJ)/libwatt.a
	$(DOS_CC) -o $@ $(DOS_APP_OBJS) $(DOS_OBJ)/libbearssl.a \
	    $(DOS_OBJ)/libwatt.a -lemu
	$(DJGPP_PREFIX)strip $@

# CWSDPMI is not in git: the binary distribution is downloaded once and
# checked against a known hash.
CWSDPMI_ZIP    := third_party/cwsdpmi/csdpmi7b.zip
CWSDPMI_URL    := https://www.delorie.com/pub/djgpp/current/v2misc/csdpmi7b.zip
CWSDPMI_SHA256 := deacda0488e1cdd7c4a9f32fab45662b34c0ed6b2d7d4d13bc07041b62004a8c

$(CWSDPMI_ZIP):
	@mkdir -p $(dir $@)
	curl -fsSL -o $@.tmp $(CWSDPMI_URL)
	echo "$(CWSDPMI_SHA256)  $@.tmp" | sha256sum -c --quiet
	mv $@.tmp $@

$(DOS_OBJ)/CWSDPMI.EXE: $(CWSDPMI_ZIP)
	@mkdir -p $(dir $@)
	unzip -p $< bin/CWSDPMI.EXE > $@

$(DOS_OBJ)/CWSDPMI.DOC: $(CWSDPMI_ZIP)
	@mkdir -p $(dir $@)
	unzip -p $< bin/cwsdpmi.doc > $@

# Watt-32: the DOS objects of src/makefile.all (CORE, BSD, BIND, ZLIB and the
# DOS-only files), minus the Windows-only ones.
WATT_SRC  := $(WATT_ROOT)/src
WATT_C    := accept bind bsddbug bsdname btree chksum close connect crc dynip \
             echo fcntl get_ai get_ip get_ni get_xbyr geteth gethost gethost6 \
             getname getnet getopt getprot getput getserv gettod idna ioctl \
             ip4_frag ip4_in ip4_out ip6_in ip6_out language linkaddr listen \
             lookup loopback misc netaddr netback neterr nettime nsapaddr \
             oldstuff packet32 pc_cbrk pcarp pcbootp pcbuf pcconfig pcdbug \
             pcdhcp pcdns pcicmp pcicmp6 pcigmp pcping pcqueue pcrarp pcrecv \
             pcsed pcstat pctcp ports ppp pppoe presaddr printk profile \
             punycode receive res_comp res_data res_debu res_init res_loc \
             res_mkqu res_quer res_send run select settod shutdown signal \
             sock_dbu sock_in sock_ini sock_io sock_prn sock_scn sock_sel \
             socket sockopt split stream misc_str swsvpkt syslog syslog2 \
             tcp_fsm tcp_md5 tftp timer transmit udp_rev version zadler32 \
             zcompres zcrc32 zdeflate zgzio zinfback zinffast zinflate \
             zinftree ztrees zuncompr zutil \
             country fsext pcpkt32 pcpkt pcintr powerpak qmsg wdpmi x32vm
WATT_OBJS := $(WATT_C:%=$(DOS_OBJ)/watt32/%.o) $(DOS_OBJ)/watt32/cpumodel.o
# -fgnu89-inline: Watt-32 relies on GNU extern-inline semantics.
WATT_CFLAGS := -O2 $(DOS_ARCH) -fgnu89-inline -w -DWATT32_BUILD \
               -I$(WATT_SRC) -I$(WATT_ROOT)/inc -Idos/watt32 -I$(DOS_OBJ)/watt32

$(DOS_OBJ)/libwatt.a: $(WATT_OBJS)
	rm -f $@
	$(DOS_AR) rcs $@ $^

$(DOS_OBJ)/watt32/%.o: $(WATT_SRC)/%.c $(DOS_OBJ)/watt32/build/djgpp/cflags.h
	@$(DOS_CC) $(WATT_CFLAGS) -c -o $@ $<

# The real-mode packet receiver (USE_FAST_PKT), assembled and turned into a
# C byte array for pcpkt2.c (what Watt-32's bin2c does).
$(DOS_OBJ)/watt32/pcpkt.o: $(DOS_OBJ)/watt32/pkt_stub.h

$(DOS_OBJ)/watt32/pkt_stub.h: $(WATT_SRC)/asmpkt.nas
	@mkdir -p $(dir $@)
	nasm -f bin -o $(DOS_OBJ)/watt32/asmpkt.bin $<
	od -An -v -tu1 $(DOS_OBJ)/watt32/asmpkt.bin | \
	    sed -e 's/^ *//' -e 's/  */,/g' -e 's/$$/,/' > $@

$(DOS_OBJ)/watt32/%.o: $(WATT_SRC)/%.S
	@mkdir -p $(dir $@)
	$(DOS_CC) $(WATT_CFLAGS) -c -o $@ $<

# Included by version.c for wattcpBuildCflags().
$(DOS_OBJ)/watt32/build/djgpp/cflags.h:
	@mkdir -p $(dir $@)
	echo 'const char *w32_cflags = "$(WATT_CFLAGS)";' > $@
	echo 'const char *w32_cc     = "$(DOS_CC)";' >> $@

$(DOS_OBJ)/libbearssl.a: $(DOS_BR_OBJS)
	rm -f $@
	$(DOS_AR) rcs $@ $^

$(DOS_OBJ)/%.o: src/%.c src/*.h build/version.h
	@mkdir -p $(dir $@)
	$(DOS_CC) $(DOS_CFLAGS) -c -o $@ $<

$(DOS_OBJ)/bearssl/%.o: $(BEARSSL)/src/%.c
	@mkdir -p $(dir $@)
	@$(DOS_CC) -std=gnu99 -O2 $(DOS_ARCH) $(DOS_BR_DEFS) -I$(BEARSSL)/inc \
	    -I$(BEARSSL)/src -c -o $@ $<

# ---------------------------------------------------------------- release
# Text files get CRLF line endings for DOS.
DIST_DIR   := build/dist
DIST_STAGE := $(DIST_DIR)/stage
CRLF       := sed 's/\r*$$/\r/'
CACERT     ?= /etc/ssl/certs/ca-certificates.crt

dist: $(DOS_OBJ)/DOSAGENT.EXE $(DOS_OBJ)/CWSDPMI.EXE $(DOS_OBJ)/CWSDPMI.DOC
	rm -rf $(DIST_STAGE) $(DIST_DIR)/DAGNT*.ZIP $(DIST_DIR)/RELNOTES.md
	mkdir -p $(DIST_STAGE)
	sh tools/relnotes.sh $(VERSION) > $(DIST_DIR)/RELNOTES.tmp
	mv $(DIST_DIR)/RELNOTES.tmp $(DIST_DIR)/RELNOTES.md
	cp $(DOS_OBJ)/DOSAGENT.EXE $(DOS_OBJ)/CWSDPMI.EXE $(DIST_STAGE)/
	$(CRLF) $(DOS_OBJ)/CWSDPMI.DOC > $(DIST_STAGE)/CWSDPMI.DOC
	$(CRLF) README.md > $(DIST_STAGE)/README.MD
	$(CRLF) LICENSE > $(DIST_STAGE)/LICENSE.TXT
	$(CRLF) DOSAGENT.CFG.example > $(DIST_STAGE)/DOSAGENT.CFG
	$(CRLF) dist/WATTCP.CFG > $(DIST_STAGE)/WATTCP.CFG
	$(CRLF) $(CACERT) > $(DIST_STAGE)/CACERT.PEM
	{ cat dist/NOTICES.TXT; \
	  sed -n -f dist/notice.sed $(WATT_SRC)/resolver.h; \
	  printf '\n\nBearSSL\n-------\n\n'; \
	  cat $(BEARSSL)/LICENSE.txt; } | $(CRLF) > $(DIST_STAGE)/NOTICES.TXT
	cd $(DIST_STAGE) && zip -q -X -9 ../$(DIST_ZIP) *
	@echo; unzip -l $(DIST_DIR)/$(DIST_ZIP)

clean:
	rm -rf build

.PHONY: host dos dist clean
