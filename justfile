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

# Clean
clean:
    @rm -rf build
