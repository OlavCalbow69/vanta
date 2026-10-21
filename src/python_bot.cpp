#include "python_bot.hpp"
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

namespace vanta
{
    std::optional<MouseButtonBinding> PythonAimButton(const std::string& key)
    {
        std::string_view value(key);
        while (!value.empty() && value.front()==' ') value.remove_prefix(1);
        while (!value.empty() && value.back()==' ') value.remove_suffix(1);
        if (value.starts_with("0x") || value.starts_with("0X")) value.remove_prefix(2);
        unsigned int code{};
        const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),code,16);
        if (error!=std::errc{} || end!=value.data()+value.size()) return std::nullopt;
        switch (code)
        {
        case 0x01: return MouseButtonBinding::left;
        case 0x02: return MouseButtonBinding::right;
        case 0x04: return MouseButtonBinding::middle;
        case 0x05: return MouseButtonBinding::x1;
        case 0x06: return MouseButtonBinding::x2;
        default: return std::nullopt;
        }
    }

    const char* PythonAxisModeJsonName(PythonAxisMode mode) noexcept
    {
        switch (mode)
        {
        case PythonAxisMode::horizontalOnly: return "horizontal_only";
        case PythonAxisMode::hybrid: return "hybrid";
        case PythonAxisMode::standard: return "standard";
        }
        return "standard";
    }

    bool ValidatePythonBotConfig(const PythonBotConfig& config,std::string& error)
    {
        const auto fail=[&](const char* message) { error=message; return false; };
        if (config.fovRadius<1 || config.fovRadius>2048) return fail("fov_radius must be 1..2048 decoded pixels");
        if (!std::isfinite(config.smoothFactor) || std::abs(config.smoothFactor)>100.0)
            return fail("smooth_factor must be finite and within -100..100");
        switch (config.axisMode)
        {
        case PythonAxisMode::standard:
        case PythonAxisMode::horizontalOnly:
        case PythonAxisMode::hybrid:
            break;
        default:
            return fail("axis_mode is not supported");
        }
        if (config.hybridVerticalTimeMilliseconds<1 || config.hybridVerticalTimeMilliseconds>10000)
            return fail("hybrid_vertical_time_ms must be 1..10000");
        if (!std::isfinite(config.minimumArea) || config.minimumArea<0.0 || config.minimumArea>1e8)
            return fail("min_contour_area must be finite and nonnegative");
        if (std::abs(static_cast<std::int64_t>(config.offsetX))>100000 ||
            std::abs(static_cast<std::int64_t>(config.offsetY))>100000)
            return fail("offsets exceed the safe integer range");
        const auto preset=std::find_if(config.presets.begin(),config.presets.end(),
            [&](const auto& entry) { return entry.key==config.activePreset; });
        if (preset==config.presets.end()) return fail("active_preset is missing from presets");
        for (const auto& entry:config.presets)
        {
            if (entry.ranges.size()!=(entry.key=="red" ? 2U : 1U))
                return fail("red needs two HSV ranges; every other preset needs one");
            for (const auto& range:entry.ranges)
                for (std::size_t channel=0;channel<3;++channel)
                    if (range.lower[channel]<0 || range.upper[channel]>255 ||
                        range.lower[channel]>range.upper[channel]) return fail("invalid HSV range");
        }
        error.clear();
        return true;
    }

    PythonBotResult ProcessPythonBotFrame(const cv::Mat& sourceBgra,const PythonBotConfig& config)
    {
        PythonBotResult result;
        if (!ValidatePythonBotConfig(config,result.error)) return result;
        const int radius=config.fovRadius;
        const int side=radius*2;
        if (sourceBgra.empty() || sourceBgra.type()!=CV_8UC4 ||
            sourceBgra.cols<side || sourceBgra.rows<side)
        {
            result.error="Sunshine must supply a BGRA frame at least 2*fov_radius on both axes";
            return result;
        }
        const cv::Rect roi{sourceBgra.cols/2-radius,sourceBgra.rows/2-radius,side,side};
        return ProcessPythonBotRoi(sourceBgra(roi),config,roi);
    }

    PythonBotResult ProcessPythonBotRoi(const cv::Mat& cropBgra,const PythonBotConfig& config,const cv::Rect& sourceRoi)
    {
        PythonBotResult result;
        if (!ValidatePythonBotConfig(config,result.error)) return result;
        const int radius=config.fovRadius,side=radius*2;
        if (cropBgra.empty() || cropBgra.type()!=CV_8UC4 || cropBgra.cols!=side || cropBgra.rows!=side || sourceRoi.size()!=cropBgra.size())
        { result.error="Sunshine crop must contain exactly 2*fov_radius native pixels on both axes"; return result; }
        result.sourceRoi=sourceRoi;
        cv::Mat bgr,hsv;
        cv::cvtColor(cropBgra,bgr,cv::COLOR_BGRA2BGR);
        cv::cvtColor(bgr,hsv,cv::COLOR_BGR2HSV);
        const auto preset=std::find_if(config.presets.begin(),config.presets.end(),
            [&](const auto& entry) { return entry.key==config.activePreset; });
        for (const auto& range:preset->ranges)
        {
            cv::Mat selected;
            cv::inRange(hsv,cv::Scalar(range.lower[0],range.lower[1],range.lower[2]),
                cv::Scalar(range.upper[0],range.upper[1],range.upper[2]),selected);
            if (result.colorMask.empty()) result.colorMask=selected;
            else cv::bitwise_or(result.colorMask,selected,result.colorMask);
        }
        const auto kernel=cv::Mat::ones(3,3,CV_8UC1);
        cv::morphologyEx(result.colorMask,result.colorMask,cv::MORPH_CLOSE,kernel);
        cv::dilate(result.colorMask,result.colorMask,kernel,cv::Point(-1,-1),1);
        result.fovMask=cv::Mat::zeros(side,side,CV_8UC1);
        for (int y=0;y<side;++y)
            for (int x=0;x<side;++x)
            {
                const std::int64_t dx=x-radius,dy=y-radius;
                if (dx*dx+dy*dy<=static_cast<std::int64_t>(radius)*radius)
                    result.fovMask.at<std::uint8_t>(y,x)=result.colorMask.at<std::uint8_t>(y,x);
            }
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(result.fovMask,contours,cv::RETR_EXTERNAL,cv::CHAIN_APPROX_SIMPLE);
        for (const auto& contour:contours)
        {
            const double area=cv::contourArea(contour);
            if (area<config.minimumArea || area>2000.0) { ++result.rejections[0]; continue; }
            const auto bounds=cv::boundingRect(contour);
            if (bounds.width>120 || bounds.height>180) { ++result.rejections[1]; continue; }
            const double aspect=static_cast<double>(bounds.height)/bounds.width;
            const double fill=area/static_cast<double>(bounds.width*bounds.height);
            if (aspect<0.25 && bounds.width>20) { ++result.rejections[2]; continue; }
            // The reference stores filter_orbs but never reads it here.
            if (aspect>=0.70 && aspect<=1.25 && fill>0.65 && bounds.width>15)
            { ++result.rejections[3]; continue; }
            bool merged=false;
            for (auto& blob:result.blobs)
            {
                if (!(bounds.x+bounds.width<blob.bounds.x-12 ||
                      bounds.x>blob.bounds.x+blob.bounds.width+12))
                {
                    blob.contours.push_back(contour);
                    blob.bounds|=bounds;
                    merged=true;
                    break; // Greedy first horizontal match; no Y test or second pass.
                }
            }
            if (!merged) result.blobs.push_back({bounds,{contour}});
        }
        double nearest=std::numeric_limits<double>::infinity();
        for (const auto& blob:result.blobs)
        {
            result.validContours.insert(result.validContours.end(),blob.contours.begin(),blob.contours.end());
            cv::Mat filled=cv::Mat::zeros(side,side,CV_8UC1);
            cv::drawContours(filled,blob.contours,-1,cv::Scalar(255),cv::FILLED);
            std::vector<cv::Point> points;
            cv::findNonZero(filled,points);
            if (points.empty()) continue;
            const int minimumY=std::min_element(points.begin(),points.end(),
                [](const auto& a,const auto& b) { return a.y<b.y; })->y;
            std::int64_t sumX=0,count=0;
            for (const auto& point:points)
                if (point.y<=minimumY+3) { sumX+=point.x; ++count; }
            const int headX=static_cast<int>(static_cast<double>(sumX)/static_cast<double>(count)+config.offsetX);
            const int headY=minimumY+config.offsetY;
            const double distance=std::hypot(static_cast<double>(headX-radius),static_cast<double>(headY-radius));
            if (distance<nearest)
            {
                nearest=distance;
                result.target=cv::Point(headX,headY);
            }
        }
        if (result.target)
        {
            // Python int(float) truncates toward zero, never lround().
            result.movement={static_cast<int>((result.target->x-radius)*config.smoothFactor),
                static_cast<int>((result.target->y-radius)*config.smoothFactor)};
            // Hybrid's timed Y gate belongs to the controller because it
            // depends on physical activation state. The pure detector keeps
            // the reference movement calculation intact for that mode.
            if (config.axisMode==PythonAxisMode::horizontalOnly) result.movement.y=0;
        }
        result.valid=true;
        return result;
    }
}
