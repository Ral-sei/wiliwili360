OXDK_ROOT := /mnt/h/OXDK
OXDK360_DIR := /mnt/h/OXDK/xbox360
OXDK360_LIBCXX := 1
OXDK360_LIBCXX_DIR := /root/oxdk-llvm/libcxx/include
XENON_ABI := 1
CLANG := /root/oxdk-llvm/build/bin/clang
include /mnt/h/OXDK/xbox360/xbox360.mk

print:
	@echo '$(OXDK360_CXXFLAGS)'
