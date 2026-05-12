# Sonic Loader — all-in-one PS5 payload.
#
# Bundles ftpsrv + ShadowMountPlus + a websrv-based web UI that
# can launch any title found in /system_data/priv/mms/app.db.
#
# kstuff is no longer baked in; the user installs it via Settings →
# "Install kstuff-lite + ShadowMountPlus" combo on first boot.
#
# Two build variants:
#   make             - sonic-loader.elf            (etaHEN daemon bundled, opt-in toggle)
#   make no-etahen   - sonic-loader-no-etahen.elf  (etaHEN payload omitted, ~36% smaller)
#   make both        - both ELFs in one go

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

VERSION_TAG := sonic-loader-1.0

# Stamp the build with the current git tag (e.g. "1.0.50") or, if
# we're not on a tag, the closest tag + short hash. Falls back to
# "dev" when there's no git tree (tarball builds). The result is
# baked into the ELF as SONIC_VERSION and surfaced via /version
# JSON, which the bottom-right corner badge in the web UI displays.
SONIC_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

PYTHON ?= python3

BIN          := sonic-loader.elf
BIN_NO_ETA   := sonic-loader-no-etahen.elf

SRCS := src/main.c src/websrv.c src/asset.c src/fs.c src/mime.c
SRCS += src/mdns.c src/smb.c src/appdb.c src/kmonitor.c src/cheats.c
SRCS += src/homebrew.c src/fan.c src/config.c src/avatar.c
SRCS += src/kstuff_updater.c
SRCS += src/smp_updater.c
SRCS += src/smp_meta.c
SRCS += src/y2jb_updater.c
SRCS += src/releases.c
SRCS += src/np.c
SRCS += src/garlic.c
SRCS += src/offact.c
SRCS += src/transfer.c
SRCS += src/jb.c
SRCS += src/dumper.c
SRCS += src/activity.c
SRCS += src/ps5/sys.c src/ps5/pt.c src/ps5/elfldr.c src/ps5/hbldr.c
SRCS += src/ps5/notify.c src/ps5/http.c
SRCS += src/third_party/stb_impl.c src/third_party/cJSON.c
SRCS += src/third_party/mc4/aes.c src/third_party/mc4/base64.c
SRCS += src/third_party/mc4/mc4decrypter.c

# SONIC_AUTOLAUNCH_HBL: on first boot the loader spawns FAKE00000 (the
# Homebrew Loader) automatically; the websrv 302-redirects the first
# GET / to /launcher.html#kstuff-update-card so the user lands directly
# on the kstuff + SMP install combo card. Subsequent boots are silent.
CFLAGS := -Os -Wall -Werror -Isrc
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -flto
CFLAGS += -DVERSION_TAG=\"$(VERSION_TAG)\"
CFLAGS += -DSONIC_VERSION=\"$(SONIC_VERSION)\"
CFLAGS += -DSONIC_AUTOLAUNCH_HBL

LDFLAGS := -Wl,--gc-sections -flto

LDADD  := -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil
LDADD  += -lSceSsl -lSceHttp -lsqlite3 -lSceRegMgr
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libmicrohttpd --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config microdns --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libsmb2 --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libarchive --libs`

ASSETS   := $(wildcard assets/*)
GEN_SRCS := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

# Sub-payloads bundled into every variant. kstuff.elf is intentionally
# absent — installed at runtime via Settings.
EMBEDDED_COMMON := payloads/ftpsrv.elf
EMBEDDED_COMMON += payloads/klogsrv.elf payloads/backpork.elf
EMBEDDED_COMMON += payloads/np-restore-account.elf
# np-fake-signin.elf is NOT embedded — it lives at
# /data/sonic-loader/np-fake-signin.elf and is lazy-downloaded from
# git.earthonion.com on first /api/np/fake-signin click. Keeps the
# loader ~120 KB smaller and lets us refresh the payload without a
# loader rebuild.
EMBEDDED_COMMON += payloads/garlic-worker.elf payloads/garlic-savemgr.elf
EMBEDDED_COMMON += payloads/nanodns.elf
EMBEDDED_COMMON += payloads/ps5-app-dumper.elf
EMBEDDED_COMMON += payloads/dpi.elf
EMBEDDED_COMMON += payloads/smp_icon.png

EMBEDDED          := $(EMBEDDED_COMMON) payloads/etahen.elf
EMBEDDED_NO_ETA   := $(EMBEDDED_COMMON)

all: $(BIN)

both: $(BIN) $(BIN_NO_ETA)

gen:
	mkdir -p gen

clean:
	rm -rf $(BIN) $(BIN_NO_ETA) gen

gen/%.c: assets/% | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(BIN): $(SRCS) $(GEN_SRCS) $(EMBEDDED)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(GEN_SRCS) $(LDADD)
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

# No-etaHEN variant: same source set, build with -DSONIC_NO_ETAHEN so
# the etahen INCASSET / spawn helpers compile out, and EMBEDDED_NO_ETA
# omits payloads/etahen.elf from the deps so the build doesn't try to
# .incbin a missing file.
$(BIN_NO_ETA): $(SRCS) $(GEN_SRCS) $(EMBEDDED_NO_ETA)
	$(CC) $(CFLAGS) -DSONIC_NO_ETAHEN $(LDFLAGS) -o $@ $(SRCS) $(GEN_SRCS) $(LDADD)
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

no-etahen: $(BIN_NO_ETA)

deploy: $(BIN)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

.PHONY: all both no-etahen deploy clean
