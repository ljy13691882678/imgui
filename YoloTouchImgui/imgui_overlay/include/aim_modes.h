#pragma once

// 自瞄算法：PID 与 贝塞尔 两种模式。
// 移植自 ljy13691882678/YoloTouchHelp 的 AimController.kt：
//   - PID：executeAimingPid（比例/积分/微分 + 误差变号清积分 + 积分限幅 + 固定采样周期 + 单帧步长上限）
//   - 贝塞尔：executeAimingBezier + BezierMover（slow-fast-slow 的 smoothstep 计时，
//     每帧按平滑比例输出 error*t 作为移动量，时长随距离自适应）
// 两者都输出与现有 AimController 一致的 AimOutput（归一化增量），以便主循环统一做
// 触控区注入。收敛判断用像素阈值 convergeThresh。

#include "aim_types.h"
#include <algorithm>
#include <cmath>
#include <chrono>

// 贝塞尔缓动计时器（slow-fast-slow）：返回自上次调用以来的平滑比例增量(0..1)。
// smoothstep(t) = t*t*(3-2t)，等价于两端控制点重合的贝塞尔曲线。
class BezierMover {
public:
    void start(long long startMs, long long endMs) {
        m_start = startMs;
        m_end = endMs;
        m_prevSmooth = 0.0f;
        m_active = true;
    }

    // 返回本次帧间平滑比例增量（>=0）；未激活或已结束时返回 0
    float tickRatio(long long nowMs) {
        if (!m_active) return 0.0f;
        float duration = (float)(m_end - m_start);
        if (duration < 1.0f) duration = 1.0f;
        float rawT = (float)(nowMs - m_start) / duration;
        rawT = std::clamp(rawT, 0.0f, 1.0f);
        float curSmooth = smoothstep(rawT);
        float delta = curSmooth - m_prevSmooth;
        m_prevSmooth = curSmooth;
        if (rawT >= 1.0f) m_active = false;
        return std::max(delta, 0.0f);
    }

    bool isActive() const { return m_active; }
    void cancel() { m_active = false; }

private:
    static float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

    bool      m_active = false;
    long long m_start = 0, m_end = 0;
    float     m_prevSmooth = 0.0f;
};

// PID 自瞄控制器（像素空间内计算，输出归一化增量）
class PidAimController {
public:
    AimOutput compute(const AimTarget& t, const AimConfig& cfg,
                      float screenCx, float screenCy, float /*dt*/,
                      float scrW, float scrH) {
        AimOutput out;
        if (!cfg.aimEnabled || !cfg.enabled) return out;

        // 预判目标位置
        float px = t.cx + t.vx * cfg.predictGain;
        float py = t.cy + t.vy * cfg.predictGain;
        // 像素误差（相对准星/屏幕中心）
        float errX = (px - screenCx) * scrW;
        float errY = (py - screenCy) * scrH;

        // 收敛：进入像素阈值框内停止拖拽
        if (std::fabs(errX) < cfg.convergeThresh && std::fabs(errY) < cfg.convergeThresh) {
            m_lastX = 0.0f; m_lastY = 0.0f;
            return out;
        }
        // 归一化死区
        float dist = std::sqrt(errX * errX + errY * errY);
        if (dist < cfg.deadZone * std::min(scrW, scrH)) {
            m_lastX = 0.0f; m_lastY = 0.0f;
            return out;
        }

        // 误差变号则清零积分（抗积分饱和 / 防过冲回摆）
        if (errX * m_prevErrX <= 0.0f) m_integralX = 0.0f;
        if (errY * m_prevErrY <= 0.0f) m_integralY = 0.0f;
        // 固定采样周期（与参考实现一致，非真实帧间隔）
        float sample = cfg.pidSamplePeriodMs / 1000.0f;
        if (sample < 0.001f) sample = 0.001f;
        m_integralX += errX * sample;
        m_integralY += errY * sample;
        const float integralLimit = 100.0f;
        m_integralX = std::clamp(m_integralX, -integralLimit, integralLimit);
        m_integralY = std::clamp(m_integralY, -integralLimit, integralLimit);
        float derivX = (errX - m_prevErrX) / sample;
        float derivY = (errY - m_prevErrY) / sample;

        float moveX = errX * cfg.pidKp + m_integralX * cfg.pidKi + derivX * cfg.pidKd;
        float moveY = errY * cfg.pidKp + m_integralY * cfg.pidKi + derivY * cfg.pidKd;
        m_prevErrX = errX; m_prevErrY = errY;

        // 输出平滑（EMA）
        float sm = std::clamp(cfg.aimMoveSmooth, 0.0f, 0.95f);
        moveX = m_lastX * sm + moveX * (1.0f - sm);
        moveY = m_lastY * sm + moveY * (1.0f - sm);
        m_lastX = moveX; m_lastY = moveY;

        // 单帧最大步长（像素，参考实现为 1200，基本无约束）
        const float maxPerFrame = 1200.0f;
        float md = std::sqrt(moveX * moveX + moveY * moveY);
        if (md > maxPerFrame) {
            moveX *= maxPerFrame / md;
            moveY *= maxPerFrame / md;
        }

        out.active = true;
        out.deltaX = moveX / scrW;   // 归一化增量
        out.deltaY = moveY / scrH;
        out.targetX = px;
        out.targetY = py;
        return out;
    }

    void reset() {
        m_prevErrX = 0.0f; m_prevErrY = 0.0f;
        m_integralX = 0.0f; m_integralY = 0.0f;
        m_lastX = 0.0f; m_lastY = 0.0f;
    }

private:
    float m_prevErrX = 0.0f, m_prevErrY = 0.0f;
    float m_integralX = 0.0f, m_integralY = 0.0f;
    float m_lastX = 0.0f, m_lastY = 0.0f;
};

// 贝塞尔自瞄控制器（slow-fast-slow 缓动移动，像素空间计算，输出归一化增量）
class BezierAimController {
public:
    AimOutput compute(const AimTarget& t, const AimConfig& cfg,
                      float screenCx, float screenCy, float /*dt*/,
                      float scrW, float scrH) {
        AimOutput out;
        if (!cfg.aimEnabled || !cfg.enabled) return out;

        float px = t.cx + t.vx * cfg.predictGain;
        float py = t.cy + t.vy * cfg.predictGain;
        float errX = (px - screenCx) * scrW;
        float errY = (py - screenCy) * scrH;

        // 收敛：进入像素阈值框内停止拖拽
        if (std::fabs(errX) < cfg.convergeThresh && std::fabs(errY) < cfg.convergeThresh) {
            m_lastX = 0.0f; m_lastY = 0.0f;
            m_prevMoveX = 0.0f; m_prevMoveY = 0.0f;
            m_mover.cancel();
            return out;
        }

        // 过冲检测：误差符号与上帧移动方向相反时，强制立即收敛
        // 防止目标切换时的"过冲-甩回"振荡
        if (m_prevMoveX != 0.0f && m_prevMoveY != 0.0f) {
            bool overshotX = (errX > 0 && m_prevMoveX < 0) || (errX < 0 && m_prevMoveX > 0);
            bool overshotY = (errY > 0 && m_prevMoveY < 0) || (errY < 0 && m_prevMoveY > 0);

            // 检测到过冲，立即停止贝塞尔移动并快速归零
            if (overshotX || overshotY) {
                m_mover.cancel();
                m_lastX *= 0.1f;  // 快速衰减旧惯性
                m_lastY *= 0.1f;
            }
        }

        long long now = nowMs();
        // 只有在 mover 不活跃时才启动新的，避免频繁重启
        if (!m_mover.isActive()) {
            float dist = std::sqrt(errX * errX + errY * errY);
            // 根据距离动态调整时长：近则快，远则慢
            long long dur = (long long)(cfg.bezierDuration * 5.0f + dist * 0.2f);
            dur = std::clamp(dur, 100LL, 500LL);  // 缩短时长，加快响应
            m_mover.start(now, now + dur);
        }
        float ratio = m_mover.tickRatio(now);

        // 直接使用误差 * ratio，不叠加旧惯性
        float moveX = errX * ratio;
        float moveY = errY * ratio;

        // 输出平滑（EMA）：使用较小的 sm 值减少惯性
        float sm = std::clamp(cfg.aimMoveSmooth * 0.5f, 0.0f, 0.5f);
        moveX = m_lastX * sm + moveX * (1.0f - sm);
        moveY = m_lastY * sm + moveY * (1.0f - sm);
        m_lastX = moveX; m_lastY = moveY;

        // 记录当前移动方向，用于下帧过冲检测
        m_prevMoveX = moveX;
        m_prevMoveY = moveY;

        out.active = true;
        out.deltaX = moveX / scrW;
        out.deltaY = moveY / scrH;
        out.targetX = px;
        out.targetY = py;
        return out;
    }

    void reset() {
        m_mover.cancel();
        m_lastX = 0.0f; m_lastY = 0.0f;
        m_prevMoveX = 0.0f; m_prevMoveY = 0.0f;
    }

private:
    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    BezierMover m_mover;
    float m_lastX = 0.0f, m_lastY = 0.0f;
    float m_prevMoveX = 0.0f, m_prevMoveY = 0.0f;  // 上帧移动方向，用于过冲检测
};
