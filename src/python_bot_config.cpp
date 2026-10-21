#include "python_bot.hpp"
#include "color_targets.hpp"
#include "logger.hpp"
#include "makcu_controller.hpp"
#include "resource.h"
#include <Windows.h>
#include <objbase.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/geometry/2d.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace
{
    using namespace winrt::Windows::Data::Json;
    struct JsonApartment
    {
        HRESULT result{CoInitializeEx(nullptr,COINIT_MULTITHREADED)};
        JsonApartment() { if (FAILED(result) && result!=RPC_E_CHANGED_MODE) winrt::check_hresult(result); }
        ~JsonApartment()
        {
            // A standalone parser call may own the last COM initialization.
            // Drop cached activation factories before that apartment unloads.
            if (result==S_OK) winrt::clear_factory_cache();
            if (SUCCEEDED(result)) CoUninitialize();
        }
    };
    double Number(const JsonObject& object,const wchar_t* key,double fallback)
    { return object.HasKey(key) ? object.GetNamedNumber(key) : fallback; }
    int Integer(const JsonObject& object,const wchar_t* key,int fallback)
    {
        const double value=Number(object,key,fallback);
        if (!std::isfinite(value) || value<std::numeric_limits<int>::min() || value>std::numeric_limits<int>::max())
            throw std::runtime_error("Integer configuration value is out of range");
        return static_cast<int>(value);
    }
    std::string String(const JsonObject& object,const wchar_t* key,const std::string& fallback)
    { return object.HasKey(key) ? winrt::to_string(object.GetNamedString(key)) : fallback; }
    bool Boolean(const JsonObject& object,const wchar_t* key,bool fallback)
    { return object.HasKey(key) ? object.GetNamedBoolean(key) : fallback; }
    vanta::PythonAxisMode AxisMode(const JsonObject& object)
    {
        if (!object.HasKey(L"axis_mode"))
            return Boolean(object,L"horizontal_only",false)
                ? vanta::PythonAxisMode::horizontalOnly
                : vanta::PythonAxisMode::standard;
        const auto value=winrt::to_string(object.GetNamedString(L"axis_mode"));
        if (value=="standard") return vanta::PythonAxisMode::standard;
        if (value=="horizontal_only") return vanta::PythonAxisMode::horizontalOnly;
        if (value=="hybrid") return vanta::PythonAxisMode::hybrid;
        throw std::runtime_error("axis_mode must be standard, horizontal_only or hybrid");
    }
    std::array<int,3> Hsv(const JsonObject& object,const std::wstring& key)
    {
        const auto array=object.GetNamedArray(key);
        if (array.Size()!=3) throw std::runtime_error("HSV values must contain exactly three integers");
        std::array<int,3> result{};
        for (std::uint32_t i=0;i<3;++i)
        {
            const double value=array.GetNumberAt(i);
            if (!std::isfinite(value) || value<0 || value>255 || std::trunc(value)!=value)
                throw std::runtime_error("HSV values must be unsigned bytes");
            result[i]=static_cast<int>(value);
        }
        return result;
    }
    void Put(JsonObject& object,const wchar_t* key,double value)
    { object.Insert(key,JsonValue::CreateNumberValue(value)); }
    void Put(JsonObject& object,const wchar_t* key,bool value)
    { object.Insert(key,JsonValue::CreateBooleanValue(value)); }
    void Put(JsonObject& object,const wchar_t* key,const std::string& value)
    { object.Insert(key,JsonValue::CreateStringValue(winrt::to_hstring(value))); }
    JsonArray Triple(const std::array<int,3>& values)
    {
        JsonArray array;
        for (const auto value:values) array.Append(JsonValue::CreateNumberValue(value));
        return array;
    }
    JsonArray Coordinates(std::initializer_list<double> values)
    {
        JsonArray array;
        for (const auto value:values) array.Append(JsonValue::CreateNumberValue(value));
        return array;
    }
    JsonArray MaskRuns(const cv::Mat& mask)
    {
        JsonArray runs;
        int start=-1;
        const int total=mask.rows*mask.cols;
        for (int index=0;index<=total;++index)
        {
            const bool occupied=index<total && mask.at<std::uint8_t>(index/mask.cols,index%mask.cols)!=0;
            if (occupied && start<0) start=index;
            if (!occupied && start>=0)
            {
                runs.Append(Coordinates({static_cast<double>(start),static_cast<double>(index-start)}));
                start=-1;
            }
        }
        return runs;
    }
}

namespace vanta
{
    std::string DefaultPythonBotJson()
    {
        const HMODULE module=GetModuleHandleW(nullptr);
        const auto resource=FindResourceW(module,MAKEINTRESOURCEW(IDR_PYTHON_CONFIG),RT_RCDATA);
        if (!resource) return {};
        const auto bytes=LoadResource(module,resource);
        const auto data=static_cast<const char*>(LockResource(bytes));
        return data ? std::string(data,SizeofResource(module,resource)) : std::string{};
    }

    bool ParsePythonBotConfig(const std::string& json,PythonBotConfig& config,std::string& error)
    {
        try
        {
            JsonApartment apartment;
            const auto object=JsonObject::Parse(winrt::to_hstring(json));
            PythonBotConfig parsed;
            parsed.activePreset=String(object,L"active_preset",parsed.activePreset);
            parsed.fovRadius=Integer(object,L"fov_radius",parsed.fovRadius);
            parsed.smoothFactor=Number(object,L"smooth_factor",parsed.smoothFactor);
            parsed.axisMode=AxisMode(object);
            const auto physicalMode=String(object,L"physical_input_mode","existing");
            if(physicalMode!="existing" && physicalMode!="fuse")
                throw std::runtime_error("physical_input_mode must be existing or fuse");
            parsed.physicalInputMode=physicalMode=="fuse"?PythonPhysicalInputMode::fuse:PythonPhysicalInputMode::existing;
            parsed.hybridVerticalTimeMilliseconds=Integer(object,L"hybrid_vertical_time_ms",parsed.hybridVerticalTimeMilliseconds);
            parsed.minimumArea=Number(object,L"min_contour_area",parsed.minimumArea);
            parsed.offsetX=Integer(object,L"offset_x",parsed.offsetX);
            parsed.offsetY=Integer(object,L"offset_y",parsed.offsetY);
            parsed.filterOrbs=Boolean(object,L"filter_orbs",parsed.filterOrbs);
            parsed.aimKey=String(object,L"aim_key",parsed.aimKey);
            parsed.screenWidth=Integer(object,L"screen_width",parsed.screenWidth);
            parsed.screenHeight=Integer(object,L"screen_height",parsed.screenHeight);
            parsed.makcuDebug=Boolean(object,L"makcu_debug",parsed.makcuDebug);
            parsed.makcuAutoReconnect=Boolean(object,L"makcu_auto_reconnect",parsed.makcuAutoReconnect);
            const auto presets=object.GetNamedObject(L"presets");
            for (const auto& pair:presets)
            {
                PythonColorPreset preset;
                preset.key=winrt::to_string(pair.Key());
                const auto entry=pair.Value().GetObject();
                preset.name=String(entry,L"name",preset.key);
                if (entry.HasKey(L"hex_variations"))
                    for (const auto& variation:entry.GetNamedArray(L"hex_variations"))
                        preset.hexVariations.push_back(winrt::to_string(variation.GetString()));
                const int count=preset.key=="red" ? 2 : 1;
                for (int range=0;range<count;++range)
                {
                    const std::wstring suffix=count==2 ? L"_"+std::to_wstring(range+1) : L"";
                    preset.ranges.push_back({Hsv(entry,L"hsv_lower"+suffix),Hsv(entry,L"hsv_upper"+suffix)});
                }
                parsed.presets.push_back(std::move(preset));
            }
            // Older/user JSON gains the new choice without replacing any saved
            // preset, selection or metadata. Keep Python's processing independent
            // of the normal detector's blacklist and filtering policy.
            if(std::none_of(parsed.presets.begin(),parsed.presets.end(),
                [](const auto& entry){return entry.key=="purple_narrow";}))
            {
                const auto& narrow=kHsvColorTargets[0];
                PythonColorPreset added;added.key="purple_narrow";added.name=narrow.label;
                added.ranges.push_back({narrow.ranges[0].lower,narrow.ranges[0].upper});
                parsed.presets.push_back(std::move(added));
            }
            if (!ValidatePythonBotConfig(parsed,error)) return false;
            config=std::move(parsed);
            return true;
        }
        catch (const winrt::hresult_error& exception) { error=winrt::to_string(exception.message()); }
        catch (const std::exception& exception) { error=exception.what(); }
        return false;
    }

    std::string SerializePythonBotConfig(const PythonBotConfig& config,const std::string& originalJson)
    {
        JsonApartment apartment;
        JsonObject object=JsonObject::Parse(winrt::to_hstring(originalJson));
        Put(object,L"active_preset",config.activePreset);
        Put(object,L"fov_radius",static_cast<double>(config.fovRadius));
        Put(object,L"smooth_factor",config.smoothFactor);
        Put(object,L"axis_mode",std::string(PythonAxisModeJsonName(config.axisMode)));
        Put(object,L"physical_input_mode",std::string(PythonPhysicalInputModeJsonName(config.physicalInputMode)));
        Put(object,L"hybrid_vertical_time_ms",static_cast<double>(config.hybridVerticalTimeMilliseconds));
        Put(object,L"horizontal_only",config.axisMode==PythonAxisMode::horizontalOnly);
        Put(object,L"min_contour_area",config.minimumArea);
        Put(object,L"offset_x",static_cast<double>(config.offsetX));
        Put(object,L"offset_y",static_cast<double>(config.offsetY));
        Put(object,L"filter_orbs",config.filterOrbs);
        Put(object,L"aim_key",config.aimKey);
        Put(object,L"screen_width",static_cast<double>(config.screenWidth));
        Put(object,L"screen_height",static_cast<double>(config.screenHeight));
        Put(object,L"makcu_debug",config.makcuDebug);
        Put(object,L"makcu_auto_reconnect",config.makcuAutoReconnect);
        if (object.HasKey(L"makcu_port")) object.Remove(L"makcu_port");
        if (object.HasKey(L"makcu_override_port")) object.Remove(L"makcu_override_port");
        JsonObject presets=object.GetNamedObject(L"presets");
        for (const auto& preset:config.presets)
        {
            const auto key=winrt::to_hstring(preset.key);
            JsonObject entry=presets.HasKey(key) ? presets.GetNamedObject(key) : JsonObject{};
            Put(entry,L"name",preset.name);
            JsonArray variations;
            for (const auto& variation:preset.hexVariations)
                variations.Append(JsonValue::CreateStringValue(winrt::to_hstring(variation)));
            entry.Insert(L"hex_variations",variations);
            for (std::size_t range=0;range<preset.ranges.size();++range)
            {
                const std::wstring suffix=preset.key=="red" ? L"_"+std::to_wstring(range+1) : L"";
                entry.Insert(L"hsv_lower"+suffix,Triple(preset.ranges[range].lower));
                entry.Insert(L"hsv_upper"+suffix,Triple(preset.ranges[range].upper));
            }
            presets.Insert(key,entry);
        }
        object.Insert(L"presets",presets);
        return winrt::to_string(object.Stringify());
    }

    bool RunPythonBotSelfTest()
    {
        const auto fail=[](const char* why) { log::Error("Python mode self-test: %s",why); return false; };
        if (!MakcuController::RunMovementOwnershipSelfTest())
            return fail("movement ownership / stale-generation guard");
        PythonBotConfig config;
        std::string error;
        if (!ParsePythonBotConfig(DefaultPythonBotJson(),config,error)) return fail(error.c_str());
        if (config.fovRadius!=60 || config.smoothFactor!=0.4030150753768844 || config.minimumArea!=1.0 ||
            config.offsetX!=1 || config.offsetY!=7 || config.axisMode!=PythonAxisMode::standard ||
            config.hybridVerticalTimeMilliseconds!=300 || config.physicalInputMode!=PythonPhysicalInputMode::existing)
            return fail("supplied configuration changed");
        if (PythonAimButton("0x05")!=MouseButtonBinding::x1 || PythonAimButton("04")!=MouseButtonBinding::middle ||
            PythonAimButton("0x10")) return fail("physical button mapping");
        PythonBotConfig roundTrip;
        if (!ParsePythonBotConfig(SerializePythonBotConfig(config,DefaultPythonBotJson()),roundTrip,error) ||
            roundTrip.smoothFactor!=config.smoothFactor || roundTrip.presets.size()!=4)
            return fail("configuration precision/round trip");
        const bool narrowMigration=[&]
        {
            JsonApartment apartment;
            auto legacy=JsonObject::Parse(winrt::to_hstring(DefaultPythonBotJson()));
            legacy.GetNamedObject(L"presets").Remove(L"purple_narrow");
            legacy.Insert(L"active_preset",JsonValue::CreateStringValue(L"yellow"));
            PythonBotConfig migrated;
            if(!ParsePythonBotConfig(winrt::to_string(legacy.Stringify()),migrated,error) || migrated.activePreset!="yellow")return false;
            const auto found=std::find_if(migrated.presets.begin(),migrated.presets.end(),[](const auto& p){return p.key=="purple_narrow";});
            if(found==migrated.presets.end() || found->ranges.size()!=1 ||
                found->ranges[0].lower!=std::array<int,3>{144,106,172} || found->ranges[0].upper!=std::array<int,3>{151,255,255})return false;
            found->name="My purple";found->ranges[0]={{145,110,180},{150,250,250}};
            const auto saved=SerializePythonBotConfig(migrated,winrt::to_string(legacy.Stringify()));
            PythonBotConfig custom;
            if(!ParsePythonBotConfig(saved,custom,error) || custom.activePreset!="yellow" || custom.presets.size()!=4)return false;
            const auto existing=std::find_if(custom.presets.begin(),custom.presets.end(),[](const auto& p){return p.key=="purple_narrow";});
            return existing!=custom.presets.end() && existing->name=="My purple" && existing->ranges[0].lower==found->ranges[0].lower &&
                existing->ranges[0].upper==found->ranges[0].upper;
        }();
        if(!narrowMigration)return fail("narrow-purple migration/custom preset preservation");
        const bool narrowBoundaries=[&]
        {
            PythonBotConfig narrow=config;narrow.activePreset="purple_narrow";
            // Exercise the actual Python BGRA-to-mask path, including morphology.
            // Saturated hue edges and separate S/V edges survive byte conversion.
            const auto found=std::find_if(narrow.presets.begin(),narrow.presets.end(),[](const auto& p){return p.key=="purple_narrow";});
            if(found==narrow.presets.end())return false;
            const std::array<cv::Vec3b,9> samples{{{144,255,255},{151,255,255},{147,106,255},{147,255,172},
                {143,255,255},{152,255,255},{147,105,255},{147,255,171},{147,200,200}}};
            const int side=narrow.fovRadius*2;
            for(std::size_t i=0;i<samples.size();++i)
            {
                cv::Mat hsv(1,1,CV_8UC3),bgr;hsv.at<cv::Vec3b>(0,0)=samples[i];
                cv::cvtColor(hsv,bgr,cv::COLOR_HSV2BGR);const auto pixel=bgr.at<cv::Vec3b>(0,0);
                cv::Mat crop(side,side,CV_8UC4,cv::Scalar(pixel[0],pixel[1],pixel[2],255));
                const auto processed=ProcessPythonBotRoi(crop,narrow,{0,0,side,side});
                const bool inside=i<4 || i==8;
                if(!processed.valid || cv::countNonZero(processed.colorMask)!=(inside?side*side:0))return false;
            }
            return true;
        }();
        if(!narrowBoundaries)return fail("narrow-purple inclusive HSV boundaries");
        const bool metadataPreserved=[&]
        {
            JsonApartment apartment;
            auto extended=JsonObject::Parse(winrt::to_hstring(DefaultPythonBotJson()));
            extended.Insert(L"custom_metadata",JsonValue::CreateStringValue(L"preserve me"));
            auto preset=extended.GetNamedObject(L"presets").GetNamedObject(L"purple");
            preset.Insert(L"custom_preset_metadata",JsonValue::CreateNumberValue(42));
            const auto serialized=SerializePythonBotConfig(config,winrt::to_string(extended.Stringify()));
            const auto restored=JsonObject::Parse(winrt::to_hstring(serialized));
            return restored.GetNamedString(L"custom_metadata")==L"preserve me" &&
                restored.GetNamedObject(L"presets").GetNamedObject(L"purple").GetNamedNumber(L"custom_preset_metadata")==42;
        }();
        if (!metadataPreserved) return fail("unknown JSON metadata was lost");
        const bool physicalModeMigration=[&]
        {
            JsonApartment apartment;
            auto legacy=JsonObject::Parse(winrt::to_hstring(DefaultPythonBotJson()));
            legacy.Remove(L"physical_input_mode");legacy.Insert(L"accessibility_note",JsonValue::CreateStringValue(L"keep"));
            PythonBotConfig parsed;
            if(!ParsePythonBotConfig(winrt::to_string(legacy.Stringify()),parsed,error) ||
                parsed.physicalInputMode!=PythonPhysicalInputMode::existing)return false;
            parsed.physicalInputMode=PythonPhysicalInputMode::fuse;
            const auto encoded=SerializePythonBotConfig(parsed,winrt::to_string(legacy.Stringify()));
            PythonBotConfig saved;
            if(!ParsePythonBotConfig(encoded,saved,error) || saved.physicalInputMode!=PythonPhysicalInputMode::fuse ||
                saved.activePreset!=parsed.activePreset || saved.presets.size()!=parsed.presets.size())return false;
            const auto restored=JsonObject::Parse(winrt::to_hstring(encoded));
            if(restored.GetNamedString(L"accessibility_note")!=L"keep")return false;
            legacy.Insert(L"physical_input_mode",JsonValue::CreateStringValue(L"unknown"));
            return !ParsePythonBotConfig(winrt::to_string(legacy.Stringify()),parsed,error);
        }();
        if(!physicalModeMigration)return fail("physical-input migration / explicit mode / metadata preservation");
        PythonBotConfig invalid;
        if (ParsePythonBotConfig("{bad",invalid,error)) return fail("malformed JSON accepted");
        cv::Mat frame=cv::Mat::zeros(240,320,CV_8UC4);
        cv::rectangle(frame,{150,92,6,20},cv::Scalar(255,0,255,255),cv::FILLED);
        const auto detected=ProcessPythonBotFrame(frame,config);
        if (!detected.valid || detected.target!=cv::Point(53,38) || detected.movement!=cv::Point(-2,-8) ||
            detected.sourceRoi!=cv::Rect(100,60,120,120)) return fail("mask aim / double precision / truncation");
        auto fusedConfig=config;fusedConfig.physicalInputMode=PythonPhysicalInputMode::fuse;
        const auto fused=ProcessPythonBotFrame(frame,fusedConfig);
        if(!fused.valid || fused.target!=detected.target || fused.movement!=detected.movement ||
            cv::norm(fused.colorMask,detected.colorMask,cv::NORM_INF)!=0 || cv::norm(fused.fovMask,detected.fovMask,cv::NORM_INF)!=0)
            return fail("Fuse changed pure detection or calculated correction");
        cv::Mat orb=cv::Mat::zeros(240,320,CV_8UC4);
        cv::rectangle(orb,{145,105,30,30},cv::Scalar(255,0,255,255),cv::FILLED);
        config.filterOrbs=false;
        if (ProcessPythonBotFrame(orb,config).target) return fail("reference's unused filter_orbs flag changed semantics");
        config.fovRadius=0;
        if (ValidatePythonBotConfig(config,error) || ProcessPythonBotFrame(frame,config).valid)
            return fail("invalid config did not fail closed");
        config=roundTrip;
        config.fovRadius=200;
        if (ProcessPythonBotFrame(frame,config).valid) return fail("oversized crop did not fail closed");
        for (auto& preset:config.presets) preset.ranges.front().lower[0]=-1;
        if (ValidatePythonBotConfig(config,error)) return fail("negative HSV accepted");
        log::Info("Python mode self-test passed: supplied JSON, precise smoothing, truncation, mask aim, original orb behavior and MAKCU button mapping");
        return true;
    }

    bool RunPythonParityFixtures(const std::string& inputPath,const std::string& outputPath)
    {
        try
        {
            JsonApartment apartment;
            std::ifstream input(std::filesystem::path(winrt::to_hstring(inputPath).c_str()),std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open parity fixtures");
            const std::string text((std::istreambuf_iterator<char>(input)),{});
            const auto root=JsonObject::Parse(winrt::to_hstring(text));
            JsonArray output;
            for (const auto& value:root.GetNamedArray(L"cases"))
            {
                const auto fixture=value.GetObject();
                PythonBotConfig config;
                std::string error;
                if (!ParsePythonBotConfig(winrt::to_string(fixture.GetNamedObject(L"config").Stringify()),config,error))
                    throw std::runtime_error(error);
                cv::Mat image=cv::Mat::zeros(Integer(fixture,L"height",240),Integer(fixture,L"width",320),CV_8UC4);
                for (const auto& item:fixture.GetNamedArray(L"shapes"))
                {
                    const auto shape=item.GetObject();
                    const auto bgr=shape.GetNamedArray(L"bgr");
                    const cv::Scalar paint(bgr.GetNumberAt(0),bgr.GetNumberAt(1),bgr.GetNumberAt(2),255);
                    if (shape.HasKey(L"points"))
                    {
                        std::vector<cv::Point> points;
                        for (const auto& pointValue:shape.GetNamedArray(L"points"))
                        {
                            const auto point=pointValue.GetArray();
                            points.emplace_back(static_cast<int>(point.GetNumberAt(0)),static_cast<int>(point.GetNumberAt(1)));
                        }
                        cv::fillPoly(image,std::vector<std::vector<cv::Point>>{points},paint);
                    }
                    else cv::rectangle(image,cv::Rect(Integer(shape,L"x",0),Integer(shape,L"y",0),
                        Integer(shape,L"w",1),Integer(shape,L"h",1)),paint,cv::FILLED);
                }
                const auto result=ProcessPythonBotFrame(image,config);
                if (!result.valid) throw std::runtime_error(result.error);
                JsonObject record;
                Put(record,L"name",String(fixture,L"name",""));
                if (result.target) record.Insert(L"target",Coordinates({static_cast<double>(result.target->x),static_cast<double>(result.target->y)}));
                else record.Insert(L"target",JsonValue::Parse(L"null"));
                record.Insert(L"movement",Coordinates({static_cast<double>(result.movement.x),static_cast<double>(result.movement.y)}));
                record.Insert(L"color_runs",MaskRuns(result.colorMask));
                record.Insert(L"fov_runs",MaskRuns(result.fovMask));
                JsonArray accepted;
                for (const auto& contour:result.validContours)
                {
                    const auto bounds=cv::boundingRect(contour);
                    accepted.Append(Coordinates({static_cast<double>(bounds.x),static_cast<double>(bounds.y),
                        static_cast<double>(bounds.width),static_cast<double>(bounds.height),cv::contourArea(contour)}));
                }
                record.Insert(L"contours",accepted);
                output.Append(record);
            }
            std::ofstream file(std::filesystem::path(winrt::to_hstring(outputPath).c_str()),std::ios::binary|std::ios::trunc);
            if (!file) throw std::runtime_error("Cannot write native parity results");
            file<<winrt::to_string(output.Stringify());
            if (!file) throw std::runtime_error("Failed to write native parity results");
            log::Info("Python parity fixture export completed: %u cases; no device connection or input injection",output.Size());
            return true;
        }
        catch (const winrt::hresult_error& exception) { log::Error("Python parity: %s",winrt::to_string(exception.message()).c_str()); }
        catch (const std::exception& exception) { log::Error("Python parity: %s",exception.what()); }
        return false;
    }
}
