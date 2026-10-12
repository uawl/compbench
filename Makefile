CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -ldl -lm

.PHONY: all v1 v2 test test-v1 test-v2 demo demo-v1 demo-v2 prompt prompt-v1 prompt-v2 evaluate-v1 evaluate-v2 charts charts-v1 charts-v2 clean
all: v1 v2
v1: build/v1/compbench build/v1/baseline-compiler
v2: build/v2/compbench build/v2/baseline-compiler

define VERSION_RULES
build/$(1):
	mkdir -p $$@

build/$(1)/compbench: $(1)/src/harness.c $(1)/src/ast.c $(1)/src/codegen.c $(1)/include/ast.h | build/$(1)
	$$(CC) $$(CPPFLAGS) -I$(1)/include $$(CFLAGS) $(1)/src/harness.c $(1)/src/ast.c $(1)/src/codegen.c -o $$@ $$(LDLIBS)

build/$(1)/baseline-compiler: $(1)/src/baseline.c $(1)/src/ast.c $(1)/src/codegen.c $(1)/include/ast.h | build/$(1)
	$$(CC) $$(CPPFLAGS) -I$(1)/include $$(CFLAGS) $(1)/src/baseline.c $(1)/src/ast.c $(1)/src/codegen.c -o $$@

build/$(1)/test-ast: $(1)/tests/test_ast.c $(1)/src/ast.c $(1)/include/ast.h | build/$(1)
	$$(CC) $$(CPPFLAGS) -I$(1)/include $$(CFLAGS) $(1)/tests/test_ast.c $(1)/src/ast.c -o $$@

test-$(1): $(1) build/$(1)/test-ast
	./build/$(1)/test-ast
	python3 $(1)/tests/test_harness.py

demo-$(1): $(1)
	./build/$(1)/compbench --compiler ./build/$(1)/baseline-compiler --output build/$(1)/results.json

prompt-$(1):
	python3 scripts/build_prompt.py --version $(1)

evaluate-$(1): $(1)
	python3 scripts/evaluate.py --version $(1) --cc '$$(CC)'

charts-$(1):
	python3 scripts/render_results.py --version $(1)
endef

$(eval $(call VERSION_RULES,v1))
$(eval $(call VERSION_RULES,v2))

test: test-v1 test-v2
	python3 tests/test_versions.py

demo: demo-v2
prompt: prompt-v1 prompt-v2
charts: charts-v1

clean:
	rm -rf build
