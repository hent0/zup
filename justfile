default:
  @just --list

# Build
build *extraFlags='':
	@cmake -B build -S .
	@cmake --build ./build

# Exec
run *args='': build
  @./build/zup {{args}}

test-build: build
  ./build/zup examples/001.zup -ir
  clang -Qunused-arguments -Wno-override-module a.ll -o a.out

# Test
test *args='': build
  @./run-tests.sh {{args}}

# Clean
clean:
    @rm -rf build
