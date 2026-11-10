// Android: JNI bridge between the Java touch overlay / activity and the game.
#include <jni.h>

#include <android/log.h>

#include <atomic>
#include <algorithm>
#include <cstdint>

#include "core/input.h"
#include "noble_android.h"

// Defined by the game hooks (src/common/menu.cpp, src/common/keyboard.cpp).
void MenuToggle();
bool MenuOpen();
void KeyboardPad(int port, input::Pad& pad);

namespace noble_android {

struct TouchState {
    std::atomic<int> buttons{0};
    std::atomic<int> a{0}, b{0}, x{0}, y{0};
    std::atomic<int> black{0}, white{0}, lt{0}, rt{0};
    std::atomic<int> lx{0}, ly{0}, rx{0}, ry{0};
    std::atomic<bool> active{false};
};

TouchState g_touch;
std::atomic<bool> g_booted{false};

static inline int MergeStick(int physical, int touch) {
    return std::abs(touch) >= std::abs(physical) ? touch : physical;
}

void SetBooted() { g_booted.store(true, std::memory_order_release); }
bool IsBooted() { return g_booted.load(std::memory_order_acquire); }

void ToggleMenu() { MenuToggle(); }
bool MenuOpen() { return ::MenuOpen(); }

void MergeTouchPad(int port, input::Pad& pad) {
    if (port != 0 || !g_touch.active.load(std::memory_order_relaxed)) return;

    pad.connected = true;
    pad.buttons |= uint16_t(g_touch.buttons.load(std::memory_order_relaxed));

    const int analog[8] = {
        g_touch.a.load(std::memory_order_relaxed), g_touch.b.load(std::memory_order_relaxed),
        g_touch.x.load(std::memory_order_relaxed), g_touch.y.load(std::memory_order_relaxed),
        g_touch.black.load(std::memory_order_relaxed), g_touch.white.load(std::memory_order_relaxed),
        g_touch.lt.load(std::memory_order_relaxed), g_touch.rt.load(std::memory_order_relaxed)};
    for (int i = 0; i < 8; i++)
        pad.analog[i] = uint8_t(std::max<int>(pad.analog[i], analog[i]));

    pad.lx = int16_t(MergeStick(pad.lx, g_touch.lx.load(std::memory_order_relaxed)));
    pad.ly = int16_t(MergeStick(pad.ly, g_touch.ly.load(std::memory_order_relaxed)));
    pad.rx = int16_t(MergeStick(pad.rx, g_touch.rx.load(std::memory_order_relaxed)));
    pad.ry = int16_t(MergeStick(pad.ry, g_touch.ry.load(std::memory_order_relaxed)));
}

}  // namespace noble_android

// The pad hook the game calls: keyboard/controller first, touch last.
void NoblePad(int port, input::Pad& pad) {
    KeyboardPad(port, pad);
    noble_android::MergeTouchPad(port, pad);
}

extern "C" JNIEXPORT void JNICALL
Java_com_noblearchimedes_NobleActivity_nativeSetTouchPad(
        JNIEnv*, jclass,
        jint buttons,
        jint a, jint b, jint x, jint y,
        jint black, jint white,
        jint lt, jint rt,
        jint lx, jint ly, jint rx, jint ry,
        jboolean active) {
    auto& t = noble_android::g_touch;
    t.buttons.store(buttons, std::memory_order_relaxed);
    t.a.store(a, std::memory_order_relaxed);
    t.b.store(b, std::memory_order_relaxed);
    t.x.store(x, std::memory_order_relaxed);
    t.y.store(y, std::memory_order_relaxed);
    t.black.store(black, std::memory_order_relaxed);
    t.white.store(white, std::memory_order_relaxed);
    t.lt.store(lt, std::memory_order_relaxed);
    t.rt.store(rt, std::memory_order_relaxed);
    t.lx.store(lx, std::memory_order_relaxed);
    t.ly.store(ly, std::memory_order_relaxed);
    t.rx.store(rx, std::memory_order_relaxed);
    t.ry.store(ry, std::memory_order_relaxed);
    t.active.store(active == JNI_TRUE, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_noblearchimedes_NobleActivity_nativeMenuToggle(JNIEnv*, jclass) {
    noble_android::ToggleMenu();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_noblearchimedes_NobleActivity_nativeMenuOpen(JNIEnv*, jclass) {
    return noble_android::MenuOpen() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_noblearchimedes_NobleActivity_nativeIsReady(JNIEnv*, jclass) {
    return noble_android::IsBooted() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_noblearchimedes_NobleActivity_nativeTouchVisibilityChanged(JNIEnv*, jclass, jboolean) {
    // The Java view persists this; native code only needs the live pad state.
}
