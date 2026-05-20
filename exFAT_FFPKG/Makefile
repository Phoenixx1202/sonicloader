PS5_HOST         ?= ps5
PS5_PORT         ?= 9021
PS5_PAYLOAD_SDK  ?= /home/phoenix/ps5-payload-sdk

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF    := exFAT_FFPKG.elf

# Adicionamos -I (Include) para os headers e -L (Library) para os binários compilados
# O restante do Makefile continua igual...

CFLAGS := -Wall -Wno-error -g \
          -I"$(PS5_PAYLOAD_SDK)/etaHEN/Source Code/include" \
          -L"$(PS5_PAYLOAD_SDK)/etaHEN/Source Code/lib" \
          -lcurl -lwolfssl -lz -lzstd -lpsl -lkernel_sys

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)

# Envia o ELF diretamente para o elfldr do PS5 via rede
send: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

# Copia o ELF compilado para os StreamingAssets do Unity
install: $(ELF)
	cp $(ELF) ../Duskaryon_V3/Assets/StreamingAssets/$(ELF)
	@echo "ELF copiado para StreamingAssets."

.PHONY: all clean send install
