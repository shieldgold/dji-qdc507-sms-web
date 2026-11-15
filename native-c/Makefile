CC ?= cc
QDC_DEPS ?= $(HOME)/.cache/qdc507-native-c/host/prefix
BUILD ?= build/host
CPPFLAGS += -Iinclude -I$(QDC_DEPS)/include -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CFLAGS ?= -Os -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections -pthread
ifeq ($(QDC_SANITIZE),1)
  CFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=address,undefined
endif
LDLIBS += -L$(QDC_DEPS)/lib -lmbedtls -lmbedx509 -lmbedcrypto -lnghttp2 -lyyjson -pthread -lm
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  LDFLAGS += -Wl,-dead_strip
else
  LDFLAGS += -Wl,--gc-sections
endif
SOURCES := $(wildcard src/*.c)
CORE := $(filter-out src/main.c,$(SOURCES))
OBJECTS := $(patsubst src/%.c,$(BUILD)/%.o,$(SOURCES))
.PHONY: all test test-common test-http test-monitor test-audio test-faults test-crypto test-auth test-domain test-tls test-push test-service clean
all: $(BUILD)/smsweb-c
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: src/%.c include/qdc.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/web_assets.c: tools/embed_web.py ../web/index.html ../web/style.css ../web/model.js ../web/app.js | $(BUILD)
	python3 tools/embed_web.py ../web $@
$(BUILD)/web_assets.o: $(BUILD)/web_assets.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/smsweb-c: $(OBJECTS) $(BUILD)/web_assets.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(BUILD)/test_common: tests/test_common.c src/common.c include/qdc.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_common.c src/common.c $(LDLIBS) -o $@
test-common: $(BUILD)/test_common
	$(BUILD)/test_common
$(BUILD)/test_http: tests/test_http.c src/http.c src/common.c include/qdc.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_http.c src/http.c src/common.c $(LDLIBS) -o $@
test-http: $(BUILD)/test_http
	$(BUILD)/test_http
$(BUILD)/test_monitor: tests/test_monitor.c src/monitor.c src/traffic.c src/common.c include/qdc_monitor.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_monitor.c src/monitor.c src/traffic.c src/common.c $(LDLIBS) -o $@
test-monitor: $(BUILD)/test_monitor
	$(BUILD)/test_monitor
$(BUILD)/test_audio: tests/test_audio.c src/audio.c src/common.c include/qdc_audio.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_audio.c src/audio.c src/common.c $(LDLIBS) -o $@
$(BUILD)/audio_driver: tests/audio_driver.c src/audio.c src/common.c src/crypto.c include/qdc_audio.h | $(BUILD)
	$(CC) $(CPPFLAGS) -DQDC_TESTING $(CFLAGS) $(LDFLAGS) tests/audio_driver.c src/audio.c src/common.c src/crypto.c $(LDLIBS) -o $@
test-audio: $(BUILD)/test_audio $(BUILD)/audio_driver
	$(BUILD)/test_audio
	python3 tests/test_audio_stream.py $(BUILD)/audio_driver
$(BUILD)/test_crypto: tests/test_crypto.c src/crypto.c src/common.c include/qdc.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_crypto.c src/crypto.c src/common.c $(LDLIBS) -o $@
test-crypto: $(BUILD)/test_crypto
	$(BUILD)/test_crypto
$(BUILD)/test_at_limits: tests/test_at_limits.c src/at.c src/common.c include/qdc_domain.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_at_limits.c src/common.c $(LDLIBS) -o $@
$(BUILD)/test_at_stop: tests/test_at_stop.c src/at.c src/common.c include/qdc_domain.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_at_stop.c src/common.c $(LDLIBS) -o $@
$(BUILD)/test_store_memory: tests/test_store_memory.c src/store.c src/common.c include/qdc_domain.h | $(BUILD)
	$(CC) $(CPPFLAGS) -DQDC_TESTING $(CFLAGS) $(LDFLAGS) tests/test_store_memory.c src/store.c src/common.c $(LDLIBS) -o $@
$(BUILD)/test_call_oom: tests/test_call_oom.c src/calls.c src/pdu.c src/crypto.c src/common.c include/qdc_domain.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_call_oom.c src/pdu.c src/crypto.c src/common.c $(LDLIBS) -o $@
$(BUILD)/test_restore_oom: tests/test_restore_oom.c src/store.c src/common.c include/qdc_domain.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_restore_oom.c src/common.c $(LDLIBS) -o $@
$(BUILD)/test_traffic_oom: tests/test_traffic_oom.c src/traffic.c src/common.c include/qdc_monitor.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_traffic_oom.c src/common.c $(LDLIBS) -o $@
test-faults: $(BUILD)/test_at_limits $(BUILD)/test_at_stop $(BUILD)/test_store_memory $(BUILD)/test_call_oom $(BUILD)/test_restore_oom $(BUILD)/test_traffic_oom
	$(BUILD)/test_at_limits
	$(BUILD)/test_at_stop
	$(BUILD)/test_store_memory
	$(BUILD)/test_call_oom
	$(BUILD)/test_restore_oom
	$(BUILD)/test_traffic_oom
test-auth:
	QDC_DEPS="$(abspath $(QDC_DEPS))" CC="$(CC)" QDC_SANITIZE="$(QDC_SANITIZE)" sh tests/test_auth_go.sh
	QDC_DEPS="$(abspath $(QDC_DEPS))" CC="$(CC)" QDC_SANITIZE="$(QDC_SANITIZE)" sh tests/test_auth_clock.sh
	QDC_DEPS="$(abspath $(QDC_DEPS))" CC="$(CC)" QDC_SANITIZE="$(QDC_SANITIZE)" sh tests/test_auth_timing.sh
	QDC_DEPS="$(abspath $(QDC_DEPS))" CC="$(CC)" QDC_SANITIZE="$(QDC_SANITIZE)" sh tests/test_auth_backup.sh
test-domain:
	python3 tools/test_domain.py --deps "$(abspath $(QDC_DEPS))" --build "$(abspath $(BUILD))/domain-tests" $(if $(filter 1,$(QDC_SANITIZE)),--sanitize,)
	QDC_DEPS="$(abspath $(QDC_DEPS))" python3 tools/test_deliver_parity.py
$(BUILD)/test_tls: tests/test_tls.c src/tls.c src/crypto.c src/clock.c src/common.c include/qdc_push.h | $(BUILD)
	$(CC) $(CPPFLAGS) -DQDC_TESTING $(CFLAGS) $(LDFLAGS) tests/test_tls.c src/tls.c src/crypto.c src/clock.c src/common.c $(LDLIBS) -o $@
test-tls: $(BUILD)/test_tls
	$(BUILD)/test_tls
	python3 tests/test_tls_network.py --binary $(BUILD)/test_tls
$(BUILD)/test_push: tests/test_push.c src/push.c src/tls.c src/crypto.c src/common.c include/qdc_push.h | $(BUILD)
	$(CC) $(CPPFLAGS) -DQDC_TESTING -DQDC_PUSH_STANDALONE $(CFLAGS) $(LDFLAGS) tests/test_push.c src/push.c src/tls.c src/crypto.c src/common.c $(LDLIBS) -o $@
test-push: $(BUILD)/test_push
	$(BUILD)/test_push
	python3 tests/test_push_network.py --binary $(BUILD)/test_push
test-service: $(BUILD)/smsweb-c
	python3 tests/test_service.py $(BUILD)/smsweb-c --scenario ready
	python3 tests/test_service.py $(BUILD)/smsweb-c --scenario pin
test: test-common test-http test-monitor test-audio test-crypto test-faults test-auth test-domain test-tls test-push test-service
clean:
	rm -rf $(BUILD)
-include $(OBJECTS:.o=.d) $(BUILD)/web_assets.d
