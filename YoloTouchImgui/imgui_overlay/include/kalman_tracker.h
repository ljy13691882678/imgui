#pragma once

#include "aim_types.h"
#include "kalman_filter.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

// 基于 Kalman 的跨帧目标跟踪器
class KalmanTracker {
public:
    struct Track {
        KalmanFilter kf;
        int   id = 0;
        int   classId = 0;
        int   lost = 0;         // 连续丢失帧数
        float lastScore = 0.0f;
        bool  active = false;
    };

    void update(const std::vector<AimTarget>& detections, float dt) {
        // 1. 标记所有轨迹为新帧丢失
        for (auto& t : m_tracks) t.lost++;

        // 2. 关联：贪心匹配（按 IoU）
        std::vector<bool> matchedDets(detections.size(), false);
        for (auto& t : m_tracks) {
            int bestIdx = -1;
            float bestIoU = 0.25f;  // 关联阈值
            for (size_t i = 0; i < detections.size(); ++i) {
                if (matchedDets[i]) continue;
                float iou = computeIoU(t.kf.x(), t.kf.y(),
                                       detections[i].cx, detections[i].cy);
                if (iou > bestIoU) {
                    bestIoU = iou;
                    bestIdx = static_cast<int>(i);
                }
            }
            if (bestIdx >= 0) {
                matchedDets[bestIdx] = true;
                t.lost = 0;
                t.classId = detections[bestIdx].classId;
                t.lastScore = detections[bestIdx].score;
                t.kf.update(detections[bestIdx].cx, detections[bestIdx].cy, dt);
                t.active = true;
            }
        }

        // 3. 未匹配的检测建立新轨迹
        for (size_t i = 0; i < detections.size(); ++i) {
            if (matchedDets[i]) continue;
            Track tr;
            tr.id = m_nextId++;
            tr.classId = detections[i].classId;
            tr.lastScore = detections[i].score;
            tr.kf.init(detections[i].cx, detections[i].cy);
            tr.active = true;
            m_tracks.push_back(tr);
        }

        // 4. 清理丢失过久的轨迹
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
            [](const Track& t) { return t.lost > 15; }), m_tracks.end());
    }

    // 返回活跃轨迹（转为 AimTarget）
    std::vector<AimTarget> activeTargets() const {
        std::vector<AimTarget> out;
        for (const auto& t : m_tracks) {
            if (t.lost > 3 || !t.active) continue;
            AimTarget a;
            a.trackId = t.id;
            a.classId = t.classId;
            a.score = t.lastScore;
            a.cx = t.kf.x();
            a.cy = t.kf.y();
            a.vx = t.kf.vx();
            a.vy = t.kf.vy();
            // 用速度预测扩展框（供绘制/瞄准）
            a.x1 = t.kf.x() - 0.03f; a.y1 = t.kf.y() - 0.03f;
            a.x2 = t.kf.x() + 0.03f; a.y2 = t.kf.y() + 0.03f;
            out.push_back(a);
        }
        return out;
    }

    void reset() { m_tracks.clear(); m_nextId = 0; }

private:
    static float computeIoU(float kx, float ky, float dx, float dy) {
        // 简化为距离相似度（用固定小框的 IoU 近似）
        const float half = 0.03f;
        float x1 = std::max(kx - half, dx - half);
        float y1 = std::max(ky - half, dy - half);
        float x2 = std::min(kx + half, dx + half);
        float y2 = std::min(ky + half, dy + half);
        float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
        float area = (2*half)*(2*half);
        float uni = 2*area - inter;
        return uni > 0 ? inter / uni : 0.0f;
    }

    std::vector<Track> m_tracks;
    int m_nextId = 0;
};
