# cdiNet serial echo test build — Docker toolchain (Microware Ultra C via wine + vcdmastr via DOSBox)
# Parametric on $(CDISDK); defaults to mount point /opt/cdi-sdk inside the container.

CDISDK ?= /opt/cdi-sdk
OS9C   := $(CDISDK)/DOS/BIN
OS68K  := $(CDISDK)/OS9/68000
OS9CDI := $(CDISDK)/OS9/CDI
XCLIB  := $(OS68K)/LIB
CDEF   := $(OS9CDI)/DEFS
CLIB   := $(OS9CDI)/LIB
MASTER := $(CDISDK)/TOOLS/Master

NAME   := serecho
MODNAME := cdi_serecho
OUTPUT := out
BUILD  := build
DISK   := disk
SRC    := src

WINE     ?= wine
# xcc.exe and friends are symlinked into cwd by the container entrypoint,
# so invoke them via ./ — cwd search is required because xcc looks for
# cpfe/ccom next to itself (i.e. in its own cwd), not on PATH.
XCC      := $(WINE) ./xcc.exe
L68      := $(WINE) ./l68.exe
FIXMOD   := $(WINE) ./fixmod.exe
CRT_LINK := /usr/local/bin/crt_link.sh

CCFLAGS := -eas=$(OUTPUT) -tp=68K,sc -v=$(CDEF) -bc -r

OBJS := $(OUTPUT)/main.r

LDPARAM := -a -n=$(MODNAME) -o=$(BUILD)/$(NAME) \
           $(CLIB)/cstart.r $(OBJS) \
           -l=$(CLIB)/cdi.l -l=$(CLIB)/cdisys.l \
           -l=$(CLIB)/clib.l -l=$(CLIB)/cio.l \
           -l=$(CLIB)/math.l -l=$(CLIB)/sys.l \
           -l=$(CLIB)/usr.l -l=$(XCLIB)/os_csl.l

.PHONY: all link cd clean dirs sdk-links
all: cd

dirs:
	@mkdir -p $(OUTPUT) $(BUILD) $(DISK)

# xcc/l68/fixmod resolve sub-phases from cwd — symlink them next to the source tree.
sdk-links:
	@for f in $(OS9C)/*.exe $(OS9C)/*.dll $(OS9C)/*.tdb; do \
		[ -e "$$f" ] && ln -sf "$$f" "./$$(basename $$f)"; \
	done; true

$(OUTPUT)/%.r: $(SRC)/%.c | dirs sdk-links
	$(XCC) $(CCFLAGS) -O=2 $<

link: $(OBJS) | dirs
	$(CRT_LINK) $(LDPARAM)
	$(L68) -z=link.txt
	$(FIXMOD) -uo=0.0 -ue=1 -up=777 $(BUILD)/$(NAME)

cd: link
	SDL_VIDEODRIVER=dummy timeout 60 dosbox -exit \
	  -c "mount d $(MASTER)" \
	  -c "mount e $(CURDIR)" \
	  -c "e:" \
	  -c "path %PATH%;d:\\" \
	  -c "vcdmastr.exe build.cd > master.log" \
	  -c "exit" >/dev/null 2>&1
	@echo "=== master.log ==="
	@cat master.log
	@# Reverted: subheader fn/ch patch removed. Kernel scans modules only on
	@# yellow (fn=1) sectors; forcing fn=0 hides the module from the scan.
	@dd if=$(DISK)/SERECHO.CDI of=$(DISK)/SERECHO.BIN bs=2352 skip=150 status=none
	@echo "stripped SERECHO.BIN: $$(stat -c%s $(DISK)/SERECHO.BIN) bytes"
	@printf 'FILE "SERECHO.BIN" BINARY\n  TRACK 01 CDI/2352\n    INDEX 01 00:00:00\n' > $(DISK)/SERECHO.CUE
	@ls -la $(DISK)/SERECHO.*

clean:
	rm -rf $(OUTPUT) link.txt master.log
	rm -f $(BUILD)/$(NAME) $(DISK)/SERECHO.*
	find . -maxdepth 1 -type l -name '*.exe' -delete
	find . -maxdepth 1 -type l -name '*.dll' -delete
	find . -maxdepth 1 -type l -name '*.tdb' -delete
