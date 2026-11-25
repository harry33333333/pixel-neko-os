#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Freestanding C++ runtime shims for OpenH264 (compiled with -fno-exceptions -fno-rtti) */

void* operator new(size_t size) {
    return malloc(size ? size : 1);
}

void* operator new[](size_t size) {
    return malloc(size ? size : 1);
}

void operator delete(void* ptr) noexcept {
    free(ptr);
}

void operator delete[](void* ptr) noexcept {
    free(ptr);
}

void operator delete(void* ptr, size_t size) noexcept {
    (void)size;
    free(ptr);
}

void operator delete[](void* ptr, size_t size) noexcept {
    (void)size;
    free(ptr);
}

extern "C" {

void* __dso_handle = (void*)0;

int __cxa_atexit(void (*func)(void*), void* arg, void* dso_handle) {
    (void)func; (void)arg; (void)dso_handle;
    return 0;
}

void __cxa_pure_virtual(void) {
    abort();
}

int __cxa_guard_acquire(uint64_t* guard) {
    uint8_t* flag = (uint8_t*)guard;
    if (__atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0) {
        return 0;
    }
    return 1;
}

void __cxa_guard_release(uint64_t* guard) {
    uint8_t* flag = (uint8_t*)guard;
    __atomic_store_n(flag, 1, __ATOMIC_RELEASE);
}

void __cxa_guard_abort(uint64_t* guard) {
    (void)guard;
}

} /* extern "C" */

