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

        // 更新速度：增大响应权重(0.3)，让速度估算快速跟踪快速移动的目标
        m_vx = m_vx * 0.7f + (mx - px) / dt * 0.3f;
        m_vy = m_vy * 0.7f + (my - py) / dt * 0.3f;

        // 限制速度最大值（归一化坐标/秒），防止速度估算异常导致预测过度
        const float maxV = 15.0f;
        if (m_vx > maxV) m_vx = maxV;
        if (m_vx < -maxV) m_vx = -maxV;
        if (m_vy > maxV) m_vy = maxV;
        if (m_vy < -maxV) m_vy = -maxV;

        // 更新位置（增大响应权重，让位置更快跟随测量值）
        m_x = px + (mx - px) * 0.7f;
        m_y = py + (my - py) * 0.7f;
    }

    float x() const { return m_x; }
    float y() const { return m_y; }
    float vx() const { return m_vx; }
    float vy() const { return m_vy; }

private:
    float m_x = 0.0f, m_y = 0.0f, m_vx = 0.0f, m_vy = 0.0f;
};
