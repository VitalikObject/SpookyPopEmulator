#if defined(__ANDROID__)

#include "emulator.h"
#include "graphics/host_gl_backend.h"

#include <android/asset_manager.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char* kLogTag = "atrasis";
constexpr const char* kLogPrefix = "[atrasis] ";
std::atomic<android_app*> g_keyboard_app{nullptr};

struct AndroidAppState {
    android_app* app = nullptr;
    std::atomic_bool emulator_started = false;
    std::thread emulator_thread;
    std::unordered_set<int32_t> active_touch_ids;
};

void LogInfo(const std::string& text) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s%s", kLogPrefix, text.c_str());
}

void LogError(const std::string& text) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s%s", kLogPrefix, text.c_str());
}

bool IsTrackedSoundAssetPath(const std::string_view path) {
    return path.find("Button_click_13.caf") != std::string_view::npos
        || path.find("Button_click_13.wav") != std::string_view::npos;
}

void SetAndroidSoftKeyboardVisible(const bool visible) {
    android_app* app = g_keyboard_app.load();
    if (app == nullptr || app->activity == nullptr) {
        return;
    }

    JNIEnv* env = nullptr;
    bool did_attach = false;
    if (app->activity->vm != nullptr) {
        const jint env_status = app->activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (env_status == JNI_EDETACHED) {
            did_attach = app->activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
        }
    }

    if (env != nullptr && app->activity->clazz != nullptr) {
        jclass activity_class = env->GetObjectClass(app->activity->clazz);
        const char* method_name = visible ? "showSoftKeyboard" : "hideSoftKeyboard";
        jmethodID method = activity_class == nullptr ? nullptr : env->GetMethodID(activity_class, method_name, "()V");
        if (method != nullptr) {
            env->CallVoidMethod(app->activity->clazz, method);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                LogError(std::string("soft keyboard Java call failed: ") + method_name);
            } else {
                LogInfo(std::string("soft keyboard ") + (visible ? "show" : "hide") + " requested via Java");
                if (activity_class != nullptr) {
                    env->DeleteLocalRef(activity_class);
                }
                if (did_attach) {
                    app->activity->vm->DetachCurrentThread();
                }
                return;
            }
        }
        if (activity_class != nullptr) {
            env->DeleteLocalRef(activity_class);
        }
    }

    if (did_attach) {
        app->activity->vm->DetachCurrentThread();
    }

    LogError(std::string("soft keyboard Java bridge unavailable: ") + (visible ? "show" : "hide"));
}

void ShowAndroidGuestPopup(const HostPopupRequest& request) {
    android_app* app = g_keyboard_app.load();
    if (app == nullptr || app->activity == nullptr) {
        EnqueueHostPopupResult(HostPopupResult{
            .token = request.token,
            .button_index = request.preferred_button_index,
        });
        return;
    }

    JNIEnv* env = nullptr;
    bool did_attach = false;
    if (app->activity->vm != nullptr) {
        const jint env_status = app->activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (env_status == JNI_EDETACHED) {
            did_attach = app->activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
        }
    }

    bool dispatched = false;
    if (env != nullptr && app->activity->clazz != nullptr) {
        jclass activity_class = env->GetObjectClass(app->activity->clazz);
        jclass string_class = env->FindClass("java/lang/String");
        jmethodID method = activity_class == nullptr ? nullptr
            : env->GetMethodID(activity_class, "showGuestPopup", "(ILjava/lang/String;Ljava/lang/String;[Ljava/lang/String;IZ)V");
        if (method != nullptr && string_class != nullptr) {
            jstring title = env->NewStringUTF(request.title.c_str());
            jstring message = env->NewStringUTF(request.message.c_str());
            jobjectArray buttons = env->NewObjectArray(
                static_cast<jsize>(request.buttons.size()), string_class, nullptr);
            if (buttons != nullptr) {
                for (jsize i = 0; i < static_cast<jsize>(request.buttons.size()); ++i) {
                    jstring button = env->NewStringUTF(request.buttons[static_cast<std::size_t>(i)].c_str());
                    if (button != nullptr) {
                        env->SetObjectArrayElement(buttons, i, button);
                        env->DeleteLocalRef(button);
                    }
                }
            }
            env->CallVoidMethod(
                app->activity->clazz,
                method,
                static_cast<jint>(request.token),
                title,
                message,
                buttons,
                static_cast<jint>(request.cancel_button_index),
                static_cast<jboolean>(request.class_name == "UIActionSheet"));
            if (!env->ExceptionCheck()) {
                dispatched = true;
                LogInfo("popup show requested token=" + std::to_string(request.token)
                    + " buttons=" + std::to_string(request.buttons.size()));
            } else {
                env->ExceptionClear();
                LogError("popup Java call failed: showGuestPopup");
            }
            if (buttons != nullptr) {
                env->DeleteLocalRef(buttons);
            }
            if (title != nullptr) {
                env->DeleteLocalRef(title);
            }
            if (message != nullptr) {
                env->DeleteLocalRef(message);
            }
        }
        if (string_class != nullptr) {
            env->DeleteLocalRef(string_class);
        }
        if (activity_class != nullptr) {
            env->DeleteLocalRef(activity_class);
        }
    }

    if (did_attach) {
        app->activity->vm->DetachCurrentThread();
    }

    if (!dispatched) {
        EnqueueHostPopupResult(HostPopupResult{
            .token = request.token,
            .button_index = request.preferred_button_index,
        });
    }
}

void DismissAndroidGuestPopup(const u32 token) {
    android_app* app = g_keyboard_app.load();
    if (app == nullptr || app->activity == nullptr) {
        return;
    }

    JNIEnv* env = nullptr;
    bool did_attach = false;
    if (app->activity->vm != nullptr) {
        const jint env_status = app->activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (env_status == JNI_EDETACHED) {
            did_attach = app->activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
        }
    }

    if (env != nullptr && app->activity->clazz != nullptr) {
        jclass activity_class = env->GetObjectClass(app->activity->clazz);
        jmethodID method = activity_class == nullptr ? nullptr
            : env->GetMethodID(activity_class, "dismissGuestPopup", "(I)V");
        if (method != nullptr) {
            env->CallVoidMethod(app->activity->clazz, method, static_cast<jint>(token));
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                LogError("popup Java call failed: dismissGuestPopup");
            }
        }
        if (activity_class != nullptr) {
            env->DeleteLocalRef(activity_class);
        }
    }

    if (did_attach) {
        app->activity->vm->DetachCurrentThread();
    }
}

struct MappedTouchPoint {
    bool inside_content = false;
    float x_points = 0.0f;
    float y_points = 0.0f;
};

MappedTouchPoint MapAndroidTouch(android_app* app, const float x, const float y) {
    constexpr float kGuestWidthPoints = 320.0f;
    constexpr float kGuestHeightPoints = 480.0f;
    constexpr float kGuestAspect = kGuestWidthPoints / kGuestHeightPoints;

    if (app == nullptr || app->window == nullptr) {
        return {};
    }

    const int window_width = std::max(1, ANativeWindow_getWidth(app->window));
    const int window_height = std::max(1, ANativeWindow_getHeight(app->window));
    float content_x = 0.0f;
    float content_y = 0.0f;
    float content_width = static_cast<float>(window_width);
    float content_height = static_cast<float>(window_height);

    const float window_aspect = content_width / content_height;
    if (window_aspect > kGuestAspect) {
        content_width = content_height * kGuestAspect;
        content_x = (static_cast<float>(window_width) - content_width) * 0.5f;
    } else {
        content_height = content_width / kGuestAspect;
        content_y = (static_cast<float>(window_height) - content_height) * 0.5f;
    }

    const bool inside = x >= content_x && x <= content_x + content_width
        && y >= content_y && y <= content_y + content_height;
    const float clamped_x = std::clamp(x, content_x, content_x + content_width);
    const float clamped_y = std::clamp(y, content_y, content_y + content_height);

    return MappedTouchPoint{
        .inside_content = inside,
        .x_points = (clamped_x - content_x) * kGuestWidthPoints / content_width,
        .y_points = (clamped_y - content_y) * kGuestHeightPoints / content_height,
    };
}

const char* TouchPhaseName(const HostTouchPhase phase) {
    switch (phase) {
    case HostTouchPhase::Began:
        return "began";
    case HostTouchPhase::Moved:
        return "moved";
    case HostTouchPhase::Ended:
        return "ended";
    case HostTouchPhase::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::optional<std::string> TextForAndroidKey(const int32_t key_code, const int32_t meta_state) {
    const bool shifted = (meta_state & (AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_RIGHT_ON)) != 0;
    const bool caps = (meta_state & AMETA_CAPS_LOCK_ON) != 0;

    if (key_code >= AKEYCODE_A && key_code <= AKEYCODE_Z) {
        char c = static_cast<char>('a' + (key_code - AKEYCODE_A));
        if (shifted ^ caps) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return std::string(1, c);
    }

    if (key_code >= AKEYCODE_0 && key_code <= AKEYCODE_9) {
        static constexpr char kNormalDigits[] = "0123456789";
        static constexpr char kShiftDigits[] = ")!@#$%^&*(";
        const int index = key_code - AKEYCODE_0;
        return std::string(1, shifted ? kShiftDigits[index] : kNormalDigits[index]);
    }

    if (key_code >= AKEYCODE_NUMPAD_0 && key_code <= AKEYCODE_NUMPAD_9) {
        return std::string(1, static_cast<char>('0' + (key_code - AKEYCODE_NUMPAD_0)));
    }

    switch (key_code) {
    case AKEYCODE_SPACE:
        return std::string(" ");
    case AKEYCODE_TAB:
        return std::string("\t");
    case AKEYCODE_COMMA:
        return std::string(1, shifted ? '<' : ',');
    case AKEYCODE_PERIOD:
        return std::string(1, shifted ? '>' : '.');
    case AKEYCODE_GRAVE:
        return std::string(1, shifted ? '~' : '`');
    case AKEYCODE_MINUS:
    case AKEYCODE_NUMPAD_SUBTRACT:
        return std::string(1, shifted ? '_' : '-');
    case AKEYCODE_EQUALS:
        return std::string(1, shifted ? '+' : '=');
    case AKEYCODE_LEFT_BRACKET:
        return std::string(1, shifted ? '{' : '[');
    case AKEYCODE_RIGHT_BRACKET:
        return std::string(1, shifted ? '}' : ']');
    case AKEYCODE_BACKSLASH:
        return std::string(1, shifted ? '|' : '\\');
    case AKEYCODE_SEMICOLON:
        return std::string(1, shifted ? ':' : ';');
    case AKEYCODE_APOSTROPHE:
        return std::string(1, shifted ? '"' : '\'');
    case AKEYCODE_SLASH:
    case AKEYCODE_NUMPAD_DIVIDE:
        return std::string(1, shifted ? '?' : '/');
    case AKEYCODE_AT:
        return std::string("@");
    case AKEYCODE_PLUS:
    case AKEYCODE_NUMPAD_ADD:
        return std::string("+");
    case AKEYCODE_STAR:
    case AKEYCODE_NUMPAD_MULTIPLY:
        return std::string("*");
    case AKEYCODE_POUND:
        return std::string("#");
    case AKEYCODE_NUMPAD_DOT:
    case AKEYCODE_NUMPAD_COMMA:
        return std::string(".");
    default:
        return std::nullopt;
    }
}

const char* AppCommandName(const int32_t command) {
    switch (command) {
    case APP_CMD_INPUT_CHANGED:
        return "INPUT_CHANGED";
    case APP_CMD_INIT_WINDOW:
        return "INIT_WINDOW";
    case APP_CMD_TERM_WINDOW:
        return "TERM_WINDOW";
    case APP_CMD_WINDOW_RESIZED:
        return "WINDOW_RESIZED";
    case APP_CMD_WINDOW_REDRAW_NEEDED:
        return "WINDOW_REDRAW_NEEDED";
    case APP_CMD_CONTENT_RECT_CHANGED:
        return "CONTENT_RECT_CHANGED";
    case APP_CMD_GAINED_FOCUS:
        return "GAINED_FOCUS";
    case APP_CMD_LOST_FOCUS:
        return "LOST_FOCUS";
    case APP_CMD_CONFIG_CHANGED:
        return "CONFIG_CHANGED";
    case APP_CMD_LOW_MEMORY:
        return "LOW_MEMORY";
    case APP_CMD_START:
        return "START";
    case APP_CMD_RESUME:
        return "RESUME";
    case APP_CMD_SAVE_STATE:
        return "SAVE_STATE";
    case APP_CMD_PAUSE:
        return "PAUSE";
    case APP_CMD_STOP:
        return "STOP";
    case APP_CMD_DESTROY:
        return "DESTROY";
    default:
        return "UNKNOWN";
    }
}

void EnqueueAndroidTouch(android_app* app,
                         AndroidAppState* state,
                         AInputEvent* event,
                         const std::size_t pointer_index,
                         const HostTouchPhase phase) {
    const int32_t pointer_id = AMotionEvent_getPointerId(event, pointer_index);
    const MappedTouchPoint mapped = MapAndroidTouch(
        app,
        AMotionEvent_getX(event, pointer_index),
        AMotionEvent_getY(event, pointer_index));

    if (phase == HostTouchPhase::Began && !mapped.inside_content) {
        return;
    }
    if (phase != HostTouchPhase::Began && state->active_touch_ids.count(pointer_id) == 0) {
        return;
    }

    if (phase == HostTouchPhase::Began) {
        state->active_touch_ids.insert(pointer_id);
    }

    EnqueueHostTouchEvent(HostTouchEvent{
        .pointer_id = pointer_id,
        .phase = phase,
        .x_points = mapped.x_points,
        .y_points = mapped.y_points,
        .timestamp_seconds = static_cast<double>(AMotionEvent_getEventTime(event)) / 1'000'000'000.0,
    });

    if (phase == HostTouchPhase::Began || phase == HostTouchPhase::Ended || phase == HostTouchPhase::Cancelled) {
        LogInfo("touch " + std::string(TouchPhaseName(phase))
            + " id=" + std::to_string(pointer_id)
            + " x=" + std::to_string(mapped.x_points)
            + " y=" + std::to_string(mapped.y_points)
            + " inside=" + std::to_string(mapped.inside_content ? 1 : 0));
    }

    if (phase == HostTouchPhase::Ended || phase == HostTouchPhase::Cancelled) {
        state->active_touch_ids.erase(pointer_id);
    }
}

int32_t HandleInputEvent(android_app* app, AInputEvent* event) {
    auto* state = static_cast<AndroidAppState*>(app->userData);
    if (state == nullptr) {
        return 0;
    }

    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        const int32_t action = AKeyEvent_getAction(event);
        if (action != AKEY_EVENT_ACTION_DOWN) {
            return 1;
        }

        const int32_t key_code = AKeyEvent_getKeyCode(event);
        const int32_t meta_state = AKeyEvent_getMetaState(event);
        const double timestamp_seconds = static_cast<double>(AKeyEvent_getEventTime(event)) / 1'000'000'000.0;

        if (key_code == AKEYCODE_DEL || key_code == AKEYCODE_FORWARD_DEL) {
            EnqueueHostKeyEvent(HostKeyEvent{
                .phase = HostKeyPhase::Backspace,
                .text = {},
                .timestamp_seconds = timestamp_seconds,
            });
            LogInfo("key backspace");
            return 1;
        }
        if (key_code == AKEYCODE_ENTER || key_code == AKEYCODE_NUMPAD_ENTER) {
            EnqueueHostKeyEvent(HostKeyEvent{
                .phase = HostKeyPhase::Enter,
                .text = {},
                .timestamp_seconds = timestamp_seconds,
            });
            LogInfo("key enter");
            return 1;
        }

        if (const std::optional<std::string> text = TextForAndroidKey(key_code, meta_state)) {
            EnqueueHostKeyEvent(HostKeyEvent{
                .phase = HostKeyPhase::Text,
                .text = *text,
                .timestamp_seconds = timestamp_seconds,
            });
            LogInfo("key text code=" + std::to_string(key_code) + " text=" + *text);
            return 1;
        }

        LogInfo("key ignored code=" + std::to_string(key_code));
        return 0;
    }

    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        return 0;
    }
    if ((AInputEvent_getSource(event) & AINPUT_SOURCE_CLASS_POINTER) == 0) {
        return 0;
    }

    const int32_t action = AMotionEvent_getAction(event);
    const int32_t action_masked = action & AMOTION_EVENT_ACTION_MASK;
    const std::size_t action_index = static_cast<std::size_t>(
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const std::size_t pointer_count = static_cast<std::size_t>(AMotionEvent_getPointerCount(event));

    switch (action_masked) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        if (action_index < pointer_count) {
            EnqueueAndroidTouch(app, state, event, action_index, HostTouchPhase::Began);
        }
        return 1;
    case AMOTION_EVENT_ACTION_MOVE:
        for (std::size_t i = 0; i < pointer_count; ++i) {
            EnqueueAndroidTouch(app, state, event, i, HostTouchPhase::Moved);
        }
        return 1;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        if (action_index < pointer_count) {
            EnqueueAndroidTouch(app, state, event, action_index, HostTouchPhase::Ended);
        }
        return 1;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (std::size_t i = 0; i < pointer_count; ++i) {
            EnqueueAndroidTouch(app, state, event, i, HostTouchPhase::Cancelled);
        }
        state->active_touch_ids.clear();
        return 1;
    default:
        return 0;
    }
}

std::optional<std::vector<u8>> ReadAssetBytes(AAssetManager* assets, const std::string& asset_path) {
    if (assets == nullptr || asset_path.empty()) {
        return std::nullopt;
    }

    if (IsTrackedSoundAssetPath(asset_path)) {
        LogInfo("[sound-debug] ReadAssetBytes request asset=" + asset_path);
    }
    AAsset* asset = AAssetManager_open(assets, asset_path.c_str(), AASSET_MODE_STREAMING);
    if (asset == nullptr) {
        if (IsTrackedSoundAssetPath(asset_path)) {
            LogError("[sound-debug] ReadAssetBytes miss asset=" + asset_path);
        }
        return std::nullopt;
    }

    std::vector<u8> bytes;
    const auto length = static_cast<std::size_t>(AAsset_getLength64(asset));
    bytes.reserve(length);
    std::array<char, 32 * 1024> buffer{};
    for (;;) {
        const int read = AAsset_read(asset, buffer.data(), buffer.size());
        if (read < 0) {
            AAsset_close(asset);
            return std::nullopt;
        }
        if (read == 0) {
            break;
        }
        const auto begin = reinterpret_cast<const u8*>(buffer.data());
        bytes.insert(bytes.end(), begin, begin + read);
    }

    AAsset_close(asset);
    if (IsTrackedSoundAssetPath(asset_path)) {
        LogInfo("[sound-debug] ReadAssetBytes hit asset=" + asset_path + " bytes=" + std::to_string(bytes.size()));
    }
    return bytes;
}

bool AssetExists(AAssetManager* assets, const std::string& asset_path) {
    if (assets == nullptr || asset_path.empty()) {
        return false;
    }
    if (IsTrackedSoundAssetPath(asset_path)) {
        LogInfo("[sound-debug] AssetExists request asset=" + asset_path);
    }
    AAsset* asset = AAssetManager_open(assets, asset_path.c_str(), AASSET_MODE_UNKNOWN);
    if (asset == nullptr) {
        if (IsTrackedSoundAssetPath(asset_path)) {
            LogError("[sound-debug] AssetExists miss asset=" + asset_path);
        }
        return false;
    }
    AAsset_close(asset);
    if (IsTrackedSoundAssetPath(asset_path)) {
        LogInfo("[sound-debug] AssetExists hit asset=" + asset_path);
    }
    return true;
}

void CheckApkAssets(AAssetManager* assets) {
    if (const auto globals = ReadAssetBytes(assets, "csv/globals.csv")) {
        LogInfo("APK assets ready globals.csv bytes=" + std::to_string(globals->size()));
    } else {
        LogError("APK asset missing csv/globals.csv");
    }
}

void RunEmulator(AndroidAppState* state) {
    try {
        AAssetManager* assets = state->app->activity->assetManager;
        CheckApkAssets(assets);
        auto binary_bytes = ReadAssetBytes(assets, "binary/Spooky Pop");
        if (!binary_bytes) {
            throw std::runtime_error("APK asset missing binary/Spooky Pop");
        }
        const std::filesystem::path sandbox_root = std::filesystem::path(state->app->activity->internalDataPath) / "sandbox";
        LogInfo("starting emulator with APK assets"
            + std::string(" binary_bytes=") + std::to_string(binary_bytes->size())
            + " sandbox at " + sandbox_root.string());
        Emulator emulator(EmulatorOptions{
            .binary_path = "Spooky Pop",
            .binary_bytes = std::move(binary_bytes),
            .external_root = {},
            .sandbox_root = sandbox_root,
            .asset_exists = [assets](const std::string& path) {
                return AssetExists(assets, path);
            },
            .read_asset = [assets](const std::string& path) {
                return ReadAssetBytes(assets, path);
            },
            .trace_shims = false,
        });
        emulator.Run(true, true, 1'000'000'000'000ULL);
    } catch (const std::exception& exception) {
        LogError(std::string("fatal: ") + exception.what());
    }
}

void MaybeStartEmulator(AndroidAppState* state) {
    if (state == nullptr || state->app == nullptr || state->app->window == nullptr) {
        return;
    }
    bool expected = false;
    if (!state->emulator_started.compare_exchange_strong(expected, true)) {
        return;
    }
    state->emulator_thread = std::thread(RunEmulator, state);
}

void HandleAppCommand(android_app* app, const int32_t command) {
    auto* state = static_cast<AndroidAppState*>(app->userData);
    LogInfo("app command " + std::to_string(command) + " " + AppCommandName(command));
    switch (command) {
    case APP_CMD_INIT_WINDOW:
        SetAndroidHostWindow(app->window);
        LogInfo("APP_CMD_INIT_WINDOW window=" + std::to_string(reinterpret_cast<std::uintptr_t>(app->window)));
        MaybeStartEmulator(state);
        break;
    case APP_CMD_TERM_WINDOW:
        if (state != nullptr) {
            state->active_touch_ids.clear();
        }
        SetAndroidHostWindow(nullptr);
        break;
    default:
        break;
    }
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_supercell_phoenix_ClashNativeActivity_nativeCommitText(JNIEnv* env, jclass, jstring text) {
    if (env == nullptr || text == nullptr) {
        return;
    }
    const char* utf = env->GetStringUTFChars(text, nullptr);
    if (utf == nullptr) {
        return;
    }
    const std::string value = utf;
    env->ReleaseStringUTFChars(text, utf);
    if (value.empty()) {
        return;
    }

    EnqueueHostKeyEvent(HostKeyEvent{
        .phase = HostKeyPhase::Text,
        .text = value,
    });
    __android_log_print(ANDROID_LOG_INFO, "atrasis", "[atrasis] ime text=%s", value.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_supercell_phoenix_ClashNativeActivity_nativeDeleteBackward(JNIEnv*, jclass) {
    EnqueueHostKeyEvent(HostKeyEvent{
        .phase = HostKeyPhase::Backspace,
        .text = {},
    });
    __android_log_print(ANDROID_LOG_INFO, "atrasis", "[atrasis] ime backspace");
}

extern "C" JNIEXPORT void JNICALL
Java_com_supercell_phoenix_ClashNativeActivity_nativeEnter(JNIEnv*, jclass) {
    EnqueueHostKeyEvent(HostKeyEvent{
        .phase = HostKeyPhase::Enter,
        .text = {},
    });
    __android_log_print(ANDROID_LOG_INFO, "atrasis", "[atrasis] ime enter");
}

extern "C" JNIEXPORT void JNICALL
Java_com_supercell_phoenix_ClashNativeActivity_nativePopupResult(JNIEnv*, jclass, jint token, jint button_index) {
    EnqueueHostPopupResult(HostPopupResult{
        .token = static_cast<u32>(token),
        .button_index = static_cast<s32>(button_index),
    });
    __android_log_print(ANDROID_LOG_INFO, "atrasis", "[atrasis] popup result token=%d button=%d", token, button_index);
}

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm == nullptr || vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        return JNI_ERR;
    }

    jclass activity_class = env->FindClass("com/supercell/phoenix/ClashNativeActivity");
    if (activity_class == nullptr) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        __android_log_print(ANDROID_LOG_INFO, "atrasis", "[atrasis] JNI_OnLoad: ClashNativeActivity not ready");
        return JNI_VERSION_1_6;
    }

    JNINativeMethod methods[] = {
        {"nativeCommitText", "(Ljava/lang/String;)V",
            reinterpret_cast<void*>(Java_com_supercell_phoenix_ClashNativeActivity_nativeCommitText)},
        {"nativeDeleteBackward", "()V",
            reinterpret_cast<void*>(Java_com_supercell_phoenix_ClashNativeActivity_nativeDeleteBackward)},
        {"nativeEnter", "()V",
            reinterpret_cast<void*>(Java_com_supercell_phoenix_ClashNativeActivity_nativeEnter)},
        {"nativePopupResult", "(II)V",
            reinterpret_cast<void*>(Java_com_supercell_phoenix_ClashNativeActivity_nativePopupResult)},
    };

    if (env->RegisterNatives(activity_class, methods, static_cast<jint>(sizeof(methods) / sizeof(methods[0]))) != JNI_OK) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        __android_log_print(ANDROID_LOG_ERROR, "atrasis", "[atrasis] JNI_OnLoad: RegisterNatives failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, "atrasis", "[atrasis] JNI_OnLoad: keyboard natives registered");
    }
    env->DeleteLocalRef(activity_class);
    return JNI_VERSION_1_6;
}

void android_main(android_app* app) {
    LogInfo("android_main entered");

    AndroidAppState state;
    state.app = app;
    g_keyboard_app.store(app);
    SetHostKeyboardVisibilityCallback(SetAndroidSoftKeyboardVisible);
    SetHostPopupCallbacks(ShowAndroidGuestPopup, DismissAndroidGuestPopup);
    app->userData = &state;
    app->onAppCmd = HandleAppCommand;
    app->onInputEvent = HandleInputEvent;

    for (;;) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(state.emulator_started ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            if (app->destroyRequested != 0) {
                SetHostKeyboardVisibilityCallback(nullptr);
                SetHostPopupCallbacks(nullptr, nullptr);
                g_keyboard_app.store(nullptr);
                SetAndroidHostWindow(nullptr);
                if (state.emulator_thread.joinable()) {
                    state.emulator_thread.detach();
                }
                return;
            }
        }
        MaybeStartEmulator(&state);
    }
}

#endif
