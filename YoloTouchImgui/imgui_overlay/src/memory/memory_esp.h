#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>

struct ImDrawList;

// ============================================================================
// 内存透视 / 物资 (三角洲行动 UWorld 内存链 + 内核读写 TimeDriver)
// 独立快照线程读取游戏内存，渲染线程仅做投影 + 绘制，避免阻塞 ImGui 主循环。
// ============================================================================

// 数学/内存类型 (移植自 dfm_engine.h / engine.h)
struct MemVec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct MemVec2 {
    float x = 0.0f, y = 0.0f;
};
struct MemQuat {
    float X = 0.0f, Y = 0.0f, Z = 0.0f, W = 1.0f;
};
struct MemRotator {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct MemTransform {
    MemQuat    Rotation{};
    MemVec3    translation{0.0f, 0.0f, 0.0f};
    MemVec3    Scale3D{1.0f, 1.0f, 1.0f};
    // 补齐到 0x30 (48B) 布局，与骨骼元素步长 BoneTransformStride=0x30 一致。
    // 缺失该尾随 padding 会让 sizeof(MemTransform)=0x28，与 0x30 骨骼布局不匹配，
    // 读取后填充/对齐错误易导致骨骼偏移或整体变形。
    uint8_t    Padding[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};
static_assert(sizeof(MemTransform) == 0x30, "MemTransform must be 0x30 (48B) layout");
struct MemCamera {
    MemVec3    Location{0.0f, 0.0f, 0.0f};
    float      Padding_0C = 0.0f;
    MemRotator Rotation{0.0f, 0.0f, 0.0f};
    float      FOV = 90.0f;
};

// 绘制开关 (由 main.cpp 从 g_cfg 拷贝而来)
struct MemEspDrawCfg {
    bool box = true;        // 人物方框
    bool health = true;     // 血条
    bool name = true;       // 名字
    bool dist = true;       // 距离
    bool team = true;       // 队伍
    bool skeleton = false;  // 骨骼
    bool loot = false;      // 物资
    bool container = false; // 容器
    bool deadbody = false;  // 尸体袋
    bool safebox = false;   // 保险箱
    bool ignoreBot = true;  // 忽略人机
    bool ignoreTeam = false;// 忽略队友
};

#define MEM_ESP_BONE_COUNT 15

// 人物快照
struct MemEspPlayer {
    uint64_t actor = 0;
    int      cat = 0;        // 1=真人 2=靶子 3=人机
    int      teamId = 0;
    float    health = 0.0f;
    float    maxHealth = 0.0f;
    bool     alive = false;
    char     name[48] = {0};
    MemVec3  worldPos{0.0f, 0.0f, 0.0f};
    bool     hasBones = false;
    MemVec3  bones[MEM_ESP_BONE_COUNT] = {}; // 世界坐标
};

// 物资快照 (cat 取值见 MemLootCategory)
struct MemEspLoot {
    uint64_t actor = 0;
    int      cat = 0;
    MemVec3  worldPos{0.0f, 0.0f, 0.0f};
    uint64_t itemId = 0;
    int32_t  count = 0;
    int32_t  value = 0;
    int      grade = 0;      // 品质 1白...6红, 0=未知
    bool     finished = false;
    bool     isBot = false;
    char     label[96] = {0};
};

// 一帧快照
struct MemEspSnapshot {
    int64_t ts = 0;
    MemCamera camera{};
    int   myTeam = 0;
    int   actorCount = 0;
    int   playerCount = 0;
    int   lootCount = 0;
    std::vector<MemEspPlayer> players;
    std::vector<MemEspLoot>   loots;
    std::string status;
};

// 物资分类 (与参考 dfm_classification.h 一致)
enum MemLootCategory {
    MemLoot_None = 0,
    MemLoot_GroundItem = 1,
    MemLoot_Container = 2,
    MemLoot_DeadBody = 3,
    MemLoot_SafeBox = 4,
    MemLoot_Computer = 5,
    MemLoot_CodedLock = 6,
};

// 生命周期
void memEspStart();
void memEspStop();
bool memEspRunning();

// 版本/Build ID 偏移配置（item 6）：不同游戏版本可传入对应 BuildID 选择偏移表。
// buildId 传 nullptr 或空串时使用默认偏移表。
void memEspSetOffsetsForBuild(const char *buildId);

// 取最新快照副本 (线程安全、无锁) 未连接/无数据返回 false
bool memEspGetSnapshot(MemEspSnapshot& out);

// 取当前快照的不可变共享指针 (原子发布，item 10)：
// 渲染线程持有该指针即可安全读取，无需拷贝大 vector 也无需加锁。
std::shared_ptr<const MemEspSnapshot> memEspGetSnapshotPtr();

// 绘制到 drawlist；sx/sy = 悬浮窗屏幕尺寸 (native_window_screen_x/y)
void memEspDraw(ImDrawList* draw, const MemEspDrawCfg& cfg, float sx, float sy);