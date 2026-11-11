# CYD Desk Dashboard - everyday commands. Run `make` (or `mingw32-make` on Windows) to list them.
# Machine-specific settings (library folder, COM port, server arguments) go in local.mk, which is gitignored:
#   LIBS = D:/my/Arduino/libraries
#   PORT = COM5
#   SERVER_ARGS = --plan max-5x --reset-weekday 5 --reset-hour 2
# Recipes are single commands with double quotes only, so they run the same under cmd.exe and sh.

PY          ?= python
ARDUINO_CLI ?= C:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe
FQBN        ?= esp32:esp32:esp32:PartitionScheme=huge_app
LIBS        ?= $(USERPROFILE)/Documents/Arduino/libraries
PORT        ?= COM8
SKETCH      ?= ESP32_tft_clock_weather
BUILD_DIR   ?= .build
SERVER_ARGS ?= --plan pro
SECS        ?= 60
-include local.mk

CAPTURE = $(PY) -I tools/cyd_capture.py $(PORT)

.PHONY: help build upload flash test check stats bench log setup restart tab shot tour server-restart server-status version clean

## Build and device (PORT from local.mk, default COM8):
##   make build            compile the firmware into .build/
##   make flash            build + upload over USB
##   make upload           upload the last build
##   make stats            health JSON: real free RAM (heap = DRAM), frame time, latencies
##   make bench            full-redraw time of 7 pages (measure before and after UI changes)
##   make log SECS=60      print the serial log: portal requests, crashes, reset reasons
##   make tab N=9          open a page; make shot FILE=x.png; make tour DIR=screens
##   make setup            reboot into Wi-Fi setup; make restart reboots
## PC server and repo:
##   make test             server unit tests
##   make server-restart   restart the PC server, re-reading calendar.txt and token.txt
##   make server-status    is the server running, and independent of Claude Code? (CYD shows OFFLINE if not)
##   make check            tests + secret scan of the commits not pushed yet: run before every push
##   make version V=2.0.4  set the version in firmware, server and web manifest
##   make clean            remove .build/
help:
	@$(PY) -c "[print(l[3:].rstrip()) for l in open('Makefile', encoding='utf-8') if l.startswith('## ')]"

# The IDE build also leaves $(SKETCH)/build behind when started from the sketch folder: always remove it.
build:
	"$(ARDUINO_CLI)" compile --fqbn $(FQBN) --libraries "$(LIBS)" --output-dir $(BUILD_DIR) $(SKETCH)
	$(PY) -c "import shutil; shutil.rmtree('$(SKETCH)/build', ignore_errors=True)"

upload:
	"$(ARDUINO_CLI)" upload --fqbn $(FQBN) -p $(PORT) --input-dir $(BUILD_DIR) $(SKETCH)

flash: build upload

test:
	$(PY) -m unittest discover -s pc_server/tests

check: test
	$(PY) -I tools/secret_scan.py

stats:
	$(CAPTURE) stats

bench:
	$(CAPTURE) cmd BENCH --secs 8

log:
	$(CAPTURE) log --secs $(SECS)

setup:
	$(CAPTURE) cmd SETUP --secs 8

restart:
	$(CAPTURE) cmd RESTART --secs 3

tab:
	$(CAPTURE) tab $(N)

shot:
	$(CAPTURE) shot $(if $(FILE),$(FILE),cyd.png)

tour:
	$(CAPTURE) tour $(if $(DIR),$(DIR),screens)

server-restart:
	$(PY) -I tools/server_restart.py $(SERVER_ARGS)

server-status:
	$(PY) -I tools/server_status.py

version:
	$(PY) -I tools/bump_version.py $(V)

clean:
	$(PY) -c "import shutil; shutil.rmtree('$(BUILD_DIR)', ignore_errors=True)"
