//========================================================================
// GLFW 3.3 Linux - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2002-2006 Marcus Geelnard
// Copyright (c) 2006-2016 Camilla Berglund <elmindreda@glfw.org>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================

#include "internal.h"

#if defined(__linux__)
#include <linux/joystick.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif // __linux__


// Attempt to open the specified joystick device
//
#if defined(__linux__)
static GLFWbool openJoystickDevice(const char* path)
{
    char axisCount, buttonCount;
    char name[256] = "";
    int jid, fd, version;
    _GLFWjoystickLinux* js;

    for (jid = GLFW_JOYSTICK_1;  jid <= GLFW_JOYSTICK_LAST;  jid++)
    {
        if (!_glfw.linux_js.js[jid].present)
            continue;

        if (_glfw_strcmp(_glfw.linux_js.js[jid].path, path) == 0)
            return GLFW_FALSE;
    }

    for (jid = GLFW_JOYSTICK_1;  jid <= GLFW_JOYSTICK_LAST;  jid++)
    {
        if (!_glfw.linux_js.js[jid].present)
            break;
    }

    if (jid > GLFW_JOYSTICK_LAST)
        return GLFW_FALSE;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd == -1)
        return GLFW_FALSE;

    // Verify that the joystick driver version is at least 1.0
    ioctl(fd, JSIOCGVERSION, &version);
    if (version < 0x010000)
    {
        // It's an old 0.x interface (we don't support it)
        close(fd);
        return GLFW_FALSE;
    }

    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) < 0)
        strncpy(name, "Unknown", sizeof(name));

    js = _glfw.linux_js.js + jid;
    js->present = GLFW_TRUE;
    js->name = _glfw_strdup(name);
    js->path = _glfw_strdup(path);
    js->fd = fd;

    ioctl(fd, JSIOCGAXES, &axisCount);
    js->axisCount = (int) axisCount;
    js->axes = _glfw_calloc(axisCount, sizeof(float));

    ioctl(fd, JSIOCGBUTTONS, &buttonCount);
    js->buttonCount = (int) buttonCount;
    js->buttons = _glfw_calloc(buttonCount, 1);

    _glfwInputJoystickChange(jid, GLFW_CONNECTED);
    return GLFW_TRUE;
}
#endif // __linux__

// Polls for and processes events the specified joystick
//
static GLFWbool pollJoystickEvents(_GLFWjoystickLinux* js)
{
#if defined(__linux__)
    _glfwPollJoystickEvents();

    if (!js->present)
        return GLFW_FALSE;

    // Read all queued events (non-blocking)
    for (;;)
    {
        struct js_event e;

        errno = 0;
        if (read(js->fd, &e, sizeof(e)) < 0)
        {
            // Reset the joystick slot if the device was disconnected
            if (errno == ENODEV)
            {
                _glfw_free(js->axes);
                _glfw_free(js->buttons);
                _glfw_free(js->name);
                _glfw_free(js->path);

                _glfw_memset(js, 0, sizeof(_GLFWjoystickLinux));

                _glfwInputJoystickChange(js - _glfw.linux_js.js,
                                         GLFW_DISCONNECTED);
            }

            break;
        }

        // Clear the initial-state bit
        e.type &= ~JS_EVENT_INIT;

        if (e.type == JS_EVENT_AXIS)
            js->axes[e.number] = (float) e.value / 32767.0f;
        else if (e.type == JS_EVENT_BUTTON)
            js->buttons[e.number] = e.value ? GLFW_PRESS : GLFW_RELEASE;
    }
#endif // __linux__
    return js->present;
}

// Lexically compare joysticks by name; used by qsort
//
#if defined(__linux__)
static int compareJoysticks(const void* fp, const void* sp)
{
    const _GLFWjoystickLinux* fj = fp;
    const _GLFWjoystickLinux* sj = sp;
    return _glfw_strcmp(fj->path, sj->path);
}
#endif // __linux__


//////////////////////////////////////////////////////////////////////////
//////                       GLFW internal API                      //////
//////////////////////////////////////////////////////////////////////////

// Initialize joystick interface
//
GLFWbool _glfwInitJoysticksLinux(void)
{
#if defined(__linux__)
    DIR* dir;
    int count = 0;
    const char* dirname = "/dev/input";

    _glfw.linux_js.inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (_glfw.linux_js.inotify == -1)
    {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Linux: Failed to initialize inotify: %s",
                        strerror(errno));
        return GLFW_FALSE;
    }

    // HACK: Register for IN_ATTRIB as well to get notified when udev is done
    //       This works well in practice but the true way is libudev

    _glfw.linux_js.watch = inotify_add_watch(_glfw.linux_js.inotify,
                                             dirname,
                                             IN_CREATE | IN_ATTRIB);
    if (_glfw.linux_js.watch == -1)
    {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Linux: Failed to watch for joystick connections in %s: %s",
                        dirname,
                        strerror(errno));
        // Continue without device connection notifications
    }

    if (regcomp(&_glfw.linux_js.regex, "^js[0-9]\\+$", 0) != 0)
    {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Linux: Failed to compile regex");
        return GLFW_FALSE;
    }

    dir = opendir(dirname);
    if (dir)
    {
        struct dirent* entry;

        while ((entry = readdir(dir)))
        {
            char path[20];
            regmatch_t match;

            if (regexec(&_glfw.linux_js.regex, entry->d_name, 1, &match, 0) != 0)
                continue;

            snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);
            if (openJoystickDevice(path))
                count++;
        }

        closedir(dir);
    }
    else
    {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "Linux: Failed to open joystick device directory %s: %s",
                        dirname,
                        strerror(errno));
        // Continue with no joysticks detected
    }

    _glfw_qsort(_glfw.linux_js.js, count,
                sizeof(_GLFWjoystickLinux),
                compareJoysticks);
#endif // __linux__

    return GLFW_TRUE;
}

// Close all opened joystick handles
//
void _glfwTerminateJoysticksLinux(void)
{
#if defined(__linux__)
    int i;

    for (i = 0;  i <= GLFW_JOYSTICK_LAST;  i++)
    {
        if (_glfw.linux_js.js[i].present)
        {
            close(_glfw.linux_js.js[i].fd);
            _glfw_free(_glfw.linux_js.js[i].axes);
            _glfw_free(_glfw.linux_js.js[i].buttons);
            _glfw_free(_glfw.linux_js.js[i].name);
            _glfw_free(_glfw.linux_js.js[i].path);
        }
    }

    regfree(&_glfw.linux_js.regex);

    if (_glfw.linux_js.inotify > 0)
    {
        if (_glfw.linux_js.watch > 0)
            inotify_rm_watch(_glfw.linux_js.inotify, _glfw.linux_js.watch);

        close(_glfw.linux_js.inotify);
    }
#endif // __linux__
}

void _glfwPollJoystickEvents(void)
{
#if defined(__linux__)
    ssize_t offset = 0;
    char buffer[16384];

    const ssize_t size = read(_glfw.linux_js.inotify, buffer, sizeof(buffer));

    while (size > offset)
    {
        regmatch_t match;
        const struct inotify_event* e = (struct inotify_event*) (buffer + offset);

        if (regexec(&_glfw.linux_js.regex, e->name, 1, &match, 0) == 0)
        {
            char path[20];
            snprintf(path, sizeof(path), "/dev/input/%s", e->name);
            openJoystickDevice(path);
        }

        offset += sizeof(struct inotify_event) + e->len;
    }
#endif
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

int _glfwPlatformJoystickPresent(int jid)
{
    _GLFWjoystickLinux* js = _glfw.linux_js.js + jid;
    return pollJoystickEvents(js);
}

const float* _glfwPlatformGetJoystickAxes(int jid, int* count)
{
    _GLFWjoystickLinux* js = _glfw.linux_js.js + jid;
    if (!pollJoystickEvents(js))
        return NULL;

    *count = js->axisCount;
    return js->axes;
}

const unsigned char* _glfwPlatformGetJoystickButtons(int jid, int* count)
{
    _GLFWjoystickLinux* js = _glfw.linux_js.js + jid;
    if (!pollJoystickEvents(js))
        return NULL;

    *count = js->buttonCount;
    return js->buttons;
}

const char* _glfwPlatformGetJoystickName(int jid)
{
    _GLFWjoystickLinux* js = _glfw.linux_js.js + jid;
    if (!pollJoystickEvents(js))
        return NULL;

    return js->name;
}

