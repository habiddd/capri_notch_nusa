#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "whistle.h"
// MinGW spells these differently from the ORT header
#ifndef _stdcall
#define _stdcall __stdcall
#endif
#define _Frees_ptr_opt_
#include "onnxruntime_c_api.h"

// Camera hand control: a hand engine (MediaPipe, or RTMPose on the GPU) finds the 21 points, run by Windows' own ONNX
// Runtime (System32), so the webcam frames never leave this process. The worker turns the hand into mouse, media and volume actions.

struct Pt { float x, y; };
// A BGRA frame as Media Foundation gives it; pitch is bytes per row and may be negative (bottom-up)
struct Frame { const unsigned char* p; int w, h, pitch; };
struct Roi { float cx, cy, side, angle; }; // square crop in frame pixels, angle in radians

constexpr float PI_F = 3.14159265f;
constexpr int PALM_SIZE = 192, HAND_SIZE = 224;
constexpr float PALM_MIN_SCORE = 0.6f;
constexpr float HAND_MIN_PRESENCE = 0.5f;
constexpr float PINCH_PRESS = 0.25f;
constexpr float PINCH_RELEASE = 0.40f;   // above PINCH_PRESS, so a pinch that wobbles does not click twice
constexpr DWORD LOST_GRACE_MS = 300;     // tracking blinks for a frame or two
constexpr DWORD ROCK_HOLD_MS = 1000;     // the open palm is the neutral hand: raising it must not toggle music
constexpr DWORD FIST_HOLD_MS = 2000;
constexpr DWORD POSE_STABLE_MS = 100;    // a pose flickers for a frame or two; it counts once it holds this long
constexpr DWORD COUNT_STEP_MS = 1000;
constexpr DWORD CANCEL_SHOW_MS = 1500;   // the "cancelado" note stays this long
constexpr int LOCK_COUNT = 3;            // seconds shown before the PC locks
constexpr float SCROLL_STEP = 0.04f;     // fraction of the frame height per wheel notch
// A swipe is a quick move: a held thumb drifts up to 0.14 in 400 ms, a deliberate swipe reaches 0.28-0.34
constexpr DWORD SWIPE_MS = 400;          // how far back a swipe may start
constexpr float SWIPE_DIST = 0.18f;      // fraction of the frame height (or width, sideways)
constexpr DWORD SWIPE_COOLDOWN_MS = 600; // after a swipe, no swipe for this long
constexpr int VOLUME_KEYS = 5;           // volume key presses per swipe
constexpr float FINGER_EXT = 0.5f;       // Straight() above this: extended; below FINGER_FOLD: folded
constexpr float FINGER_FOLD = 0.0f;
constexpr float INDEX_REACH = 1.15f;     // the index tip this much farther from the wrist than its knuckle: pointing, not a thumbs-up
constexpr float FIST_FOLD = -0.6f;       // a fist folds middle, ring and pinky harder than the index
constexpr float THUMB_STRAIGHT = 0.6f;   // a thumb held out is straight; a fist wraps it
constexpr float ONE_EURO_MIN_CUTOFF = 0.3f, ONE_EURO_BETA = 6.4f, ONE_EURO_D_CUTOFF = 1.0f;
constexpr float CURSOR_DEAD = 0.0008f;   // about one camera pixel: smaller moves of the palm are noise
constexpr float GAIN_MIN = 512.f, GAIN_MAX = 3840.f; // screen pixels per unit of palm travel: slow is precise, fast covers the screen
constexpr float SPEED_FAST = 0.3125f;    // units per second at which the gain reaches GAIN_MAX
constexpr float DRAG_START = 0.01f;      // a pinched hand must move this far to drag; less is a click
constexpr DWORD LONG_PRESS_MS = 600;     // a pinch held this long, still, is a right click
constexpr DWORD POINT_RECENT_MS = 500;   // a pinch counts as a click from a point only if the point was seen this recently
// RTMPose (see RtmHand): the detection and pose thresholds, and the finger rules measured on its points
constexpr float RTM_DET_MIN = 0.3f;      // a detection must score this
constexpr float RTM_MIN_SCORE = 0.2f;    // the pose score that counts as a hand: real hands 0.2-0.76, a shirt logo 0.08-0.16
constexpr float RTM_EXT_LEN = 1.25f;     // tip-to-wrist over palm length: extended above this...
constexpr float RTM_FOLD_LEN = 1.05f;    // ...folded below this (a finger folded toward the camera is short but straight)
constexpr float RTM_THUMB_AWAY = 0.8f;   // a thumb tip this far from the ring knuckle (over palm length) is out, not wrapped

// 2016 anchors of the palm detector: 24x24 cells with 2 each, then 12x12 cells with 6 each
inline std::vector<Pt> PalmAnchors() {
    std::vector<Pt> a;
    const int layers[2][2] = { { 24, 2 }, { 12, 6 } }; // grid, anchors per cell
    for (const auto& L : layers)
        for (int y = 0; y < L[0]; ++y)
            for (int x = 0; x < L[0]; ++x)
                for (int k = 0; k < L[1]; ++k) a.push_back({ (x + 0.5f) / L[0], (y + 0.5f) / L[0] });
    return a;
}

// Bilinear sample of channel c (0=R, 1=G, 2=B) at (x,y); corners outside the frame count as 0
inline float Pixel(const Frame& f, float x, float y, int c) {
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    const int ch = 2 - c; // BGRA in memory
    auto at = [&](int px, int py) -> float {
        if (px < 0 || py < 0 || px >= f.w || py >= f.h) return 0.f;
        return f.p[(std::ptrdiff_t)py * f.pitch + px * 4 + ch];
    };
    return (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
}

// The frame scaled into the model's square, padded on the short side
struct Letter { float s, padX, padY; };
inline Letter LetterOf(int w, int h) {
    const float s = PALM_SIZE / (float)std::max(w, h);
    return { s, (PALM_SIZE - w * s) / 2, (PALM_SIZE - h * s) / 2 };
}

// Frame to 192x192 RGB floats (NHWC, 0..1): the palm model takes 0..1, unlike the hand model
inline void Letterbox(const Frame& f, float* out) {
    const Letter L = LetterOf(f.w, f.h);
    for (int v = 0; v < PALM_SIZE; ++v)
        for (int u = 0; u < PALM_SIZE; ++u) {
            const float x = (u + 0.5f - L.padX) / L.s - 0.5f, y = (v + 0.5f - L.padY) / L.s - 0.5f;
            for (int c = 0; c < 3; ++c) out[(v * PALM_SIZE + u) * 3 + c] = Pixel(f, x, y, c) / 255.f;
        }
}

inline float Wrap(float a) { // to [-pi, pi)
    a = std::fmod(a + PI_F, 2 * PI_F);
    if (a < 0) a += 2 * PI_F;
    return a - PI_F;
}

// The palm detector's best anchor as one hand's square crop; false when no anchor scores PALM_MIN_SCORE
inline bool DecodePalm(const float* reg, const float* logit, const std::vector<Pt>& anchors, int w, int h, Roi& roi) {
    size_t k = 0;
    for (size_t i = 1; i < anchors.size(); ++i)
        if (logit[i] > logit[k]) k = i;
    if (1 / (1 + std::exp(-logit[k])) < PALM_MIN_SCORE) return false;
    const Letter L = LetterOf(w, h);
    const float* r = reg + 18 * k; // 4 box values, then 7 keypoints
    const Pt a = anchors[k];
    auto toFrame = [&](float i, float j) { return Pt{ (i * PALM_SIZE - L.padX) / L.s, (j * PALM_SIZE - L.padY) / L.s }; };
    auto point = [&](int i) { return toFrame(r[4 + 2 * i] / PALM_SIZE + a.x, r[5 + 2 * i] / PALM_SIZE + a.y); };
    const Pt c = toFrame(r[0] / PALM_SIZE + a.x, r[1] / PALM_SIZE + a.y);
    const float W = r[2] / L.s, H = r[3] / L.s;
    const Pt kp0 = point(0), kp2 = point(2);
    const float angle = Wrap(PI_F / 2 - std::atan2(-(kp2.y - kp0.y), kp2.x - kp0.x));
    // shifted towards the fingers: the box centre is on the palm, the crop must hold the whole hand
    roi = { c.x + 0.5f * H * std::sin(angle), c.y - 0.5f * H * std::cos(angle), 2.6f * std::max(W, H), angle };
    return true;
}

// Crop (u,v) in HAND_SIZE pixels to the frame
inline Pt RoiToFrame(const Roi& r, float u, float v) {
    const float lx = (u / HAND_SIZE - 0.5f) * r.side, ly = (v / HAND_SIZE - 0.5f) * r.side;
    const float c = std::cos(r.angle), s = std::sin(r.angle);
    return { r.cx + lx * c - ly * s, r.cy + lx * s + ly * c };
}

// Frame to 224x224 RGB floats (NHWC, 0..255), the crop turned by the roi's angle: the hand model normalises inside its graph
inline void CropRoi(const Frame& f, const Roi& r, float* out) {
    for (int v = 0; v < HAND_SIZE; ++v)
        for (int u = 0; u < HAND_SIZE; ++u) {
            const Pt p = RoiToFrame(r, u + 0.5f, v + 0.5f);
            for (int c = 0; c < 3; ++c) out[(v * HAND_SIZE + u) * 3 + c] = Pixel(f, p.x, p.y, c);
        }
}

// Where the hand is, from its landmarks: the next frame's crop without running the palm detector
inline Roi RoiFromLandmarks(const Pt lm[21]) {
    const float angle = Wrap(PI_F / 2 - std::atan2(-(lm[9].y - lm[0].y), lm[9].x - lm[0].x));
    const float c = std::cos(angle), s = std::sin(angle);
    const int ids[12] = { 0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18 }; // the knuckles and wrist, not the fingertips
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (int id : ids) {
        const float x = lm[id].x * c + lm[id].y * s, y = -lm[id].x * s + lm[id].y * c; // turned by -angle
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
    }
    const float W = maxX - minX, H = maxY - minY;
    const float mx = (minX + maxX) / 2, my = (minY + maxY) / 2;
    float cx = mx * c - my * s, cy = mx * s + my * c; // back to the frame
    cx += 0.1f * H * s;
    cy -= 0.1f * H * c;
    return { cx, cy, 2.0f * std::max(W, H), angle };
}

enum Pose { POSE_NONE, POSE_POINT, POSE_V, POSE_PALM, POSE_FIST, POSE_THUMB, POSE_ROCK, POSE_OTHER };

inline float Dist(Pt a, Pt b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

// Wrap-safe: true once tick count t has reached `when` (GetTickCount wraps every 49.7 days)
inline bool TimeReached(DWORD t, DWORD when) { return (int)(t - when) >= 0; }

// Cosine of the angle at joint b: 1 when a, b, c are in a line, -1 when the finger folds back on itself
inline float Straight(const Pt lm[21], int a, int b, int c) {
    const float ux = lm[b].x - lm[a].x, uy = lm[b].y - lm[a].y, vx = lm[c].x - lm[b].x, vy = lm[c].y - lm[b].y;
    return (ux * vx + uy * vy) / (std::sqrt(ux * ux + uy * uy) * std::sqrt(vx * vx + vy * vy) + 1e-6f);
}

// Measured on live frames: extended fingers read +0.8..+1.0, folded -0.6..-1.0; a front fist wraps the thumb (about 0.1),
// thumbs-out and the resting fist keep it straight (about 0.8); the mouth pose has middle and ring only (about -0.45), so OTHER
inline Pose ClassifyPose(const Pt lm[21]) {
    const float i = Straight(lm, 5, 6, 8), m = Straight(lm, 9, 10, 12), r = Straight(lm, 13, 14, 16), k = Straight(lm, 17, 18, 20);
    // an index that is straight but curled in (a thumbs-up) has its tip no farther out than its knuckle
    const bool idx = i > FINGER_EXT && Dist(lm[8], lm[0]) > INDEX_REACH * Dist(lm[6], lm[0]);
    if (std::min({ i, m, r, k }) > FINGER_EXT) return POSE_PALM;
    if (idx && m > FINGER_EXT && r < FINGER_FOLD && k < FINGER_FOLD) return POSE_V;
    if (k > FINGER_EXT && m < FINGER_FOLD && r < FINGER_FOLD) return POSE_ROCK; // the index may be up or down
    if (idx && m < FINGER_FOLD && r < FINGER_FOLD && k < FINGER_FOLD) return POSE_POINT;
    if (m < FIST_FOLD && r < FIST_FOLD && k < FIST_FOLD && !idx) {
        const float th = std::min(Straight(lm, 1, 2, 3), Straight(lm, 2, 3, 4));
        return th > THUMB_STRAIGHT ? POSE_THUMB : POSE_FIST;
    }
    return POSE_OTHER;
}

// RTMPose's points: a finger is extended when its tip is far from the wrist and straight, folded when near or bent
inline Pose ClassifyPoseRtm(const Pt lm[21]) {
    const float palm = Dist(lm[0], lm[9]);
    const int tip[4] = { 8, 12, 16, 20 };
    const float s[4] = { Straight(lm, 5, 6, 8), Straight(lm, 9, 10, 12), Straight(lm, 13, 14, 16), Straight(lm, 17, 18, 20) };
    bool ext[4], fold[4];
    for (int f = 0; f < 4; ++f) {
        const float len = Dist(lm[tip[f]], lm[0]) / palm;
        ext[f] = len > RTM_EXT_LEN && s[f] > FINGER_EXT;
        fold[f] = len < RTM_FOLD_LEN || s[f] < FIST_FOLD;
    }
    if (ext[0] && ext[1] && ext[2] && ext[3]) return POSE_PALM;
    if (ext[0] && ext[1] && fold[2] && fold[3]) return POSE_V;
    if (ext[3] && fold[1] && fold[2]) return POSE_ROCK; // the index may be either way
    if (ext[0] && fold[1] && fold[2] && fold[3]) return POSE_POINT;
    if (fold[0] && fold[1] && fold[2] && fold[3]) {
        const float th = std::min(Straight(lm, 1, 2, 3), Straight(lm, 2, 3, 4));
        return (th > THUMB_STRAIGHT || Dist(lm[4], lm[13]) / palm > RTM_THUMB_AWAY) ? POSE_THUMB : POSE_FIST;
    }
    return POSE_OTHER;
}

// How closed a pinch is: thumb to fingertip, over the hand's size
inline float PinchRatio(const Pt lm[21], int tip) { return Dist(lm[4], lm[tip]) / Dist(lm[0], lm[9]); }

// One Euro filter: smooth when slow (no jitter), quick when fast (no lag)
class OneEuro {
public:
    float Filter(float x, double tSec) {
        if (!started) { started = true; x_ = x; t_ = tSec; return x; }
        const double dt = std::max(tSec - t_, 1e-3);
        t_ = tSec;
        const float dx = (float)((x - x_) / dt);
        const float edx = Alpha(dt, ONE_EURO_D_CUTOFF) * dx + (1 - Alpha(dt, ONE_EURO_D_CUTOFF)) * dx_;
        dx_ = edx;
        const float cutoff = ONE_EURO_MIN_CUTOFF + ONE_EURO_BETA * std::fabs(edx);
        x_ = Alpha(dt, cutoff) * x + (1 - Alpha(dt, cutoff)) * x_;
        return x_;
    }
    void Reset() { started = false; dx_ = 0; } // the next sample starts a new track, so a re-activation never jumps

private:
    static float Alpha(double dt, float cutoff) {
        const double r = 2 * 3.14159265358979 * cutoff * dt;
        return (float)(r / (r + 1));
    }
    bool started = false;
    double t_ = 0;
    float x_ = 0, dx_ = 0;
};

struct Action {
    enum Kind { MOVE, LDOWN, LUP, RCLICK, SCROLL, PLAYPAUSE, NEXT, PREV, VOLUP, VOLDOWN, COUNTDOWN, CANCEL, LOCK } kind;
    float x = 0, y = 0; // MOVE: the change in screen pixels
    int n = 0;          // SCROLL: +1 up, -1 down; COUNTDOWN: seconds left; VOLUP/VOLDOWN: key presses
};

struct ThumbPoint { DWORD t; float x, y; }; // where the hand was while the thumb pose held

// Turns each frame's hand into actions. Times are GetTickCount() milliseconds.
class GestureMachine {
public:
    // raw: this frame's pose. Holds, scroll, countdown, the thumb and the click pinch use the debounced pose or the raw pinch.
    std::vector<Action> Step(bool hand, Pose raw, float pinchL, Pt cursor, float handY, DWORD t) {
        std::vector<Action> out;
        if (hand) lastHand_ = t;
        else if (t - lastHand_ <= LOST_GRACE_MS) { ResetCursor(); return out; }
        else { // lost past the grace: no debounce; the button lets go and a pending pinch is dropped
            cand_ = POSE_NONE;
            candSince_ = t;
            SetPose(POSE_NONE, t, out);
            if (held_) { held_ = false; out.push_back({ Action::LUP }); }
            click_ = CLICK_IDLE;
            ResetCursor();
            return out;
        }
        if (raw != cand_) { cand_ = raw; candSince_ = t; }
        if (t - candSince_ >= POSE_STABLE_MS) SetPose(cand_, candSince_, out);
        const Pose pose = pose_;
        if (raw == POSE_POINT) lastPointT_ = t;
        // the debounced pose: a single raw frame of a fist or thumb (a noisy frame in a drag) is not a release
        const bool thumb = pose == POSE_THUMB, fist = pose == POSE_FIST;
        const bool pinchOpen = pinchL > PINCH_RELEASE, pinchShut = pinchL < PINCH_PRESS;

        // a drag (button down) ends when the pinch opens or the hand stops pointing
        if (held_ && (thumb || fist || !armed_ || pinchOpen)) { held_ = false; out.push_back({ Action::LUP }); }

        // A pinch from a POINT: opened quickly is a click, moved is a drag, held still is a right click
        if (click_ == CLICK_PENDING) {
            if (!armed_ || fist || thumb) click_ = CLICK_IDLE; // dropped silently
            else if (pinchOpen) { out.push_back({ Action::LDOWN }); out.push_back({ Action::LUP }); click_ = CLICK_IDLE; }
            else if (Dist(cursor, pressAt_) >= DRAG_START) { held_ = true; out.push_back({ Action::LDOWN }); click_ = CLICK_IDLE; }
            else if (t - pressT_ >= LONG_PRESS_MS) { out.push_back({ Action::RCLICK }); click_ = CLICK_RWAIT; }
        } else if (click_ == CLICK_RWAIT) {
            if (pinchOpen || !armed_ || fist || thumb) click_ = CLICK_IDLE;
        } else if (armed_ && !fist && !thumb && !held_ && pinchShut && t - lastPointT_ <= POINT_RECENT_MS) {
            click_ = CLICK_PENDING;
            pressT_ = t;
            pressAt_ = cursor;
        }

        // The cursor follows the palm while pointing or dragging: relative and accelerated; frozen while a pinch is pending
        const bool active = (raw == POSE_POINT && click_ != CLICK_PENDING && click_ != CLICK_RWAIT) || held_;
        if (!active) ResetCursor();
        else {
            const double sec = t / 1000.0;
            const Pt p = { fx_.Filter(cursor.x, sec), fy_.Filter(cursor.y, sec) };
            if (hasPrev_) {
                float dx = p.x - prev_.x, dy = p.y - prev_.y;
                const float len = std::sqrt(dx * dx + dy * dy), dt = std::max((float)(sec - prevSec_), 1e-3f);
                if (len < CURSOR_DEAD) dx = dy = 0;
                const float gain = std::min(GAIN_MAX, GAIN_MIN + (GAIN_MAX - GAIN_MIN) * (len / dt) / SPEED_FAST);
                const float px = dx * gain + remX_, py = dy * gain + remY_; // the fraction carries to the next frame
                const float ix = std::round(px), iy = std::round(py);
                remX_ = px - ix;
                remY_ = py - iy;
                if (ix != 0 || iy != 0) out.push_back({ Action::MOVE, ix, iy });
            }
            prev_ = p;
            prevSec_ = sec;
            hasPrev_ = true;
        }

        if (pose != POSE_V) scrollRef_ = handY;
        while (pose == POSE_V && std::fabs(handY - scrollRef_) >= SCROLL_STEP) {
            const int n = handY < scrollRef_ ? 1 : -1; // the hand moved up: wheel up
            out.push_back({ Action::SCROLL, 0, 0, n });
            scrollRef_ -= n * SCROLL_STEP;
        }

        // the thumb: a quick swipe up or down is volume, a quick swipe to the side is the track
        if (pose != POSE_THUMB) thumbPts_.clear();
        else {
            thumbPts_.push_back({ t, cursor.x, handY });
            while (t - thumbPts_.front().t > SWIPE_MS) thumbPts_.pop_front();
            if (TimeReached(t, swipeCooldown_)) {
                float bestDy = 0, bestDx = 0; // the largest travel from any point still kept
                for (const ThumbPoint& p : thumbPts_) {
                    if (std::fabs(handY - p.y) > std::fabs(bestDy)) bestDy = handY - p.y;
                    if (std::fabs(cursor.x - p.x) > std::fabs(bestDx)) bestDx = cursor.x - p.x;
                }
                const bool vertical = std::fabs(bestDy) >= SWIPE_DIST && std::fabs(bestDy) > std::fabs(bestDx);
                const bool sideways = std::fabs(bestDx) >= SWIPE_DIST && std::fabs(bestDx) > std::fabs(bestDy);
                if (vertical || sideways) {
                    if (vertical) out.push_back({ bestDy < 0 ? Action::VOLUP : Action::VOLDOWN, 0, 0, VOLUME_KEYS });
                    else out.push_back({ bestDx > 0 ? Action::NEXT : Action::PREV }); // mirrored: a move to the user's right is dx > 0
                    thumbPts_.clear();
                    swipeCooldown_ = t + SWIPE_COOLDOWN_MS;
                }
            }
        }

        const DWORD ms = t - poseSince_;
        if (pose == POSE_ROCK && !fired_ && ms >= ROCK_HOLD_MS) { out.push_back({ Action::PLAYPAUSE }); fired_ = true; }

        // COUNTDOWN 3, 2, 1 one second apart, then LOCK; one step per call, so a stall cannot fire the lock at once.
        // LOCK needs this frame to be a fist too: a glimpse of another pose waits, and the debounce cancels it.
        // nextStep_ spaces the steps in time, so a stall cannot show them in a few frames
        if (pose == POSE_FIST && countdown_ <= LOCK_COUNT && TimeReached(t, nextStep_) && ms >= FIST_HOLD_MS + countdown_ * COUNT_STEP_MS) {
            if (countdown_ < LOCK_COUNT) { out.push_back({ Action::COUNTDOWN, 0, 0, LOCK_COUNT - countdown_ }); ++countdown_; nextStep_ = t + COUNT_STEP_MS; }
            else if (raw == POSE_FIST) { out.push_back({ Action::LOCK }); ++countdown_; nextStep_ = t + COUNT_STEP_MS; }
        }
        return out;
    }

    bool Held() const { return held_; }
    Pose Current() const { return pose_; }

private:
    enum Click { CLICK_IDLE, CLICK_PENDING, CLICK_RWAIT }; // RWAIT: a right click fired, waiting for the pinch to open

    bool counting() const { return countdown_ > 0 && countdown_ <= LOCK_COUNT; }

    void ResetCursor() {
        fx_.Reset();
        fy_.Reset();
        hasPrev_ = false;
        remX_ = remY_ = 0;
    }

    // A new pose restarts the holds; leaving a fist cancels its countdown; leaving the POINT family disarms the click
    void SetPose(Pose p, DWORD since, std::vector<Action>& out) {
        if (p == pose_) return;
        if (counting()) out.push_back({ Action::CANCEL });
        if (p == POSE_POINT) armed_ = true;
        else if (p != POSE_OTHER) { // the hand is no longer pointing: a held button lets go
            armed_ = false;
            if (held_) { held_ = false; out.push_back({ Action::LUP }); }
        }
        pose_ = p;
        poseSince_ = since;
        countdown_ = 0;
        nextStep_ = 0;
        fired_ = false;
    }

    Pose pose_ = POSE_NONE, cand_ = POSE_NONE; // current (debounced) pose; the raw pose waiting to count
    DWORD candSince_ = 0;
    DWORD poseSince_ = 0, lastHand_ = 0, nextStep_ = 0, pressT_ = 0, lastPointT_ = 0;
    bool held_ = false, fired_ = false, armed_ = false;
    Click click_ = CLICK_IDLE;
    Pt pressAt_ = { 0, 0 };          // where the pinch closed
    int countdown_ = 0;              // countdown messages sent for this fist; LOCK_COUNT + 1 once lock is sent
    float scrollRef_ = 0;
    std::deque<ThumbPoint> thumbPts_; // the thumb's last SWIPE_MS of hand points
    DWORD swipeCooldown_ = 0;         // no swipe before this tick count
    OneEuro fx_, fy_;                 // the palm centre, filtered while the cursor is active
    Pt prev_ = { 0, 0 };              // the filtered palm centre last frame
    double prevSec_ = 0;
    bool hasPrev_ = false;
    float remX_ = 0, remY_ = 0;       // sub-pixel remainders carried between frames
};

// ---- RTMPose hands: the same 21 points as MediaPipe, from a person detector and a pose model on the GPU ----
// The math is the offline bench's (verified on the user's clips): change it only with a new bench run

// RTMDet input: top-left letterbox to 320 on grey 114, BGR, mean/std, NCHW. Returns the scale to the frame.
inline float RtmDetInput(const Frame& f, float* din) {
    const float r = std::min(320.f / f.h, 320.f / f.w);
    const float mean[3] = { 103.53f, 116.28f, 123.675f }, sd[3] = { 57.375f, 57.12f, 58.395f };
    for (int y = 0; y < 320; ++y)
        for (int x = 0; x < 320; ++x)
            for (int c = 0; c < 3; ++c) { // c: 0=B 1=G 2=R
                const float v = (x < f.w * r && y < f.h * r) ? Pixel(f, (x + 0.5f) / r - 0.5f, (y + 0.5f) / r - 0.5f, 2 - c) : 114.f;
                din[(size_t)c * 320 * 320 + y * 320 + x] = (v - mean[c]) / sd[c];
            }
    return r;
}

// The best detection scoring RTM_DET_MIN or more, as a box in frame pixels; false when there is none
inline bool RtmPickBox(const float* dets, size_t n, float r, float box[4]) {
    int best = -1;
    for (size_t i = 0; i < n; ++i)
        if (dets[i * 5 + 4] > RTM_DET_MIN && (best < 0 || dets[i * 5 + 4] > dets[best * 5 + 4])) best = (int)i;
    if (best >= 0)
        for (int k = 0; k < 4; ++k) box[k] = dets[best * 5 + k] / r;
    return best >= 0;
}

// The pose crop: the centre of the box, and a side 1.25 times its longer edge
inline void RtmCrop(const float box[4], float& cx, float& cy, float& s) {
    cx = (box[0] + box[2]) / 2;
    cy = (box[1] + box[3]) / 2;
    s = std::max(box[2] - box[0], box[3] - box[1]) * 1.25f;
}

// RTMPose input: the box's crop at 256 (no rotation), RGB mean/std, NCHW
inline void RtmPoseInput(const Frame& f, const float box[4], float* pin) {
    float cx, cy, s;
    RtmCrop(box, cx, cy, s);
    const float mean[3] = { 123.675f, 116.28f, 103.53f }, sd[3] = { 58.395f, 57.12f, 57.375f };
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const float sx = cx + (x - 128) * s / 256, sy = cy + (y - 128) * s / 256;
            for (int c = 0; c < 3; ++c) pin[(size_t)c * 65536 + y * 256 + x] = (Pixel(f, sx, sy, c) - mean[c]) / sd[c];
        }
}

// A SimCC bin (512 per axis, two per crop pixel) back to frame pixels
inline Pt RtmKeypoint(int ix, int iy, float cx, float cy, float s) {
    return { ix / 2.f / 256 * s + cx - s / 2, iy / 2.f / 256 * s + cy - s / 2 };
}

// The 21 points from the SimCC arrays; returns their mean confidence
inline float RtmDecode(const float* sx, const float* sy, float cx, float cy, float s, Pt kp[21]) {
    float score = 0;
    for (int k = 0; k < 21; ++k) {
        const float* px = sx + k * 512;
        const float* py = sy + k * 512;
        const int ix = (int)(std::max_element(px, px + 512) - px), iy = (int)(std::max_element(py, py + 512) - py);
        score += 0.5f * (px[ix] + py[iy]) / 21;
        kp[k] = RtmKeypoint(ix, iy, cx, cy, s);
    }
    return score;
}

// The next frame's box: a square around the points' extent, 1.1 times its longer side
inline void RtmBox(const Pt kp[21], float box[4]) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < 21; ++i) {
        x0 = std::min(x0, kp[i].x); y0 = std::min(y0, kp[i].y);
        x1 = std::max(x1, kp[i].x); y1 = std::max(y1, kp[i].y);
    }
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, s = std::max(x1 - x0, y1 - y0) * 1.1f;
    box[0] = cx - s / 2; box[1] = cy - s / 2; box[2] = cx + s / 2; box[3] = cy + s / 2;
}

// ---- Worker: the webcam, the models and the actions (a process of its own, like MirageWorker) ----

inline void SendMouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof in);
}

// Media keys go through keybd_event as src/media.h does
inline void PressKey(BYTE vk) {
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY, 0);
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
}

inline void RunAction(const Action& a, HWND notch) {
    switch (a.kind) {
    case Action::MOVE: { POINT p = {}; GetCursorPos(&p); SetCursorPos(p.x + (int)a.x, p.y + (int)a.y); break; }
    case Action::LDOWN: SendMouse(MOUSEEVENTF_LEFTDOWN); break;
    case Action::LUP: SendMouse(MOUSEEVENTF_LEFTUP); break;
    case Action::RCLICK: SendMouse(MOUSEEVENTF_RIGHTDOWN); SendMouse(MOUSEEVENTF_RIGHTUP); break;
    case Action::SCROLL: SendMouse(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(a.n * WHEEL_DELTA)); break;
    case Action::PLAYPAUSE: PressKey(VK_MEDIA_PLAY_PAUSE); break;
    case Action::NEXT: PressKey(VK_MEDIA_NEXT_TRACK); break;
    case Action::PREV: PressKey(VK_MEDIA_PREV_TRACK); break;
    case Action::VOLUP: for (int i = 0; i < a.n; ++i) PressKey(VK_VOLUME_UP); break;
    case Action::VOLDOWN: for (int i = 0; i < a.n; ++i) PressKey(VK_VOLUME_DOWN); break;
    case Action::COUNTDOWN: WhistleSend(notch, WHISTLE_ALERT, "Bloqueando el PC en " + std::to_string(a.n) + "\xE2\x80\xA6 abre la mano para cancelar", CACTUS_CAMERA); break;
    case Action::CANCEL: WhistleSend(notch, WHISTLE_TEXT, "Bloqueo cancelado", CACTUS_CAMERA); break;
    case Action::LOCK: LockWorkStation(); break;
    }
}

// The notch's card line for what the hand is doing; "" for no gesture
inline const char* StatusLabel(bool hand, Pose p, bool held) {
    if (!hand) return "Sin mano a la vista";
    if (held) return "Arrastrando";
    switch (p) {
    case POSE_POINT: return "Cursor";
    case POSE_V: return "Desplazando";
    case POSE_ROCK: return "Cuernos: reproducir o pausar";
    case POSE_FIST: return "Pu\xC3\xB1o: mant\xC3\xA9n 2 s para bloquear el PC";
    case POSE_THUMB: return "Pulgar: desliza arriba/abajo = volumen, a los lados = pista";
    default: return "";
    }
}

// Releases an ORT status; true when there was no error
inline bool OrtOk(const OrtApi* ort, OrtStatus* s) {
    if (s) ort->ReleaseStatus(s);
    return s == NULL;
}

// An RTMPose model on the GPU (DirectML) when it takes, else on the CPU with 4 threads; false only when neither loads
inline bool OpenRtmModel(const OrtApi* ort, OrtEnv* env, const std::wstring& path, OrtSession** out) {
    const char* keys[1] = { "device_id" };
    const char* vals[1] = { "0" };
    OrtSessionOptions* so = NULL;
    if (OrtOk(ort, ort->CreateSessionOptions(&so)) && OrtOk(ort, ort->SessionOptionsAppendExecutionProvider(so, "DML", keys, vals, 1)) &&
        OrtOk(ort, ort->CreateSession(env, path.c_str(), so, out))) {
        ort->ReleaseSessionOptions(so);
        return true;
    }
    if (so) ort->ReleaseSessionOptions(so);
    so = NULL;
    const bool cpu = OrtOk(ort, ort->CreateSessionOptions(&so)) && OrtOk(ort, ort->SetIntraOpNumThreads(so, 4)) &&
                     OrtOk(ort, ort->CreateSession(env, path.c_str(), so, out));
    if (so) ort->ReleaseSessionOptions(so);
    return cpu;
}

// The RTMPose pair: detector, then pose. err is set when ORT refuses a call; the worker then stops with the models message.
struct RtmHand {
    const OrtApi* ort;
    OrtMemoryInfo* mi;
    OrtSession *det = NULL, *pose = NULL;
    std::vector<float> din = std::vector<float>(3 * 320 * 320), pin = std::vector<float>(3 * 256 * 256);
    bool err = false;

    // Runs s on one float input tensor; the outputs are for the caller to read and release
    bool Run(OrtSession* s, float* data, size_t n, int64_t* sh, const char* const* outNames, OrtValue** outs) {
        const char* inName = "input";
        OrtValue* in = NULL;
        const bool made = OrtOk(ort, ort->CreateTensorWithDataAsOrtValue(mi, data, n * sizeof(float), sh, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
        const bool ran = made && OrtOk(ort, ort->Run(s, NULL, &inName, &in, 1, outNames, 2, outs));
        if (in) ort->ReleaseValue(in);
        err = err || !ran;
        return ran;
    }

    // The best person's box in frame pixels; false when none scores RTM_DET_MIN (or on an ORT error)
    bool Detect(const Frame& f, float box[4]) {
        const float r = RtmDetInput(f, din.data());
        int64_t sh[4] = { 1, 3, 320, 320 };
        const char* outN[2] = { "dets", "labels" };
        OrtValue* o[2] = {};
        if (!Run(det, din.data(), din.size(), sh, outN, o)) return false;
        OrtTensorTypeAndShapeInfo* ti = NULL;
        size_t n = 0;
        float* d = NULL;
        const bool read = OrtOk(ort, ort->GetTensorTypeAndShape(o[0], &ti)) && OrtOk(ort, ort->GetTensorShapeElementCount(ti, &n)) &&
                          OrtOk(ort, ort->GetTensorMutableData(o[0], (void**)&d));
        if (ti) ort->ReleaseTensorTypeAndShapeInfo(ti);
        const bool found = read && RtmPickBox(d, n / 5, r, box);
        ort->ReleaseValue(o[0]);
        ort->ReleaseValue(o[1]);
        err = err || !read;
        return found;
    }

    // The mean confidence of the pose and the 21 points in frame pixels (set even when the confidence is low)
    float Keypoints(const Frame& f, const float box[4], Pt kp[21]) {
        RtmPoseInput(f, box, pin.data());
        int64_t sh[4] = { 1, 3, 256, 256 };
        const char* outN[2] = { "simcc_x", "simcc_y" };
        OrtValue* o[2] = {};
        if (!Run(pose, pin.data(), pin.size(), sh, outN, o)) return 0;
        float *sx = NULL, *sy = NULL;
        const bool read = OrtOk(ort, ort->GetTensorMutableData(o[0], (void**)&sx)) && OrtOk(ort, ort->GetTensorMutableData(o[1], (void**)&sy));
        float cx, cy, s;
        RtmCrop(box, cx, cy, s);
        const float score = read ? RtmDecode(sx, sy, cx, cy, s, kp) : 0;
        ort->ReleaseValue(o[0]);
        ort->ReleaseValue(o[1]);
        err = err || !read;
        return score;
    }
};

// Loads the models of the chosen hand engine and reads the webcam until the notch is gone; failures go to the notch as WHISTLE_ERROR
inline int CameraWorker(HWND notch) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // cursor deltas must be physical pixels on scaled displays
    auto fail = [&](const char* why) { WhistleSend(notch, WHISTLE_ERROR, why, CACTUS_CAMERA); return 1; };
    const char* const BROKEN = "Modelos de la c\xC3\xA1mara da\xC3\xB1" "ados. Desc\xC3\xA1rgalos otra vez en Configuraci\xC3\xB3n.";
    typedef const OrtApiBase* (ORT_API_CALL* BaseFn)(void);
    HMODULE lib = LoadLibraryExW(L"onnxruntime.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    BaseFn base = lib ? (BaseFn)(void*)GetProcAddress(lib, "OrtGetApiBase") : NULL;
    const OrtApi* ort = base ? base()->GetApi(ORT_API_VERSION) : NULL;
    if (!ort) return fail("Tu Windows no trae ONNX Runtime: hace falta Windows 11 al d\xC3\xAD" "a.");

    // Every ORT status is released here; true when it was not an error
    auto ok = [&](OrtStatus* s) { return OrtOk(ort, s); };
    const char* inName = "input_1";
    const char* const outNames[2] = { "Identity", "Identity_1" }; // palm: regressors, logits; hand: landmarks, presence
    std::vector<float> palmBuf(PALM_SIZE * PALM_SIZE * 3), handBuf(HAND_SIZE * HAND_SIZE * 3);
    const int64_t palmShape[4] = { 1, PALM_SIZE, PALM_SIZE, 3 }, handShape[4] = { 1, HAND_SIZE, HAND_SIZE, 3 };
    const int engine = g_settings.handEngine;
    OrtEnv* env = NULL;
    OrtSessionOptions* so = NULL;
    OrtSession *palmS = NULL, *handS = NULL;
    OrtMemoryInfo* mem = NULL;
    OrtValue *palmIn = NULL, *handIn = NULL;
    bool ready = ok(ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "capri", &env)) && ok(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem));
    RtmHand rtm = { ort, mem };
    if (engine == HAND_RTMPOSE) {
        ready = ready && OpenRtmModel(ort, env, HandModel(HAND_RTMPOSE, 0), &rtm.det) && OpenRtmModel(ort, env, HandModel(HAND_RTMPOSE, 1), &rtm.pose);
    } else {
        ready = ready && ok(ort->CreateSessionOptions(&so)) && ok(ort->SetIntraOpNumThreads(so, 2)) &&
                ok(ort->CreateSession(env, HandModel(HAND_MEDIAPIPE, 0).c_str(), so, &palmS)) &&
                ok(ort->CreateSession(env, HandModel(HAND_MEDIAPIPE, 1).c_str(), so, &handS)) &&
                ok(ort->CreateTensorWithDataAsOrtValue(mem, palmBuf.data(), palmBuf.size() * sizeof(float), palmShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &palmIn)) &&
                ok(ort->CreateTensorWithDataAsOrtValue(mem, handBuf.data(), handBuf.size() * sizeof(float), handShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &handIn));
    }
    if (!ready) return fail(BROKEN);
    // The tensors wrap the buffers, which are refilled each frame; outputs are allocated by ORT and released after reading
    auto run = [&](OrtSession* s, OrtValue* in, OrtValue** outs) { return ok(ort->Run(s, NULL, &inName, &in, 1, outNames, 2, outs)); };
    auto floats = [&](OrtValue* v) { void* p = NULL; return ok(ort->GetTensorMutableData(v, &p)) ? (const float*)p : (const float*)NULL; };

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    IMFAttributes* attr = NULL;
    IMFActivate** devs = NULL;
    UINT32 nDevs = 0;
    MFCreateAttributes(&attr, 1);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    MFEnumDeviceSources(attr, &devs, &nDevs);
    if (nDevs == 0) return fail("No hay ninguna c\xC3\xA1mara.");
    IMFMediaSource* src = NULL;
    IMFSourceReader* reader = NULL;
    IMFAttributes* readerAttr = NULL;
    IMFMediaType* type = NULL;
    IMFMediaType* cur = NULL;
    HRESULT hr = devs[0]->ActivateObject(IID_PPV_ARGS(&src));
    if (SUCCEEDED(hr)) {
        MFCreateAttributes(&readerAttr, 1);
        readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        hr = MFCreateSourceReaderFromMediaSource(src, readerAttr, &reader);
    }
    if (SUCCEEDED(hr)) {
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, type);
    }
    if (FAILED(hr)) return fail("La c\xC3\xA1mara est\xC3\xA1 en uso por otra app.");
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    UINT32 fw = 0, fh = 0;
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &fw, &fh);
    WhistleSend(notch, WHISTLE_READY, "", CACTUS_CAMERA);

    const std::vector<Pt> anchors = PalmAnchors();
    GestureMachine gestures;
    Roi roi = {};
    Pt lm[21] = {};
    bool tracking = false;
    unsigned frameNo = 0;
    const char* shown = NULL; // the status line last sent
    DWORD cancelUntil = 0;    // no status line before this tick count, so the cancel note stays readable
    while (IsWindow(notch)) {
        IMFSample* sample = NULL;
        DWORD flags = 0;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, NULL, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) return fail("La c\xC3\xA1mara se desconect\xC3\xB3.");
        if (!sample) continue;
        IMFMediaBuffer* buf = NULL;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
            BYTE* px = NULL;
            LONG pitch = (LONG)fw * 4;
            IMF2DBuffer* b2 = NULL;
            const bool has2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2)));
            const bool locked = has2d ? SUCCEEDED(b2->Lock2D(&px, &pitch)) : SUCCEEDED(buf->Lock(&px, NULL, NULL));
            if (locked) {
                const Frame f = { px, (int)fw, (int)fh, (int)pitch };
                if (engine == HAND_RTMPOSE) {
                    // the detector when nothing is tracked (every third frame, as MediaPipe's palm), else last frame's points
                    float box[4] = {};
                    bool have = tracking;
                    if (tracking) RtmBox(lm, box);
                    else have = frameNo++ % 3 == 0 && rtm.Detect(f, box);
                    tracking = have && rtm.Keypoints(f, box, lm) >= RTM_MIN_SCORE;
                    if (rtm.err) return fail(BROKEN);
                } else {
                    // Not tracking: palm detection every third frame; tracking: the crop follows the last landmarks
                    if (tracking) roi = RoiFromLandmarks(lm);
                    bool have = tracking;
                    if (!tracking && frameNo++ % 3 == 0) {
                        Letterbox(f, palmBuf.data());
                        OrtValue* outs[2] = {};
                        if (!run(palmS, palmIn, outs)) return fail(BROKEN);
                        const float* reg = floats(outs[0]);
                        const float* logit = floats(outs[1]);
                        have = reg && logit && DecodePalm(reg, logit, anchors, (int)fw, (int)fh, roi);
                        for (OrtValue* o : outs) ort->ReleaseValue(o);
                    }
                    tracking = false;
                    if (have) {
                        CropRoi(f, roi, handBuf.data());
                        OrtValue* outs[2] = {};
                        if (!run(handS, handIn, outs)) return fail(BROKEN);
                        const float* xyz = floats(outs[0]);
                        const float* presence = floats(outs[1]);
                        tracking = xyz && presence && presence[0] >= HAND_MIN_PRESENCE;
                        if (tracking)
                            for (int i = 0; i < 21; ++i) lm[i] = RoiToFrame(roi, xyz[i * 3], xyz[i * 3 + 1]);
                        for (OrtValue* o : outs) ort->ReleaseValue(o);
                    }
                }
                const bool hand = tracking;
                Pose pose = POSE_NONE;
                float pinchL = 1, handY = 0;
                Pt cursor = { 0, 0 };
                if (hand) {
                    pose = engine == HAND_RTMPOSE ? ClassifyPoseRtm(lm) : ClassifyPose(lm);
                    pinchL = PinchRatio(lm, 8);
                    // the palm's centre, both axes over the frame width (same units), mirrored: the camera is not
                    const float fwf = (float)f.w;
                    const float cx = (lm[0].x + lm[5].x + lm[9].x + lm[13].x + lm[17].x) / 5;
                    const float cy = (lm[0].y + lm[5].y + lm[9].y + lm[13].y + lm[17].y) / 5;
                    cursor = { (fwf - cx) / fwf, cy / fwf };
                    handY = lm[9].y / f.h;
                }
                const DWORD now = GetTickCount();
                for (const Action& a : gestures.Step(hand, pose, pinchL, cursor, handY, now)) {
                    RunAction(a, notch);
                    if (a.kind == Action::CANCEL) { cancelUntil = now + CANCEL_SHOW_MS; shown = NULL; } // the status line returns after the note
                }
                const char* label = StatusLabel(hand, gestures.Current(), gestures.Held());
                if (label != shown && TimeReached(now, cancelUntil)) { shown = label; WhistleSend(notch, WHISTLE_TEXT, label, CACTUS_CAMERA); }
            }
            if (locked && has2d) b2->Unlock2D();
            else if (locked) buf->Unlock();
            if (b2) b2->Release();
            buf->Release();
        }
        sample->Release();
    }
    if (gestures.Held()) SendMouse(MOUSEEVENTF_LEFTUP); // the notch closed mid-drag
    return 0;
}
