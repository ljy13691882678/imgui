#pragma once

// 轻量级 2D 匀速 Kalman 滤波器
// 状态: (x, y, vx, vy)；用于跨帧目标跟踪与位置/速度预测
class KalmanFilter {
public:
    void init(float x, float y, float vx = 0.0f, float vy = 0.0f) {
        m_x = x; m_y = y; m_vx = vx; m_vy = vy;
    }

    // 用新的测量值更新（dt 秒）
    void update(float mx, float my, float dt) {
        if (dt <= 0.0f) dt = 0.016f;

        // 预测
        float px = m_x + m_vx * dt;
        float py = m_y + m_vy * dt;

        // 更新速度（EMA 平滑速度增量）
        m_vx = m_vx * 0.9f + (mx - px) / dt * 0.1f;
        m_vy = m_vy * 0.9f + (my - py) / dt * 0.1f;

        // 更新位置（简化固定增益）
        m_x = px + (mx - px) * 0.5f;
        m_y = py + (my - py) * 0.5f;
    }

    float x() const { return m_x; }
    float y() const { return m_y; }
    float vx() const { return m_vx; }
    float vy() const { return m_vy; }

private:
    float m_x = 0.0f, m_y = 0.0f, m_vx = 0.0f, m_vy = 0.0f;
};
