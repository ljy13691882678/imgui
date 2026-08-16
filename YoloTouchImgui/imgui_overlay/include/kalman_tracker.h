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
        float w = 0.0f;         // 检测框宽（归一化，EMA 平滑）
        float h = 0.0f;         // 检测框高（归一化，EMA 平滑）
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
                // 保存原始检测框尺寸（EMA 平滑，避免框大小跳动）
                float nw = detections[bestIdx].x2 - detections[bestIdx].x1;
                float nh = detections[bestIdx].y2 - detections[bestIdx].y1;
                if (t.w <= 0.0f) { t.w = nw; t.h = nh; }
                else { t.w = t.w * 0.7f + nw * 0.3f; t.h = t.h * 0.7f + nh * 0.3f; }
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
            tr.w = detections[i].x2 - detections[i].x1;
            tr.h = detections[i].y2 - detections[i].y1;
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
            // 用跟踪器保存的原始检测框尺寸（而非固定 0.03），保证框大小与目标一致
            float hw = t.w * 0.5f, hh = t.h * 0.5f;
            a.x1 = t.kf.x() - hw; a.y1 = t.kf.y() - hh;
            a.x2 = t.kf.x() + hw; a.y2 = t.kf.y() + hh;
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
