#include "ldc_config.h"

#include <nlohmann/json.hpp>

#include <fstream>

bool load_app_ldc_config(const std::string &path, AppLdcConfig &config,
                         std::string &error)
{
    try
    {
        std::ifstream input(path.c_str());
        if (!input)
        {
            error = "cannot open " + path;
            return false;
        }
        const nlohmann::json root = nlohmann::json::parse(input);
        const nlohmann::json &size = root.at("image_size");
        const nlohmann::json &ldc = root.at("sophgo_vpss_ldc");
        if (!size.is_array() || size.size() != 2)
        {
            error = "image_size must be [width, height]";
            return false;
        }

        AppLdcConfig parsed;
        parsed.enabled = true;
        parsed.calibration_width = size.at(0).get<CVI_U32>();
        parsed.calibration_height = size.at(1).get<CVI_U32>();
        parsed.ratio = ldc.at("ratio").get<CVI_S32>();
        parsed.center_x = ldc.at("center_x").get<CVI_S32>();
        parsed.center_y = ldc.at("center_y").get<CVI_S32>();
        parsed.view_ratio = ldc.at("view_ratio").get<CVI_S32>();
        const std::string::size_type slash = path.find_last_of('/');
        parsed.cache_dir = slash == std::string::npos ? "." :
                           slash == 0 ? "/" : path.substr(0, slash);
        if (!parsed.calibration_width || !parsed.calibration_height)
        {
            error = "image_size dimensions must be positive";
            return false;
        }
        if (parsed.ratio < -300 || parsed.ratio > 500 ||
            parsed.center_x < -511 || parsed.center_x > 511 ||
            parsed.center_y < -511 || parsed.center_y > 511 ||
            parsed.view_ratio < 0 || parsed.view_ratio > 100)
        {
            error = "LDC values exceed SG2000 hardware ranges";
            return false;
        }
        config = parsed;
        return true;
    }
    catch (const std::exception &e)
    {
        error = std::string("invalid calibration JSON: ") + e.what();
        return false;
    }
}
