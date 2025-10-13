#include <jni.h>
#include <string>
#include <zygisk.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <unordered_map>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>
#include <mutex>
#include <functional>
#include <sys/stat.h>

using json = nlohmann::json;

#define LOG_TAG "SpoofModule"
#define LOGD(...) if (debug_mode) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static bool debug_mode = true;

static const std::unordered_map<std::string, int> android_version_to_sdk = {
    {"16", 36},
    {"15", 35},
    {"14", 34},
    {"13", 33},  // Android 13 (Tiramisu)
    {"12", 31},  // Android 12 (Snow Cone)
    {"11", 30},  // Android 11 (Red Velvet Cake)
    {"10", 29},  // Android 10 (Quince Tart)
    {"9", 28},   // Android 9 (Pie)
    {"8.1", 27}, // Android 8.1 (Oreo)
    {"8.0", 26}  // Android 8.0 (Oreo)
};

struct DeviceInfo {
    std::string brand;
    std::string device;
    std::string manufacturer;
    std::string model;
    std::string fingerprint;
    std::string product;
    std::string android_version; 
    int sdk_int;                
};

static DeviceInfo current_info;
static std::mutex info_mutex;
static jclass buildClass = nullptr;
static jclass versionClass = nullptr; 
static jfieldID modelField = nullptr;
static jfieldID brandField = nullptr;
static jfieldID deviceField = nullptr;
static jfieldID manufacturerField = nullptr;
static jfieldID fingerprintField = nullptr;
static jfieldID productField = nullptr;
static jfieldID releaseField = nullptr; 
static jfieldID sdkIntField = nullptr;  
static std::once_flag build_once;

static time_t last_config_mtime = 0;
static const std::string config_path = "/data/adb/modules/COPG/config.json";

struct JniString {
    JNIEnv* env;
    jstring jstr;
    const char* chars;
    JniString(JNIEnv* e, jstring s) : env(e), jstr(s), chars(nullptr) {
        if (jstr) chars = env->GetStringUTFChars(jstr, nullptr);
    }
    ~JniString() {
        if (jstr && chars) env->ReleaseStringUTFChars(jstr, chars);
    }
    const char* get() const { return chars; }
};

class SpoofModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;

        LOGD("Module loaded successfully");

        ensureBuildClass();
        reloadIfNeeded(true);
    }

    void onUnload() {
        std::lock_guard<std::mutex> lock(info_mutex);
        if (buildClass) {
            env->DeleteGlobalRef(buildClass);
            buildClass = nullptr;
            LOGD("Global ref for Build class released");
        }
        if (versionClass) {
            env->DeleteGlobalRef(versionClass);
            versionClass = nullptr;
            LOGD("Global ref for VERSION class released");
        }
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        if (!args || !args->nice_name) {
            LOGD("No package name provided, closing module");
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        JniString pkg(env, args->nice_name);
        const char* package_name = pkg.get();
        if (!package_name) {
            LOGE("Failed to get package name");
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        LOGD("Processing package: %s", package_name);

        reloadIfNeeded(false);

        bool should_close = true;
        {
            std::lock_guard<std::mutex> lock(info_mutex);
            auto it = package_map.find(package_name);
            if (it != package_map.end()) {
                current_info = it->second;
                LOGD("Spoofing device for package %s: %s (Android %s, SDK %d)", 
                     package_name, current_info.model.c_str(), 
                     current_info.android_version.c_str(), current_info.sdk_int);
                spoofDevice(current_info);
                should_close = false;
            }
        }

        if (should_close) {
            LOGD("Package %s not found in config, closing module", package_name);
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        } else {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            LOGD("Set DLCLOSE after spoofing for stealth");
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!args || !args->nice_name || package_map.empty()) return;

        ensureBuildClass();
        if (!buildClass || !versionClass) {
            LOGE("Build or VERSION class not initialized, skipping postAppSpecialize");
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        JniString pkg(env, args->nice_name);
        const char* package_name = pkg.get();
        if (!package_name) {
            LOGE("Failed to get package name in postAppSpecialize");
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(info_mutex);
            auto it = package_map.find(package_name);
            if (it != package_map.end()) {
                current_info = it->second;
                LOGD("Post-specialize spoofing for %s: %s (Android %s, SDK %d)", 
                     package_name, current_info.model.c_str(), 
                     current_info.android_version.c_str(), current_info.sdk_int);
                spoofDevice(current_info);
            }
        }

        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        LOGD("Set DLCLOSE in postAppSpecialize for extra stealth");
    }

private:
    zygisk::Api* api;
    JNIEnv* env;
    std::unordered_map<std::string, DeviceInfo> package_map;

    void ensureBuildClass() {
        std::call_once(build_once, [&] {
            jclass localBuild = env->FindClass("android/os/Build");
            if (!localBuild || env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGE("Failed to find android/os/Build class");
                return;
            }
            buildClass = static_cast<jclass>(env->NewGlobalRef(localBuild));
            env->DeleteLocalRef(localBuild);
            if (!buildClass) {
                LOGE("Failed to create global reference for Build class");
                return;
            }

            jclass localVersion = env->FindClass("android/os/Build$VERSION");
            if (!localVersion || env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGE("Failed to find android/os/Build$VERSION class");
                return;
            }
            versionClass = static_cast<jclass>(env->NewGlobalRef(localVersion));
            env->DeleteLocalRef(localVersion);
            if (!versionClass) {
                LOGE("Failed to create global reference for VERSION class");
                return;
            }

            modelField = env->GetStaticFieldID(buildClass, "MODEL", "Ljava/lang/String;");
            brandField = env->GetStaticFieldID(buildClass, "BRAND", "Ljava/lang/String;");
            deviceField = env->GetStaticFieldID(buildClass, "DEVICE", "Ljava/lang/String;");
            manufacturerField = env->GetStaticFieldID(buildClass, "MANUFACTURER", "Ljava/lang/String;");
            fingerprintField = env->GetStaticFieldID(buildClass, "FINGERPRINT", "Ljava/lang/String;");
            productField = env->GetStaticFieldID(buildClass, "PRODUCT", "Ljava/lang/String;");

            
            releaseField = env->GetStaticFieldID(versionClass, "RELEASE", "Ljava/lang/String;");
            sdkIntField = env->GetStaticFieldID(versionClass, "SDK_INT", "I");

            if (env->ExceptionCheck() || !modelField || !brandField || !deviceField ||
                !manufacturerField || !fingerprintField || !productField ||
                !releaseField || !sdkIntField) {
                env->ExceptionClear();
                LOGE("Failed to get field IDs for Build or VERSION class");
                env->DeleteGlobalRef(buildClass);
                env->DeleteGlobalRef(versionClass);
                buildClass = nullptr;
                versionClass = nullptr;
            }
        });
    }

    void reloadIfNeeded(bool force = false) {
        struct stat file_stat;
        if (stat(config_path.c_str(), &file_stat) != 0) {
            LOGE("Failed to stat config file: %s", strerror(errno));
            return;
        }

        time_t current_mtime = file_stat.st_mtime;
        if (!force && current_mtime == last_config_mtime) {
            LOGD("Config unchanged, skipping reload");
            return;
        }

        LOGD("Config changed or force load, reloading...");

        std::ifstream file(config_path);
        if (!file.is_open()) {
            LOGE("Failed to open config.json at %s", config_path.c_str());
            return;
        }
        LOGD("Config file opened successfully");

        try {
            json config = json::parse(file);
            std::unordered_map<std::string, DeviceInfo> new_map;

            for (auto& [key, value] : config.items()) {
                if (key.find("PACKAGES_") != 0 || key.rfind("_DEVICE") == (key.size() - 7)) continue;
                if (!value.is_array()) {
                    LOGE("Invalid package list for key %s", key.c_str());
                    continue;
                }
                auto packages = value.get<std::vector<std::string>>();
                std::string device_key = key + "_DEVICE";
                if (!config.contains(device_key) || !config[device_key].is_object()) {
                    LOGE("No valid device info for key %s", key.c_str());
                    continue;
                }
                auto device = config[device_key];

                DeviceInfo info;
                info.brand = device.value("BRAND", "generic");
                info.device = device.value("DEVICE", "generic");
                info.manufacturer = device.value("MANUFACTURER", "generic");
                info.model = device.value("MODEL", "generic");
                info.fingerprint = device.value("FINGERPRINT", "generic/brand/device:13/TQ3A.230805.001/123456:user/release-keys");
                info.product = device.value("PRODUCT", info.brand);
                info.android_version = device.value("ANDROID_VERSION", "13"); 
                auto sdk_it = android_version_to_sdk.find(info.android_version);
                info.sdk_int = (sdk_it != android_version_to_sdk.end()) ? sdk_it->second : 33; 

                for (const auto& pkg : packages) {
                    new_map[pkg] = info;
                    LOGD("Loaded package %s with model %s (Android %s, SDK %d)", 
                         pkg.c_str(), info.model.c_str(), info.android_version.c_str(), info.sdk_int);
                }
            }

            {
                std::lock_guard<std::mutex> lock(info_mutex);
                package_map = std::move(new_map);
            }

            last_config_mtime = current_mtime;
            LOGD("Config reloaded with %zu packages", package_map.size());
        } catch (const json::exception& e) {
            LOGE("JSON parsing error: %s", e.what());
        } catch (const std::exception& e) {
            LOGE("Error loading config: %s", e.what());
        }
        file.close();
    }

    void spoofDevice(const DeviceInfo& info) {
        if (!buildClass || !versionClass) {
            LOGE("Build or VERSION class is not initialized!");
            return;
        }

        LOGD("Spoofing device: %s (Android %s, SDK %d)", 
             info.model.c_str(), info.android_version.c_str(), info.sdk_int);

        auto setStr = [&](jclass clazz, jfieldID field, const std::string& value) {
            if (!field) return;
            jstring js = env->NewStringUTF(value.c_str());
            if (!js || env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGE("Failed to create string for field");
                return;
            }
            env->SetStaticObjectField(clazz, field, js);
            env->DeleteLocalRef(js);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGE("Failed to set field");
            }
        };

        auto setInt = [&](jclass clazz, jfieldID field, int value) {
            if (!field) return;
            env->SetStaticIntField(clazz, field, value);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGE("Failed to set int field");
            }
        };

        setStr(buildClass, modelField, info.model);
        setStr(buildClass, brandField, info.brand);
        setStr(buildClass, deviceField, info.device);
        setStr(buildClass, manufacturerField, info.manufacturer);
        setStr(buildClass, fingerprintField, info.fingerprint);
        setStr(buildClass, productField, info.product);

        setStr(versionClass, releaseField, info.android_version);
        setInt(versionClass, sdkIntField, info.sdk_int);
    }
};

REGISTER_ZYGISK_MODULE(SpoofModule)
