#include "target_detection_ui.hpp"
#include "logger.hpp"
#include <opencv2/imgproc.hpp>
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace vanta
{
    bool RenderDetectionControls(TargetDetectionConfig& c)
    {
        bool changed = ImGui::Checkbox("Enhanced contour detector", &c.enabled);
        ImGui::SameLine();
        if (ImGui::Button("Apply 1440p preset")) { c = {}; changed = true; }
        if (!c.enabled)
        {
            ImGui::TextDisabled("Legacy detector: original bounding-box aim and 4x4 dilation.");
            return changed;
        }
        if (!ImGui::CollapsingHeader("Contour filters and aim point", ImGuiTreeNodeFlags_DefaultOpen)) return changed;
        ImGui::TextWrapped("Sizes and offsets use the reference resolution; the ROI is a crop. Kernel sizes are decoded pixels.");
        changed |= ImGui::InputInt("Reference decoded width", &c.referenceWidth);
        changed |= ImGui::InputInt("Reference decoded height", &c.referenceHeight);
        const auto pair = [&](const char* label, bool& enabled, double& minimum, double& maximum)
        {
            ImGui::PushID(label);
            changed |= ImGui::Checkbox(label, &enabled);
            if (enabled)
            {
                changed |= ImGui::InputDouble("Minimum", &minimum, 0, 0, "%.3f");
                changed |= ImGui::InputDouble("Maximum", &maximum, 0, 0, "%.3f");
            }
            ImGui::PopID();
        };
        pair("Area (reference px squared)", c.areaFilter,c.minimumArea,c.maximumArea);
        pair("Width (reference px)", c.widthFilter,c.minimumWidth,c.maximumWidth);
        pair("Height (reference px)", c.heightFilter,c.minimumHeight,c.maximumHeight);
        pair("Aspect (height / width)", c.aspectFilter,c.minimumAspect,c.maximumAspect);
        pair("Solidity (area / convex hull)", c.solidityFilter,c.minimumSolidity,c.maximumSolidity);
        changed |= ImGui::Checkbox("Reject flat contours", &c.flatFilter);
        if (c.flatFilter)
        {
            changed |= ImGui::InputDouble("Flat maximum height/width", &c.flatAspectMaximum);
            changed |= ImGui::InputDouble("Flat minimum width", &c.flatMinimumWidth);
        }
        changed |= ImGui::Checkbox("Reject solid square/orb shapes", &c.orbFilter);
        if (c.orbFilter)
        {
            changed |= ImGui::InputDouble("Orb minimum height/width", &c.orbAspectMinimum);
            changed |= ImGui::InputDouble("Orb maximum height/width", &c.orbAspectMaximum);
            changed |= ImGui::InputDouble("Orb minimum width", &c.orbMinimumWidth);
            changed |= ImGui::InputDouble("Orb minimum bounding-box fill", &c.orbFillMinimum);
            changed |= ImGui::InputDouble("Orb minimum true solidity", &c.orbSolidityMinimum);
        }
        changed |= ImGui::Checkbox("Morphological closing", &c.closingEnabled);
        const auto kernel = [&](const char* label, int& value)
        {
            int selected = (value - 1) / 2;
            if (ImGui::Combo(label, &selected, "1x1\0" "3x3\0" "5x5\0" "7x7\0"))
            { value = selected * 2 + 1; changed = true; }
        };
        if (c.closingEnabled) kernel("Closing kernel (decoded px)", c.closingKernel);
        kernel("Dilation kernel (decoded px)", c.dilationKernel);
        changed |= ImGui::InputDouble("Merge proximity (reference px)", &c.mergeProximity);
        int mode = static_cast<int>(c.aimPointMode);
        if (ImGui::Combo("Aim point", &mode, "Bounding-box percentage\0Mask top\0"))
        { c.aimPointMode = static_cast<AimPointMode>(mode); changed = true; }
        if (c.aimPointMode == AimPointMode::maskTop)
            changed |= ImGui::InputInt("Top occupied rows", &c.topRows);
        changed |= ImGui::InputDouble("Aim offset X (reference px)", &c.offsetX, .1, 1, "%.3f");
        changed |= ImGui::InputDouble("Aim offset Y (reference px)", &c.offsetY, .1, 1, "%.3f");
        if (changed) c = SanitizeDetectionConfig(c);
        return changed;
    }

    void RenderDetectionPreview(const TargetDetectionResult& d, const bool ignoreBelowCrosshair)
    {
        if (d.mask.empty()) { ImGui::TextDisabled("Waiting for a captured frame."); return; }
        ImGui::Text("Scale X/Y %.3f/%.3f | contours %zu accepted, %zu rejected | blobs %zu",
            d.scaleX,d.scaleY,d.acceptedContours,d.rejected.size(),d.targets.size());
        ImGui::Text("Effective area %.1f..%.1f px2",d.minimumArea,d.maximumArea);
        ImGui::Text("Effective width %.1f..%.1f px | height %.1f..%.1f px",
            d.minimumWidth,d.maximumWidth,d.minimumHeight,d.maximumHeight);
        for (std::size_t i=0;i<d.rejectionCounts.size();++i)
            if (d.rejectionCounts[i])
                ImGui::Text("Rejected %s: %zu", RejectionName(static_cast<ContourRejection>(i)),d.rejectionCounts[i]);
        if (ImGui::CollapsingHeader("Contour measurements (decoded pixels)") &&
            ImGui::BeginTable("contour-measurements",5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0,150)))
        {
            ImGui::TableSetupColumn("Result");
            ImGui::TableSetupColumn("W x H");
            ImGui::TableSetupColumn("Area");
            ImGui::TableSetupColumn("Box fill");
            ImGui::TableSetupColumn("True solidity");
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(d.accepted.size()+d.rejected.size()));
            while (clipper.Step())
                for (int index=clipper.DisplayStart;index<clipper.DisplayEnd;++index)
                {
                    const auto offset=static_cast<std::size_t>(index);
                    const bool accepted=offset<d.accepted.size();
                    const auto& contour=accepted ? d.accepted[offset] : d.rejected[offset-d.accepted.size()];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(accepted ? "Accepted" : RejectionName(contour.reason));
                    ImGui::TableNextColumn(); ImGui::Text("%d x %d",contour.bounds.width,contour.bounds.height);
                    ImGui::TableNextColumn(); ImGui::Text("%.1f",contour.area);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f",contour.fill);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f",contour.solidity);
                }
            ImGui::EndTable();
        }
        const float side = std::min(360.0F, ImGui::GetContentRegionAvail().x);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float sx = side / static_cast<float>(d.mask.cols);
        const float sy = side / static_cast<float>(d.mask.rows);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(start,ImVec2(start.x+side,start.y+side),IM_COL32(12,14,22,255));
        for (int y=0;y<d.mask.rows;y+=2)
        {
            const auto* row=d.mask.ptr<std::uint8_t>(y);
            for (int x=0;x<d.mask.cols;)
            {
                if (!row[x]) { ++x; continue; }
                const int left=x;
                while (x<d.mask.cols && row[x]) ++x;
                draw->AddRectFilled(ImVec2(start.x+left*sx,start.y+y*sy),
                    ImVec2(start.x+x*sx,start.y+std::min(y+2,d.mask.rows)*sy),IM_COL32(135,110,190,190));
            }
        }
        const auto box = [&](const cv::Rect& r, ImU32 color)
        { draw->AddRect(ImVec2(start.x+r.x*sx,start.y+r.y*sy),
            ImVec2(start.x+(r.x+r.width)*sx,start.y+(r.y+r.height)*sy),color); };
        for (const auto& r:d.rejected) box(r.bounds,IM_COL32(255,80,80,255));
        const DetectedTarget* selected=nullptr;
        double nearest=d.fovRadius;
        for (const auto& t:d.targets)
        {
            const double aimX=std::round(t.aim.x), aimY=std::round(t.aim.y);
            const double distance=std::hypot(aimX-d.mask.cols/2,aimY-d.mask.rows/2);
            if ((!ignoreBelowCrosshair || aimY<=d.mask.rows/2) && distance<nearest)
            { nearest=distance; selected=&t; }
            box(t.bounds,IM_COL32(80,255,130,255));
            const ImVec2 p(start.x+static_cast<float>(t.aim.x)*sx,start.y+static_cast<float>(t.aim.y)*sy);
            draw->AddLine(ImVec2(p.x-4,p.y),ImVec2(p.x+4,p.y),IM_COL32(255,235,50,255),2);
            draw->AddLine(ImVec2(p.x,p.y-4),ImVec2(p.x,p.y+4),IM_COL32(255,235,50,255),2);
        }
        const ImVec2 center(start.x+(d.mask.cols/2)*sx,start.y+(d.mask.rows/2)*sy);
        draw->AddCircle(center,d.fovRadius*sx,IM_COL32(130,160,255,160));
        if (selected)
        {
            draw->AddCircle(ImVec2(start.x+static_cast<float>(selected->aim.x)*sx,
                start.y+static_cast<float>(selected->aim.y)*sy),6,IM_COL32(255,255,255,255),0,2);
        }
        ImGui::Dummy(ImVec2(side,side));
        ImGui::TextDisabled("Green = accepted, red = rejected, yellow = calibrated aim point");
        if (selected) ImGui::Text("Nearest eligible aim: %.2f, %.2f (white circle)",selected->aim.x,selected->aim.y);
        else ImGui::TextDisabled("No eligible aim point inside the FOV.");
    }

    bool RunDetectionUiSelfTest()
    {
        // Exercise the real widgets/draw lists without a device, window, serial
        // connection, settings writes, or any injected input.
        ImGuiContext* previous=ImGui::GetCurrentContext();
        ImGuiContext* context=ImGui::CreateContext();
        auto& io=ImGui::GetIO();
        io.IniFilename=nullptr;
        io.LogFilename=nullptr;
        io.DisplaySize=ImVec2(1200,3000);
        io.DeltaTime=1.0F/60.0F;
        io.Fonts->AddFontDefault();
        bool passed=io.Fonts->Build();
        cv::Mat mask=cv::Mat::zeros(120,120,CV_8UC1);
        cv::rectangle(mask,{25,45,8,30},cv::Scalar(255),cv::FILLED);
        cv::rectangle(mask,{68,45,30,30},cv::Scalar(255),cv::FILLED);
        for (int variant=0;passed && variant<4;++variant)
        {
            TargetDetectionConfig config;
            config.enabled=variant!=0;
            if (variant==2) config.aimPointMode=AimPointMode::boundingBox;
            if (variant==3)
            {
                config.areaFilter=config.widthFilter=config.heightFilter=false;
                config.aspectFilter=config.solidityFilter=config.flatFilter=config.orbFilter=false;
                config.closingEnabled=false;
            }
            const auto detection=DetectTargets(mask,config,{2560,1440,0,0},55,15);
            // The first frame lays out the new window, the next has draw output.
            for (int frame=0;frame<2;++frame)
            {
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(1000,2800));
                ImGui::Begin("Detector UI self-test");
                RenderDetectionControls(config);
                ImGui::GetStateStorage()->SetInt(ImGui::GetID("Contour measurements (decoded pixels)"),1);
                RenderDetectionPreview(detection,true);
                ImGui::End();
                ImGui::Render();
            }
            passed=ImGui::GetDrawData()->TotalVtxCount>0;
        }
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
        if (passed) log::Info("detector UI self-test passed: controls, contour measurements and aim preview draw lists");
        else log::Error("detector UI self-test failed");
        return passed;
    }
}
