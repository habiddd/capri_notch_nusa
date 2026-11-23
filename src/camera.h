#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
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
// Runtime (System32), so the webcam frames never leave this process. The worker matches the hand against the user's recorded
// gestures (src/gesture_templates.h) and turns them into volume, track and lock actions.

struct Pt { float x, y; };
// A BGRA frame as Media Foundation gives it; pitch is bytes per row and may be negative (bottom-up)
struct Frame { const unsigned char* p; int w, h, pitch; };
struct Roi { float cx, cy, side, angle; }; // square crop in frame pixels, angle in radians

constexpr float PI_F = 3.14159265f;
constexpr int PALM_SIZE = 192, HAND_SIZE = 224;
constexpr float PALM_MIN_SCORE = 0.6f;
constexpr float HAND_MIN_PRESENCE = 0.5f;
constexpr DWORD LOST_GRACE_MS = 300;     // tracking blinks for a frame or two
constexpr DWORD POSE_STABLE_MS = 300;    // a gesture counts once it holds this long: on the clips, 300 ms left no false action
constexpr DWORD VOLUME_REPEAT_MS = 300;  // a held volume gesture steps again this often
constexpr int VOLUME_KEYS = 2;           // volume key presses per step
constexpr DWORD LOCK_HOLD_MS = 1000;     // the lock pose held this long starts the countdown
constexpr DWORD COUNT_STEP_MS = 1000;
constexpr DWORD CANCEL_SHOW_MS = 1500;   // the "cancelado" note stays this long
constexpr int LOCK_COUNT = 3;            // seconds shown before the PC locks
// RTMPose (see RtmHand): the detection and pose thresholds
constexpr float RTM_DET_MIN = 0.3f;      // a detection must score this
constexpr float RTM_MIN_SCORE = 0.2f;    // the pose score that counts as a hand: real hands 0.2-0.76, a shirt logo 0.08-0.16

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

// Frame to 224x224 RGB floats (NHWC, 0..1), the crop turned by the roi's angle.
// 0..1 like the palm model: on a live frame 0..255 read presence 0.001, 0..1 read 0.998
inline void CropRoi(const Frame& f, const Roi& r, float* out) {
    for (int v = 0; v < HAND_SIZE; ++v)
        for (int u = 0; u < HAND_SIZE; ++u) {
            const Pt p = RoiToFrame(r, u + 0.5f, v + 0.5f);
            for (int c = 0; c < 3; ++c) out[(v * HAND_SIZE + u) * 3 + c] = Pixel(f, p.x, p.y, c) / 255.f;
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

enum Pose { POSE_NONE, POSE_VOLUP, POSE_VOLDOWN, POSE_NEXT, POSE_PREV, POSE_LOCK };

// Wrap-safe: true once tick count t has reached `when` (GetTickCount wraps every 49.7 days)
inline bool TimeReached(DWORD t, DWORD when) { return (int)(t - when) >= 0; }

// The hand as 40 numbers: points 1..20 from the wrist, over the points' longer box side.
// Not turned upright: two fingers up, down, left and right are four different gestures
constexpr int GESTURE_DIM = 40;
inline void GestureFeatures(const Pt lm[21], float out[GESTURE_DIM]) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < 21; ++i) {
        x0 = std::min(x0, lm[i].x); y0 = std::min(y0, lm[i].y);
        x1 = std::max(x1, lm[i].x); y1 = std::max(y1, lm[i].y);
    }
    const float s = std::max(std::max(x1 - x0, y1 - y0), 1.f);
    for (int i = 1; i < 21; ++i) {
        out[(i - 1) * 2] = (lm[i].x - lm[0].x) / s;
        out[(i - 1) * 2 + 1] = (lm[i].y - lm[0].y) / s;
    }
}

struct GestureTemplate { Pose pose; float f[GESTURE_DIM]; };
#include "gesture_templates.h"

// Nearest-template distance above which a match is no gesture, by pose. Measured on the clips (train on one half, test on
// the other): volume up, next, prev and lock match at a median 0.09-0.24 while the moving hand reached them at 0.58-1.44.
// Volume down varies more (up to 3.1) and the moving hand never matched it, so it takes any distance
constexpr float GESTURE_REJECT[] = { 1e9f, 0.8f, 1e9f, 0.8f, 0.8f, 0.8f };

// The recorded gesture nearest to this hand, or POSE_NONE
inline Pose ClassifyGesture(const Pt lm[21]) {
    float f[GESTURE_DIM];
    GestureFeatures(lm, f);
    float best = 1e30f;
    Pose pose = POSE_NONE;
    for (const GestureTemplate& g : GESTURE_TEMPLATES) {
        float d = 0;
        for (int i = 0; i < GESTURE_DIM; ++i) d += (f[i] - g.f[i]) * (f[i] - g.f[i]);
        if (d < best) { best = d; pose = g.pose; }
    }
    return std::sqrt(best) > GESTURE_REJECT[pose] ? POSE_NONE : pose;
}

struct Action {
    enum Kind { NEXT, PREV, VOLUP, VOLDOWN, COUNTDOWN, CANCEL, LOCK } kind;
    int n = 0; // COUNTDOWN: seconds left; VOLUP/VOLDOWN: key presses
};

// Turns each frame's gesture into actions. Times are GetTickCount() milliseconds.
class GestureMachine {
public:
    // raw: this frame's gesture (POSE_NONE without a hand). It counts once it holds POSE_STABLE_MS.
    std::vector<Action> Step(bool hand, Pose raw, DWORD t) {
        std::vector<Action> out;
        if (hand) lastHand_ = t;
        else if (t - lastHand_ <= LOST_GRACE_MS) return out; // tracking blinks for a frame or two
        if (raw != cand_) { cand_ = raw; candSince_ = t; }
        if (!hand || t - candSince_ >= POSE_STABLE_MS) SetPose(cand_, candSince_, out); // a lost hand ends the gesture at once
        const DWORD ms = t - poseSince_;
        if ((pose_ == POSE_VOLUP || pose_ == POSE_VOLDOWN) && TimeReached(t, nextStep_)) { // a step now, then one every VOLUME_REPEAT_MS
            out.push_back({ pose_ == POSE_VOLUP ? Action::VOLUP : Action::VOLDOWN, VOLUME_KEYS });
            nextStep_ = t + VOLUME_REPEAT_MS;
        }
        if ((pose_ == POSE_NEXT || pose_ == POSE_PREV) && !fired_) { out.push_back({ pose_ == POSE_NEXT ? Action::NEXT : Action::PREV }); fired_ = true; }
        // COUNTDOWN 3, 2, 1 one second apart, then LOCK; one step per call, so a stall cannot fire the lock at once.
        // LOCK needs this frame to be the lock pose too: a glimpse of another pose waits, and the debounce cancels it.
        if (pose_ == POSE_LOCK && countdown_ <= LOCK_COUNT && TimeReached(t, nextStep_) && ms >= LOCK_HOLD_MS + countdown_ * COUNT_STEP_MS) {
            if (countdown_ < LOCK_COUNT || raw == POSE_LOCK) {
                out.push_back(countdown_ < LOCK_COUNT ? Action{ Action::COUNTDOWN, LOCK_COUNT - countdown_ } : Action{ Action::LOCK });
                ++countdown_;
                nextStep_ = t + COUNT_STEP_MS;
            }
        }
        return out;
    }

    Pose Current() const { return pose_; }

private:
    // A new gesture restarts the repeats and the countdown; leaving the lock pose mid-countdown cancels it
    void SetPose(Pose p, DWORD since, std::vector<Action>& out) {
        if (p == pose_) return;
        if (countdown_ > 0 && countdown_ <= LOCK_COUNT) out.push_back({ Action::CANCEL });
        pose_ = p;
        poseSince_ = since;
        nextStep_ = since; // already reached: the first step fires now
        countdown_ = 0;
        fired_ = false;
    }

    Pose pose_ = POSE_NONE, cand_ = POSE_NONE; // current (debounced) gesture; the raw gesture waiting to count
    DWORD candSince_ = 0, poseSince_ = 0, lastHand_ = 0, nextStep_ = 0;
    bool fired_ = false;
    int countdown_ = 0; // countdown messages sent for this lock pose; LOCK_COUNT + 1 once lock is sent
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

// Media keys go through keybd_event as src/media.h does
inline void PressKey(BYTE vk) {
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY, 0);
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
}

inline void RunAction(const Action& a, HWND notch) {
    switch (a.kind) {
    case Action::NEXT: PressKey(VK_MEDIA_NEXT_TRACK); break;
    case Action::PREV: PressKey(VK_MEDIA_PREV_TRACK); break;
    case Action::VOLUP: for (int i = 0; i < a.n; ++i) PressKey(VK_VOLUME_UP); break;
    case Action::VOLDOWN: for (int i = 0; i < a.n; ++i) PressKey(VK_VOLUME_DOWN); break;
    case Action::COUNTDOWN: WhistleSend(notch, WHISTLE_ALERT, "Bloqueando el PC en " + std::to_string(a.n) + "\xE2\x80\xA6 quita la palma para cancelar", CACTUS_CAMERA); break;
    case Action::CANCEL: WhistleSend(notch, WHISTLE_TEXT, "Bloqueo cancelado", CACTUS_CAMERA); break;
    case Action::LOCK: LockWorkStation(); break;
    }
}

// The notch's card line for what the hand is doing; "" for no gesture
inline const char* StatusLabel(bool hand, Pose p) {
    if (!hand) return "Sin mano a la vista";
    switch (p) {
    case POSE_VOLUP: return "Subiendo volumen";
    case POSE_VOLDOWN: return "Bajando volumen";
    case POSE_NEXT: return "Siguiente canci\xC3\xB3n";
    case POSE_PREV: return "Canci\xC3\xB3n anterior";
    case POSE_LOCK: return "Palma: mant\xC3\xA9nla para bloquear el PC";
    default: return "";
    }
}

// The collapsed pill's bump for a command that just ran, UTF-8 "emoji\tlabel"; nullptr when the action needs none
// (the lock countdown/cancel/lock, which the notch already shows)
inline const char* GestureFeedback(const Action& a) {
    switch (a.kind) {
    case Action::NEXT: return "\xE2\x8F\xAD\tSiguiente";
    case Action::PREV: return "\xE2\x8F\xAE\tAnterior";
    case Action::VOLUP: return "\xF0\x9F\x94\x8A\tSubir volumen";
    case Action::VOLDOWN: return "\xF0\x9F\x94\x89\tBajar volumen";
    default: return nullptr;
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
    const char* lastFb = NULL; // the bump last sent, so scroll and volume repeats are throttled
    DWORD lastFbAt = 0;
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
                const DWORD now = GetTickCount();
                const Pose pose = hand ? ClassifyGesture(lm) : POSE_NONE;
                for (const Action& a : gestures.Step(hand, pose, now)) {
                    RunAction(a, notch);
                    const char* fb = GestureFeedback(a);
                    if (fb && (fb != lastFb || TimeReached(now, lastFbAt + 250))) { lastFb = fb; lastFbAt = now; WhistleSend(notch, WHISTLE_GESTURE, fb, CACTUS_CAMERA); }
                    if (a.kind == Action::CANCEL) { cancelUntil = now + CANCEL_SHOW_MS; shown = NULL; } // the status line returns after the note
                }
                const char* label = StatusLabel(hand, gestures.Current());
                if (label != shown && TimeReached(now, cancelUntil)) { shown = label; WhistleSend(notch, WHISTLE_TEXT, label, CACTUS_CAMERA); }
            }
            if (locked && has2d) b2->Unlock2D();
            else if (locked) buf->Unlock();
            if (b2) b2->Release();
            buf->Release();
        }
        sample->Release();
    }
    return 0;
}
