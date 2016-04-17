/*
 * Copyright (c) 2016 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "logging.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include "timer_hal.h"
#include "service_debug.h"

namespace {

volatile log_message_callback_type log_msg_callback = 0;
volatile log_write_callback_type log_write_callback = 0;
volatile log_enabled_callback_type log_enabled_callback = 0;

} // namespace

void log_set_callbacks(log_message_callback_type log_msg, log_write_callback_type log_write,
        log_enabled_callback_type log_enabled, void *reserved) {
    log_msg_callback = log_msg;
    log_write_callback = log_write;
    log_enabled_callback = log_enabled;
}

void log_message(int level, const char *category, const char *file, int line, const char *func, void *reserved,
        const char *fmt, ...) {
    if (!log_write_callback && (!log_compat_callback || level < log_compat_level)) {
        return;
    }
    const uint32_t time = HAL_Timer_Get_Milli_Seconds();
    if (file) {
        // Strip directory path
        const char *p = strrchr(file, '/');
        if (p) {
            file = p + 1;
        }
    }
    char buf[LOG_MAX_STRING_LENGTH];
    if (log_msg_callback) {
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n > (int)sizeof(buf) - 1) {
            buf[sizeof(buf) - 2] = '~';
        }
        log_msg_callback(buf, level, category, time, file, line, func, 0);
    } else {
        // Using compatibility callback
        const char* const levelName = log_level_name(level, 0);
        int n = 0;
        if (file && func) {
            n = snprintf(buf, sizeof(buf), "%010u %s:%d, %s: %s", (unsigned)time, file, line, func, levelName);
        } else {
            n = snprintf(buf, sizeof(buf), "%010u %s", (unsigned)time, levelName);
        }
        if (n > (int)sizeof(buf) - 1) {
            buf[sizeof(buf) - 2] = '~';
        }
        log_compat_callback(buf);
        log_compat_callback(": ");
        va_list args;
        va_start(args, fmt);
        n = vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n > (int)sizeof(buf) - 1) {
            buf[sizeof(buf) - 2] = '~';
        }
        log_compat_callback(buf);
        log_compat_callback("\r\n");
    }
}

void log_write(int level, const char *category, const char *data, size_t size, void *reserved) {
    if (!size) {
        return;
    }
    if (log_write_callback) {
        log_write_callback(data, size, level, category, 0);
    } else if (log_compat_callback && level >= log_compat_level) {
        // Compatibility callback expects null-terminated strings
        if (!data[size - 1]) {
            log_compat_callback(data);
        } else {
            char buf[LOG_MAX_STRING_LENGTH];
            size_t offs = 0;
            do {
                const size_t n = std::min(size - offs, sizeof(buf) - 1);
                memcpy(buf, data + offs, n);
                buf[n] = 0;
                log_compat_callback(buf);
                offs += n;
            } while (offs < size);
        }
    }
}

void log_format(int level, const char *category, void *reserved, const char *fmt, ...) {
    if (!log_write_callback && (!log_compat_callback || level < log_compat_level)) {
        return;
    }
    char buf[LOG_MAX_STRING_LENGTH];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > (int)sizeof(buf) - 1) {
        buf[sizeof(buf) - 2] = '~';
        n = sizeof(buf) - 1;
    }
    if (log_write_callback) {
        log_write_callback(buf, n, level, category, 0);
    } else {
        log_compat_callback(buf); // Compatibility callback
    }
}

void log_dump(int level, const char *category, const void *data, size_t size, int flags, void *reserved) {
    if (!size || (!log_write_callback && (!log_compat_callback || level < log_compat_level))) {
        return;
    }
    static const char hex[] = "0123456789abcdef";
    char buf[LOG_MAX_STRING_LENGTH / 2 * 2 + 1]; // Hex data is flushed in chunks
    buf[sizeof(buf) - 1] = 0; // Compatibility callback expects null-terminated strings
    size_t offs = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = ((const uint8_t*)data)[i];
        buf[offs++] = hex[b >> 4];
        buf[offs++] = hex[b & 0x0f];
        if (offs == sizeof(buf) - 1) {
            if (log_write_callback) {
                log_write_callback(buf, sizeof(buf) - 1, level, category, 0);
            } else {
                log_compat_callback(buf);
            }
            offs = 0;
        }
    }
    if (offs) {
        if (log_write_callback) {
            log_write_callback(buf, offs, level, category, 0);
        } else {
            buf[offs] = 0;
            log_compat_callback(buf);
        }
    }
}

int log_enabled(int level, const char *category, void *reserved) {
    if (log_enabled_callback) {
        return log_enabled_callback(level, category, 0);
    }
    if (log_compat_callback && level >= log_compat_level) { // Compatibility callback
        return 1;
    }
    return 0;
}

const char* log_level_name(int level, void *reserved) {
    static const char* const names[] = {
        "TRACE",
        "LOG",
        "DEBUG",
        "INFO",
        "WARN",
        "ERROR",
        "PANIC"
    };
    const int i = std::max(0, std::min<int>(level / 10, sizeof(names) / sizeof(names[0]) - 1));
    return names[i];
}
