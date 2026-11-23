// g++ -std=c++17 -Wall tests/camera_test.cpp -o %TEMP%\camera_test.exe -static -lwinhttp -lwinmm -lbcrypt -lshell32 -lole32 -lgdi32 -lmfplat -lmfreadwrite -lmf -lmfuuid && %TEMP%\camera_test.exe
#define _WIN32_WINNT 0x0A00 // same prelude as main.cpp: BCryptHash needs Windows 10 headers
#include <windows.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "../src/camera.h"

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

int main() {
    // PalmAnchors: 24x24x2 then 12x12x6
    const std::vector<Pt> anchors = PalmAnchors();
    assert(anchors.size() == 2016);
    assert(Near(anchors[0].x, 0.5f / 24) && Near(anchors[0].y, 0.5f / 24));
    assert(Near(anchors[1].x, 0.5f / 24) && Near(anchors[1].y, 0.5f / 24));
    assert(Near(anchors[2].x, 1.5f / 24) && Near(anchors[2].y, 0.5f / 24));
    for (int k = 1152; k < 1158; ++k) assert(Near(anchors[k].x, 0.5f / 12) && Near(anchors[k].y, 0.5f / 12));
    assert(Near(anchors[1158].x, 1.5f / 12) && Near(anchors[1158].y, 0.5f / 12));

    // RoiToFrame: the crop centre is the roi centre; angle pi/2 turns the crop's top towards +x
    const Roi r0 = { 100, 100, 224, 0 };
    Pt p = RoiToFrame(r0, 112, 112);
    assert(Near(p.x, 100) && Near(p.y, 100));
    p = RoiToFrame(r0, 0, 0);
    assert(Near(p.x, -12) && Near(p.y, -12));
    const Roi rq = { 100, 100, 224, PI_F / 2 };
    p = RoiToFrame(rq, 112, 0);
    assert(Near(p.x, 212) && Near(p.y, 100));

    // CropRoi on a 64x48 BGRA frame with a red 16x16 square at (24..40, 16..32)
    std::vector<unsigned char> px(64 * 48 * 4, 0);
    for (int y = 16; y < 32; ++y)
        for (int x = 24; x < 40; ++x) { unsigned char* q = &px[(y * 64 + x) * 4]; q[0] = 0; q[1] = 0; q[2] = 255; q[3] = 255; }
    const Frame fr = { px.data(), 64, 48, 64 * 4 };
    const Roi sq = { 32, 24, 48, 0 };
    std::vector<float> crop(HAND_SIZE * HAND_SIZE * 3);
    CropRoi(fr, sq, crop.data());
    assert(crop[(112 * HAND_SIZE + 112) * 3 + 0] > 0.99f);      // centre: red, 0..1
    assert(crop[(112 * HAND_SIZE + 112) * 3 + 1] < 0.01f);      // no green
    assert(crop[0] < 0.01f);                                     // corner: outside the square

    // DecodePalm: 640x480 frame (scale 0.3, 24 px padding top and bottom). Anchor 5 wins with logit 5.
    // Its box lands on the frame centre (320,240), 320 px square, keypoint 0 at (320,400), keypoint 2 at (320,240): upright.
    std::vector<float> reg(2016 * 18, 0.f), logit(2016, -10.f);
    const Pt a5 = anchors[5];
    float* r = &reg[5 * 18];
    r[0] = (0.5f - a5.x) * 192; r[1] = (0.5f - a5.y) * 192;      // centre
    r[2] = 96; r[3] = 96;                                        // 0.5 of the model square
    r[4] = (0.5f - a5.x) * 192; r[5] = (0.75f - a5.y) * 192;     // keypoint 0 (wrist)
    r[8] = (0.5f - a5.x) * 192; r[9] = (0.5f - a5.y) * 192;      // keypoint 2 (above the wrist)
    logit[5] = 5;
    Roi roi = {};
    assert(DecodePalm(reg.data(), logit.data(), anchors, 640, 480, roi));
    assert(Near(roi.cx, 320) && Near(roi.cy, 80) && Near(roi.side, 832) && Near(roi.angle, 0));
    std::fill(logit.begin(), logit.end(), -10.f);
    assert(!DecodePalm(reg.data(), logit.data(), anchors, 640, 480, roi));

    // RoiFromLandmarks: an upright hand, wrist at y=300, fingertips at y=150, all on x=100
    Pt up[21];
    for (Pt& q : up) q = { 100, 225 };
    up[0] = { 100, 300 };
    up[9] = { 100, 200 };
    const Roi ur = RoiFromLandmarks(up);
    assert(Near(ur.angle, 0));
    assert(ur.cy < 300 && Near(ur.cx, 100));

    {   // GestureFeatures: the same hand moved and scaled reads the same; turned upside down it does not
        Pt a[21], b[21];
        for (int i = 0; i < 21; ++i) { a[i] = { 100.f + 7 * i, 200.f - 11 * i + (i % 3) * 5 }; b[i] = { 40 + 2 * a[i].x, 10 + 2 * a[i].y }; }
        float fa[GESTURE_DIM], fb[GESTURE_DIM];
        GestureFeatures(a, fa);
        GestureFeatures(b, fb);
        for (int i = 0; i < GESTURE_DIM; ++i) assert(Near(fa[i], fb[i]));
        for (Pt& q : b) q.y = -q.y;
        GestureFeatures(b, fb);
        assert(!Near(fa[1], fb[1]));
    }
    {   // ClassifyGesture: each recorded frame, rebuilt as points, reads back as its own gesture
        for (const GestureTemplate& g : GESTURE_TEMPLATES) {
            Pt lm[21];
            lm[0] = { 300, 200 };
            for (int i = 1; i < 21; ++i) lm[i] = { 300 + 150 * g.f[(i - 1) * 2], 200 + 150 * g.f[(i - 1) * 2 + 1] };
            assert(ClassifyGesture(lm) == g.pose);
        }
        int per[6] = {};
        for (const GestureTemplate& g : GESTURE_TEMPLATES) ++per[g.pose];
        for (int p : per) assert(p == 40); // the five gestures and the no-gesture hand
    }
    {   // the pill's bump: only commands the user must see; each is "emoji<\t>label"
        auto fb = [](Action::Kind k) { Action a; a.kind = k; return GestureFeedback(a); };
        for (Action::Kind k : { Action::COUNTDOWN, Action::CANCEL, Action::LOCK }) assert(!fb(k));
        for (Action::Kind k : { Action::NEXT, Action::PREV, Action::VOLUP, Action::VOLDOWN }) assert(fb(k) && strchr(fb(k), '\t'));
    }

    // GestureMachine, with injected times (ms), 33 ms frames
    auto run = [](GestureMachine& g, Pose p, DWORD from, DWORD to, std::vector<Action>* all, bool hand = true) {
        for (DWORD t = from; t <= to; t += 33)
            for (const Action& a : g.Step(hand, p, t)) all->push_back(a);
    };
    auto count = [](const std::vector<Action>& v, Action::Kind k) { int n = 0; for (const Action& a : v) n += a.kind == k; return n; };
    {   // volume up held 1 s: nothing for the first 300 ms, then a step at once and one every 300 ms
        GestureMachine g;
        std::vector<Action> v;
        run(g, POSE_VOLUP, 1000, 1290, &v);
        assert(v.empty());
        run(g, POSE_VOLUP, 1323, 2000, &v);
        assert(count(v, Action::VOLUP) == 3 && v.size() == 3 && v[0].n == VOLUME_KEYS);
    }
    {   // a gesture shorter than POSE_STABLE_MS does nothing
        GestureMachine g;
        std::vector<Action> v;
        run(g, POSE_NONE, 1000, 2000, &v);
        run(g, POSE_NEXT, 2033, 2250, &v);
        run(g, POSE_NONE, 2283, 3000, &v);
        assert(v.empty());
    }
    {   // next held 2 s: one next track; prev after it: one previous track
        GestureMachine g;
        std::vector<Action> v;
        run(g, POSE_NEXT, 1000, 3000, &v);
        run(g, POSE_PREV, 3033, 5000, &v);
        assert(count(v, Action::NEXT) == 1 && count(v, Action::PREV) == 1 && v.size() == 2);
    }
    {   // the lock pose from 1000 to 6000: countdown 3, 2, 1 and one lock
        GestureMachine g;
        std::vector<Action> v;
        run(g, POSE_LOCK, 1000, 6000, &v);
        std::vector<int> counts;
        for (const Action& a : v) if (a.kind == Action::COUNTDOWN) counts.push_back(a.n);
        assert((counts == std::vector<int>{ 3, 2, 1 }) && count(v, Action::LOCK) == 1);
    }
    {   // the lock pose left mid-countdown: cancelled, no lock
        GestureMachine g;
        std::vector<Action> v;
        run(g, POSE_LOCK, 1000, 3600, &v);
        run(g, POSE_NONE, 3633, 7000, &v);
        assert(count(v, Action::COUNTDOWN) == 2 && count(v, Action::CANCEL) == 1 && count(v, Action::LOCK) == 0);
    }
    {   // the lock pose with one 33 ms frame of no gesture: the countdown goes on and locks
        GestureMachine g;
        std::vector<Action> v;
        run(g, POSE_LOCK, 1000, 2500, &v);
        run(g, POSE_NONE, 2533, 2533, &v);
        run(g, POSE_LOCK, 2566, 6000, &v);
        assert(count(v, Action::CANCEL) == 0 && count(v, Action::LOCK) == 1);
    }
    {   // the hand lost past the grace: the volume stops at once; a short blink does not stop it
        GestureMachine g;
        std::vector<Action> v;
        run(g, POSE_VOLDOWN, 1000, 1400, &v);
        const size_t before = v.size();
        run(g, POSE_NONE, 1433, 1600, &v, false); // a blink: under LOST_GRACE_MS, nothing changes
        assert(g.Current() == POSE_VOLDOWN);
        run(g, POSE_NONE, 1633, 3000, &v, false);
        assert(g.Current() == POSE_NONE && count(v, Action::VOLDOWN) == (int)before);
    }
    {   // tick counts past 2^31 (24.8 days up): the first step still fires
        GestureMachine g;
        std::vector<Action> v;
        run(g, POSE_NEXT, 0x90000000u, 0x90000000u + 400, &v);
        assert(count(v, Action::NEXT) == 1);
    }
    // RTMPose: the detector's input from a synthetic 64x48 frame (B=50 G=100 R=200): the canvas is letterboxed at 5x,
    // inside it the frame's values come BGR, and past the frame's 240 rows it is the grey 114
    {
        std::vector<unsigned char> bgra(64 * 48 * 4);
        for (size_t i = 0; i < bgra.size(); i += 4) { bgra[i] = 50; bgra[i + 1] = 100; bgra[i + 2] = 200; bgra[i + 3] = 255; }
        const Frame rf = { bgra.data(), 64, 48, 64 * 4 };
        std::vector<float> din(3 * 320 * 320);
        assert(Near(RtmDetInput(rf, din.data()), 5.f));
        const float mean[3] = { 103.53f, 116.28f, 123.675f }, sd[3] = { 57.375f, 57.12f, 58.395f };
        assert(Near(din[0 * 320 * 320 + 10 * 320 + 10], (50 - mean[0]) / sd[0], 1e-3f));   // channel 0 is B
        assert(Near(din[2 * 320 * 320 + 10 * 320 + 10], (200 - mean[2]) / sd[2], 1e-3f)); // channel 2 is R
        assert(Near(din[0 * 320 * 320 + 300 * 320 + 10], (114 - mean[0]) / sd[0], 1e-3f)); // below the frame: grey
    }
    {   // the best detection above RTM_DET_MIN, back in frame pixels; none when all score below
        const float dets[10] = { 0, 0, 10, 10, 0.2f, 0, 0, 20, 40, 0.9f };
        float box[4] = {};
        assert(RtmPickBox(dets, 2, 2.f, box));
        assert(Near(box[2], 10) && Near(box[3], 20));
        const float low[5] = { 0, 0, 10, 10, 0.2f };
        assert(!RtmPickBox(low, 1, 2.f, box));
    }
    {   // the pose crop: a side 1.25 times the longer edge; the centre bin (256 of 512) is the box centre
        const float box[4] = { 100, 50, 200, 90 };
        float cx, cy, s;
        RtmCrop(box, cx, cy, s);
        assert(Near(cx, 150) && Near(cy, 70) && Near(s, 125));
        const Pt c = RtmKeypoint(256, 256, cx, cy, s);
        assert(Near(c.x, cx) && Near(c.y, cy));
    }
    {   // SimCC decode: peaks at the centre bin give centre points and a confidence of 1
        std::vector<float> sx(21 * 512, 0.f), sy(21 * 512, 0.f);
        for (int k = 0; k < 21; ++k) { sx[k * 512 + 256] = 1.f; sy[k * 512 + 256] = 1.f; }
        Pt kp[21];
        assert(Near(RtmDecode(sx.data(), sy.data(), 150, 70, 125, kp), 1.f));
        assert(Near(kp[0].x, 150) && Near(kp[20].y, 70));
    }
    {   // the next box: square on the points' centre, 1.1 times their longer extent
        Pt kp[21];
        for (int i = 0; i < 21; ++i) kp[i] = { 10.f + i, 2.f * i };
        float box[4];
        RtmBox(kp, box);
        assert(Near(box[0], -2) && Near(box[1], -2) && Near(box[2], 42) && Near(box[3], 42));
    }
    puts("camera_test ok");
    return 0;
}
