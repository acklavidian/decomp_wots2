#pragma once
#include "engine/containers.h"
#include "game/chara.h"
#include "hal/ps2_types.h"

struct Sword;

// The character manager (0x143030–0x1471C0): the list of characters in a
// scene, the queues that load their files one at a time, and a function per
// thing the rest of the game asks of a character by its id.
struct CharaMgr {
    s32   reloading;        // +0x00  1 while a reload (phases 5, 6) is in hand
    s32   loadStep;         // +0x04  0–7, see CharaMgr_LoadStep
    s32   pendingStep;      // +0x08  step to restart the player's load from, −1
    s32   savedStep;        // +0x0C  step to return to afterwards, −1
    s32   showNear;         // +0x10  0: characters closer than 0.5 to the camera are not drawn
    u32   _14[3];
    float light[4];         // +0x20  handed to every character's Draw
    GuestList charas;       // +0x30  Chara*
    GuestList loadQueue;    // +0x3C
    GuestList reloadQueue;  // +0x48
    GuestList drawn;        // +0x54  those drawn this frame
    float projection[16];   // +0x60  for culling
    float planes[8][4];     // +0xA0
};

extern "C" {
    CharaMgr* CharaMgr_Ctor(void* mem);                                     // 0x1471C0
    CharaMgr* CharaMgr_Dtor(CharaMgr* mgr, s16 flag);                       // 0x146F50
    void  CharaMgr_Init(CharaMgr* mgr, int forceLoad);                      // 0x146EC0
    void  CharaMgr_Add(CharaMgr* mgr, int id, int kind, int model, int arg);    // 0x146DA0
    void  CharaMgr_Remove(CharaMgr* mgr, int id);                           // 0x145DF0
    void  CharaMgr_QueueLoad(CharaMgr* mgr, int id);                        // 0x145790
    int   CharaMgr_LoadStep(CharaMgr* mgr);                                 // 0x1450D0
    int   CharaMgr_ReloadStep(CharaMgr* mgr);                               // 0x144F50
    int   CharaMgr_IsBusy(CharaMgr* mgr);                                   // 0x145400  0 while the player's files load
    void  CharaMgr_Update(CharaMgr* mgr, int load);                         // 0x145BE0
    void  CharaMgr_Draw(CharaMgr* mgr, int pass);                           // 0x145830
    Chara* CharaMgr_Find(CharaMgr* mgr, int id);                            // 0x145F90
    int   CharaMgr_IdAt(CharaMgr* mgr, int index);                          // 0x145FE0
    int   CharaMgr_IsLoaded(CharaMgr* mgr, int id);                         // 0x1441F0
    char  CharaMgr_IsReady(CharaMgr* mgr);                                  // 0x1441B0  everyone loaded
    int   CharaMgr_IsPlayerReady(CharaMgr* mgr);                            // 0x143E50  the player and its sword
    CharaParams* CharaMgr_GetParams(CharaMgr* mgr, int id);                 // 0x145F30
    void* CharaMgr_GetTransform(CharaMgr* mgr, int id);                     // 0x146D10
    void  CharaMgr_SetTransform(CharaMgr* mgr, int id, const void* matrix); // 0x146A20
    float* CharaMgr_GetPosition(CharaMgr* mgr, int id);                     // 0x146990
    void  CharaMgr_SetPosition(CharaMgr* mgr, int id, const void* position);    // 0x1468F0
}
