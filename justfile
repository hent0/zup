default:
  @just --list

# Build
build *extraFlags='':
	@cmake -B build -S .
	@cmake --build ./build

# Exec
run *args='': build
  @./build/zup {{args}}

# Build stage1
build-stage1 *args='': build
  @./build/zup stage1/src/main.zup -o build/stage1

run-stage1 *args='': build-stage1
  @./build/stage1 {{args}}

# Test
test *args='': build
  @./run-tests.sh {{args}}

test-stage1 *args='': build-stage1
  @ZUP_BIN=build/stage1 ./run-tests.sh {{args}}

# Diff stage0 vs stage1 LLVM IR for one input
test-ir file: build-stage1
  @./build/zup {{file}} -ir -o - > build/og.ll
  @./build/stage1 {{file}} -ir -o - > build/new.ll
  @diff --color -u build/og.ll build/new.ll && echo "test-ir: identical"

# Clean
clean:
    @rm -rf build
