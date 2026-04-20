.PHONY: all dev release test format format-check tidy cppcheck lint clean

all: dev

dev:
	cmake --preset dev
	cmake --build --preset dev

release:
	cmake --preset release
	cmake --build --preset release

test: dev
	ctest --preset dev

format:
	cmake --preset dev
	cmake --build --preset format

format-check:
	cmake --preset dev
	cmake --build --preset format-check

tidy:
	cmake --preset dev
	cmake --build --preset dev
	cmake --build --preset tidy

cppcheck:
	cmake --preset dev
	cmake --build --preset cppcheck

lint: format-check tidy cppcheck

clean:
	rm -rf build
