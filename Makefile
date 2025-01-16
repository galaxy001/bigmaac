CC ?= gcc
CXX ?= g++
OFLAGS = -Wall -pthread
LDFLAGS = -lc

OMPFLAGS := -fopenmp
COMPILER_VERSION := $(shell $(CC) --version)
ifneq '' '$(findstring clang,$(COMPILER_VERSION))'
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		OSFLAG += -D OSX
		# https://mac.r-project.org/openmp/#do
		OMPFLAGS = -Xclang -fopenmp -lomp -I/usr/local/opt/libomp/include -L/usr/local/opt/libomp/lib
		# https://stackoverflow.com/a/60043467
		# brew install llvm libomp
	endif
endif

BIGMAAC_ENV_TEMPLATE := BIGMAAC_TEMPLATE="/tmp/bigmaax.XXXXXXXX"
BIGMAAC_ENV_FRY      := BIGMAAC_MIN_FRY_SIZE=0 SIZE_FRIES=549755813888
BIGMAAC_ENV_BIGMAAC  := BIGMAAC_MIN_BIGMAAC_SIZE=314572800 SIZE_BIGMAAC=549755813888
BIGMAAC_ENV_DEFAULT  := $(BIGMAAC_ENV_TEMPLATE) $(BIGMAAC_ENV_FRY) $(BIGMAAC_ENV_BIGMAAC)

BINARY := bigmaac.so bigmaac_debug.so preload test_bigmaac bigmaac_main bigmaac_main_debug c_test c_app

all: $(BINARY)

bigmaac_main: bigmaac.c bigmaac.h
	$(CC) $(OFLAGS) -DMAIN bigmaac.c -o bigmaac_main -Wall -g -ldl $(OMPFLAGS)

bigmaac_main_debug: bigmaac.c bigmaac.h
	$(CC) $(OFLAGS) -DMAIN -DDEBUG bigmaac.c -o bigmaac_main_debug -Wall -g -ldl $(OMPFLAGS)

bigmaac.so: bigmaac.c bigmaac.h
	$(CC) $(OFLAGS) -shared -fPIC bigmaac.c -o bigmaac.so -ldl -Wall -O3

bigmaac_debug.so: bigmaac.c bigmaac.h
	$(CC) $(OFLAGS) -shared -DDEBUG -fPIC bigmaac.c -o bigmaac_debug.so -ldl -Wall -g

preload: preload.c
	$(CC) -Wall preload.c -o preload

test_bigmaac: test_bigmaac.c bigmaac.h
	$(CC) -Wall test_bigmaac.c -o test_bigmaac -g

c_test: c_test.c
	$(CC) $(OFLAGS) -O3  $< -o $@ -lc -g $(LDFLAGS) $(OMPFLAGS)

c_app: c_test.c bigmaac.c
	$(CC) $(OFLAGS) -O3 -DNOTCOMPAT $^ -o $@ -lc -g $(LDFLAGS) $(OMPFLAGS)

.PHONY: clean all test fmt run

test: bigmaac.so test_bigmaac preload
	./test_bigmaac > output_without_bigmaac
	$(BIGMAAC_ENV_DEFAULT) ./preload ./bigmaac.so ./test_bigmaac > output_with_bigmaac

clean:
	rm -f $(BINARY) output_with_bigmaac output_without_bigmaac
	rm -fr *.dSYM

fmt:
	clang-format -i *.c *.h

run: c_test
	 $(BIGMAAC_ENV_DEFAULT) LD_PRELOAD=./bigmaac_debug.so ./c_test
