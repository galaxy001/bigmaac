#define _GNU_SOURCE
#include "bigmaac.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <malloc/malloc.h>
#define MALLOCSIZE malloc_size
#else
#error "Unsupported Apple platform"
#endif
#elif __linux__
#include <malloc.h>
#define MALLOCSIZE malloc_usable_size
#else
#error "Unsupported compiler"
#endif

#if defined(__clang__)
#define FORCE_INLINE __attribute__((__always_inline__, __gnu_inline__)) static inline
#elif defined(__GNUC__)
#define FORCE_INLINE __attribute__((__always_inline__)) static inline
#elif defined(_MSC_VER)
#pragma warning(error : 4714)
#define FORCE_INLINE __forceinline
#else
#warning Unsupported compiler, fall back to `static inline`.
#define FORCE_INLINE static inline
#endif

#if defined(NOTCOMPAT)
#include "mmap_malloc.h"
#define PREFIX(x) mmap_##x
#else
#define PREFIX(x) x
#endif

#define OOM()                                                      \
	fprintf(stderr, "BigMaac : Failed to find available space\n"); \
	errno = ENOMEM;
#define LARGER_GAP(h, a, b) (((h->node_array[a]->size) > (h->node_array[b]->size)) ? a : b)
#define SIZE_TO_MULTIPLE(size, multiple) ((size % multiple) > 0 ? size + (multiple - size % multiple) : size)
#define SWAP_NODES(na, idx_a, idx_b)  \
	{                                 \
		na[idx_a]->heap_idx = idx_b;  \
		na[idx_b]->heap_idx = idx_a;  \
		struct node* tmp = na[idx_a]; \
		na[idx_a] = na[idx_b];        \
		na[idx_b] = tmp;              \
	}
#define UNLINK(n)                            \
	{                                        \
		node* tmp = n;                       \
		tmp->next->previous = tmp->previous; \
		tmp->previous->next = tmp->next;     \
	}

enum memory_use { IN_USE = 0, FREE = 1 };
enum load_status { LIBRARY_FAIL = -1, NOT_LOADED = 0, LOADING_MEM_FUNCS = 1, LOADING_LIBRARY = 2, LOADED = 3 };

typedef struct heap {
	size_t used;
	size_t length;
	struct node** node_array;
} heap;

typedef struct node {
	struct node* next;
	struct node* previous;
	enum memory_use in_use;
	int heap_idx;
	char* ptr;
	size_t size;
	heap* heap;
} node;

// heap operations
static void heap_remove_idx(heap* const heap, const int idx);
static void heapify_up(heap* const heap, const int idx);
static void heapify_down(heap* const heap, const int idx);
static int heap_insert(node* const head, node* const n);
static int heap_free_node(node* const head, node* const n);
static node* heap_pop_split(node* const head, const size_t size);
static node* heap_find_node(void* const ptr);
static void heapify_down(heap* const heap, const int idx);

// linked list operations
static node* ll_new(void* const ptr, const size_t size);

static void bigmaac_init(void);

// BigMaac helper functions
static int mmap_tmpfile(void* const ptr, const size_t size);
static int remove_chunk_with_ptr(void* const ptr, void* const prev_ptr, const size_t prev_size);
static void* create_chunk(const size_t size);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void* (*real_malloc)(size_t) = NULL;
static void* (*real_calloc)(size_t, size_t) = NULL;
static void* (*real_free)(size_t) = NULL;
static void* (*real_realloc)(void*, size_t) = NULL;
// static void* (*real_reallocarray)(void*, size_t, size_t) = NULL;

// GLOBAL vars
static int active_mmaps = 0;

static node* _head_bigmaacs;  // head of the bigmaac heap
static node* _head_fries;     // head of the fries heap

static size_t min_size_bigmaac = DEFAULT_MIN_BIGMAAC_SIZE;
static size_t min_size_fry = DEFAULT_MIN_FRY_SIZE;

static void* base_fries = 0x0;
static void* base_bigmaac = 0x0;
static void* end_fries = 0x0;
static void* end_bigmaac = 0x0;

static size_t size_fries = DEFAULT_MAX_FRIES;
static size_t size_bigmaac = DEFAULT_MAX_BIGMAAC;
static char* template = DEFAULT_TEMPLATE;
static size_t fry_size_multiple = DEFAULT_FRY_SIZE_MULTIPLE;

static size_t used_fries = 0;
static size_t used_bigmaacs = 0;
static size_t page_size = 0;

static enum load_status load_state = NOT_LOADED;

// debug functions
static inline void verify_memory(node* head, int global);
static inline void log_bm(const char* data, ...);
#ifdef DEBUG
static void print_ll(node* head);
static void print_heap(heap* heap);
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE* f;
static int this_pid = 0;

void log_bm(const char* data, ...) {
	pthread_mutex_lock(&log_lock);
	int pid = getpid();
	if (pid != this_pid) {
		char s[1024];
		sprintf(s, "%d.log", pid);
		f = fopen(s, "w");
		setbuf(f, NULL);
		this_pid = pid;
	}

	va_list pl;
	va_start(pl, data);
	vfprintf(f, data, pl);

	fflush(f);
	fsync(fileno(f));
	pthread_mutex_unlock(&log_lock);
}

static inline void verify_memory(node* head, int global) {
	// print_heap(head->heap);
	// print_ll(head);
	size_t heap_free = 0;
	for (int i = 0; i < head->heap->used; i++) {
		assert(head->heap->node_array[i]->ptr != NULL);
		heap_free += head->heap->node_array[i]->size;
	}
	size_t t = 0;
	size_t ll_free = 0;
	node* prev = NULL;
	node* c = head;
	while (c != NULL) {
		if (c->in_use == FREE) {
			ll_free += c->size;
		}
		t += c->size;
		assert(c->previous == prev);
		prev = c;
		c = c->next;
	}
	if (global == 1) {
		if (head == _head_fries) {
			assert(used_fries == t - ll_free);
		} else {
			assert(used_bigmaacs == t - ll_free);
		}
	}
	assert(heap_free == ll_free);
	assert(t == size_bigmaac);
}

static __attribute__((__unused__)) void print_ll(node* head) {
	while (head != NULL) {
		fprintf(stderr, "%p n=%p, u=%d, p=%p, size=%ld, ptr=%p\n", head, head->next, head->in_use, head->previous, head->size, head->ptr);
		head = head->next;
	}
}

static __attribute__((__unused__)) void print_heap(heap* heap) {
	for (int i = 0; i < heap->used; i++) {
		fprintf(stderr, "parent %d node %d , ptr=%p size=%ld\n", (i - 1) / 2, i, heap->node_array[i]->ptr, heap->node_array[i]->size);
	}
}

#else

static inline void verify_memory(node* head, int global) {}
static inline void log_bm(const char* data, ...) {}

#endif

// BigMaac heap

static void heap_remove_idx(heap* const heap, const int idx) {
	if (heap->used == 1) {
		heap->used = 0;
		heap->node_array[0]->heap_idx = -1;
		return;
	}

	// take the last one and place it here
	heap->node_array[idx]->heap_idx = -1;              // node is out of the heap
	heap->node_array[heap->used - 1]->heap_idx = idx;  // node has moved up in the heap
	heap->node_array[idx] = heap->node_array[heap->used - 1];
	heap->used--;  // the heap is now smaller

	heapify_down(heap, idx);
}

static void heapify_up(heap* const heap, const int idx) {
	if (idx == 0) {  // this node has no parent
		return;
	}

	const int parent_idx = (idx - 1) / 2;

	if (LARGER_GAP(heap, idx, parent_idx) != parent_idx) {
		SWAP_NODES(heap->node_array, idx, parent_idx);
		heapify_up(heap, parent_idx);
	}
}

static void heapify_down(heap* const heap, const int idx) {
	int largest_idx = idx;

	const int left_child_idx = (idx + 1) * 2 - 1;
	const int right_child_idx = (idx + 1) * 2;

	if (left_child_idx < heap->used) {
		largest_idx = LARGER_GAP(heap, largest_idx, left_child_idx);
	}
	if (right_child_idx < heap->used) {
		largest_idx = LARGER_GAP(heap, largest_idx, right_child_idx);
	}
	if (largest_idx != idx) {
		SWAP_NODES(heap->node_array, idx, largest_idx);
		heapify_down(heap, largest_idx);
	}  // else we are done
}

static int heap_insert(node* const head, node* const n) {
	heap* heap = head->heap;
	if (heap->used == heap->length) {
		heap->node_array = (node**)real_realloc(heap->node_array, sizeof(node*) * heap->length * 2);
		if (heap->node_array == NULL) {
			fprintf(stderr, "BigMaac : failed to heap insert\n");
			return -1;
		}
		head->heap->length *= 2;
	}
	// gauranteed to have space
	heap->node_array[heap->used] = n;
	n->heap_idx = heap->used;

	heap->used++;

	heapify_up(heap, n->heap_idx);
	return 0;
}

static int heap_free_node(node* const head, node* const n) {
#ifdef DEBUG
	assert(n->in_use == IN_USE);
#endif
	if (n->next != NULL && n->next->in_use == FREE) {
		if (n->previous != NULL && n->previous->in_use == FREE) {
			// update size and pointer
			n->size += n->previous->size;
			n->ptr = n->previous->ptr;

			// unlink the node tmp
			node* tmp = n->previous;
			UNLINK(n->previous);

			heap_remove_idx(head->heap, tmp->heap_idx);
			real_free((size_t)tmp);
		}
		// update size and pointer
		n->next->size += n->size;
		n->next->ptr = n->ptr;

		UNLINK(n);
		heapify_up(head->heap, n->next->heap_idx);
		real_free((size_t)n);
	} else if (n->previous != NULL && n->previous->in_use == FREE) {
		// add it to the previous node
		n->previous->size += n->size;

		UNLINK(n);
		heapify_up(head->heap, n->previous->heap_idx);
		real_free((size_t)n);
	} else {  // add a whole new node
		n->in_use = FREE;
		return heap_insert(head, n);
	}
	return 0;
}

static node* heap_pop_split(node* const head, const size_t size) {
	verify_memory(head, 0);
	if (head->heap->used == 0) {
		return NULL;
	}

	heap* heap = head->heap;
	node** node_array = heap->node_array;

	node* free_node = node_array[0];
	if (free_node->size < size) {
		return NULL;
	}

	// check left and right child ( avoid further fragmenting largest chunk )
	// How can you have any pudding if you dont eat yer meat?
	const int left_child_idx = 1;
	if (heap->used > left_child_idx && node_array[left_child_idx]->size >= size) {
		free_node = node_array[left_child_idx];
	}
	const int right_child_idx = 2;
	if (heap->used > right_child_idx && node_array[right_child_idx]->size >= size && node_array[right_child_idx]->size < free_node->size) {
		free_node = node_array[right_child_idx];
	}

	if (free_node->size == size) {
		heap_remove_idx(heap, free_node->heap_idx);
		free_node->in_use = IN_USE;
		verify_memory(head, 1);
		return free_node;
	}

	// need to split this node
	node* used_node = (node*)real_malloc(sizeof(node));
	if (used_node == NULL) {
		return NULL;
	}
	// heapify from this node down
	*used_node = (node){.size = size, .ptr = free_node->ptr, .next = free_node, .previous = free_node->previous, .in_use = IN_USE, .heap_idx = -1};

	free_node->size -= size;  // need to now heapify this node
	free_node->ptr = free_node->ptr + size;

	free_node->previous->next = used_node;
	free_node->previous = used_node;

	heapify_down(heap, free_node->heap_idx);
	verify_memory(head, 1);

	return used_node;
}

static node* heap_find_node(void* const ptr) {
	node* head = ptr < base_bigmaac ? _head_fries : _head_bigmaacs;
	verify_memory(head, 0);
	while (head != NULL) {
		if (head->ptr == ptr) {
			return head;
		}
		head = head->next;
	}
	return NULL;
}

// BigMaac linked list

static node* ll_new(void* const ptr, const size_t size) {
	node* const head = (node*)real_malloc(sizeof(node) * 2);
	if (head == NULL) {
		fprintf(stderr, "BigMalloc heap: failed to make list\n");
		return NULL;
	}

	head[0] = (node){.size = 0, .ptr = NULL, .next = head + 1, .previous = NULL, .in_use = IN_USE, .heap_idx = -1};
	head[1] = (node){.size = size, .ptr = ptr, .next = NULL, .previous = head, .in_use = FREE, .heap_idx = 0};

	head->heap = (heap*)real_malloc(sizeof(heap));
	if (head->heap == NULL) {
		fprintf(stderr, "BigMalloc heap failed\n");
		return NULL;
	}
	head->heap->node_array = (node**)real_malloc(sizeof(node*) * 1);
	if (head->heap->node_array == NULL) {
		fprintf(stderr, "BigMalloc heap failed 2\n");
		return NULL;
	}
	head->heap->length = 1;
	head->heap->used = 1;
	head->heap->node_array[0] = head + 1;

	return head;
}

// BigMaac

static void bigmaac_init(void) {
	pthread_mutex_lock(&lock);
	if (load_state == LIBRARY_FAIL) {
		return;  // error initializing
	}
	if (load_state != NOT_LOADED) {
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "Already init %d\n", load_state);
		return;
	}
	fprintf(stderr, "Loading Bigmaac Heap X! PID:%d PPID:%d\n", getpid(), getppid());
	load_state = LOADING_MEM_FUNCS;
	real_malloc = dlsym(RTLD_NEXT, "malloc");
	real_free = dlsym(RTLD_NEXT, "free");
	real_calloc = dlsym(RTLD_NEXT, "calloc");
	real_realloc = dlsym(RTLD_NEXT, "realloc");
	// real_reallocarray = dlsym(RTLD_NEXT, "reallocarray");
	if (!real_malloc || !real_free || !real_calloc || !real_realloc /* || !real_reallocarray*/) {
		fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
	}
	load_state = LOADING_LIBRARY;

	log_bm("OPEN LIB\n");

	page_size = sysconf(_SC_PAGE_SIZE);

	// load enviornment variables
	const char* env_template = getenv("BIGMAAC_TEMPLATE");
	if (env_template != NULL) {
		template = strdup(env_template);
	}

	const char* env_min_size_bigmaac = getenv("BIGMAAC_MIN_BIGMAAC_SIZE");
	if (env_min_size_bigmaac != NULL) {
		sscanf(env_min_size_bigmaac, "%zu", &min_size_bigmaac);
	}

	const char* env_min_size_fry = getenv("BIGMAAC_MIN_FRY_SIZE");
	if (env_min_size_fry != NULL) {
		sscanf(env_min_size_fry, "%zu", &min_size_fry);
	}
	if (min_size_fry == 0) {
		min_size_fry = min_size_bigmaac;  // disabled
	}

	if (min_size_fry > min_size_bigmaac) {
		fprintf(stderr, "BigMaac: Failed to initialize library, fries must be smaller than bigmaac, %ld %ld\n", min_size_fry, min_size_bigmaac);
		load_state = LIBRARY_FAIL;
		return;
	}

	const char* env_size_fries = getenv("SIZE_FRIES");
	if (env_size_fries != NULL) {
		sscanf(env_size_fries, "%zu", &size_fries);
	}
	const char* env_size_bigmaac = getenv("SIZE_BIGMAAC");
	if (env_size_bigmaac != NULL) {
		sscanf(env_size_bigmaac, "%zu", &size_bigmaac);
	}

	const size_t size_total = size_fries + size_bigmaac;
	base_fries = mmap(NULL, size_total, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);  // reserve the full contiguous range
	if (base_fries == MAP_FAILED) {
		fprintf(stderr, "BigMaac: Failed to initialize library %s\n", strerror(errno));
		load_state = LIBRARY_FAIL;
		pthread_mutex_unlock(&lock);
		return;
	}
	active_mmaps++;

	const int ret = mmap_tmpfile(base_fries, size_fries);  // allocate fries right away
	if (ret < 0) {
		fprintf(stderr, "BigMaac: Failed to initialize library\n");
		load_state = LIBRARY_FAIL;
		pthread_mutex_unlock(&lock);
		return;
	}

	end_fries = ((char*)base_fries) + size_fries;

	base_bigmaac = end_fries;
	end_bigmaac = ((char*)base_fries) + size_total;

	// initialize a heap
	_head_bigmaacs = ll_new(base_bigmaac, size_bigmaac);
	_head_fries = ll_new(base_fries, size_fries);

	load_state = LOADED;
	pthread_mutex_unlock(&lock);
}

// BigMaac helper functions

static int mmap_tmpfile(void* const ptr, const size_t size) {
	char* const filename = (char*)real_malloc(sizeof(char) * (strlen(template) + 1));
	if (filename == NULL) {
		fprintf(stderr, "Bigmaac: failed to allocate memory in mmap_tmpfile\n");
		return -1;
	}
	strcpy(filename, template);
	fprintf(stderr, "BIGMAAC: make file %0.2f MB\n", ((double)size) / (1024.0 * 1024.0));
	const int fd = mkstemp(filename);
	if (fd < 0) {
		fprintf(stderr, "Bigmaac: Failed to make temp file %s\n", strerror(errno));
		real_free((size_t)filename);
		return -1;
	}

	int ret = unlink(filename);
	if (ret != 0) {
		fprintf(stderr, "BigMaac: unlink tmpfile failed! %s\n", strerror(errno));
		real_free((size_t)filename);
		return -1;
	}
	real_free((size_t)filename);

	ret = ftruncate(fd, size);  // resize the file
	if (ret != 0) {
		fprintf(stderr, "BigMaac: ftruncate failed! %s\n", strerror(errno));
		return -1;
	}
	void* ret_ptr = mmap(ptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	if (ret_ptr == MAP_FAILED) {
		fprintf(stderr, "BigMaac: mmap failed! mmap() [ active mmaps %d , bigmaac capacity free: %0.2f , fries capacity free: %0.2f, check /proc/sys/vm/max_map_count : %s\n", active_mmaps,
		        1.0 - ((float)used_fries) / size_fries, 1.0 - ((float)used_bigmaacs) / size_bigmaac, strerror(errno));
		return -1;
	}
	active_mmaps++;

	ret = close(fd);  // mmap keeps the fd open now
	if (ret == -1) {
		fprintf(stderr, "BigMaac: close fd failed! %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static void* create_chunk(size_t size) {
	node* const head = size > min_size_bigmaac ? _head_bigmaacs : _head_fries;  // TODO lock per head?
	pthread_mutex_lock(&lock);                                                  // keep lock here so that verify is consistent
	// page align the size requested
	if (head == _head_bigmaacs) {
		size = SIZE_TO_MULTIPLE(size, page_size);
		used_bigmaacs += size;
	} else {
		size = SIZE_TO_MULTIPLE(size, fry_size_multiple);
		used_fries += size;
	}

	node* heap_chunk = heap_pop_split(head, size);
	pthread_mutex_unlock(&lock);

	if (heap_chunk == NULL) {
		return NULL;
	}

	if (head == _head_bigmaacs) {
		int ret = mmap_tmpfile(heap_chunk->ptr, size);
		if (ret < 0) {
			return NULL;
		}
	}

	return heap_chunk->ptr;
}

FORCE_INLINE void memblock_copy(void* const old_ptr, void* const new_ptr, const size_t old_size, const size_t new_size, const bool from_heap) {
	const size_t m = (old_size < new_size) ? old_size : new_size;
	memcpy(new_ptr, old_ptr, m);
#ifdef DEBUG
	if (from_heap)
		log_bm("realloc Mmap[%p]%zu <--%ld-- Heap[%p]%zu\n", new_ptr, new_size, m, old_ptr, old_size);
	else
		log_bm("realloc Mmap[%p]%zu <--%ld-- Mmap[%p]%zu\n", new_ptr, new_size, m, old_ptr, old_size);
#endif
}

static int remove_chunk_with_ptr(void* const ptr, void* const new_ptr, const size_t new_size) {
	pthread_mutex_lock(&lock);

	node* n = heap_find_node(ptr);
	if (n == NULL) {
		fprintf(stderr, "BigMaac: Cannot find node in BigMaac\n");
		pthread_mutex_unlock(&lock);
		return 0;
	}

	if (new_ptr != NULL) {
		memblock_copy(n->ptr, new_ptr, n->size, new_size, false);
	}

	node* head = ptr < base_bigmaac ? _head_fries : _head_bigmaacs;
	if (head == _head_bigmaacs) {
		const void* remap = mmap(n->ptr, n->size, PROT_NONE, MAP_ANONYMOUS | MAP_FIXED | MAP_PRIVATE, -1, 0);
		if (remap == MAP_FAILED) {
			fprintf(stderr, "BigMaac: wrong with munmap()! %s\n", strerror(errno));
			pthread_mutex_unlock(&lock);
			return 0;
		}
		active_mmaps--;
		used_bigmaacs -= n->size;
	} else {
		used_fries -= n->size;
	}

	verify_memory(head, 0);
	const int r = heap_free_node(head, n);
	verify_memory(head, 1);
	pthread_mutex_unlock(&lock);

	if (r < 0) {
		return r;
	}

	return 1;
}

// BigMaac C library memory functions

void* PREFIX(malloc)(size_t size) {
	if (load_state == NOT_LOADED && real_malloc == NULL) {
		bigmaac_init();
	}

	if (load_state != LOADED || size == 0) {
		return real_malloc(size);
	}

	if (size > min_size_fry) {
		void* p = create_chunk(size);
		if (p == NULL) {
			OOM();
			return NULL;
		}
		return p;
	}

	return real_malloc(size);
}

void* PREFIX(calloc)(size_t count, size_t size) {
	if (load_state > NOT_LOADED && load_state < LOADED) {
		return NULL;
	}

	if (load_state == NOT_LOADED || real_malloc == NULL) {
		bigmaac_init();
	}

	if (load_state != LOADED || count == 0 || size == 0) {
		return real_calloc(count, size);
	}

	// library is loaded and count/size are reasonable
	if (size > min_size_fry) {
		void* p = create_chunk(size);
		if (p == NULL) {
			OOM();
			return NULL;
		}
		if (size <= min_size_bigmaac) {  // its a fry
			memset(p, 0, count * size);
		}
		return p;
	}

	return real_calloc(count, size);
}

void* PREFIX(reallocarray)(void* ptr, size_t size, size_t count) { return PREFIX(realloc)(ptr, size * count); }

void* PREFIX(realloc)(void* ptr, size_t size) {
	if (load_state == NOT_LOADED && real_malloc == NULL) {
		bigmaac_init();
	}

	if (load_state != LOADED) {
		return real_realloc(ptr, size);
	}

	if (ptr == NULL || size == 0) {
		return PREFIX(malloc)(size);
	}

	// currently managed by BigMaac
	if (ptr >= base_fries && ptr < end_bigmaac) {
		// check if already allocated is big enough
		pthread_mutex_lock(&lock);
		node* n = heap_find_node(ptr);
		if (n == NULL) {
			fprintf(stderr, "BigMaac: Cannot find node in BigMaac\n");
			pthread_mutex_unlock(&lock);
			return NULL;
		}
		pthread_mutex_unlock(&lock);

		// allocated memory is big enough
		if (n->size >= size) {
			return ptr;
		}

		// existing chunk is not big enough
		void* p = NULL;
		if (size > min_size_fry) {
			p = create_chunk(size);
			if (p == NULL) {
				OOM();  // set errno
			}
		} else {  // if this isnt a fry or big maac or bigmaac failed
			p = real_malloc(size);
		}

		if (p == NULL) {
			return NULL;
		}

		int r = remove_chunk_with_ptr(ptr, p, size);  // Check if this pointer is>> address space reserved fr mmap
		if (r < 0) {
			OOM();
			return NULL;
		} else if (r == 0) {
			fprintf(stderr, "BigMaac: is missing memory address it should have\n");
			return NULL;
		}
		return p;
	}

	// currently managed by system
	// if (size>24570 && size<24577) { //debug pytest
	if (size > min_size_fry) {
		size_t old_size = MALLOCSIZE(ptr);

		void* p = create_chunk(size);
		if (p != NULL) {
			memblock_copy(ptr, p, old_size, size, true);
			real_free((size_t)ptr);
		} else {
			OOM();
			return NULL;
		}
		return p;
	}

	// size <= min_size_fry
	return real_realloc(ptr, size);
}

void PREFIX(free)(void* ptr) {
	if (load_state == NOT_LOADED && real_malloc == NULL) {
		bigmaac_init();
	}

	// if ptr is managed by system or BigMaac is not loaded yet
	if (load_state != LOADED || ptr < base_fries || ptr >= end_bigmaac) {
		real_free((size_t)ptr);
		return;
	}
	// ptr is managed by BigMaac and library is fully loaded
	int chunks_removed = remove_chunk_with_ptr(ptr, NULL, 0);  // Check if this pointer is>> address space reserved fr mmap
	if (chunks_removed == 0) {
		fprintf(stderr, "BigMaac: Free was called on pointer that was not alloc'd %p\n", ptr);
		return;
	}
}

#ifdef MAIN
#define T 32
#define N (4096 * 16)
#define N_size 1024 * 16
#define X 1024 * 16

#include <omp.h>

int** ptrs;
size_t* sizes;

int main() {
	ptrs = (int**)PREFIX(calloc)(1, sizeof(int*) * T * N);
	sizes = (size_t*)PREFIX(calloc)(1, sizeof(size_t) * T * N);
	for (int i = 0; i < N * T; i++) {
		ptrs[i] = NULL;
		sizes[i] = 0;
	}
	omp_set_num_threads(T);
#pragma omp parallel
	{
		int t = omp_get_thread_num();
		fprintf(stderr, "T%d\n", t);
		srand(123 + t);
		for (int i = 1; i < N; i++) {
			if (i % 25 == 0) {
				fprintf(stderr, "%d: %d\n", t, i);
			}
			int r = rand();
			int x = (r % X) - X / 2;
			size_t sz = N_size < x ? 3 : N_size - x;
			if (i % 2 == 0) {
				ptrs[i + t * N] = (int*)PREFIX(malloc)(sz * sizeof(int));
			} else {
				ptrs[i + t * N] = (int*)PREFIX(calloc)(1, sz * sizeof(int));
			}
			sizes[i + t * N] = sz;
			for (int j = 0; j < sz; j++) {
				ptrs[i + t * N][j] = rand();
			}

			// lets free something
			r = rand();
			x = (r % X) - X / 2;
			int k = r % i + t * N;
			if (ptrs[k] != NULL) {
				if (k % 2 == 0) {
					PREFIX(free)(ptrs[k]);
					ptrs[k] = NULL;
					sizes[k] = 0;
				} else {
					size_t new_size = sizes[k] < x ? 3 : sizes[k] - x;
					ptrs[k] = PREFIX(realloc)(ptrs[k], new_size * sizeof(int));
					for (int j = 0; j < new_size; j++) {
						ptrs[k][j] = rand();
					}
				}
			}
		}
	}
}
#endif
