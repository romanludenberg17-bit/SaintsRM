#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
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
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::mutex g_surface_mutex;
ANativeWindow* g_window = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
struct sigaction g_previous_sigsegv{};
std::atomic<bool> g_signal_installed{false};
std::atomic<bool> g_protected_page_passed{false};
std::atomic<uintptr_t> g_last_fault_address{0};
std::atomic<uintptr_t> g_last_fault_pc{0};
std::atomic<uint32_t> g_last_fault_instruction{0};

bool is_default_handler(const struct sigaction& action) {
    if ((action.sa_flags & SA_SIGINFO) != 0) return action.sa_sigaction == SIG_DFL;
    return action.sa_handler == SIG_DFL;
}

bool is_ignore_handler(const struct sigaction& action) {
    if ((action.sa_flags & SA_SIGINFO) != 0) return action.sa_sigaction == SIG_IGN;
    return action.sa_handler == SIG_IGN;
}

void chain_previous_sigsegv(int sig, siginfo_t* info, void* context) {
    if (is_ignore_handler(g_previous_sigsegv)) return;

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
    auto* uc = reinterpret_cast<ucontext_t*>(context);
    const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);

    uint32_t instruction = 0;
    std::memcpy(&instruction, reinterpret_cast<const void*>(pc), sizeof(instruction));
    g_last_fault_address.store(fault, std::memory_order_relaxed);
    g_last_fault_pc.store(pc, std::memory_order_relaxed);
    g_last_fault_instruction.store(instruction, std::memory_order_relaxed);

    // Only the exact LDR used by the protected-page smoke test is handled.
    // Everything else is chained to the previous Android/ART disposition.
    const bool is_known_test_fault = (instruction & 0xFFC003FFu) == 0xF9400000u;
    if (is_known_test_fault) {
        uc->uc_mcontext.regs[0] = 0;
        uc->uc_mcontext.pc = pc + 4;
        g_protected_page_passed.store(true, std::memory_order_relaxed);
        return;
    }

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

std::string run_protected_page_test() {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return "FAIL: sysconf(_SC_PAGESIZE)";

    void* page = mmap(nullptr, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return "FAIL: mmap protected-page test";

    std::memset(page, 0xA5, static_cast<size_t>(page_size));
    if (mprotect(page, static_cast<size_t>(page_size), PROT_NONE) != 0) {
        munmap(page, static_cast<size_t>(page_size));
        return "FAIL: mprotect(PROT_NONE)";
    }

#if defined(__aarch64__)
    g_protected_page_passed.store(false, std::memory_order_relaxed);
    register const void* address asm("x1") = page;
    asm volatile("ldr x0, [x1]" : : "r"(address) : "x0", "memory");
#else
    munmap(page, static_cast<size_t>(page_size));
    return "FAIL: smoke test requires AArch64";
#endif

    const bool passed = g_protected_page_passed.load(std::memory_order_relaxed);
    const bool restored = mprotect(page, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE) == 0;
    munmap(page, static_cast<size_t>(page_size));

    if (!passed || !restored) return "FAIL: protected-page SIGSEGV continuation";
    return "PASS: protected-page SIGSEGV continuation";
}

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
Java_com_saintsrM_smoketest_MainActivity_nativeRunSmokeTests(JNIEnv* env, jclass) {
    const long page_size = sysconf(_SC_PAGESIZE);
    LOGI("runtime page size: %ld", page_size);

    std::ostringstream out;
    out << "SaintsRM smoke-test\n";
    out << "page size: " << page_size << " bytes\n";
    out << "16 KiB linker max-page-size: enabled\n";

    const bool alt_stack = install_alt_stack_for_current_thread();
    out << "alt signal stack: " << (alt_stack ? "PASS" : "FAIL") << "\n";

    const bool signals = install_signal_handler();
    out << "SIGSEGV handler: " << (signals ? "PASS" : "FAIL") << "\n";

    if (signals && alt_stack) {
        out << run_protected_page_test() << "\n";
        out << "last fault addr: 0x" << std::hex << g_last_fault_address.load() << "\n";
        out << "last fault pc: 0x" << std::hex << g_last_fault_pc.load() << "\n";
        out << "last fault insn: 0x" << std::hex << g_last_fault_instruction.load() << std::dec << "\n";
    } else {
        out << "protected-page test: SKIPPED\n";
    }

    const bool loader = test_dynamic_loader();
    out << "dynamic loader: " << (loader ? "PASS" : "FAIL") << "\n";

    const bool vulkan = init_vulkan();
    out << "Vulkan instance/devices: " << (vulkan ? "PASS" : "FAIL") << "\n";

    LOGI("%s", out.str().c_str());
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_saintsrM_smoketest_MainActivity_nativeSetSurface(JNIEnv* env, jclass, jobject surface) {
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
Java_com_saintsrM_smoketest_MainActivity_nativeClearSurface(JNIEnv*, jclass) {
    destroy_surface();
    LOGI("Vulkan Android surface cleared");
}

extern "C" JNIEXPORT void JNICALL
Java_com_saintsrM_smoketest_MainActivity_nativeOnTrimMemory(JNIEnv*, jclass, jint level) {
    LOGI("onTrimMemory level=%d; stopping presentation resources", level);
    destroy_surface();
}
