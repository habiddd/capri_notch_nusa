// g++ -std=c++17 -Wall tests/camera_test.cpp -o %TEMP%\camera_test.exe -static -lwinhttp -lwinmm -lbcrypt -lshell32 -lole32 -lgdi32 -lmfplat -lmfreadwrite -lmf -lmfuuid && %TEMP%\camera_test.exe
#define _WIN32_WINNT 0x0A00 // same prelude as main.cpp: BCryptHash needs Windows 10 headers
#include <windows.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../src/camera.h"

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// Turns joint b so that the angle at b (a, b, c) has cosine s: c = b + len units at that angle from the first segment
static void Bend(Pt lm[21], int a, int b, int c, float s, float len = 40) {
    const float ux = lm[b].x - lm[a].x, uy = lm[b].y - lm[a].y, n = std::sqrt(ux * ux + uy * uy);
    const float sn = std::sqrt(1 - s * s);
    lm[c] = { lm[b].x + len * (ux * s - uy * sn) / n, lm[b].y + len * (ux * sn + uy * s) / n };
}

// A finger from the palm: the first segment points up, the second bends to straightness s (1 straight, -1 folded back)
static void Finger(Pt lm[21], int mcp, float bx, float s) {
    lm[mcp] = { bx, -60 };
    lm[mcp + 1] = { bx, -100 };
    Bend(lm, mcp, mcp + 1, mcp + 3, s);
    lm[mcp + 2] = { (lm[mcp + 1].x + lm[mcp + 3].x) / 2, (lm[mcp + 1].y + lm[mcp + 3].y) / 2 };
}

// The thumb out to the side: both joints bent to straightness s
static void Thumb(Pt lm[21], float s) {
    lm[1] = { -30, -10 };
    lm[2] = { -55, -30 };
    Bend(lm, 1, 2, 3, s);
    Bend(lm, 2, 3, 4, s);
}

// Synthetic hand, wrist at the origin, fingers up (y grows downwards)
static void MakeHand(Pt lm[21], float i, float m, float r, float k, float th) {
    lm[0] = { 0, 0 };
    Finger(lm, 5, -25, i);
    Finger(lm, 9, 0, m);
    Finger(lm, 13, 22, r);
    Finger(lm, 17, 40, k);
    Thumb(lm, th);
}

static Pose Classify(float i, float m, float r, float k, float th = 0.8f) {
    Pt lm[21];
    MakeHand(lm, i, m, r, k, th);
    return ClassifyPose(lm);
}

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
    assert(crop[(112 * HAND_SIZE + 112) * 3 + 0] > 254.f);      // centre: red, 0..255 (not normalised)
    assert(crop[(112 * HAND_SIZE + 112) * 3 + 1] < 1.f);        // no green
    assert(crop[0] < 1.f);                                       // corner: outside the square

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

    // Straight: the synthetic joints read back the cosine they were built with
    {
        Pt lm[21];
        MakeHand(lm, 0.9f, -0.8f, -0.8f, 0.9f, 0.8f);
        assert(Near(Straight(lm, 5, 6, 8), 0.9f) && Near(Straight(lm, 9, 10, 12), -0.8f));
        assert(Near(Straight(lm, 1, 2, 3), 0.8f) && Near(Straight(lm, 2, 3, 4), 0.8f));
    }

    // ClassifyPose: the straightness rules, and the index reach
    assert(Classify(0.9f, 0.9f, 0.9f, 0.9f) == POSE_PALM);
    assert(Classify(0.9f, 0.9f, -0.8f, -0.8f) == POSE_V);
    assert(Classify(-0.8f, -0.8f, -0.8f, 0.9f) == POSE_ROCK);   // index folded
    assert(Classify(0.9f, -0.8f, -0.8f, 0.9f) == POSE_ROCK);    // index up
    assert(Classify(0.9f, -0.8f, -0.8f, -0.8f) == POSE_POINT);  // a real point: straight and reaching out
    assert(Classify(-0.8f, -0.8f, -0.8f, -0.8f, 0.8f) == POSE_THUMB);
    assert(Classify(-0.8f, -0.8f, -0.8f, -0.8f, 0.1f) == POSE_FIST); // the thumb wraps the fist
    assert(Classify(-0.8f, -0.45f, -0.45f, -0.8f) == POSE_OTHER);    // the resting hand at the mouth
    {   // a thumbs-up: the index semi-straight (0.6) but curled in, its tip about as far out as its knuckle
        Pt lm[21];
        MakeHand(lm, 0.6f, -0.8f, -0.8f, -0.8f, 0.8f);
        Bend(lm, 5, 6, 8, 0.6f, 10);
        assert(ClassifyPose(lm) == POSE_THUMB);
    }

    // GestureMachine, with injected times (ms). Each Step: hand present, the raw pose, the pinch, the palm, the hand's y, the time.
    const Pt mid = { 0.5f, 0.5f };
    auto count = [](const std::vector<Action>& v, Action::Kind k) { int n = 0; for (const Action& a : v) n += a.kind == k; return n; };
    {   // ROCK for 1100 ms: one play/pause
        GestureMachine g;
        int playPause = 0;
        for (DWORD t = 1000; t <= 2100; t += 50) playPause += count(g.Step(true, POSE_ROCK, 1, mid, 0.5f, t), Action::PLAYPAUSE);
        assert(playPause == 1);
    }
    {   // the open palm held 2 s: no action (neutral hand)
        GestureMachine g;
        int actions = 0;
        for (DWORD t = 1000; t <= 3000; t += 50) actions += (int)g.Step(true, POSE_PALM, 1, mid, 0.5f, t).size();
        assert(actions == 0);
    }
    {   // FIST from 1000 to 6000: countdown 3, 2, 1 and one lock
        GestureMachine g;
        std::vector<int> counts;
        int locks = 0;
        for (DWORD t = 1000; t <= 6000; t += 50)
            for (const Action& a : g.Step(true, POSE_FIST, 1, mid, 0.5f, t)) {
                if (a.kind == Action::COUNTDOWN) counts.push_back(a.n);
                if (a.kind == Action::LOCK) ++locks;
            }
        assert((counts == std::vector<int>{ 3, 2, 1 }) && locks == 1);
    }
    {   // FIST with one 33 ms frame of OTHER at 2518: the countdown is not cancelled and still locks
        GestureMachine g;
        int cancels = 0, locks = 0, countdowns = 0;
        for (DWORD t = 1000; t <= 6100; t += 33) {
            const Pose p = (t >= 2500 && t < 2533) ? POSE_OTHER : POSE_FIST;
            for (const Action& a : g.Step(true, p, 1, mid, 0.5f, t)) {
                cancels += a.kind == Action::CANCEL;
                locks += a.kind == Action::LOCK;
                countdowns += a.kind == Action::COUNTDOWN;
            }
        }
        assert(cancels == 0 && locks == 1 && countdowns == 3);
    }
    {   // FIST released at 4500 (3500 ms in): cancelled, no lock
        GestureMachine g;
        int cancels = 0, locks = 0;
        for (DWORD t = 1000; t <= 6000; t += 50) {
            const Pose p = t < 4500 ? POSE_FIST : POSE_POINT;
            for (const Action& a : g.Step(true, p, 1, mid, 0.5f, t)) {
                cancels += a.kind == Action::CANCEL;
                locks += a.kind == Action::LOCK;
            }
        }
        assert(cancels == 1 && locks == 0);
    }
    {   // a stall: fist held to 1900 ms, then four FIST frames from 5500 ms: only COUNTDOWN 3 shows, the rest wait a second
        GestureMachine g;
        for (DWORD t = 1000; t <= 1900; t += 100) g.Step(true, POSE_FIST, 1, mid, 0.5f, t);
        int fired = 0;
        for (DWORD t : { 5500u, 5533u, 5566u, 5600u })
            for (const Action& a : g.Step(true, POSE_FIST, 1, mid, 0.5f, t)) {
                assert(a.kind == Action::COUNTDOWN && a.n == 3);
                ++fired;
            }
        assert(fired == 1);
        std::vector<Action> v = g.Step(true, POSE_FIST, 1, mid, 0.5f, 6500);
        assert(v.size() == 1 && v[0].kind == Action::COUNTDOWN && v[0].n == 2);
        v = g.Step(true, POSE_FIST, 1, mid, 0.5f, 7500);
        assert(v.size() == 1 && v[0].kind == Action::COUNTDOWN && v[0].n == 1);
        v = g.Step(true, POSE_FIST, 1, mid, 0.5f, 8500);
        assert(v.size() == 1 && v[0].kind == Action::LOCK);
    }
    {   // the lock moment with a raw OTHER frame: no lock, and the debounced OTHER cancels the countdown
        GestureMachine g;
        int locks = 0, cancels = 0;
        for (DWORD t = 1000; t <= 6300; t += 100) {
            const Pose p = t >= 6000 ? POSE_OTHER : POSE_FIST;
            for (const Action& a : g.Step(true, p, 1, mid, 0.5f, t)) {
                locks += a.kind == Action::LOCK;
                cancels += a.kind == Action::CANCEL;
            }
        }
        assert(locks == 0 && cancels == 1);
    }
    {   // ROCK held, one 33 ms frame of OTHER at ~700 ms: still exactly one play/pause by 1100 ms
        GestureMachine g;
        int playPause = 0;
        for (DWORD t = 0; t <= 1100; t += 33) {
            const Pose p = (t >= 700 && t < 733) ? POSE_OTHER : POSE_ROCK;
            playPause += count(g.Step(true, p, 1, mid, 0.5f, t), Action::PLAYPAUSE);
        }
        assert(playPause == 1);
    }
    {   // a pinch with no POINT before it (the resting hand at the mouth): no click, no right click
        GestureMachine g;
        int clicks = 0;
        for (DWORD t = 1000; t <= 2000; t += 100) {
            for (const Action& a : g.Step(true, POSE_OTHER, 0.1f, mid, 0.5f, t))
                clicks += a.kind == Action::LDOWN || a.kind == Action::RCLICK;
        }
        assert(clicks == 0);
    }
    {   // a fist disarms the click: a pinch after it is not a press
        GestureMachine g;
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1100); // armed
        for (DWORD t = 1200; t <= 1400; t += 100) g.Step(true, POSE_FIST, 1, mid, 0.5f, t);
        int clicks = 0;
        for (DWORD t = 1500; t <= 1900; t += 100)
            for (const Action& a : g.Step(true, POSE_OTHER, 0.1f, mid, 0.5f, t)) clicks += a.kind == Action::LDOWN || a.kind == Action::RCLICK;
        assert(clicks == 0);
    }
    {   // quick pinch (200 ms): one click where the cursor was frozen, no move while pending
        GestureMachine g;
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1100); // armed
        int movesPending = 0, downs = 0, ups = 0;
        for (DWORD t = 1200; t <= 1300; t += 50) {
            const float pl = t < 1300 ? 0.1f : 0.5f;
            const Pt c = { 0.5f + 0.005f * (t - 1200) / 100.f, 0.5f }; // a wobble, under the drag distance
            for (const Action& a : g.Step(true, POSE_POINT, pl, c, 0.5f, t)) {
                if (a.kind == Action::MOVE && t < 1300) ++movesPending;
                downs += a.kind == Action::LDOWN;
                ups += a.kind == Action::LUP;
            }
        }
        assert(movesPending == 0 && downs == 1 && ups == 1);
    }
    {   // pinch held still for 700 ms: one right click, no left button
        GestureMachine g;
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1100); // armed
        int rclicks = 0, downs = 0;
        for (DWORD t = 1200; t <= 1900; t += 50)
            for (const Action& a : g.Step(true, POSE_POINT, 0.1f, mid, 0.5f, t)) {
                rclicks += a.kind == Action::RCLICK;
                downs += a.kind == Action::LDOWN;
            }
        for (const Action& a : g.Step(true, POSE_POINT, 0.5f, mid, 0.5f, 1950))
            downs += a.kind == Action::LDOWN || a.kind == Action::LUP;
        assert(rclicks == 1 && downs == 0);
    }
    {   // pinch, then the hand moves 0.02: a drag, cursor moves, released on open
        GestureMachine g;
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1100); // armed
        std::vector<Action::Kind> seq;
        int moves = 0;
        for (DWORD t = 1200; t <= 1500; t += 50) {
            const float pl = t < 1450 ? 0.1f : 0.5f;
            const float x = t <= 1250 ? 0.5f : 0.52f + 0.03f * (t - 1250) / 250.f; // moves 0.02 at 1250, then drags on
            for (const Action& a : g.Step(true, POSE_POINT, pl, { x, 0.5f }, 0.5f, t)) {
                seq.push_back(a.kind);
                moves += a.kind == Action::MOVE;
            }
        }
        const auto down = std::find(seq.begin(), seq.end(), Action::LDOWN);
        const auto up = std::find(seq.begin(), seq.end(), Action::LUP);
        assert(down != seq.end() && up != seq.end() && down < up);
        assert(moves > 0);
        assert(std::count(seq.begin(), seq.end(), Action::RCLICK) == 0);
    }
    {   // pointing with the pinch open for 2 s: no clicks at all
        GestureMachine g;
        int clicks = 0;
        for (DWORD t = 1000; t <= 3000; t += 50)
            for (const Action& a : g.Step(true, POSE_POINT, 1, mid, 0.5f, t))
                clicks += a.kind == Action::LDOWN || a.kind == Action::LUP || a.kind == Action::RCLICK;
        assert(clicks == 0);
    }
    {   // the palm centre still: no movement, even with noise under the dead zone
        GestureMachine g;
        int moves = 0;
        for (DWORD t = 1000; t <= 3000; t += 33) {
            const Pt c = { 0.5f + ((t / 33) % 2 ? 0.0005f : -0.0005f), 0.5f };
            for (const Action& a : g.Step(true, POSE_POINT, 1, c, 0.5f, t)) moves += a.kind == Action::MOVE;
        }
        assert(moves == 0);
    }
    {   // a constant palm: no movement
        GestureMachine g;
        int moves = 0;
        for (DWORD t = 1000; t <= 2000; t += 33)
            for (const Action& a : g.Step(true, POSE_POINT, 1, mid, 0.5f, t)) moves += a.kind == Action::MOVE;
        assert(moves == 0);
    }
    {   // a 0.1 sweep over 300 ms: the cursor travels between 150 and 400 pixels
        GestureMachine g;
        float total = 0;
        for (DWORD t = 1000; t <= 1300; t += 33) {
            const Pt c = { 0.5f + 0.1f * (t - 1000) / 300.f, 0.5f };
            for (const Action& a : g.Step(true, POSE_POINT, 1, c, 0.5f, t)) total += a.x;
        }
        assert(total > 150 && total < 400);
        // acceleration: 0.1 moved in 2000 ms (slow) vs in 100 ms (fast); the final point is held 600 ms so the filter settles
        auto travel = [&](DWORD span) {
            GestureMachine g;
            float sum = 0;
            for (DWORD t = 1000; t <= 1000 + span + 600; t += 33) {
                const Pt c = { 0.5f + 0.1f * std::min(1.f, (float)(t - 1000) / span), 0.5f };
                for (const Action& a : g.Step(true, POSE_POINT, 1, c, 0.5f, t)) sum += a.x;
            }
            return sum;
        };
        const float slow = travel(2000), fast = travel(100);
        assert(fast > 2 * slow && slow > 0);
    }
    {   // leaving POINT, then returning far away: no jump
        GestureMachine g;
        for (DWORD t = 1000; t <= 1500; t += 33) g.Step(true, POSE_POINT, 1, mid, 0.5f, t);
        for (DWORD t = 1533; t <= 2000; t += 33) g.Step(true, POSE_OTHER, 1, mid, 0.5f, t);
        int jump = 0;
        for (DWORD t = 2033; t <= 2500; t += 33)
            for (const Action& a : g.Step(true, POSE_POINT, 1, { 0.8f, 0.3f }, 0.3f, t)) jump += a.kind == Action::MOVE;
        assert(jump == 0);
    }
    {   // THUMB held with slow drift (0.14 over 4 s, and 0.12 within 400 ms): no swipe, no action
        GestureMachine g;
        int acts = 0;
        for (DWORD t = 1000; t <= 5000; t += 50) acts += (int)g.Step(true, POSE_THUMB, 1, mid, 0.5f - 0.14f * (t - 1000) / 4000.f, t).size();
        assert(acts == 0);
        GestureMachine h;
        acts = 0;
        for (DWORD t = 1000; t <= 1400; t += 50) acts += (int)h.Step(true, POSE_THUMB, 1, mid, 0.5f - 0.12f * (t - 1000) / 400.f, t).size();
        assert(acts == 0);
    }
    {   // THUMB: a 0.25 swipe up within 200 ms is one volume up of VOLUME_KEYS presses
        GestureMachine g;
        std::vector<Action> all;
        for (DWORD t = 1000; t <= 1400; t += 25) {
            const float y = t < 1100 ? 0.5f : std::max(0.25f, 0.5f - 0.25f * (t - 1100) / 200.f);
            for (const Action& a : g.Step(true, POSE_THUMB, 1, mid, y, t)) all.push_back(a);
        }
        assert(all.size() == 1 && all[0].kind == Action::VOLUP && all[0].n == VOLUME_KEYS);
    }
    {   // THUMB: a 0.25 swipe to the user's right within 200 ms is one next track
        GestureMachine g;
        std::vector<Action> all;
        for (DWORD t = 1000; t <= 1400; t += 25) {
            const float x = t < 1100 ? 0.5f : std::min(0.75f, 0.5f + 0.25f * (t - 1100) / 200.f);
            for (const Action& a : g.Step(true, POSE_THUMB, 1, { x, 0.5f }, 0.5f, t)) all.push_back(a);
        }
        assert(all.size() == 1 && all[0].kind == Action::NEXT);
    }
    {   // THUMB: two swipes 300 ms apart: only the first counts (600 ms cooldown)
        GestureMachine g;
        int volume = 0;
        for (DWORD t = 1000; t <= 1700; t += 25) {
            float y = 0.5f;
            if (t >= 1100 && t <= 1300) y = 0.5f - 0.25f * (t - 1100) / 200.f;          // swipe up
            else if (t > 1300 && t < 1400) y = 0.25f;
            else if (t >= 1400 && t <= 1600) y = 0.25f + 0.25f * (t - 1400) / 200.f;    // swipe down
            else if (t > 1600) y = 0.5f;
            for (const Action& a : g.Step(true, POSE_THUMB, 1, mid, y, t))
                volume += a.kind == Action::VOLUP || a.kind == Action::VOLDOWN;
        }
        assert(volume == 1);
    }
    {   // THUMB: small mixed jitter, under both steps, does nothing
        GestureMachine g;
        g.Step(true, POSE_THUMB, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_THUMB, 1, mid, 0.5f, 1100);
        assert(g.Step(true, POSE_THUMB, 1, { 0.52f, 0.47f }, 0.47f, 1200).empty());
    }
    {   // V (current after the debounce): the hand moving up 0.05 scrolls one notch up
        GestureMachine g;
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_V, 1, mid, 0.5f, 1100);
        g.Step(true, POSE_V, 1, mid, 0.5f, 1200);
        const std::vector<Action> v = g.Step(true, POSE_V, 1, mid, 0.45f, 1300);
        assert(count(v, Action::SCROLL) == 1 && v[0].n == 1);
    }
    {   // a drag, then the hand lost past the grace: the button is released
        GestureMachine g;
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1100); // armed
        g.Step(true, POSE_POINT, 0.1f, mid, 0.5f, 1200);
        assert(count(g.Step(true, POSE_POINT, 0.1f, { 0.53f, 0.5f }, 0.5f, 1250), Action::LDOWN) == 1);
        assert(g.Step(false, POSE_NONE, 1, mid, 0.5f, 1300).empty());
        assert(count(g.Step(false, POSE_NONE, 1, mid, 0.5f, 1600), Action::LUP) == 1);
    }

    {   // a drag with three single raw THUMB frames in it: no release until the pinch opens
        GestureMachine g;
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1100); // armed
        g.Step(true, POSE_POINT, 0.1f, mid, 0.5f, 1200); // pending
        int downs = 0, earlyUps = 0, ups = 0;
        for (DWORD t = 1250; t <= 1700; t += 50) {
            const Pose p = (t == 1350 || t == 1450 || t == 1550) ? POSE_THUMB : POSE_POINT;
            const float pl = t < 1650 ? 0.1f : 0.5f;
            const float x = 0.5f + 0.05f * std::min(1.f, (t - 1200) / 200.f);
            for (const Action& a : g.Step(true, p, pl, { x, 0.5f }, 0.5f, t)) {
                downs += a.kind == Action::LDOWN;
                if (a.kind == Action::LUP) { ++ups; earlyUps += t < 1650; }
            }
        }
        assert(downs == 1 && ups == 1 && earlyUps == 0);
    }
    {   // a pinch 800 ms after the last raw POINT (the hand went on to other poses): no click, no right click
        GestureMachine g;
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1000);
        g.Step(true, POSE_POINT, 1, mid, 0.5f, 1100); // armed; the last raw POINT
        int clicks = 0;
        for (DWORD t = 1200; t <= 2700; t += 50) {
            const float pl = (t >= 1900 && t < 2650) ? 0.1f : 1.f;
            for (const Action& a : g.Step(true, POSE_OTHER, pl, mid, 0.5f, t))
                clicks += a.kind == Action::LDOWN || a.kind == Action::LUP || a.kind == Action::RCLICK;
        }
        assert(clicks == 0);
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
    {   // RTMPose's finger rules on synthetic hands
        Pt lm[21];
        MakeHand(lm, 0.9f, 0.9f, 0.9f, 0.9f, 0.8f);
        assert(ClassifyPoseRtm(lm) == POSE_PALM);
        MakeHand(lm, 0.9f, 0.9f, -0.8f, -0.8f, 0.8f);
        assert(ClassifyPoseRtm(lm) == POSE_V);
        MakeHand(lm, -0.8f, -0.8f, -0.8f, 0.9f, 0.8f);
        assert(ClassifyPoseRtm(lm) == POSE_ROCK);
        MakeHand(lm, -0.8f, -0.8f, -0.8f, -0.8f, 0.8f);
        assert(ClassifyPoseRtm(lm) == POSE_THUMB);
        MakeHand(lm, -0.8f, -0.8f, -0.8f, -0.8f, 0.1f);
        assert(ClassifyPoseRtm(lm) == POSE_FIST); // the thumb wraps: its tip is near the ring knuckle
        // a point with ring and pinky curled toward the camera: short (under 1.05 of the palm) though straight
        MakeHand(lm, 0.9f, -0.8f, -0.8f, -0.8f, 0.8f);
        lm[13] = { 22, -60 }; lm[14] = { 22, -40 };
        Bend(lm, 13, 14, 16, 0.9f, 20);
        lm[17] = { 40, -50 }; lm[18] = { 40, -35 };
        Bend(lm, 17, 18, 20, 0.9f, 20);
        assert(ClassifyPoseRtm(lm) == POSE_POINT);
    }

    // OneEuro: a constant input stays constant
    OneEuro f;
    for (int i = 0; i < 100; ++i) assert(Near(f.Filter(0.5f, i / 30.0), 0.5f, 1e-6f));

    puts("camera_test ok");
    return 0;
}
