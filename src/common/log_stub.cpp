#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>
#include <string_view>
#include <array>
#include <algorithm>
#include <cstring>

extern "C" {

static std::mutex g_log_mutex;

static void safe_write(int prio, const char* tag, const char* msg) noexcept {
    static constexpr std::string_view null_tag = "(null-tag)";
    static constexpr std::string_view null_msg = "(null-msg)";
    
    std::string_view tag_sv = tag ? tag : null_tag;
    std::string_view msg_sv = msg ? msg : null_msg;
    
    std::lock_guard lock(g_log_mutex);
    
    const char prefix[] = "[";
    const char suffix[] = "] ";
    const char separator[] = ": ";
    const char newline[] = "\n";
    
    size_t total_len = std::size(prefix) - 1 + 
                       std::to_string(prio).length() +
                       std::size(suffix) - 1 +
                       tag_sv.length() +
                       std::size(separator) - 1 +
                       msg_sv.length() +
                       std::size(newline) - 1;
    
    std::string buffer;
    buffer.reserve(total_len);
    buffer.append(prefix);
    buffer.append(std::to_string(prio));
    buffer.append(suffix);
    buffer.append(tag_sv);
    buffer.append(separator);
    buffer.append(msg_sv);
    buffer.append(newline);
    
    fwrite(buffer.data(), 1, buffer.size(), stdout);
    fflush(stdout);
}

int __android_log_write(int prio, const char* tag, const char* text) noexcept {
    safe_write(prio, tag, text);
    return 0;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) noexcept {
    if (!fmt) {
        safe_write(prio, tag, "(null format)");
        return -1;
    }
    
    va_list args;
    va_start(args, fmt);
    
    std::lock_guard lock(g_log_mutex);
    
    // Write prefix: [prio] tag: 
    fwrite("[", 1, 1, stdout);
    fprintf(stdout, "%d", prio);
    fwrite("] ", 1, 2, stdout);
    if (tag) {
        fwrite(tag, 1, std::strlen(tag), stdout);
    } else {
        fwrite("(null-tag)", 1, 10, stdout);
    }
    fwrite(": ", 1, 2, stdout);
    
    // Write formatted message directly
    vfprintf(stdout, fmt, args);
    
    // Write newline and flush
    fwrite("\n", 1, 1, stdout);
    fflush(stdout);
    
    va_end(args);
    
    // Return 0 to indicate success (no length counted)
    return 0;
}

} // extern "C"
