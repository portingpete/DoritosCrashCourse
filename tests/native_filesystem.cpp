#include "runtime/native/runtime.h"
#include "runtime/native/filesystem.h"
#include "runtime/native/kernel_objects.h"
#include <cstdio>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
namespace Doritos::Native {
int runGuest(PPCContext&, uint8_t*, PPCFunc*) { fail("Filesystem contract unexpectedly launched guest code"); }
}
using namespace Doritos::Native;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    uint32_t data;
    PPCContext ctx{};
    explicit Fixture(Memory& guest) : data(guest.allocate(8192)) {
        guest.initializeThread(ctx, 0x100000);
        currentContext = &ctx;
    }
    void name(const char* text, uint32_t descriptor, uint32_t buffer) {
        const uint32_t length = uint32_t(strlen(text));
        memcpy(memory->base() + buffer, text, length);
        memory->write32(descriptor, (length << 16) | length);
        memory->write32(descriptor + 4, buffer);
    }
    void attributes(const char* text, uint32_t root = 0xfffffffd) {
        name(text, data + 32, data + 256);
        memory->write32(data + 16, root);
        memory->write32(data + 20, data + 32);
        memory->write32(data + 24, 0x40);
    }
    uint32_t call(const char* text) {
        require(filesystemImport(ctx, memory->base(), text), "unrecognized filesystem operation");
        return ctx.r3.u32;
    }
    uint32_t open(const char* text, uint32_t access = 0x100001, uint32_t options = 0x60, uint32_t root = 0xfffffffd) {
        attributes(text, root);
        ctx.r3.u64 = data; ctx.r4.u64 = access; ctx.r5.u64 = data + 16;
        ctx.r6.u64 = data + 8; ctx.r7.u64 = 7; ctx.r8.u64 = options;
        require(call("NtOpenFile") == 0, "native NtOpenFile");
        require(memory->read32(data + 8) == 0 && memory->read32(data + 12) == 1, "open IO_STATUS_BLOCK big endian layout");
        return memory->read32(data);
    }
    uint32_t transfer(uint32_t handle, const char* operation, uint32_t length, uint32_t offsetPointer = 0, uint32_t event = 0) {
        ctx.r3.u64 = handle; ctx.r4.u64 = event; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        ctx.r7.u64 = data + 8; ctx.r8.u64 = data + 1024; ctx.r9.u64 = length; ctx.r10.u64 = offsetPointer;
        return call(operation);
    }
};

void packageCacheContract(Fixture& test, const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = size_t(input.tellg());
    auto bytes = std::make_shared<std::vector<uint8_t>>(size);
    input.seekg(0); input.read(reinterpret_cast<char*>(bytes->data()), std::streamsize(size));
    require(bool(input), "read cache contract package snapshot");
    const uint32_t event = registerObject(CreateEventW(nullptr, TRUE, FALSE, nullptr), ObjectKind::Event);
    auto position = [&](uint32_t handle) {
        test.ctx.r3.u64 = handle; test.ctx.r4.u64 = test.data + 8;
        test.ctx.r5.u64 = test.data + 96; test.ctx.r6.u64 = 8; test.ctx.r7.u64 = 14;
        require(test.call("NtQueryInformationFile") == 0, "query package position");
        return memory->read64(test.data + 96);
    };
    for (const uint32_t options : {0x60u, 0x68u, 0x40u}) {
        retainFilesystemPackage({});
        const auto disk = test.open("game:\\data.pak", 0x100001, options);
        retainFilesystemPackage(bytes);
        const auto cached = test.open("dvd:\\DATA.PAK", 0x100001, options);
        require(!lookupObject(disk)->fileState->package && lookupObject(cached)->fileState->package,
            "only new exact package handles use immutable retained bytes");
        const auto duplicate = duplicateObject(cached);
        require(lookupObject(duplicate)->fileState == lookupObject(cached)->fileState,
            "duplicated cached handle shares cursor and snapshot lifetime");
        auto seek = [&](uint32_t handle, int64_t offset) {
            memory->write64(test.data + 64, uint64_t(offset));
            test.ctx.r3.u64 = handle; test.ctx.r4.u64 = test.data + 8;
            test.ctx.r5.u64 = test.data + 64; test.ctx.r6.u64 = 8; test.ctx.r7.u64 = 14;
            return test.call("NtSetInformationFile");
        };
        auto compare = [&](const char* label, uint32_t count, bool explicitOffset, int64_t offset = 0) {
            struct Result {
                uint32_t status, io, information; bool signalled; uint64_t position;
                std::array<uint8_t, 512> data;
                bool operator==(const Result&) const = default;
            };
            auto read = [&](uint32_t handle) {
                memset(memory->base() + test.data + 1024, 0xcc, 512);
                memory->write64(test.data + 64, uint64_t(offset));
                ResetEvent(lookupObject(event)->handle);
                Result result{};
                result.status = test.transfer(handle, "NtReadFile", count, explicitOffset ? test.data + 64 : 0, event);
                result.io = memory->read32(test.data + 8); result.information = memory->read32(test.data + 12);
                result.signalled = WaitForSingleObject(lookupObject(event)->handle, 0) == WAIT_OBJECT_0;
                memcpy(result.data.data(), memory->base() + test.data + 1024, 512);
                result.position = position(handle);
                return result;
            };
            const auto native = read(disk), ram = read(duplicate);
            if (!(native == ram)) {
                fprintf(stderr, "Package differential %s options=%X disk=%08X/%08X/%u/event%u/pos%llu ram=%08X/%08X/%u/event%u/pos%llu\n",
                    label, options, native.status, native.io, native.information, native.signalled, native.position,
                    ram.status, ram.io, ram.information, ram.signalled, ram.position);
                throw std::runtime_error("RAM package read differs from real Windows file behavior");
            }
        };
        compare("sequential", 512, false);
        compare("explicit", 512, true, 1024);
        compare("file pointer sentinel", 512, true, -2);
        compare("invalid sentinel", 512, true, -1);
        compare("invalid negative", 512, true, -3);
        compare("zero length", 0, true, 512);
        compare("EOF", 512, true, int64_t(size));
        compare("beyond EOF", 512, true, int64_t(size + 512));
        compare("zero beyond EOF", 0, true, int64_t(size + 512));
        compare("short final read", 32, true, int64_t(size - 7));
        compare("after EOF", 32, false);
        compare("unaligned", 512, true, 1);
        for (const int64_t offset : {int64_t(0), int64_t(7), int64_t(-1), int64_t(size + 512), int64_t(512)}) {
            require(seek(disk, offset) == seek(duplicate, offset), "cached seek has native validation/status");
            require(position(disk) == position(cached), "cached seek/duplicate cursor equals native cursor");
        }
        compare("after seek", 512, false);
        const auto reopened = test.open("game:\\data.pak", 0x100001, options);
        require(position(reopened) == 0, "independent package open starts at offset zero");
        closeObject(reopened); closeObject(duplicate); closeObject(cached); closeObject(disk);
    }
    const auto stats = filesystemPackageCacheStats();
    require(stats.residentBytes == size && stats.readCalls && stats.readBytes,
        "cache statistics prove full package retention and reads");
    retainFilesystemPackage({});
    require(!filesystemPackageCacheStats().residentBytes, "cache-off retains no package");
    closeObject(event);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        const auto gameRoot = std::filesystem::absolute(argv[1]).parent_path();
        wchar_t temp[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temp) > 0, "temporary filesystem contract root");
        const auto saveRoot = std::filesystem::path(temp) / (L"DoritosNativeFileContract-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        const auto overrides = saveRoot / "asset-overrides";
        std::filesystem::create_directories(overrides / "data/templates");
        { std::ofstream output(overrides / "data/templates/guigroup.wbr", std::ios::binary); output << "patched-menu"; }
        { std::ofstream output(overrides / "data/templates/unexpected.wbr", std::ios::binary); output << "not-whitelisted"; }
        { std::ofstream output(overrides / "data.pak", std::ios::binary); output << "must-not-replace-original-package"; }
        initializeFilesystem(gameRoot, saveRoot, overrides);
        require(filesystemHasAssetOverride("data/templates/guigroup.wbr") &&
            filesystemHasAssetOverride("DATA\\TEMPLATES\\GUIGROUP.WBR"), "original package policy selects only existing exact override files");
        require(!filesystemHasAssetOverride("data/templates/scripts.wbr") &&
            !filesystemHasAssetOverride("data.pak") && !filesystemHasAssetOverride("data/templates/unexpected.wbr") &&
            !filesystemHasAssetOverride("data/templates/../templates/guigroup.wbr"), "missing, foreign and traversal paths keep original package selection");
        Fixture test(guest);
        uint32_t asset = test.open("game:\\data.pak");
        require(test.transfer(asset, "NtReadFile", 16) == 0 && guest.read32(test.data + 12) == 16, "actual title package read");
        std::ifstream package(gameRoot / "data.pak", std::ios::binary);
        char original[16]{}; package.read(original, 16);
        require(memcmp(guest.base() + test.data + 1024, original, 16) == 0, "guest receives original package bytes");
        const uint32_t menu = test.open("dvd:\\data\\templates\\GUIGROUP.WBR");
        require(test.transfer(menu, "NtReadFile", 12) == 0 && guest.read32(test.data + 12) == 12 &&
            memcmp(guest.base() + test.data + 1024, "patched-menu", 12) == 0,
            "exact menu whitelist routes case-insensitive read through game volume aliases");
        require(test.transfer(menu, "NtWriteFile", 1) == 0xc00000a2, "override handles remain read only");
        const uint32_t menuDirectory = test.open("game:\\data\\templates\\", 0x100001, 0x21);
        const uint32_t relativeMenu = test.open("guigroup.wbr", 0x100001, 0x60, menuDirectory);
        require(test.transfer(relativeMenu, "NtReadFile", 12) == 0 &&
            memcmp(guest.base() + test.data + 1024, "patched-menu", 12) == 0,
            "directory-relative override opens retain the game namespace");
        test.attributes("unexpected.wbr", menuDirectory);
        test.ctx.r3.u64 = test.data; test.ctx.r4.u64 = 0x100001; test.ctx.r5.u64 = test.data + 16;
        test.ctx.r6.u64 = test.data + 8; test.ctx.r7.u64 = 7; test.ctx.r8.u64 = 0x60;
        require(int32_t(test.call("NtOpenFile")) < 0, "directory handles do not expose non-whitelisted overlay files");
        test.name("*", test.data + 48, test.data + 512);
        auto menuQuery = [&](bool restart) {
            guest.write32(test.ctx.r1.u32 + 84, restart);
            test.ctx.r3.u64 = menuDirectory; test.ctx.r4.u64 = 0; test.ctx.r5.u64 = 0; test.ctx.r6.u64 = 0;
            test.ctx.r7.u64 = test.data + 8; test.ctx.r8.u64 = test.data + 2048;
            test.ctx.r9.u64 = 512; test.ctx.r10.u64 = test.data + 48;
            return test.call("NtQueryDirectoryFile");
        };
        require(menuQuery(true) == 0 && guest.read32(test.data + 2048 + 60) == 12 &&
            !memcmp(guest.base() + test.data + 2048 + 64, "guigroup.wbr", 12), "override directory enumerates the actual whitelisted menu");
        require(menuQuery(false) == 0x80000006, "override enumeration omits unrelated files and directory entries");
        { std::ofstream output(overrides / "data/templates/bonedmodel.wbr", std::ios::binary); output << "declared-model"; }
        require(filesystemHasAssetOverride("data/templates/bonedmodel.wbr"),
            "only an existing owned BoneModel declaration selects the loose route");
        const uint32_t boneDeclaration = test.open("bonedmodel.wbr", 0x100001, 0x60, menuDirectory);
        require(test.transfer(boneDeclaration, "NtReadFile", 14) == 0 &&
            !memcmp(guest.base() + test.data + 1024, "declared-model", 14),
            "BoneModel declaration reads through the original directory-relative namespace");
        require(test.transfer(boneDeclaration, "NtWriteFile", 1) == 0xc00000a2,
            "BoneModel declaration remains read only");
        test.name("bonedmodel.wbr", test.data + 48, test.data + 512);
        require(menuQuery(true) == 0 &&
            guest.read32(test.data + 2048 + 60) == 14 &&
            !memcmp(guest.base() + test.data + 2048 + 64, "bonedmodel.wbr", 14),
            "filtered template enumeration discovers the owned BoneModel declaration");
        { std::ofstream output(overrides / "data/templates/shader.wbr",std::ios::binary); output << "declared-shader"; }
        const uint32_t shaderDeclaration = test.open("game:\\data\\templates\\shader.wbr");
        require(test.transfer(shaderDeclaration,"NtReadFile",15) == 0 &&
            !memcmp(guest.base()+test.data+1024,"declared-shader",15),"owned shader declaration reads through game namespace");
        test.name("shader.wbr",test.data+48,test.data+512);
        require(menuQuery(true) == 0 && guest.read32(test.data+2048+60) == 10 &&
            !memcmp(guest.base()+test.data+2048+64,"shader.wbr",10),"filtered declaration directory includes owned texture declarations");
        { std::ofstream output(overrides / "data/templates/texture.wbr",std::ios::binary); output << "declared-texture"; }
        require(filesystemHasAssetOverride("data/templates/texture.wbr"), "existing exact image declaration selects owned override");
        const uint32_t textureDeclaration = test.open("texture.wbr",0x100001,0x60,menuDirectory);
        require(test.transfer(textureDeclaration,"NtReadFile",15) == 0 && guest.read32(test.data + 12) == 15 &&
            !memcmp(guest.base()+test.data+1024,"declared-texture",15),"image declarations retain directory-relative game namespace");
        require(test.transfer(textureDeclaration,"NtWriteFile",1) == 0xc00000a2,"image declarations remain read only");
        test.name("texture.wbr",test.data+48,test.data+512);
        require(menuQuery(true) == 0 && guest.read32(test.data+2048+60) == 11 &&
            !memcmp(guest.base()+test.data+2048+64,"texture.wbr",11),"filtered template enumeration discovers image declarations");
        std::filesystem::create_directories(overrides / "data/art/_avatar");
        for(const char* nativeTemplate:{"guilabel.wbr","unistrings.wbr"}) {
            const auto logical=std::string("data/templates/")+nativeTemplate;
            {std::ofstream output(overrides/logical,std::ios::binary);output<<"native-video";}
            require(filesystemHasAssetOverride(logical),"native video label and strings select exact owned overlays");
            const auto handle=test.open(nativeTemplate,0x100001,0x60,menuDirectory);
            require(test.transfer(handle,"NtReadFile",12)==0 && !memcmp(guest.base()+test.data+1024,"native-video",12),
                "native label/string directory-relative opens retain game namespace");
            require(test.transfer(handle,"NtWriteFile",1)==0xc00000a2,"native label/string overlays remain read only");
            closeObject(handle);
        }
        const std::array<const char*,7> ownedCharacterFiles{"cj_native.cfg","cj_native.cmf","cj_native.mat.txt",
            "cj_native_0.png","cj_native_1.png","cj_native_2.png","cj_native_3.png"};
        for (const char* filename : ownedCharacterFiles) {
            const auto logical = std::string("data/art/_avatar/")+filename;
            { std::ofstream output(overrides/logical,std::ios::binary); output << "owned-cj"; }
            require(filesystemHasAssetOverride(logical),"exact existing CJ files select owned override");
            const auto file = test.open((std::string("dvd:\\")+logical).c_str());
            require(test.transfer(file,"NtReadFile",8) == 0 && !memcmp(guest.base()+test.data+1024,"owned-cj",8),
                "owned CJ mesh/config/material/textures read through original volume alias");
            require(test.transfer(file,"NtWriteFile",1) == 0xc00000a2,"owned CJ files remain read only");
            closeObject(file);
        }
        { std::ofstream output(overrides / "data/art/_avatar/cj_native_4.png",std::ios::binary); output << "decoy"; }
        require(!filesystemHasAssetOverride("data/art/_avatar/cj_native_4.png") &&
            !filesystemHasAssetOverride("data/art/_avatar/../_avatar/cj_native.cfg") &&
            !filesystemHasAssetOverride("data/art/_avatar/p_test.csf"),"foreign, traversal and original skeleton paths retain package policy");
        std::filesystem::create_directories(overrides / "data/art/dxshaders");
        { std::ofstream output(overrides / "data/art/dxshaders/precache_xenon.bsh",std::ios::binary); output << "source-cache"; }
        const auto sourceCache = test.open("game:\\data\\art\\dxshaders\\precache_xenon.bsh");
        require(test.transfer(sourceCache,"NtReadFile",12) == 0 && !memcmp(guest.base()+test.data+1024,"source-cache",12),
            "genuine extended original cache reads through exact shader filename");
        require(test.transfer(sourceCache,"NtWriteFile",1) == 0xc00000a2,"source cache addition remains read only");
        closeObject(sourceCache);
        test.attributes("game:\\data\\templates\\guigroup.wbr");
        test.ctx.r3.u64 = test.data; test.ctx.r4.u64 = GENERIC_WRITE; test.ctx.r5.u64 = test.data + 16;
        test.ctx.r6.u64 = test.data + 8; test.ctx.r7.u64 = 7; test.ctx.r8.u64 = 0x60;
        require(test.call("NtOpenFile") == 0xc00000a2, "override write opens are rejected");
        test.attributes("game:\\data\\templates\\..\\..\\default.xex");
        test.ctx.r3.u64 = test.data;
        test.ctx.r4.u64 = 0x100001;
        require(test.call("NtOpenFile") == 0xc0000033, "override routes retain traversal rejection");
        test.attributes("game:\\data.pak");
        test.ctx.r3.u64 = test.data; test.ctx.r4.u64 = GENERIC_WRITE; test.ctx.r5.u64 = test.data + 16;
        test.ctx.r6.u64 = test.data + 8; test.ctx.r7.u64 = 7; test.ctx.r8.u64 = 0x60;
        require(test.call("NtOpenFile") == 0xc00000a2, "game files are read only");
        test.attributes("save:\\..\\default.xex");
        test.ctx.r3.u64 = test.data; test.ctx.r4.u64 = 0x100001; test.ctx.r5.u64 = test.data + 16;
        test.ctx.r6.u64 = test.data + 8; test.ctx.r7.u64 = 7; test.ctx.r8.u64 = 0x60;
        require(test.call("NtOpenFile") == 0xc0000033, "guest path traversal is rejected");
        test.attributes("save:\\must-not-create.bin");
        guest.write32(test.ctx.r1.u32 + 84, 0x60);
        test.ctx.r3.u64 = 0x70000000; test.ctx.r4.u64 = 0x100003; test.ctx.r5.u64 = test.data + 16;
        test.ctx.r6.u64 = test.data + 8; test.ctx.r7.u64 = 0; test.ctx.r8.u64 = 0x80; test.ctx.r9.u64 = 7; test.ctx.r10.u64 = 2;
        require(test.call("NtCreateFile") == 0xc0000005 && !std::filesystem::exists(saveRoot / "must-not-create.bin"), "invalid output rejects creation before host mutation");
        const char* filename = "directory-entry-retry-preserves-this-long-name.bin";
        const std::string guestName = std::string("save:\\") + filename;
        test.attributes(guestName.c_str());
        test.ctx.r3.u64 = test.data; test.ctx.r4.u64 = 0x100003; test.ctx.r5.u64 = test.data + 16;
        test.ctx.r6.u64 = test.data + 8; test.ctx.r7.u64 = 0; test.ctx.r8.u64 = 0x80; test.ctx.r9.u64 = 7; test.ctx.r10.u64 = 2;
        require(test.call("NtCreateFile") == 0 && guest.read32(test.data + 12) == 2, "create disposition and stack options ABI");
        const uint32_t writable = guest.read32(test.data);
        memcpy(guest.base() + test.data + 1024, "native-save", 11);
        require(test.transfer(writable, "NtWriteFile", 11) == 0 && guest.read32(test.data + 12) == 11, "native save write completion");
        const uint32_t duplicate = duplicateObject(writable);
        require(duplicate && lookupObject(duplicate)->fileState == lookupObject(writable)->fileState, "duplicate shares file metadata");
        guest.write64(test.data + 64, 0);
        test.ctx.r3.u64 = duplicate; test.ctx.r4.u64 = test.data + 8; test.ctx.r5.u64 = test.data + 64; test.ctx.r6.u64 = 8; test.ctx.r7.u64 = 14;
        require(test.call("NtSetInformationFile") == 0, "shared file position seek");
        memset(guest.base() + test.data + 1024, 0, 32);
        const uint32_t completion = registerObject(CreateEventW(nullptr, TRUE, FALSE, nullptr), ObjectKind::Event);
        require(test.transfer(writable, "NtReadFile", 32, 0, completion) == 0 && guest.read32(test.data + 12) == 11 &&
            memcmp(guest.base() + test.data + 1024, "native-save", 11) == 0, "duplicate position affects original read");
        require(WaitForSingleObject(lookupObject(completion)->handle, 0) == WAIT_OBJECT_0, "actual I/O completion signals guest event");
        require(test.transfer(writable, "NtReadFile", 1) == 0xc0000011 && guest.read32(test.data + 12) == 0, "EOF status and zero byte count");
        test.ctx.r3.u64 = writable; test.ctx.r4.u64 = test.data + 8; test.ctx.r5.u64 = test.data + 96; test.ctx.r6.u64 = 24; test.ctx.r7.u64 = 5;
        require(test.call("NtQueryInformationFile") == 0 && guest.read64(test.data + 104) == 11, "file length query big endian LARGE_INTEGER");
        const uint32_t directory = test.open("save:\\", 0x100001, 0x21);
        test.name(filename, test.data + 48, test.data + 512);
        auto directoryQuery = [&](uint32_t length, bool restart) {
            guest.write32(test.ctx.r1.u32 + 84, restart);
            test.ctx.r3.u64 = directory; test.ctx.r4.u64 = 0; test.ctx.r5.u64 = 0; test.ctx.r6.u64 = 0;
            test.ctx.r7.u64 = test.data + 8; test.ctx.r8.u64 = test.data + 2048; test.ctx.r9.u64 = length; test.ctx.r10.u64 = test.data + 48;
            return test.call("NtQueryDirectoryFile");
        };
        require(directoryQuery(68, true) == 0x80000005 && guest.read32(test.data + 12) == 68, "directory overflow reports copied bytes");
        require(directoryQuery(512, false) == 0 && guest.read32(test.data + 2048 + 60) == strlen(filename) &&
            memcmp(guest.base() + test.data + 2048 + 64, filename, strlen(filename)) == 0, "directory overflow retry retains exact entry");
        require(directoryQuery(512, false) == 0x80000006, "directory iterator reaches end");
        test.ctx.r3.u64 = directory; test.ctx.r4.u64 = test.data + 8; test.ctx.r5.u64 = test.data + 96; test.ctx.r6.u64 = 24; test.ctx.r7.u64 = 3;
        require(test.call("NtQueryVolumeInformationFile") == 0 && guest.read64(test.data + 96) > 0 &&
            guest.read32(test.data + 116) >= 512, "host volume geometry translated to guest");
        test.name("\\??\\native-test:", test.data + 48, test.data + 512);
        test.name("save:\\", test.data + 56, test.data + 768);
        test.ctx.r3.u64 = test.data + 48; test.ctx.r4.u64 = test.data + 56;
        require(test.call("ObCreateSymbolicLink") == 0, "native guest symbolic alias");
        const uint32_t aliasFile = test.open((std::string("native-test:\\") + filename).c_str());
        require(test.transfer(aliasFile, "NtReadFile", 11) == 0 && guest.read32(test.data + 12) == 11, "symbolic alias routes actual file");
        test.ctx.r3.u64 = test.data + 48;
        require(test.call("ObDeleteSymbolicLink") == 0, "symbolic alias deletion");
        packageCacheContract(test, gameRoot / "data.pak");
        closeObject(aliasFile); closeObject(directory); closeObject(duplicate); closeObject(writable); closeObject(asset);
        closeObject(menu); closeObject(menuDirectory); closeObject(relativeMenu); closeObject(completion);
        closeObject(boneDeclaration);
        closeObject(shaderDeclaration);
        closeObject(textureDeclaration);
        for (const char* filename : ownedCharacterFiles)
            require(std::filesystem::remove(overrides / "data/art/_avatar" / filename),"remove exact owned character fixture");
        require(std::filesystem::remove(overrides / "data/art/_avatar/cj_native_4.png") &&
            std::filesystem::remove(overrides / "data/art/dxshaders/precache_xenon.bsh") &&
            std::filesystem::remove(overrides / "data/templates/shader.wbr") &&
            std::filesystem::remove(overrides / "data/templates/texture.wbr") &&
            std::filesystem::remove(overrides / "data/templates/guilabel.wbr") &&
            std::filesystem::remove(overrides / "data/templates/unistrings.wbr") &&
            std::filesystem::remove(overrides / "data/art/_avatar") &&
            std::filesystem::remove(overrides / "data/art/dxshaders") &&
            std::filesystem::remove(overrides / "data/art"),"remove only exact empty owned fixture directories");
        require(std::filesystem::remove(saveRoot / filename), "remove exact temporary contract file");
        require(std::filesystem::remove(overrides / "data/templates/guigroup.wbr") &&
            std::filesystem::remove(overrides / "data/templates/bonedmodel.wbr") &&
            std::filesystem::remove(overrides / "data/templates/unexpected.wbr") &&
            std::filesystem::remove(overrides / "data.pak") && std::filesystem::remove(overrides / "data/templates") &&
            std::filesystem::remove(overrides / "data") && std::filesystem::remove(overrides), "remove exact override contract files and empty directories");
        require(std::filesystem::remove(saveRoot / "cache") && std::filesystem::remove(saveRoot), "remove empty contract directories");
        currentContext = nullptr; memory = nullptr;
        fprintf(stderr, "Native file ABI, package read, save I/O, completion, metadata, directory retry and mount contracts passed.\n");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "Filesystem contract: %s\n", error.what()); return 1; }
}
