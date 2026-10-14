#include "target_detection.hpp"
#include "color_targets.hpp"
#include "logger.hpp"
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace vanta
{
    const char* RejectionName(const ContourRejection reason) noexcept
    {
        constexpr std::array names{"degenerate", "area", "width", "height",
            "aspect", "solidity", "flat", "orb"};
        const auto index = static_cast<std::size_t>(reason);
        return index < names.size() ? names[index] : "unknown";
    }

    TargetDetectionConfig SanitizeDetectionConfig(TargetDetectionConfig c)
    {
        const auto bounded = [](double x, double low, double high, double fallback)
        { return std::clamp(std::isfinite(x) ? x : fallback, low, high); };
        c.referenceWidth = std::clamp(c.referenceWidth, 16, 16384);
        c.referenceHeight = std::clamp(c.referenceHeight, 16, 16384);
        c.minimumArea = bounded(c.minimumArea, 0.0, 1e8, 5.0);
        c.maximumArea = bounded(c.maximumArea, c.minimumArea, 1e8, 3500.0);
        c.minimumWidth = bounded(c.minimumWidth, 0.0, 16384.0, 1.0);
        c.maximumWidth = bounded(c.maximumWidth, c.minimumWidth, 16384.0, 160.0);
        c.minimumHeight = bounded(c.minimumHeight, 0.0, 16384.0, 1.0);
        c.maximumHeight = bounded(c.maximumHeight, c.minimumHeight, 16384.0, 240.0);
        c.minimumAspect = bounded(c.minimumAspect, 0.0, 1000.0, 0.0);
        c.maximumAspect = bounded(c.maximumAspect, c.minimumAspect, 1000.0, 100.0);
        c.minimumSolidity = bounded(c.minimumSolidity, 0.0, 1.0, 0.0);
        c.maximumSolidity = bounded(c.maximumSolidity, c.minimumSolidity, 1.0, 1.0);
        c.flatAspectMaximum = bounded(c.flatAspectMaximum, 0.0, 100.0, 0.25);
        c.flatMinimumWidth = bounded(c.flatMinimumWidth, 0.0, 16384.0, 27.0);
        c.orbAspectMinimum = bounded(c.orbAspectMinimum, 0.0, 100.0, 0.70);
        c.orbAspectMaximum = bounded(c.orbAspectMaximum, c.orbAspectMinimum, 100.0, 1.25);
        c.orbMinimumWidth = bounded(c.orbMinimumWidth, 0.0, 16384.0, 20.0);
        c.orbFillMinimum = bounded(c.orbFillMinimum, 0.0, 1.0, 0.65);
        c.orbSolidityMinimum = bounded(c.orbSolidityMinimum, 0.0, 1.0, 0.90);
        c.mergeProximity = bounded(c.mergeProximity, 0.0, 1024.0, 16.0);
        const auto kernel = [](int size) { return std::clamp(size | 1, 1, 7); };
        c.closingKernel = kernel(c.closingKernel);
        c.dilationKernel = kernel(c.dilationKernel);
        c.topRows = std::clamp(c.topRows, 1, 32);
        c.offsetX = bounded(c.offsetX, -4096.0, 4096.0, 0.0);
        c.offsetY = bounded(c.offsetY, -4096.0, 4096.0, 0.0);
        if (c.aimPointMode != AimPointMode::boundingBox && c.aimPointMode != AimPointMode::maskTop)
            c.aimPointMode = AimPointMode::maskTop;
        return c;
    }

    TargetDetectionResult DetectTargets(const cv::Mat& colorMask,
        TargetDetectionConfig config, const DetectionGeometry geometry,
        const int fovRadius, const int targetHeightPercent)
    {
        TargetDetectionResult result;
        if (colorMask.empty() || colorMask.type() != CV_8UC1 ||
            geometry.sourceWidth <= 0 || geometry.sourceHeight <= 0) return result;
        config = SanitizeDetectionConfig(config);
        result.scaleX = static_cast<double>(geometry.sourceWidth) / config.referenceWidth;
        result.scaleY = static_cast<double>(geometry.sourceHeight) / config.referenceHeight;
        const double areaScale = result.scaleX * result.scaleY;
        result.minimumArea = config.minimumArea * areaScale;
        result.maximumArea = config.maximumArea * areaScale;
        result.minimumWidth = config.minimumWidth * result.scaleX;
        result.maximumWidth = config.maximumWidth * result.scaleX;
        result.minimumHeight = config.minimumHeight * result.scaleY;
        result.maximumHeight = config.maximumHeight * result.scaleY;

        cv::Mat circle = cv::Mat::zeros(colorMask.size(), CV_8UC1);
        const cv::Point center(colorMask.cols / 2, colorMask.rows / 2);
        const int radius = std::clamp(fovRadius, 0, std::min(center.x, center.y));
        result.fovRadius=radius;
        cv::circle(circle, center, radius, cv::Scalar(255), cv::FILLED);
        cv::bitwise_and(colorMask, circle, result.mask);
        if (config.closingEnabled)
            cv::morphologyEx(result.mask, result.mask, cv::MORPH_CLOSE,
                cv::getStructuringElement(cv::MORPH_RECT,
                    cv::Size(config.closingKernel, config.closingKernel)));
        cv::dilate(result.mask, result.mask,
            cv::getStructuringElement(cv::MORPH_RECT,
                cv::Size(config.dilationKernel, config.dilationKernel)));
        cv::bitwise_and(result.mask, circle, result.mask);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(result.mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        struct Blob { cv::Rect bounds; std::vector<std::vector<cv::Point>> contours; };
        std::vector<Blob> blobs;
        for (auto& contour : contours)
        {
            const cv::Rect bounds = cv::boundingRect(contour);
            const double area = cv::contourArea(contour);
            std::vector<cv::Point> hull;
            cv::convexHull(contour, hull);
            const double hullArea = cv::contourArea(hull);
            const double solidity = hullArea > 0.0 ? area / hullArea : 0.0;
            const double fill = area / static_cast<double>(bounds.area());
            const double width = bounds.width / result.scaleX;
            const double height = bounds.height / result.scaleY;
            const double aspect = height / width;
            ContourRejection rejection = ContourRejection::count;
            if (hullArea <= 0.0 || area <= 0.0) rejection = ContourRejection::degenerate;
            else if (config.areaFilter && (area < result.minimumArea || area > result.maximumArea))
                rejection = ContourRejection::area;
            else if (config.widthFilter && (width < config.minimumWidth || width > config.maximumWidth))
                rejection = ContourRejection::width;
            else if (config.heightFilter && (height < config.minimumHeight || height > config.maximumHeight))
                rejection = ContourRejection::height;
            else if (config.aspectFilter && (aspect < config.minimumAspect || aspect > config.maximumAspect))
                rejection = ContourRejection::aspect;
            else if (config.solidityFilter && (solidity < config.minimumSolidity || solidity > config.maximumSolidity))
                rejection = ContourRejection::solidity;
            else if (config.flatFilter && width > config.flatMinimumWidth && aspect < config.flatAspectMaximum)
                rejection = ContourRejection::flat;
            else if (config.orbFilter && width > config.orbMinimumWidth &&
                aspect >= config.orbAspectMinimum && aspect <= config.orbAspectMaximum &&
                fill >= config.orbFillMinimum && solidity >= config.orbSolidityMinimum)
                rejection = ContourRejection::orb;
            if (rejection != ContourRejection::count)
            {
                ++result.rejectionCounts[static_cast<std::size_t>(rejection)];
                result.rejected.push_back({bounds, rejection, area, solidity, fill});
            }
            else
            {
                ++result.acceptedContours;
                result.accepted.push_back({bounds, ContourRejection::count, area, solidity, fill});
                blobs.push_back({bounds, {std::move(contour)}});
            }
        }

        // Match stable Vanta's iterative rectangle union, retaining members.
        const double proximityX = config.mergeProximity * result.scaleX;
        const double proximityY = config.mergeProximity * result.scaleY;
        bool merged = true;
        while (merged)
        {
            merged = false;
            for (std::size_t i = 0; i < blobs.size() && !merged; ++i)
                for (std::size_t j = i + 1; j < blobs.size(); ++j)
                {
                    const auto a = blobs[i].bounds;
                    const auto b = blobs[j].bounds;
                    if (a.x + a.width < b.x - proximityX || b.x + b.width < a.x - proximityX ||
                        a.y + a.height < b.y - proximityY || b.y + b.height < a.y - proximityY) continue;
                    blobs[i].bounds = a | b;
                    auto& members = blobs[i].contours;
                    members.insert(members.end(),
                        std::make_move_iterator(blobs[j].contours.begin()),
                        std::make_move_iterator(blobs[j].contours.end()));
                    blobs.erase(blobs.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
        }

        for (const auto& blob : blobs)
        {
            cv::Point2d aim(blob.bounds.x + blob.bounds.width / 2.0,
                blob.bounds.y + blob.bounds.height * std::clamp(targetHeightPercent, 0, 100) / 100.0);
            if (config.aimPointMode == AimPointMode::maskTop)
            {
                cv::Mat filled = cv::Mat::zeros(blob.bounds.size(), CV_8UC1);
                cv::drawContours(filled, blob.contours, -1, cv::Scalar(255), cv::FILLED,
                    cv::LINE_8, cv::noArray(), std::numeric_limits<int>::max(),
                    cv::Point(-blob.bounds.x, -blob.bounds.y));
                std::int64_t sumX = 0;
                std::size_t count = 0;
                int occupied = 0;
                for (int y = 0; y < filled.rows && occupied < config.topRows; ++y)
                {
                    const auto* row = filled.ptr<std::uint8_t>(y);
                    bool hasPixels = false;
                    for (int x = 0; x < filled.cols; ++x)
                        if (row[x]) { sumX += x; ++count; hasPixels = true; }
                    if (hasPixels)
                    {
                        if (occupied == 0) aim.y = blob.bounds.y + y;
                        ++occupied;
                    }
                }
                if (count == 0) continue;
                aim.x = blob.bounds.x + static_cast<double>(sumX) / static_cast<double>(count);
            }
            aim.x += config.offsetX * result.scaleX;
            aim.y += config.offsetY * result.scaleY;
            if (aim.x < 0 || aim.x >= colorMask.cols || aim.y < 0 || aim.y >= colorMask.rows) continue;
            result.targets.push_back({blob.bounds, aim});
        }
        return result;
    }

    bool RunTargetDetectionSelfTest()
    {
        const auto fail = [](const char* why) { log::Error("detector self-test: %s", why); return false; };
        cv::Mat hsv(1, 6, CV_8UC3);
        const std::array<unsigned char, 6> hues{0, 9, 179, 30, 147, 90};
        for (int x = 0; x < hsv.cols; ++x) hsv.at<cv::Vec3b>(0,x) = {hues[x],255,255};
        cv::Mat mask;
        BuildHsvMask(hsv, 3, mask);
        if (cv::countNonZero(mask) != 3 || !mask.at<unsigned char>(0,2)) return fail("red hue wrap");
        BuildHsvMask(hsv, 2, mask);
        if (cv::countNonZero(mask) != 1 || !mask.at<unsigned char>(0,3)) return fail("yellow isolation");
        for (int preset = 0; preset < 2; ++preset)
        {
            cv::Mat expected;
            const auto& range = kHsvColorTargets[static_cast<std::size_t>(preset)].ranges[0];
            cv::inRange(hsv, cv::Scalar(range.lower[0],range.lower[1],range.lower[2]),
                cv::Scalar(range.upper[0],range.upper[1],range.upper[2]), expected);
            BuildHsvMask(hsv, preset, mask);
            if (cv::countNonZero(mask != expected) != 0) return fail("legacy purple changed");
        }
        TargetDetectionConfig c;
        c.closingEnabled = false; c.dilationKernel = 1;
        c.offsetX = c.offsetY = 0;
        const DetectionGeometry geometry{2560,1440,0,0};
        const auto rectangle = [](cv::Rect r) {
            cv::Mat image = cv::Mat::zeros(240,240,CV_8UC1);
            cv::rectangle(image,r,cv::Scalar(255),cv::FILLED); return image; };
        const auto rejects = [&](TargetDetectionConfig cfg, cv::Rect r, ContourRejection reason) {
            const auto d = DetectTargets(rectangle(r),cfg,geometry,115,15);
            return d.targets.empty() && d.rejectionCounts[static_cast<std::size_t>(reason)] == 1; };
        if (!rejects(c,{118,110,1,1},ContourRejection::degenerate)) return fail("degenerate");
        auto rule = c; rule.minimumArea = 100;
        if (!rejects(rule,{110,110,4,12},ContourRejection::area)) return fail("min area");
        rule = c; rule.maximumArea = 100;
        if (!rejects(rule,{110,90,12,40},ContourRejection::area)) return fail("max area");
        rule = c; rule.minimumWidth = 12;
        if (!rejects(rule,{110,90,8,40},ContourRejection::width)) return fail("min width");
        rule = c; rule.maximumWidth = 6;
        if (!rejects(rule,{110,90,8,40},ContourRejection::width)) return fail("max width");
        rule = c; rule.minimumHeight = 50;
        if (!rejects(rule,{110,90,8,40},ContourRejection::height)) return fail("min height");
        rule = c; rule.maximumHeight = 30;
        if (!rejects(rule,{110,90,8,40},ContourRejection::height)) return fail("max height");
        rule = c; rule.maximumAspect = 2;
        if (!rejects(rule,{110,90,8,40},ContourRejection::aspect)) return fail("aspect");
        rule = c; rule.minimumAspect=6;
        if (!rejects(rule,{110,90,8,40},ContourRejection::aspect)) return fail("minimum aspect");
        rule = c; rule.maximumSolidity = 0.90;
        if (!rejects(rule,{110,90,8,40},ContourRejection::solidity)) return fail("true solidity");
        if (!rejects(c,{90,110,50,5},ContourRejection::flat) ||
            !rejects(c,{105,105,30,30},ContourRejection::orb)) return fail("flat/orb");
        cv::Mat irregular = cv::Mat::zeros(240,240,CV_8UC1);
        cv::rectangle(irregular,{90,80,8,12},cv::Scalar(255),cv::FILLED);
        cv::rectangle(irregular,{90,91,40,42},cv::Scalar(255),cv::FILLED);
        auto d = DetectTargets(irregular,c,geometry,115,50);
        if (d.targets.size() != 1 || std::abs(d.targets[0].aim.x - 93.5) > .01 || d.targets[0].aim.y != 80)
            return fail("mask silhouette");
        c.aimPointMode = AimPointMode::boundingBox; c.offsetX = 2; c.offsetY = -3;
        d = DetectTargets(irregular,c,geometry,115,50);
        if (d.targets.size() != 1 || d.targets[0].aim.x != 112 || d.targets[0].aim.y != 103.5)
            return fail("bbox and offsets");
        c.aimPointMode = AimPointMode::maskTop; c.offsetX = c.offsetY = 0;
        auto fragments = rectangle({85,80,8,40});
        cv::rectangle(fragments,{102,82,8,40},cv::Scalar(255),cv::FILLED);
        d = DetectTargets(fragments,c,geometry,115,15);
        if (d.targets.size() != 1 || d.acceptedContours != 2) return fail("merge members");
        c.mergeProximity = 0;
        if (DetectTargets(fragments,c,geometry,115,15).targets.size() != 2) return fail("merge off");
        for (const int kernel : {1,3,5,7})
        {
            c.closingEnabled = true; c.closingKernel = kernel; c.dilationKernel = kernel;
            if (DetectTargets(rectangle({110,80,8,40}),c,geometry,115,15).targets.size() != 1)
                return fail("kernel options");
        }
        c.closingEnabled = false; c.dilationKernel = 1;
        cv::Mat full = cv::Mat::zeros(640,640,CV_8UC1);
        cv::rectangle(full,{310,290,8,40},cv::Scalar(255),cv::FILLED);
        const auto large = DetectTargets(full,c,geometry,55,15);
        const auto small = DetectTargets(full(cv::Rect(260,260,120,120)),c,{2560,1440,260,260},55,15);
        if (large.targets.size()!=1 || small.targets.size()!=1 ||
            large.targets[0].bounds.size()!=small.targets[0].bounds.size() ||
            std::abs(large.targets[0].aim.x-small.targets[0].aim.x-260)>0.01)
            return fail("ROI crop changed scale");
        const auto lower = DetectTargets(full,c,{1920,1080,0,0},55,15);
        if (lower.scaleX != .75 || lower.scaleY != .75 ||
            lower.maximumArea != c.maximumArea*.5625) return fail("decoded resolution scale");
        cv::Mat scaledFull;
        cv::resize(full,scaledFull,cv::Size(480,480),0,0,cv::INTER_NEAREST);
        const auto scaled=DetectTargets(scaledFull,c,{1920,1080,0,0},55,15);
        if (scaled.targets.size()!=1 || scaled.targets[0].bounds.width!=6 ||
            scaled.targets[0].bounds.height!=30) return fail("1080p scaled target");
        cv::Mat gap=rectangle({105,80,12,40});
        gap(cv::Rect(105,98,12,1)).setTo(0);
        c.mergeProximity=0;
        const auto unclosed=DetectTargets(gap,c,geometry,115,15);
        c.closingEnabled=true; c.closingKernel=3;
        const auto closed=DetectTargets(gap,c,geometry,115,15);
        if (unclosed.acceptedContours!=2 || closed.acceptedContours!=1)
            return fail("closing did not reconnect fragmented contour");
        c.closingEnabled=false;
        cv::Mat concave=cv::Mat::zeros(240,240,CV_8UC1);
        cv::rectangle(concave,{90,90,8,38},cv::Scalar(255),cv::FILLED);
        cv::rectangle(concave,{90,120,38,8},cv::Scalar(255),cv::FILLED);
        rule=c; rule.minimumSolidity=.90;
        if (DetectTargets(concave,rule,geometry,115,15).rejectionCounts[
            static_cast<std::size_t>(ContourRejection::solidity)]!=1)
            return fail("minimum convex-hull solidity");
        if (DetectTargets(concave,c,geometry,115,15).targets.size()!=1)
            return fail("concave square silhouette incorrectly rejected as orb");
        c.orbFilter = false;
        if (DetectTargets(rectangle({105,105,30,30}),c,geometry,115,15).targets.size()!=1)
            return fail("orb toggle");
        log::Info("detector self-test passed: color wrap, filters, masks, kernels, merge, offsets, crop and resolution");
        return true;
    }
}
