.PHONY: all dev release test format format-check tidy cppcheck lint coverage clean

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

# Coverage flow: configure with --coverage, run the suite, post-
# process with lcov + genhtml. Output lands in build/coverage/lcov
# and build/coverage/html. lcov / genhtml are not invoked when the
# binaries are missing — the configure + ctest steps remain useful
# on their own for IDE-driven coverage tooling.
coverage:
	cmake --preset coverage
	cmake --build --preset coverage
	ctest --preset coverage
	@if command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then \
		lcov --capture --directory build/coverage \
		     --output-file build/coverage/lcov.info \
		     --rc lcov_branch_coverage=1 \
		     --ignore-errors gcov,unused,inconsistent,mismatch >/dev/null; \
		lcov --remove build/coverage/lcov.info '/usr/*' '*/tests/*' \
		     --output-file build/coverage/lcov.filtered.info \
		     --ignore-errors unused,inconsistent >/dev/null; \
		genhtml build/coverage/lcov.filtered.info \
		        --output-directory build/coverage/html \
		        --branch-coverage >/dev/null; \
		echo "coverage report: build/coverage/html/index.html"; \
	else \
		echo "lcov / genhtml not found; raw .gcda files in build/coverage/"; \
	fi

clean:
	rm -rf build
