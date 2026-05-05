CXX      := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
LDFLAGS  := -framework Hypervisor

hyp: hyp.cpp hyp.entitlements
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ hyp.cpp
	codesign --entitlements hyp.entitlements --force --sign - $@

run: hyp
	./hyp

clean:
	rm -f hyp

.PHONY: run clean
