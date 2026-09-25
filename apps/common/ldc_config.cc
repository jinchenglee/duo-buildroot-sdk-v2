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
        if (root.find("camera_matrix") != root.end() &&
            root.find("distortion_coefficients") != root.end())
        {
            const nlohmann::json &camera = root.at("camera_matrix");
            if (!camera.is_array() || camera.size() != 3)
            {
                error = "camera_matrix must be 3x3";
                return false;
            }
            for (int r = 0; r < 3; ++r)
            {
                if (!camera.at(r).is_array() || camera.at(r).size() != 3)
                {
                    error = "camera_matrix must be 3x3";
                    return false;
                }
                for (int c = 0; c < 3; ++c)
                    parsed.camera_matrix[r * 3 + c] = camera.at(r).at(c).get<double>();
            }
            parsed.distortion = root.at("distortion_coefficients").get<std::vector<double>>();
            const size_t n = parsed.distortion.size();
            if (n != 4 && n != 5 && n != 8 && n != 12 && n != 14)
            {
                error = "distortion_coefficients must have 4, 5, 8, 12 or 14 values";
                return false;
            }
            parsed.has_opencv_model = true;
        }
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
