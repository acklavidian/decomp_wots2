#pragma once
#include "hal/ps2_types.h"

// What the world is made of.  None of this is decompiled yet: the functions
// are implemented by src/native/hle.cpp (character manager) and
// src/native/hle_gameplay.cpp (the rest) as stand-ins.  Names are provisional.

struct Sword;
struct World;

extern "C" {
    // ── Character manager (0x143000–0x147000; 0x120 bytes) ──
    void* CharaMgr_Ctor(void* mem);                                 // 0x1470B0
    void  CharaMgr_Init(void* mgr, int a);                          // 0x146F60
    void  CharaMgr_Dtor(void* mgr, int flag);                       // 0x146FE0
    void  CharaMgr_Add(void* mgr, int id, int kind, int chara, int a);      // 0x146DA0
    int   CharaMgr_SetState(void* mgr, int id, int a, u32 b, u32 c);        // 0x144EC0  → action handle
    void  CharaMgr_QueueLoad(void* mgr, int id);                    // 0x145790
    void  CharaMgr_Remove(void* mgr, int id);                       // 0x145DF0
    void  CharaMgr_SetPosition(void* mgr, int id, const void* position);   // 0x1468F0
    void* CharaMgr_GetTransform(void* mgr, int id);                 // 0x146D10
    void  CharaMgr_SetTransform(void* mgr, int id, const void* matrix);     // 0x146A20
    void  CharaMgr_Draw(void* mgr, int pass);                       // 0x145830
    void  CharaMgr_Update(void* mgr, int loading);                  // 0x145BE0
    int   CharaMgr_IsBusy(void* mgr);                               // 0x145400
    char  CharaMgr_IsReady(void* mgr);                              // 0x1441B0
    void  CharaMgr_SetIdle(void* mgr, int id);                      // 0x144E60
    // 0x100 fighting, 0x200 dead, 2 / 4 / 8 sword states
    u32   CharaMgr_GetFlags(void* mgr, int id);                     // 0x1467B0
    int   CharaMgr_HasOpponent(void* mgr, int id);                  // 0x146820
    void  CharaMgr_OnPlayerDeath(void* mgr);                        // 0x143100
    float* CharaMgr_GetPosition(void* mgr, int id);                 // 0x146990
    char  CharaMgr_EquipSword(void* mgr, void* swordDb, Sword* sword);      // 0x1447F0
    void  CharaMgr_EquipKind(void* mgr, int id, int sword, int a);  // 0x144960
    u8*   CharaMgr_GetParams(void* mgr, int id);                    // 0x145F30  the character's live parameter block
    int   CharaMgr_IsActive(void* mgr, int id);                     // 0x146CB0
    void  CharaMgr_fn143550(void* mgr, int id, u32 lips);           // 0x143550  lip data for a spoken line
    int   CharaMgr_DoorAt(void* mgr, int id);                       // 0x143EF0  exit the character stands at, -1
    int   CharaMgr_ExitAt(void* mgr);                               // 0x144040  -1 none, 0xFFFFFF closed
    int   CharaMgr_TalkTarget(void* mgr);                           // 0x143FD0
    int   CharaMgr_HintSpot(void* mgr);                             // 0x143F60
    void  CharaMgr_ClearTransient(void* mgr);                       // 0x1440D0
    void  CharaMgr_Respawn(void* mgr, int indoors, int exit);       // 0x145D90
    void  CharaMgr_ResetAll(void* mgr);                             // 0x143950
    void  CharaMgr_Release(void* mgr, int id);                      // 0x144E00
    void  CharaMgr_fn143c60(void* mgr, int id);                     // 0x143C60
    void  CharaMgr_fn1466e0(void* mgr, int id, int a);              // 0x1466E0
    void  CharaMgr_Sheathe(void* mgr, int id, int a);               // 0x146300
    void  CharaMgr_fn146230(void* mgr, int id);                     // 0x146230
    int   CharaMgr_Exists(void* mgr, int id);                       // 0x144250
    int   CharaMgr_IsHidden(void* mgr, int id);                     // 0x146C20
    void  CharaMgr_StepA(void* mgr);                                // 0x145C90
    void  CharaMgr_StepB(void* mgr);                                // 0x1435D0
    void  CharaMgr_StepFlags(void* mgr, void* flagsObj);            // 0x144300
    void  CharaMgr_StepDrops(void* mgr, void* itemDrops, void* swordDb);    // 0x1436C0
    void  CharaMgr_StepC(void* mgr);                                // 0x145B80
    void  CharaMgr_StepMap(void* mgr, u32 map, void* stage);        // 0x145A60
    void  CharaMgr_StepD(void* mgr);                                // 0x144AB0
    void  CharaMgr_StepFlags2(void* mgr, void* flagsObj);           // 0x1442A0
    void  CharaMgr_StepSwords(void* mgr, void* swordDb);            // 0x144540
    int   CharaMgr_GetAction(void* mgr, int id);                    // 0x144D10
    void  CharaMgr_fn143140(void* mgr, int id);                     // 0x143140  it has no opponent any more
    void  CharaMgr_fn144d90(void* mgr, int id);                     // 0x144D90
    void  CharaMgr_Unflag(void* mgr, void* flagsObj, int id);       // 0x144B20
    void  CharaMgr_Flag(void* mgr, void* flagsObj, int id);         // 0x144BD0
    void  CharaMgr_SetGrouped(void* mgr, int id, int grouped);      // 0x146580
    void  CharaMgr_SetGroup(void* mgr, int id, int group);          // 0x146520
    void  CharaMgr_Spawn(void* mgr, int id, int indoors, int a);    // 0x145D00
    void  CharaMgr_Revive(void* mgr, int id);                       // 0x143090  resets the character
    void  CharaMgr_SetActive(void* mgr, int id, int active);        // 0x1465E0  Chara+0x78
    void  CharaMgr_Set68(void* mgr, int id, int value);             // 0x1464C0
    void  CharaMgr_Set6C(void* mgr, int id, int value);             // 0x1460A0
    void  CharaMgr_Set70(void* mgr, int id, int value);             // 0x146460
    void  CharaMgr_RestoreSword(void* mgr, void* swordDb, int id);  // 0x144640
    void  CharaMgr_fn144370(void* mgr, int id);                     // 0x144370
    int   CharaMgr_GetPartner(void* mgr, int id);                   // 0x146B00  Chara+0x74, −1 none
    void  CharaMgr_SetPartner(void* mgr, int id, int other);        // 0x146400

    // ── Map state (0x14B640–0x14C330; 0x80 bytes): the loaded map, its collision and sky ──
    void* MapState_Ctor(void* mem);                                 // 0x14C330
    void  MapState_Dtor(void* self, int flag);                      // 0x14C0B0
    void  MapState_Load(void* self, int map);                       // 0x14BE30
    void  MapState_SetCurrent(void* self, int map);                 // 0x14BAF0
    int   MapState_IsLoaded(void* self);                            // 0x14BD20
    void  MapState_LoadNext(void* self, int map);                   // 0x14BD80
    void  MapState_SetNext(void* self, int map);                    // 0x14BAE0
    void  MapState_Return(void* self, int map);                     // 0x14BEE0
    void  MapState_BeginDraw(void* self, void* stage);              // 0x14BB60
    void  MapState_Draw(void* self, const float* viewer, void* stage, void* lights);    // 0x14BBD0
    void  MapState_EndDraw(void* self);                             // 0x14BB00
    u32   MapState_GetMap(void* self);                              // 0x14B640

    // ── Stage (0x165450–0x1654B0; 0xD0 bytes): lights, and at +0x60 the camera controllers ──
    void* Stage_Ctor(void* mem);                                    // 0x1654B0
    void  Stage_Dtor(void* self, int flag);                         // 0x165450
    void  Stage_Update(void* self);                                 // 0x1653E0
    void  Stage_Apply(void* self, void* matrix);                    // 0x165430
    void* FollowCam_Ctor(void* mem, void* target, const void* config);  // 0x152470  (0xC0 bytes)
    void  CamList_Push(void* list, void* controller);               // 0x150E60
    u32   CamList_GetFlags(void* list);                             // 0x1503F0
    void  CamList_Update(void* list);                               // 0x150650
    void  CamList_Apply(void* list);                                // 0x1505E0
    void  Camera_Reset(void* camera);                               // 0x1663E0
    void  Camera_SetMap(void* camera, int map);                     // 0x1664A0
    void  Camera_Update(void* camera, GsDevice* dev);               // 0x165CB0
    void  Camera_Finish(void* camera);                              // 0x166040
    void  Camera_Release(void* camera);                             // 0x166A70
    void  GsDevice_ApplyView(GsDevice* dev);                        // 0x12EBD0

    // ── Item drops (0x21B470–0x21C570; 0x80 bytes) ──
    void* ItemDrops_Ctor(void* mem, u32 flagsObj);                  // 0x21C570
    void  ItemDrops_Dtor(void* self, int flag);                     // 0x21C460
    void  ItemDrops_SetFlagsObj(void* self, u32 flagsObj);          // 0x21C530
    void  ItemDrops_Clear(void* self);                              // 0x21B540
    void  ItemDrops_LoadMap(void* self, int map);                   // 0x21C280
    void  ItemDrops_Reset(void* self);                              // 0x21B650
    void  ItemDrops_OnSwordsLoaded(void* self);                     // 0x21BA60
    void  ItemDrops_Update(void* self, u32 map);                    // 0x21B930
    void  ItemDrops_Draw(void* self);                               // 0x21B8B0
    void  ItemDrops_ForgetChara(void* self, int id);                // 0x21B470

    // ── The smaller objects ──
    void* FlagsObj_Ctor(void* mem);                                 // 0x1571F0  (0x10 bytes)
    void  FlagsObj_Dtor(void* self, int flag);                      // 0x1570A0
    void  FlagsObj_StepA(void* self);                               // 0x156DE0
    void  FlagsObj_StepB(void* self);                               // 0x156A00
    void  FlagsObj_Reset(void* self);                               // 0x155270
    void* Effects_Ctor(void* mem);                                  // 0x190450  (0x3C bytes)
    void  Effects_Dtor(void* self, int flag);                       // 0x1902D0
    void  Effects_Update(void* self, void* camList, void* charaMgr, void* hud);     // 0x18F880
    void  Effects_Draw(void* self);                                 // 0x18F860
    int   Effects_IsBusy(void* self, int a);                        // 0x18E830
    void* Pairs_Ctor(void* mem);                                    // 0x229630  (8 bytes)
    void  Pairs_Dtor(void* self, int flag);                         // 0x2295E0
    void  Pairs_Update(void* self, void* charaMgr, void* mapState); // 0x229580
    void  Pairs_UpdateHud(void* self, void* charaMgr, void* mapState);      // 0x227B30
    void* Targeting_Ctor(void* mem);                                // 0x2ABA50  (0x18 bytes)
    void  Targeting_Dtor(void* self, int flag);                     // 0x2AB9A0
    void  Targeting_SetPlayer(void* self, u8* params);              // 0x2AB880
    u8    Targeting_IsReady(void* self);                            // 0x2AB600
    void  Targeting_Draw(void* self, u32 map);                      // 0x2AB530
    void  Collision_Update(void* charaMgr, void* mapState, void* itemDrops);    // 0x1759B0
    void  Collision_Release();                                      // 0x175A00
    // A blob shadow under a character; null if it could not be made.
    void  Shadow_SetHidden(void* shadow, int hidden);               // 0x1673F0
    void  Shadow_Release(void* shadow);                             // 0x172090
    void  RenderMgr_SetFlash(void* renderMgr, const float* color);  // 0x14EBB0
    void  RenderMgr_SetMap(void* renderMgr, u32 map);               // 0x14F900
    void  RenderMgr_Draw(void* renderMgr);                          // 0x14F580

    // ── HUD (0x21C900–0x21DD10; *0x2EE69C) ──
    void  Hud_ShowGameOver(void* hud);                              // 0x21CE70
    int   Hud_GameOverBusy(void* hud);                              // 0x21CEA0
    int   Hud_DeathState(void* hud);                                // 0x21CCD0  1 shown, 2 done
    void  Hud_EndDeath(void* hud);                                  // 0x21CC90
    void  Hud_SetGauges(void* hud, const void* gauges);             // 0x21CED0
    int   Hud_UpdateDrops(void* hud, void* itemDrops, const void* matrix, void* swordDb);   // 0x21CFA0

    // ── Others ──
    void  SwordDb_DropAt(void* db, int sword, const float* pos);    // 0x15C3C0
    void  SwordDb_FreeModels(void* db);                             // 0x15C780
    void  SwordDb_ChangeMap(void* db, int map, int other);          // 0x15C560
    void  SwordDb_Draw(void* db);                                   // 0x15C5F0
    void  SwordDb_Update(void* db, u32 map);                        // 0x15C690
    void  SwordDb_Unload(void* db);                                 // 0x15DC60
    void  CharaResources_Purge(void* res);                          // 0x1D2AC0
    void  CharaResources_Prepare(void* res);                        // 0x1D2A60
    void* WorldLink_Ctor(void* mem, World* world);                  // 0x197EB0  (8 bytes; *0x2EE690)
}
