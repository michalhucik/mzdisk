# ===========================================================================
#  mzdisk build systém
#
#  make libs          -> build-libs/   (sdílené knihovny .a)
#  make cli           -> build-cli/    (CLI nástroje, závisí na libs)
#  make gui           -> build-gui/    (GUI aplikace, závisí na libs)
#  make test             -> build-tests/  (C unit testy + CLI shell testy)
#  make test-libs        -> jen C unit testy knihoven
#  make test-cli         -> jen integrační shell testy CLI nástrojů
#                           (syntetické fixture disky, nezávislé na refdata/)
#  make test-cli-refdata -> CLI testy, ale fixture disky brány z refdata/dsk/
#  make all           -> libs + cli
#  make portable      -> portable/ (GUI + CLI dohromady, bez HTML docs)
#  make portable-gui  -> portable/mzdisk-gui-X.Y.Z/  (samostatně spustitelné GUI vč. DLL)
#  make portable-cli  -> portable/mzdisk-cli-X.Y.Z/  (CLI binárky + docs .md)
#  make portable-full -> portable/ včetně HTML docs (vyžaduje python3 + markdown)
#  make release-gui   -> portable/mzdisk-gui-X.Y.Z.zip
#  make release-cli   -> portable/mzdisk-cli-X.Y.Z.zip
#  make release       -> oba release archivy
#  make docs-html     -> docs-html/  (HTML dokumentace z docs/*/*.md,
#                        vyžaduje python3 a Python modul 'markdown')
#  make clean         -> vyčistí vše (libs + cli + gui + testy + portable + docs-html)
# ===========================================================================

# Detekce platformy: MSYS2/MinGW vs Linux/POSIX
ifneq ($(MSYSTEM),)
    export TEMP := $(shell cygpath -w /tmp)
    export TMP  := $(TEMP)
    EXE_SUFFIX := .exe
    CMAKE_GENERATOR := MSYS Makefiles
else
    CMAKE_GENERATOR := Unix Makefiles
endif

CMAKE_CONFIGURE_LIBS  := cmake -S src/libs -B build-libs -G "$(CMAKE_GENERATOR)"
CMAKE_CONFIGURE_CLI   := cmake -S src/tools -B build-cli -G "$(CMAKE_GENERATOR)"
CMAKE_CONFIGURE_GUI   := cmake -S src/mzdisk -B build-gui -G "$(CMAKE_GENERATOR)"
CMAKE_CONFIGURE_TESTS := cmake -S tests -B build-tests -G "$(CMAKE_GENERATOR)"

BUILD_LIBS_DIR    := build-libs
BUILD_CLI_DIR     := build-cli
BUILD_GUI_DIR     := build-gui
BUILD_TESTS_DIR   := build-tests

# Release verze GUI a CLI (nezávislé release cykly). Verze se načítají
# přímo z hlavičkových souborů, aby byly zdrojem pravdy C makra (nedochází
# k divergenci mezi Makefile a kódem).
MZDISK_GUI_VERSION := $(shell awk '/define[[:space:]]+MZDISK_VERSION[[:space:]]+"/ { gsub(/"/,"",$$3); print $$3 }' src/mzdisk/app.h)
MZDISK_CLI_VERSION := $(shell awk '/define[[:space:]]+MZDISK_CLI_RELEASE_VERSION[[:space:]]+"/ { gsub(/"/,"",$$3); print $$3 }' src/tools/common/mzdisk_cli_version.h)

PORTABLE_GUI_NAME := mzdisk-gui-$(MZDISK_GUI_VERSION)
PORTABLE_CLI_NAME := mzdisk-cli-$(MZDISK_CLI_VERSION)
PORTABLE_GUI_DIR  := portable/$(PORTABLE_GUI_NAME)
PORTABLE_CLI_DIR  := portable/$(PORTABLE_CLI_NAME)
PORTABLE_GUI_ZIP  := portable/$(PORTABLE_GUI_NAME).zip
PORTABLE_CLI_ZIP  := portable/$(PORTABLE_CLI_NAME).zip

JOBS              := $(shell nproc)

.PHONY: all libs cli gui clean libs-clean cli-clean gui-clean \
        libs-rebuild cli-rebuild gui-rebuild rebuild \
        test test-libs test-cli test-cli-refdata tests-clean \
        portable portable-full portable-gui portable-gui-clean \
        portable-cli portable-cli-clean \
        release release-gui release-cli release-clean \
        docs-html docs-html-clean

# --- Implicitní target: knihovny + CLI ---

all: cli

# --- Knihovny (build-libs/) ---

libs: $(BUILD_LIBS_DIR)/Makefile
	cmake --build $(BUILD_LIBS_DIR) -j$(JOBS)

$(BUILD_LIBS_DIR)/Makefile:
	$(CMAKE_CONFIGURE_LIBS)

libs-clean:
	@if [ -d $(BUILD_LIBS_DIR) ]; then rm -rf $(BUILD_LIBS_DIR); fi

libs-rebuild: libs-clean libs

# --- CLI nástroje (build-cli/, závisí na libs) ---

cli: libs $(BUILD_CLI_DIR)/Makefile
	cmake --build $(BUILD_CLI_DIR) -j$(JOBS)

$(BUILD_CLI_DIR)/Makefile:
	$(CMAKE_CONFIGURE_CLI)

cli-clean:
	@if [ -d $(BUILD_CLI_DIR) ]; then rm -rf $(BUILD_CLI_DIR); fi

cli-rebuild: cli-clean cli

# --- GUI aplikace (build-gui/, závisí na libs) ---

gui: libs $(BUILD_GUI_DIR)/Makefile locale
	cmake --build $(BUILD_GUI_DIR) -j$(JOBS)

# --- Lokalizace (.po -> .mo, kopie do build-gui/) ---

LOCALE_SRC  := src/locale
LOCALE_DIST := locale
LOCALE_GUI  := $(BUILD_GUI_DIR)/locale
PO_FILES    := $(wildcard $(LOCALE_SRC)/*/LC_MESSAGES/mzdisk.po)
MO_DIST     := $(patsubst $(LOCALE_SRC)/%/LC_MESSAGES/mzdisk.po,$(LOCALE_DIST)/%/LC_MESSAGES/mzdisk.mo,$(PO_FILES))
MO_GUI      := $(patsubst $(LOCALE_SRC)/%/LC_MESSAGES/mzdisk.po,$(LOCALE_GUI)/%/LC_MESSAGES/mzdisk.mo,$(PO_FILES))

.PHONY: locale locale-clean

locale: $(MO_DIST) $(MO_GUI)

$(LOCALE_DIST)/%/LC_MESSAGES/mzdisk.mo: $(LOCALE_SRC)/%/LC_MESSAGES/mzdisk.po
	@mkdir -p $(dir $@)
	msgfmt -o $@ $<

$(LOCALE_GUI)/%/LC_MESSAGES/mzdisk.mo: $(LOCALE_DIST)/%/LC_MESSAGES/mzdisk.mo
	@mkdir -p $(dir $@)
	cp $< $@

locale-clean:
	@rm -rf $(LOCALE_DIST) $(LOCALE_GUI)

$(BUILD_GUI_DIR)/Makefile:
	$(CMAKE_CONFIGURE_GUI)

gui-clean:
	@if [ -d $(BUILD_GUI_DIR) ]; then rm -rf $(BUILD_GUI_DIR); fi

gui-rebuild: gui-clean gui

# --- Testy (build-tests/, závisí na libs) ---

test: test-libs test-cli

test-libs: libs $(BUILD_TESTS_DIR)/Makefile
	cmake --build $(BUILD_TESTS_DIR) -j$(JOBS)
	@echo ""
	@echo "=== Running C unit tests ==="
	@failed=0; \
	for t in $(BUILD_TESTS_DIR)/test_*$(EXE_SUFFIX); do \
		if [ -x "$$t" ]; then \
			echo ""; \
			$$t || failed=1; \
		fi; \
	done; \
	echo ""; \
	if [ $$failed -ne 0 ]; then \
		echo "*** SOME TESTS FAILED ***"; \
		exit 1; \
	else \
		echo "=== All tests passed ==="; \
	fi

test-cli: cli
	@echo ""
	@echo "=== Running CLI integration tests (synthetic fixtures) ==="
	@failed=0; \
	for t in tests/cli/test_*.sh; do \
		if [ -x "$$t" ] || [ -f "$$t" ]; then \
			echo ""; \
			bash "$$t" </dev/null || failed=1; \
		fi; \
	done; \
	echo ""; \
	if [ $$failed -ne 0 ]; then \
		echo "*** SOME CLI TESTS FAILED ***"; \
		exit 1; \
	else \
		echo "=== All CLI tests passed ==="; \
	fi

# CLI testy s referenčními disky z refdata/dsk/.
# Stejné testy jako `test-cli`, ale USE_REFDATA=1 přepne fixture funkce
# tak, aby místo syntetického disku vrátily cestu do refdata/dsk/.
# Předpokládá, že je adresář refdata/dsk/ dostupný (není součástí repa).
test-cli-refdata: cli
	@if [ ! -d refdata/dsk ]; then \
		echo "*** refdata/dsk/ not found - skipping refdata CLI tests ***"; \
		exit 1; \
	fi
	@echo ""
	@echo "=== Running CLI integration tests (refdata fixtures) ==="
	@failed=0; \
	for t in tests/cli/test_*.sh; do \
		if [ -x "$$t" ] || [ -f "$$t" ]; then \
			echo ""; \
			USE_REFDATA=1 bash "$$t" </dev/null || failed=1; \
		fi; \
	done; \
	echo ""; \
	if [ $$failed -ne 0 ]; then \
		echo "*** SOME CLI TESTS FAILED ***"; \
		exit 1; \
	else \
		echo "=== All CLI tests passed (refdata) ==="; \
	fi

$(BUILD_TESTS_DIR)/Makefile:
	$(CMAKE_CONFIGURE_TESTS)

tests-clean:
	@if [ -d $(BUILD_TESTS_DIR) ]; then rm -rf $(BUILD_TESTS_DIR); fi

# --- Portable distribuce (vše) ---
#
# `portable`      - GUI + CLI, HTML dokumentace se nevytváří (nezávisí na
#                   žádném Python modulu). Pokud ale adresář docs-html/
#                   existuje (např. po samostatném `make docs-html`),
#                   portable-cli ho přikopíruje do balíku.
# `portable-full` - navíc nejdřív vygeneruje HTML dokumentaci. Tento target
#                   vyžaduje python3 a Python modul `markdown`.

portable: portable-gui portable-cli

portable-full: docs-html portable

# --- Portable GUI bundle (portable/mzdisk/) ---
#
# Sestaví samostatně spustitelnou kopii GUI:
#   - mzdisk binárka
#   - ui_resources/ (fonty, ikony, vlajky)
#   - locale/ (přeložené .mo soubory)
#   - na MSYS2 navíc všechny DLL knihovny z MSYS2 prefixu (SDL3, libintl, ...)
#
# Výsledek je přenosný adresář, který lze spustit i na stroji bez MSYS2.

portable-gui: gui
	@echo ""
	@echo "=== Building portable GUI in $(PORTABLE_GUI_DIR)/ ==="
	@rm -rf $(PORTABLE_GUI_DIR)
	@mkdir -p $(PORTABLE_GUI_DIR)
	cp $(BUILD_GUI_DIR)/mzdisk$(EXE_SUFFIX) $(PORTABLE_GUI_DIR)/
	cp -r ui_resources $(PORTABLE_GUI_DIR)/
	cp -r $(LOCALE_DIST) $(PORTABLE_GUI_DIR)/
ifneq ($(MSYSTEM),)
	@echo "--- Copying MSYS2 DLL libraries ($$MSYSTEM_PREFIX) ---"
	@ldd $(PORTABLE_GUI_DIR)/mzdisk$(EXE_SUFFIX) \
		| awk '{print $$3}' \
		| grep -i "^$$MSYSTEM_PREFIX/" \
		| sort -u \
		| while read dll; do \
			echo "  $$(basename $$dll)"; \
			cp "$$dll" $(PORTABLE_GUI_DIR)/; \
		done
endif
	@echo ""
	@echo "=== Done: $(PORTABLE_GUI_DIR)/ ==="

portable-gui-clean:
	@if [ -d $(PORTABLE_GUI_DIR) ]; then rm -rf $(PORTABLE_GUI_DIR); fi
	@if [ -d portable ] && [ -z "$$(ls -A portable 2>/dev/null)" ]; then rmdir portable; fi

# --- Portable CLI bundle (portable/mzdisk-cli/) ---
#
# Sestaví distribuční balík CLI nástrojů:
#   - bin/         - všechny mzdsk-* binárky
#   - docs/cz/     - .md dokumentace (česky), volitelně i .html
#   - docs/en/     - .md dokumentace (anglicky), volitelně i .html
#
# HTML dokumentace není závislostí - pokud adresář docs-html/ existuje
# (typicky po `make docs-html` nebo `make portable-full`), přikopíruje se.
# Jinak se přeskočí s informační hláškou. Tím `make portable-cli` a
# `make portable` fungují i bez python3 a Python modulu `markdown`.

portable-cli: cli
	@echo ""
	@echo "=== Building portable CLI in $(PORTABLE_CLI_DIR)/ ==="
	@rm -rf $(PORTABLE_CLI_DIR)
	@mkdir -p $(PORTABLE_CLI_DIR)/bin
	@mkdir -p $(PORTABLE_CLI_DIR)/docs/cz
	@mkdir -p $(PORTABLE_CLI_DIR)/docs/en
	cp $(BUILD_CLI_DIR)/mzdsk-*$(EXE_SUFFIX) $(PORTABLE_CLI_DIR)/bin/
	cp docs/cz/tools-*.md $(PORTABLE_CLI_DIR)/docs/cz/
	cp docs/en/tools-*.md $(PORTABLE_CLI_DIR)/docs/en/
	@if [ -d docs-html ]; then \
		echo "--- Including HTML docs from docs-html/ ---"; \
		cp docs-html/cz/tools-*.html $(PORTABLE_CLI_DIR)/docs/cz/ 2>/dev/null || true; \
		cp docs-html/en/tools-*.html $(PORTABLE_CLI_DIR)/docs/en/ 2>/dev/null || true; \
	else \
		echo "--- HTML docs not present (run 'make docs-html' or 'make portable-full' to include them) ---"; \
	fi
	@echo ""
	@echo "=== Done: $(PORTABLE_CLI_DIR)/ ==="

portable-cli-clean:
	@if [ -d $(PORTABLE_CLI_DIR) ]; then rm -rf $(PORTABLE_CLI_DIR); fi
	@if [ -d portable ] && [ -z "$$(ls -A portable 2>/dev/null)" ]; then rmdir portable; fi

# --- Release archivy (.zip) ---
#
# `release-gui` vytvoří portable/mzdisk-gui-X.Y.Z.zip z portable/mzdisk-gui-X.Y.Z/.
# `release-cli` vytvoří portable/mzdisk-cli-X.Y.Z.zip z portable/mzdisk-cli-X.Y.Z/.
# Archivy obsahují adresář s verzí v názvu (po unzipu nevytečou soubory).
# Závislost: `cd portable && zip -r ...` zajistí správnou vnitřní strukturu.

release: release-gui release-cli

release-gui: portable-gui
	@echo ""
	@echo "=== Creating $(PORTABLE_GUI_ZIP) ==="
	@rm -f $(PORTABLE_GUI_ZIP)
	@cd portable && zip -rq $(PORTABLE_GUI_NAME).zip $(PORTABLE_GUI_NAME)
	@echo "=== Done: $(PORTABLE_GUI_ZIP) ==="

release-cli: portable-cli
	@echo ""
	@echo "=== Creating $(PORTABLE_CLI_ZIP) ==="
	@rm -f $(PORTABLE_CLI_ZIP)
	@cd portable && zip -rq $(PORTABLE_CLI_NAME).zip $(PORTABLE_CLI_NAME)
	@echo "=== Done: $(PORTABLE_CLI_ZIP) ==="

release-clean:
	@rm -f portable/mzdisk-gui-*.zip portable/mzdisk-cli-*.zip
	@if [ -d portable ] && [ -z "$$(ls -A portable 2>/dev/null)" ]; then rmdir portable; fi

# --- HTML dokumentace (docs/*/*.md -> docs-html/*/*.html) ---

DOCS_MD   := $(wildcard docs/*/*.md)
DOCS_HTML := $(patsubst docs/%.md,docs-html/%.html,$(DOCS_MD))

docs-html: $(DOCS_HTML)

docs-html/%.html: docs/%.md tools/md2html.py
	@mkdir -p $(dir $@)
	python3 tools/md2html.py $< $@

docs-html-clean:
	@if [ -d docs-html ]; then rm -rf docs-html; fi

# --- Čištění ---

clean: libs-clean cli-clean gui-clean tests-clean portable-gui-clean portable-cli-clean release-clean docs-html-clean

rebuild: clean libs cli gui
