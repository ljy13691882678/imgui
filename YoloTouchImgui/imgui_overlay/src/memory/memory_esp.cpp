// memory_esp.cpp — 三角洲行动 内存透视/物资
// 独立线程用 TimeDriver 内核读写游戏内存，构建人物/物资快照；渲染线程投影绘制。
// 移植自 udp_decode 参考工程 (dfm_engine.h / engine.h / dfm_classification.h / esp_colors.h)。
#include "memory_esp.h"

#include "time_driver.h"
#include "item_database.h"
#include "udp_actors.h"

#include "ImGui/imgui.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cctype>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>

// ============================================================================
// DFM 内存偏移 (移植自 dfm_engine.h)
// ============================================================================
namespace dfmoff {
constexpr uint64_t GWorld                 = 0x1C9C0A50;
constexpr uint64_t GNames                 = 0x1CDCB8C0;

// UWorld
constexpr uint64_t PersistentLevel        = 0xF8;
constexpr uint64_t OwningGameInstance     = 0x190;

// UGameInstance
constexpr uint64_t LocalPlayers           = 0x38;

// UPlayer->PlayerController
constexpr uint64_t PlayerController       = 0x30;

// APlayerController
constexpr uint64_t AcknowledgedPawn       = 0x3F0;
constexpr uint64_t PlayerCameraManager    = 0x408;

// APlayerCameraManager
constexpr uint64_t CameraCachePrivate     = 0x2BA0;
constexpr uint64_t CamPOV                 = CameraCachePrivate + 0x10;

// ULevel
constexpr uint64_t AActors                = 0x98;
constexpr uint64_t ActorCount             = 0xA0;

// AActor / USceneComponent
constexpr uint64_t RootComponent          = 0x180;
constexpr uint64_t ComponentToWorld       = 0x210;
constexpr uint64_t Translation            = 0x220;

// ACharacter
constexpr uint64_t Mesh                   = 0x3D0;
constexpr uint64_t Mesh_BoneArray         = 0x730;
constexpr uint64_t BoneTransformStride    = 0x30;

// APawn / PlayerState
constexpr uint64_t PlayerState            = 0x390;
constexpr uint64_t PS_PlayerName          = 0x378;
constexpr uint64_t PS_PlayerNamePrivate   = 0x470;
constexpr uint64_t PS_bIsPlayerAI         = 0x510;
constexpr uint64_t PS_TeamID              = 0x658;

// AGPCharacterBase
constexpr uint64_t HealthComp             = 0x10C8;
constexpr uint64_t TeamComp               = 0x10D0;
constexpr uint64_t TeamComp_TeamID        = 0x108;
constexpr uint64_t BlackboardComp         = 0x1030;
constexpr uint64_t BB_CharacterLiveStatus = 0x391;

// UGPAttributeSetHealth (相对 HealthSet)
constexpr uint64_t HealthSet              = 0x280;
constexpr uint64_t Attr_Health            = 0x30;
constexpr uint64_t Attr_MaxHealth         = 0x48;
constexpr uint64_t Attr_Current           = 0xC;

// 存活状态枚举
constexpr int Live_Alive = 1;
constexpr int Live_Death = 2;
constexpr int Live_Downed = 3;

// UObject / FNamePool
constexpr uint64_t UObject_ClassPrivate   = 0x8;
constexpr uint64_t UObject_NamePrivate    = 0x1C;
constexpr uint64_t FNamePool_Blocks       = 0x38;
constexpr uint64_t FNamePool_Stride       = 0x2;
constexpr uint32_t FNamePool_BlocksBit    = 18;
constexpr uint64_t FNameEntry_Header      = 0x0;

// 物资
constexpr uint64_t Pickup_InventoryIdName = 0xF98;
constexpr uint64_t Pickup_ItemInfo        = 0x11E8;
constexpr uint64_t ItemInfo_Category      = 0x10;
constexpr uint64_t ItemInfo_Sequence      = 0x14;
constexpr uint64_t ItemInfo_ItemCount     = 0x38;
constexpr uint64_t Pickup_ValuePtr        = 0x11A8;
constexpr uint64_t PickupValue_Value      = 0xDC;

constexpr uint64_t SIC_BoxId              = 0x1070;
constexpr uint64_t SIC_ContainerType      = 0x10E0;
constexpr uint64_t SIC_CachedOpenBox      = 0x1128;
constexpr uint64_t SIC_bFinished          = 0x1158;
constexpr uint64_t SIC_bSafeBoxUnlocked   = 0x1159;

constexpr uint64_t OpenBox_RealSafeBoxPwd = 0x2478;
constexpr uint64_t OpenBox_SafeBoxPwd     = 0x2460;

constexpr uint64_t HackPC_Password        = 0x1074;
constexpr uint64_t CodedLock_PwdStr       = 0x10E0;

constexpr uint64_t DeadBody_PlayerName    = 0x2520;
constexpr uint64_t DeadBody_IsAI          = 0x2662;
constexpr uint64_t DeadBody_Looted        = 0x2678;
constexpr uint64_t DeadBody_OwnerPS       = 0x2680;

constexpr int kDfmBoneIndices[15] = {31, 30, 1, 34, 6, 35, 7, 36, 8, 58, 62, 59, 63, 60, 64};
} // namespace dfmoff

// ============================================================================
// 工具：UTF16→UTF8、投影、四元数旋转、颜色
// ============================================================================
namespace {

inline std::string Utf16ToUtf8(const std::u16string &str16) {
    std::string str8;
    str8.reserve(str16.length() * 3);
    for (char16_t c : str16) {
        if (c < 0x80) {
            str8.push_back((char)c);
        } else if (c < 0x800) {
            str8.push_back((char)((c >> 6) | 0xC0));
            str8.push_back((char)((c & 0x3F) | 0x80));
        } else {
            str8.push_back((char)((c >> 12) | 0xE0));
            str8.push_back((char)(((c >> 6) & 0x3F) | 0x80));
            str8.push_back((char)((c & 0x3F) | 0x80));
        }
    }
    return str8;
}

inline float ClampFloat(float v, float mn, float mx) {
    return v < mn ? mn : (v > mx ? mx : v);
}

inline float GetDistance(const MemVec3 &a, const MemVec3 &b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline MemVec2 WorldToScreenCamera(const MemVec3 &wp, const MemCamera &cam, float sx, float sy) {
    constexpr float pi = 3.14159265358979323846f;
    const float pitch = cam.Rotation.x * pi / 180.0f;
    const float yaw   = cam.Rotation.y * pi / 180.0f;
    const float roll  = cam.Rotation.z * pi / 180.0f;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy_ = std::sin(yaw),   cy = std::cos(yaw);
    const float sr = std::sin(roll),   cr = std::cos(roll);

    const MemVec3 axisX = {cp * cy, cp * sy_, sp};
    const MemVec3 axisY = {sr * sp * cy - cr * sy_,
                           sr * sp * sy_ + cr * cy,
                           -sr * cp};
    const MemVec3 axisZ = {-(cr * sp * cy + sr * sy_),
                           cy * sr - cr * sp * sy_,
                           cr * cp};

    const MemVec3 d = {wp.x - cam.Location.x, wp.y - cam.Location.y, wp.z - cam.Location.z};

    const MemVec3 tr = {d.x * axisY.x + d.y * axisY.y + d.z * axisY.z,
                        d.x * axisZ.x + d.y * axisZ.y + d.z * axisZ.z,
                        d.x * axisX.x + d.y * axisX.y + d.z * axisX.z};

    if (tr.z < 1.0f || cam.FOV <= 1.0f)
        return {-9999.0f, -9999.0f};

    const float cx = sx * 0.5f, cyy = sy * 0.5f;
    const float scale = cx / std::tan(cam.FOV * 0.5f * pi / 180.0f);
    return {cx + tr.x * scale / tr.z, cyy - tr.y * scale / tr.z};
}

inline MemVec3 QuatRotate(const MemQuat &q, const MemVec3 &v) {
    const MemVec3 u{q.X, q.Y, q.Z};
    const float s = q.W;
    // u * (2*uv) + v*(s*s - uu) + (u x v) * (2s)
    const float uv = u.x * v.x + u.y * v.y + u.z * v.z;
    const float uu = u.x * u.x + u.y * u.y + u.z * u.z;
    const MemVec3 uxuv = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    MemVec3 r;
    r.x = u.x * (2.0f * uv) + v.x * (s * s - uu) + uxuv.x * (2.0f * s);
    r.y = u.y * (2.0f * uv) + v.y * (s * s - uu) + uxuv.y * (2.0f * s);
    r.z = u.z * (2.0f * uv) + v.z * (s * s - uu) + uxuv.z * (2.0f * s);
    return r;
}

inline MemVec3 MultiplyBone(const MemTransform &parent, const MemTransform &local) {
    MemTransform r;
    r.Rotation.W = parent.Rotation.W * local.Rotation.W - parent.Rotation.X * local.Rotation.X -
                   parent.Rotation.Y * local.Rotation.Y - parent.Rotation.Z * local.Rotation.Z;
    r.Rotation.X = parent.Rotation.W * local.Rotation.X + parent.Rotation.X * local.Rotation.W +
                   parent.Rotation.Y * local.Rotation.Z - parent.Rotation.Z * local.Rotation.Y;
    r.Rotation.Y = parent.Rotation.W * local.Rotation.Y - parent.Rotation.X * local.Rotation.Z +
                   parent.Rotation.Y * local.Rotation.W + parent.Rotation.Z * local.Rotation.X;
    r.Rotation.Z = parent.Rotation.W * local.Rotation.Z + parent.Rotation.X * local.Rotation.Y -
                   parent.Rotation.Y * local.Rotation.X + parent.Rotation.Z * local.Rotation.W;
    MemVec3 sl = {local.translation.x * parent.Scale3D.x,
                  local.translation.y * parent.Scale3D.y,
                  local.translation.z * parent.Scale3D.z};
    MemVec3 rot = QuatRotate(parent.Rotation, sl);
    r.translation = {parent.translation.x + rot.x,
                     parent.translation.y + rot.y,
                     parent.translation.z + rot.z};
    r.Scale3D = {parent.Scale3D.x * local.Scale3D.x,
                 parent.Scale3D.y * local.Scale3D.y,
                 parent.Scale3D.z * local.Scale3D.z};
    return r.translation;
}

// —— 配色 (移植自 esp_colors.h) ——
inline ImU32 GetCategoryColor(int cat) {
    switch (cat) {
    case 1: return IM_COL32(60, 255, 60, 255);
    case 2: return IM_COL32(255, 215, 0, 255);
    case 3: return IM_COL32(255, 255, 255, 255);
    default: return IM_COL32(200, 200, 200, 255);
    }
}
inline ImU32 GetEquipLevelColor(int lv) {
    switch (lv) {
    case 1: return IM_COL32(255, 255, 255, 255);
    case 2: return IM_COL32(0, 255, 0, 255);
    case 3: return IM_COL32(30, 120, 255, 255);
    case 4: return IM_COL32(170, 0, 255, 255);
    case 5: return IM_COL32(255, 195, 0, 255);
    case 6: return IM_COL32(255, 0, 0, 255);
    default: return IM_COL32(160, 160, 160, 255);
    }
}
inline ImU32 GetRatioColor(float ratio) {
    ratio = ClampFloat(ratio, 0.0f, 1.0f);
    if (ratio > 0.5f) {
        const float t = (1.0f - ratio) * 2.0f;
        return IM_COL32((uint8_t)(t * 255.0f), 255, 0, 255);
    }
    const float t = (0.5f - ratio) * 2.0f;
    return IM_COL32(255, (uint8_t)((1.0f - t) * 255.0f), 0, 255);
}
inline ImU32 GetTeamColor(int teamId) {
    if (teamId <= 0) return IM_COL32(160, 160, 160, 255);
    static const ImU32 pal[] = {
        IM_COL32(255, 60, 60, 255), IM_COL32(255, 150, 30, 255), IM_COL32(255, 230, 0, 255),
        IM_COL32(60, 255, 60, 255), IM_COL32(0, 220, 220, 255), IM_COL32(60, 140, 255, 255),
        IM_COL32(170, 60, 255, 255), IM_COL32(255, 90, 200, 255), IM_COL32(170, 255, 60, 255),
        IM_COL32(220, 160, 80, 255)};
    return pal[(teamId - 1) % 10];
}
inline ImU32 GetDistanceColor(float dm) {
    constexpr float kNear = 10.0f, kFar = 150.0f;
    const float t = ClampFloat((dm - kNear) / (kFar - kNear), 0.0f, 1.0f);
    return IM_COL32((uint8_t)((1.0f - t) * 255.0f), (uint8_t)(t * 255.0f), 0, 255);
}
inline float GetWidthFloor(float dm) {
    constexpr float nearD = 10.0f, farD = 100.0f, nearW = 60.0f, farW = 50.0f;
    if (dm <= nearD) return 0.0f;
    const float t = ClampFloat((dm - nearD) / (farD - nearD), 0.0f, 1.0f);
    return nearW + t * (farW - nearW);
}
inline bool IsValidScreen(const ImVec2 &p) {
    return p.x != 0.0f && p.y != 0.0f && p.x > -1000.0f && p.y > -1000.0f;
}
} // namespace

// ============================================================================
// 分类
// ============================================================================
namespace {
inline bool IsAICharacter(const std::string &c) {
    return c.rfind("BP_DFMCharacter_AI", 0) == 0 ||
           c.rfind("NC_BP_DFMCharacter_AI", 0) == 0 ||
           c.rfind("BP_DFMAICharacter", 0) == 0 ||
           c.rfind("NC_BP_DFMAICharacter", 0) == 0 ||
           c == "BP_DFMCharacter_TutorialPlayerAi_C" ||
           c == "NC_BP_DFMCharacter_TutorialPlayerAi_C";
}
inline bool IsPlayerCharacter(const std::string &c) {
    return c == "BP_DFMCharacter_C" ||
           c == "NC_BP_DFMCharacter_C" ||
           c == "BP_AZCharacter_C" ||
           c == "NC_BP_AZCharacter_C";
}
inline bool IsTargetCharacter(const std::string &c) {
    return c == "BP_RangeTargetCharacter_C";
}
inline int GetCharacterCategory(const std::string &c) {
    if (IsPlayerCharacter(c)) return 1;
    if (IsTargetCharacter(c)) return 2;
    if (IsAICharacter(c)) return 3;
    return 0;
}
inline int GetLootCategory(const std::string &c) {
    if (c.find("DeadBody") != std::string::npos) return MemLoot_DeadBody;
    if (c.find("CodedLock") != std::string::npos) return MemLoot_CodedLock;
    if (c.find("HackPC") != std::string::npos || c.find("Computer") != std::string::npos)
        return MemLoot_Computer;
    const bool isPickup = c.find("InventoryPickup") != std::string::npos;
    const bool isScene = !isPickup &&
                         (c.find("Interactor_Container") != std::string::npos ||
                          c.find("SingleItemContainer") != std::string::npos);
    if (isScene) {
        if (c.find("SafeBox") != std::string::npos) return MemLoot_SafeBox;
        return MemLoot_Container;
    }
    if (isPickup) return MemLoot_GroundItem;
    return MemLoot_None;
}
} // namespace

// ============================================================================
// 内存读取
// ============================================================================
namespace {
class MemRW {
public:
    bool connected = false;
    pid_t pid = 0;
    uint64_t libUE4 = 0;

    bool ok(uint64_t a) const { return connected && pid > 0 && a > 0x1000000ULL && a < 0x100000000000ULL; }
    bool R(uint64_t a, void *b, size_t n) const {
        if (!ok(a) || !TIME_Driver) return false;
        return TIME_Driver->Read_Memory_Fast(pid, a, b, n);
    }
    template <class T> bool RM(T &o, uint64_t a) const { return R(a, &o, sizeof(o)); }
    template <class T> T read(uint64_t a) const { T v{}; RM(v, a); return v; }
};
} // namespace

// ============================================================================
// FName 解析
// ============================================================================
namespace {
// 三角洲 FName 字符串解密
inline std::string DecryptName(std::string name) {
    const uint32_t len = (uint32_t)name.length();
    if (len == 0) return name;
    uint32_t key = 0;
    switch (len % 9) {
    case 0: key = (len & 0x1F) + len; break;
    case 1: key = (len ^ 0xDF) + len; break;
    case 2: key = (len | 0xCF) + len; break;
    case 3: key = 33 * len; break;
    case 4: key = len + (len >> 2); break;
    case 5: key = 3 * len + 5; break;
    case 6: key = ((4 * len) | 5) + len; break;
    case 7: key = ((len >> 4) | 7) + len; break;
    case 8: key = (len ^ 0x0C) + len; break;
    default: key = (len ^ 0x40) + len; break;
    }
    for (char &c : name)
        c = (char)((key & 0x80) ^ ~c);
    return name;
}

inline std::string GetFNameEntry(const MemRW &rw, uint64_t entry) {
    if (!entry) return "";
    const uint16_t header = rw.read<uint16_t>(entry + dfmoff::FNameEntry_Header);
    const uint32_t len = header >> 6;
    if (len == 0 || len > 127) return "";
    const bool wide = (header & 0x1) != 0;
    if (wide) {
        std::vector<char16_t> buf(len + 1);
        if (!rw.R(entry + 2, buf.data(), len * sizeof(char16_t))) return "";
        buf[len] = 0;
        return Utf16ToUtf8(std::u16string(buf.data()));
    }
    char buf[128] = {};
    if (!rw.R(entry + 2, buf, len)) return "";
    buf[len] = 0;
    return std::string(buf);
}

inline std::string ResolveFName(const MemRW &rw, uint32_t idx) {
    const uint32_t blockId = idx >> dfmoff::FNamePool_BlocksBit;
    const uint32_t offset = idx & ((1u << dfmoff::FNamePool_BlocksBit) - 1u);
    if (blockId >= 8192 || !rw.libUE4) return "";
    const uint64_t pool = rw.libUE4 + dfmoff::GNames;
    const uint64_t block = rw.read<uint64_t>(pool + dfmoff::FNamePool_Blocks + blockId * sizeof(uint64_t));
    if (!block) return "";
    return DecryptName(GetFNameEntry(rw, block + offset * dfmoff::FNamePool_Stride));
}

inline std::string GetClassName(const MemRW &rw, uint64_t actor) {
    if (!actor) return "";
    const uint64_t cls = rw.read<uint64_t>(actor + dfmoff::UObject_ClassPrivate);
    if (!cls) return "";
    const uint32_t nameIdx = rw.read<uint32_t>(cls + dfmoff::UObject_NamePrivate);
    return ResolveFName(rw, nameIdx);
}

inline std::string ReadFString16(const MemRW &rw, uint64_t addr) {
    if (!addr) return "";
    const uint64_t data = rw.read<uint64_t>(addr);
    const int32_t len = rw.read<int32_t>(addr + 0x8);
    const int32_t max = rw.read<int32_t>(addr + 0xC);
    if (!data || len <= 0 || len > 64 || max <= 0 || len > max) return "";
    std::vector<char16_t> buf((size_t)len + 1);
    if (!rw.R(data, buf.data(), (size_t)len * sizeof(char16_t))) return "";
    buf[len] = 0;
    return Utf16ToUtf8(std::u16string(buf.data()));
}

inline std::string ReadPwdDigitArray(const MemRW &rw, uint64_t addr) {
    const uint64_t data = rw.read<uint64_t>(addr);
    const int32_t num = rw.read<int32_t>(addr + 0x8);
    if (!data || num <= 0 || num > 12) return "";
    std::vector<int32_t> d((size_t)num);
    if (!rw.R(data, d.data(), d.size() * sizeof(int32_t))) return "";
    std::string out;
    for (int32_t v : d) {
        if (v < 0 || v > 9) return "";
        out.push_back((char)('0' + v));
    }
    return out;
}
} // namespace

// ============================================================================
// 全局快照 / 生命周期
// ============================================================================
static std::thread gThread;
static std::atomic<bool> gRun{false};
static MemEspSnapshot gSnap;
static std::mutex gSnapMutex;
static std::atomic<bool> gUdpDecrypt{false}; // UDP 明文解包绘制开关

// ============================================================================
// 快照采集
// ============================================================================
namespace {

// 通过 /proc 按包名查 PID
inline pid_t FindPidByPackage(const char *pkg) {
    DIR *dir = opendir("/proc");
    if (!dir) return 0;
    struct dirent *ent;
    char path[64], line[512];
    pid_t found = 0;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, line, sizeof(line) - 1);
        close(fd);
        if (n <= 0) continue;
        line[n] = 0;
        if (strstr(line, pkg)) { found = (pid_t)atoi(ent->d_name); break; }
    }
    closedir(dir);
    return found;
}

inline bool GetWorldPos(const MemRW &rw, uint64_t actor, MemVec3 &out) {
    out = {0, 0, 0};
    const uint64_t rc = rw.read<uint64_t>(actor + dfmoff::RootComponent);
    if (!rc) return false;
    if (!rw.RM(out, rc + dfmoff::Translation)) return false;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

inline int GetTeamId(const MemRW &rw, uint64_t actor) {
    const uint64_t tc = rw.read<uint64_t>(actor + dfmoff::TeamComp);
    int t = 0;
    if (tc) t = rw.read<int>(tc + dfmoff::TeamComp_TeamID);
    if (t <= 0) {
        const uint64_t ps = rw.read<uint64_t>(actor + dfmoff::PlayerState);
        if (ps) t = rw.read<int>(ps + dfmoff::PS_TeamID);
    }
    return t;
}

inline void ReadHealth(const MemRW &rw, uint64_t actor, float &h, float &mh) {
    h = mh = 0.0f;
    const uint64_t hc = rw.read<uint64_t>(actor + dfmoff::HealthComp);
    if (!hc) return;
    const uint64_t hs = rw.read<uint64_t>(hc + dfmoff::HealthSet);
    if (!hs) return;
    h = rw.read<float>(hs + dfmoff::Attr_Health + dfmoff::Attr_Current);
    mh = rw.read<float>(hs + dfmoff::Attr_MaxHealth + dfmoff::Attr_Current);
}

inline bool IsAlive(const MemRW &rw, uint64_t actor, float health, float maxHealth) {
    const uint64_t bb = rw.read<uint64_t>(actor + dfmoff::BlackboardComp);
    int live = 0;
    if (bb) live = rw.read<uint8_t>(bb + dfmoff::BB_CharacterLiveStatus);
    if (live != 0) return live != dfmoff::Live_Death;
    return maxHealth > 0.0f && health > 0.0f;
}

inline void ReadPlayerName(const MemRW &rw, uint64_t actor, char *outName, size_t cap) {
    outName[0] = 0;
    const uint64_t ps = rw.read<uint64_t>(actor + dfmoff::PlayerState);
    if (!ps) return;
    std::string nm = ReadFString16(rw, ps + dfmoff::PS_PlayerName);
    if (nm.empty()) nm = ReadFString16(rw, ps + dfmoff::PS_PlayerNamePrivate);
    if (!nm.empty())
        snprintf(outName, cap, "%s", nm.c_str());
}

inline bool ReadBones(const MemRW &rw, MemEspPlayer &p) {
    const uint64_t mesh = rw.read<uint64_t>(p.actor + dfmoff::Mesh);
    if (!mesh) return false;
    MemTransform ct = rw.read<MemTransform>(mesh + dfmoff::ComponentToWorld);
    ct.translation = p.worldPos;
    ct.Scale3D = MemVec3{1.0f, 1.0f, 1.0f};
    const uint64_t rawBone = rw.read<uint64_t>(mesh + dfmoff::Mesh_BoneArray);
    const uint64_t boneArr = rawBone & 0x00FFFFFFFFFFFFFFULL;
    if (boneArr < 0x10000000ULL || boneArr >= 0x10000000000ULL) return false;
    for (int i = 0; i < MEM_ESP_BONE_COUNT; ++i) {
        const uint64_t ba = boneArr + (uint64_t)dfmoff::kDfmBoneIndices[i] * dfmoff::BoneTransformStride;
        const MemTransform bl = rw.read<MemTransform>(ba);
        const MemVec3 w = MultiplyBone(ct, bl);
        if (!std::isfinite(w.x) || !std::isfinite(w.y) || !std::isfinite(w.z)) return false;
        p.bones[i] = w;
    }
    return true;
}

inline uint64_t GetPickupItemId(const MemRW &rw, uint64_t actor) {
    if (!actor) return 0;
    const uint32_t invNameIdx = rw.read<uint32_t>(actor + dfmoff::Pickup_InventoryIdName);
    std::string invName = ResolveFName(rw, invNameIdx);
    if (!invName.empty() && invName != "None") {
        const uint64_t parsed = strtoull(invName.c_str(), nullptr, 10);
        if (parsed != 0) return parsed;
    }
    const uint64_t base = actor + dfmoff::Pickup_ItemInfo;
    const uint32_t cat = rw.read<uint32_t>(base + dfmoff::ItemInfo_Category);
    const uint32_t seq = rw.read<uint32_t>(base + dfmoff::ItemInfo_Sequence);
    if (cat == 0) return 0;
    return (uint64_t)cat * 10000ULL + seq;
}

inline void Capture(MemRW &rw, MemEspSnapshot &snap) {
    snap = MemEspSnapshot();
    snap.ts = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();

    uint64_t myPawn = 0; // 本机 Pawn：绘制时排除自己
    if (!rw.connected || !rw.libUE4) {
        snap.status = "驱动未连接或未找到 libUE4";
        return;
    }

    const uint64_t uworld = rw.read<uint64_t>(rw.libUE4 + dfmoff::GWorld);
    if (!uworld) { snap.status = "UWorld=0 (未进对局)"; return; }

    // 相机 (本地渲染层)
    {
        const uint64_t ginst = rw.read<uint64_t>(uworld + dfmoff::OwningGameInstance);
        const uint64_t lpArr = ginst ? rw.read<uint64_t>(ginst + dfmoff::LocalPlayers) : 0;
        const uint64_t lplayer = lpArr ? rw.read<uint64_t>(lpArr) : 0;
        const uint64_t pc = lplayer ? rw.read<uint64_t>(lplayer + dfmoff::PlayerController) : 0;
        const uint64_t camMgr = pc ? rw.read<uint64_t>(pc + dfmoff::PlayerCameraManager) : 0;
        if (camMgr) {
            const uint64_t pov = camMgr + dfmoff::CamPOV;
            MemVec3 loc{}; MemRotator rot{}; float fov = 90.0f;
            if (rw.RM(loc, pov) && rw.RM(rot, pov + 0x10)) {
                snap.camera.Location = loc;
                snap.camera.Rotation = rot;
                rw.RM(fov, pov + 0x1C);
                snap.camera.FOV = fov > 1.0f ? fov : 90.0f;
            }
            myPawn = rw.read<uint64_t>(pc + dfmoff::AcknowledgedPawn);
            if (myPawn) {
                snap.myTeam = GetTeamId(rw, myPawn);
                // UDP 解包用自机坐标锚定原点；这里同时留存当前自机坐标供面板显示
                const uint64_t pmrc = rw.read<uint64_t>(myPawn + dfmoff::RootComponent);
                if (pmrc) {
                    MemVec3 sp{};
                    if (rw.RM(sp, pmrc + dfmoff::Translation))
                        snap.selfPos = sp;
                }
            }
        } else {
            snap.status = "未取到相机";
        }
    }

    // Actor 数组
    const uint64_t pl = rw.read<uint64_t>(uworld + dfmoff::PersistentLevel);
    if (!pl) { snap.status = "PersistentLevel=0"; return; }
    const uint64_t arr = rw.read<uint64_t>(pl + dfmoff::AActors);
    int32_t count = rw.read<int32_t>(pl + dfmoff::ActorCount);
    if (!arr || count <= 0 || count > 65536) { snap.status = "无 Actor 数组"; return; }

    std::vector<uint64_t> actors((size_t)count);
    if (!rw.R(arr, actors.data(), actors.size() * sizeof(uint64_t))) { snap.status = "读 Actor 失败"; return; }
    snap.actorCount = count;

    // 人物默认身高 (世界单位≈cm)，用于无骨骼时方框推算
    (void)snap.playerCount;

    for (uint64_t actor : actors) {
        if (!actor) continue;
        if (actor == myPawn) continue; // 不绘制自己

        const std::string cls = GetClassName(rw, actor);
        if (cls.empty()) continue;

        const int cat = GetCharacterCategory(cls);
        if (cat == 0) {
            // 物资
            const int lootCat = GetLootCategory(cls);
            if (lootCat == MemLoot_None) continue;
            MemEspLoot L;
            L.actor = actor;
            L.cat = lootCat;
            if (!GetWorldPos(rw, actor, L.worldPos)) continue;
            switch (lootCat) {
            case MemLoot_GroundItem: {
                L.itemId = GetPickupItemId(rw, actor);
                if (L.itemId == 0) continue;
                L.count = rw.read<int32_t>(actor + dfmoff::Pickup_ItemInfo + dfmoff::ItemInfo_ItemCount);
                const uint64_t vp = rw.read<uint64_t>(actor + dfmoff::Pickup_ValuePtr);
                if (vp) L.value = rw.read<int32_t>(vp + dfmoff::PickupValue_Value);
                const ItemDbEntry *e = FindItemById(L.itemId);
                L.grade = e ? e->Grade : 0;
                break;
            }
            case MemLoot_Container: {
                L.finished = (rw.read<uint8_t>(actor + dfmoff::SIC_bFinished) & 0x1) != 0;
                const uint8_t t = rw.read<uint8_t>(actor + dfmoff::SIC_ContainerType);
                switch (t) {
                case 2: snprintf(L.label, sizeof(L.label), "武器箱"); break;
                case 3: snprintf(L.label, sizeof(L.label), "储物柜"); break;
                case 4: snprintf(L.label, sizeof(L.label), "垃圾箱"); break;
                case 7: snprintf(L.label, sizeof(L.label), "水泥车"); break;
                default: snprintf(L.label, sizeof(L.label), "容器"); break;
                }
                break;
            }
            case MemLoot_DeadBody: {
                const uint64_t ownerPS = rw.read<uint64_t>(actor + dfmoff::DeadBody_OwnerPS);
                if (ownerPS)
                    L.isBot = (rw.read<uint8_t>(ownerPS + dfmoff::PS_bIsPlayerAI) & 0x1) != 0;
                else
                    L.isBot = (rw.read<uint8_t>(actor + dfmoff::DeadBody_IsAI) & 0x1) != 0;
                L.finished = (rw.read<uint8_t>(actor + dfmoff::DeadBody_Looted) & 0x1) != 0;
                const std::string nm = ReadFString16(rw, actor + dfmoff::DeadBody_PlayerName);
                if (L.isBot)
                    snprintf(L.label, sizeof(L.label), "人机遗物%s", L.finished ? " (已搜刮)" : "");
                else if (!nm.empty())
                    snprintf(L.label, sizeof(L.label), "%s%s", nm.c_str(), L.finished ? " (已搜刮)" : "");
                else
                    snprintf(L.label, sizeof(L.label), "玩家遗物%s", L.finished ? " (已搜刮)" : "");
                break;
            }
            case MemLoot_SafeBox: {
                L.finished = (rw.read<uint8_t>(actor + dfmoff::SIC_bSafeBoxUnlocked) & 0x1) != 0;
                snprintf(L.label, sizeof(L.label), L.finished ? "保险箱 (已开)" : "保险箱");
                const uint64_t ob = rw.read<uint64_t>(actor + dfmoff::SIC_CachedOpenBox);
                if (ob) {
                    std::string pwd = ReadPwdDigitArray(rw, ob + dfmoff::OpenBox_RealSafeBoxPwd);
                    if (pwd.empty()) pwd = ReadPwdDigitArray(rw, ob + dfmoff::OpenBox_SafeBoxPwd);
                    if (!pwd.empty()) snprintf(L.label, sizeof(L.label), "保险箱 密码%s", pwd.c_str());
                }
                break;
            }
            case MemLoot_Computer: {
                const int32_t pwd = rw.read<int32_t>(actor + dfmoff::HackPC_Password);
                if (pwd > 0 && pwd <= 99999999)
                    snprintf(L.label, sizeof(L.label), "电脑 密码%d", (int)pwd);
                else
                    snprintf(L.label, sizeof(L.label), "电脑");
                break;
            }
            case MemLoot_CodedLock: {
                std::string pwd = ReadFString16(rw, actor + dfmoff::CodedLock_PwdStr);
                if (pwd.size() <= 12)
                    snprintf(L.label, sizeof(L.label), "密码门 %s", pwd.c_str());
                else
                    snprintf(L.label, sizeof(L.label), "密码门");
                break;
            }
            default:
                continue;
            }
            snap.loots.push_back(L);
            continue;
        }

        // 人物
        MemEspPlayer P;
        P.actor = actor;
        P.cat = cat;
        if (!GetWorldPos(rw, actor, P.worldPos)) continue;
        P.teamId = GetTeamId(rw, actor);
        ReadHealth(rw, actor, P.health, P.maxHealth);
        P.alive = IsAlive(rw, actor, P.health, P.maxHealth);
        ReadPlayerName(rw, actor, P.name, sizeof(P.name));
        P.hasBones = ReadBones(rw, P);
        snap.players.push_back(P);
        ++snap.playerCount;
    }

    char st[64];
    snprintf(st, sizeof(st), "DX: %d人 %d物", snap.playerCount, (int)snap.loots.size());
    snap.status = st;
}

// UDP 明文解包绘制：喂给 udp_actors 自机内存坐标(用于原点对齐)，
// 并把解出的真实世界坐标合并进内存透视的人物(保留内存人物UI与名字)，
// 而不是单独用 "UDP" 命名重画方框。坐标是明文真实帧，不随内存加密偏移。
inline void FeedUdp(MemEspSnapshot &snap) {
    g_Config.EnableUdpDecrypt = gUdpDecrypt.load();
    const bool on = gUdpDecrypt.load();

    const MemVec3 &sp = snap.selfPos;
    const bool haveSelf = std::isfinite(sp.x) && std::isfinite(sp.y) && std::isfinite(sp.z) &&
                          (std::fabs(sp.x) + std::fabs(sp.y) + std::fabs(sp.z)) > 1.0f;
    const Vector3 memSelf = {sp.x, sp.y, sp.z};
    // 关闭时也要调用 Tick(nullptr) 让内部 StopCapture 停掉 tcpdump 子进程
    UdpActors::Tick(on && haveSelf ? &memSelf : nullptr, /*approximate=*/false);
    if (!on) return;

    // 原点对齐前 UDP 世界坐标不可信(整体偏移会很远)，先不混入绘制。
    const bool trusted = UdpActors::GetStats().originValid;
    if (!trusted) return;

    const float selfR2 = 200.0f * 200.0f;

    for (const UdpActors::ActorPose &pose : UdpActors::Snapshot()) {
        if (!std::isfinite(pose.world.x) || !std::isfinite(pose.world.y) || !std::isfinite(pose.world.z))
            continue;
        // 自机：不绘制
        if (haveSelf) {
            const float dx = pose.world.x - sp.x, dy = pose.world.y - sp.y, dz = pose.world.z - sp.z;
            if (dx * dx + dy * dy + dz * dz <= selfR2) continue;
        }

        MemEspPlayer UDPp;
        UDPp.actor = UdpActors::ActorId(pose.channel, pose.slot);
        UDPp.cat = 1; // 真人
        UDPp.teamId = 0;
        UDPp.health = 100.0f;
        UDPp.maxHealth = 100.0f;
        UDPp.alive = true;
        UDPp.worldPos = {pose.world.x, pose.world.y, pose.world.z};
        UDPp.hasBones = false;
        const char *onm = pose.name.empty() ? "" : pose.name.c_str();
        snprintf(UDPp.name, sizeof(UDPp.name), "%s", onm);

        // 与最近的内存人物配对：命中则用 UDP 真实坐标修正其位置并整体平移骨骼，
        // 继续沿用内存人物的方框/血条/名字/队伍 UI。
        MemEspPlayer *best = nullptr;
        float bestD = 1e18f;
        for (MemEspPlayer &mp : snap.players) {
            if (!mp.alive) continue;
            const float dx = mp.worldPos.x - pose.world.x;
            const float dy = mp.worldPos.y - pose.world.y;
            const float dz = mp.worldPos.z - pose.world.z;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) { bestD = d; best = &mp; }
        }
        constexpr float kMergeTol2 = 4000.0f * 4000.0f; // ≤40m 视为同一人物
        if (best && bestD <= kMergeTol2) {
            const MemVec3 dv = {pose.world.x - best->worldPos.x,
                                pose.world.y - best->worldPos.y,
                                pose.world.z - best->worldPos.z};
            best->worldPos = {pose.world.x, pose.world.y, pose.world.z};
            for (int i = 0; i < MEM_ESP_BONE_COUNT; ++i) {
                best->bones[i].x += dv.x;
                best->bones[i].y += dv.y;
                best->bones[i].z += dv.z;
            }
            // 内存名字为空时，用 UDP 网络昵称补上(仍走内存UI显示)
            if (!best->name[0] && UDPp.name[0])
                snprintf(best->name, sizeof(best->name), "%s", UDPp.name);
        } else if (UDPp.name[0]) {
            // 未配对到内存人物：作为独立人物用同一套 UI 显示(UDP 提供的网络昵称)
            snap.players.push_back(UDPp);
            ++snap.playerCount;
        }
        // 既没配对、又无昵称的 UDP 数据：忽略，避免凭空多出未知方框
    }
}

void Loop() {
    MemRW rw;
    while (memEspRunning()) {
        // —— 防机制：驱动未正常连接时，一律不允许任何内核读写透视 ——
        // 只有 TIME_Driver 已连接才允许继续（读 libUE4 模块内存、解析人物/物资）。
        rw.connected = TIME_Driver && TIME_Driver->IsConnected();
        if (!rw.connected) {
            MemEspSnapshot s;
            s.status = "驱动未连接";
            std::lock_guard<std::mutex> lk(gSnapMutex);
            gSnap = s;
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            continue;
        }
        if (rw.pid <= 0) {
            rw.pid = FindPidByPackage("com.tencent.tmgp.dfm");
            if (rw.pid <= 0) {
                MemEspSnapshot s;
                s.status = "未找到游戏进程";
                std::lock_guard<std::mutex> lk(gSnapMutex);
                gSnap = s;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }
        if (rw.libUE4 == 0) {
            rw.libUE4 = TIME_Driver->Get_Module_Base(rw.pid, "libUE4.so");
            if (rw.libUE4 == 0) {
                MemEspSnapshot s;
                s.status = "未找到 libUE4";
                std::lock_guard<std::mutex> lk(gSnapMutex);
                gSnap = s;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }

        MemEspSnapshot s;
        Capture(rw, s);
        FeedUdp(s); // UDP 明文解包：自机锚点 + 合入 UDP 人物
        {
            std::lock_guard<std::mutex> lk(gSnapMutex);
            gSnap = std::move(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // ~33Hz
    }
}

} // namespace

void memEspStart() {
    if (gRun.load()) return;
    gRun.store(true);
    gThread = std::thread(Loop);
}

void memEspStop() {
    gRun.store(false);
    if (gThread.joinable()) gThread.join();
}

bool memEspRunning() {
    return gRun.load();
}

bool memEspGetSnapshot(MemEspSnapshot &out) {
    // 用 try_lock：渲染主线程绝不因等待快照锁而阻塞，
    // 避免内存读线程异常/长期持锁时悬浮窗冻结、无法交互。
    if (!gSnapMutex.try_lock())
        return false;
    out = gSnap;
    gSnapMutex.unlock();
    // 只要有状态即视为“已发布快照”：
    // - 面板据此显示真实状态（驱动未连接 / 未找到游戏 / DX: N人N物）
    // - 绘制侧另行判断 ts（>0 才真正投影绘制），未连接时 ts=0 自动跳过
    return !out.status.empty();
}

// ============================================================================
// 绘制
// ============================================================================
namespace {

// 计算人物屏幕方框 (优先骨骼，回退身高推算)
inline bool ComputePlayerBox(const MemEspPlayer &p, const MemCamera &cam, float sx, float sy,
                             ImVec2 &mn, ImVec2 &mx) {
    if (p.hasBones) {
        ImVec2 pts[MEM_ESP_BONE_COUNT];
        bool any = false;
        for (int i = 0; i < MEM_ESP_BONE_COUNT; ++i) {
            const MemVec2 sc = WorldToScreenCamera(p.bones[i], cam, sx, sy);
            if (sc.x >= 0.0f && sc.y >= 0.0f && sc.x <= sx && sc.y <= sy) {
                pts[i] = ImVec2(sc.x, sc.y);
                any = true;
            } else {
                pts[i] = ImVec2(0, 0);
            }
        }
        if (!any) return false;
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (int i = 0; i < MEM_ESP_BONE_COUNT; ++i) {
            if (!IsValidScreen(pts[i])) continue;
            if (pts[i].x < minX) minX = pts[i].x;
            if (pts[i].x > maxX) maxX = pts[i].x;
            if (pts[i].y < minY) minY = pts[i].y;
            if (pts[i].y > maxY) maxY = pts[i].y;
        }
        const float w = maxX - minX, h = maxY - minY;
        if (h < 2.0f || w < 1.0f) return false;
        const float padX = ClampFloat(w * 0.18f, 3.0f, 18.0f);
        const float padY = ClampFloat(h * 0.08f, 4.0f, 22.0f);
        mn = ImVec2(minX - padX, minY - padY);
        mx = ImVec2(maxX + padX, maxY + padY);
        return true;
    }

    // 回退：无骨骼时用身高推算方框。RootComponent 平移一般约在人物身体中部，
    // 因此以 worldPos 为中心向上下各外扩约半个人高(92cm≈成人半身高)，
    // 使框能覆盖从脚底到头顶的整个人物，而不是从胸部开始往上。
    constexpr float kHalfBody = 92.0f;
    const MemVec2 ctr = WorldToScreenCamera(p.worldPos, cam, sx, sy);
    if (ctr.x <= -9990.0f && ctr.y <= -9990.0f) return false;
    if (ctr.x < 0.0f || ctr.y < 0.0f || ctr.x > sx || ctr.y > sy) return false;
    const MemVec3 feet3 = {p.worldPos.x, p.worldPos.y, p.worldPos.z - kHalfBody};
    const MemVec3 head3 = {p.worldPos.x, p.worldPos.y, p.worldPos.z + kHalfBody};
    const MemVec2 feet = WorldToScreenCamera(feet3, cam, sx, sy);
    const MemVec2 head = WorldToScreenCamera(head3, cam, sx, sy);
    const float h = std::fabs(feet.y - head.y);
    if (h < 2.0f) return false;
    const float w = h * 0.5f;
    mn = ImVec2(ctr.x - w * 0.5f, std::fminf(head.y, feet.y));
    mx = ImVec2(ctr.x + w * 0.5f, std::fmaxf(head.y, feet.y));
    return true;
}

inline void DrawTextShadow(ImDrawList *d, ImFont *font, float size, const ImVec2 &pos, ImU32 col, const char *txt) {
    if (font) {
        d->AddText(font, size, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 220), txt);
        d->AddText(font, size, ImVec2(pos.x, pos.y), col, txt);
    } else {
        d->AddText(pos, col, txt);
    }
}

} // namespace

void memEspDraw(ImDrawList *draw, const MemEspDrawCfg &cfg, float sx, float sy) {
    if (!draw || sx <= 0 || sy <= 0) return;
    MemEspSnapshot snap;
    if (!memEspGetSnapshot(snap) || snap.ts == 0) {
#if 0
        // 未就绪时在左上角给个状态提示 (可关)
        const char* st = snap.status.empty() ? "内存穿透: 未连接" : snap.status.c_str();
        draw->AddText(ImVec2(10, 10), IM_COL32(255, 80, 80, 200), st);
#endif
        return;
    }

    const MemCamera &cam = snap.camera;
    ImFont *font = ImGui::GetFont();
    bool anyDraw = false;

    // ── 人物 ──
    const bool wantPlayer = cfg.box || cfg.health || cfg.name || cfg.dist || cfg.team || cfg.skeleton;
    if (wantPlayer) {
        for (const MemEspPlayer &p : snap.players) {
            if (!p.alive) continue;
            if (cfg.ignoreBot && p.cat == 3) continue;
            if (cfg.ignoreTeam && snap.myTeam > 0 && p.teamId == snap.myTeam) continue;

            const float distMeters = GetDistance(p.worldPos, cam.Location) / 100.0f;
            const ImU32 catCol = GetCategoryColor(p.cat);
            const ImU32 boxCol = (p.teamId > 0) ? GetTeamColor(p.teamId) : catCol;

            ImVec2 mn(0, 0), mx(0, 0);
            const bool hasBox = ComputePlayerBox(p, cam, sx, sy, mn, mx);

            // 骨骼
            if (cfg.skeleton && p.hasBones) {
                ImVec2 bk[MEM_ESP_BONE_COUNT];
                bool ok = true;
                for (int i = 0; i < MEM_ESP_BONE_COUNT; ++i) {
                    const MemVec2 sc = WorldToScreenCamera(p.bones[i], cam, sx, sy);
                    if (sc.x < 0 || sc.y < 0 || sc.x > sx || sc.y > sy) { ok = false; break; }
                    bk[i] = ImVec2(sc.x, sc.y);
                }
                if (ok) {
                    const int seg[][2] = {{0,1},{1,2},{1,3},{1,4},{3,5},{4,6},{5,7},{6,8},{2,9},{2,10},{9,11},{10,12},{11,13},{12,14}};
                    for (auto &s : seg) {
                        if (IsValidScreen(bk[s[0]]) && IsValidScreen(bk[s[1]]))
                            draw->AddLine(bk[s[0]], bk[s[1]], ((boxCol & 0x00FFFFFFu) | (255u << 24)), 1.8f);
                    }
                    anyDraw = true;
                }
            }

            // 方框
            if (cfg.box && hasBox) {
                const float w = GetWidthFloor(distMeters);
                const float bw = (w > (mx.x - mn.x)) ? w : (mx.x - mn.x);
                draw->AddRect(mn, mx, IM_COL32(0, 0, 0, 200), 0.0f, 0, 4.0f);
                draw->AddRect(mn, mx, boxCol, 0.0f, 0, 2.0f);
                anyDraw = true;
            }

            if (!hasBox) continue;

            // 顶部血条 + 名字
            const float dxW = (mx.x - mn.x);
            const float floorW = GetWidthFloor(distMeters);
            const float barW = (floorW > dxW) ? floorW : dxW;
            const float barH = distMeters > 50.0f ? 4.0f : (distMeters < 10.0f ? 6.0f : 6.0f - (distMeters - 10.0f) / 40.0f * 2.0f);
            const float barX = (mn.x + mx.x) * 0.5f - barW * 0.5f;
            float barY = mn.y - 6.0f - barH;

            if (cfg.health && p.maxHealth > 0.0f) {
                const float ratio = p.health < p.maxHealth ? p.health / p.maxHealth : 1.0f;
                const ImU32 hpCol = GetRatioColor(ratio);
                draw->AddRectFilled(ImVec2(barX - 1, barY - 1), ImVec2(barX + barW + 1, barY + barH + 1), IM_COL32(0, 0, 0, 160));
                draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * ratio, barY + barH), hpCol);
                anyDraw = true;
            }

            // 名字
            if (cfg.name) {
                const char *nm = (p.cat == 3) ? "BOT" : (p.cat == 2 ? "BUTT" : (p.name[0] ? p.name : "?"));
                const float nameSize = ClampFloat(barW * 0.42f, 16.0f, 40.0f);
                if (font) {
                    const ImVec2 nsz = font->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, nm);
                    const float nx = (mn.x + mx.x) * 0.5f - nsz.x * 0.5f;
                    const float ny = barY - 2.0f - nsz.y;
                    DrawTextShadow(draw, font, nameSize, ImVec2(nx, ny), catCol, nm);
                }
                anyDraw = true;
            }

            // 队伍/距离：方框下方
            if (cfg.team || cfg.dist) {
                const float infoSize = ClampFloat(dxW * 0.26f, 12.0f, 22.0f);
                if (font) {
                    std::string s;
                    ImU32 col = boxCol;
                    if (cfg.team && p.teamId > 0) {
                        char tb[24]; snprintf(tb, sizeof(tb), "队伍%d", p.teamId);
                        s = tb;
                        if (cfg.dist) s += "  ";
                    }
                    if (cfg.dist) {
                        char db[24]; snprintf(db, sizeof(db), "%.0fm", distMeters);
                        s += db;
                        col = GetDistanceColor(distMeters);
                    }
                    const ImVec2 sz = font->CalcTextSizeA(infoSize, FLT_MAX, 0.0f, s.c_str());
                    const float nx = (mn.x + mx.x) * 0.5f - sz.x * 0.5f;
                    DrawTextShadow(draw, font, infoSize, ImVec2(nx, mx.y + 4.0f), col, s.c_str());
                }
                anyDraw = true;
            }
        }
    }

    // ── 物资 ──
    if (cfg.loot || cfg.container || cfg.deadbody || cfg.safebox) {
        for (const MemEspLoot &L : snap.loots) {
            bool show = false;
            ImU32 col = IM_COL32(255, 255, 255, 255);
            const char *text = L.label;
            char gbuf[128];
            switch (L.cat) {
            case MemLoot_GroundItem:
                show = cfg.loot;
                col = GetEquipLevelColor(L.grade);
                if (L.count > 1)
                    snprintf(gbuf, sizeof(gbuf), "%s x%d", GetItemName(L.itemId), L.count);
                else
                    snprintf(gbuf, sizeof(gbuf), "%s", GetItemName(L.itemId));
                text = gbuf;
                break;
            case MemLoot_Container:
                show = cfg.container;
                col = IM_COL32(255, 220, 0, 255);
                break;
            case MemLoot_DeadBody:
                show = cfg.deadbody;
                col = IM_COL32(60, 255, 60, 255);
                break;
            case MemLoot_SafeBox:
                show = cfg.safebox;
                col = IM_COL32(255, 165, 0, 255);
                break;
            default:
                show = cfg.loot;
                col = IM_COL32(255, 255, 255, 255);
                break;
            }
            if (!show) continue;

            const MemVec2 sc = WorldToScreenCamera(L.worldPos, cam, sx, sy);
            if (sc.x <= -9990.0f && sc.y <= -9990.0f) continue;
            if (sc.x < 0.0f || sc.y < 0.0f || sc.x > sx || sc.y > sy) continue;

            const float dm = GetDistance(L.worldPos, cam.Location) / 100.0f;
            const float t = ClampFloat((dm - 20.0f) / (80.0f - 20.0f), 0.0f, 1.0f);
            const float fs = 22.0f + t * (14.0f - 22.0f);

            const char *disp = text ? text : "";
            float lineH = 0.0f;
            if (font && disp[0]) lineH = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, disp).y;

            const float vsize = lineH + 3.0f;
            const float vx = sc.x;
            float vy = sc.y - vsize;
            if (font && disp[0])
                DrawTextShadow(draw, font, fs, ImVec2(vx - font->CalcTextSizeA(fs, FLT_MAX, 0.0f, disp).x * 0.5f, vy), col, disp);

            if (font) {
                char db[32]; snprintf(db, sizeof(db), "%.0fm", dm);
                const float dfs = fs * 0.8f;
                const float dfy = vy - 3.0f - font->CalcTextSizeA(dfs, FLT_MAX, 0.0f, db).y;
                DrawTextShadow(draw, font, dfs, ImVec2(vx - font->CalcTextSizeA(dfs, FLT_MAX, 0.0f, db).x * 0.5f, dfy),
                               GetDistanceColor(dm), db);
            }

            draw->AddCircleFilled(ImVec2(sc.x, sc.y), 3.0f, col);
            anyDraw = true;
        }
    }

    (void)anyDraw;
}

// ============================================================================
// UDP 明文解包：开关 / 读取状态
// ============================================================================
void memEspSetUdpDecrypt(bool on) {
    gUdpDecrypt.store(on);
}

bool memEspGetUdpStats(MemEspUdpStats &out) {
    out = MemEspUdpStats();
    out.enabled = gUdpDecrypt.load();
    const UdpActors::Stats st = UdpActors::GetStats();
    out.capturing = st.capturing;
    out.originValid = st.originValid;
    out.poses = st.poses;
    out.names = st.names;
    out.mvOk = st.mvOk;
    out.mvFails = st.mvFails;
    out.packets = st.packets;
    out.gamePackets = st.gamePackets;
    return true;
}