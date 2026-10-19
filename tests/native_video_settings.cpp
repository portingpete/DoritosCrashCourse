#include "renderer/native/video_settings.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Doritos::Graphics;
namespace {
size_t checks = 0;
void require(bool condition, const char* detail) { ++checks; if (!condition) throw std::runtime_error(detail); }
auto clockTime = std::chrono::steady_clock::time_point{};
auto fakeClock() { return clockTime; }
void advance(std::chrono::steady_clock::duration duration) { clockTime += duration; }
struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        (L"DoritosVideoSettingsTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::vector<std::filesystem::path> leaves;
    Fixture() { require(std::filesystem::create_directory(directory),"create isolated settings fixture directory"); }
    ~Fixture() {
        std::error_code ignored;
        for (const auto& leaf : leaves) std::filesystem::remove(leaf,ignored);
        // Only these exact owned leaves and the now-empty fixture directory are
        // removed. Tests never recursively traverse or delete a directory.
        std::filesystem::remove(directory,ignored);
    }
    std::filesystem::path path(const wchar_t* name) { leaves.push_back(directory / name); return leaves.back(); }
};
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path,std::ios::binary | std::ios::trunc);
    file.write(text.data(),std::streamsize(text.size()));
    require(file.good(),"write owned malformed settings fixture");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
std::string substitute(std::string text, const std::string& before, const std::string& after) {
    const auto at = text.find(before);
    require(at != text.npos,"fixture substitution finds its own source token");
    text.replace(at,before.size(),after); return text;
}
VideoSettings configured() {
    VideoSettings settings;
    settings.windowMode = WindowMode::Borderless; settings.width = 3440; settings.height = 1440;
    settings.monitor = 2; settings.vsync = VSyncMode::Off;
    settings.frameLimit = 144; settings.backgroundFrameLimit = 10;
    settings.scaling = ScalingMode::Stretch; settings.filtering = TextureFiltering::Trilinear;
    settings.anisotropy = 16; settings.mipBias = -.375f; settings.fxaa = true;
    settings.sharpness = .4375f; settings.brightness = -.125f;
    settings.contrast = 1.25f; settings.gamma = .875f; settings.saturation = 1.5f;
    settings.lowLatency = true; settings.showFps = true;
    return settings;
}
VideoSettingsRequest take(const char* detail) {
    const auto request = takeVideoSettingsRequest();
    require(request.has_value(),detail); return *request;
}
void initialize(const std::filesystem::path& path) {
    clockTime = {};
    initializeVideoSettings(path,fakeClock);
    const auto initial = take("initialization requests loaded settings exactly once");
    require(!takeVideoSettingsRequest(),"dispatched initialization request is not duplicated");
    completeVideoSettingsRequest(initial.serial,true);
    const auto status = videoSettingsStatus();
    require(!status.previewActive && !status.previewPending && status.secondsRemaining == 0,
        "successful startup apply never starts a preview timer");
}
void validation() {
    require(validateVideoSettings(VideoSettings{}),"native defaults are valid");
    require(validateVideoSettings(configured()),"modern ultrawide settings are valid");
    VideoSettings edge;
    edge.width = 320; edge.height = 200; edge.monitor = 63;
    edge.frameLimit = edge.backgroundFrameLimit = 0;
    edge.mipBias = -1; edge.sharpness = 1; edge.brightness = -.25f;
    edge.contrast = .5f; edge.gamma = 2; edge.saturation = 0;
    require(validateVideoSettings(edge),"inclusive lower dimensions and float boundaries support unlimited frame rates");
    edge.width = edge.height = 8192; edge.frameLimit = edge.backgroundFrameLimit = 1000;
    edge.mipBias = 2; edge.brightness = .25f; edge.contrast = 1.5f; edge.gamma = .5f; edge.saturation = 2;
    require(validateVideoSettings(edge),"inclusive upper dimensions frame rates and float boundaries");
    const auto rejects = [](auto mutate) {
        auto invalid = configured(); mutate(invalid);
        require(!validateVideoSettings(invalid),"invalid preference is rejected before renderer or persistence");
    };
    rejects([](auto& s) { s.windowMode = static_cast<WindowMode>(99); });
    rejects([](auto& s) { s.vsync = static_cast<VSyncMode>(99); });
    rejects([](auto& s) { s.scaling = static_cast<ScalingMode>(99); });
    rejects([](auto& s) { s.filtering = static_cast<TextureFiltering>(99); });
    rejects([](auto& s) { s.width = 319; }); rejects([](auto& s) { s.width = 8193; });
    rejects([](auto& s) { s.height = 199; }); rejects([](auto& s) { s.height = 8193; });
    rejects([](auto& s) { s.monitor = 64; });
    rejects([](auto& s) { s.frameLimit = 1; }); rejects([](auto& s) { s.frameLimit = 1001; });
    rejects([](auto& s) { s.backgroundFrameLimit = 9; }); rejects([](auto& s) { s.backgroundFrameLimit = 1001; });
    for (const auto value : {1u,3u,32u,UINT32_MAX}) rejects([=](auto& s) { s.anisotropy = value; });
    for (auto member : {&VideoSettings::mipBias,&VideoSettings::sharpness,&VideoSettings::brightness,
        &VideoSettings::contrast,&VideoSettings::gamma,&VideoSettings::saturation}) {
        rejects([=](auto& s) { s.*member = std::numeric_limits<float>::quiet_NaN(); });
        rejects([=](auto& s) { s.*member = std::numeric_limits<float>::infinity(); });
        rejects([=](auto& s) { s.*member = -std::numeric_limits<float>::infinity(); });
    }
    rejects([](auto& s) { s.mipBias = -1.01f; }); rejects([](auto& s) { s.sharpness = 1.01f; });
    rejects([](auto& s) { s.brightness = .251f; }); rejects([](auto& s) { s.contrast = .49f; });
    rejects([](auto& s) { s.gamma = 2.01f; }); rejects([](auto& s) { s.saturation = -.001f; });
}
void persistence(Fixture& fixture) {
    const auto path = fixture.path(L"native video \u03b3.ini");
    std::string error = "old error";
    require(loadVideoSettings(path,error) == VideoSettings{} && error.empty(),"missing configuration defaults without an error");
    const auto settings = configured();
    require(saveVideoSettings(path,settings,error) && error.empty(),"save valid complete native configuration");
    require(loadVideoSettings(path,error) == settings && error.empty(),"all settings round trip exactly including float values");
    const auto original = read(path);
    auto different = VideoSettings{}; different.vsync = VSyncMode::On; different.filtering = TextureFiltering::Bilinear;
    require(saveVideoSettings(path,different,error),"replace an existing configuration atomically");
    require(loadVideoSettings(path,error) == different && error.empty(),"replacement contains every new field");
    const auto preserved = read(path);
    auto invalid = settings; invalid.gamma = std::numeric_limits<float>::quiet_NaN();
    require(!saveVideoSettings(path,invalid,error) && !error.empty() && read(path) == preserved,
        "validation failure preserves previous bytes");
    HANDLE held = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(held != INVALID_HANDLE_VALUE,"hold old configuration without delete sharing");
    const bool saved = saveVideoSettings(path,settings,error);
    CloseHandle(held);
    require(!saved && !error.empty() && read(path) == preserved,
        "failed atomic rename leaves complete previous file intact");
    for (const auto& leaf : std::filesystem::directory_iterator(fixture.directory))
        require(leaf.path().filename().wstring().find(L".tmp-") == std::wstring::npos,"failed rename cleans up only its own temporary leaf");

    const auto malformed = fixture.path(L"malformed.ini");
    const std::vector<std::string> bad{
        {}, "[Video]\nversion=1\n", substitute(original,"[Video]","[Other]"),
        substitute(original,"version=1","version=2"), original + "width=800\n",
        original + "unknown=1\n", substitute(original,"width=3440\n","unknown=3440\n"),
        substitute(original,"width=3440","width=-1"), substitute(original,"width=3440","width=4294967296"),
        substitute(original,"height=1440","height=8193"), substitute(original,"monitor=2","monitor=64"),
        substitute(original,"frame_limit=144","frame_limit=9"),
        substitute(original,"fxaa=1","fxaa=true"), substitute(original,"vsync=off","vsync=adaptive"),
        substitute(original,"gamma=0.875","gamma=nan"), substitute(original,"gamma=0.875","gamma=inf"),
        substitute(original,"gamma=0.875","gamma=0,875"), substitute(original,"gamma=0.875","gamma=2.1"),
        original + "[Video]\n", original + std::string(300,'#') + "\n", std::string(8193,'x'),
        original + std::string("\0ignored",8)};
    for (const auto& content : bad) {
        write(malformed,content);
        require(loadVideoSettings(malformed,error) == VideoSettings{} && !error.empty(),
            "malformed or unsupported configuration loads complete defaults without partial values");
    }
    std::string crlf;
    for (char c : original) { if (c == '\n') crlf += '\r'; crlf += c; }
    write(malformed,"# Native configuration\r\n\r\n" + crlf);
    require(loadVideoSettings(malformed,error) == settings && error.empty(),"comments blank lines and CRLF are accepted");

    // A reader holds one old or new file identity across size/read, even while
    // same-directory atomic replacement publishes the other complete version.
    require(saveVideoSettings(path,settings,error),"seed atomic reader fixture");
    std::atomic<bool> done{false}, readerValid{true};
    std::thread reader([&] {
        while (!done.load()) {
            std::string localError;
            const auto loaded = loadVideoSettings(path,localError);
            if (!localError.empty() || (loaded != settings && loaded != different)) {
                if(readerValid.exchange(false)) std::fprintf(stderr,"Atomic reader diagnostic: error=%s dimensions=%u,%u gamma=%g\n",localError.c_str(),loaded.width,loaded.height,loaded.gamma);
            }
        }
    });
    bool writesValid = true;
    for (unsigned index = 0; index < 20; ++index)
        if (!saveVideoSettings(path,index % 2 ? settings : different,error)) {
            if(writesValid) std::fprintf(stderr,"Atomic writer diagnostic: %s\n",error.c_str());
            writesValid = false;
        }
    done.store(true); reader.join();
    require(writesValid && readerValid.load(),"concurrent readers observe complete old or new atomic configurations");
}
void previews(Fixture& fixture) {
    const auto path = fixture.path(L"preview.ini");
    initialize(path);
    const auto first = configured();
    require(requestVideoSettingsPreview(first),"valid preview queues renderer apply");
    require(videoSettingsStatus().previewPending && !videoSettingsStatus().previewActive &&
        currentVideoSettings() == VideoSettings{} && !confirmVideoSettingsPreview(),
        "queued preview cannot be kept or published before renderer success");
    const auto request = take("consume preview request");
    advance(std::chrono::hours(1)); tickVideoSettings();
    require(videoSettingsStatus().previewPending && !videoSettingsStatus().previewActive,
        "time waiting for renderer acknowledgement does not spend confirmation time");
    completeVideoSettingsRequest(request.serial,true);
    require(currentVideoSettings() == first && committedVideoSettings() == VideoSettings{} &&
        videoSettingsStatus().previewActive && videoSettingsStatus().secondsRemaining == 15,
        "successful matching acknowledgement starts full preview and preserves committed preferences");
    advance(std::chrono::milliseconds(1));
    require(videoSettingsStatus().secondsRemaining == 15,"countdown rounds remaining fraction upward");
    advance(std::chrono::milliseconds(14998)); tickVideoSettings();
    require(videoSettingsStatus().previewActive && videoSettingsStatus().secondsRemaining == 1,
        "preview stays active through its final millisecond");
    advance(std::chrono::milliseconds(1)); tickVideoSettings();
    require(!videoSettingsStatus().previewActive && !confirmVideoSettingsPreview() && currentVideoSettings() == first,
        "deadline forbids Keep and queues restore without falsely publishing unapplied settings");
    const auto restore = take("timeout requests committed settings restore");
    require(restore.settings == VideoSettings{} && restore.serial > request.serial,"timeout restores committed serial");
    completeVideoSettingsRequest(restore.serial,true);
    require(currentVideoSettings() == VideoSettings{} && !std::filesystem::exists(path),
        "timeout completes restore and never persists preview");

    require(requestVideoSettingsPreview(first),"queue first serial in superseded preview case");
    const auto stale = take("dispatch first preview");
    auto second = first; second.width = 1920; second.height = 1080; second.gamma = 1.1f;
    require(requestVideoSettingsPreview(second),"newer preview supersedes first serial");
    const auto latest = take("dispatch latest preview");
    completeVideoSettingsRequest(stale.serial,true);
    require(currentVideoSettings() == VideoSettings{} && videoSettingsStatus().previewPending,
        "stale successful acknowledgement cannot activate or publish superseded settings");
    completeVideoSettingsRequest(stale.serial,false,"stale error");
    require(videoSettingsStatus().error.empty(),"stale failed acknowledgement cannot overwrite latest status");
    completeVideoSettingsRequest(latest.serial,true);
    require(currentVideoSettings() == second && confirmVideoSettingsPreview(),"Keep saves latest acknowledged successful preview");
    std::string error;
    require(committedVideoSettings() == second && loadVideoSettings(path,error) == second && error.empty() &&
        !videoSettingsStatus().previewActive,"Keep commits all fields and cancels countdown");
    advance(std::chrono::seconds(30)); tickVideoSettings();
    require(!takeVideoSettingsRequest() && currentVideoSettings() == second,"confirmed settings are not reverted by old deadline");

    require(requestVideoSettingsPreview(first),"queue unapplied preview before Cancel");
    const auto cancelled = take("dispatch cancelled preview");
    cancelVideoSettingsPreview();
    const auto cancellationRestore = take("Cancel queues committed restore even for an in-flight apply");
    completeVideoSettingsRequest(cancelled.serial,true);
    require(currentVideoSettings() == second && !videoSettingsStatus().previewActive,
        "late cancelled apply acknowledgement cannot activate preview");
    completeVideoSettingsRequest(cancellationRestore.serial,true);
    require(currentVideoSettings() == second && committedVideoSettings() == second,"Cancel restores committed settings");

    require(requestVideoSettingsPreview(first),"queue renderer rejection case");
    const auto rejected = take("dispatch rejected preview");
    completeVideoSettingsRequest(rejected.serial,false,"Unsupported display mode");
    require(currentVideoSettings() == second && committedVideoSettings() == second &&
        !videoSettingsStatus().previewActive && !videoSettingsStatus().previewPending &&
        videoSettingsStatus().error == "Unsupported display mode" && !confirmVideoSettingsPreview(),
        "renderer rejection preserves applied and committed settings and reports concrete error");

    require(requestVideoSettingsPreview(first),"queue active preview before rejected replacement");
    const auto active = take("dispatch active preview"); completeVideoSettingsRequest(active.serial,true);
    advance(std::chrono::seconds(10));
    auto third = first; third.gamma = 1.25f;
    require(requestVideoSettingsPreview(third),"queue replacement while prior preview is active");
    const auto failedReplacement = take("dispatch failed replacement");
    completeVideoSettingsRequest(failedReplacement.serial,false,"Apply failed");
    require(currentVideoSettings() == first && videoSettingsStatus().previewActive &&
        videoSettingsStatus().secondsRemaining == 5,"failed replacement preserves original preview deadline");
    advance(std::chrono::seconds(5)); tickVideoSettings();
    const auto failedReplacementRestore = take("original preview still expires after rejected replacement");
    completeVideoSettingsRequest(failedReplacementRestore.serial,true);
    require(currentVideoSettings() == second,"rejected replacement does not prevent committed restore");

    auto invalid = first; invalid.gamma = std::numeric_limits<float>::quiet_NaN();
    require(!requestVideoSettingsPreview(invalid) && !takeVideoSettingsRequest() && !videoSettingsStatus().error.empty(),
        "invalid preview is rejected before any renderer request");
    cancelVideoSettingsPreview();
    require(!takeVideoSettingsRequest(),"Cancel without any preview leaves renderer unchanged");

    require(requestVideoSettingsPreview(first),"queue preview before controller reinitialization");
    const auto priorInitialization = take("dispatch preview from previous initialization");
    clockTime = {}; initializeVideoSettings(path,fakeClock);
    require(currentVideoSettings() == VideoSettings{} && committedVideoSettings() == second,
        "startup distinguishes loaded committed settings from not-yet-applied settings");
    const auto newInitialization = take("new initialization queues fresh monotonic serial");
    require(newInitialization.serial > priorInitialization.serial,"serial does not reset across initialization");
    completeVideoSettingsRequest(priorInitialization.serial,true);
    require(currentVideoSettings() == VideoSettings{} && !videoSettingsStatus().previewActive,
        "acknowledgement from old initialization cannot alter new controller");
    completeVideoSettingsRequest(newInitialization.serial,true);
    require(currentVideoSettings() == second && !videoSettingsStatus().previewActive,"loaded settings become current only after startup apply");
}
void failedKeep(Fixture& fixture) {
    const auto path = fixture.path(L"keep failure.ini");
    std::string error;
    require(saveVideoSettings(path,VideoSettings{},error),"seed failure-to-save committed configuration");
    initialize(path);
    require(requestVideoSettingsPreview(configured()),"queue save-failure preview");
    const auto request = take("dispatch save-failure preview"); completeVideoSettingsRequest(request.serial,true);
    HANDLE held = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(held != INVALID_HANDLE_VALUE,"hold configuration to reject Keep atomic replacement");
    const bool kept = confirmVideoSettingsPreview(); CloseHandle(held);
    require(!kept && videoSettingsStatus().previewActive && !videoSettingsStatus().error.empty() &&
        committedVideoSettings() == VideoSettings{} && loadVideoSettings(path,error) == VideoSettings{},
        "failed persistence cannot silently commit or cancel the safety countdown");
    advance(std::chrono::seconds(15)); tickVideoSettings();
    const auto restore = take("failed Keep still automatically restores at deadline"); completeVideoSettingsRequest(restore.serial,true);
    require(currentVideoSettings() == VideoSettings{},"failed Keep eventually restores the last saved settings");
}
void recovery(Fixture& fixture) {
    const auto path = fixture.path(L"recovery.ini");
    const auto saved = configured();
    std::string error;
    require(saveVideoSettings(path,saved,error),"seed saved settings for startup recovery");
    clockTime = {}; initializeVideoSettings(path,fakeClock);
    const auto initial = take("dispatch saved startup preferences");
    completeVideoSettingsRequest(initial.serial,false,"Saved display is disconnected");
    require(currentVideoSettings() == VideoSettings{} && committedVideoSettings() == VideoSettings{} &&
        !takeVideoSettingsRequest() && videoSettingsStatus().error.find("Saved display is disconnected") != std::string::npos &&
        loadVideoSettings(path,error) == saved,"rejected startup settings use applied defaults as session undo target without rewriting file");
    auto preview = saved; preview.monitor = 0; preview.width = 1920; preview.height = 1080;
    require(requestVideoSettingsPreview(preview),"queue preview after startup defaults fallback");
    const auto first = take("dispatch preview after rejected startup"); completeVideoSettingsRequest(first.serial,true);
    cancelVideoSettingsPreview();
    const auto initialUndo = take("Cancel after rejected startup queues supported session defaults");
    require(initialUndo.settings == VideoSettings{},"startup undo does not retry rejected saved display");
    completeVideoSettingsRequest(initialUndo.serial,true);

    initialize(path);
    require(requestVideoSettingsPreview(preview),"queue successful preview before display disappears during restore");
    const auto applied = take("dispatch preview before restore failure"); completeVideoSettingsRequest(applied.serial,true);
    cancelVideoSettingsPreview();
    const auto restore = take("dispatch committed display restore");
    completeVideoSettingsRequest(restore.serial,false,"Committed display disappeared");
    require(!videoSettingsStatus().previewActive && currentVideoSettings() == preview &&
        committedVideoSettings() == saved,"restore rejection does not claim unapplied defaults or restart preview countdown");
    const auto fallback = take("restore failure queues exactly one conservative defaults recovery");
    require(fallback.settings == VideoSettings{} && fallback.serial > restore.serial,"recovery targets windowed primary-display defaults");
    completeVideoSettingsRequest(fallback.serial,true);
    require(currentVideoSettings() == VideoSettings{} && committedVideoSettings() == VideoSettings{} &&
        loadVideoSettings(path,error) == saved && !takeVideoSettingsRequest() && !confirmVideoSettingsPreview() &&
        videoSettingsStatus().error.find("Committed display disappeared") != std::string::npos &&
        videoSettingsStatus().error.find("Apply and Keep") != std::string::npos,
        "successful recovery updates only session state and explains how to save defaults explicitly");
    advance(std::chrono::seconds(30)); tickVideoSettings();
    require(!takeVideoSettingsRequest(),"successful recovery never retries the unavailable saved display");

    initialize(path);
    require(requestVideoSettingsPreview(preview),"queue preview before both restore and recovery fail");
    const auto second = take("dispatch preview for bounded recovery failure"); completeVideoSettingsRequest(second.serial,true);
    cancelVideoSettingsPreview();
    const auto failedRestore = take("dispatch restore that will fail");
    completeVideoSettingsRequest(failedRestore.serial,false,"Restore rejected");
    const auto failedFallback = take("dispatch the single recovery attempt");
    completeVideoSettingsRequest(failedFallback.serial,false,"Recovery rejected");
    require(currentVideoSettings() == preview && committedVideoSettings() == saved && !takeVideoSettingsRequest() &&
        !videoSettingsStatus().previewActive && !confirmVideoSettingsPreview() && loadVideoSettings(path,error) == saved &&
        videoSettingsStatus().error.find("Restore rejected") != std::string::npos &&
        videoSettingsStatus().error.find("Recovery rejected") != std::string::npos,
        "failed recovery stops after one attempt, reports both errors and preserves last applied state and file");
    advance(std::chrono::hours(1)); tickVideoSettings(); cancelVideoSettingsPreview();
    require(!takeVideoSettingsRequest(),"failed recovery does not loop on timers or Cancel");

    initialize(path);
    require(requestVideoSettingsPreview(preview),"queue preview for superseded recovery test");
    const auto third = take("dispatch preview for superseded recovery"); completeVideoSettingsRequest(third.serial,true);
    cancelVideoSettingsPreview();
    const auto supersededRestore = take("dispatch restore before recovery is superseded");
    completeVideoSettingsRequest(supersededRestore.serial,false,"Old restore error");
    const auto supersededFallback = take("dispatch fallback that will be superseded");
    require(requestVideoSettingsPreview(VideoSettings{}),"a new user apply supersedes queued defaults recovery");
    const auto latest = take("dispatch latest user preview after recovery");
    completeVideoSettingsRequest(supersededFallback.serial,true);
    require(currentVideoSettings() == preview && videoSettingsStatus().previewPending && videoSettingsStatus().error.empty(),
        "stale recovery cannot change current or committed state or reintroduce old error");
    completeVideoSettingsRequest(latest.serial,true);
    require(currentVideoSettings() == VideoSettings{} && videoSettingsStatus().previewActive &&
        videoSettingsStatus().secondsRemaining == 15 && confirmVideoSettingsPreview() && loadVideoSettings(path,error) == VideoSettings{},
        "new user preview after recovery can be explicitly kept and persisted");
}
}
int main() {
    try {
        Fixture fixture;
        validation(); persistence(fixture); previews(fixture); failedKeep(fixture); recovery(fixture);
        std::printf("Native video settings contract: %zu checks passed\n",checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"Native video settings contract failed: %s\n",error.what()); return 1;
    }
}
