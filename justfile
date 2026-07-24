default:
  @just --list

# Build
stage0 *extraFlags='':
	@cmake -B build -S stage0
	@cmake --build ./build

# Exec
run *args='': build
  @./build/zup {{args}}

# Build stage1
build *args='': stage0
  @./build/stage0 src/main.zup -o build/zup

# Bootstrap: build the self-hosting chain and require the fixed point
bootstrap *args='': build
  @./build/zup src/main.zup -o build/stage2
  @./build/zup src/main.zup -ir -o - > /tmp/s1.ll
  @./build/stage2 src/main.zup -ir -o - > /tmp/s2.ll
  @diff /tmp/s1.ll /tmp/s2.ll && echo "bootstrap: fixed point reached"

# Test
test *args='': build
  @./run-tests.sh {{args}}

test-stage0 *args='': stage0
  @ZUP_BIN=build/stage0 ./run-tests.sh {{args}}

# Diff stage0 vs stage1 LLVM IR for one input
test-ir file: stage0 build
  @./build/stage0 {{file}} -ir -o - > /tmp/stage0.ll
  @./build/zup {{file}} -ir -o - > /tmp/zup.ll
  @diff --color -u /tmp/stage0.ll /tmp/zup.ll && echo "test-ir: identical"

# Clean
clean:
    @rm -rf build
