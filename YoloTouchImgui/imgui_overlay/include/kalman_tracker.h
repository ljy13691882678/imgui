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

        // 2. 关联：贪心匹配
        //    优先用中心距离门控（比固定框 IoU 更稳定，适应快速移动的目标）
        //    中心距离 < gate * (track_size + det_size) 时直接关联，不依赖 IoU
        //    中心距离 > 远距离门限时用宽松 IoU 兜底
        std::vector<bool> matchedDets(detections.size(), false);
        for (auto& t : m_tracks) {
            int bestIdx = -1;
            float bestScore = -1.0f;
            float trackHalfX = std::max(0.01f, t.w * 0.5f);
            float trackHalfY = std::max(0.01f, t.h * 0.5f);

            for (size_t i = 0; i < detections.size(); ++i) {
                if (matchedDets[i]) continue;

                // 中心距离门控（归一化像素距离）
                float dx = detections[i].cx - t.kf.x();
                float dy = detections[i].cy - t.kf.y();
                float dist = std::sqrt(dx*dx + dy*dy);

                float detW = detections[i].x2 - detections[i].x1;
                float detH = detections[i].y2 - detections[i].y1;
                float detHalfX = std::max(0.01f, detW * 0.5f);
                float detHalfY = std::max(0.01f, detH * 0.5f);

                // 宽松门限：目标框对角线长度的 2 倍（归一化值）
                float gateX = (trackHalfX + detHalfX) * 1.2f;
                float gateY = (trackHalfY + detHalfY) * 1.2f;

                float matchScore = -1.0f;
                if (std::fabs(dx) <= gateX && std::fabs(dy) <= gateY) {
                    // 在门控内：用 IoU 精确评分
                    float iou = computeIoU(t.kf.x(), t.kf.y(),
                                           detections[i].cx, detections[i].cy,
                                           trackHalfX, trackHalfY,
                                           detHalfX, detHalfY);
                    if (iou > 0.08f) matchScore = iou;
                    else matchScore = 0.5f;  // 距离很近但 IoU 低时仍关联（容忍框大小抖动）
                } else {
                    // 距离较远：用宽松 IoU 兜底（阈值降低到 0.12）
                    float iou = computeIoU(t.kf.x(), t.kf.y(),
                                           detections[i].cx, detections[i].cy,
                                           trackHalfX, trackHalfY,
                                           detHalfX, detHalfY);
                    if (iou > 0.12f) matchScore = iou;
                }

                if (matchScore > bestScore) {
                    bestScore = matchScore;
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
                else { t.w = t.w * 0.6f + nw * 0.4f; t.h = t.h * 0.6f + nh * 0.4f; }
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

        // 4. 清理丢失过久的轨迹（延长丢失容忍到 20 帧，避免偶发丢失导致闪框）
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
            [](const Track& t) { return t.lost > 20; }), m_tracks.end());
    }

    // 返回活跃轨迹（转为 AimTarget）
    std::vector<AimTarget> activeTargets() const {
        std::vector<AimTarget> out;
        for (const auto& t : m_tracks) {
            // 延长丢失容忍到 6 帧（原 3 帧），让偶发丢失/漏帧的目标不闪框
            if (t.lost > 6 || !t.active) continue;
            AimTarget a;
            a.trackId = t.id;
            a.classId = t.classId;
            a.score = t.lastScore;
            a.cx = t.kf.x();
            a.cy = t.kf.y();
            a.vx = t.kf.vx();
            a.vy = t.kf.vy();
            // 用跟踪器保存的 EMA 平滑后的框尺寸，保证框大小稳定
            float hw = t.w * 0.5f, hh = t.h * 0.5f;
            a.x1 = t.kf.x() - hw; a.y1 = t.kf.y() - hh;
            a.x2 = t.kf.x() + hw; a.y2 = t.kf.y() + hh;
            out.push_back(a);
        }
        return out;
    }

    void reset() { m_tracks.clear(); m_nextId = 0; }

private:
    // 计算 IoU，允许指定两个框的半宽半高（更精确）
    static float computeIoU(float kx, float ky, float dx, float dy,
                            float khx, float khy, float dhx, float dhy) {
        float x1 = std::max(kx - khx, dx - dhx);
        float y1 = std::max(ky - khy, dy - dhy);
        float x2 = std::min(kx + khx, dx + dhx);
        float y2 = std::min(ky + khy, dy + dhy);
        float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
        float areaA = (2*khx) * (2*khy);
        float areaB = (2*dhx) * (2*dhy);
        float uni = areaA + areaB - inter;
        return uni > 0.0f ? inter / uni : 0.0f;
    }

    std::vector<Track> m_tracks;
    int m_nextId = 0;
};
