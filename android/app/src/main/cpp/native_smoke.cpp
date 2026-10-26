#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#define LOG_TAG "SaintsRM-Smoke"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::mutex g_surface_mutex;
ANativeWindow* g_window = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
struct sigaction g_previous_sigsegv{};
std::atomic<bool> g_signal_installed{false};

std::atomic_flag g_fault_handler_busy = ATOMIC_FLAG_INIT;
std::atomic<uintptr_t> g_last_fault_address{0};
std::atomic<uintptr_t> g_last_fault_pc{0};
std::atomic<uint32_t> g_last_fault_instruction{0};
std::atomic<uint32_t> g_fault_reentry_count{0};
std::atomic<uint32_t> g_fault_contention_count{0};

enum class FaultMode : uint32_t { kNone = 0, kDumpAndSkip = 1, kDemandCommit = 2 };
std::atomic<uint32_t> g_fault_mode{0};
std::atomic<uintptr_t> g_test_page{0};
std::atomic<size_t> g_test_page_size{0};
std::atomic<uint32_t> g_test_expected_instruction{0xF9400020u};
std::atomic<bool> g_test_fault_seen{false};
std::atomic<bool> g_test_mprotect_succeeded{false};

using SigInfoHandler = void (*)(int, siginfo_t*, void*);

bool is_default_handler(const struct sigaction& action) {
    if ((action.sa_flags & SA_SIGINFO) != 0) {
        return action.sa_sigaction == reinterpret_cast<SigInfoHandler>(SIG_DFL);
    }
    return action.sa_handler == SIG_DFL;
}

bool is_ignore_handler(const struct sigaction& action) {
    if ((action.sa_flags & SA_SIGINFO) != 0) {
        return action.sa_sigaction == reinterpret_cast<SigInfoHandler>(SIG_IGN);
    }
    return action.sa_handler == SIG_IGN;
}

void chain_previous_sigsegv(int sig, siginfo_t* info, void* context) {
    if (is_ignore_handler(g_previous_sigsegv)) {
        sigaction(SIGSEGV, &g_previous_sigsegv, nullptr);
        return;
    }

    if (is_default_handler(g_previous_sigsegv)) {
        sigaction(SIGSEGV, &g_previous_sigsegv, nullptr);
        return;
    }

    if ((g_previous_sigsegv.sa_flags & SA_SIGINFO) != 0 && g_previous_sigsegv.sa_sigaction) {
        g_previous_sigsegv.sa_sigaction(sig, info, context);
        return;
    }

    if (g_previous_sigsegv.sa_handler) {
        g_previous_sigsegv.sa_handler(sig);
        return;
    }

    sigaction(SIGSEGV, &g_previous_sigsegv, nullptr);
}

#if defined(__aarch64__)
void saintsrm_sigsegv_handler(int sig, siginfo_t* info, void* context) {
    if (g_fault_handler_busy.test_and_set(std::memory_order_acquire)) {
        g_fault_reentry_count.fetch_add(1, std::memory_order_relaxed);
        chain_previous_sigsegv(sig, info, context);
        return;
    }

    auto* uc = reinterpret_cast<ucontext_t*>(context);
    const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);

    g_last_fault_address.store(fault, std::memory_order_relaxed);
    g_last_fault_pc.store(pc, std::memory_order_relaxed);
    const uint32_t expected = g_test_expected_instruction.load(std::memory_order_relaxed);
    g_last_fault_instruction.store(expected, std::memory_order_relaxed);

    const uintptr_t page = g_test_page.load(std::memory_order_relaxed);
    const size_t page_size = g_test_page_size.load(std::memory_order_relaxed);
    const uint32_t mode = g_fault_mode.load(std::memory_order_relaxed);
    const bool is_test_page = page != 0 && page_size != 0 &&
        fault >= page && fault < page + page_size;

    if (is_test_page && mode == static_cast<uint32_t>(FaultMode::kDemandCommit)) {
        g_test_fault_seen.store(true, std::memory_order_relaxed);
        const long result = syscall(SYS_mprotect,
                                    reinterpret_cast<void*>(page),
                                    page_size,
                                    PROT_READ | PROT_WRITE);
        g_test_mprotect_succeeded.store(result == 0, std::memory_order_relaxed);
        if (result == 0) {
            g_fault_handler_busy.clear(std::memory_order_release);
            return;
        }
    }

    if (is_test_page && mode == static_cast<uint32_t>(FaultMode::kDumpAndSkip)) {
        g_test_fault_seen.store(true, std::memory_order_relaxed);
        uc->uc_mcontext.regs[0] = 0;
        uc->uc_mcontext.pc = pc + 4;
        g_fault_handler_busy.clear(std::memory_order_release);
        return;
    }

    g_fault_contention_count.fetch_add(1, std::memory_order_relaxed);
    g_fault_handler_busy.clear(std::memory_order_release);
    chain_previous_sigsegv(sig, info, context);
}
#else
void saintsrm_sigsegv_handler(int sig, siginfo_t* info, void* context) {
    chain_previous_sigsegv(sig, info, context);
}
#endif

bool install_signal_handler() {
    if (g_signal_installed.load(std::memory_order_acquire)) return true;

    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = saintsrm_sigsegv_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;

    if (sigaction(SIGSEGV, &action, &g_previous_sigsegv) != 0) {
        LOGE("sigaction(SIGSEGV) failed: %s", std::strerror(errno));
        return false;
    }

    g_signal_installed.store(true, std::memory_order_release);
    return true;
}

bool install_alt_stack_for_current_thread() {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;

    const size_t stack_size = static_cast<size_t>(std::max<long>(page_size * 4, SIGSTKSZ));
    void* memory = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        LOGE("sigaltstack mmap failed: %s", std::strerror(errno));
        return false;
    }

    stack_t stack{};
    stack.ss_sp = memory;
    stack.ss_size = stack_size;
    stack.ss_flags = 0;
    if (sigaltstack(&stack, nullptr) != 0) {
        LOGE("sigaltstack failed: %s", std::strerror(errno));
        munmap(memory, stack_size);
        return false;
    }
    return true;
}

struct TestResult {
    bool fault_seen;
    bool mprotect_succeeded;
    uint64_t read_value;
    uintptr_t fault_pc;
};

#if defined(__aarch64__)
TestResult run_protected_page_test(FaultMode mode, uint64_t initial_value) {
    TestResult result{false, false, 0, 0};
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return result;

    void* page = mmap(nullptr, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return result;

    *reinterpret_cast<volatile uint64_t*>(page) = initial_value;
    if (mprotect(page, static_cast<size_t>(page_size), PROT_NONE) != 0) {
        munmap(page, static_cast<size_t>(page_size));
        return result;
    }

    g_test_page.store(reinterpret_cast<uintptr_t>(page), std::memory_order_relaxed);
    g_test_page_size.store(static_cast<size_t>(page_size), std::memory_order_relaxed);
    g_fault_mode.store(static_cast<uint32_t>(mode), std::memory_order_release);
    g_test_fault_seen.store(false, std::memory_order_relaxed);
    g_test_mprotect_succeeded.store(false, std::memory_order_relaxed);

    register const void* address asm("x1") = page;
    register uint64_t value asm("x0");
    asm volatile("ldr x0, [x1]" : "=r"(value) : "r"(address) : "memory");
    result.read_value = value;

    result.fault_seen = g_test_fault_seen.load(std::memory_order_relaxed);
    result.mprotect_succeeded = g_test_mprotect_succeeded.load(std::memory_order_relaxed);
    result.fault_pc = g_last_fault_pc.load(std::memory_order_relaxed);

    g_fault_mode.store(0, std::memory_order_release);
    g_test_page.store(0, std::memory_order_relaxed);
    g_test_page_size.store(0, std::memory_order_relaxed);
    g_fault_handler_busy.clear(std::memory_order_release);

    mprotect(page, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE);
    munmap(page, static_cast<size_t>(page_size));
    return result;
}
#else
TestResult run_protected_page_test(FaultMode, uint64_t) { return {false, false, 0, 0}; }
#endif

bool test_dynamic_loader() {
    void* handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        LOGE("dlopen(libvulkan.so) failed: %s", dlerror());
        return false;
    }
    dlclose(handle);
    return true;
}

bool init_vulkan() {
    if (g_instance != VK_NULL_HANDLE) return true;

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "SaintsRM Smoke Test";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "SaintsRM";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = extensions;

    if (vkCreateInstance(&create_info, nullptr, &g_instance) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed");
        return false;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_instance, &count, nullptr);
    LOGI("Vulkan physical devices: %u", count);
    return count > 0;
}

void destroy_surface() {
    std::lock_guard lock(g_surface_mutex);
    if (g_surface != VK_NULL_HANDLE && g_instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
        g_surface = VK_NULL_HANDLE;
    }
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_saintsm_smoketest_MainActivity_nativeRunSmokeTests(JNIEnv* env, jclass) {
    const long page_size = sysconf(_SC_PAGESIZE);
    LOGI("runtime page size: %ld", page_size);

    std::ostringstream out;
    out << "SaintsRM smoke-test\n";
    out << "page size: " << page_size << " bytes\n";
    out << "ELF 16 KiB linker flags: enabled\n";
    out << "ABI: arm64-v8a\n";

    const bool alt_stack = install_alt_stack_for_current_thread();
    out << "alt signal stack (current thread): " << (alt_stack ? "PASS" : "FAIL") << "\n";

    const bool signals = install_signal_handler();
    out << "SIGSEGV handler: " << (signals ? "PASS" : "FAIL") << "\n";

    if (signals && alt_stack) {
        const TestResult dump = run_protected_page_test(FaultMode::kDumpAndSkip, 0x1122334455667788ULL);
        out << "protected-page dump/skip: "
            << (dump.fault_seen && dump.read_value == 0 ? "PASS" : "FAIL") << "\n";
        out << "  read value: 0x" << std::hex << dump.read_value << std::dec << "\n";
        out << "  fault pc: 0x" << std::hex << dump.fault_pc << std::dec << "\n";

        const TestResult demand = run_protected_page_test(FaultMode::kDemandCommit, 0x1122334455667788ULL);
        out << "protected-page demand-commit/retry: "
            << (demand.fault_seen && demand.mprotect_succeeded &&
                demand.read_value == 0x1122334455667788ULL ? "PASS" : "FAIL") << "\n";
        out << "  read value: 0x" << std::hex << demand.read_value << std::dec << "\n";
        out << "  fault pc: 0x" << std::hex << demand.fault_pc << std::dec << "\n";
        out << "fault-handler reentry: " << g_fault_reentry_count.load() << "\n";
        out << "fault-handler contention: " << g_fault_contention_count.load() << "\n";
    } else {
        out << "protected-page tests: SKIPPED\n";
    }

    const bool loader = test_dynamic_loader();
    out << "dynamic loader (libvulkan.so): " << (loader ? "PASS" : "FAIL") << "\n";

    const bool vulkan = init_vulkan();
    out << "Vulkan instance/devices: " << (vulkan ? "PASS" : "FAIL") << "\n";

    LOGI("%s", out.str().c_str());
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_saintsm_smoketest_MainActivity_nativeSetSurface(JNIEnv* env, jclass, jobject surface) {
    std::lock_guard lock(g_surface_mutex);
    if (g_instance == VK_NULL_HANDLE || surface == nullptr) return;

    ANativeWindow* new_window = ANativeWindow_fromSurface(env, surface);
    if (!new_window) return;

    if (g_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
        g_surface = VK_NULL_HANDLE;
    }
    if (g_window) ANativeWindow_release(g_window);
    g_window = new_window;

    VkAndroidSurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surface_info.window = g_window;
    if (vkCreateAndroidSurfaceKHR(g_instance, &surface_info, nullptr, &g_surface) != VK_SUCCESS) {
        LOGE("vkCreateAndroidSurfaceKHR failed");
        ANativeWindow_release(g_window);
        g_window = nullptr;
        return;
    }
    LOGI("Vulkan Android surface created/recreated");
}

extern "C" JNIEXPORT void JNICALL
Java_com_saintsm_smoketest_MainActivity_nativeClearSurface(JNIEnv*, jclass) {
    destroy_surface();
    LOGI("Vulkan Android surface cleared");
}

extern "C" JNIEXPORT void JNICALL
Java_com_saintsm_smoketest_MainActivity_nativeOnTrimMemory(JNIEnv*, jclass, jint level) {
    LOGI("onTrimMemory level=%d; stopping presentation resources", level);
    destroy_surface();
}
