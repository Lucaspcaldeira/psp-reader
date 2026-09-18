# ---------------------------------------------------------------------------
# PSP Reader - Makefile PSPSDK
# Rodar SEMPRE dentro do container pspdev (use ./build.ps1), nunca no Windows.
# ---------------------------------------------------------------------------

TARGET = ereader

# src/pdf e src/read sao PORTAVEIS (C99 puro, sem PSPSDK) e tambem compilam no
# host via test.ps1. src/psp e o que toca o hardware.
OBJS = src/main.o \
       src/read/utf8.o \
       src/read/textlines.o \
       src/read/reflow.o \
       src/read/layout.o \
       src/read/txt.o \
       src/read/doc.o \
       src/read/zip.o \
       src/read/epub.o \
       src/read/progress.o \
       src/pdf/pdf_arena.o \
       src/pdf/pdf_io.o \
       src/pdf/pdf_obj.o \
       src/pdf/pdf_lex.o \
       src/pdf/pdf_filt.o \
       src/pdf/pdf_doc.o \
       src/pdf/pdf_font.o \
       src/pdf/pdf_text.o \
       src/psp/fs.o \
       src/psp/input.o \
       src/psp/gfx.o \
       src/psp/texture.o \
       src/psp/font.o \
       src/psp/library.o \
       src/psp/theme.o \
       src/psp/pdf_io_psp.o

PSPPREFIX = $(shell psp-config --psp-prefix)

INCDIR   = include $(PSPPREFIX)/include/freetype2
CFLAGS   = -O2 -G0 -Wall -Wextra -std=gnu99
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBDIR  =
LDFLAGS =

# Ordem importa em link estatico: freetype depende de png/z/bz2, que precisam
# vir DEPOIS dele. O pkg-config da imagem nao serve aqui porque o zlib do
# pspdev foi instalado sem zlib.pc, e o freetype2.pc o exige - a consulta falha
# mesmo com as bibliotecas todas presentes.
#
# NAO ha -lintrafont aqui, de proposito: intraFont e CC BY-SA 3.0 (share-alike)
# e este projeto e MIT. Ver PLAN.md secao 2.
LIBS = -lpspgu -lpsprtc -lfreetype -lpng16 -lz -lbz2 -lm

# --- Metadados do EBOOT (o que aparece no XMB) -----------------------------
EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Reader
# Descomente quando os assets do XMB existirem:
# PSP_EBOOT_ICON = assets/eboot/ICON0.PNG
# PSP_EBOOT_PIC1 = assets/eboot/PIC1.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# Empacota o que vai para ms0:/PSP/GAME/EREADER/
.PHONY: dist
dist: EBOOT.PBP
	mkdir -p dist/EREADER
	cp EBOOT.PBP dist/EREADER/
	if [ -d data ]; then cp -r data dist/EREADER/; fi
	@echo "==> dist/EREADER pronto para copiar ao PSP"
