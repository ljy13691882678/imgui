#pragma once

#include "aim_types.h"
#include <algorithm>
#include <cmath>

// PID 风格自瞄控制器：结合死区、平滑、预判（原版算法）
class AimController {
public:
    void reset() {
        m_lastX = -1.0f;
        m_lastY = -1.0f;
        m_lastTargetId = -1;
    }

    // screenCx/screenCy：准星位置（归一化），默认屏幕中心 0.5,0.5
    AimOutput compute(const AimTarget& target, const AimConfig& cfg,
                   float screenCx, float screenCy, float dt) {
        AimOutput out;
        if (!cfg.aimEnabled || !cfg.enabled) return out;

        // 预判目标位置
        float px = target.cx + target.vx * cfg.predictGain;
        float py = target.cy + target.vy * cfg.predictGain;

        float dx = px - screenCx;
        float dy = py - screenCy;
        float dist = std::sqrt(dx*dx + dy*dy);

        // 死区：目标已足够靠近准星则不移动
        if (dist < cfg.deadZone) {
            out.active = false;
            m_lastX = px; m_lastY = py;
            return out;
        }

        // 减速区：进入 3×死区 范围后按比例减弱移动量，平滑收敛，避免过冲/抖动
        float ease = 1.0f;
        if (cfg.deadZone > 0.0f && dist < cfg.deadZone * 3.0f) {
            ease = std::clamp((dist - cfg.deadZone) / (cfg.deadZone * 2.0f), 0.0f, 1.0f);
        }

        // PID 比例控制（这里用 P 项 + 平滑 + 减速区）
        float kP = cfg.aimSpeed * 0.5f * ease;
        float moveX = dx * kP;
        float moveY = dy * kP;

        // 平滑（指数移动平均）
        if (m_lastX >= 0.0f) {
            moveX = moveX * (1.0f - cfg.smoothX) + m_lastX * cfg.smoothX;
            moveY = moveY * (1.0f - cfg.smoothY) + m_lastY * cfg.smoothY;
        }
        m_lastX = moveX;
        m_lastY = moveY;

        out.active = true;
        out.deltaX = moveX;
        out.deltaY = moveY;
        out.targetX = px;
        out.targetY = py;
        return out;
    }

private:
    float m_lastX = -1.0f;
    float m_lastY = -1.0f;
    int   m_lastTargetId = -1;
};
