#pragma once

#include "aim_types.h"
#include <cmath>
#include <cstdint>
#include <sys/time.h>

// TriggerBot：目标进入准星区域时自动开火（通过触摸注入）
class TriggerController {
public:
    void update(const AimTarget& target, const AimConfig& cfg,
                float screenCx, float screenCy, bool fingerDown,
                bool& fire, bool& holdFire) {
        fire = false;
        holdFire = false;
        if (!cfg.triggerEnabled || !cfg.enabled) return;

        // 距离准星的距离（归一化）
        float dx = target.cx - screenCx;
        float dy = target.cy - screenCy;
        float dist = std::sqrt(dx*dx + dy*dy);

        // 触发半径（灵敏度反向映射）
        float radius = 0.03f + (1.0f - cfg.triggerSensitivity) * 0.04f;

        if (dist < radius) {
            if (cfg.triggerHold) {
                holdFire = true;
            } else if (!fingerDown) {
                fire = true;
                m_lastFireMs = nowMs();
            }
        } else {
            m_cooldownUntil = 0;
        }
    }

    void reset() { m_cooldownUntil = 0; }

private:
    static int64_t nowMs() {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    }
    int64_t m_cooldownUntil = 0;
    int64_t m_lastFireMs = 0;
};
