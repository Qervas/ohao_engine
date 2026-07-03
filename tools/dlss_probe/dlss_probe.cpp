// dlss_probe.cpp
//
// Phase 0 feasibility gate for DLSS Ray Reconstruction (DLSS-RR / "dlssd") on this
// machine. Standalone: creates a Vulkan device on the NVIDIA GPU, initializes NGX,
// and queries whether the Ray-Reconstruction (SuperSamplingDenoising) feature is
// available. Prints a definitive AVAILABLE / UNAVAILABLE verdict with the raw NGX
// result code decoded.
//
// It does NOT integrate anything into the engine. Build/run instructions live in
// tools/dlss_probe/build.sh.

#include <vulkan/vulkan.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const char* kSnippetDirDefault =
    "/home/frankyin/Desktop/Github/ohao_engine/external/DLSS/lib/Linux_x86_64/rel";
static const char* kAppDataDirDefault =
    "/home/frankyin/Desktop/Github/ohao_engine/tools/dlss_probe/ngx_appdata";
static const char* kProjectId  = "a0f57b54-1daf-4934-90ae-c4035c19df04"; // arbitrary UUID
static const char* kEngineVer  = "1.0.0";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring widen(const std::string& s) {
    // Paths here are ASCII; simple widening is sufficient.
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

static const char* ngxResultName(NVSDK_NGX_Result r) {
    switch (r) {
        case NVSDK_NGX_Result_Success:                       return "Success";
        case NVSDK_NGX_Result_Fail:                          return "Fail";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:      return "FAIL_FeatureNotSupported";
        case NVSDK_NGX_Result_FAIL_PlatformError:            return "FAIL_PlatformError";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:     return "FAIL_FeatureAlreadyExists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound:          return "FAIL_FeatureNotFound";
        case NVSDK_NGX_Result_FAIL_InvalidParameter:         return "FAIL_InvalidParameter";
        case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:    return "FAIL_ScratchBufferTooSmall";
        case NVSDK_NGX_Result_FAIL_NotInitialized:           return "FAIL_NotInitialized";
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:   return "FAIL_UnsupportedInputFormat";
        case NVSDK_NGX_Result_FAIL_RWFlagMissing:            return "FAIL_RWFlagMissing";
        case NVSDK_NGX_Result_FAIL_MissingInput:             return "FAIL_MissingInput";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:return "FAIL_UnableToInitializeFeature";
        case NVSDK_NGX_Result_FAIL_OutOfDate:                return "FAIL_OutOfDate";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:           return "FAIL_OutOfGPUMemory";
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat:        return "FAIL_UnsupportedFormat";
        case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:return "FAIL_UnableToWriteToAppDataPath";
        case NVSDK_NGX_Result_FAIL_UnsupportedParameter:     return "FAIL_UnsupportedParameter";
        case NVSDK_NGX_Result_FAIL_Denied:                   return "FAIL_Denied";
        case NVSDK_NGX_Result_FAIL_NotImplemented:           return "FAIL_NotImplemented";
        default:                                             return "Unknown";
    }
}

static const char* ngxSupportName(NVSDK_NGX_Feature_Support_Result r) {
    switch (r) {
        case NVSDK_NGX_FeatureSupportResult_Supported:                    return "Supported";
        case NVSDK_NGX_FeatureSupportResult_CheckNotPresent:             return "CheckNotPresent";
        case NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported:    return "DriverVersionUnsupported";
        case NVSDK_NGX_FeatureSupportResult_AdapterUnsupported:          return "AdapterUnsupported";
        case NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported: return "OSVersionBelowMinimumSupported";
        case NVSDK_NGX_FeatureSupportResult_NotImplemented:             return "NotImplemented";
        default:                                                         return "Unknown";
    }
}

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[FATAL] %s -> VkResult %d\n", #x, (int)_r); return 2; } } while (0)

int main(int argc, char** argv) {
    std::string snippetDir = (argc > 1) ? argv[1] : kSnippetDirDefault;
    std::string appDataDir = (argc > 2) ? argv[2] : kAppDataDirDefault;
    mkdir(appDataDir.c_str(), 0755); // best-effort; NGX needs a writable dir

    std::wstring snippetDirW = widen(snippetDir);
    std::wstring appDataDirW = widen(appDataDir);

    printf("=== DLSS-RR availability probe ===\n");
    printf("snippet dir : %s\n", snippetDir.c_str());
    printf("appdata dir : %s\n", appDataDir.c_str());

    // -- NGX common info: tell NGX where the dlssd snippet .so lives, verbose logging.
    const wchar_t* pathList[1] = { snippetDirW.c_str() };
    NVSDK_NGX_FeatureCommonInfo commonInfo = {};
    commonInfo.PathListInfo.Path                = pathList;
    commonInfo.PathListInfo.Length              = 1;
    commonInfo.LoggingInfo.LoggingCallback      = nullptr;
    commonInfo.LoggingInfo.MinimumLoggingLevel  = NVSDK_NGX_LOGGING_LEVEL_ON;
    commonInfo.LoggingInfo.DisableOtherLoggingSinks = false;

    // -- Feature-discovery descriptor for Ray Reconstruction (dlssd).
    NVSDK_NGX_FeatureDiscoveryInfo disc = {};
    disc.SDKVersion                        = NVSDK_NGX_Version_API;
    disc.FeatureID                         = NVSDK_NGX_Feature_RayReconstruction;
    disc.Identifier.IdentifierType         = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    disc.Identifier.v.ProjectDesc.ProjectId    = kProjectId;
    disc.Identifier.v.ProjectDesc.EngineType   = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    disc.Identifier.v.ProjectDesc.EngineVersion= kEngineVer;
    disc.ApplicationDataPath               = appDataDirW.c_str();
    disc.FeatureInfo                       = &commonInfo;

    // -----------------------------------------------------------------------
    // 1. Instance extension requirements for DLSS-RR
    // -----------------------------------------------------------------------
    std::vector<const char*> instExtensions = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    {
        unsigned int n = 0;
        VkExtensionProperties* props = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&disc, &n, &props);
        printf("\n[instance ext query] result=0x%08X (%s) count=%u\n",
               (unsigned)r, ngxResultName(r), n);
        if (NVSDK_NGX_SUCCEED(r) && props) {
            for (unsigned i = 0; i < n; ++i) {
                printf("    require instance ext: %s\n", props[i].extensionName);
                instExtensions.push_back(strdup(props[i].extensionName));
            }
        } else {
            printf("    (falling back to base instance extension set)\n");
        }
    }

    // -----------------------------------------------------------------------
    // 2. Create Vulkan instance (API 1.3)
    // -----------------------------------------------------------------------
    VkApplicationInfo appInfo = {};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "dlss_probe";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName        = "ohao";
    appInfo.engineVersion      = 1;
    appInfo.apiVersion         = VK_API_VERSION_1_3;

    // Dedup instance extensions.
    std::vector<const char*> instExtDedup;
    for (const char* e : instExtensions) {
        bool dup = false;
        for (const char* k : instExtDedup) if (strcmp(e, k) == 0) { dup = true; break; }
        if (!dup) instExtDedup.push_back(e);
    }

    VkInstanceCreateInfo ici = {};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &appInfo;
    ici.enabledExtensionCount   = (uint32_t)instExtDedup.size();
    ici.ppEnabledExtensionNames = instExtDedup.data();

    VkInstance instance = VK_NULL_HANDLE;
    VKCHECK(vkCreateInstance(&ici, nullptr, &instance));
    printf("\n[vk] instance created with %zu extension(s)\n", instExtDedup.size());

    // -----------------------------------------------------------------------
    // 3. Pick the NVIDIA physical device (vendorID 0x10DE)
    // -----------------------------------------------------------------------
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (devCount == 0) { fprintf(stderr, "[FATAL] no Vulkan devices\n"); return 2; }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties chosenProps = {};
    printf("\n[vk] enumerated %u physical device(s):\n", devCount);
    for (auto d : devs) {
        VkPhysicalDeviceProperties p = {};
        vkGetPhysicalDeviceProperties(d, &p);
        printf("    - %-45s vendorID=0x%04X type=%d\n", p.deviceName, p.vendorID, p.deviceType);
        if (p.vendorID == 0x10DE && phys == VK_NULL_HANDLE) { phys = d; chosenProps = p; }
    }
    if (phys == VK_NULL_HANDLE) {
        fprintf(stderr, "[FATAL] no NVIDIA (0x10DE) device found — cannot probe DLSS.\n");
        return 2;
    }
    printf("[vk] SELECTED GPU: %s (driverVersion=0x%08X, apiVersion=%u.%u.%u)\n",
           chosenProps.deviceName, chosenProps.driverVersion,
           VK_VERSION_MAJOR(chosenProps.apiVersion),
           VK_VERSION_MINOR(chosenProps.apiVersion),
           VK_VERSION_PATCH(chosenProps.apiVersion));

    // -----------------------------------------------------------------------
    // 4. Independent availability pre-check: GetFeatureRequirements
    // -----------------------------------------------------------------------
    {
        NVSDK_NGX_FeatureRequirement req = {};
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, phys, &disc, &req);
        printf("\n[GetFeatureRequirements] result=0x%08X (%s)\n", (unsigned)r, ngxResultName(r));
        printf("    FeatureSupported=%d (%s) minHWArch=0x%X minOS='%s'\n",
               (int)req.FeatureSupported, ngxSupportName(req.FeatureSupported),
               req.MinHWArchitecture, req.MinOSVersion);
    }

    // -----------------------------------------------------------------------
    // 5. Device extension requirements for DLSS-RR
    // -----------------------------------------------------------------------
    std::vector<const char*> devExtensions;
    {
        unsigned int n = 0;
        VkExtensionProperties* props = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, phys, &disc, &n, &props);
        printf("\n[device ext query] result=0x%08X (%s) count=%u\n", (unsigned)r, ngxResultName(r), n);
        if (NVSDK_NGX_SUCCEED(r) && props) {
            for (unsigned i = 0; i < n; ++i) {
                printf("    require device ext: %s\n", props[i].extensionName);
                devExtensions.push_back(strdup(props[i].extensionName));
            }
        }
    }
    // Intersect required device extensions with those actually supported.
    uint32_t availCount = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> avail(availCount);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &availCount, avail.data());
    auto isSupported = [&](const char* name) {
        for (auto& e : avail) if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    std::vector<const char*> devExtEnabled;
    for (const char* e : devExtensions) {
        if (isSupported(e)) devExtEnabled.push_back(e);
        else printf("    [warn] required device ext NOT supported by device: %s\n", e);
    }

    // -----------------------------------------------------------------------
    // 6. Create a VkDevice with a graphics+compute queue
    // -----------------------------------------------------------------------
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
    uint32_t qfIdx = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { qfIdx = i; break; }
    }
    if (qfIdx == UINT32_MAX) { fprintf(stderr, "[FATAL] no graphics+compute queue family\n"); return 2; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfIdx;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    // Enable a couple of commonly-required features (harmless if unused).
    VkPhysicalDeviceVulkan12Features feats12 = {};
    feats12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 feats2 = {};
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.pNext = &feats12;
    vkGetPhysicalDeviceFeatures2(phys, &feats2);
    // Keep only what the device supports (query returned supported bits).

    VkDeviceCreateInfo dci = {};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = &feats2;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = (uint32_t)devExtEnabled.size();
    dci.ppEnabledExtensionNames = devExtEnabled.data();

    VkDevice device = VK_NULL_HANDLE;
    VKCHECK(vkCreateDevice(phys, &dci, nullptr, &device));
    printf("\n[vk] device created (queueFamily=%u, %zu ext enabled)\n", qfIdx, devExtEnabled.size());

    // -----------------------------------------------------------------------
    // 7. Initialize NGX for Vulkan (ProjectID form)
    // -----------------------------------------------------------------------
    NVSDK_NGX_Result initRes = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVer,
        appDataDirW.c_str(), instance, phys, device,
        vkGetInstanceProcAddr, vkGetDeviceProcAddr, &commonInfo, NVSDK_NGX_Version_API);
    printf("\n[NVSDK_NGX_VULKAN_Init_with_ProjectID] result=0x%08X (%s)\n",
           (unsigned)initRes, ngxResultName(initRes));

    int    ssdAvailable      = 0;
    int    ssdInitResultRaw  = 0;
    int    ssdNeedsDriver    = 0;
    unsigned ssdMinMaj = 0, ssdMinMin = 0;
    bool   gotCaps           = false;

    if (NVSDK_NGX_SUCCEED(initRes)) {
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Result capRes = NVSDK_NGX_VULKAN_GetCapabilityParameters(&params);
        printf("[NVSDK_NGX_VULKAN_GetCapabilityParameters] result=0x%08X (%s)\n",
               (unsigned)capRes, ngxResultName(capRes));
        if (NVSDK_NGX_SUCCEED(capRes) && params) {
            gotCaps = true;
            NVSDK_NGX_Result g1 = params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &ssdAvailable);
            NVSDK_NGX_Result g2 = params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &ssdInitResultRaw);
            params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &ssdNeedsDriver);
            params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &ssdMinMaj);
            params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &ssdMinMin);

            printf("\n--- capability parameters (SuperSamplingDenoising == DLSS-RR) ---\n");
            printf("    Available            = %d  (Get result 0x%08X %s)\n",
                   ssdAvailable, (unsigned)g1, ngxResultName(g1));
            printf("    FeatureInitResult    = 0x%08X (%s)  (Get result 0x%08X %s)\n",
                   (unsigned)ssdInitResultRaw, ngxResultName((NVSDK_NGX_Result)ssdInitResultRaw),
                   (unsigned)g2, ngxResultName(g2));
            printf("    NeedsUpdatedDriver   = %d\n", ssdNeedsDriver);
            printf("    MinDriverVersion     = %u.%u\n", ssdMinMaj, ssdMinMin);

            NVSDK_NGX_VULKAN_DestroyParameters(params);
        }
    }

    // -----------------------------------------------------------------------
    // 8. VERDICT
    // -----------------------------------------------------------------------
    printf("\n================================================================\n");
    if (gotCaps && ssdAvailable) {
        printf("DLSS-RR AVAILABLE on: %s\n", chosenProps.deviceName);
        if (ssdMinMaj || ssdMinMin)
            printf("  (min driver %u.%u)\n", ssdMinMaj, ssdMinMin);
    } else {
        NVSDK_NGX_Result why = (NVSDK_NGX_Result)ssdInitResultRaw;
        if (!NVSDK_NGX_SUCCEED(initRes)) why = initRes; // init itself failed
        printf("DLSS-RR UNAVAILABLE (result=0x%08X, meaning=%s)\n",
               (unsigned)why, ngxResultName(why));
        printf("  selected GPU: %s\n", chosenProps.deviceName);
        if (ssdNeedsDriver)
            printf("  NeedsUpdatedDriver=1 -> require driver >= %u.%u\n", ssdMinMaj, ssdMinMin);
    }
    printf("================================================================\n");

    // -----------------------------------------------------------------------
    // 9. Cleanup
    // -----------------------------------------------------------------------
    if (NVSDK_NGX_SUCCEED(initRes)) NVSDK_NGX_VULKAN_Shutdown1(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    // Exit code: 0 if available, 1 if unavailable (both are "successful runs").
    return (gotCaps && ssdAvailable) ? 0 : 1;
}
