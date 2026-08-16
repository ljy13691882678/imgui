#pragma once

#include "aim_types.h"
#include <cmath>
#include <cstdint>
#include <sys/time.h>

// TriggerBot：目标进入准星区域时自动开火（通过触摸注入）
// 支持两种模式：
//   - 点射（triggerHold=false）：目标进入触发区时执行一次 touch_down+touch_up，然后冷却
//   - 按住（triggerHold=true）：目标进入触发区时保持 touch_down，离开时 touch_up
class TriggerController {
public:
    void update(const AimTarget& target, const AimConfig& cfg,
                float screenCx, float screenCy, bool fingerDown,
                bool& fireOnce, bool& holdFire, bool& holdRelease) {
        fireOnce = false;
        holdFire = false;
        holdRelease = false;
        if (!cfg.triggerEnabled || !cfg.enabled) return;

        // 距离准星的距离（归一化）
        float dx = target.cx - screenCx;
        float dy = target.cy - screenCy;
        float dist = std::sqrt(dx*dx + dy*dy);

        // 触发半径（灵敏度反向映射）：sensitivity 1.0 → 半径 0.03, sensitivity 0.1 → 半径 0.07
        float radius = 0.03f + (1.0f - cfg.triggerSensitivity) * 0.04f;

        int64_t now = nowMs();

        if (dist < radius) {
            // 目标在触发区内
            if (cfg.triggerHold) {
                // 按住模式：持续输出 holdFire=true，直到离开
                holdFire = true;
                m_wasHolding = true;
            } else {
                // 点射模式：仅在冷却期内不重复触发
                if (now >= m_cooldownUntil && !fingerDown) {
                    fireOnce = true;
                    m_lastFireMs = now;
                    m_cooldownUntil = now + cfg.triggerCooldownMs;
                }
            }
        } else {
            // 目标离开触发区
            if (m_wasHolding) {
                // 按住模式：释放虚拟手指
                holdRelease = true;
                m_wasHolding = false;
            }
            m_cooldownUntil = 0;  // 离开后重置冷却，下次进入立刻触发
        }
    }

    void reset() {
        m_cooldownUntil = 0;
        m_lastFireMs = 0;
        m_wasHolding = false;
    }

private:
    static int64_t nowMs() {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    }
    int64_t m_cooldownUntil = 0;
    int64_t m_lastFireMs = 0;
    bool    m_wasHolding = false;
};