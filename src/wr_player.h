// wr_player.h  --  the player's own origin and velocity, read rather than
//                  inferred.
//
// WHY THIS EXISTS
//
// Everything live in this tool is derived from one thing: the world-to-clip
// matrix wr_scan.cpp finds, out of which wr_engine.cpp solves a camera origin.
// Position is that origin; velocity is that origin differenced over a 40 ms
// window; the player's feet are that origin less a SETTING called eyeHeight.
//
// That chain was good enough for every readout built on it until somebody asked
// for board numbers a competitive surfer could trust, and then all three links
// turned out to be carrying error that no amount of arithmetic downstream can
// remove:
//
//   THE EYE HEIGHT IS A GUESS, AND IT IS 36 UNITS WRONG WHILE CROUCHED.
//     Source's standing view offset is 64 and its ducked one is 28. The tool
//     subtracts 64 unconditionally -- wr_bspload.h says so in as many words --
//     so a ducked player's feet are taken to be 36 units below where they are.
//     The nearest-surface query has a radius of 24. A crouched surfer is
//     therefore asking about a point outside its own search radius, and surfers
//     crouch.
//
//   THE DUCK ITSELF IS INJECTED INTO THE VELOCITY.
//     The view offset lerps between 64 and 28 over a fraction of a second, and
//     the estimator cannot tell that from the player moving. Nothing about the
//     trajectory changed; the number did.
//
//   AND VIEW BOB RIDES ON TOP OF ALL OF IT.
//     wr_energy.h picked its 40 ms window precisely so that a two-unit bob
//     reads as 50 u/s rather than 400. That is a defence, not a fix.
//
// The game holds all three quantities exactly, as floats, in its own writable
// memory, updated every tick. Reading them removes every one of the errors
// above at the source rather than filtering it afterwards.
//
// WHAT THIS DOES AND DOES NOT DO
//
// It is the same technique wr_scan.cpp already uses and it is held to the same
// rules, which are worth restating because they are the whole reason this is
// acceptable:
//
//   * READ ONLY. ReadProcessMemory on our own process. Never a write, never a
//     call, never a transfer of control into game code. A page freed underneath
//     the scan returns false rather than raising.
//   * NOTHING IS TAKEN ON FAITH. A candidate is accepted only after it has
//     predicted something independently known, over and over, for long enough
//     that coincidence is not an explanation.
//   * IT MAY FAIL, AND FAILING IS FINE. Every caller keeps working on the
//     camera estimate. This makes the numbers better where it succeeds; it is
//     not load-bearing anywhere.
//
// THE ORACLE, AND WHY IT IS STRONG
//
// The camera and the player origin are THE SAME POINT IN X AND Y. Source's view
// offset is purely vertical. So the question is not "does this look like a
// position" -- billions of float triples do -- it is "do these two floats track
// the two the camera solve produces, this frame and the next and the next,
// while the third stays a plausible constant below it".
//
// A surfer moves 30+ units a frame. Two coordinates agreeing to within a unit,
// frame after frame, through turns and ramps and teleports, is not something
// unrelated memory does. The third float then has to sit 8 to 80 units below
// the camera and stay there. Nothing has to be assumed about the layout of any
// struct, and no offset from any version of any SDK is used anywhere.
//
// AND IT MEASURES THE EYE HEIGHT AS A SIDE EFFECT, which is the point. Once the
// origin is known, cam.z - origin.z IS the view offset, live, whatever the game
// happens to use and whether or not the player is crouched. The setting stops
// being a guess and becomes a fallback.
//
// THE VELOCITY IS FOUND THE SAME WAY, FROM A BETTER SEED
//
// With an exact origin in hand, differencing it frame to frame gives a velocity
// good to a few units -- far better than the camera could, because it carries
// no bob and no duck. That is not the end goal; it is the ORACLE for finding
// the game's own velocity vector, which is exact and per-tick.
//
// Two tests, and the second is the one that cannot be faked: the triple must
// predict the origin's own motion, and while the player is in free flight its z
// must fall by exactly gravity times the elapsed time. Nothing else in memory
// is accelerating downward at precisely sv_gravity in step with the player.

#ifndef WR_PLAYER_H
#define WR_PLAYER_H

#include "wr_common.h"

// Per frame, on the render thread, AFTER the camera has been refreshed for this
// frame. Validates candidates, promotes a winner, retires one that has died.
void WrPlayerTick(const Vec3 &cam, float dt);

// Throw everything away and look again. A level change moves heap objects.
void WrPlayerOnMapChanged(void);

// Give up on whatever was found and re-scan from cold. Safe to call any time.
void WrPlayerRescan(void);

// THE ANSWERS. False when nothing has been proved, which is a normal state and
// not an error -- every caller has a camera-derived fallback and must use it.
bool WrPlayerOrigin(Vec3 *out);     // the feet, exactly
bool WrPlayerVelocity(Vec3 *out);   // the game's own velocity vector

// The live view offset, cam.z - origin.z, or a negative number when the origin
// has not been found. This is the number `phys.eyeHeight` was standing in for.
float WrPlayerEyeHeight(void);

// Diagnostics, for the panel. Never null.
const char *WrPlayerStatus(void);
bool WrPlayerBusy(void);
int  WrPlayerCandidates(void);      // origin candidates still alive
int  WrPlayerVelCandidates(void);

#endif // WR_PLAYER_H
