#ifndef DUO_APPS_COMMON_LDC_CONFIG_H
#define DUO_APPS_COMMON_LDC_CONFIG_H

#include <cvi_vpss.h>
#include <cvi_gdc.h>

#include <cstdio>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

// Generate or load the LDC mesh once at startup; VPSS/DWA applies it to each
// frame. A zero ratio means no correction is requested.
struct AppLdcConfig
{
    bool enabled = false;
    CVI_S32 ratio = 0;
    CVI_S32 center_x = 0;
    CVI_S32 center_y = 0;
    CVI_S32 view_ratio = 100;
    CVI_U32 calibration_width = 0;
    CVI_U32 calibration_height = 0;
    std::string cache_dir;
};

bool load_app_ldc_config(const std::string &path, AppLdcConfig &config,
                         std::string &error);

// The SDK's raw mesh format is specific to this GDC implementation. Include
// an explicit format revision and every output-space LDC parameter in the
// cache key; a changed calibration automatically gets a different file.
inline std::string app_ldc_cache_path(const AppLdcConfig &config,
                                      CVI_U32 width, CVI_U32 height)
{
    char signature[160];
    std::snprintf(signature, sizeof(signature),
                  "cv181x-mesh-v1:%u:%u:%d:%d:%d:%d",
                  width, height, config.ratio, config.center_x,
                  config.center_y, config.view_ratio);
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(signature); *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    char filename[64];
    std::snprintf(filename, sizeof(filename), ".sg2000-ldc-%016llx.mesh",
                  static_cast<unsigned long long>(hash));
    return config.cache_dir + "/" + filename;
}

inline bool apply_app_ldc(VPSS_GRP group, VPSS_CHN channel,
                          CVI_U32 width, CVI_U32 height,
                          AppLdcConfig config)
{
    if (!config.enabled)
        return true;

    const double aspect_in = static_cast<double>(config.calibration_width) /
                             config.calibration_height;
    const double aspect_out = static_cast<double>(width) / height;
    if (std::fabs(aspect_in - aspect_out) > 0.005 * aspect_out)
    {
        std::fprintf(stderr,
                     "[ldc] calibration aspect %.5f does not match VPSS output %ux%u; regenerate calibration at matching aspect\n",
                     aspect_in, width, height);
        return false;
    }
    // The LDC mesh runs on the detector output. Scale the optical-center
    // offsets when calibration came from a larger same-aspect image; the
    // radial ratio is dimensionless under this resize.
    config.center_x = static_cast<CVI_S32>(std::lround(
        config.center_x * static_cast<double>(width) / config.calibration_width));
    config.center_y = static_cast<CVI_S32>(std::lround(
        config.center_y * static_cast<double>(height) / config.calibration_height));
    if (config.center_x < -511 || config.center_x > 511 ||
        config.center_y < -511 || config.center_y > 511)
    {
        std::fprintf(stderr, "[ldc] scaled center offset exceeds SG2000 hardware range\n");
        return false;
    }

    VPSS_LDC_ATTR_S attr{};
    attr.bEnable = CVI_TRUE;
    attr.stAttr.bAspect = CVI_TRUE;
    attr.stAttr.s32XYRatio = config.view_ratio;
    attr.stAttr.s32XRatio = config.view_ratio;
    attr.stAttr.s32YRatio = config.view_ratio;
    attr.stAttr.s32CenterXOffset = config.center_x;
    attr.stAttr.s32CenterYOffset = config.center_y;
    attr.stAttr.s32DistortionRatio = config.ratio;

    const std::string cache_path = app_ldc_cache_path(config, width, height);
    MESH_DUMP_ATTR_S mesh_file{};
    mesh_file.enModId = CVI_ID_VPSS;
    mesh_file.vpssMeshAttr.grp = group;
    mesh_file.vpssMeshAttr.chn = channel;
    const bool cache_path_fits = cache_path.size() < sizeof(mesh_file.binFileName) - 8;
    const auto start = std::chrono::steady_clock::now();
    if (cache_path_fits && access(cache_path.c_str(), R_OK) == 0)
    {
        std::snprintf(mesh_file.binFileName, sizeof(mesh_file.binFileName),
                      "%s", cache_path.c_str());
        if (CVI_GDC_LoadMesh(&mesh_file, &attr.stAttr) == CVI_SUCCESS)
        {
            const double load_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            std::fprintf(stderr, "[ldc] loaded VPSS group %d channel %d (%ux%u) mesh in %.1f ms from %s\n",
                         group, channel, width, height, load_ms, cache_path.c_str());
            return true;
        }
        std::fprintf(stderr, "[ldc] cached mesh rejected; regenerating %s\n",
                     cache_path.c_str());
    }

    std::fprintf(stderr, "[ldc] generating mesh for VPSS group %d channel %d (%ux%u)\n",
                 group, channel, width, height);
    const CVI_S32 rc = CVI_VPSS_SetChnLDCAttr(group, channel, &attr);
    const double setup_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    if (rc != CVI_SUCCESS)
    {
        std::fprintf(stderr, "[ldc] VPSS group %d channel %d (%ux%u) setup failed after %.1f ms: %#x\n",
                     group, channel, width, height, setup_ms, rc);
        return false;
    }
    std::fprintf(stderr,
                 "[ldc] enabled on VPSS group %d channel %d (%ux%u) after %.1f ms: ratio=%d center=(%d,%d) view=%d%%\n",
                 group, channel, width, height, setup_ms, config.ratio,
                 config.center_x, config.center_y, config.view_ratio);

    if (cache_path_fits)
    {
        std::string temp_path = cache_path + ".XXXXXX";
        int fd = mkstemp(&temp_path[0]);
        if (fd >= 0)
        {
            close(fd);
            std::snprintf(mesh_file.binFileName, sizeof(mesh_file.binFileName),
                          "%s", temp_path.c_str());
            struct stat mesh_stat{};
            if (CVI_GDC_DumpMesh(&mesh_file) == CVI_SUCCESS &&
                stat(temp_path.c_str(), &mesh_stat) == 0 && mesh_stat.st_size > 0 &&
                rename(temp_path.c_str(), cache_path.c_str()) == 0)
                std::fprintf(stderr, "[ldc] cached mesh at %s (%lld bytes)\n",
                             cache_path.c_str(), static_cast<long long>(mesh_stat.st_size));
            else
            {
                unlink(temp_path.c_str());
                std::fprintf(stderr, "[ldc] mesh cache write skipped: %s\n", cache_path.c_str());
            }
        }
        else
            std::fprintf(stderr, "[ldc] mesh cache unavailable: %s\n", cache_path.c_str());
    }
    else
        std::fprintf(stderr, "[ldc] mesh cache path too long; regeneration will be needed at next start\n");
    return true;
}

#endif
