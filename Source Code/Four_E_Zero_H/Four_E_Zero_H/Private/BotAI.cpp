#include "pch.h"
#include "../Public/BotAI.h"
#include "../Public/Configuration.h"

#include "../Public/NPCs.h"

#include "../Public/PlaceDump.h"
#include "../Public/ServerConsole.h"
#include "../Public/NoKillerMessages.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"

#include "../../FortniteGame/Public/FortWeapon.h"
#include "../../FortniteGame/Public/BuildingSMActor.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"

#include "../../FortniteGame/Public/FortLootPackage.h"

#include "../Plugins/CrashReporter/Public/CrashReporter.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern bool GStormMapDataWasRepaired;

extern uint64_t ApplyCharacterCustomization;

uint8 ToDeathCause(AFortPlayerPawnAthena* Pawn, FGameplayTagContainer& DeathTags, bool bDBNO);

uint64 FindEnterAircraft();

static void LogLine(const char* Fmt, ...)
{
    if (!FConfiguration::bBotLog || !Fmt)
        return;

    char Buffer[1024];

    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);

    const size_t Length = strlen(Buffer);
    printf("%s%s", Buffer, (Length > 0 && Buffer[Length - 1] == '\n') ? "" : "\n");
    fflush(stdout);
}

namespace BotAI
{

    static const char* GTickStage = "starting";

    static void* GFightBot = nullptr;

    static bool bBusDeparted = false;
    static float BusStartedTime = 0.f;

    static std::set<void*> ThankedBots;

    static std::set<void*> MovedToIsland;

    static std::set<void*> BTRetried;

    static int MoveResult_Failed = 0;
    static int MoveResult_AlreadyAtGoal = 0;
    static int MoveResult_Success = 0;
    static int MoveResult_Other = 0;

    static float BusCountdownStart = -1.f;
    static int LastBusSecondAnnounced = -1;

    static std::map<void*, int> BotCombatTick;

    static AFortInventory* EnsureBotInventory(AActor* BotControllerActor);

    static const UFortItemDefinition* FindItemDefByName(const wchar_t* name);

    static int GiveBotLootpoolKit(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState);
    static void NoteFixedBotKit(AFortPlayerStateAthena* PlayerState, const char* Why);
    static std::string GiveBotSpareRounds(AFortInventory* Inventory, const UFortItemDefinition* Gun);

    static std::set<void*> LoadoutGiven;

    static std::map<void*, const UFortItemDefinition*> BotWeapons;

    static std::map<void*, AFortInventory*> BotInventories;

    static std::map<void*, int> BotWeaponClipSize;

    static std::set<void*> BotTeamsAssigned;

    static std::set<void*> PickaxeGiven;

    static std::map<void*, int> BotZoneMoveTick;

    static std::map<void*, float> BotLastZoneDistance;

    static std::map<void*, float> BotFacingYaw;

    struct FBotFireState
    {
        int ReactionTicksLeft = -1;
        int BurstTicksLeft = 0;
        int CooldownTicksLeft = 0;
        bool bFiring = false;

        int RoundsSinceReload = 0;
    };

    static std::map<void*, FBotFireState> BotFire;

    struct FBotReload
    {
        bool bActive = false;
        void* Weapon = nullptr;
        FWeakObjectPtr WeaponWeak{};
        int RoundsBefore = -1;
        int Magazine = 0;
        float StartedAt = 0.f;
        float Duration = 0.f;
        const char* Why = "";
        float NoFightSince = -1.f;
        float ReloadFlagSince = -1.f;
        float EquipFlagSince = -1.f;
        void* MagazineOf = nullptr;
        int MagazineSize = 0;
    };

    static std::map<void*, FBotReload> BotReloads;

    struct FBotBody
    {
        float Clock = 0.f;
        bool bHasGoal = false;
        FVector Goal{};
        float AcceptRadius = 100.f;
        bool bScripted = false;
        bool bChecked = false;
        float WatchSince = -1.f;
        float WatchZ = 0.f;
        void* Owner = nullptr;
        bool bGroundKnown = false;
        double GroundZ = 0.0;
        FVector GroundFor{};
        bool bOnGround = false;
    };

    static std::map<void*, FBotBody> BotBodies;

    static bool IsPlayerControllerBot(AActor* Controller)
    {
        static auto PlayerControllerClass = AFortPlayerControllerAthena::StaticClass();
        return Controller && PlayerControllerClass && Controller->IsA(PlayerControllerClass);
    }

    static bool SetBoolByName(UObject* Object, const char* Name, bool bValue)
    {
        auto Prop = Object ? Object->GetProperty(Name, 0x20000) : nullptr;
        if (!Prop)
            return false;

        const uint32 Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
        const uint8 Mask = Prop->GetFieldMask();

        if (!Mask || Offset >= 0x10000)
            return false;

        uint8& Byte = GetFromOffset<uint8>(Object, Offset);

        if (Mask == 0xFF)
            Byte = bValue ? 1 : 0;
        else if (bValue)
            Byte |= Mask;
        else
            Byte &= (uint8)~Mask;

        return true;
    }

    static int ReadBoolByName(const UObject* Object, const char* Name)
    {
        auto Prop = Object ? Object->GetProperty(Name, 0x20000) : nullptr;
        if (!Prop)
            return -1;

        const uint32 Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
        const uint8 Mask = Prop->GetFieldMask();

        if (!Mask || Offset >= 0x10000)
            return -1;

        return (GetFromOffset<uint8>(Object, Offset) & Mask) ? 1 : 0;
    }

    static void MarkBotReadyLikeAPlayer(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState, bool bHasPawn)
    {
        if (!Controller)
            return;

        auto PC = (UObject*)Controller;

        SetBoolByName(PC, "bHasClientFinishedLoading", true);
        SetBoolByName(PC, "bHasServerFinishedLoading", true);
        SetBoolByName(PC, "bHasFinishedLoading", true);
        SetBoolByName(PC, "bReadyToStartMatch", true);
        SetBoolByName(PC, "bAssignedStartSpawn", true);

        if (bHasPawn)
        {
            SetBoolByName(PC, "bHasInitiallySpawned", true);
            SetBoolByName(PC, "bClientPawnIsLoaded", true);
        }

        SetBoolByName(PC, "bMarkedAlive", true);

        if (PlayerState)
        {
            auto PS = (UObject*)PlayerState;

            SetBoolByName(PS, "bIsSpectator", false);
            SetBoolByName(PS, "bHasFinishedLoading", true);
            SetBoolByName(PS, "bHasStartedPlaying", true);
        }
    }

    struct FBusExitLatch
    {
        bool bResolved = false;
        bool bSupported = false;
        uint32 ExitedOffset = 0;
        uint32 ReadyOffset = 0;
        uint8 ReadyMask = 0;
        int Written = 0;
        int Applied = 0;
        int Refused = 0;
        int StillAboard = 0;
        int Logged = 0;
    };

    static FBusExitLatch GBusExitLatch;

    struct FCmpByteZero
    {
        uint8 BaseRegister = 0;
        uint32 Displacement = 0;
        size_t Size = 0;
    };

    static bool DecodeCmpByteZero(const uint8* Code, size_t Remaining, FCmpByteZero& Out)
    {
        if (!Code || Remaining < 7)
            return false;

        size_t Cursor = 0;
        uint8 Rex = 0;

        if ((Code[Cursor] & 0xF0) == 0x40)
        {
            Rex = Code[Cursor++];

            if (Remaining - Cursor < 7)
                return false;
        }

        if (Code[Cursor++] != 0x80)
            return false;

        const uint8 ModRM = Code[Cursor++];

        if ((ModRM & 0xF8) != 0xB8)
            return false;

        uint8 BaseRegister = ModRM & 7;

        if (BaseRegister == 4)
        {
            if (Remaining <= Cursor)
                return false;

            const uint8 Sib = Code[Cursor++];
            const uint8 IndexRegister = (uint8)(((Sib >> 3) & 7) + ((Rex & 0x02) ? 8 : 0));

            if (IndexRegister != 4)
                return false;

            BaseRegister = (uint8)((Sib & 7) + ((Rex & 0x01) ? 8 : 0));
        }
        else
        {
            BaseRegister = (uint8)(BaseRegister + ((Rex & 0x01) ? 8 : 0));
        }

        if (Remaining - Cursor < sizeof(uint32) + 1)
            return false;

        uint32 Displacement = 0;
        memcpy(&Displacement, Code + Cursor, sizeof(Displacement));
        Cursor += sizeof(Displacement);

        if (Code[Cursor++] != 0)
            return false;

        Out.BaseRegister = BaseRegister;
        Out.Displacement = Displacement;
        Out.Size = Cursor;
        return true;
    }

    static bool FunctionSavesThisInRegister(const uint8* Code, size_t End, uint8 ExpectedRegister)
    {
        if (!Code || ExpectedRegister > 15)
            return false;

        for (size_t At = 0; At + 2 < End; At++)
        {
            size_t Cursor = At;
            uint8 Rex = 0;

            if ((Code[Cursor] & 0xF0) == 0x40)
                Rex = Code[Cursor++];

            if (Cursor + 1 >= End)
                break;

            const uint8 Opcode = Code[Cursor++];

            if (Opcode != 0x8B && Opcode != 0x89)
                continue;

            const uint8 ModRM = Code[Cursor];

            if ((ModRM & 0xC0) != 0xC0)
                continue;

            const uint8 Reg = (uint8)(((ModRM >> 3) & 7) + ((Rex & 0x04) ? 8 : 0));
            const uint8 Rm = (uint8)((ModRM & 7) + ((Rex & 0x01) ? 8 : 0));
            const uint8 Destination = Opcode == 0x8B ? Reg : Rm;
            const uint8 Source = Opcode == 0x8B ? Rm : Reg;

            if (Source == 1 && Destination == ExpectedRegister)
                return true;
        }

        return false;
    }

    static bool MemoryIsReadable(const uint8* Start, size_t Size)
    {
        if (!Start || !Size)
            return false;

        for (size_t At = 0; At < Size; At += 0x1000)
            if (IsBadReadPtr((void*)(Start + At)))
                return false;

        return !IsBadReadPtr((void*)(Start + Size - 1));
    }

    static bool ResolveBusExitLatch(AFortPlayerControllerAthena* Controller)
    {
        auto& Latch = GBusExitLatch;

        if (Latch.bResolved)
            return Latch.bSupported;

        if (!Controller)
            return false;

        Latch.bResolved = true;

        auto ReadyProp = ((UObject*)Controller)->GetProperty("bReadyToStartMatch", 0x20000);

        if (!ReadyProp)
        {
            LogLine("[BUS] Where the game marks a player as off the bus was NOT looked for: bReadyToStartMatch is not on this "
                    "controller class -- bots get their pawn by hand, and the bus may still throw them out at the end of its drop zone.");
            return false;
        }

        const uint32 ReadyOffset = GetFromOffset<uint32>(ReadyProp, Offsets::Offset_Internal);
        const uint8 ReadyMask = ReadyProp->GetFieldMask();

        if (!ReadyMask || ReadyOffset >= 0x20000)
        {
            LogLine("[BUS] Where the game marks a player as off the bus was NOT looked for: bReadyToStartMatch reads as offset 0x%X, "
                    "mask 0x%X.", ReadyOffset, (unsigned)ReadyMask);
            return false;
        }

        const uint64 EnterAircraft = FindEnterAircraft();
        const size_t ScanSize = 0x600;
        const uint64 WarningRef = Memcury::Scanner::FindStringRef(
            L"EnterAircraft: [%s] is attempting to enter aircraft after having already exited.", false, 0,
            VersionInfo.FortniteVersion >= 19).Get();

        if (!EnterAircraft || WarningRef < EnterAircraft || WarningRef >= EnterAircraft + ScanSize ||
            !MemoryIsReadable((const uint8*)EnterAircraft, ScanSize))
        {
            LogLine("[BUS] Where the game marks a player as off the bus was NOT found: the game's EnterAircraft is %s, its "
                    "\"already exited\" warning %s.",
                    EnterAircraft ? "found" : "NOT found",
                    !WarningRef ? "is NOT found" : ((WarningRef >= EnterAircraft && WarningRef < EnterAircraft + ScanSize)
                                                    ? "is inside it" : "is NOT inside it"));
            return false;
        }

        const uint8* Code = (const uint8*)EnterAircraft;
        const size_t WarningAt = (size_t)(WarningRef - EnterAircraft);
        uint32 ResolvedOffset = 0;
        int MatchCount = 0;

        for (size_t FirstAt = 0; FirstAt + 7 <= ScanSize; FirstAt++)
        {
            FCmpByteZero First{};

            if (!DecodeCmpByteZero(Code + FirstAt, ScanSize - FirstAt, First))
                continue;

            if (First.Displacement == 0 || First.Displacement >= ReadyOffset || ReadyOffset - First.Displacement > 0x40 ||
                FirstAt >= WarningAt || WarningAt - FirstAt > 0x100 ||
                !FunctionSavesThisInRegister(Code, FirstAt, First.BaseRegister))
                continue;

            const size_t PairEnd = (FirstAt + 0x300 < ScanSize) ? FirstAt + 0x300 : ScanSize;
            bool bFoundPair = false;

            for (size_t SecondAt = FirstAt + First.Size; SecondAt + 7 <= PairEnd; SecondAt++)
            {
                FCmpByteZero Second{};

                if (!DecodeCmpByteZero(Code + SecondAt, ScanSize - SecondAt, Second))
                    continue;

                if (Second.BaseRegister == First.BaseRegister && Second.Displacement + 1 == First.Displacement)
                {
                    bFoundPair = true;
                    break;
                }
            }

            if (!bFoundPair)
                continue;

            ResolvedOffset = First.Displacement;
            MatchCount++;
        }

        if (MatchCount != 1 || ResolvedOffset == 0)
        {
            LogLine("[BUS] Where the game marks a player as off the bus was NOT found: %d matching pair(s) of checks in EnterAircraft "
                    "(exactly 1 is needed) -- nothing is written; bots get their pawn by hand, and if the bus throws them out "
                    "anyway they are put back on their own pawn.", MatchCount);
            return false;
        }

        Latch.ExitedOffset = ResolvedOffset;
        Latch.ReadyOffset = ReadyOffset;
        Latch.ReadyMask = ReadyMask;
        Latch.bSupported = true;

        LogLine("[BUS] Found where the game marks a player as off the bus (the way Magnesium finds it): \"in the aircraft\" at "
                "+0x%X and \"has exited the aircraft\" at +0x%X on the controller, %u byte(s) before bReadyToStartMatch.",
                ResolvedOffset - 1, ResolvedOffset, ReadyOffset - ResolvedOffset);
        return true;
    }

    static bool MarkBotOffTheBus(AFortPlayerControllerAthena* Controller, const char** Why)
    {
        const char* Ignored = nullptr;
        const char*& Reason = Why ? *Why : Ignored;

        if (!Controller)
        {
            Reason = "no controller";
            return false;
        }

        if (!Controller->IsInAircraft())
        {
            Reason = "the game already had it off the bus";
            return true;
        }

        if (!ResolveBusExitLatch(Controller) || !GBusExitLatch.bSupported)
        {
            Reason = "where the game marks it was not found on this build";
            return false;
        }

        auto& Latch = GBusExitLatch;
        uint8* Base = (uint8*)Controller;
        const uint8* ReadyByte = Base + Latch.ReadyOffset;
        uint8* State = Base + Latch.ExitedOffset - 1;

        if ((*ReadyByte & Latch.ReadyMask) == 0)
        {
            Reason = "it is not marked ready to start the match";
            return false;
        }

        const uint8 InAircraft = State[0];
        const uint8 HasExited = State[1];

        if (InAircraft > 1 || HasExited > 1)
        {
            Latch.Refused++;
            Reason = "the two bytes are not a pair of bools on this controller -- left alone";
            return false;
        }

        if (InAircraft != 0 || HasExited != 1)
        {
            State[0] = 0;
            State[1] = 1;
            Latch.Written++;
        }

        const bool bStillAboard = Controller->IsInAircraft();

        if (Latch.Logged < 3)
        {
            Latch.Logged++;
            LogLine("[BUS] Marked off the bus (check %d of 3): \"in the aircraft\" %u -> %u, \"has exited\" %u -> %u | the game's "
                    "IsInAircraft: on board before, %s after.",
                    Latch.Logged, (unsigned)InAircraft, (unsigned)State[0], (unsigned)HasExited, (unsigned)State[1],
                    bStillAboard ? "STILL ON BOARD" : "off the bus");
        }

        if (bStillAboard)
        {
            Latch.StillAboard++;
            Reason = "the two bytes are set, but the game's IsInAircraft still says on board";
            return false;
        }

        Latch.Applied++;
        Reason = "marked off the bus, and the game agrees";
        return true;
    }

    static int GCombatEngagements = 0;
    static int GCombatBurstsFired = 0;
    static int GCombatBotOnBotHits = 0;

    static int GCombatNoWeapon = 0;

    static int GMagazinesRefilled = 0;

    static int GReloadsStarted = 0;

    static int GBotsMadeKillableAgain = 0;

    static int GWeaponsReissued = 0;

    static int GLocksMade = 0;
    static int GLocksEnded = 0;
    static int GBurstsCutOutOfSight = 0;
    static int GTicksHeldFireOutOfSight = 0;
    static int GAimDeliberateMisses = 0;
    static int GChaseHops = 0;
    static std::map<void*, std::string> BotLockedOnName;

    static std::map<void*, AFortPlayerPawnAthena*> BotCombatTarget;

    static std::map<void*, FVector> BotLastSeenSpot;

    static std::set<void*> BotStoppedForFight;

    static std::set<void*> BotHasFocus;

    struct FBotAimRecord
    {
        double Yaw = 0.0;
        double Pitch = 0.0;
    };
    static std::map<void*, FBotAimRecord> BotIntendedAim;
    static int GAimChecks = 0;
    static int GAimHeld = 0;
    static double GAimOffSum = 0.0;
    static double GAimOffMax = 0.0;

    static int GHitsOnLockedTarget = 0;
    static int GHitsOnSomebodyElse = 0;

    static int GTerrainBlocks = 0;

    static const uint8 GamePhase_Aircraft = 2;
    static const uint8 GamePhase_SafeZones = 3;

    static bool PlayersAreProtected(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasGamePhase())
            return false;

        return GameState->GamePhase < GamePhase_Aircraft;
    }

    static std::set<void*> LandedBots;

    static bool AircraftPhaseReached()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return false;

        auto GS = (AFortGameStateAthena*)World->GameState;

        if (!GS->HasGamePhase())
            return true;

        return GS->GamePhase >= GamePhase_Aircraft;
    }

    static bool HasLanded(void* ControllerKey, AFortPlayerPawnAthena* Pawn)
    {
        if (!ControllerKey)
            return false;

        if (!AircraftPhaseReached())
            return false;

        if (LandedBots.find(ControllerKey) != LandedBots.end())
            return true;

        if (!Pawn)
            return false;

        bool bDropped = false;

        if (MovedToIsland.find((void*)Pawn) != MovedToIsland.end())
            bDropped = true;

        if (!bDropped && Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
            bDropped = true;

        if (!bDropped && Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus)
            bDropped = true;

        if (!bDropped && Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen)
            bDropped = true;

        if (!bDropped)
        {
            FVector Loc = Pawn->K2_GetActorLocation();
            if (Loc.Z > 8000.0)
                bDropped = true;
        }

        if (bDropped)
        {
            LandedBots.insert(ControllerKey);
            return true;
        }

        return false;
    }

    static void KeepTheClockHonest()
    {
        if (!FConfiguration::bLockServerSpeed)
            return;

        auto World = UWorld::GetWorld();
        if (!World || !World->HasPersistentLevel() || !World->PersistentLevel)
            return;

        auto Level = (UObject*)World->PersistentLevel;

        static int32 SettingsOffset = -2;
        if (SettingsOffset == -2)
            SettingsOffset = (int32)Level->GetOffset("WorldSettings");

        if (SettingsOffset < 0)
            return;

        auto Settings = GetFromOffset<UObject*>(Level, (uint32)SettingsOffset);
        if (!Settings)
            return;

        static const char* Fields[] = { "TimeDilation", "MatineeTimeDilation", "DemoPlayTimeDilation" };
        static int32 Offsets[3] = { -2, -2, -2 };

        for (int i = 0; i < 3; i++)
        {
            if (Offsets[i] == -2)
                Offsets[i] = (int32)Settings->GetOffset(Fields[i]);

            if (Offsets[i] < 0)
                continue;

            float& Value = GetFromOffset<float>(Settings, (uint32)Offsets[i]);

            if (Value != 1.f)
            {
                Value = 1.f;

                static int Reported = 0;
                if (++Reported <= 3)
                    LogLine("[SPEED] Something changed the server's clock (%s). Put back to normal speed.", Fields[i]);
            }
        }
    }

    static bool LooksLikeAName(const std::string& Name)
    {
        if (Name.size() < 3 || Name.size() > 32)
            return false;

        int Letters = 0;
        int HexDigits = 0;

        for (size_t i = 0; i < Name.size(); i++)
        {
            const unsigned char Ch = (unsigned char)Name[i];

            if (Ch < 0x20 || Ch > 0x7E)
                return false;

            if ((Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z'))
                Letters++;

            if ((Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f') || (Ch >= 'A' && Ch <= 'F'))
                HexDigits++;
        }

        if (Letters == 0)
            return false;

        if (Name.size() >= 16 && HexDigits == (int)Name.size())
            return false;

        return true;
    }

    static std::string LongestAnsiRun(const uint8* Bytes, int Count)
    {
        std::string Best;
        std::string Current;

        for (int i = 0; i < Count; i++)
        {
            const unsigned char Ch = (unsigned char)Bytes[i];

            if (Ch >= 0x20 && Ch <= 0x7E)
            {
                Current += (char)Ch;
                continue;
            }

            if (Current.size() > Best.size())
                Best = Current;

            Current.clear();
        }

        if (Current.size() > Best.size())
            Best = Current;

        return Best;
    }

    static std::string LongestWideRun(const uint8* Bytes, int Count)
    {
        std::string Best;

        for (int Start = 0; Start < 2; Start++)
        {
            std::string Current;

            for (int i = Start; i + 1 < Count; i += 2)
            {
                const unsigned char Low = (unsigned char)Bytes[i];
                const unsigned char High = (unsigned char)Bytes[i + 1];

                if (High == 0x00 && Low >= 0x20 && Low <= 0x7E)
                {
                    Current += (char)Low;
                    continue;
                }

                if (Current.size() > Best.size())
                    Best = Current;

                Current.clear();
            }

            if (Current.size() > Best.size())
                Best = Current;
        }

        return Best;
    }

    static std::string NameFromUniqueId(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState)
            return std::string();

        const uint8* Bytes = nullptr;
        int Count = 0;

        if (!FUniqueNetIdRepl::HasReplicationBytes())
            return std::string();

        if (PlayerState->HasUniqueId())
        {
            auto& Replicated = PlayerState->UniqueId.ReplicationBytes;

            if (Replicated.IsValid() && Replicated.Num() > 0)
            {
                Bytes = (const uint8*)Replicated.GetData();
                Count = Replicated.Num();
            }
        }

        if ((!Bytes || Count <= 0) && PlayerState->HasUniqueID())
        {
            auto& Replicated = PlayerState->UniqueID.ReplicationBytes;

            if (Replicated.IsValid() && Replicated.Num() > 0)
            {
                Bytes = (const uint8*)Replicated.GetData();
                Count = Replicated.Num();
            }
        }

        if (!Bytes || Count <= 0 || Count > 4096)
            return std::string();

        std::string Narrow = LongestAnsiRun(Bytes, Count);
        std::string Wide = LongestWideRun(Bytes, Count);

        std::string Picked = Wide.size() > Narrow.size() ? Wide : Narrow;

        const size_t Colon = Picked.find_last_of(':');

        if (Colon != std::string::npos && (Picked.size() - Colon - 1) >= 3)
            Picked = Picked.substr(Colon + 1);

        while (!Picked.empty() && Picked.front() == ' ')
            Picked.erase(Picked.begin());

        while (!Picked.empty() && Picked.back() == ' ')
            Picked.pop_back();

        return LooksLikeAName(Picked) ? Picked : std::string();
    }

    static std::string NameAsStored(AFortPlayerStateAthena* PlayerState);

    static std::string SafeNameOf(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState)
            return std::string();

        const bool bIsBot = PlayerState->HasbIsABot() && PlayerState->bIsABot;

        if (!bIsBot)
        {
            const std::string FromAccount = NameFromUniqueId(PlayerState);

            if (!FromAccount.empty())
                return FromAccount;
        }

        return NameAsStored(PlayerState);
    }

    static const FString* PlayerNameField(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState)
            return nullptr;

        if (PlayerState->HasPlayerNamePrivate())
            return &PlayerState->GetPlayerNamePrivate();

        static int32 Offset = -2;

        if (Offset == -2)
        {
            const uint32 Off = ((UObject*)PlayerState)->GetOffset("PlayerName");
            Offset = (Off == (uint32)-1) ? -1 : (int32)Off;

            LogLine("[NAMES] This build keeps player names in %s.",
                    Offset >= 0 ? "APlayerState::PlayerName (there is no PlayerNamePrivate here)"
                                : "NEITHER PlayerNamePrivate NOR PlayerName -- names will come out blank");
        }

        if (Offset < 0)
            return nullptr;

        return (const FString*)((const uint8*)PlayerState + Offset);
    }

    static std::wstring RawStoredName(AFortPlayerStateAthena* PlayerState)
    {
        const FString* Field = PlayerNameField(PlayerState);

        if (!Field)
            return std::wstring();

        const auto& Stored = *Field;

        const wchar_t* Characters = Stored.CStr();
        int32 Length = Stored.Num();

        if (!Characters || Length <= 0 || Length > 512)
            return std::wstring();

        std::wstring Wide;
        Wide.reserve((size_t)Length);

        for (int32 i = 0; i < Length; i++)
        {
            const wchar_t Ch = Characters[i];

            if (Ch == L'\0')
                break;

            Wide += Ch;
        }

        return Wide;
    }

    static int GNamesScrambled = -1;

    static std::wstring UnscrambleName(const std::wstring& Stored)
    {
        std::wstring Plain = Stored;
        const size_t Length = Plain.size();

        for (size_t i = 0; i < Length; i++)
            Plain[i] = (wchar_t)(Plain[i] + (wchar_t)((((size_t)(Length & 3)) + 3 * (i + 1)) & 7));

        return Plain;
    }

    static std::string NameAsStored(AFortPlayerStateAthena* PlayerState)
    {
        std::wstring Wide = RawStoredName(PlayerState);

        if (Wide.empty())
            return std::string();

        if (GNamesScrambled == 1)
            Wide = UnscrambleName(Wide);

        std::string Narrow;

        const int Needed = WideCharToMultiByte(CP_UTF8, 0, Wide.c_str(), (int)Wide.size(), nullptr, 0, nullptr, nullptr);

        if (Needed > 0)
        {
            Narrow.resize((size_t)Needed);
            WideCharToMultiByte(CP_UTF8, 0, Wide.c_str(), (int)Wide.size(), &Narrow[0], Needed, nullptr, nullptr);
        }
        else
        {
            for (size_t i = 0; i < Wide.size(); i++)
            {
                const wchar_t Ch = Wide[i];

                if (Ch > 0 && Ch < 128)
                    Narrow += (char)Ch;
            }
        }

        std::string Clean;
        Clean.reserve(Narrow.size());

        for (size_t i = 0; i < Narrow.size(); i++)
        {
            const unsigned char Ch = (unsigned char)Narrow[i];

            if (Ch >= 0x20 && Ch != 0x7F)
                Clean += (char)Ch;
        }

        while (!Clean.empty() && Clean.front() == ' ')
            Clean.erase(Clean.begin());

        while (!Clean.empty() && Clean.back() == ' ')
            Clean.pop_back();

        return Clean;
    }

    static void HoldTheStorm(AFortGameMode* GameMode, float Now)
    {
        if (!UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone)
            return;

        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return;

        auto Indicator = GameMode->SafeZoneIndicator;

        float HeldOpen = Now + 600.f;

        if (Indicator->HasSafeZoneStartShrinkTime() && (float)Indicator->SafeZoneStartShrinkTime < HeldOpen)
            Indicator->SafeZoneStartShrinkTime = HeldOpen;

        if (Indicator->HasSafeZoneFinishShrinkTime() && (float)Indicator->SafeZoneFinishShrinkTime < HeldOpen)
            Indicator->SafeZoneFinishShrinkTime = HeldOpen;

        auto World = UWorld::GetWorld();

        if (World && World->GameState)
        {
            auto GameState = (AFortGameStateAthena*)World->GameState;

            if (GameState->HasSafeZonesStartTime() && (float)GameState->SafeZonesStartTime < HeldOpen)
                GameState->SafeZonesStartTime = HeldOpen;
        }

        if (GameMode->HasbSafeZonePaused())
            GameMode->bSafeZonePaused = true;

        static bool bSaid = false;
        if (!bSaid)
        {
            bSaid = true;
            LogLine("[ZONE] Storm held where it is. Nothing shrinks and the clock stops until \"cheat resumesafezone\".");
        }
    }

    static void ReleaseTheStorm(AFortGameMode* GameMode, float Now)
    {
        static bool bWasPaused = false;

        if (UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone)
        {
            bWasPaused = true;
            return;
        }

        if (!bWasPaused)
            return;

        bWasPaused = false;

        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return;

        auto Indicator = GameMode->SafeZoneIndicator;

        float StartsIn = Now + 30.f;
        float FinishesIn = Now + 60.f;

        if (Indicator->HasSafeZoneStartShrinkTime())
            Indicator->SafeZoneStartShrinkTime = StartsIn;

        if (Indicator->HasSafeZoneFinishShrinkTime())
            Indicator->SafeZoneFinishShrinkTime = FinishesIn;

        auto World = UWorld::GetWorld();

        if (World && World->GameState)
        {
            auto GameState = (AFortGameStateAthena*)World->GameState;

            if (GameState->HasSafeZonesStartTime() && (float)GameState->SafeZonesStartTime > Now)
                GameState->SafeZonesStartTime = Now;
        }

        if (GameMode->HasbSafeZonePaused())
            GameMode->bSafeZonePaused = false;

        LogLine("[ZONE] Storm running again -- it starts closing in 30 seconds.");
    }

    static std::set<const UFortItemDefinition*> GrapplerDefinitions;

    static void ApplyWeaponHotfixes()
    {
        static bool bDone = false;

        if (bDone)
            return;

        static const wchar_t* Grapplers[] = {
            nullptr
        };

        const int Wanted = FConfiguration::GrapplerClipSize;

        if (Wanted <= 0)
        {
            bDone = true;
            return;
        }

        int Patched = 0;
        int Found = 0;

        for (auto Name : Grapplers)
        {
            if (!Name)
                break;

            auto Definition = FindObject<UFortWeaponItemDefinition>(Name);

            if (!Definition)
            {
                std::wstring Wide = Name;
                std::string Narrow(Wide.begin(), Wide.end());
                Definition = TUObjectArray::FindObject<UFortWeaponItemDefinition>(Narrow.c_str());
            }

            if (!Definition)
                continue;

            Found++;
            GrapplerDefinitions.insert((const UFortItemDefinition*)Definition);

            auto Stats = AFortInventory::GetStats(Definition);

            if (!Stats || !FFortRangedWeaponStats::HasClipSize())
                continue;

            if (Stats->ClipSize == Wanted)
            {
                Patched++;
                continue;
            }

            int32 Clip = (int32)Wanted;
            Stats->ClipSize = Clip;

            if (FFortRangedWeaponStats::HasInitialClips())
            {
                int32 Clips = 1;
                Stats->InitialClips = Clips;
            }

            Patched++;
        }

        if (Found == 0)
            return;

        bDone = true;

        LogLine("[HOTFIX] Grappler clip size set to %d on %d of %d grappler definition(s) found -- the same thing the "
                "backend RowUpdate does, applied here instead.", Wanted, Patched, Found);
    }

    static int32* WeaponAmmoCount(AActor* Weapon);

    static void TopUpGrapplers(AFortGameStateAthena* GameState, int TickNow)
    {
        const int Wanted = FConfiguration::GrapplerClipSize;

        if (Wanted <= 0 || GrapplerDefinitions.empty() || !GameState || !GameState->HasPlayerArray())
            return;

        static int NextCheck = 0;

        if (TickNow < NextCheck)
            return;

        NextCheck = TickNow + 15;

        if (!FFortItemEntry::HasItemDefinition() || !FFortItemEntry::HasLoadedAmmo())
            return;

        int ToppedUp = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            if (PS->HasbIsABot() && PS->bIsABot)
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PS->Owner;
            if (!Controller || !Controller->IsA<AFortPlayerControllerAthena>())
                continue;

            auto Inventory = Controller->WorldInventory;
            if (!Inventory || !Inventory->HasInventory())
                continue;

            for (int e = 0; e < Inventory->Inventory.ItemInstances.Num(); e++)
            {
                auto Item = Inventory->Inventory.ItemInstances[e];
                if (!Item || !Item->HasItemEntry())
                    continue;

                auto& Entry = Item->ItemEntry;

                if (!Entry.ItemDefinition)
                    continue;

                if (GrapplerDefinitions.find(Entry.ItemDefinition) == GrapplerDefinitions.end())
                    continue;

                if (Entry.LoadedAmmo >= Wanted)
                    continue;

                int32 Full = (int32)Wanted;
                Entry.LoadedAmmo = Full;
                Inventory->Update(&Entry);
                ToppedUp++;
            }

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

            if (Pawn && Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
            {
                auto Held = (AFortWeapon*)Pawn->CurrentWeapon;

                if (Held->HasWeaponData() && Held->WeaponData &&
                    GrapplerDefinitions.find((const UFortItemDefinition*)Held->WeaponData) != GrapplerDefinitions.end())
                {
                    if (auto Ammo = WeaponAmmoCount((AActor*)Held))
                        if (*Ammo < Wanted)
                            *Ammo = Wanted;
                }
            }
        }

        if (ToppedUp > 0)
        {
            static int Reported = 0;

            if (++Reported <= 5)
                LogLine("[HOTFIX] Grappler put back to %d round(s) on %d item(s).", Wanted, ToppedUp);
        }
    }

    static std::map<void*, void*> HumanPawnToppedUp;
    static std::map<void*, int> HumanTopUpUntilTick;

    static void* GOwnerController = nullptr;

    bool IsServerOwner(AFortPlayerControllerAthena* Controller)
    {
        if (!Controller)
            return false;

        if (!GOwnerController)
            return true;

        return GOwnerController == (void*)Controller;
    }

    static void ClaimServerOwner(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        void* First = nullptr;
        bool bOwnerStillHere = false;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            if ((PS->HasbIsABot() && PS->bIsABot) || NPCs::IsNPC(PS))
                continue;

            auto Owner = (AActor*)PS->Owner;
            if (!Owner)
                continue;

            if (!First)
                First = (void*)Owner;

            if (GOwnerController == (void*)Owner)
                bOwnerStillHere = true;
        }

        if (GOwnerController && bOwnerStillHere)
            return;

        if (GOwnerController != First)
        {
            GOwnerController = First;

            if (GOwnerController)
                LogLine("[OWNER] The server owner is the first player who joined.");
        }
    }

    static bool bBotSpawningPaused = false;

    static bool bBusClearedForLaunch = false;

    void ClearBusForLaunch(const char* Why)
    {
        if (bBusClearedForLaunch)
            return;

        bBusClearedForLaunch = true;

        LogLine("[BUS] Cleared for launch (%s).", Why ? Why : "no reason given");
    }

    bool BusIsClearedForLaunch()
    {
        return bBusClearedForLaunch;
    }

    void EnsureWeaponHotfixes()
    {
        ApplyWeaponHotfixes();
    }

    void SetBotSpawningPaused(bool bPaused)
    {
        if (bBotSpawningPaused == bPaused)
            return;

        bBotSpawningPaused = bPaused;

        if (bPaused)
        {

            bBusClearedForLaunch = false;
            BusCountdownStart = -1.f;
            LastBusSecondAnnounced = -1;
        }

        LogLine("[BotAI] Bot spawning %s.", bPaused ? "paused -- the bus is held on the ground too" : "resumed");
    }

    bool AreBotSpawnsPaused()
    {
        return bBotSpawningPaused;
    }

    void HoldTheWarmupClock(AFortGameStateAthena* GameState, AFortGameMode* GameMode, float Now)
    {
        float HeldOpen = Now + 600.f;

        if (GameState)
        {
            if (GameState->HasWarmupCountdownStartTime())
                GameState->WarmupCountdownStartTime = Now;
            if (GameState->HasWarmupCountdownEndTime())
                GameState->WarmupCountdownEndTime = HeldOpen;
        }

        if (GameMode && GameMode->HasWarmupEarlyCountdownDuration())
            GameMode->WarmupEarlyCountdownDuration = HeldOpen;

        static int Reported = 0;
        if (++Reported <= 3)
            LogLine("[BUS] Warmup clock held open -- the bus waits for %d players.",
                    FConfiguration::AutoFillTargetPlayerCount);
    }

    bool BusMayLaunch()
    {
        if (bBotSpawningPaused)
            return false;

        const int Target = FConfiguration::AutoFillTargetPlayerCount;

        if (Target <= 0)
            return true;

        auto World = UWorld::GetWorld();

        if (!World || !World->AuthorityGameMode)
            return false;

        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;

        if (!GameMode->HasAlivePlayers())
            return true;

        return GameMode->AlivePlayers.Num() >= Target;
    }

    static float GBusCheckUntil = -1.f;
    static float GBusLaunchAt = -1.f;
    static int GBusPhaseBefore = -1;
    static bool GBusTriedCommand = false;
    static const char* GBusRoute = "";

    static int GamePhaseOf(AFortGameStateAthena* GameState)
    {
        return (GameState && GameState->HasGamePhase()) ? (int)GameState->GamePhase : -1;
    }

    static AFortPlayerControllerAthena* ConnectedPlayerController(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return nullptr;

        static int32 PlayerOffset = -2;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || (PS->HasbIsABot() && PS->bIsABot))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PS->Owner;
            if (!Controller)
                continue;

            if (PlayerOffset == -2)
            {
                const uint32 Off = ((UObject*)Controller)->GetOffset("Player");
                PlayerOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
            }

            if (PlayerOffset < 0 || GetFromOffset<UObject*>(Controller, (uint32)PlayerOffset))
                return Controller;
        }

        return nullptr;
    }

    static bool SendStartAircraftCommand(UWorld* World, AFortGameStateAthena* GameState, AFortPlayerControllerAthena* Through)
    {
        auto Controller = Through ? Through : ConnectedPlayerController(GameState);

        if (!Controller)
            return false;

        UKismetSystemLibrary::ExecuteConsoleCommand(World, FString(L"startaircraft"), Controller);
        return true;
    }

    void LaunchTheBus(const char* Why, AFortPlayerControllerAthena* Through)
    {
        ClearBusForLaunch(Why);

        auto World = UWorld::GetWorld();
        if (!World || !World->AuthorityGameMode || !World->GameState)
            return;

        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        auto GameState = (AFortGameStateAthena*)World->GameState;
        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        if (GamePhaseOf(GameState) >= 3)
        {
            LogLine("[BUS] Asked to start the bus (%s), but it has already left (game phase %d) -- not started again.",
                    Why ? Why : "no reason given", GamePhaseOf(GameState));
            return;
        }

        if (!bBusDeparted)
        {
            bBusDeparted = true;
            BusStartedTime = Now;
        }

        GBusPhaseBefore = GamePhaseOf(GameState);
        GBusLaunchAt = Now;
        GBusCheckUntil = Now + 5.f;
        GBusTriedCommand = false;

        bool bSent = true;

        if (AFortGameMode::StartAircraftPhaseOG)
        {
            GBusRoute = "the game's own StartAircraftPhase";

            AFortGameMode::StartAircraftPhase(GameMode, 0);
        }
        else
        {
            GBusRoute = "the startaircraft console command";
            GBusTriedCommand = true;
            bSent = SendStartAircraftCommand(World, GameState, Through);
        }

        LogLine("[BUS] Started with %s (%s) -- game phase %d before, %d right after (2 is the warmup, 3 the aircraft phase).%s",
                GBusRoute, Why ? Why : "no reason given", GBusPhaseBefore, GamePhaseOf(GameState),
                bSent ? "" : " There was no player with a connection to send the command through.");
    }

    static std::atomic<const char*> GBusLaunchRequest{ nullptr };

    void RequestBusLaunch(const char* Why)
    {
        GBusLaunchRequest.store(Why ? Why : "no reason given");
    }

    static void CheckTheBusLeft(UWorld* World, AFortGameStateAthena* GameState, float Now)
    {
        if (GBusCheckUntil < 0.f)
            return;

        const int Phase = GamePhaseOf(GameState);

        if (Phase < 0)
        {
            LogLine("[BUS] This build's game state has no GamePhase -- whether the bus really left cannot be checked.");
            GBusCheckUntil = -1.f;
            return;
        }

        if (Phase >= 3)
        {
            LogLine("[BUS] The bus is in the air: game phase %d, %.1fs after it was started (%s).", Phase, Now - GBusLaunchAt, GBusRoute);
            GBusCheckUntil = -1.f;
            return;
        }

        if (Now < GBusCheckUntil)
            return;

        if (!GBusTriedCommand)
        {
            GBusTriedCommand = true;

            const bool bSent = SendStartAircraftCommand(World, GameState, nullptr);

            LogLine("[BUS] %.0fs after %s the game is still in phase %d -- %s.", Now - GBusLaunchAt, GBusRoute, Phase,
                    bSent ? "sent the startaircraft console command as well, through a player with a connection"
                          : "and there is no player with a connection to send the startaircraft console command through");

            if (bSent)
            {
                GBusRoute = "the startaircraft console command, sent through a player with a connection";
                GBusCheckUntil = Now + 5.f;
                return;
            }
        }

        LogLine("[BUS] The bus did NOT leave: the game is still in phase %d, %.0fs after it was started (phase %d before; last tried: "
                "%s).", Phase, Now - GBusLaunchAt, GBusPhaseBefore, GBusRoute);
        GBusCheckUntil = -1.f;
    }

    bool MatchHasStarted()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return true;

        return !PlayersAreProtected((AFortGameStateAthena*)World->GameState);
    }

    static void WipeTheSlateAtBusTime(AFortGameStateAthena* GameState);

    static std::map<void*, std::pair<void*, int>> ShotAtBy;

    static std::map<void*, std::pair<void*, int>> AggroedByHuman;

    static std::map<void*, int> BotUnderFireSince;
    static std::set<void*> BotAlreadyBuiltWall;

    static int GElimsFromLedger = 0;

    static std::map<void*, FVector> MovementProbe;

    static int LandingLogCount = 0;

    static std::map<void*, std::string> AssignedBotNames;

    static std::set<void*> EliminatedBots;

    static std::set<void*> SeenAliveBots;

    static std::map<void*, float> BotBeingFixedUntil;

    static bool BotIsBeingFixed(void* Controller)
    {
        if (!Controller || BotBeingFixedUntil.empty())
            return false;

        auto It = BotBeingFixedUntil.find(Controller);
        if (It == BotBeingFixedUntil.end())
            return false;

        auto World = UWorld::GetWorld();
        if (World && (float)UGameplayStatics::GetTimeSeconds(World) <= It->second)
            return true;

        BotBeingFixedUntil.erase(It);
        return false;
    }

    static void HandleBotDeath(AFortPlayerControllerAthena* DeadBotController);

    static bool ActorIsLive(const UObject* Object);

    static bool bVictoryDeclared = false;

    static int PeakAlivePlayers = 0;
    static int PeakAliveBots = 0;

    static int VictoryConfirmTicks = 0;
    static constexpr int VictoryConfirmTicksRequired = 3;

    static int VictoryResendTicksLeft = 0;
    static uint8 VictoryDeathCause = 0;
    static constexpr int VictoryResendDurationTicks = 450;
    static constexpr int VictoryResendEveryTicks = 30;

    struct FAliveSnapshot
    {
        int Total = 0;
        int Bots = 0;
        int Humans = 0;
    };

    static bool PlayerIsStillIn(AFortGameMode* GameMode, AFortPlayerStateAthena* PS, bool* OutOnTheBus = nullptr)
    {
        if (OutOnTheBus)
            *OutOnTheBus = false;

        if (!PS)
            return false;

        auto OwnerActor = (AActor*)PS->Owner;
        if (!OwnerActor)
            return false;

        auto Pw = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)OwnerActor)->Pawn;
        if (Pw)
            return Pw->GetHealth() > 0.f || BotIsBeingFixed((void*)OwnerActor);

        if (EliminatedBots.find((void*)OwnerActor) != EliminatedBots.end())
            return false;

        if (!GameMode || !GameMode->HasAlivePlayers())
            return false;

        for (int i = 0; i < GameMode->AlivePlayers.Num(); i++)
        {
            if ((void*)GameMode->AlivePlayers[i] == (void*)OwnerActor)
            {
                if (OutOnTheBus)
                    *OutOnTheBus = true;

                return true;
            }
        }

        return false;
    }

    static AFortGameMode* CurrentGameMode()
    {
        auto World = UWorld::GetWorld();
        return World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
    }

    static FAliveSnapshot SnapshotAlive(AFortGameStateAthena* GameState,
                                        std::vector<AFortPlayerStateAthena*>* OutAlive,
                                        AFortPlayerStateAthena* Ignore)
    {
        FAliveSnapshot Snap{};

        if (OutAlive)
            OutAlive->clear();

        if (!GameState || !GameState->HasPlayerArray())
            return Snap;

        auto GameMode = CurrentGameMode();

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || PS == Ignore || NPCs::IsNPC(PS))
                continue;

            if (!PlayerIsStillIn(GameMode, PS))
                continue;

            Snap.Total++;
            if (PS->HasbIsABot() && PS->bIsABot)
                Snap.Bots++;
            else
                Snap.Humans++;

            if (OutAlive)
                OutAlive->push_back(PS);
        }

        return Snap;
    }

    static void InvokeNoArgs(const UObject* Object, const char* FunctionName)
    {
        if (!Object || !FunctionName)
            return;

        auto Fn = Object->GetFunction(FunctionName);
        if (!Fn)
            return;

        const int32 Size = Fn->GetPropertiesSize();

        if (Size <= 0 || Size > 0x1000)
        {
            Object->ProcessEvent(Fn, nullptr);
            return;
        }

        std::vector<uint8> Buffer((size_t)Size, 0);
        Object->ProcessEvent(Fn, Buffer.data());
    }

    static std::set<void*> WinAnnouncedTo;

    static void SendWinTo(AFortPlayerStateAthena* WinnerState, uint8 DeathCause, bool bAnnounceToClient)
    {
        if (!WinnerState)
            return;

        if (WinnerState->HasPlace())
        {
            WinnerState->Place = 1;
            InvokeNoArgs(WinnerState, "OnRep_Place");
        }

        auto OwnerActor = (AActor*)WinnerState->Owner;
        auto Controller = OwnerActor ? OwnerActor->Cast<AFortPlayerControllerAthena>() : nullptr;
        if (!Controller)
            return;

        if (bAnnounceToClient && WinAnnouncedTo.find((void*)WinnerState) == WinAnnouncedTo.end())
        {
            WinAnnouncedTo.insert((void*)WinnerState);

            auto WinnerPawn = Controller->Pawn;
            UObject* NoWeapon = nullptr;

            if (Controller->GetFunction("PlayWinEffects"))
                Controller->PlayWinEffects(WinnerPawn, NoWeapon, DeathCause, false);

            if (Controller->GetFunction("ClientNotifyWon"))
                Controller->ClientNotifyWon(WinnerPawn, NoWeapon, DeathCause);

            if (Controller->GetFunction("ClientNotifyTeamWon"))
                Controller->ClientNotifyTeamWon(WinnerPawn, NoWeapon, DeathCause);
        }

        InvokeNoArgs(Controller, "ForceNetUpdate");
    }

    static void DeclareVictory(const std::vector<AFortPlayerStateAthena*>& Winners, uint8 DeathCause,
                               const char* Reason)
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || Winners.empty())
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;

        bVictoryDeclared = true;
        VictoryDeathCause = DeathCause;

        AFortPlayerStateAthena* Headline = Winners[0];
        for (auto PS : Winners)
        {
            if (PS && !(PS->HasbIsABot() && PS->bIsABot))
            {
                Headline = PS;
                break;
            }
        }

        if (GameState->HasWinningTeam() && Headline->HasTeamIndex())
        {
            GameState->WinningTeam = (int32)Headline->TeamIndex;
            InvokeNoArgs(GameState, "OnRep_WinningTeam");
        }

        if (GameState->HasWinningPlayerState())
        {
            GameState->WinningPlayerState = Headline;
            InvokeNoArgs(GameState, "OnRep_WinningPlayerState");
        }

        for (auto PS : Winners)
            SendWinTo(PS, DeathCause,  true);

        if (GameState->HasPlayersLeft())
        {
            GameState->PlayersLeft = (int32)Winners.size();
            InvokeNoArgs(GameState, "OnRep_PlayersLeft");
        }
        if (GameState->HasTeamsLeft())
            GameState->TeamsLeft = 1;

        if (GameState->HasGamePhase() && (uint8)GameState->GamePhase < 5)
        {
            GameState->GamePhase = (uint8)5;
            InvokeNoArgs(GameState, "OnRep_GamePhase");
        }

        VictoryResendTicksLeft = VictoryResendDurationTicks;

        LogLine("[WIN] ================ VICTORY ROYALE ================");
        LogLine("[WIN] %d winner(s) -- reason: %s", (int)Winners.size(), Reason);
        for (auto PS : Winners)
        {
            if (!PS)
                continue;
            const std::string n = SafeNameOf(PS);
            LogLine("[WIN]   '%s' (team %d)%s", n.c_str(),
                    PS->HasTeamIndex() ? (int)PS->TeamIndex : -1,
                    (PS->HasbIsABot() && PS->bIsABot) ? "  [bot -- no client to notify]" : "");
        }
        LogLine("[WIN] Notification sent once. The won state is re-asserted for the next 15s in case a client is still catching up.");
    }

    static void TickVictoryResend()
    {
        if (VictoryResendTicksLeft <= 0)
            return;

        VictoryResendTicksLeft--;
        if ((VictoryResendTicksLeft % VictoryResendEveryTicks) != 0)
            return;

        auto World = UWorld::GetWorld();
        auto GameState = World ? (AFortGameStateAthena*)World->GameState : nullptr;
        if (!GameState || !GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasPlace() || PS->Place != 1)
                continue;

            SendWinTo(PS, VictoryDeathCause,  false);
        }
    }

    bool ShouldGrantVictoryTo(AFortPlayerStateAthena* CandidateState, AFortPlayerStateAthena* DyingVictimState)
    {
        if (bVictoryDeclared || !CandidateState || CandidateState == DyingVictimState)
            return false;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return false;

        auto GameState = (AFortGameStateAthena*)World->GameState;

        if (PlayersAreProtected(GameState))
            return false;

        if (PeakAlivePlayers < 2)
            return false;

        std::vector<AFortPlayerStateAthena*> Alive;
        auto Snap = SnapshotAlive(GameState, &Alive, DyingVictimState);

        const bool bLastStanding = (Snap.Total <= 1);
        const bool bAllBotsDead = (PeakAliveBots > 0 && Snap.Bots == 0);

        if (!bLastStanding && !bAllBotsDead)
            return false;

        if (std::find(Alive.begin(), Alive.end(), CandidateState) == Alive.end())
            Alive.push_back(CandidateState);

        DeclareVictory(Alive, 0, bLastStanding ? "final elimination -- one player left"
                                               : "final elimination -- all AI players dead");
        return true;
    }

    void CheckForVictory()
    {
        if (bVictoryDeclared)
        {
            TickVictoryResend();
            return;
        }

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;

        if (PlayersAreProtected(GameState))
        {
            VictoryConfirmTicks = 0;
            return;
        }

        static auto InProgressName = FName(L"InProgress");
        if (GameMode->HasMatchState() && !(GameMode->MatchState == InProgressName))
        {
            VictoryConfirmTicks = 0;
            return;
        }

        std::vector<AFortPlayerStateAthena*> Alive;
        auto Snap = SnapshotAlive(GameState, &Alive, nullptr);

        if (Snap.Total > PeakAlivePlayers)
            PeakAlivePlayers = Snap.Total;
        if (Snap.Bots > PeakAliveBots)
            PeakAliveBots = Snap.Bots;

        if (PeakAlivePlayers < 2 || Snap.Total == 0)
        {
            VictoryConfirmTicks = 0;
            return;
        }

        const char* Reason = nullptr;

        if (Snap.Total == 1)
        {
            Reason = "one player left standing";
        }
        else
        {
            bool bSameTeam = Alive[0] && Alive[0]->HasTeamIndex();
            if (bSameTeam)
            {
                const uint8 Team = (uint8)Alive[0]->TeamIndex;
                for (auto PS : Alive)
                {
                    if (!PS || !PS->HasTeamIndex() || (uint8)PS->TeamIndex != Team)
                    {
                        bSameTeam = false;
                        break;
                    }
                }
            }

            if (bSameTeam)
                Reason = "only one team left alive";
        }

        if (!Reason)
        {
            VictoryConfirmTicks = 0;
            return;
        }

        if (VictoryConfirmTicks == 0)
            LogLine("[WIN] Win condition met (%s) -- confirming over %d ticks before awarding...",
                    Reason, VictoryConfirmTicksRequired);

        if (++VictoryConfirmTicks < VictoryConfirmTicksRequired)
            return;

        DeclareVictory(Alive,  0, Reason);
    }

    static float LastThankTime = -1000.f;

    static std::map<void*, float> PendingParachuteOpen;

    static std::map<void*, AFortPickupAthena*> BotLootTargets;

    static std::map<void*, float> GlidingBots;
    static std::map<void*, int> GlidingSettledFor;

    class BFortPlayerPawn : public UObject
    {
    public:
        UCLASS_COMMON_MEMBERS(BFortPlayerPawn);

        DEFINE_FUNC(ForceOpenParachuteAndOverrideGlider, void);
        DEFINE_FUNC(OnRep_IsParachuteForcedOpen, void);
        DEFINE_BITFIELD_PROP(bIsParachuteForcedOpen);
    };

    static const wchar_t* BotGliderIds[] = {
        L"Umbrella_Season_11",
        L"Umbrella_BuildABrella",
        L"Umbrella_Silver",
        L"Glider_ID_015_Brite",
        L"Glider_ID_031_Metal",
        L"Glider_ID_080_PrairiePusher",
    };

    static std::vector<const UObject*> BotGliderChoices;
    static std::map<void*, const UObject*> BotGliderOf;

    static const UObject* PickBotGlider()
    {
        static bool bResolved = false;

        if (!bResolved)
        {
            bResolved = true;

            static auto GliderItemClass = FindClass("AthenaGliderItemDefinition");

            const int Listed = (int)(sizeof(BotGliderIds) / sizeof(BotGliderIds[0]));
            std::string Missing;

            for (int i = 0; i < Listed; i++)
            {
                std::wstring Wide(BotGliderIds[i]);
                std::wstring Path = L"/Game/Athena/Items/Cosmetics/Gliders/" + Wide + L"." + Wide;
                std::string Narrow(Wide.begin(), Wide.end());

                const UObject* Glider = FindObject(Path.c_str(), UObject::StaticClass());

                if (!Glider)
                    Glider = TUObjectArray::FindObject<UObject>(Narrow.c_str());

                if (Glider && GliderItemClass && !Glider->IsA(GliderItemClass))
                    Glider = nullptr;

                if (Glider)
                {
                    BotGliderChoices.push_back(Glider);
                }
                else
                {
                    if (!Missing.empty())
                        Missing += ", ";

                    Missing += Narrow;
                }
            }

            LogLine("[GLIDE] %d of the %d bot gliders are on this build%s%s.", (int)BotGliderChoices.size(), Listed,
                    Missing.empty() ? "" : " -- not found: ", Missing.c_str());

            if (GliderItemClass)
            {
                const size_t Named = BotGliderChoices.size();
                std::string Extra;

                for (int i = 0; i < TUObjectArray::Num(); i++)
                {
                    const UObject* Object = TUObjectArray::GetObjectByIndex(i);

                    if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA(GliderItemClass))
                        continue;

                    if (std::find(BotGliderChoices.begin(), BotGliderChoices.end(), Object) != BotGliderChoices.end())
                        continue;

                    BotGliderChoices.push_back(Object);

                    if (BotGliderChoices.size() - Named <= 12)
                    {
                        if (!Extra.empty())
                            Extra += ", ";

                        Extra += Object->Name.ToString().c_str();
                    }
                }

                LogLine("[GLIDE] %zu more glider(s) loaded on this server%s%s%s -- %zu to pick from.", BotGliderChoices.size() - Named,
                        Extra.empty() ? "" : ": ", Extra.c_str(), (BotGliderChoices.size() - Named) > 12 ? ", ..." : "", BotGliderChoices.size());
            }
        }

        if (BotGliderChoices.empty())
            return nullptr;

        return BotGliderChoices[(size_t)rand() % BotGliderChoices.size()];
    }

    struct FBotGlide
    {
        float JumpTime = 0.f;
        float JumpZ = 0.f;
        float OpenTime = -1.f;
        float OpenZ = 0.f;
        float LastDescentZ = 0.f;
        float LastDescentTime = 0.f;
        float NextReopenTime = 0.f;
        int Reopens = 0;
        bool bOpenLastTick = false;
    };

    static std::map<void*, FBotGlide> BotGlides;

    static int GGlideOpened = 0;
    static int GGlideOpenedByGame = 0;
    static int GGlideOpenedByFlag = 0;
    static int GGlideOpenFailed = 0;
    static int GGlideReopened = 0;
    static int GGlideLanded = 0;
    static int GGlideLandedGliding = 0;
    static bool bGliderForceOpenInFlight = false;
    static bool bGliderForceOpenBroken = false;

    static std::string BotPawnName(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasPlayerState() || !Pawn->PlayerState)
            return "a bot";

        const std::string Name = SafeNameOf((AFortPlayerStateAthena*)Pawn->PlayerState);
        return Name.empty() ? std::string("a bot") : Name;
    }

    static const char* OpenBotGlider(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasbIsParachuteOpen())
            return "";

        if (bGliderForceOpenInFlight)
        {
            bGliderForceOpenInFlight = false;
            bGliderForceOpenBroken = true;
            LogLine("[GLIDE] The game's force-open crashed on a bot -- gliders are opened with bIsParachuteOpen from now on.");
        }

        auto GliderIt = BotGliderOf.find((void*)Pawn);
        const UObject* Glider = (GliderIt != BotGliderOf.end()) ? GliderIt->second : nullptr;

        if (!bGliderForceOpenBroken && Glider && ((UObject*)Pawn)->GetFunction("ForceOpenParachuteAndOverrideGlider"))
        {
            const UObject* GliderArgument = Glider;
            bool bResetOverrideOnLanding = true;
            bool bIntoGliderRedeploy = false;

            bGliderForceOpenInFlight = true;
            ((BFortPlayerPawn*)Pawn)->ForceOpenParachuteAndOverrideGlider(GliderArgument, bResetOverrideOnLanding, bIntoGliderRedeploy);
            bGliderForceOpenInFlight = false;

            if (Pawn->bIsParachuteOpen)
                return "the game's force-open";
        }

        if (!bGliderForceOpenBroken && !((UObject*)Pawn)->GetFunction("ForceOpenParachuteAndOverrideGlider") &&
            ((UObject*)Pawn)->GetFunction("BP_ForceOpenParachute"))
        {
            bGliderForceOpenInFlight = true;
            InvokeNoArgs((UObject*)Pawn, "BP_ForceOpenParachute");
            bGliderForceOpenInFlight = false;

            if (Pawn->bIsParachuteOpen)
                return "the game's BP_ForceOpenParachute";
        }

        Pawn->bIsParachuteOpen = true;
        Pawn->OnRep_IsParachuteOpen(false);

        return Pawn->bIsParachuteOpen ? "bIsParachuteOpen" : "";
    }

    static int GliderActorOf(AFortPlayerPawnAthena* Pawn)
    {
        static int32 Offset = -2;

        if (Offset == -2 && Pawn)
        {
            const uint32 Off = ((UObject*)Pawn)->GetOffset("ParachuteAttachment");
            Offset = (Off == (uint32)-1) ? -1 : (int32)Off;
        }

        if (!Pawn || Offset < 0)
            return -1;

        return GetFromOffset<UObject*>(Pawn, (uint32)Offset) ? 1 : 0;
    }

    static void ForceClearSkydive(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn)
            return;

        if (Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen)
        {
            Pawn->bIsParachuteOpen = false;
            Pawn->OnRep_IsParachuteOpen(true);
        }

        auto Chute = (BFortPlayerPawn*)Pawn;
        if (Chute->HasbIsParachuteForcedOpen() && Chute->bIsParachuteForcedOpen)
        {
            Chute->bIsParachuteForcedOpen = false;
            Chute->OnRep_IsParachuteForcedOpen();
        }

        if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
        {
            Pawn->bIsSkydiving = false;

            bool bWasSkydiving = true;
            Pawn->OnRep_IsSkydiving(bWasSkydiving);
        }
        if (Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus)
            Pawn->bIsSkydivingFromBus = false;

        if (AFortPlayerPawnAthena::EndSkydivingOG)
            AFortPlayerPawnAthena::EndSkydiving(Pawn);
    }

    static int32 GLoadoutGliderOffset = -2;
    static int32 GPawnLoadoutOffset = -2;
    static int32 GControllerLoadoutOffset = -2;
    static int GGlidersPutInLoadout = 0;
    static int GGlidersAlreadyInLoadout = 0;

    static bool ResolveLoadoutGlider(AFortPlayerPawnAthena* Pawn)
    {
        if (GLoadoutGliderOffset == -2 && Pawn)
        {
            auto Struct = FindStruct("FortAthenaLoadout");
            const uint32 Off = Struct ? Struct->GetOffset("Glider") : (uint32)-1;
            GLoadoutGliderOffset = (Off == (uint32)-1 || Off > 0x200) ? -1 : (int32)Off;

            const char* PawnField = "CustomizationLoadout";
            uint32 PawnOff = ((UObject*)Pawn)->GetOffset(PawnField);

            if (PawnOff == (uint32)-1)
            {
                PawnField = "CosmeticLoadout";
                PawnOff = ((UObject*)Pawn)->GetOffset(PawnField);
            }

            GPawnLoadoutOffset = (PawnOff == (uint32)-1) ? -1 : (int32)PawnOff;

            LogLine("[GLIDE] The cosmetic loadout's glider: %s | the pawn's loadout: %s.",
                    GLoadoutGliderOffset >= 0 ? "found" : "NOT on this build", GPawnLoadoutOffset >= 0 ? PawnField : "NOT on this build");
        }

        return GLoadoutGliderOffset >= 0 && GPawnLoadoutOffset >= 0;
    }

    static const UObject* GliderInBotLoadout(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !ResolveLoadoutGlider(Pawn))
            return nullptr;

        return *(const UObject**)((uint8*)Pawn + GPawnLoadoutOffset + GLoadoutGliderOffset);
    }

    static const char* PutGliderInBotLoadout(AFortPlayerPawnAthena* Pawn, const UObject* Glider)
    {
        if (!Pawn || !Glider)
            return "no glider to put in";

        if (!ResolveLoadoutGlider(Pawn))
            return "this build's loadout has no glider";

        auto& PawnGlider = *(const UObject**)((uint8*)Pawn + GPawnLoadoutOffset + GLoadoutGliderOffset);
        const char* What = "already had one";

        if (!PawnGlider)
        {
            PawnGlider = Glider;
            GGlidersPutInLoadout++;
            What = "put in its loadout";
        }
        else
        {
            GGlidersAlreadyInLoadout++;
        }

        if (auto Controller = (UObject*)Pawn->Controller)
        {
            if (GControllerLoadoutOffset == -2)
            {
                uint32 Off = Controller->GetOffset("CustomizationLoadout");

                if (Off == (uint32)-1)
                    Off = Controller->GetOffset("CosmeticLoadoutPC");

                GControllerLoadoutOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
            }

            if (GControllerLoadoutOffset >= 0)
            {
                auto& ControllerGlider = *(const UObject**)((uint8*)Controller + GControllerLoadoutOffset + GLoadoutGliderOffset);

                if (!ControllerGlider)
                    ControllerGlider = PawnGlider;
            }
        }

        return What;
    }

    static bool GGlideSpeedRead = false;
    static float GGlideTerminal = 700.f;
    static int GGlideHeldTicks = 0;
    static double GGlideRateSum = 0.0;
    static int GGlideRateCount = 0;

    static float ReadAirTerminalVelocity(UObject* Move, const char* ParamsName)
    {
        static int32 TerminalOffset = -2;

        if (TerminalOffset == -2)
        {
            auto Struct = FindStruct("AirControlParams");
            const uint32 Off = Struct ? Struct->GetOffset("TerminalVelocity") : (uint32)-1;
            TerminalOffset = (Off == (uint32)-1 || Off > 0x200) ? -1 : (int32)Off;
        }

        const uint32 ParamsOffset = Move ? Move->GetOffset(ParamsName) : (uint32)-1;

        if (TerminalOffset < 0 || ParamsOffset == (uint32)-1 || ParamsOffset > 0x4000)
            return -1.f;

        auto Speed = (FScalableFloat*)((uint8*)Move + ParamsOffset + TerminalOffset);
        return Speed->Evaluate();
    }

    static float BotGlideTerminal(AFortPlayerPawnAthena* Pawn)
    {
        if (GGlideSpeedRead || !Pawn)
            return GGlideTerminal;

        GGlideSpeedRead = true;

        auto Move = (Pawn->HasCharacterMovement() && Pawn->CharacterMovement) ? (UObject*)Pawn->CharacterMovement : nullptr;

        const float Skydive = ReadAirTerminalVelocity(Move, "SkydivingControlParamsPassive");
        const float Diving = ReadAirTerminalVelocity(Move, "SkydivingControlParamsActive");
        const float Glider = ReadAirTerminalVelocity(Move, "ParachuteControlParamsSkydive");
        const float GliderFromJump = ReadAirTerminalVelocity(Move, "ParachuteControlParamsJump");

        const bool bUsable = Glider >= 150.f && Glider <= 1500.f;

        if (bUsable)
            GGlideTerminal = Glider;

        LogLine("[GLIDE] This build's own fall speeds, units a second: skydiving %.0f (diving %.0f), under the glider %.0f (opened from a "
                "jump %.0f) -- bots come down under their glider at no more than %.0f a second (%s).",
                Skydive, Diving, Glider, GliderFromJump, GGlideTerminal,
                bUsable ? "the game's own glider speed" : "the game's own could not be read, so the scripted glide's speed");

        return GGlideTerminal;
    }

    static void HoldBotGlideDescent(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasbIsParachuteOpen() || !Pawn->bIsParachuteOpen)
            return;

        auto GlideIt = BotGlides.find((void*)Pawn);
        if (GlideIt == BotGlides.end() || GlideIt->second.OpenTime < 0.f)
            return;

        auto Move = (Pawn->HasCharacterMovement() && Pawn->CharacterMovement) ? Pawn->CharacterMovement : nullptr;
        if (!Move || !Move->HasVelocity())
            return;

        const double Limit = -(double)BotGlideTerminal(Pawn);
        FVector Velocity = Move->Velocity;

        if ((double)Velocity.Z < Limit)
        {
            Velocity.Z = Limit;
            Move->Velocity = Velocity;
            GGlideHeldTicks++;
        }
    }

    void TickLandings(UWorld* World)
    {

        if (World && World->GameState)
        {
            auto GS = (AFortGameStateAthena*)World->GameState;
            if (GS->HasPlayerArray())
            {
                for (int i = 0; i < GS->PlayerArray.Num(); i++)
                {
                    auto PS = (AFortPlayerStateAthena*)GS->PlayerArray[i];
                    if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                        continue;
                    auto Ctrl = (AFortPlayerControllerAthena*)PS->Owner;
                    if (!Ctrl)
                        continue;
                    auto Pawn = (AFortPlayerPawnAthena*)Ctrl->Pawn;
                    if (!Pawn || Pawn->GetHealth() <= 0.f)
                        continue;

                    if (GlidingBots.find((void*)Pawn) != GlidingBots.end())
                        continue;

                    bool bWalking = false;
                    if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement &&
                        Pawn->CharacterMovement->HasMovementMode())
                    {
                        const uint8 Mode = Pawn->CharacterMovement->MovementMode;

                        if (Mode == 1 || Mode == 2)
                            bWalking = true;
                    }

                    const bool bSkydiveStuck =
                        (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) ||
                        (Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus) ||
                        (Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen);

                    if (bSkydiveStuck && bWalking)
                    {
                        ForceClearSkydive(Pawn);
                        GlidingBots.erase((void*)Pawn);
                        GlidingSettledFor.erase((void*)Pawn);
                        MovedToIsland.insert((void*)Pawn);
                    }
                }
            }
        }

        static std::map<void*, std::pair<float, float>> HumanGlideStart;
        static std::set<void*> HumanGlideMeasured;
        static int HumanGlideLines = 0;

        if (World && World->GameState && HumanGlideLines < 4)
        {
            auto GS = (AFortGameStateAthena*)World->GameState;
            const float NowHuman = (float)UGameplayStatics::GetTimeSeconds(World);

            for (int i = 0; GS->HasPlayerArray() && i < GS->PlayerArray.Num(); i++)
            {
                auto PS = (AFortPlayerStateAthena*)GS->PlayerArray[i];
                if (!PS || (PS->HasbIsABot() && PS->bIsABot) || NPCs::IsNPC(PS) || !PS->Owner)
                    continue;

                auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)PS->Owner)->Pawn;
                if (!Pawn || HumanGlideMeasured.find((void*)Pawn) != HumanGlideMeasured.end())
                    continue;

                const bool bOpen = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;
                auto Start = HumanGlideStart.find((void*)Pawn);

                if (Start == HumanGlideStart.end())
                {
                    if (bOpen)
                        HumanGlideStart[(void*)Pawn] = { (float)Pawn->K2_GetActorLocation().Z, NowHuman };

                    continue;
                }

                const float Flown = NowHuman - Start->second.second;

                if (bOpen && Flown < 6.f)
                    continue;

                HumanGlideMeasured.insert((void*)Pawn);

                if (Flown >= 1.f)
                {
                    const float Z = (float)Pawn->K2_GetActorLocation().Z;
                    HumanGlideLines++;

                    LogLine("[GLIDE] '%s' (a real player) came down %.0f a second under their glider (%.0f units in %.1fs from Z=%.0f) -- "
                            "this build's own glider, to hold the bots' glide up against.",
                            SafeNameOf(PS).c_str(), (Start->second.first - Z) / Flown, Start->second.first - Z, Flown, Start->second.first);
                }
            }
        }

        if (GlidingBots.empty())
            return;

        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        for (auto it = GlidingBots.begin(); it != GlidingBots.end();)
        {
            auto Pawn = (AFortPlayerPawnAthena*)it->first;

            if (!Pawn)
            {
                GlidingSettledFor.erase(it->first);
                BotGlides.erase(it->first);
                it = GlidingBots.erase(it);
                continue;
            }

            const float Height = (float)Pawn->K2_GetActorLocation().Z;
            FBotGlide& Glide = BotGlides[it->first];

            if (Glide.LastDescentTime <= 0.f)
            {
                Glide.LastDescentZ = Height;
                Glide.LastDescentTime = Now;

                if (Glide.JumpTime <= 0.f)
                {
                    Glide.JumpTime = Now;
                    Glide.JumpZ = Height;
                }
            }

            const bool bGliderOpen = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;

            bool bDown = false;
            const char* Why = "";

            if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement &&
                Pawn->CharacterMovement->HasMovementMode())
            {
                const uint8 Mode = Pawn->CharacterMovement->MovementMode;

                if (Mode == 1 || Mode == 2)
                {
                    bDown = true;
                    Why = "on the ground";
                }
                else if (Mode == 4)
                {
                    bDown = true;
                    Why = "in the water";
                }
            }

            if (Height < Glide.LastDescentZ - 20.f)
            {
                Glide.LastDescentZ = Height;
                Glide.LastDescentTime = Now;
            }
            else if (!bDown && (Now - Glide.LastDescentTime) > 1.5f)
            {
                bDown = true;
                Why = "stopped going down";
            }

            if (!bDown && (Now - Glide.JumpTime) > 240.f)
            {
                bDown = true;
                Why = "four minutes without landing";
            }

            it->second = Height;

            if (!bDown)
            {

                if (Glide.OpenTime >= 0.f && !bGliderOpen && Now >= Glide.NextReopenTime)
                {
                    Glide.NextReopenTime = Now + 0.5f;

                    Pawn->bIsParachuteOpen = true;
                    Pawn->OnRep_IsParachuteOpen(false);

                    Glide.Reopens++;
                    GGlideReopened++;

                    if (GGlideReopened <= 6)
                        LogLine("[GLIDE] %s's glider was closed in the air at Z=%.0f -- opened it again (now %s).",
                                BotPawnName(Pawn).c_str(), Height, Pawn->bIsParachuteOpen ? "open" : "STILL CLOSED");
                }

                Glide.bOpenLastTick = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;

                ++it;
                continue;
            }

            const bool bWasGliding = bGliderOpen || Glide.bOpenLastTick;

            GGlideLanded++;

            if (bWasGliding)
                GGlideLandedGliding++;

            const float Flown = (Glide.OpenTime >= 0.f) ? Now - Glide.OpenTime : 0.f;
            const float GlideRate = (Flown > 0.5f) ? (Glide.OpenZ - Height) / Flown : -1.f;

            if (GlideRate > 0.f)
            {
                GGlideRateSum += (double)GlideRate;
                GGlideRateCount++;
            }

            if (GGlideLanded <= 8)
            {
                char Glided[160] = "";

                if (Glide.OpenTime >= 0.f)
                    snprintf(Glided, sizeof(Glided), ", glider opened at Z=%.0f and flown for %.1fs, coming down %.0f a second under it",
                             Glide.OpenZ, Flown, GlideRate);

                LogLine("[GLIDE] '%s' touched down (%s) at Z=%.0f, %.1fs after leaving the bus -- %s%s%s.", BotPawnName(Pawn).c_str(), Why,
                        Height, Now - Glide.JumpTime, bWasGliding ? "still under its glider" : "NOT under a glider", Glided,
                        Glide.Reopens > 0 ? " (it had to be re-opened in the air)" : "");
            }
            else if ((GGlideLanded % 20) == 0)
            {
                LogLine("[GLIDE] %d bot(s) down from the bus: %d still under the glider when they touched down, %d not.", GGlideLanded,
                        GGlideLandedGliding, GGlideLanded - GGlideLandedGliding);
            }

            ForceClearSkydive(Pawn);
            MovedToIsland.insert((void*)Pawn);

            GlidingSettledFor.erase(it->first);
            BotGlides.erase(it->first);
            BotGliderOf.erase(it->first);
            it = GlidingBots.erase(it);
        }

        static bool bSaidEveryoneDown = false;

        if (GlidingBots.empty() && GGlideLanded > 0 && !bSaidEveryoneDown)
        {
            bSaidEveryoneDown = true;
            LogLine("[GLIDE] Every bot is down from the bus: %d of %d were still under the glider when they touched down | gliders opened: "
                    "%d (%d with the game's force-open and the bot's own glider, %d with bIsParachuteOpen), %d could not be opened, "
                    "%d re-opened after closing in the air.",
                    GGlideLandedGliding, GGlideLanded, GGlideOpened, GGlideOpenedByGame, GGlideOpenedByFlag, GGlideOpenFailed, GGlideReopened);

            LogLine("[GLIDE] Under their gliders they came down %.0f a second on average (%d bot(s) measured; the game's own glider: %.0f) | "
                    "ticks a bot was coming down faster than a glider and was held to it: %d | gliders put in a bot's cosmetic loadout: %d "
                    "(it had one already: %d).",
                    GGlideRateCount > 0 ? GGlideRateSum / (double)GGlideRateCount : -1.0, GGlideRateCount, GGlideTerminal, GGlideHeldTicks,
                    GGlidersPutInLoadout, GGlidersAlreadyInLoadout);
        }
    }

    void TickParachutes(UWorld* World)
    {
        if (PendingParachuteOpen.empty())
            return;

        float Now = (float)UGameplayStatics::GetTimeSeconds(World);
        for (auto it = PendingParachuteOpen.begin(); it != PendingParachuteOpen.end();)
        {
            if (Now < it->second)
            {
                ++it;
                continue;
            }

            auto Pawn = (AFortPlayerPawnAthena*)it->first;
            it = PendingParachuteOpen.erase(it);

            if (!Pawn || GlidingBots.find((void*)Pawn) == GlidingBots.end())
                continue;

            const char* How = OpenBotGlider(Pawn);
            const float Z = (float)Pawn->K2_GetActorLocation().Z;

            FBotGlide& Glide = BotGlides[(void*)Pawn];

            if (*How)
            {
                Glide.OpenTime = Now;
                Glide.OpenZ = Z;

                GGlideOpened++;

                if (How[0] == 't')
                    GGlideOpenedByGame++;
                else
                    GGlideOpenedByFlag++;
            }
            else
            {
                GGlideOpenFailed++;
            }

            if ((GGlideOpened + GGlideOpenFailed) <= 10)
            {
                auto GliderIt = BotGliderOf.find((void*)Pawn);
                const UObject* Glider = (GliderIt != BotGliderOf.end()) ? GliderIt->second : nullptr;

                const UObject* InLoadout = GliderInBotLoadout(Pawn);

                const int GliderActor = GliderActorOf(Pawn);

                LogLine("[GLIDE] '%s' %s its glider (%s) at Z=%.0f, %.1fs after leaving the bus%s%s | its loadout's glider: %s | skydiving: %s, "
                        "glider open: %s%s | coming down %.0f a second.",
                        BotPawnName(Pawn).c_str(), *How ? "opened" : "could NOT open", Glider ? Glider->Name.ToString().c_str() : "the default one",
                        Z, Glide.JumpTime > 0.f ? Now - Glide.JumpTime : 0.f, *How ? " -- " : "", How,
                        InLoadout ? InLoadout->Name.ToString().c_str() : "NONE",
                        (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) ? "yes" : "no",
                        (Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen) ? "yes" : "no",
                        GliderActor < 0 ? "" : (GliderActor ? ", glider actor everybody's game draws: there" : ", glider actor everybody's game draws: NONE"),
                        (Pawn->HasCharacterMovement() && Pawn->CharacterMovement && Pawn->CharacterMovement->HasVelocity())
                            ? (float)-Pawn->CharacterMovement->Velocity.Z : 0.f);
            }
        }
    }

    static uint32 BotPersonalSeed(void* Pawn)
    {
        uint64 Value = (uint64)Pawn;

        Value ^= Value >> 33;
        Value *= 0xFF51AFD7ED558CCDull;
        Value ^= Value >> 33;

        return (uint32)Value;
    }

    static bool BotIsLookingAt(AFortPlayerPawnAthena* Me, const FVector& MyLoc, const FVector& ThemLoc)
    {
        const double dx = ThemLoc.X - MyLoc.X;
        const double dy = ThemLoc.Y - MyLoc.Y;
        const double Flat = sqrt(dx * dx + dy * dy);

        if (Flat <= (double)FConfiguration::BotHearingRange)
            return true;

        if (Flat < 1.0)
            return true;

        FVector Facing = Me->GetActorForwardVector();

        const double FacingLength = sqrt(Facing.X * Facing.X + Facing.Y * Facing.Y);

        if (FacingLength < 0.001)
            return true;

        const double Dot = ((Facing.X / FacingLength) * (dx / Flat)) + ((Facing.Y / FacingLength) * (dy / Flat));

        double HalfAngle = (double)FConfiguration::BotFieldOfView * 0.5;

        if (!(HalfAngle > 5.0) || HalfAngle > 180.0)
            HalfAngle = 60.0;

        return Dot >= cos(HalfAngle * 3.14159265358979323846 / 180.0);
    }

    static bool BotTraceAvailable();
    static bool BotStraightLineSees(AFortPlayerPawnAthena* Me, AActor* Them, std::string* WhatIsInTheWay);

    static int GSightByLineClear = 0;
    static int GSightByLineBlocked = 0;
    static std::string GSightFirstBlocker;

    static bool BotHasLineOfSight(AAIController* AIController, AActor* Them)
    {
        static int Available = -1;

        if (Available == -1)
        {
            Available = (AIController && AIController->GetFunction("LineOfSightTo")) ? 1 : 0;

            LogLine("[EYES] The game's own line-of-sight check %s -- bots %s see through walls.",
                    Available ? "is available" : "is NOT on this build",
                    Available ? "cannot" : "can still");
        }

        if (AIController && Them && IsPlayerControllerBot((AActor*)AIController))
        {
            auto MyPawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)AIController)->Pawn;

            if (MyPawn && BotTraceAvailable())
            {
                std::string Blocker;
                const bool bSees = BotStraightLineSees(MyPawn, Them, &Blocker);

                if (bSees)
                {
                    GSightByLineClear++;
                }
                else
                {
                    GSightByLineBlocked++;

                    if (GSightFirstBlocker.empty())
                        GSightFirstBlocker = Blocker;
                }

                static int Compared = 0;

                if (Available == 1 && Compared < 6)
                {
                    Compared++;

                    FVector ViewPoint = MyPawn->K2_GetActorLocation();
                    ViewPoint.Z = ViewPoint.Z + 60.0;

                    const bool bGameSays = AIController->LineOfSightTo(Them, ViewPoint, false);
                    auto MyState = (AFortPlayerStateAthena*)((AFortPlayerControllerAthena*)AIController)->PlayerState;

                    LogLine("[EYES] Sample %d: the straight line from '%s's eyes to '%s' %s%s -- the game's own check says \"%s\" "
                            "(the straight line decides).",
                            Compared, MyState ? SafeNameOf(MyState).c_str() : "a bot", BotPawnName((AFortPlayerPawnAthena*)Them).c_str(),
                            bSees ? "is clear" : "runs into ", bSees ? "" : Blocker.c_str(),
                            bGameSays ? "can see" : "cannot see");
                }

                return bSees;
            }
        }

        if (Available != 1 || !AIController || !Them)
            return true;

        FVector ViewPoint{};

        if (IsPlayerControllerBot((AActor*)AIController))
        {
            auto MyPawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)AIController)->Pawn;

            if (MyPawn)
            {
                ViewPoint = MyPawn->K2_GetActorLocation();
                ViewPoint.Z = ViewPoint.Z + 60.0;
            }
        }

        return AIController->LineOfSightTo(Them, ViewPoint, false);
    }

    static std::map<void*, std::pair<void*, int>> BotLastSaw;

    static bool BotCanSee(AAIController* AIController, AFortPlayerPawnAthena* Me, const FVector& MyLoc,
                          AFortPlayerPawnAthena* Them, AActor* ThemActor, int TickNow, bool bStrict = false)
    {
        if (!Me || !Them)
            return false;

        const FVector ThemLoc = Them->K2_GetActorLocation();

        const bool bInFront = BotIsLookingAt(Me, MyLoc, ThemLoc);
        const bool bClearLine = bInFront && BotHasLineOfSight(AIController, ThemActor);

        if (bInFront && bClearLine)
        {
            BotLastSaw[(void*)Me] = { (void*)Them, TickNow };
            return true;
        }

        if (bStrict)
            return false;

        auto Remembered = BotLastSaw.find((void*)Me);

        if (Remembered != BotLastSaw.end() && Remembered->second.first == (void*)Them)
        {
            int Memory = FConfiguration::BotMemoryTicks;

            if (Memory < 0)
                Memory = 0;

            if ((TickNow - Remembered->second.second) <= Memory)
                return true;

            BotLastSaw.erase(Remembered);
        }

        return false;
    }

    static bool BotHasLanded(void* Pawn)
    {
        if (!Pawn)
            return false;
        if (!bBusDeparted)
            return false;
        if (MovedToIsland.find(Pawn) == MovedToIsland.end())
            return false;
        if (GlidingBots.find(Pawn) != GlidingBots.end())
            return false;

        return true;
    }

    static bool BotCombatIsOpen()
    {
        if (!bBusDeparted)
            return false;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return false;

        auto GS = (AFortGameStateAthena*)World->GameState;
        if (PlayersAreProtected(GS))
            return false;

        if (GS->HasGamePhase() && GS->GamePhase < GamePhase_SafeZones)
            return false;

        return true;
    }

    static bool GWorldInitialized = false;

    void NoteWorldInitialized()
    {
        if (GWorldInitialized)
            return;

        GWorldInitialized = true;
        LogLine("[BotAI] The game finished setting up the world (FinishWorldInitialization) -- bots can be spawned from now on.");
    }

    static bool ClassIsChildOf(const UClass* Class, const UClass* Wanted)
    {
        if (!Class)
            return false;

        if (!Wanted)
            return Class->GetSuper() != nullptr;

        int Depth = 0;

        for (auto Struct = (const UStruct*)Class; Struct && Depth < 64; Struct = (const UStruct*)Struct->GetSuper(), Depth++)
        {
            if (Struct == (const UStruct*)Wanted)
                return true;
        }

        return false;
    }

    static bool ClassIsFortPlayerPawn(const UClass* Class)
    {
        return ClassIsChildOf(Class, AFortPlayerPawnAthena::StaticClass());
    }

    static const UClass* GBotPawnClass = nullptr;

    static const UClass* BotPawnClass(AFortGameMode* GameMode)
    {
        if (GBotPawnClass)
            return GBotPawnClass;

        const UClass* Chosen = nullptr;

        if (GameMode && ClassIsFortPlayerPawn(GameMode->DefaultPawnClass))
        {
            Chosen = GameMode->DefaultPawnClass;
        }
        else
        {
            auto Athena = FindObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");

            if (ClassIsFortPlayerPawn(Athena))
                Chosen = Athena;
        }

        if (!Chosen)
            return nullptr;

        GBotPawnClass = Chosen;

        LogLine("[BotAI] Bots get the pawn class %s, a fully set-up Fortnite player pawn%s.", Chosen->Name.ToString().c_str(),
                (GameMode && Chosen != GameMode->DefaultPawnClass) ? " (the game mode's own pawn class was not usable yet)" : "");

        return Chosen;
    }

    static bool WorldReadyForBots(AFortGameMode* GameMode, float Now)
    {
        static float FirstAsked = -1.f;
        static bool bSaidWaiting = false;
        static bool bSaidGaveUp = false;
        static bool bSaidNoClass = false;

        if (FirstAsked < 0.f)
            FirstAsked = Now;

        if (!GWorldInitialized && AFortGameMode::FinishWorldInitializationOG)
        {
            if (Now - FirstAsked < 90.f)
            {
                if (!bSaidWaiting)
                {
                    bSaidWaiting = true;
                    LogLine("[BotAI] Waiting for the game to finish setting up the world (FinishWorldInitialization) before the first bot -- "
                            "a bot spawned before it gets a pawn the game cannot use yet, and that crashed the server when you joined early.");
                }

                return false;
            }

            if (!bSaidGaveUp)
            {
                bSaidGaveUp = true;
                LogLine("[BotAI] The world was still not set up %.0fs after the bots were ready to go -- going ahead, but only with a pawn "
                        "class that is really usable.", Now - FirstAsked);
            }
        }

        if (!BotPawnClass(GameMode))
        {
            if (!bSaidNoClass)
            {
                bSaidNoClass = true;
                LogLine("[BotAI] No usable pawn class for bots yet (the game mode's is %s) -- the first bot waits for one.",
                        (GameMode && GameMode->DefaultPawnClass) ? GameMode->DefaultPawnClass->Name.ToString().c_str() : "none");
            }

            return false;
        }

        return true;
    }

    int SpawnBots(const FVector& Origin, int Count, float ScatterRadius, const std::wstring& SpawnerDataAssetPath)
    {
        (void)SpawnerDataAssetPath;
        if (Count > 100)
        {
            LogLine("[BotAI] Requested %d bots, capping at 100.\n", Count);
            Count = 100;
        }

        auto World = UWorld::GetWorld();
        if (!World)
        {
            LogLine("[BotAI] No world yet, can't spawn bots.\n");
            return 0;
        }

        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        auto GameState = World->GameState ? (AFortGameStateAthena*)World->GameState : nullptr;
        if (!GameMode || !GameState)
        {
            LogLine("[BotAI] GameMode/GameState missing -- can't spawn bots.\n");
            return 0;
        }

        auto PawnClass = BotPawnClass(GameMode);

        if (!PawnClass)
        {
            static bool bSaidNoClass = false;

            if (!bSaidNoClass)
            {
                bSaidNoClass = true;
                LogLine("[BotAI] No usable pawn class for bots yet (the game mode's is %s) -- no bots are spawned until there is one.",
                        GameMode->DefaultPawnClass ? GameMode->DefaultPawnClass->Name.ToString().c_str() : "none");
            }

            return 0;
        }

        static auto PCClass = FindObject<UClass>(L"/Game/Athena/Athena_PlayerController.Athena_PlayerController_C");
        if (!PCClass)
            PCClass = FindClass("FortPlayerControllerAthena");
        if (!PCClass)
        {
            LogLine("[BotAI] Could not find Athena_PlayerController class.\n");
            return 0;
        }

        if (!ClassIsChildOf(PCClass, AFortPlayerControllerAthena::StaticClass()))
        {
            static bool bSaidPCClass = false;

            if (!bSaidPCClass)
            {
                bSaidPCClass = true;
                LogLine("[BotAI] The bots' player controller class (%s) is not fully set up yet -- no bots are spawned until it is.",
                        PCClass->Name.ToString().c_str());
            }

            return 0;
        }

        TArray<AActor*> PlayerStartsLocal;
        static auto WarmupStartClass = FindClass("FortPlayerStartWarmup");
        if (WarmupStartClass)
            UGameplayStatics::GetAllActorsOfClass(World, WarmupStartClass, &PlayerStartsLocal);

        static auto PlayerStartClass = FindClass("PlayerStart");
        if (PlayerStartsLocal.Num() == 0 && PlayerStartClass)
            UGameplayStatics::GetAllActorsOfClass(World, PlayerStartClass, &PlayerStartsLocal);

        int spawned = 0;
        for (int i = 0; i < Count; i++)
        {
            FVector SpawnLoc = Origin;
            if (PlayerStartsLocal.Num() > 0)
            {
                auto Start = PlayerStartsLocal[rand() % PlayerStartsLocal.Num()];
                if (Start)
                    SpawnLoc = Start->K2_GetActorLocation();
            }
            else if (ScatterRadius > 0.f)
            {
                SpawnLoc.X += ((float)(rand() % 2000) - 1000.f);
                SpawnLoc.Y += ((float)(rand() % 2000) - 1000.f);
            }

            FTransform Transform{};
            Transform.Translation = SpawnLoc;
            FVector OneScale(1.f, 1.f, 1.f);
            Transform.Scale3D = OneScale;

            auto Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(PawnClass, Transform);
            auto PC = (AFortPlayerControllerAthena*)UWorld::SpawnActor(PCClass, Transform);

            const bool bFortPawn = Pawn && Pawn->IsA(AFortPlayerPawnAthena::StaticClass());
            const bool bFortPC = PC && PC->IsA(AFortPlayerControllerAthena::StaticClass());

            if (!PC || !Pawn || !bFortPawn || !bFortPC)
            {
                if (Pawn)
                    Pawn->K2_DestroyActor();
                if (PC)
                    PC->K2_DestroyActor();
                LogLine("[BotAI] SpawnActor failed for bot %d (PC=%p Pawn=%p%s%s).\n", i, (void*)PC, (void*)Pawn,
                        (Pawn && !bFortPawn) ? ", and that pawn is not a Fortnite player pawn" : "",
                        (PC && !bFortPC) ? ", and that controller is not a Fortnite player controller" : "");

                if ((Pawn && !bFortPawn) || (PC && !bFortPC))
                {
                    GBotPawnClass = nullptr;
                    LogLine("[BotAI] No more bots this round -- they are tried again at the next fill.");
                    break;
                }

                continue;
            }

            auto PlayerState = PC->PlayerState ? PC->PlayerState->Cast<AFortPlayerStateAthena>() : nullptr;
            if (!PlayerState)
            {
                PlayerState = (AFortPlayerStateAthena*)UWorld::SpawnActor(AFortPlayerStateAthena::StaticClass(), Transform);
                if (PlayerState)
                {
                    PlayerState->SetOwner(PC);
                    PC->PlayerState = PlayerState;
                    PC->OnRep_PlayerState();
                }
            }

            if (!PlayerState)
            {
                Pawn->K2_DestroyActor();
                PC->K2_DestroyActor();
                LogLine("[BotAI] No PlayerState for bot %d.\n", i);
                continue;
            }

            PC->Possess(Pawn);
            if (PC->HasMyFortPawn())
                PC->MyFortPawn = Pawn;

            if (Pawn->PlayerState != PlayerState)
            {
                Pawn->PlayerState = PlayerState;
                Pawn->OnRep_PlayerState();
            }

            if (PlayerState->HasbIsABot())
                PlayerState->bIsABot = true;

            MarkBotReadyLikeAPlayer(PC, PlayerState, true);

            Pawn->SetMaxHealth(100.f);
            Pawn->SetHealth(100.f);

            Pawn->SetShield(0.f);
            if (Pawn->HasbAlwaysRelevant())
                Pawn->bAlwaysRelevant = true;

            PutGliderInBotLoadout(Pawn, PickBotGlider());

            if (!PC->WorldInventory)
            {
                PC->WorldInventory = (AFortInventory*)UWorld::SpawnActor(AFortInventory::StaticClass(), Transform);
                if (PC->WorldInventory)
                {
                    PC->WorldInventory->SetOwner(PC);
                    if (PC->WorldInventory->HasInventoryType())
                        PC->WorldInventory->InventoryType = 0;
                }
            }
            if (PC->HasbHasInitializedWorldInventory())
                PC->bHasInitializedWorldInventory = true;

            if (GameMode->HasStartingItems() && PC->WorldInventory)
            {
                for (int si = 0; si < GameMode->StartingItems.Num(); si++)
                {
                    auto& StartingItem = GameMode->StartingItems.Get(si, FItemAndCount::Size());
                    if (StartingItem.Count && StartingItem.Item)
                        PC->WorldInventory->GiveItem(StartingItem.Item, StartingItem.Count);
                }
            }

            const bool bLootpoolKit = PC->WorldInventory && GiveBotLootpoolKit(PC, PlayerState) > 0;

            static const wchar_t* KitPaths[] = {
                L"/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_SR_Ore_T03.WID_Assault_AutoHigh_Athena_SR_Ore_T03",
                L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_C_Ore_T03.WID_Shotgun_Standard_Athena_C_Ore_T03",
                L"/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03",
                L"/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff",
                L"/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff",
                L"/Game/Athena/Items/Consumables/Grenade/Athena_Grenade.Athena_Grenade",
            };
            static const wchar_t* KitShort[] = {
                L"WID_Assault_AutoHigh_Athena_SR_Ore_T03",
                L"WID_Shotgun_Standard_Athena_C_Ore_T03",
                L"WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03",
                L"Athena_PurpleStuff",
                L"Athena_PurpleStuff",
                L"Athena_Grenade",
            };
            if (PC->WorldInventory && !bLootpoolKit)
            {
                for (int ki = 0; ki < 6; ki++)
                {
                    auto Def = FindObject<UFortItemDefinition>(KitPaths[ki]);
                    if (!Def)
                        Def = FindObject<UFortItemDefinition>(KitShort[ki]);
                    if (!Def)
                    {
                        std::wstring wn(KitShort[ki]);
                        std::string nn(wn.begin(), wn.end());
                        Def = TUObjectArray::FindObject<UFortItemDefinition>(nn.c_str());
                    }
                    if (!Def)
                    {
                        std::wstring wn(KitShort[ki]);
                        std::string nn(wn.begin(), wn.end());
                        std::string dn = nn + "." + nn;
                        Def = TUObjectArray::FindObject<UFortItemDefinition>(dn.c_str());
                    }
                    if (Def)
                    {
                        int clip = 1;
                        if (auto W = Def->Cast<UFortWeaponItemDefinition>())
                        {
                            if (auto Stats = AFortInventory::GetStats(W))
                                if (Stats->ClipSize > 0)
                                    clip = Stats->ClipSize;
                            PC->WorldInventory->GiveItem(Def, 1, clip);

                            GiveBotSpareRounds(PC->WorldInventory, Def);
                        }
                        else
                            PC->WorldInventory->GiveItem(Def, 1);
                    }
                }

                NoteFixedBotKit(PlayerState, "the loot tables gave no gun");
            }

            if (GameState->HasPlayersLeft())
            {
                GameState->PlayersLeft++;
                GameState->OnRep_PlayersLeft();
            }
            if (GameMode->HasAlivePlayers())
                GameMode->AlivePlayers.Add(PC);

            if (Pawn->HasbAlwaysRelevant())
                Pawn->bAlwaysRelevant = true;

            LoadoutGiven.insert((void*)PlayerState);
            spawned++;
        }

        LogLine("[BotAI] Magnesium-style spawn: %d/%d bot(s) created.\n", spawned, Count);
        return spawned;
    }

    static bool GameJumpRaw(AFortPlayerControllerAthena* Controller)
    {
        FRotator Facing{};
        Controller->ServerAttemptAircraftJump(Facing);
        return true;
    }

    static bool GameJumpGuarded(AFortPlayerControllerAthena* Controller)
    {
        __try
        {
            return GameJumpRaw(Controller);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool GGameJumpBroken = false;
    static int GGameJumpTries = 0;
    static int GGameJumps = 0;
    static int GHandMadeBotPawns = 0;
    static int GThrownOutByBus = 0;
    static int GMarkedOffTheBus = 0;
    static int GNotMarkedOffTheBus = 0;
    static int GBrokenPawnsReplaced = 0;

    static float BusDropEndTime(AFortGameStateAthena* GameState)
    {
        if (!GameState)
            return -1.f;

        AFortAthenaAircraft* Aircraft = nullptr;

        if (GameState->HasAircrafts() && GameState->Aircrafts.Num() > 0)
            Aircraft = GameState->Aircrafts[0];
        if (!Aircraft && GameState->HasAircraft())
            Aircraft = GameState->Aircraft;

        if (!Aircraft || !Aircraft->HasDropEndTime())
            return -1.f;

        const float DropEnd = Aircraft->DropEndTime;
        return (DropEnd > 0.f) ? DropEnd : -1.f;
    }

    static float BusDropStartTime(AFortGameStateAthena* GameState)
    {
        if (!GameState)
            return -1.f;

        AFortAthenaAircraft* Aircraft = nullptr;

        if (GameState->HasAircrafts() && GameState->Aircrafts.Num() > 0)
            Aircraft = GameState->Aircrafts[0];
        if (!Aircraft && GameState->HasAircraft())
            Aircraft = GameState->Aircraft;

        if (!Aircraft || !Aircraft->HasDropStartTime())
            return -1.f;

        return Aircraft->DropStartTime;
    }

    static std::map<void*, void*> BotPawnHealthChecked;
    static int GFreshPawnsHealed = 0;

    static void GiveFreshBotPawnHealth(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, const char* Where)
    {
        if (!Controller || !Pawn)
            return;

        auto& Checked = BotPawnHealthChecked[(void*)Controller];
        if (Checked == (void*)Pawn)
            return;

        Checked = (void*)Pawn;

        const float Before = Pawn->GetHealth();

        if (Before > 0.f || ReadBoolByName((UObject*)Pawn, "bIsDying") == 1)
            return;

        Pawn->SetMaxHealth(100.f);
        Pawn->SetHealth(100.f);

        GFreshPawnsHealed++;

        if (GFreshPawnsHealed <= 3 || (GFreshPawnsHealed % 25) == 0)
        {
            auto PlayerState = (AFortPlayerStateAthena*)Controller->PlayerState;

            LogLine("[BUS] '%s' got a pawn with %.0f health (%s) -- given 100, the health a bot spawns with. Health read back: %.0f "
                    "(%d bot pawn(s) fixed so far).",
                    PlayerState ? SafeNameOf(PlayerState).c_str() : "a bot", Before, Where, Pawn->GetHealth(), GFreshPawnsHealed);
        }
    }

    static void GiveFreshBotPawnsHealth(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (Pawn)
                GiveFreshBotPawnHealth(Controller, Pawn, "a pawn the game made");
            else
                BotPawnHealthChecked.erase((void*)Controller);
        }
    }

    struct FGoodBotPawn
    {
        AFortPlayerPawnAthena* Pawn = nullptr;
        FWeakObjectPtr Weak{};
        float LastHealth = 0.f;
    };

    static std::map<void*, FGoodBotPawn> GoodBotPawns;

    static void RememberGoodBotPawn(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
    {
        if (!Controller || !Pawn)
            return;

        FGoodBotPawn& Good = GoodBotPawns[(void*)Controller];
        Good.Pawn = Pawn;
        Good.Weak = FWeakObjectPtr((const UObject*)Pawn);
        Good.LastHealth = Pawn->GetHealth();
    }

    static bool BotPawnStillExists(const FGoodBotPawn& Good)
    {
        if (!Good.Pawn || Good.Weak.Get() != (const UObject*)Good.Pawn)
            return false;

        if (VersionInfo.EngineVersion < 5.0)
        {

            auto Item = TUObjectArray::GetItemByIndex(Good.Weak.ObjectIndex);

            if (!Item || (Item->Flags & (0x20000000 | 0x10000000)))
                return false;
        }

        return true;
    }

    static bool BotPawnIsUsable(const FGoodBotPawn& Good)
    {
        if (!BotPawnStillExists(Good))
            return false;

        if (ReadBoolByName((UObject*)Good.Pawn, "bIsDying") == 1)
            return false;

        return Good.Pawn->GetHealth() > 0.f;
    }

    static std::vector<FVector> LandingPlan;
    static size_t LandingPlanCursor = 0;
    static bool bPlanBuilt = false;

    static int LandUnderSpot(AActor* Context, double X, double Y, double& OutZ, std::string& OutWhat);

    static bool GroundZUnder(AActor* Context, double X, double Y, double FromZ, double& OutZ);

    static void CheckLandUnderSpots(std::vector<FVector>& Plan, const FVector& Middle, AFortGameStateAthena* GameState)
    {
        if (Plan.empty() || !GameState)
            return;

        std::vector<int> Results(Plan.size(), -1);
        int Land = 0, NoLand = 0, Unknown = 0;
        std::string FirstNoLand;

        for (size_t i = 0; i < Plan.size(); i++)
        {
            double GroundZ = 0.0;
            std::string What;

            Results[i] = LandUnderSpot((AActor*)GameState, Plan[i].X, Plan[i].Y, GroundZ, What);

            if (Results[i] > 0)
                Land++;
            else if (Results[i] == 0)
            {
                NoLand++;

                if (FirstNoLand.empty())
                    FirstNoLand = What;
            }
            else
                Unknown++;
        }

        if (Unknown == (int)Plan.size())
        {
            LogLine("[LAND] The landing spots could NOT be checked for land under them (no line trace on this build) -- left as they are.");
            return;
        }

        if (NoLand * 3 > (int)Plan.size() * 2)
        {
            LogLine("[LAND] %d of %zu landing spot(s) seem to have no land under them (the first: %s) -- that many means the check "
                    "itself is not right on this build, so the plan is left exactly as it was.", NoLand, Plan.size(), FirstNoLand.c_str());
            return;
        }

        int PulledIn = 0, NeverFound = 0, Logged = 0;

        for (size_t i = 0; i < Plan.size(); i++)
        {
            if (Results[i] != 0)
                continue;

            FVector& Spot = Plan[i];
            const double StartX = Spot.X, StartY = Spot.Y;

            double GroundZ = 0.0;
            std::string FirstWhat, What;
            LandUnderSpot((AActor*)GameState, StartX, StartY, GroundZ, FirstWhat);

            bool bFound = false;

            for (int Step = 1; Step <= 8 && !bFound; Step++)
            {
                const double Keep = 1.0 - 0.12 * (double)Step;

                Spot.X = Middle.X + (StartX - Middle.X) * Keep;
                Spot.Y = Middle.Y + (StartY - Middle.Y) * Keep;

                bFound = LandUnderSpot((AActor*)GameState, Spot.X, Spot.Y, GroundZ, What) > 0;
            }

            if (bFound)
                PulledIn++;
            else
            {

                Spot.X = StartX;
                Spot.Y = StartY;
                NeverFound++;
            }

            if (++Logged <= 8)
                LogLine("[LAND] Spot X=%.0f Y=%.0f had %s under it -- %s.", (float)StartX, (float)StartY, FirstWhat.c_str(),
                        bFound ? ("pulled in towards the middle of the map to X=" + std::to_string((int)Spot.X) + " Y=" +
                                  std::to_string((int)Spot.Y) + " (" + What + " there)").c_str()
                               : "no land anywhere on the way in to the middle either: left where it was");
        }

        LogLine("[LAND] %zu landing spot(s) checked for land under them: %d had it, %d were pulled in towards the middle of the map "
                "until they did, %d never found any (left where they were)%s.", Plan.size(), Land, PulledIn, NeverFound,
                Unknown ? " -- some could NOT be checked" : "");
    }

    static void BuildLandingPlan(UWorld* World, AFortGameStateAthena* GameState)
    {
        if (bPlanBuilt || !World || !GameState)
            return;

        {
            bPlanBuilt = true;

            auto Normalize = [](std::string s) -> std::string
            {
                std::string out;
                for (char ch : s)
                {
                    if (ch == ' ' || ch == '\'')
                        continue;
                    out += (char)tolower((unsigned char)ch);
                }
                return out;
            };

            TArray<AActor*> PoiVolumesRaw;
            static auto PoiVolumeClass = FindClass("FortPoiVolume");
            if (PoiVolumeClass)
                UGameplayStatics::GetAllActorsOfClass(World, PoiVolumeClass, &PoiVolumesRaw);

            LogLine("[BotAI] Found %d POI volume(s) in the level.\n", PoiVolumesRaw.Num());

            struct FoundPoi { FVector Loc; std::vector<std::string> Tags; bool bUsed; };
            std::vector<FoundPoi> FoundPois;
            for (int i = 0; i < PoiVolumesRaw.Num(); i++)
            {
                auto Poi = (AFortPoiVolume*)PoiVolumesRaw[i];
                if (!Poi)
                    continue;

                FoundPoi fp;
                fp.Loc = Poi->K2_GetActorLocation();
                fp.bUsed = false;

                if (Poi->HasLocationTags())
                {
                    for (int t = 0; t < Poi->LocationTags.GameplayTags.Num(); t++)
                    {
                        auto& Tag = Poi->LocationTags.GameplayTags.Get(t, FGameplayTag::Size());
                        auto WideStr = Tag.TagName.ToString();
                        std::string TagStr(WideStr.begin(), WideStr.end());
                        fp.Tags.push_back(Normalize(TagStr));
                    }
                }
                FoundPois.push_back(fp);
            }

            std::vector<FoundPoi*> RealPois;
            for (auto& fp : FoundPois)
            {
                for (auto& tag : fp.Tags)
                {
                    if (tag.find("location.poi.") != std::string::npos)
                    {
                        RealPois.push_back(&fp);

                        LogLine("[BotAI] Real POI: %-40s at X=%.0f Y=%.0f Z=%.0f\n",
                               tag.c_str(), (float)fp.Loc.X, (float)fp.Loc.Y, (float)fp.Loc.Z);
                        break;
                    }
                }
            }

            LogLine("[BotAI] %zu real POI volume(s) identified out of %zu total volumes.\n",
                   RealPois.size(), FoundPois.size());

            std::vector<FVector> Plan;

            FVector BoundsOrigin{};
            FVector BoundsExtent{};
            bool bHaveBounds = false;

            if (GameState->HasMapInfo() && GameState->MapInfo && GameState->MapInfo->HasCachedPlayableBoundsForClients())
            {

                auto& B = GameState->MapInfo->CachedPlayableBoundsForClients;
                const uint8* Raw = (const uint8*)&B;
                const int32 VectorSize = FVector::Size();

                memcpy((void*)&BoundsOrigin, Raw, (size_t)VectorSize);
                memcpy((void*)&BoundsExtent, Raw + VectorSize, (size_t)VectorSize);

                const double Radius = VectorSize == 0xC ? (double)*(const float*)(Raw + 2 * VectorSize) : *(const double*)(Raw + 2 * VectorSize);

                if (BoundsExtent.X > 1000.0 && BoundsExtent.Y > 1000.0)
                    bHaveBounds = true;
                LogLine("[BotAI] Playable bounds: origin X=%.0f Y=%.0f Z=%.0f extent X=%.0f Y=%.0f Z=%.0f, sphere radius %.0f (usable=%s)\n",
                       (float)BoundsOrigin.X, (float)BoundsOrigin.Y, (float)BoundsOrigin.Z,
                       (float)BoundsExtent.X, (float)BoundsExtent.Y, (float)BoundsExtent.Z, (float)Radius, bHaveBounds ? "yes" : "no");
            }
            else
            {
                LogLine("[BotAI] MapInfo/CachedPlayableBoundsForClients unavailable.\n");
            }

            if (!bHaveBounds)
            {
                BoundsOrigin = FVector(0, 0, 0);
                BoundsExtent = FVector(112000, 112000, 0);
                LogLine("[BotAI] Using fallback playable bounds +/-112000 (derived from level overlay extents).\n");
            }

            {
                int total = FConfiguration::AutoFillTargetPlayerCount;
                if (total < 1) total = 100;

                int cols = (int)ceil(sqrt((double)total));
                if (cols < 1) cols = 1;
                int rows = (total + cols - 1) / cols;

                double usableX = BoundsExtent.X * 1.55;
                double usableY = BoundsExtent.Y * 1.55;
                double startX = BoundsOrigin.X - usableX * 0.5;
                double startY = BoundsOrigin.Y - usableY * 0.5;
                double cellW = usableX / (double)cols;
                double cellH = usableY / (double)rows;

                for (int i = 0; i < total; i++)
                {
                    int cx = i % cols;
                    int cy = i / cols;

                    double jx = ((double)(rand() % 1000) / 1000.0) * cellW * 0.85 + cellW * 0.075;
                    double jy = ((double)(rand() % 1000) / 1000.0) * cellH * 0.85 + cellH * 0.075;

                    FVector Spot{};
                    Spot.X = startX + cx * cellW + jx;
                    Spot.Y = startY + cy * cellH + jy;
                    Spot.Z = 0.0;
                    Plan.push_back(Spot);
                }

                LogLine("[BotAI] Built %zu widely-spread landing spots on a %dx%d grid (usable %.0fx%.0f).\n",
                       Plan.size(), cols, rows, (float)usableX, (float)usableY);
            }

            CheckLandUnderSpots(Plan, BoundsOrigin, GameState);

            {
                const char* Wanted = FConfiguration::HotDropPoi;
                int HowMany = FConfiguration::HotDropBots;

                if (HowMany > (int)Plan.size())
                    HowMany = (int)Plan.size();

                if (Wanted && *Wanted && HowMany > 0)
                {
                    FVector Middle{};
                    bool bFound = false;

                    const std::string WantedTag = Normalize(Wanted);

                    for (auto* Poi : RealPois)
                    {
                        for (auto& tag : Poi->Tags)
                        {
                            if (tag.find(WantedTag) == std::string::npos)
                                continue;

                            const double Away = sqrt((double)Poi->Loc.X * (double)Poi->Loc.X +
                                                     (double)Poi->Loc.Y * (double)Poi->Loc.Y);

                            if (Away < 1000.0)
                            {
                                LogLine("[BotAI] The level's \"%s\" volume says it is at the map origin, "
                                        "which cannot be right -- using the recording instead.", Wanted);
                                continue;
                            }

                            Middle = Poi->Loc;
                            bFound = true;
                            break;
                        }

                        if (bFound)
                            break;
                    }

                    if (bFound)
                        LogLine("[BotAI] Hot drop \"%s\" found as a POI volume at X=%.0f Y=%.0f.",
                                Wanted, (float)Middle.X, (float)Middle.Y);
                    else if (PlaceDump::GetLandmark(Wanted, Middle))
                    {
                        bFound = true;
                        LogLine("[BotAI] Hot drop \"%s\" taken from the recorded places at X=%.0f Y=%.0f.",
                                Wanted, (float)Middle.X, (float)Middle.Y);
                    }

                    if (!bFound)
                        LogLine("[BotAI] Hot drop \"%s\" is not a POI volume on this map and there is no "
                                "recording by that name -- everyone spreads out as usual.", Wanted);
                    else
                    {
                        std::vector<FVector> Spots((size_t)HowMany);

                        int Got = PlaceDump::GetPlacesNear(Middle, FConfiguration::HotDropRadius,
                                                           HowMany, Spots.data());

                        if (Got <= 0)
                        {
                            for (int i = 0; i < HowMany; i++)
                            {
                                const double Angle = ((double)(rand() % 3600) / 10.0) * 3.14159265358979323846 / 180.0;
                                const double Away = (double)FConfiguration::HotDropRadius * ((double)(rand() % 1000) / 1000.0);

                                Spots[(size_t)i] = Middle;
                                Spots[(size_t)i].X += cos(Angle) * Away;
                                Spots[(size_t)i].Y += sin(Angle) * Away;
                            }

                            Got = HowMany;
                        }

                        for (int i = 0; i < Got && i < (int)Plan.size(); i++)
                            Plan[(size_t)i] = Spots[(size_t)i];

                        LogLine("[BotAI] %d bot(s) are dropping on \"%s\"; the other %d spread out over the map.",
                                Got, Wanted, (int)Plan.size() - Got);
                    }
                }
            }

            for (size_t i = Plan.size(); i > 1; i--)
            {
                size_t j = (size_t)(rand() % (int)i);
                std::swap(Plan[i - 1], Plan[j]);
            }

            LandingPlan = Plan;
            LogLine("[BotAI] Landing plan built: %zu destinations.\n", LandingPlan.size());
        }
    }

    static double GBusDropZ = -1.0;

    static const double kDropShare = 0.255;
    static const double kDropFallbackZ = 20800.0;
    static const double kDropClearance = 5000.0;

    static double BusDropHeight(UWorld* World, AFortGameStateAthena* GameState)
    {
        if (GBusDropZ > 0.0)
            return GBusDropZ;

        double Z = -1.0;
        const char* From = "";

        auto GameMode = World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;

        if (GameMode && GameMode->HasAircrafts())
        {
            auto& Aircrafts = GameMode->Aircrafts;

            for (int i = 0; i < Aircrafts.Num() && Z <= 0.0; i++)
            {
                auto Aircraft = Aircrafts[i];

                if (Aircraft && ActorIsLive((const UObject*)Aircraft))
                {
                    Z = (double)Aircraft->K2_GetActorLocation().Z;
                    From = "the bus itself";
                }
            }
        }

        if (!(Z > 1000.0 && Z < 500000.0) && GameState)
        {
            const uint32 PathsOffset = ((UObject*)GameState)->GetOffset("TeamFlightPaths");

            if (PathsOffset != (uint32)-1)
            {
                auto& Paths = GetFromOffset<TArray<uint8>>(GameState, PathsOffset);

                if (Paths.Num() > 0 && Paths.Data)
                {
                    const uint8* Start = (const uint8*)Paths.Data;
                    Z = (FVector::Size() >= 0x18) ? *(const double*)(Start + 0x10) : (double)*(const float*)(Start + 0x8);
                    From = "the bus's flight path";
                }
            }
        }

        if (!(Z > 1000.0 && Z < 500000.0))
            return -1.0;

        GBusDropZ = Z;

        LogLine("[BUS] The bus flies at Z=%.0f (read from %s) -- bots appear at %.1f%% of that, Z=%.0f, over their landing spots "
                "(the full height took them over two minutes to glide down).", Z, From, kDropShare * 100.0, Z * kDropShare);

        return GBusDropZ;
    }

    static double BotDropHeight(UWorld* World, AFortGameStateAthena* GameState, double X, double Y)
    {
        const double BusZ = BusDropHeight(World, GameState);
        double Z = kDropFallbackZ;

        if (BusZ > 0.0)
        {
            Z = BusZ * kDropShare;
        }
        else
        {
            static bool bSaidFallback = false;

            if (!bSaidFallback)
            {
                bSaidFallback = true;
                LogLine("[BUS] The bus's height could NOT be read yet (the bus and its flight path) -- bots appear at Z=%.0f until it can.",
                        kDropFallbackZ);
            }
        }

        double Ground = 0.0;

        if (GameState && GroundZUnder((AActor*)GameState, X, Y, Z + 30000.0, Ground) && Z < Ground + kDropClearance)
        {
            static int Raised = 0;
            const double Planned = Z;

            Z = Ground + kDropClearance;

            if (++Raised <= 5)
                LogLine("[BUS] The ground under landing spot X=%.0f Y=%.0f is at Z=%.0f -- that bot appears at Z=%.0f instead of Z=%.0f, so "
                        "it still has room to open its glider (%d spot(s) raised so far).", X, Y, Ground, Z, Planned, Raised);
        }

        return Z;
    }

    static FVector NextLandingSpot(UWorld* World, AFortGameStateAthena* GameState)
    {
        BuildLandingPlan(World, GameState);

        FVector Dest{};

        if (!LandingPlan.empty())
        {
            Dest = LandingPlan[LandingPlanCursor % LandingPlan.size()];
            LandingPlanCursor++;
        }

        Dest.Z = BotDropHeight(World, GameState, (double)Dest.X, (double)Dest.Y);
        return Dest;
    }

    static std::map<void*, FVector> BotLandingSpot;

    static FVector LandingSpotFor(UWorld* World, AFortGameStateAthena* GameState, void* Controller)
    {
        auto It = BotLandingSpot.find(Controller);

        if (It != BotLandingSpot.end())
        {
            FVector Spot = It->second;
            return Spot;
        }

        FVector Spot = NextLandingSpot(World, GameState);
        BotLandingSpot.emplace(Controller, Spot);
        return Spot;
    }

    static AFortPlayerPawnAthena* MakeBotPawnByHand(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState,
                                                    const FVector& Where)
    {
        auto World = UWorld::GetWorld();
        auto GM = World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
        if (!Controller || !GM)
            return nullptr;

        auto PawnClass = BotPawnClass(GM);
        if (!PawnClass)
            return nullptr;

        FTransform Tr{};
        FVector One(1.f, 1.f, 1.f);
        FVector SpawnAt(Where.X, Where.Y, Where.Z);
        Tr.Scale3D = One;
        Tr.Translation = SpawnAt;

        auto Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(PawnClass, Tr);
        if (!Pawn)
            return nullptr;

        if (!Pawn->IsA(AFortPlayerPawnAthena::StaticClass()))
        {
            Pawn->K2_DestroyActor();
            GBotPawnClass = nullptr;
            return nullptr;
        }

        Controller->Possess(Pawn);
        if (Controller->HasMyFortPawn())
            Controller->MyFortPawn = Pawn;
        Pawn->PlayerState = PlayerState;
        Pawn->OnRep_PlayerState();
        Pawn->SetMaxHealth(100.f);
        Pawn->SetHealth(100.f);
        Pawn->SetShield(0.f);

        GHandMadeBotPawns++;
        return Pawn;
    }

    static void SendBotToItsLandingSpot(UWorld* World, AFortPlayerPawnAthena* Pawn, FVector Dest)
    {
        if (!World || !Pawn)
            return;

        MovedToIsland.insert((void*)Pawn);

        FVector Before = Pawn->K2_GetActorLocation();

        bool bTeleported = Pawn->K2_TeleportTo(Dest, Pawn->K2_GetActorRotation());
        if (!bTeleported)
            Pawn->K2_SetActorLocation(Dest, false, (void*)nullptr, true);

        Pawn->BeginSkydiving(true);

        if (Pawn->HasbIsSkydivingFromBus() && !Pawn->bIsSkydivingFromBus)
            Pawn->bIsSkydivingFromBus = true;
        if (Pawn->HasbIsSkydiving() && !Pawn->bIsSkydiving)
        {
            Pawn->bIsSkydiving = true;
            bool bWasSkydiving = false;
            Pawn->OnRep_IsSkydiving(bWasSkydiving);
        }

        const UObject* Glider = GliderInBotLoadout(Pawn);
        const char* GliderLoadout = "in its loadout since it spawned";

        if (!Glider)
        {
            Glider = PickBotGlider();
            GliderLoadout = PutGliderInBotLoadout(Pawn, Glider);
        }

        if (Glider)
            BotGliderOf[(void*)Pawn] = Glider;

        FVector After = Pawn->K2_GetActorLocation();
        double movedDistSq = (After.X - Before.X) * (After.X - Before.X)
                           + (After.Y - Before.Y) * (After.Y - Before.Y);

        if (LandingLogCount < 12)
        {
            LandingLogCount++;
            LogLine("[BotAI] Land#%d: target X=%.0f Y=%.0f Z=%.0f | teleport=%s | ended X=%.0f Y=%.0f Z=%.0f | moved %.0f units | skydiving=%s | glider=%s (%s) | health %.0f\n",
                   LandingLogCount,
                   (float)Dest.X, (float)Dest.Y, (float)Dest.Z,
                   bTeleported ? "OK" : "REFUSED->forced",
                   (float)After.X, (float)After.Y, (float)After.Z,
                   (float)sqrt(movedDistSq),
                   (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) ? "yes" : "NO",
                   Glider ? Glider->Name.ToString().c_str() : "default", GliderLoadout,
                   Pawn->GetHealth());
        }

        const float DropTime = (float)UGameplayStatics::GetTimeSeconds(World);

        PendingParachuteOpen[(void*)Pawn] = DropTime + 3.f;

        GlidingBots[(void*)Pawn] = (float)After.Z;
        GlidingSettledFor[(void*)Pawn] = 0;

        FBotGlide& Glide = BotGlides[(void*)Pawn];
        Glide = FBotGlide{};
        Glide.JumpTime = DropTime;
        Glide.JumpZ = (float)After.Z;
        Glide.LastDescentZ = (float)After.Z;
        Glide.LastDescentTime = DropTime;
    }

    static std::string DescribeBotPawn(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn)
            return std::string("no pawn at all");

        char Text[96];
        const FVector At = Pawn->K2_GetActorLocation();
        snprintf(Text, sizeof(Text), "a pawn with %.0f health at Z=%.0f", Pawn->GetHealth(), (float)At.Z);
        return std::string(Text);
    }

    static int GPawnSwapsUndone = 0;
    static int GPawnSwapsNotUndone = 0;
    static int GGamePawnsAdopted = 0;
    static int GLostPawnsRemade = 0;
    static int GBrokenPawnsSwappedIn = 0;
    static int GReplacementsFailed = 0;

    static void KeepBotsOnTheirOwnPawns(AFortGameStateAthena* GameState)
    {
        static bool bWindowClosed = false;

        if (bWindowClosed || !bBusDeparted || !GameState || !GameState->HasPlayerArray() || GoodBotPawns.empty())
            return;

        auto World = UWorld::GetWorld();
        if (!World)
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        static float FirstLook = -1.f;
        if (FirstLook < 0.f)
            FirstLook = Now;

        static float DropEnd = -1.f;
        if (DropEnd <= 0.f)
            DropEnd = BusDropEndTime(GameState);

        const float Until = (DropEnd > 0.f) ? DropEnd + 20.f : FirstLook + 120.f;

        if (Now > Until)
        {

            bWindowClosed = true;

            {
                int StillOnBoard = 0;
                for (int i = 0; i < GameState->PlayerArray.Num(); i++)
                {
                    auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                    if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                        continue;

                    auto PC = (AFortPlayerControllerAthena*)PS->Owner;
                    if (PC && IsPlayerControllerBot((AActor*)PC) && PC->IsInAircraft())
                        StillOnBoard++;
                }

                LogLine("[BUS] The bus is gone (bot pawns watched until %.0fs): the game moved a bot off its own pawn %d time(s) -- put "
                        "back on it %d time(s), could NOT be put back %d time(s) | pawns the game gave a bot that it kept: %d | given a "
                        "new pawn by hand after the game took theirs: %d (left on a broken pawn: %d, left with none: %d) | could not be "
                        "given one: %d | bots the game still has on board: %d.",
                        Until, GPawnSwapsUndone + GPawnSwapsNotUndone, GPawnSwapsUndone, GPawnSwapsNotUndone, GGamePawnsAdopted,
                        GBrokenPawnsSwappedIn + GLostPawnsRemade, GBrokenPawnsSwappedIn, GLostPawnsRemade, GReplacementsFailed,
                        StillOnBoard);
            }

            return;
        }

        static int SnapshotsTaken = 0;

        if (DropEnd > 0.f && SnapshotsTaken < 2 && Now >= DropEnd + (SnapshotsTaken == 0 ? 1.f : 5.f))
        {
            SnapshotsTaken++;

            int OnOwn = 0, OnOther = 0, OnNone = 0, Aboard = 0, NotOffYet = 0;

            for (int i = 0; i < GameState->PlayerArray.Num(); i++)
            {
                auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                    continue;

                auto PC = (AFortPlayerControllerAthena*)PS->Owner;
                if (!PC || !IsPlayerControllerBot((AActor*)PC) || EliminatedBots.find((void*)PC) != EliminatedBots.end())
                    continue;

                if (PC->IsInAircraft())
                    Aboard++;

                auto Own = GoodBotPawns.find((void*)PC);
                auto P = (AFortPlayerPawnAthena*)PC->Pawn;

                if (Own == GoodBotPawns.end())
                    NotOffYet++;
                else if (!P)
                    OnNone++;
                else if (P == Own->second.Pawn)
                    OnOwn++;
                else
                    OnOther++;
            }

            LogLine("[BUS] %.0fs after the end of the bus's drop zone (%.0fs): bots on their own pawn %d, on another pawn %d, with no pawn "
                    "%d, never got off %d | bots the game still has on board: %d | put right so far: %d put back on their own pawn, %d "
                    "kept the pawn the game gave them, %d given a new one.",
                    Now - DropEnd, DropEnd, OnOwn, OnOther, OnNone, NotOffYet, Aboard,
                    GPawnSwapsUndone, GGamePawnsAdopted, GBrokenPawnsSwappedIn + GLostPawnsRemade);
        }

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            auto GoodIt = GoodBotPawns.find((void*)Controller);
            if (GoodIt == GoodBotPawns.end())
                continue;

            FGoodBotPawn& Good = GoodIt->second;
            auto Current = (AFortPlayerPawnAthena*)Controller->Pawn;

            if (Current && Current == Good.Pawn)
            {
                if (BotPawnStillExists(Good))
                    Good.LastHealth = Current->GetHealth();

                continue;
            }

            if (BotPawnIsUsable(Good))
            {
                const std::string Other = DescribeBotPawn(Current);

                Controller->Possess(Good.Pawn);
                if (Controller->HasMyFortPawn())
                    Controller->MyFortPawn = Good.Pawn;

                if ((void*)Good.Pawn->PlayerState != (void*)PlayerState)
                {
                    Good.Pawn->PlayerState = PlayerState;
                    Good.Pawn->OnRep_PlayerState();
                }

                const bool bBack = (void*)Controller->Pawn == (void*)Good.Pawn;
                bool bRemoved = false;

                if (bBack && Current && Current != Good.Pawn && !Current->Controller)
                {
                    Current->K2_DestroyActor();
                    bRemoved = true;
                }

                const char* Why = "it was not on board";
                if (bBack && Controller->IsInAircraft())
                    MarkBotOffTheBus(Controller, &Why);

                if (bBack)
                {
                    GPawnSwapsUndone++;
                    BotBeingFixedUntil.erase((void*)Controller);
                }
                else
                {
                    GPawnSwapsNotUndone++;
                }

                const int Swaps = GPawnSwapsUndone + GPawnSwapsNotUndone;

                if (Swaps <= 5 || (Swaps % 25) == 0 || (!bBack && GPawnSwapsNotUndone <= 5))
                    LogLine("[BUS] The game moved '%s' off its own pawn, onto %s -- %s (its own pawn: %.0f health at Z=%.0f)%s | "
                            "the game says it is on the bus: %s (%s) | %d time(s) so far.",
                            SafeNameOf(PlayerState).c_str(), Other.c_str(),
                            bBack ? "put back on its own pawn" : "could NOT be put back on its own pawn",
                            Good.Pawn->GetHealth(), (float)Good.Pawn->K2_GetActorLocation().Z,
                            bRemoved ? ", the other pawn removed" : "",
                            Controller->IsInAircraft() ? "YES" : "no", Why, Swaps);

                Good.LastHealth = Good.Pawn->GetHealth();
                continue;
            }

            if (BotPawnStillExists(Good))
            {
                Good.LastHealth = 0.f;
                continue;
            }

            if (!(Good.LastHealth > 0.f))
                continue;

            {
                auto Mode = (AFortGameMode*)World->AuthorityGameMode;
                bool bStillIn = false;

                if (Mode && Mode->HasAlivePlayers())
                    for (int a = 0; a < Mode->AlivePlayers.Num(); a++)
                        if ((void*)Mode->AlivePlayers[a] == (void*)Controller)
                        {
                            bStillIn = true;
                            break;
                        }

                if (!bStillIn)
                {
                    Good.LastHealth = 0.f;
                    continue;
                }
            }

            const std::string Was = DescribeBotPawn(Current);

            if (Current)
            {
                if (!(Current->GetHealth() > 0.f) && ReadBoolByName((UObject*)Current, "bIsDying") != 1)
                {
                    Current->SetMaxHealth(100.f);
                    Current->SetHealth(100.f);
                }

                if (Current->GetHealth() > 0.f)
                {
                    RememberGoodBotPawn(Controller, Current);

                    const bool bSent = MovedToIsland.find((void*)Current) == MovedToIsland.end();
                    if (bSent)
                        SendBotToItsLandingSpot(World, Current, LandingSpotFor(World, GameState, (void*)Controller));

                    GGamePawnsAdopted++;
                    BotBeingFixedUntil.erase((void*)Controller);

                    if (GGamePawnsAdopted <= 5 || (GGamePawnsAdopted % 25) == 0)
                        LogLine("[BUS] The game took '%s's own pawn away and gave it %s -- kept (health now %.0f)%s (%d so far).",
                                SafeNameOf(PlayerState).c_str(), Was.c_str(), Current->GetHealth(),
                                bSent ? " and sent to its landing spot" : ", left where it is", GGamePawnsAdopted);

                    continue;
                }
            }

            const FVector Spot = LandingSpotFor(World, GameState, (void*)Controller);
            auto Fresh = MakeBotPawnByHand(Controller, PlayerState, Spot);

            if (!Fresh)
            {

                BotBeingFixedUntil[(void*)Controller] = Now + 3.f;
                GReplacementsFailed++;

                if (GReplacementsFailed <= 3)
                    LogLine("[BUS] '%s' lost its own pawn and has %s -- making it a new one by hand FAILED; tried again next tick "
                            "(it counts as alive for 3 s).", SafeNameOf(PlayerState).c_str(), Was.c_str());

                continue;
            }

            bool bRemoved = false;
            if (Current && Current != Fresh && !Current->Controller)
            {
                Current->K2_DestroyActor();
                bRemoved = true;
            }

            RememberGoodBotPawn(Controller, Fresh);
            SendBotToItsLandingSpot(World, Fresh, Spot);
            BotBeingFixedUntil.erase((void*)Controller);

            const char* Why = "it was not on board";
            if (Controller->IsInAircraft())
                MarkBotOffTheBus(Controller, &Why);

            if (Current)
                GBrokenPawnsSwappedIn++;
            else
                GLostPawnsRemade++;

            if ((GBrokenPawnsSwappedIn + GLostPawnsRemade) <= 5 || ((GBrokenPawnsSwappedIn + GLostPawnsRemade) % 25) == 0)
                LogLine("[BUS] The game took '%s's own pawn away and left it with %s -- given a new pawn by hand at its landing spot "
                        "(health read back: %.0f)%s | the game says it is on the bus: %s (%s).",
                        SafeNameOf(PlayerState).c_str(), Was.c_str(), Fresh->GetHealth(), bRemoved ? ", the broken pawn removed" : "",
                        Controller->IsInAircraft() ? "YES" : "no", Why);
        }
    }

    bool MoveSomeBotsToIsland(int MaxThisTick)
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return true;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return true;

        BuildLandingPlan(World, GameState);

        if (LandingPlan.empty())
        {
            LogLine("[BotAI] Landing plan is empty -- can't place bots on the island.\n");
            return true;
        }

        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);
        const float DropStart = BusDropStartTime(GameState);
        const float DropEnd = BusDropEndTime(GameState);

        if (DropStart > 0.f && Now < DropStart)
        {
            static bool bSaidSo = false;

            if (!bSaidSo)
            {
                bSaidSo = true;
                LogLine("[BUS] Bots get off when the doors open: the drop zone runs from %.0fs to %.0fs (now %.0fs).",
                        DropStart, DropEnd, Now);
            }

            return false;
        }

        int movedThisTick = 0;
        bool anyRemaining = false;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            if (movedThisTick >= MaxThisTick)
            {
                anyRemaining = true;
                break;
            }

            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller)
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

            if (Pawn && MovedToIsland.find((void*)Pawn) != MovedToIsland.end())
                continue;

            const bool bThrownOut = Pawn != nullptr;
            bool bJumpedItself = false;
            bool bHandMade = false;

            if (!Pawn)
            {
                const bool bWasAboard = Controller->IsInAircraft();

                static std::set<void*> ReadiedForTheJump;
                if (ReadiedForTheJump.insert((void*)Controller).second)
                    MarkBotReadyLikeAPlayer(Controller, PlayerState, false);

                if (bWasAboard && !GGameJumpBroken && (GGameJumps > 0 || GGameJumpTries < 3))
                {
                    GGameJumpTries++;

                    if (!GameJumpGuarded(Controller))
                    {
                        GGameJumpBroken = true;
                        LogLine("[BUS] The game's own aircraft jump faulted on a bot -- from now on bots get their pawn made by hand.");
                    }

                    Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

                    if (Pawn)
                    {
                        bJumpedItself = true;
                        GGameJumps++;

                        if (GGameJumps <= 3 || (GGameJumps % 25) == 0)
                            LogLine("[BUS] '%s' jumped out of the bus with the game's own jump (%d so far) | still on board after: %s | "
                                    "came out at Z=%.0f with %.0f health.",
                                    SafeNameOf(PlayerState).c_str(), GGameJumps, Controller->IsInAircraft() ? "YES" : "no",
                                    (float)Pawn->K2_GetActorLocation().Z, Pawn->GetHealth());
                    }
                    else
                    {
                        LogLine("[BUS] The game's own jump did not let '%s' off the bus (try %d of 3) | on board: yes | bus doors locked: %s | "
                                "drop zone %.0fs to %.0fs, now %.0fs | loading flags: client %d, server %d, ready %d.",
                                SafeNameOf(PlayerState).c_str(), GGameJumpTries,
                                ReadBoolByName((UObject*)GameState, "bAircraftIsLocked") == 1 ? "YES"
                                    : (ReadBoolByName((UObject*)GameState, "bAircraftIsLocked") == 0 ? "no" : "?"),
                                DropStart, DropEnd, Now,
                                ReadBoolByName((UObject*)Controller, "bHasClientFinishedLoading"),
                                ReadBoolByName((UObject*)Controller, "bHasServerFinishedLoading"),
                                ReadBoolByName((UObject*)Controller, "bReadyToStartMatch"));

                        if (GGameJumpTries == 3 && GGameJumps == 0)
                            LogLine("[BUS] The game's own jump turned down the first 3 bots, as it did all of them in the last two logs -- "
                                    "every bot now gets its pawn made by hand at its landing spot and is marked off the bus.");
                    }
                }
            }

            const FVector Dest = LandingSpotFor(World, GameState, (void*)Controller);

            if (!Pawn)
            {
                Pawn = MakeBotPawnByHand(Controller, PlayerState, Dest);

                if (!Pawn)
                {
                    static int Failures = 0;
                    if (++Failures <= 3)
                        LogLine("[BUS] Making a pawn by hand for '%s' FAILED -- tried again next tick.", SafeNameOf(PlayerState).c_str());

                    anyRemaining = true;
                    continue;
                }

                bHandMade = true;
            }
            else if (bThrownOut)
            {
                GThrownOutByBus++;

                if (GThrownOutByBus <= 3 || (GThrownOutByBus % 25) == 0)
                {
                    const FVector Where = Pawn->K2_GetActorLocation();

                    LogLine("[BUS] '%s' already had a pawn when it got off (it kept its pawn from the island, or the game let it out "
                            "itself): at X=%.0f Y=%.0f Z=%.0f with %.0f health -- moved to its landing spot (%d so far).",
                            SafeNameOf(PlayerState).c_str(), (float)Where.X, (float)Where.Y, (float)Where.Z, Pawn->GetHealth(), GThrownOutByBus);
                }
            }

            if (!bHandMade)
            {
                GiveFreshBotPawnHealth(Controller, Pawn, bJumpedItself ? "off the bus with the game's own jump" : "a pawn it already had");

                if (!(Pawn->GetHealth() > 0.f) && ReadBoolByName((UObject*)Pawn, "bIsDying") != 1)
                {
                    auto Broken = Pawn;
                    auto Fresh = MakeBotPawnByHand(Controller, PlayerState, Dest);

                    if (!Fresh)
                    {

                        BotBeingFixedUntil[(void*)Controller] = Now + 3.f;

                        static int FixFailures = 0;
                        if (++FixFailures <= 3)
                            LogLine("[BUS] '%s' is on a pawn with no health that will not take any, and making it a new one by hand "
                                    "FAILED -- tried again next tick (it counts as alive for 3 s).", SafeNameOf(PlayerState).c_str());

                        anyRemaining = true;
                        continue;
                    }

                    {
                        const bool bRemoved = Broken != Fresh && !Broken->Controller;
                        if (bRemoved)
                            Broken->K2_DestroyActor();

                        Pawn = Fresh;
                        bHandMade = true;
                        GBrokenPawnsReplaced++;

                        if (GBrokenPawnsReplaced <= 3 || (GBrokenPawnsReplaced % 25) == 0)
                            LogLine("[BUS] The pawn the game gave '%s' has no health and will not take any -- replaced by a pawn made by "
                                    "hand at its landing spot (health read back: %.0f)%s (%d so far).",
                                    SafeNameOf(PlayerState).c_str(), Fresh->GetHealth(), bRemoved ? ", the broken one removed" : "",
                                    GBrokenPawnsReplaced);
                    }
                }
            }

            const char* Why = "it was not on board";
            bool bOffTheBus = !Controller->IsInAircraft();

            if (!bOffTheBus)
            {
                bOffTheBus = MarkBotOffTheBus(Controller, &Why);

                if (bOffTheBus)
                    GMarkedOffTheBus++;
                else
                    GNotMarkedOffTheBus++;
            }

            if (bHandMade && (GHandMadeBotPawns <= 3 || (GHandMadeBotPawns % 25) == 0))
                LogLine("[BUS] '%s' got its pawn by hand at its landing spot (X=%.0f Y=%.0f Z=%.0f, %.0f health; %d so far) | off the bus "
                        "as far as the game is concerned: %s (%s).",
                        SafeNameOf(PlayerState).c_str(), (float)Dest.X, (float)Dest.Y, (float)Dest.Z, Pawn->GetHealth(),
                        GHandMadeBotPawns, bOffTheBus ? "yes" : "NO", Why);
            else if (!bOffTheBus && GNotMarkedOffTheBus <= 3)
                LogLine("[BUS] '%s' could not be marked off the bus (%s) -- if the bus throws it out at the end of its drop zone, it is "
                        "put back on its own pawn.", SafeNameOf(PlayerState).c_str(), Why);

            if (Pawn->GetHealth() > 0.f)
            {
                RememberGoodBotPawn(Controller, Pawn);
            }
            else
            {
                static int NoHealthReports = 0;
                if (++NoHealthReports <= 3)
                    LogLine("[BUS] '%s's pawn reads %.0f health after it got off the bus (%s) -- it is sent to its landing spot anyway, "
                            "and will be counted as dead.", SafeNameOf(PlayerState).c_str(), Pawn->GetHealth(),
                            bHandMade ? "made by hand" : "made by the game");
            }

            SendBotToItsLandingSpot(World, Pawn, Dest);
            movedThisTick++;
        }

        if (!anyRemaining)
        {
            static bool bSummarized = false;

            if (!bSummarized)
            {
                bSummarized = true;

                int StillOnBoard = 0, WithHealth = 0;
                for (int i = 0; i < GameState->PlayerArray.Num(); i++)
                {
                    auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                    if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                        continue;

                    auto PC = (AFortPlayerControllerAthena*)PS->Owner;
                    if (!PC || !IsPlayerControllerBot((AActor*)PC))
                        continue;

                    if (PC->IsInAircraft())
                        StillOnBoard++;

                    auto P = (AFortPlayerPawnAthena*)PC->Pawn;
                    if (P && P->GetHealth() > 0.f)
                        WithHealth++;
                }

                LogLine("[BUS] Every bot is off the bus (%.0fs, the drop zone ends at %.0fs): %d with the game's own jump, %d already on "
                        "a pawn, %d given a pawn by hand (%d of those because the game's pawn had no health) | marked off the bus: %d, "
                        "could not be: %d (bytes written %d, refused %d) | bots the game still has on board: %d | bots on a pawn with "
                        "health: %d.",
                        Now, DropEnd, GGameJumps, GThrownOutByBus, GHandMadeBotPawns, GBrokenPawnsReplaced, GMarkedOffTheBus,
                        GNotMarkedOffTheBus, GBusExitLatch.Written, GBusExitLatch.Refused, StillOnBoard, WithHealth);
            }
        }

        return !anyRemaining;
    }

    void ThankAllBots()
    {
        if (!bBusDeparted)
            return;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        float Now = (float)UGameplayStatics::GetTimeSeconds(World);
        if ((Now - LastThankTime) < 1.0f)
            return;
        LastThankTime = Now;

        int thanksThisPass = 3;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AActor*)PlayerState->Owner;
            if (!Controller)
                continue;

            if (ThankedBots.find((void*)Controller) != ThankedBots.end())
                continue;
            ThankedBots.insert((void*)Controller);

            if (PlayerState->HasbThankedBusDriver())
            {
                PlayerState->bThankedBusDriver = true;
                PlayerState->OnRep_ThankedBusDriver();

                {
                    const std::string Narrow = SafeNameOf(PlayerState);

                    if (!Narrow.empty())
                        LogLine("[BUS] %s thanked the bus driver.", Narrow.c_str());
                }
            }

            if (--thanksThisPass <= 0)
                break;
        }
    }

    void LogHudState()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameState->HasPlayerArray())
            return;

        static float LastHudLog = -1000.f;
        float Now = (float)UGameplayStatics::GetTimeSeconds(World);
        if ((Now - LastHudLog) < 5.0f)
            return;
        LastHudLog = Now;

        int trueAliveBots = 0;
        int trueAliveHumans = 0;
        int botsWithPawn = 0;
        int botsDeadPawn = 0;
        int onTheBus = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS) continue;
            bool bIsBot = PS->HasbIsABot() && PS->bIsABot;

            auto OwnerActor = (AActor*)PS->Owner;
            AFortPlayerPawnAthena* Pw = nullptr;
            if (OwnerActor)
                Pw = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)OwnerActor)->Pawn;

            bool bPawnless = false;
            bool bAlive = PlayerIsStillIn(GameMode, PS, &bPawnless);

            if (bAlive && bPawnless)
                onTheBus++;

            if (bIsBot)
            {
                if (Pw) botsWithPawn++; else botsDeadPawn++;
                if (bAlive) trueAliveBots++;
            }
            else if (bAlive)
            {
                trueAliveHumans++;
            }
        }

        int hudPlayersLeft = GameState->HasPlayersLeft() ? (int)GameState->PlayersLeft : -1;
        int hudTeamsLeft   = GameState->HasTeamsLeft()   ? (int)GameState->TeamsLeft   : -1;
        int aliveArrayNum  = GameMode->HasAlivePlayers() ? GameMode->AlivePlayers.Num() : -1;
        int trueAlive      = trueAliveBots + trueAliveHumans;

        LogLine("[HUD] PlayersLeft(shown)=%d TeamsLeft(shown)=%d | TRUE alive=%d (bots=%d humans=%d, %d of them with no pawn yet -- on the bus or not spawned) | AlivePlayers[]=%d PlayerArray=%d | bots w/pawn=%d w/o=%d%s\n",
               hudPlayersLeft, hudTeamsLeft, trueAlive, trueAliveBots, trueAliveHumans, onTheBus,
               aliveArrayNum, GameState->PlayerArray.Num(), botsWithPawn, botsDeadPawn,
               (hudPlayersLeft != trueAlive) ? "  <<< MISMATCH: HUD disagrees with reality" : "");

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS) continue;
            bool bIsBot = PS->HasbIsABot() && PS->bIsABot;

            int kills = PS->HasKillScore() ? (int)PS->KillScore : -1;
            int killScore = PS->HasKillScore() ? (int)PS->KillScore : -1;

            if (!bIsBot)
            {
                const std::string nm = SafeNameOf(PS);
                LogLine("[HUD] Human '%s': Kills(shown)=%d KillScore=%d\n", nm.c_str(), kills, killScore);
            }
            else if (kills > 0)
            {
                LogLine("[HUD] Bot has %d kill(s) (KillScore=%d)\n", kills, killScore);
            }
        }
    }

    void UpdateHudCounts()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray() || !GameState->HasPlayersLeft())
            return;

        auto GameMode = CurrentGameMode();

        int trueAlive = 0;
        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            if (PlayerIsStillIn(GameMode, PS))
                trueAlive++;
        }

        if (trueAlive <= 0)
            return;

        if ((int)GameState->PlayersLeft != trueAlive)
        {
            int before = (int)GameState->PlayersLeft;
            GameState->PlayersLeft = trueAlive;
            GameState->OnRep_PlayersLeft();
            if (GameState->HasTeamsLeft())
                GameState->TeamsLeft = trueAlive;

            LogLine("[HUD] PlayersLeft corrected %d -> %d (live recount).\n", before, trueAlive);
        }
    }

    static void NoOpExecFunction(void*, void*, void*)
    {
        static int Calls = 0;

        if (++Calls <= 5)
        {
            printf("[Storm] A silenced distant-storm function was called (%d) -- did nothing, as intended.\n", Calls);
            fflush(stdout);
        }
    }

    static void SilenceDistantStorm()
    {
        static bool bDone = false;
        if (bDone)
            return;

        static int Countdown = 0;
        static int Attempts = 0;

        if (Countdown-- > 0)
            return;

        Countdown = 15;

        if (++Attempts > 240)
        {
            bDone = true;
            printf("[SafeZone] Never found the safe zone indicator class -- distant storm left as it is.\n");
            fflush(stdout);
            return;
        }

        const UStruct* IndicatorClass = (const UStruct*)FindClass("SafeZoneIndicator_C");

        if (!IndicatorClass)
        {
            const int Total = TUObjectArray::Num();

            for (int i = 0; i < Total; i++)
            {
                auto Obj = TUObjectArray::GetObjectByIndex(i);

                if (!Obj || !Obj->Class || !(Obj->Class->GetCastFlags() & 0x20))
                    continue;

                auto RawName = Obj->Name.ToString();
                std::string Lowered;
                for (const char* p = RawName.c_str(); *p; p++)
                    Lowered += (char)tolower((unsigned char)*p);

                if (Lowered.find("safezoneindicator") != std::string::npos)
                {
                    IndicatorClass = (const UStruct*)Obj;
                    break;
                }
            }
        }

        if (!IndicatorClass)
            return;

        bDone = true;

        int Silenced = 0;

        for (const UStruct* Cls = IndicatorClass; Cls; Cls = Cls->GetSuper())
        {
            for (const UField* Child = Cls->GetChildren(); Child; Child = Child->GetNext())
            {
                if (!Child->Class || !(Child->Class->GetCastFlags() & 0x80000))
                    continue;

                auto RawName = Child->GetName().ToString();
                std::string Lowered;
                for (const char* p = RawName.c_str(); *p; p++)
                    Lowered += (char)tolower((unsigned char)*p);

                if (Lowered.find("distant") == std::string::npos && Lowered.find("oneshot") == std::string::npos)
                    continue;

                Hooking::ExecHook((UFunction*)Child, (void*)&NoOpExecFunction);
                Silenced++;

                printf("[SafeZone] Silenced cosmetic storm function \"%s\".\n", RawName.c_str());
            }
        }

        if (Silenced == 0)
            printf("[SafeZone] Found the indicator class but no distant-storm functions on it.\n");
        else
            printf("[SafeZone] %d distant-storm function(s) silenced -- that is the one that crashes.\n", Silenced);

        fflush(stdout);
    }

    static const float kZoneWait[] = {
        170.f,
        120.f,
         90.f,
         80.f,
         50.f,
         30.f,
          0.f,
          0.f,
          0.f,
         10.f,
    };
    static const float kZoneShrink[] = {
        210.f,
        120.f,
         90.f,
         70.f,
         60.f,
         60.f,
         55.f,
         50.f,
         75.f,
         10.f,
    };
    static const int kZoneCount = (int)(sizeof(kZoneWait) / sizeof(kZoneWait[0]));

    static void ApplyCustomZoneTimings(AFortGameMode* GameMode)
    {
        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return;

        auto Indicator = GameMode->SafeZoneIndicator;
        if (!Indicator->HasSafeZonePhases())
            return;

        auto& Phases = Indicator->SafeZonePhases;
        const int N = Phases.Num();
        if (N <= 0)
            return;

        static bool bLogged = false;
        for (int i = 0; i < N; i++)
        {

            int idx = i;
            if (idx >= kZoneCount)
                idx = kZoneCount - 1;

            auto& Phase = Phases.Get(i, FFortSafeZonePhaseInfo::Size());
            if (FFortSafeZonePhaseInfo::HasWaitTime())
            {
                float W = kZoneWait[idx];
                Phase.WaitTime = W;
            }
            if (FFortSafeZonePhaseInfo::HasShrinkTime())
            {
                float S = kZoneShrink[idx];
                Phase.ShrinkTime = S;
            }
        }

        if (!bLogged)
        {
            bLogged = true;
            LogLine("[SafeZone] Applied custom phase timings (%d phases).", N);
        }
    }

    static bool GStormTimedByTick = false;

    static void EnsureStormTimingData(AFortGameStateAthena* GameState)
    {
        static bool bDone = false;

        if (bDone || !GameState || !GameState->HasMapInfo() || !GameState->MapInfo)
            return;

        auto MapInfo = GameState->MapInfo;

        if (!MapInfo->HasSafeZoneDefinition())
            return;

        bDone = true;

        auto& Definition = MapInfo->SafeZoneDefinition;

        auto IndicatorClass = FindClass("FortSafeZoneIndicator");
        const bool bPhaseList = IndicatorClass && IndicatorClass->GetOffset("SafeZonePhases") != (uint32)-1;

        if (!bPhaseList)
        {
            GStormTimedByTick = true;

            LogLine("[SafeZone] This build's storm keeps no phase list, so nothing is forced into the map's storm data: each phase gets "
                    "its hold and close times the moment it starts, and phase 0 (the whole map, nothing closing) is ended at once so "
                    "the first real circle forms when the storm starts.");

            for (int i = 0; i < kZoneCount; i++)
                LogLine("[SafeZone]   phase %-2d holds %4.0fs, then closes over %4.0fs | the map's own data: radius %7.0f, hold %6.1fs, "
                        "close %6.1fs", i + 1, kZoneWait[i], kZoneShrink[i] < 10.f ? 10.f : kZoneShrink[i],
                        FFortSafeZoneDefinition::HasRadius() ? Definition.Radius.Evaluate((float)(i + 1)) : -1.f,
                        FFortSafeZoneDefinition::HasWaitTime() ? Definition.WaitTime.Evaluate((float)(i + 1)) : -1.f,
                        FFortSafeZoneDefinition::HasShrinkTime() ? Definition.ShrinkTime.Evaluate((float)(i + 1)) : -1.f);

            return;
        }

        if (FFortSafeZoneDefinition::HasWaitTime())
        {
            auto& WaitTime = Definition.WaitTime;
            WaitTime.Value = kZoneWait[0];
            WaitTime.Curve.CurveTable = nullptr;
            GStormMapDataWasRepaired = true;
            printf("[SafeZone] Definition WaitTime forced to %.0fs (phase 1 form).\n", kZoneWait[0]);
        }

        if (FFortSafeZoneDefinition::HasShrinkTime())
        {
            auto& ShrinkTime = Definition.ShrinkTime;
            ShrinkTime.Value = kZoneShrink[0];
            ShrinkTime.Curve.CurveTable = nullptr;
            GStormMapDataWasRepaired = true;
            printf("[SafeZone] Definition ShrinkTime forced to %.0fs (phase 1 close).\n", kZoneShrink[0]);
        }

        fflush(stdout);
    }

    static bool GetSafeZoneCircle(AFortGameMode* GameMode, FVector& OutCentre, float& OutRadius)
    {
        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return false;

        auto Indicator = GameMode->SafeZoneIndicator;

        bool bHaveCentre = false;

        if (Indicator->HasNextCenter())
        {
            OutCentre = Indicator->NextCenter;
            bHaveCentre = true;
        }
        else if (Indicator->HasLastCenter())
        {
            OutCentre = Indicator->LastCenter;
            bHaveCentre = true;
        }
        else if (Indicator->HasPreviousCenter())
        {
            OutCentre = Indicator->PreviousCenter;
            bHaveCentre = true;
        }

        if (!bHaveCentre)
            return false;

        OutRadius = 0.f;
        if (Indicator->HasNextRadius())
            OutRadius = (float)Indicator->NextRadius;
        if (!(OutRadius > 0.f) && Indicator->HasLastRadius())
            OutRadius = (float)Indicator->LastRadius;
        if (!(OutRadius > 0.f) && Indicator->HasPreviousRadius())
            OutRadius = (float)Indicator->PreviousRadius;

        if (!(OutRadius > 0.f))
            return false;

        if (OutCentre.X == 0.0 && OutCentre.Y == 0.0 && OutCentre.Z == 0.0)
            return false;

        return true;
    }

    static int GetStormPhase(AFortGameMode* GameMode)
    {
        if (!GameMode)
            return 0;

        if (GameMode->HasSafeZoneIndicator() && GameMode->SafeZoneIndicator && GameMode->SafeZoneIndicator->HasCurrentPhase())
            return (int)GameMode->SafeZoneIndicator->CurrentPhase;

        if (GameMode->HasSafeZonePhase())
            return (int)GameMode->SafeZonePhase;

        return 0;
    }

    static float StormDamagePerSecond(int Phase)
    {
        static const float Table[] = {
             1.f,
             1.f,
             1.f,
             2.f,
             5.f,
             8.f,
             10.f,
             10.f,
             10.f,
             10.f,
        };

        const int Count = (int)(sizeof(Table) / sizeof(Table[0]));

        if (Phase < 0)
            Phase = 0;
        if (Phase >= Count)
            Phase = Count - 1;

        float Damage = Table[Phase] * FConfiguration::StormDamageScale;

        if (!(Damage > 0.f))
            Damage = 0.f;

        return Damage;
    }

    static int GStormTick = 0;

    static std::map<void*, float> LastKnownHealth;

    static std::map<void*, int> LastPlayerDamageTick;

    static std::map<void*, int> LastOutsideZoneTick;

    static std::map<void*, FVector> LastKnownLocation;

    static void ApplyStormDamage(AFortGameMode* GameMode, AFortGameStateAthena* GameState, int TickNow)
    {
        if (!FConfiguration::bManageStormDamage || FConfiguration::bDisableStorm)
        {
            if (GameState && GameState->HasPlayerArray())
            {
                for (int i = 0; i < GameState->PlayerArray.Num(); i++)
                {
                    auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                    if (!PS)
                        continue;

                    auto Owner = (AActor*)PS->Owner;
                    if (!Owner)
                        continue;

                    auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
                    if (!Pawn)
                        continue;

                    if (Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone)
                        LastOutsideZoneTick[(void*)Owner] = TickNow;
                }
            }

            return;
        }

        if (!GameMode || !GameState || !GameState->HasPlayerArray())
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(GameState);

        static float LastRunTime = 0.f;
        float Delta = Now - LastRunTime;
        LastRunTime = Now;

        if (!(Delta > 0.f) || Delta > 1.0f)
            return;

        const int Phase = GetStormPhase(GameMode);
        const float PerSecond = StormDamagePerSecond(Phase);
        const float ThisTick = PerSecond * Delta;

        static int Reported = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            auto Owner = (AActor*)PS->Owner;
            if (!Owner)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
            if (!Pawn)
                continue;

            const float Health = Pawn->GetHealth();
            auto Previous = LastKnownHealth.find((void*)Owner);
            const float Before = (Previous != LastKnownHealth.end()) ? Previous->second : Health;

            LastKnownHealth[(void*)Owner] = Health;

            if (Health <= 0.f)
                continue;

            const bool bOutside = Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone;
            if (!bOutside)
                continue;

            LastOutsideZoneTick[(void*)Owner] = TickNow;

            if (Pawn->HasbIsInvulnerable() && Pawn->bIsInvulnerable)
                continue;

            auto Hit = LastPlayerDamageTick.find((void*)Owner);
            const bool bTradingShots = (Hit != LastPlayerDamageTick.end()) && ((TickNow - Hit->second) < 20);

            float Take = ThisTick;

            if (!bTradingShots)
            {
                const float Lost = Before - Health;

                if (Lost > 0.f)
                    Take = ThisTick - Lost;
            }

            if (!(Take > 0.f))
                continue;

            float Target = Health - Take;

            if (Target < 0.f)
                Target = 0.f;

            Pawn->SetHealth(Target);
            LastKnownHealth[(void*)Owner] = Target;

            if (++Reported <= 3)
                LogLine("[ZONE] Storm holding at %.1f health a second (phase %d): took %.2f off this tick%s.",
                        PerSecond, Phase, Take, bTradingShots ? " on top of the shot they just took" : "");
        }
    }

    static void LogStormState(AFortGameMode* GameMode, AFortGameStateAthena* GameState)
    {
        if (!FConfiguration::bStormLog || !(FConfiguration::StormLogIntervalSeconds > 0.f))
            return;

        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return;

        auto Indicator = GameMode->SafeZoneIndicator;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(GameState);

        static float NextReport = 0.f;
        if (Now < NextReport)
            return;
        NextReport = Now + FConfiguration::StormLogIntervalSeconds;

        const int Phase = Indicator->HasCurrentPhase()
                              ? (int)Indicator->CurrentPhase
                              : (GameMode->HasSafeZonePhase() ? (int)GameMode->SafeZonePhase : -1);

        const float Start = Indicator->HasSafeZoneStartShrinkTime() ? (float)Indicator->SafeZoneStartShrinkTime : -1.f;
        const float Finish = Indicator->HasSafeZoneFinishShrinkTime() ? (float)Indicator->SafeZoneFinishShrinkTime : -1.f;
        const float NextRadius = Indicator->HasNextRadius() ? (float)Indicator->NextRadius : -1.f;

        float CurrentRadius = -1.f;
        if (Indicator->HasPreviousRadius() && Indicator->PreviousRadius > 0.f)
            CurrentRadius = (float)Indicator->PreviousRadius;
        else if (Indicator->HasLastRadius())
            CurrentRadius = (float)Indicator->LastRadius;

        const char* What = (Now < Start) ? "holding" : (Now < Finish ? "CLOSING" : "due to move on");

        printf("[Storm] phase %d, %s -- starts closing in %.1fs, done in %.1fs | radius %.0f -> %.0f\n",
               Phase, What, Start - Now, Finish - Now, CurrentRadius, NextRadius);
        fflush(stdout);
    }

    static void SeasonOneStormTiming(int Phase, float& OutWait, float& OutShrink)
    {
        static const float Wait[] = { 200.f, 200.f, 150.f, 120.f, 120.f, 90.f, 90.f, 60.f, 45.f, 30.f };
        static const float Shrink[] = { 180.f, 180.f, 90.f, 90.f, 60.f, 40.f, 30.f, 30.f, 25.f, 20.f };
        const int Count = (int)(sizeof(Wait) / sizeof(Wait[0]));

        if (Phase < 0)
            Phase = 0;
        if (Phase >= Count)
            Phase = Count - 1;

        OutWait = Wait[Phase];
        OutShrink = Shrink[Phase];
    }

    static void LogStormChanges(AFortGameMode* GameMode, AFortGameStateAthena* GameState)
    {
        if (!GameMode || !GameState)
            return;

        struct FStormSeen
        {
            const void* Indicator = (const void*)1;
            int ModePhase = -999;
            int StatePhase = -999;
            float StormStartsAt = -12345.f;
            float Start = -12345.f;
            float Finish = -12345.f;
            float LastR = -12345.f;
            float NextR = -12345.f;
        };

        static FStormSeen Seen{};
        static int32 StatePhaseOffset = -2;
        static int32 RadiusOffset = -2;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(GameState);

        if (StatePhaseOffset == -2)
        {
            const uint32 Off = ((UObject*)GameState)->GetOffset("SafeZonePhase");
            StatePhaseOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
        }

        FStormSeen Current{};
        auto Indicator = GameMode->HasSafeZoneIndicator() ? GameMode->SafeZoneIndicator : nullptr;

        Current.Indicator = (const void*)Indicator;
        Current.ModePhase = GameMode->HasSafeZonePhase() ? (int)GameMode->SafeZonePhase : -1;
        Current.StatePhase = StatePhaseOffset >= 0 ? (int)GetFromOffset<uint8>(GameState, (uint32)StatePhaseOffset) : -1;
        Current.StormStartsAt = GameState->HasSafeZonesStartTime() ? (float)GameState->SafeZonesStartTime : -1.f;

        float RadiusNow = -1.f;

        if (Indicator)
        {
            Current.Start = Indicator->HasSafeZoneStartShrinkTime() ? (float)Indicator->SafeZoneStartShrinkTime : -1.f;
            Current.Finish = Indicator->HasSafeZoneFinishShrinkTime() ? (float)Indicator->SafeZoneFinishShrinkTime : -1.f;
            Current.LastR = Indicator->HasLastRadius() ? (float)Indicator->LastRadius : -1.f;
            Current.NextR = Indicator->HasNextRadius() ? (float)Indicator->NextRadius : -1.f;

            if (RadiusOffset == -2)
            {
                const uint32 Off = ((UObject*)Indicator)->GetOffset("Radius");
                RadiusOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
            }

            if (RadiusOffset >= 0)
                RadiusNow = GetFromOffset<float>(Indicator, (uint32)RadiusOffset);
        }

        auto Moved = [](float A, float B, float Tolerance) { return fabsf(A - B) > Tolerance; };

        const bool bChanged = Current.Indicator != Seen.Indicator || Current.ModePhase != Seen.ModePhase ||
                              Current.StatePhase != Seen.StatePhase || Moved(Current.StormStartsAt, Seen.StormStartsAt, 0.05f) ||
                              Moved(Current.Start, Seen.Start, 0.05f) || Moved(Current.Finish, Seen.Finish, 0.05f) ||
                              Moved(Current.LastR, Seen.LastR, 1.f) || Moved(Current.NextR, Seen.NextR, 1.f);

        if (!bChanged)
            return;

        Seen = Current;

        if (!Indicator)
        {
            LogLine("[STORM] What the clients are told changed (%.1fs): no storm indicator yet | game mode's storm phase %d, game "
                    "state's %d | storm starts at %.1f (%s%.1fs).",
                    Now, Current.ModePhase, Current.StatePhase, Current.StormStartsAt,
                    Current.StormStartsAt >= Now ? "in " : "", fabsf(Current.StormStartsAt - Now));
            return;
        }

        const char* What = (Now < Current.Start) ? "holding" : (Now < Current.Finish ? "CLOSING" : "done -- due to move on");

        LogLine("[STORM] What the clients are told changed (%.1fs): phase %d (game state %d), %s | closes from %.1f (in %.1fs) to "
                "%.1f (in %.1fs) | radius now %.0f, from %.0f to %.0f | storm start time %.1f.",
                Now, Current.ModePhase, Current.StatePhase, What, Current.Start, Current.Start - Now, Current.Finish,
                Current.Finish - Now, RadiusNow, Current.LastR, Current.NextR, Current.StormStartsAt);
    }

    static std::map<void*, std::pair<void*, int>> BotTopDamager;

    static std::map<void*, std::pair<void*, int>> BotLastDamager;

    static std::map<void*, std::pair<void*, int>> BotCodeLastHit;

    static AFortPlayerStateAthena* ResolveDamagerState(AActor* Causer)
    {
        if (!Causer)
            return nullptr;

        if (Causer->IsA<AFortPlayerStateAthena>())
            return (AFortPlayerStateAthena*)Causer;

        if (Causer->IsA<AFortPlayerPawnAthena>())
        {
            auto PawnController = (AActor*)((AFortPlayerPawnAthena*)Causer)->Controller;

            return PawnController ? (AFortPlayerStateAthena*)((AFortPlayerControllerAthena*)PawnController)->PlayerState : nullptr;
        }

        if (Causer->GetOffset("PlayerState") != (uint32)-1)
            return (AFortPlayerStateAthena*)((AFortPlayerControllerAthena*)Causer)->PlayerState;

        return nullptr;
    }

    static int32 DamagerInfoSize()
    {
        static int32 Size = -1;

        if (Size == -1)
        {
            auto Struct = FindStruct("DamagerInfo");
            Size = Struct ? Struct->GetPropertiesSize() : (int32)sizeof(FDamagerInfo);

            if (Size <= 0 || Size > 0x200)
                Size = (int32)sizeof(FDamagerInfo);
        }

        return Size;
    }

    static void WipeTheSlateAtBusTime(AFortGameStateAthena* GameState)
    {
        static bool bDone = false;

        if (!GameState || !GameState->HasPlayerArray())
            return;

        if (PlayersAreProtected(GameState))
        {
            bDone = false;
            LandedBots.clear();
            return;
        }

        if (bDone)
            return;

        bDone = true;

        int Healed = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasOwner() || !PS->Owner)
                continue;

            auto Owner = (AActor*)PS->Owner;
            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
            if (!Pawn)
                continue;

            const float Health = Pawn->GetHealth();

            if (Health > 0.f && Health < 100.f)
            {
                float Full = 100.f;
                Pawn->SetHealth(Full);
                Healed++;
            }

            LastKnownHealth.erase((void*)Owner);
            LastPlayerDamageTick.erase((void*)Owner);
            BotTopDamager.erase((void*)Owner);
            BotLastDamager.erase((void*)Owner);
            ShotAtBy.erase((void*)Owner);
            AggroedByHuman.erase((void*)Owner);
        }

        LogLine("[LANDED] Everyone is on the ground. %d player(s) healed back to full and every scratch from the island "
                "and the bus forgotten. Damage and eliminations start counting from here.", Healed);
    }

    static constexpr double kTile   = 512.0;
    static constexpr double kFloor  = 384.0;
    static constexpr double kSub    = 128.0;

    static double SnapTile(double v)
    {
        return round(v / kTile) * kTile;
    }

    static double SnapFloorZ(double v)
    {

        return round(v / kFloor) * kFloor;
    }

    static bool BuildFacesEastWest(double Yaw)
    {
        const long Quarter = lround(Yaw / 90.0);
        return (Quarter % 2) != 0;
    }

    static FVector SnapBuildLocation(const FVector& V, double Yaw)
    {
        if (BuildFacesEastWest(Yaw))
            return FVector{ SnapTile(V.X - 256.0) + 256.0, SnapTile(V.Y), SnapFloorZ(V.Z) };

        return FVector{ SnapTile(V.X), SnapTile(V.Y - 256.0) + 256.0, SnapFloorZ(V.Z) };
    }

    static FRotator SnapBuildRotation(const FRotator& R)
    {
        FRotator Out{};
        Out.Pitch = 0.0;
        Out.Roll  = 0.0;
        Out.Yaw   = round(R.Yaw / 90.0) * 90.0;
        return Out;
    }

    static UClass* ResolveBuildClass(const wchar_t* Path, const char* ShortName)
    {
        UClass* Cls = const_cast<UClass*>(FindObject<UClass>(Path));
        if (!Cls)
            Cls = const_cast<UClass*>(FindClass(ShortName));
        return Cls;
    }

    static ABuildingSMActor* SpawnBuildPiece(UClass* Class, const FVector& Loc, const FRotator& Rot,
                                             AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PS)
    {
        if (!Class || !Controller)
            return nullptr;

        FRotator GridRot = SnapBuildRotation(Rot);
        FVector Snapped = SnapBuildLocation(Loc, GridRot.Yaw);

        ABuildingSMActor* Building = UWorld::SpawnActorUnfinished<ABuildingSMActor>(Class, Snapped, GridRot, Controller);
        if (!Building)
            return nullptr;

        Building->InitializeKismetSpawnedBuildingActor(Building, Controller, true, nullptr, false);
        UWorld::FinishSpawnActor(Building, Snapped, GridRot);

        Building->CurrentBuildingLevel = 0;
        Building->OnRep_CurrentBuildingLevel();
        Building->SetMirrored(false);
        Building->bPlayerPlaced = true;

        if (PS && PS->HasTeamIndex())
            Building->Team = PS->TeamIndex;
        if (Building->HasTeamIndex())
            Building->TeamIndex = Building->Team;

        Building->ForceNetUpdate();
        return Building;
    }

    static void TryBotReactiveBuild(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        const int BuildAfterTicks = 0;

        static UClass* WallWood = nullptr;
        static UClass* WallBrick = nullptr;
        static UClass* WallMetal = nullptr;
        static bool bResolved = false;

        if (!bResolved)
        {
            bResolved = true;
            WallWood  = ResolveBuildClass(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Solid.PBWA_W1_Solid_C", "PBWA_W1_Solid_C");
            WallBrick = ResolveBuildClass(L"/Game/Building/ActorBlueprints/Player/Stone/L1/PBWA_S1_Solid.PBWA_S1_Solid_C", "PBWA_S1_Solid_C");
            WallMetal = ResolveBuildClass(L"/Game/Building/ActorBlueprints/Player/Metal/L1/PBWA_M1_Solid.PBWA_M1_Solid_C", "PBWA_M1_Solid_C");

            if (!WallWood)
                WallWood = ResolveBuildClass(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Brace.PBWA_W1_Brace_C", "PBWA_W1_Brace_C");

            if (!WallWood && !WallBrick && !WallMetal)
            {
                LogLine("[BUILD] Could not resolve any wall class -- reactive building disabled.");
                return;
            }
            LogLine("[BUILD] Tile=512 (5m) Floor=384 | classes ready.");
        }

        if (!WallWood && !WallBrick && !WallMetal)
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PS->Owner;
            if (!Controller)
                continue;

            if (BotAlreadyBuiltWall.find((void*)Controller) != BotAlreadyBuiltWall.end())
                continue;

            auto Since = BotUnderFireSince.find((void*)Controller);
            if (Since == BotUnderFireSince.end())
                continue;
            if ((GStormTick - Since->second) > 60)
                continue;
            if ((GStormTick - Since->second) < BuildAfterTicks)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f)
                continue;

            if (!BotHasLanded((void*)Pawn))
                continue;
            if (GlidingBots.find((void*)Pawn) != GlidingBots.end())
                continue;
            if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
                continue;
            if (Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus)
                continue;
            if (Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen)
                continue;

            FVector Loc = Pawn->K2_GetActorLocation();
            if (Loc.Z > 8000.0)
                continue;

            AFortPlayerPawnAthena* Threat = nullptr;
            const char* Toward = "the way it was facing";

            {
                auto Locked = BotCombatTarget.find((void*)Pawn);

                if (Locked != BotCombatTarget.end() && Locked->second && Locked->second->GetHealth() > 0.f)
                {
                    Threat = Locked->second;
                    Toward = "the one it is fighting";
                }
                else
                {
                    auto Shooter = BotLastDamager.find((void*)Controller);

                    if (Shooter != BotLastDamager.end() && Shooter->second.first)
                    {
                        auto ShooterState = (AFortPlayerStateAthena*)Shooter->second.first;

                        if (ShooterState->HasOwner() && ShooterState->Owner)
                        {
                            auto ShooterPawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)ShooterState->Owner)->Pawn;

                            if (ShooterPawn && ShooterPawn != Pawn && ShooterPawn->GetHealth() > 0.f)
                            {
                                Threat = ShooterPawn;
                                Toward = "whoever just shot it";
                            }
                        }
                    }
                }
            }

            FRotator Facing = Pawn->K2_GetActorRotation();

            if (Threat)
            {
                const FVector ThreatLoc = Threat->K2_GetActorLocation();
                const double ToX = ThreatLoc.X - Loc.X;
                const double ToY = ThreatLoc.Y - Loc.Y;

                if ((ToX * ToX + ToY * ToY) > 1.0)
                {
                    double ThreatYaw = atan2(ToY, ToX) * (180.0 / 3.14159265358979323846);
                    Facing.Yaw = ThreatYaw;
                }
            }

            FRotator GridFacing = SnapBuildRotation(Facing);

            const double YawRad = GridFacing.Yaw * (3.14159265358979323846 / 180.0);
            const double Fx = cos(YawRad);
            const double Fy = sin(YawRad);
            const bool bEastWest = BuildFacesEastWest(GridFacing.Yaw);

            double Ahead = (double)FConfiguration::BotWallDistance;
            if (!(Ahead > 0.0))
                Ahead = kTile + kTile;

            FVector Wall1{};

            if (!bEastWest)
            {
                Wall1.X = SnapTile(Loc.X + (Fx > 0.0 ? Ahead : -Ahead));
                Wall1.Y = SnapTile(Loc.Y - 256.0) + 256.0;
            }
            else
            {
                Wall1.X = SnapTile(Loc.X - 256.0) + 256.0;
                Wall1.Y = SnapTile(Loc.Y + (Fy > 0.0 ? Ahead : -Ahead));
            }

            const double FloorZ = floor(Loc.Z / kFloor) * kFloor;
            Wall1.Z = FloorZ;

            UClass* WallCls = WallWood;
            int MatRoll = rand() % 100;
            if (MatRoll >= 70 && WallBrick) WallCls = WallBrick;
            else if (MatRoll >= 90 && WallMetal) WallCls = WallMetal;
            if (!WallCls) WallCls = WallWood ? WallWood : (WallBrick ? WallBrick : WallMetal);

            const bool bPlaced = SpawnBuildPiece(WallCls, Wall1, GridFacing, Controller, PS) != nullptr;

            BotAlreadyBuiltWall.insert((void*)Controller);

            if (bPlaced)
            {
                static int WallReports = 0;

                const double Away = bEastWest ? fabs(Wall1.Y - Loc.Y) : fabs(Wall1.X - Loc.X);
                const char* Direction = !bEastWest ? (Fx > 0.0 ? "north" : "south") : (Fy > 0.0 ? "east" : "west");

                const int ModX = ((int)llround(Wall1.X) % 512 + 512) % 512;
                const int ModY = ((int)llround(Wall1.Y) % 512 + 512) % 512;

                if (++WallReports <= 25)
                    LogLine("[BUILD] '%s' put up ONE wall %.1f m to its %s, towards %s | X=%.0f Y=%.0f Z=%.0f "
                            "(X%%512=%d Y%%512=%d -- where your own %s walls snap).",
                            SafeNameOf(PS).c_str(), (float)(Away / 100.0), Direction, Toward,
                            (float)Wall1.X, (float)Wall1.Y, (float)Wall1.Z, ModX, ModY,
                            bEastWest ? "east/west-facing" : "north/south-facing");
            }
        }
    }

    void TrackBotDamage()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        const int32 Stride = DamagerInfoSize();

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Controller = (AActor*)PS->Owner;
            if (!Controller)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Controller)->Pawn;
            if (!Pawn || !Pawn->HasDamagers())
                continue;

            auto& Damagers = Pawn->Damagers;

            if (Damagers.Num() <= 0 || !Damagers.GetData())
                continue;

            void* Causer = nullptr;
            int CauserDamage = 0;

            for (int d = Damagers.Num() - 1; d >= 0; d--)
            {
                auto& Entry = Damagers.Get(d, Stride);

                auto DamagerState = ResolveDamagerState(Entry.DamageCauser);
                if (!DamagerState || DamagerState == PS)
                    continue;

                Causer = (void*)DamagerState;
                CauserDamage = Entry.DamageAmount;
                break;
            }

            {
                auto Previous = BotTopDamager.find((void*)Controller);
                const bool bChanged = (Previous == BotTopDamager.end()) ||
                                      (Previous->second.first != Causer) ||
                                      (Previous->second.second != CauserDamage);

                if (bChanged && Causer)
                {
                    auto CauserState = (AFortPlayerStateAthena*)Causer;
                    if (CauserState->HasbIsABot() && CauserState->bIsABot)
                    {
                        GCombatBotOnBotHits++;

                        if (CauserState->HasOwner() && CauserState->Owner)
                        {
                            auto ShooterPawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)CauserState->Owner)->Pawn;
                            auto ShooterLock = BotCombatTarget.find((void*)ShooterPawn);

                            if (ShooterLock != BotCombatTarget.end() && ShooterLock->second == Pawn)
                            {
                                GHitsOnLockedTarget++;
                            }
                            else
                            {
                                GHitsOnSomebodyElse++;

                                if (GHitsOnSomebodyElse <= 10)
                                {
                                    auto LockName = BotLockedOnName.find((void*)ShooterPawn);

                                    LogLine("[HITS] '%s' hit '%s', but the one it is locked onto is '%s'.",
                                            SafeNameOf(CauserState).c_str(), SafeNameOf(PS).c_str(),
                                            LockName != BotLockedOnName.end() ? LockName->second.c_str() : "nobody");
                                }
                            }
                        }
                    }
                    else
                    {

                        AggroedByHuman[(void*)Controller] = { Causer, GStormTick };
                    }
                }
            }

            BotTopDamager[(void*)Controller] = { Causer, CauserDamage };

            if (Causer)
            {
                auto PrevLast = BotLastDamager.find((void*)Controller);
                const bool bNewDamager = (PrevLast == BotLastDamager.end()) || (PrevLast->second.first != Causer);

                BotLastDamager[(void*)Controller] = { Causer, GStormTick };

                BotTopDamager[(void*)Controller] = { Causer, CauserDamage };

                BotUnderFireSince[(void*)Controller] = GStormTick;

                if (bNewDamager)
                    BotAlreadyBuiltWall.erase((void*)Controller);
            }
        }
    }

    static void TrackEveryBotAlive(AFortGameStateAthena* GameState, int TickNow)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Controller = (AActor*)PS->Owner;
            if (!Controller)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Controller)->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f)
                continue;

            if (BotTopDamager.find((void*)Controller) == BotTopDamager.end())
                BotTopDamager[(void*)Controller] = { nullptr, 0 };

            auto Tracked = BotTopDamager.find((void*)Controller);
            if (Tracked != BotTopDamager.end() && Tracked->second.first)
            {
                static std::map<void*, std::pair<void*, int>> LastSeenLedger;

                auto Seen = LastSeenLedger.find((void*)Controller);
                if (Seen == LastSeenLedger.end() || Seen->second != Tracked->second)
                {
                    LastSeenLedger[(void*)Controller] = Tracked->second;
                    LastPlayerDamageTick[(void*)Controller] = TickNow;
                }
            }
        }
    }

    static FGameplayTagContainer* ResolveDamageTags(AFortPlayerPawnAthena* Pawn, FGameplayTagContainer& Fallback)
    {
        if (!Pawn)
            return &Fallback;

        const int32 Anchor = Pawn->GetOffset("MoveSoundStimulusBroadcastInterval");
        if (Anchor == -1)
            return &Fallback;

        const int32 Delta = (VersionInfo.FortniteVersion >= 11 && VersionInfo.FortniteVersion < 18) ? 0x18 : 0x10;
        auto Candidate = (FGameplayTagContainer*)(__int64(Pawn) + Anchor + Delta);

        if (IsBadReadPtr((void*)Candidate))
            return &Fallback;

        return Candidate;
    }

    static bool DiedToTheStorm(void* Controller, int TickNow)
    {
        if (!Controller)
            return false;

        auto Outside = LastOutsideZoneTick.find(Controller);
        if (Outside == LastOutsideZoneTick.end() || (TickNow - Outside->second) > 90)
            return false;

        auto Hit = LastPlayerDamageTick.find(Controller);
        if (Hit != LastPlayerDamageTick.end() && (TickNow - Hit->second) < 150)
            return false;

        return true;
    }

    static const int KillCreditWindowTicks = 15 * 30;

    static AFortPlayerStateAthena* FindBotKiller(AActor* BotController, AFortPlayerStateAthena* VictimState,
                                                 AFortGameStateAthena* GameState, const char** OutHow)
    {
        const char* How = "no damage from another player on record";
        AFortPlayerStateAthena* Killer = nullptr;

        if (PlayersAreProtected(GameState))
        {
            if (OutHow)
                *OutHow = "the match had not started";
            return nullptr;
        }

        auto Last = BotLastDamager.find((void*)BotController);
        if (Last != BotLastDamager.end() && Last->second.first)
        {
            if ((GStormTick - Last->second.second) <= KillCreditWindowTicks)
            {
                Killer = (AFortPlayerStateAthena*)Last->second.first;
                How = "the last player to damage it";
                GElimsFromLedger++;
            }
            else
            {
                How = "its last damage from another player was more than 15 seconds before it died";
            }
        }

        if (!Killer)
        {
            auto Tracked = BotTopDamager.find((void*)BotController);
            auto When = LastPlayerDamageTick.find((void*)BotController);

            if (Tracked != BotTopDamager.end() && Tracked->second.first && When != LastPlayerDamageTick.end() &&
                (GStormTick - When->second) <= KillCreditWindowTicks)
            {
                Killer = (AFortPlayerStateAthena*)Tracked->second.first;
                How = "the game's own damage list";
                GElimsFromLedger++;
            }
        }

        if (Killer == VictimState)
        {
            Killer = nullptr;
            How = "the only damage on record was its own";
        }

        if (OutHow)
            *OutHow = How;

        return Killer;
    }

    static std::set<void*> LootAlreadyDropped;

    static void DropBotLoot(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
    {
        if (!Controller)
            return;

        if (LootAlreadyDropped.find((void*)Controller) != LootAlreadyDropped.end())
            return;

        LootAlreadyDropped.insert((void*)Controller);

        auto VictimPS = (AFortPlayerStateAthena*)Controller->PlayerState;

        FVector dropLoc{};
        bool bHaveDropSpot = false;

        if (Pawn)
        {
            dropLoc = Pawn->K2_GetActorLocation();
            bHaveDropSpot = true;
        }
        else
        {
            auto Seen = LastKnownLocation.find((void*)Controller);

            if (Seen != LastKnownLocation.end())
            {
                dropLoc = Seen->second;
                bHaveDropSpot = true;
            }
        }

        if (!bHaveDropSpot)
        {
            if (VictimPS)
                BotInventories.erase((void*)VictimPS);

            static int noSpotReported = 0;
            if (++noSpotReported <= 3)
                LogLine("[DROP] A bot died with no pawn and no last known position -- nothing to drop onto.");

            return;
        }

        dropLoc.Z += 100.0;

        int droppedCount = 0;
        int fromInventory = 0;
        bool bDroppedMaterials = false;

        auto DropOne = [&](const UFortItemDefinition* Definition, int Count, int LoadedAmmo)
        {
            if (!Definition || Count <= 0)
                return;

            FVector spot = dropLoc;
            float ang = (float)(rand() % 360) * (3.14159265f / 180.f);
            spot.X += cosf(ang) * (float)(40 + (rand() % 90));
            spot.Y += sinf(ang) * (float)(40 + (rand() % 90));

            AFortInventory::SpawnPickup(spot, Definition, Count, LoadedAmmo,
                                        EFortPickupSourceTypeFlag::GetPlayer(),
                                        EFortPickupSpawnSource::GetPlayerElimination());
            droppedCount++;
        };

        if (VictimPS)
        {
            auto invIt = BotInventories.find((void*)VictimPS);

            if (invIt != BotInventories.end() && invIt->second)
            {
                auto Inv = invIt->second;
                auto& Entries = Inv->Inventory.ReplicatedEntries;

                for (int e = 0; e < Entries.Num(); e++)
                {
                    auto& Entry = Entries.Get(e, FFortItemEntry::Size());
                    if (!Entry.HasItemDefinition() || !Entry.ItemDefinition)
                        continue;

                    if (Entry.ItemDefinition->Cast<UFortWeaponMeleeItemDefinition>() || !Entry.ItemDefinition->CanBeDropped())
                        continue;

                    int cnt  = Entry.HasCount() ? Entry.Count : 1;
                    int ammo = Entry.HasLoadedAmmo() ? Entry.LoadedAmmo : 0;
                    if (cnt <= 0)
                        cnt = 1;

                    DropOne(Entry.ItemDefinition, cnt, ammo);
                    fromInventory++;
                }

                BotInventories.erase(invIt);
            }
        }

        if (droppedCount == 0 && Pawn && Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
        {
            auto Held = (AFortWeapon*)Pawn->CurrentWeapon;

            if (Held->HasWeaponData() && Held->WeaponData)
                DropOne((const UFortItemDefinition*)Held->WeaponData, 1, 30);
        }

        if (droppedCount == 0 && VictimPS)
        {
            auto wep = BotWeapons.find((void*)VictimPS);

            if (wep != BotWeapons.end() && wep->second)
            {
                int Clip = 30;
                auto ClipIt = BotWeaponClipSize.find((void*)VictimPS);

                if (ClipIt != BotWeaponClipSize.end() && ClipIt->second > 0)
                    Clip = ClipIt->second;

                DropOne(wep->second, 1, Clip);

                if (wep->second->Cast<UFortWeaponRangedItemDefinition>())
                {

                    static const wchar_t* SpareAmmo[] = {
                        L"AthenaAmmoDataBulletsMedium",
                        L"AthenaAmmoDataBulletsLight",
                        L"AthenaAmmoDataShells",
                        L"AthenaAmmoDataBulletsHeavy",
                    };

                    auto AmmoDef = FindItemDefByName(SpareAmmo[rand() % (int)(sizeof(SpareAmmo) / sizeof(SpareAmmo[0]))]);
                    DropOne(AmmoDef, 60, 0);
                }

                DropOne(FindItemDefByName(L"Athena_Bandage"), 3, 0);
            }
        }

        if (droppedCount > 0)
        {
            static const wchar_t* Materials[] = { L"WoodItemData", L"StoneItemData", L"MetalItemData" };

            auto MatDef = FindItemDefByName(Materials[rand() % 3]);

            if (MatDef)
            {
                DropOne(MatDef, 30 + (rand() % 60), 0);
                bDroppedMaterials = true;
            }
        }

        if (VictimPS)
        {
            BotWeapons.erase((void*)VictimPS);
            BotWeaponClipSize.erase((void*)VictimPS);
        }

        static int dropsReported = 0;
        if (++dropsReported <= 6)
            LogLine("[DROP] Bot died -- spawned %d pickup(s) at the death location (%d from its inventory; materials %s).",
                    droppedCount, fromInventory, bDroppedMaterials ? "yes" : "NO");
    }

    static std::string BotObjectName(const UObject* Object);

    struct FSettledDeath
    {
        float When = -1000.f;
        const void* PawnKey = nullptr;
        bool bCredited = false;
        std::string DecidedBy;
        std::string Outcome;
    };

    static std::map<void*, FSettledDeath> GSettledDeaths;

    static int GElimsCredited = 0;
    static int GElimsToNobody = 0;
    static int GElimsByGameReport = 0;
    static int GElimsBySweep = 0;
    static int GElimsSecondReports = 0;
    static int GElimPopupsSent = 0;
    static int GElimPopupsNotSent = 0;

    static bool DeathCauseIsWorkedOutHere()
    {
        return VersionInfo.EngineVersion < 4.19;
    }

    static std::string DeathCauseName(uint8 Cause)
    {
        static const char* Names[] = { "the storm", "a fall", "a pistol", "a shotgun", "a rifle", "an SMG", "a sniper",
                                       "melee", "a heavy weapon", "bleeding out", "the banhammer", "leaving the game",
                                       "unspecified" };

        if (DeathCauseIsWorkedOutHere() && Cause < (uint8)(sizeof(Names) / sizeof(Names[0])))
            return Names[Cause];

        return "cause " + std::to_string((int)Cause);
    }

    static uint8 DeathCauseFromWeapon(const UObject* WeaponDefinition)
    {
        if (!WeaponDefinition)
            return 255;

        std::string Lower = BotObjectName(WeaponDefinition);
        for (auto& Ch : Lower)
            Ch = (char)tolower((unsigned char)Ch);

        auto Has = [&](const char* Part) { return Lower.find(Part) != std::string::npos; };

        if (Has("pickaxe") || Has("harvest") || Has("melee"))
            return 7;
        if (Has("sniper"))
            return 6;
        if (Has("shotgun"))
            return 3;
        if (Has("smg") || Has("autoheavy") || Has("scavenger") || Has("machinepistol"))
            return 5;
        if (Has("pistol") || Has("revolver") || Has("handcannon") || Has("sixshooter"))
            return 2;
        if (Has("launcher") || Has("rocket") || Has("grenade") || Has("rpg"))
            return 8;
        if (Has("assault") || Has("rifle") || Has("minigun") || Has("lmg"))
            return 4;

        return 255;
    }

    static AFortPlayerPawnAthena* PawnOfPlayerState(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState || !PlayerState->HasOwner() || !PlayerState->Owner)
            return nullptr;

        auto Owner = (AActor*)PlayerState->Owner;

        if (!Owner->IsA<AFortPlayerControllerAthena>())
            return nullptr;

        return (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
    }

    static const UObject* KillingWeaponDefinition(AActor* DamageCauser, AFortPlayerPawnAthena* KillerPawn)
    {
        if (DamageCauser && DamageCauser->IsA<AFortWeapon>())
        {
            auto Weapon = (AFortWeapon*)DamageCauser;

            if (Weapon->HasWeaponData() && Weapon->WeaponData)
                return (const UObject*)Weapon->WeaponData;
        }

        if (DamageCauser && DamageCauser->IsA<AFortPlayerPawnAthena>() && !KillerPawn)
            KillerPawn = (AFortPlayerPawnAthena*)DamageCauser;

        if (KillerPawn && KillerPawn->HasCurrentWeapon() && KillerPawn->CurrentWeapon)
        {
            auto Weapon = (AFortWeapon*)KillerPawn->CurrentWeapon;

            if (Weapon->HasWeaponData() && Weapon->WeaponData)
                return (const UObject*)Weapon->WeaponData;
        }

        return nullptr;
    }

    static bool ReportKillTo(AFortPlayerStateAthena* Killer, AFortPlayerStateAthena* Victim, const std::string& VictimName)
    {
        static UFunction* Fn = nullptr;
        static int Kind = -1;
        static uint32 ParamOffset = 0;
        static uint32 ParamsSize = 0;

        if (!Killer || !Victim)
            return false;

        if (Kind == -1)
        {
            Kind = 2;
            Fn = Killer->GetFunction("ClientReportKill");

            if (Fn)
            {
                auto Params = Fn->GetParamsNamed();
                ParamsSize = Params.Size;

                if (!Params.NameOffsetMap.empty())
                {
                    const auto& First = Params.NameOffsetMap[0];
                    ParamOffset = First.Offset;

                    if (First.ElementSize == 0x10)
                        Kind = 0;
                    else if (First.ElementSize == 8)
                        Kind = 1;

                    LogLine("[ELIM] ClientReportKill on this build takes '%s' (%u bytes) -- the \"Eliminated\" popup is sent %s.",
                            First.Name.c_str(), (unsigned)First.ElementSize,
                            Kind == 0 ? "the victim's name" : (Kind == 1 ? "the victim's player state" : "nothing (unknown kind -- no popup)"));
                }
            }
            else
            {
                LogLine("[ELIM] ClientReportKill is not on this build -- no \"Eliminated\" popup.");
            }
        }

        alignas(16) uint8 Buffer[0x110] = {};

        if (Kind == 2 || !Fn || ParamsSize == 0 || ParamsSize > 0x100 || ParamOffset + 0x10 > (uint32)sizeof(Buffer))
            return false;

        if (Kind == 1)
        {
            AFortPlayerStateAthena* VictimPointer = Victim;
            memcpy(Buffer + ParamOffset, &VictimPointer, sizeof(VictimPointer));
            Killer->ProcessEvent(Fn, Buffer);
            return true;
        }

        std::wstring Wide;

        if (const FString* Raw = PlayerNameField(Victim))
            if (Raw->CStr() && Raw->Num() > 1)
                Wide = std::wstring(Raw->CStr());

        if (Wide.empty() && !VictimName.empty())
        {
            const int Needed = MultiByteToWideChar(CP_UTF8, 0, VictimName.c_str(), (int)VictimName.size(), nullptr, 0);

            if (Needed > 0)
            {
                Wide.resize((size_t)Needed);
                MultiByteToWideChar(CP_UTF8, 0, VictimName.c_str(), (int)VictimName.size(), &Wide[0], Needed);
            }
        }

        if (Wide.empty())
            return false;

        FString Name(Wide.c_str());
        memcpy(Buffer + ParamOffset, (const void*)&Name, 0x10);

        Killer->ProcessEvent(Fn, Buffer);

        if (Name.Data)
            FMemory::Free(Name.Data);

        return true;
    }

    struct FDeathFacts
    {
        AFortPlayerStateAthena* Victim = nullptr;
        AFortPlayerStateAthena* Killer = nullptr;
        AFortPlayerPawnAthena* VictimPawn = nullptr;
        const void* PawnKey = nullptr;
        AFortPlayerPawnAthena* KillerPawn = nullptr;
        AActor* DamageCauser = nullptr;
        int Storm = -1;
        bool bFromGameReport = false;
        std::string How;
        FVector Where{};
        bool bHaveWhere = false;
    };

    static bool DeathIsAlreadySettled(AFortPlayerStateAthena* Victim, const void* PawnKey, float Now, const FSettledDeath** Out)
    {
        auto Done = GSettledDeaths.find((void*)Victim);

        if (Done == GSettledDeaths.end())
            return false;

        const bool bSameBody = PawnKey && Done->second.PawnKey == PawnKey;
        const bool bJustNow = (Now - Done->second.When) < 10.f;

        if (!bSameBody && !bJustNow)
            return false;

        if (Out)
            *Out = &Done->second;

        return true;
    }

    static bool SettleElimination(FDeathFacts Facts)
    {
        auto Victim = Facts.Victim;
        if (!Victim)
            return false;

        auto World = UWorld::GetWorld();
        auto GameState = (World && World->GameState) ? (AFortGameStateAthena*)World->GameState : nullptr;
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        const char* DecidedBy = Facts.bFromGameReport ? "the game's own death report" : "the bot code's dead-bot sweep";
        const std::string VictimName = SafeNameOf(Victim);

        const FSettledDeath* Earlier = nullptr;

        if (DeathIsAlreadySettled(Victim, Facts.PawnKey, Now, &Earlier))
        {
            GElimsSecondReports++;

            if (GElimsSecondReports <= 20 || (GElimsSecondReports % 25) == 0)
                LogLine("[ELIM] '%s': %s reported this death as well -- %s already decided it %.1fs ago (%s). Nothing changed, "
                        "nobody gets a second kill (%d such second report(s) so far).",
                        VictimName.c_str(), DecidedBy, Earlier->DecidedBy.c_str(), Now - Earlier->When, Earlier->Outcome.c_str(),
                        GElimsSecondReports);
            return false;
        }

        auto Killer = Facts.Killer;
        std::string NoKillerBecause = Facts.How.empty() ? std::string("no damage from another player on record") : Facts.How;

        if (Killer == Victim)
        {
            Killer = nullptr;
            NoKillerBecause = "it was its own doing";
        }

        if (Killer && GameState && PlayersAreProtected(GameState))
        {
            Killer = nullptr;
            NoKillerBecause = "the match had not started";
        }

        if (Killer && !((UObject*)Killer)->IsA<AFortPlayerStateAthena>())
        {
            Killer = nullptr;
            NoKillerBecause = "what the game named as the killer is not a player";
        }

        int Storm = Facts.Storm;

        if (Storm < 0)
            Storm = (Victim->HasOwner() && Victim->Owner && DiedToTheStorm((void*)Victim->Owner, GStormTick)) ? 1 : 0;

        if (Killer && Storm == 1 && !Facts.bFromGameReport)
        {
            Killer = nullptr;
            NoKillerBecause = "it was outside the circle and nobody had hit it in the last 5 seconds -- the storm finished it";
        }

        if (!Killer && Storm == 1 && NoKillerBecause.find("storm") == std::string::npos)
            NoKillerBecause += ", in the storm";

        uint8 Cause = 0;
        std::string CauseFrom;

        if (DeathCauseIsWorkedOutHere())
        {
            if (Killer)
            {
                auto KillerPawn = Facts.KillerPawn ? Facts.KillerPawn : PawnOfPlayerState(Killer);
                const UObject* Weapon = KillingWeaponDefinition(Facts.DamageCauser, KillerPawn);
                const uint8 FromWeapon = DeathCauseFromWeapon(Weapon);

                if (FromWeapon != 255)
                {
                    Cause = FromWeapon;
                    CauseFrom = "from " + BotObjectName(Weapon);
                }
                else
                {
                    Cause = 5;
                    CauseFrom = Weapon ? ("could not tell from " + BotObjectName(Weapon) + ", so an SMG as before")
                                       : std::string("no weapon to read, so an SMG as before");
                }
            }
            else
            {
                Cause = (Storm == 1) ? 0 : 1;
                CauseFrom = (Storm == 1) ? "it was outside the circle" : "no killer and not the storm: the fall/void line";
            }
        }
        else
        {
            FGameplayTagContainer EmptyTags{};
            FGameplayTagContainer* SourceTags = ResolveDamageTags(Facts.VictimPawn, EmptyTags);

            Cause = (Storm == 1 && !Killer) ? 0 : ToDeathCause(Facts.VictimPawn, *SourceTags, false);

            if (Cause == 0 && Killer)
                Cause = 5;

            CauseFrom = "the game's own ToDeathCause";
        }

        if (Victim->HasDeathInfo())
        {
            FGameplayTagContainer EmptyTags{};
            FGameplayTagContainer* SourceTags = ResolveDamageTags(Facts.VictimPawn, EmptyTags);

            if (FDeathInfo::HasKiller())
                Victim->DeathInfo.Killer = Killer;
            if (FDeathInfo::HasFinisherOrDowner())
                Victim->DeathInfo.FinisherOrDowner = Killer ? (AActor*)Killer : (AActor*)Victim;
            if (FDeathInfo::HasbDBNO())
                Victim->DeathInfo.bDBNO = false;
            if (FDeathInfo::HasDeathLocation() && Facts.bHaveWhere)
                Victim->DeathInfo.DeathLocation = Facts.Where;
            if (!Facts.bFromGameReport)
            {

                if (FDeathInfo::HasDeathTags())
                    Victim->DeathInfo.DeathTags = *SourceTags;
                if (FDeathInfo::HasVictimTags() && Facts.VictimPawn)
                    Victim->DeathInfo.VictimTags = Facts.VictimPawn->GameplayTags;
            }
            if (FDeathInfo::HasDeathClassSlot())
                Victim->DeathInfo.DeathClassSlot = -1;
            if (FDeathInfo::HasDeathCause())
                Victim->DeathInfo.DeathCause = Cause;
            if (FDeathInfo::HasbInitialized())
                Victim->DeathInfo.bInitialized = true;

            Victim->OnRep_DeathInfo();
        }

        FSettledDeath& Settled = GSettledDeaths[(void*)Victim];
        Settled.When = Now;
        Settled.PawnKey = Facts.PawnKey;
        Settled.DecidedBy = DecidedBy;

        if (Facts.bFromGameReport)
            GElimsByGameReport++;
        else
            GElimsBySweep++;

        if (Killer)
        {
            const std::string KillerName = SafeNameOf(Killer);

            int Before = -1, After = -1;

            if (Killer->HasKillScore())
            {
                Before = (int)Killer->KillScore;
                Killer->KillScore++;
                After = (int)Killer->KillScore;
            }
            else if (Killer->HasKills())
            {
                Before = (int)Killer->Kills;
                Killer->Kills++;
                After = (int)Killer->Kills;
            }

            Killer->OnRep_Kills();

            if (Killer->HasTeamKillScore())
            {
                Killer->TeamKillScore++;
                Killer->OnRep_TeamKillScore();
            }

            const bool bKillerIsHuman = !(Killer->HasbIsABot() && Killer->bIsABot) && !NPCs::IsNPC(Killer);
            const char* Popup = "a bot -- no screen, no popup";

            if (bKillerIsHuman)
            {
                if (ReportKillTo(Killer, Victim, VictimName))
                {
                    GElimPopupsSent++;
                    Popup = "\"Eliminated <name>\" sent with the victim's name";
                }
                else
                {
                    GElimPopupsNotSent++;
                    Popup = "the popup could NOT be sent (see the ClientReportKill line)";
                }

                if (Killer->HasTeamKillScore())
                    Killer->ClientReportTeamKill(Killer->TeamKillScore);
            }

            const bool bVictimIsBot = Victim->HasbIsABot() && Victim->bIsABot;

            if (bVictimIsBot && FConfiguration::SiphonAmount > 0)
            {
                auto KillerPawn = PawnOfPlayerState(Killer);

                if (KillerPawn && KillerPawn->GetHealth() > 0.f)
                {
                    float Health = KillerPawn->GetHealth();
                    float Shield = KillerPawn->GetShield();
                    float Remaining = (float)FConfiguration::SiphonAmount;

                    if (Health < 100.f)
                    {
                        float Need = 100.f - Health;
                        float Give = Remaining < Need ? Remaining : Need;
                        Health += Give;
                        Remaining -= Give;
                    }

                    if (Remaining > 0.f && Shield < 100.f)
                    {
                        float Need = 100.f - Shield;
                        float Give = Remaining < Need ? Remaining : Need;
                        Shield += Give;
                    }

                    KillerPawn->SetHealth(Health);
                    KillerPawn->SetShield(Shield);
                }
            }

            GElimsCredited++;
            Settled.bCredited = true;
            Settled.Outcome = "'" + KillerName + "' got the elim";

            LogLine("[ELIM] '%s' eliminated '%s' with %s (%s) -- decided by %s (%s) | %s's kills: %d -> %d | %s.",
                    KillerName.c_str(), VictimName.c_str(), DeathCauseName(Cause).c_str(), CauseFrom.c_str(), DecidedBy,
                    Facts.How.empty() ? "the killer it named" : Facts.How.c_str(), KillerName.c_str(), Before, After, Popup);

            return true;
        }

        const FString* Raw = PlayerNameField(Victim);
        const int Pick = NoKillerMessages::PickFor((Raw && Raw->CStr()) ? Raw->CStr() : L"", Raw ? Raw->Num() : 0);
        const std::string Line = NoKillerMessages::LineUtf8(Pick, VictimName.empty() ? std::string("someone") : VictimName);

        GElimsToNobody++;
        Settled.bCredited = false;
        Settled.Outcome = "nobody got the elim: \"" + Line + "\"";

        LogLine("[ELIM] %s -- nobody gets the elim (%s; decided by %s) | killfeed: %s (%s), no kill for anyone.",
                Line.c_str(), NoKillerBecause.c_str(), DecidedBy, DeathCauseName(Cause).c_str(), CauseFrom.c_str());

        return false;
    }

    struct FSweepDeath
    {
        AFortPlayerStateAthena* Victim = nullptr;
        FWeakObjectPtr VictimWeak{};
        AFortPlayerStateAthena* Killer = nullptr;
        FWeakObjectPtr KillerWeak{};
        const void* PawnKey = nullptr;
        std::string How;
        int Storm = 0;
        FVector Where{};
        bool bHaveWhere = false;
        float DecideAt = 0.f;
    };

    static std::vector<FSweepDeath> GSweepDeaths;

    static bool WeakStillThere(const FWeakObjectPtr& Weak, const void* Pointer)
    {
        if (!Pointer || Weak.ObjectIndex < 0 || Weak.Get() != (const UObject*)Pointer)
            return false;

        if (VersionInfo.EngineVersion < 5.0)
        {
            auto Item = TUObjectArray::GetItemByIndex(Weak.ObjectIndex);

            if (!Item || (Item->Flags & (0x20000000 | 0x10000000)))
                return false;
        }

        return true;
    }

    static void QueueSweepDeath(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, AFortGameStateAthena* GameState)
    {
        if (!Controller)
            return;

        auto Victim = (AFortPlayerStateAthena*)Controller->PlayerState;
        if (!Victim || !ActorIsLive((const UObject*)Victim))
            return;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        FSweepDeath Death;
        Death.Victim = Victim;
        Death.VictimWeak = FWeakObjectPtr((const UObject*)Victim);
        Death.PawnKey = (const void*)Pawn;

        const char* How = "";
        Death.Killer = FindBotKiller((AActor*)Controller, Victim, GameState, &How);
        Death.How = How ? How : "";

        if (Death.Killer)
            Death.KillerWeak = FWeakObjectPtr((const UObject*)Death.Killer);

        Death.Storm = DiedToTheStorm((void*)Controller, GStormTick) ? 1 : 0;

        if (Pawn)
        {
            Death.Where = Pawn->K2_GetActorLocation();
            Death.bHaveWhere = true;
        }
        else
        {
            auto Seen = LastKnownLocation.find((void*)Controller);

            if (Seen != LastKnownLocation.end())
            {
                Death.Where = Seen->second;
                Death.bHaveWhere = true;
            }
        }

        Death.DecideAt = Now + 1.0f;

        static int Reported = 0;
        if (++Reported <= 12)
            LogLine("[ELIM] '%s' is down (the bot code's sweep saw it%s) -- waiting a second for the game's own death report; the "
                    "sweep's own reading: %s (%s)%s.",
                    SafeNameOf(Victim).c_str(), Pawn ? "" : ", its body already gone",
                    Death.Killer ? ("'" + SafeNameOf(Death.Killer) + "'").c_str() : "nobody", Death.How.c_str(),
                    Death.Storm ? ", in the storm" : "");

        GSweepDeaths.push_back(Death);
    }

    static void DecideSweepDeaths()
    {
        if (GSweepDeaths.empty())
            return;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        std::vector<FSweepDeath> StillWaiting;

        for (auto& Death : GSweepDeaths)
        {
            if (Now < Death.DecideAt)
            {
                StillWaiting.push_back(Death);
                continue;
            }

            if (!WeakStillThere(Death.VictimWeak, Death.Victim))
            {
                LogLine("[ELIM] A bot the sweep saw go down is gone from the game before it could be decided -- nobody credited.");
                continue;
            }

            FDeathFacts Facts;
            Facts.Victim = Death.Victim;
            Facts.Killer = (Death.Killer && WeakStillThere(Death.KillerWeak, Death.Killer)) ? Death.Killer : nullptr;
            Facts.PawnKey = Death.PawnKey;
            Facts.Storm = Death.Storm;
            Facts.bFromGameReport = false;
            Facts.How = (Death.Killer && !Facts.Killer) ? std::string("the one who damaged it has left the game") : Death.How;
            Facts.Where = Death.Where;
            Facts.bHaveWhere = Death.bHaveWhere;

            SettleElimination(Facts);
        }

        GSweepDeaths.swap(StillWaiting);
    }

    bool SettleEliminationFromGameReport(AFortPlayerStateAthena* Victim, AFortPlayerStateAthena* Killer, AFortPlayerPawnAthena* VictimPawn,
                                         AFortPlayerPawnAthena* KillerPawn, AActor* DamageCauser)
    {
        FDeathFacts Facts;
        Facts.Victim = Victim;
        Facts.Killer = Killer;
        Facts.VictimPawn = VictimPawn;
        Facts.PawnKey = (const void*)VictimPawn;
        Facts.KillerPawn = KillerPawn;
        Facts.DamageCauser = DamageCauser;
        Facts.Storm = -1;
        Facts.bFromGameReport = true;
        Facts.How = Killer ? "the killer the game named" : "the game named no killer";

        if (!Killer && Victim && Victim->HasOwner() && Victim->Owner)
        {
            auto Hit = BotCodeLastHit.find((void*)Victim->Owner);

            if (Hit != BotCodeLastHit.end() && Hit->second.first && Hit->second.first != (void*)Victim &&
                (GStormTick - Hit->second.second) <= 5 * 30)
            {
                Facts.Killer = (AFortPlayerStateAthena*)Hit->second.first;
                Facts.KillerPawn = PawnOfPlayerState(Facts.Killer);
                Facts.How = "the game named no killer -- the bot that hit it with the bot code's own damage in the 5 seconds before";
            }
        }

        if (VictimPawn)
        {
            Facts.Where = VictimPawn->K2_GetActorLocation();
            Facts.bHaveWhere = true;
        }

        return SettleElimination(Facts);
    }

    static void LogEliminationSummary()
    {
        const int Decided = GElimsByGameReport + GElimsBySweep;

        if (Decided == 0 && GElimsSecondReports == 0)
            return;

        LogLine("[ELIM] Deaths decided: %d (by the game's own death report %d, by the bot code's sweep %d) | kills credited %d | "
                "to nobody %d | second reports of a decided death, ignored %d | \"Eliminated <name>\" popups sent %d, not sent %d | "
                "sweep kills traced to real damage %d.",
                Decided, GElimsByGameReport, GElimsBySweep, GElimsCredited, GElimsToNobody, GElimsSecondReports,
                GElimPopupsSent, GElimPopupsNotSent, GElimsFromLedger);
    }

    void LogFromClient(AFortPlayerControllerAthena* Controller, const char* Text)
    {
        if (!Controller || !Text)
            return;

        struct FBudget
        {
            float WindowStart = -100.f;
            int Used = 0;
            int Dropped = 0;
        };

        static std::map<void*, FBudget> Budgets;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        auto PS = (AFortPlayerStateAthena*)Controller->PlayerState;
        const std::string Who = PS ? SafeNameOf(PS) : std::string("?");

        FBudget& Budget = Budgets[(void*)Controller];

        if (Now - Budget.WindowStart >= 10.f)
        {
            if (Budget.Dropped > 0)
                LogLine("[CLIENT] ('%s' sent %d more line(s) in those 10 seconds -- not written, flood guard.)", Who.c_str(), Budget.Dropped);

            Budget.WindowStart = Now;
            Budget.Used = 0;
            Budget.Dropped = 0;
        }

        if (++Budget.Used > 40)
        {
            Budget.Dropped++;
            return;
        }

        std::string Clean;

        for (const char* p = Text; *p && Clean.size() < 600; p++)
        {
            const unsigned char Ch = (unsigned char)*p;
            Clean += (Ch >= 0x20 && Ch != 0x7F) ? (char)Ch : ' ';
        }

        LogLine("[CLIENT] '%s': %s", Who.c_str(), Clean.c_str());
    }

    struct FSpeedWatch
    {
        FWeakObjectPtr Weak{};
        std::string Name;
        float Multiplier = 1.f;
        float Since = -1.f;
        double Fastest = 0.0;
        int Moving = 0;
    };

    static std::map<void*, FSpeedWatch> GSpeedWatches;

    static float SpeedMultiplierNow(AFortPlayerPawnAthena* Pawn, float* OutBase)
    {
        static int32 SetOffset = -2, AttributeOffset = -2, CurrentOffset = -1, BaseOffset = -1;

        if (OutBase)
            *OutBase = -1.f;

        if (!Pawn)
            return -1.f;

        if (SetOffset == -2)
        {
            const uint32 Off = ((UObject*)Pawn)->GetOffset("MovementSet");
            SetOffset = (Off == (uint32)-1) ? -1 : (int32)Off;

            auto Data = FindStruct("GameplayAttributeData");
            const uint32 Current = Data ? Data->GetOffset("CurrentValue") : (uint32)-1;
            const uint32 Base = Data ? Data->GetOffset("BaseValue") : (uint32)-1;

            CurrentOffset = (Current == (uint32)-1) ? -1 : (int32)Current;
            BaseOffset = (Base == (uint32)-1) ? -1 : (int32)Base;
        }

        if (SetOffset < 0 || CurrentOffset < 0)
            return -1.f;

        auto Set = GetFromOffset<UObject*>(Pawn, (uint32)SetOffset);
        if (!Set)
            return -1.f;

        if (AttributeOffset == -2)
        {
            const uint32 Off = Set->GetOffset("SpeedMultiplier");
            AttributeOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
        }

        if (AttributeOffset < 0)
            return -1.f;

        if (OutBase && BaseOffset >= 0)
            *OutBase = GetFromOffset<float>(Set, (uint32)(AttributeOffset + BaseOffset));

        return GetFromOffset<float>(Set, (uint32)(AttributeOffset + CurrentOffset));
    }

    void WatchPlayerSpeed(AFortPlayerControllerAthena* Controller, float Multiplier)
    {
        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        auto Pawn = Controller ? (AFortPlayerPawnAthena*)Controller->Pawn : nullptr;
        auto State = Controller ? (AFortPlayerStateAthena*)Controller->PlayerState : nullptr;

        float Base = -1.f;
        const float Current = SpeedMultiplierNow(Pawn, &Base);

        if (!Controller)
            return;

        FSpeedWatch Watch{};
        Watch.Weak = FWeakObjectPtr((const UObject*)Controller);
        Watch.Name = State ? SafeNameOf(State) : std::string("?");
        Watch.Multiplier = Multiplier;
        Watch.Since = Now;

        GSpeedWatches[(void*)Controller] = Watch;

        if (Current < 0.f)
            LogLine("[SPEED] '%s' typed \"cheat speed %.2f\" -- the game's own SetMovementSpeedMultiplier was called, but the movement set's "
                    "speed multiplier cannot be read back on this build. How fast they really move over the next 20 seconds follows.",
                    Watch.Name.c_str(), Multiplier);
        else
            LogLine("[SPEED] '%s' typed \"cheat speed %.2f\" -- the game's own SetMovementSpeedMultiplier was called; the movement set's speed "
                    "multiplier on the server reads %.2f now (base %.2f). How fast they really move over the next 20 seconds follows.",
                    Watch.Name.c_str(), Multiplier, Current, Base);
    }

    static void TickSpeedWatch()
    {
        if (GSpeedWatches.empty())
            return;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        for (auto It = GSpeedWatches.begin(); It != GSpeedWatches.end();)
        {
            FSpeedWatch& Watch = It->second;

            if (!WeakStillThere(Watch.Weak, It->first))
            {
                It = GSpeedWatches.erase(It);
                continue;
            }

            auto Controller = (AFortPlayerControllerAthena*)It->first;
            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

            if (Pawn && Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
            {
                auto Move = Pawn->CharacterMovement;

                if (Move->HasMovementMode() && Move->MovementMode == 1 && Move->HasVelocity())
                {
                    const FVector Velocity = Move->Velocity;
                    const double Flat = sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y);

                    if (Flat > 50.0)
                    {
                        Watch.Moving++;

                        if (Flat > Watch.Fastest)
                            Watch.Fastest = Flat;
                    }
                }
            }

            if (Now - Watch.Since < 20.f)
            {
                ++It;
                continue;
            }

            float Base = -1.f;
            const float Current = SpeedMultiplierNow(Pawn, &Base);

            if (Watch.Moving == 0)
                LogLine("[SPEED] '%s' after \"cheat speed %.2f\": they did not run on foot in the 20 seconds after it (speed multiplier now %.2f).",
                        Watch.Name.c_str(), Watch.Multiplier, Current);
            else
                LogLine("[SPEED] '%s' after \"cheat speed %.2f\": fastest on foot in the 20 seconds after it %.0f units a second (%.1f m/s, "
                        "seen moving on %d tick(s) on the server) | speed multiplier now %.2f (base %.2f).",
                        Watch.Name.c_str(), Watch.Multiplier, Watch.Fastest, Watch.Fastest / 100.0, Watch.Moving, Current, Base);

            It = GSpeedWatches.erase(It);
        }
    }

    static void HandleBotDeath(AFortPlayerControllerAthena* DeadBotController)
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !DeadBotController)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        GTickStage = "the dead-bot sweep: reading a dead bot's body (HandleBotDeath)";

        auto DeadPawn = (AFortPlayerPawnAthena*)DeadBotController->Pawn;

        if (DeadPawn && !ActorIsLive((const UObject*)DeadPawn))
            DeadPawn = nullptr;

        GTickStage = "the dead-bot sweep: writing down a death (QueueSweepDeath)";
        QueueSweepDeath(DeadBotController, DeadPawn, GameState);

        GTickStage = "the dead-bot sweep: dropping a dead bot's loot (DropBotLoot)";
        DropBotLoot(DeadBotController, DeadPawn);

        GTickStage = "the dead-bot sweep (RemoveDeadBots)";

        BotTopDamager.erase((void*)DeadBotController);
        BotLastDamager.erase((void*)DeadBotController);
        BotUnderFireSince.erase((void*)DeadBotController);
        BotAlreadyBuiltWall.erase((void*)DeadBotController);
        LastPlayerDamageTick.erase((void*)DeadBotController);
        LastKnownHealth.erase((void*)DeadBotController);
        LastOutsideZoneTick.erase((void*)DeadBotController);

        ShotAtBy.erase((void*)DeadBotController);
        AggroedByHuman.erase((void*)DeadBotController);

        LandedBots.erase((void*)DeadBotController);
        LastKnownLocation.erase((void*)DeadBotController);
    }

    static bool StillOnTheSpawnIsland(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasGamePhase())
            return false;

        return GameState->GamePhase < 3;
    }

    void ApplyDamageProtection()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            auto Owner = (AActor*)PS->Owner;
            if (!Owner)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
            if (!Pawn)
                continue;

            HasLanded((void*)Owner, Pawn);

            const bool bKnownBot = PS->HasbIsABot() && PS->bIsABot;
            const bool bKnownHuman = PS->HasbIsABot() && !PS->bIsABot && !NPCs::IsNPC(PS);

            if (Pawn->HasbIgnoreNextFallingDamage())
                Pawn->bIgnoreNextFallingDamage = true;

            Pawn->ResetFallingHeight();

            if (Pawn->HasLastFallDistance())
                Pawn->LastFallDistance = 0.f;

            const bool bProtected = !AircraftPhaseReached() || StillOnTheSpawnIsland(GameState);

            static bool bProtectionSeen = false;
            static bool bProtectionLifted = false;

            if (bProtected)
            {
                if (!bProtectionSeen)
                {
                    bProtectionSeen = true;
                    LogLine("[PROTECT] Spawn island: nobody can be damaged until the bus leaves (game phase %d).",
                            GameState->HasGamePhase() ? (int)GameState->GamePhase : -1);
                }

                if (Pawn->HasbCanBeDamaged())
                    Pawn->bCanBeDamaged = false;

                if (Pawn->HasbIsInvulnerable())
                    Pawn->bIsInvulnerable = true;

                const float Health = Pawn->GetHealth();

                if (bKnownBot && Health < 100.f)
                {
                    static int HurtReports = 0;

                    if (++HurtReports <= 5)
                        LogLine("[PROTECT] '%s' lost health on the spawn island even so (%.0f left, invulnerable: %s) -- %s.",
                                SafeNameOf(PS).c_str(), Health,
                                (Pawn->HasbIsInvulnerable() && Pawn->bIsInvulnerable) ? "yes" : "NO",
                                Health > 0.f ? "healed back to 100" : "it is dead, the protection did NOT stop it");
                }

                if (Health > 0.f && Health < 100.f)
                {
                    float Full = 100.f;
                    Pawn->SetHealth(Full);
                }

                continue;
            }

            if (bProtectionSeen && !bProtectionLifted)
            {
                bProtectionLifted = true;
                LogLine("[PROTECT] The bus is in the air (game phase %d) -- spawn island protection off, everyone can be damaged from here.",
                        GameState->HasGamePhase() ? (int)GameState->GamePhase : -1);
            }

            if (bKnownHuman)
            {
                auto Remembered = HumanPawnToppedUp.find((void*)Owner);
                const bool bFreshPawn = Remembered == HumanPawnToppedUp.end() || Remembered->second != (void*)Pawn;

                if (bFreshPawn)
                {
                    HumanPawnToppedUp[(void*)Owner] = (void*)Pawn;

                    HumanTopUpUntilTick[(void*)Owner] = GStormTick + 60;

                    LogLine("[PROTECT] Real player on a new pawn -- 100 health and 100 shield.");
                }

                auto Until = HumanTopUpUntilTick.find((void*)Owner);

                if (Until != HumanTopUpUntilTick.end() && GStormTick <= Until->second)
                {
                    float Full = 100.f;

                    if (Pawn->GetHealth() < 100.f)
                        Pawn->SetHealth(Full);

                    if (Pawn->GetShield() <= 0.f)
                        Pawn->SetShield(Full);
                }
                else if (Until != HumanTopUpUntilTick.end())
                {
                    HumanTopUpUntilTick.erase(Until);
                }
            }

            if (Pawn->HasbCanBeDamaged() && !Pawn->bCanBeDamaged)
                Pawn->bCanBeDamaged = true;

            if (Pawn->HasbIsInvulnerable())
            {
                if (bKnownHuman && FConfiguration::bGodModeForRealPlayer)
                {
                    Pawn->bIsInvulnerable = true;
                }
                else if (Pawn->bIsInvulnerable)
                {
                    Pawn->bIsInvulnerable = false;

                    if (bKnownBot)
                        GBotsMadeKillableAgain++;
                }
            }
        }

        static bool bAnnounced = false;
        if (!bAnnounced)
        {
            bAnnounced = true;
            LogLine("[PROTECT] Fall damage off for everyone, and nothing takes damage until the aircraft phase "
                    "starts. From that moment nobody is invulnerable and everyone can be shot out of the sky.");
        }

        static int LastReported = 0;
        if (GBotsMadeKillableAgain > LastReported)
        {
            LastReported = GBotsMadeKillableAgain;
            if (LastReported <= 10)
                LogLine("[PROTECT] Found a bot stuck on god mode and made it killable again (%d so far).", LastReported);
        }
    }

    static std::map<void*, FWeakObjectPtr> SeenAliveWeak;
    static int GBotsGoneBeforeSweep = 0;

    void RemoveDeadBots()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameState->HasPlayerArray() || !GameMode->HasAlivePlayers())
            return;

        DecideSweepDeaths();

        int removed = 0;
        float lowestHealthSeen = 100.0f;
        int botsChecked = 0;

        {
            std::set<void*> presentNow;
            for (int j = 0; j < GameMode->AlivePlayers.Num(); j++)
            {
                auto Ctrl = (AFortPlayerControllerAthena*)GameMode->AlivePlayers[j];
                if (!Ctrl || !Ctrl->PlayerState || !Ctrl->PlayerState->bIsABot || NPCs::IsNPC(Ctrl))
                    continue;

                auto P = (AFortPlayerPawnAthena*)Ctrl->Pawn;
                if (P)
                {
                    HasLanded((void*)Ctrl, P);
                    LastKnownLocation[(void*)Ctrl] = P->K2_GetActorLocation();
                }
                bool bAlive = ((P != nullptr) && (P->GetHealth() > 0.f)) || BotIsBeingFixed((void*)Ctrl);

                if (bAlive)
                {
                    presentNow.insert((void*)Ctrl);

                    if (SeenAliveWeak.find((void*)Ctrl) == SeenAliveWeak.end())
                        SeenAliveWeak.emplace((void*)Ctrl, FWeakObjectPtr((const UObject*)Ctrl));
                }
            }

            if (!PlayersAreProtected(GameState))
            {
                for (auto prev : SeenAliveBots)
                {
                    if (presentNow.find(prev) != presentNow.end())
                        continue;
                    if (EliminatedBots.find(prev) != EliminatedBots.end())
                        continue;

                    if (LandedBots.find(prev) == LandedBots.end())
                        continue;

                    EliminatedBots.insert(prev);
                    removed++;

                    auto Weak = SeenAliveWeak.find(prev);
                    const bool bStillThere = Weak != SeenAliveWeak.end() && WeakStillThere(Weak->second, prev);

                    if (Weak != SeenAliveWeak.end())
                        SeenAliveWeak.erase(Weak);

                    if (!bStillThere)
                    {
                        GBotsGoneBeforeSweep++;

                        if (GBotsGoneBeforeSweep <= 5)
                            LogLine("[ELIM] A bot dropped out of the alive list with its controller already gone from the game -- left to "
                                    "the game's own death report, nothing here touches it (%d so far).", GBotsGoneBeforeSweep);
                        continue;
                    }

                    HandleBotDeath((AFortPlayerControllerAthena*)prev);
                }
            }

            SeenAliveBots = presentNow;
        }

        for (int j = GameMode->AlivePlayers.Num() - 1; j >= 0; j--)
        {
            auto Controller = (AFortPlayerControllerAthena*)GameMode->AlivePlayers[j];
            if (!Controller || !Controller->PlayerState || !Controller->PlayerState->bIsABot || NPCs::IsNPC(Controller))
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (Pawn)
            {
                HasLanded((void*)Controller, Pawn);
                botsChecked++;
                float health = Pawn->GetHealth();
                if (health < lowestHealthSeen)
                    lowestHealthSeen = health;
            }

            bool bDead = (!Pawn || Pawn->GetHealth() <= 0.0f) && !PlayersAreProtected(GameState) &&
                         LandedBots.find((void*)Controller) != LandedBots.end() && !BotIsBeingFixed((void*)Controller);

            if (bDead && EliminatedBots.find((void*)Controller) == EliminatedBots.end())
            {
                EliminatedBots.insert((void*)Controller);
                removed++;

                GTickStage = "the dead-bot sweep: writing down a death (QueueSweepDeath)";
                QueueSweepDeath(Controller, Pawn, GameState);

                GTickStage = "the dead-bot sweep: dropping a dead bot's loot (DropBotLoot)";
                DropBotLoot(Controller, Pawn);

                GTickStage = "the dead-bot sweep (RemoveDeadBots)";

                BotTopDamager.erase((void*)Controller);
                BotLastDamager.erase((void*)Controller);
                LastPlayerDamageTick.erase((void*)Controller);
                LastKnownHealth.erase((void*)Controller);
                LastOutsideZoneTick.erase((void*)Controller);
                ShotAtBy.erase((void*)Controller);
                LandedBots.erase((void*)Controller);

                if (Pawn)
                {
                    BotLastZoneDistance.erase((void*)Pawn);
                    BotZoneMoveTick.erase((void*)Pawn);
                    BotFire.erase((void*)Pawn);
                    BotFacingYaw.erase((void*)Pawn);
                    BotReloads.erase((void*)Pawn);
                }

                LogLine("[BotAI] Bot eliminated (recorded, engine owns removal).\n");
            }
        }

        static int reportCounter = 0;
        if (((++reportCounter) % 300) == 0 && botsChecked > 0)
            LogLine("[BotAI] RemoveDeadBots: %d bot(s) checked, lowest health seen = %.1f, removed this pass = %d.\n",
                   botsChecked, lowestHealthSeen, removed);
    }

    void GiveBotPickaxes()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        static auto DefaultPickaxe = FindObject<UFortItemDefinition>(
            L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");

        static bool bReported = false;
        if (!bReported)
        {
            bReported = true;
            LogLine("[BotAI] Default pickaxe asset: %s\n", DefaultPickaxe ? "found" : "NOT FOUND");
        }
        if (!DefaultPickaxe)
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto OwnerActor = (AActor*)PlayerState->Owner;
            if (!OwnerActor)
                continue;

            if (PickaxeGiven.find((void*)OwnerActor) != PickaxeGiven.end())
                continue;

            AFortInventory* Inventory = nullptr;

            if (OwnerActor->IsA<AFortPlayerControllerAthena>())
            {
                Inventory = ((AFortPlayerControllerAthena*)OwnerActor)->WorldInventory;
            }
            else
            {
                Inventory = EnsureBotInventory(OwnerActor);
            }

            if (!Inventory)
            {
                static int noInvReported = 0;
                if (++noInvReported <= 3)
                    LogLine("[BotAI] Could not create an inventory for bot yet (no real player joined?).");
                continue;
            }

            PickaxeGiven.insert((void*)OwnerActor);
            Inventory->GiveItem(DefaultPickaxe);

            static int given = 0;
            if (++given <= 3)
                LogLine("[BotAI] Gave pickaxe to bot #%d.\n", given);
        }
    }

    static uint8 AllocateFreeTeam(AFortGameStateAthena* GameState)
    {
        for (int attempt = 0; attempt < 251; attempt++)
        {
            if (AFortGameMode::CurrentTeam < 3 || AFortGameMode::CurrentTeam > 253)
                AFortGameMode::CurrentTeam = 3;

            const uint8 candidate = AFortGameMode::CurrentTeam;

            AFortGameMode::CurrentTeam++;
            AFortGameMode::PlayersOnCurTeam = 0;

            bool bTaken = false;
            if (GameState->HasPlayerArray())
            {
                for (int i = 0; i < GameState->PlayerArray.Num(); i++)
                {
                    auto Other = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                    if (!Other || !Other->HasTeamIndex())
                        continue;

                    if ((uint8)Other->TeamIndex == candidate)
                    {
                        bTaken = true;
                        break;
                    }
                }
            }

            if (!bTaken)
                return candidate;
        }

        return 3;
    }

    void AssignBotTeams()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Ctrl = (AActor*)PS->Owner;
            if (!Ctrl)
                continue;

            if (BotTeamsAssigned.find((void*)Ctrl) != BotTeamsAssigned.end())
                continue;
            BotTeamsAssigned.insert((void*)Ctrl);

            uint8 team = AllocateFreeTeam(GameState);

            uint8 oldTeam = PS->HasTeamIndex() ? PS->TeamIndex : 0;

            if (PS->HasTeamIndex())
                PS->TeamIndex = team;
            if (PS->HasSquadId())
            {
                PS->SquadId = team;
                PS->OnRep_SquadId();
            }

            static int reported = 0;
            if (++reported <= 5)
                LogLine("[TEAM] Bot assigned team %d (was %d).", (int)team, (int)oldTeam);
        }
    }

    static const UFortItemDefinition* FindItemDefByName(const wchar_t* name)
    {
        if (!name || !*name)
            return nullptr;

        static std::map<std::wstring, const UFortItemDefinition*> cache;
        static std::map<std::wstring, std::chrono::steady_clock::time_point> missedAt;

        auto cached = cache.find(name);
        if (cached != cache.end())
            return cached->second;

        const auto NowTp = std::chrono::steady_clock::now();

        auto missed = missedAt.find(name);
        if (missed != missedAt.end() &&
            std::chrono::duration_cast<std::chrono::seconds>(NowTp - missed->second).count() < 30)
            return nullptr;

        std::wstring n(name);
        const UFortItemDefinition* found = nullptr;

        static auto AnyClass = UObject::StaticClass();

        auto TryPath = [&](const std::wstring& Path) -> bool
        {
            found = (const UFortItemDefinition*)FindObject(Path.c_str(), AnyClass);
            return found != nullptr;
        };

        auto StartsWith = [&](const wchar_t* Prefix) -> bool
        {
            return n.rfind(Prefix, 0) == 0;
        };

        const char* How = "";

        if (n[0] == L'/')
        {

            if (TryPath(n))
                How = "full path";
        }
        else
        {

            if (TryPath(n + L"." + n))
                How = "short path";

            std::wstring Likely;

            if (StartsWith(L"AthenaAmmoData") || StartsWith(L"AmmoData"))
                Likely = L"/Game/Athena/Items/Ammo/";
            else if (n == L"WoodItemData" || n == L"StoneItemData" || n == L"MetalItemData")
                Likely = L"/Game/Items/ResourcePickups/";
            else if (StartsWith(L"Athena_") && n.size() > 7)
                Likely = L"/Game/Athena/Items/Consumables/" + n.substr(7) + L"/";
            else if (StartsWith(L"WID_"))
                Likely = L"/Game/Athena/Items/Weapons/";

            if (!found && !Likely.empty() && TryPath(Likely + n + L"." + n))
                How = "its own folder";

            if (!found)
            {
                static const wchar_t* folders[] = {
                    L"/Game/Athena/Items/Weapons/",
                    L"/Game/Items/Weapons/",
                    L"/Game/Weapons/",
                    L"/Game/Athena/Items/Consumables/",
                    L"/Game/Items/Consumables/",
                    L"/Game/Athena/Items/Ammo/",
                    L"/Game/Items/Ammo/",
                    L"/Game/Items/ResourcePickups/",
                    L"/Game/Athena/Items/",
                };

                for (auto f : folders)
                {
                    if (Likely == f)
                        continue;

                    if (TryPath(std::wstring(f) + n + L"." + n))
                    {
                        How = "folder search";
                        break;
                    }
                }
            }

            if (!found && TryPath(n))
                How = "bare name";

            if (!found)
            {
                std::string narrowName(n.begin(), n.end());
                found = TUObjectArray::FindObject<UFortItemDefinition>(narrowName.c_str());
                if (found)
                    How = "already loaded";
            }
        }

        std::string narrow(n.begin(), n.end());

        if (found)
        {
            cache[n] = found;
            missedAt.erase(n);

            static int hitsReported = 0;
            if (++hitsReported <= 15)
                LogLine("[ASSET] Lookup '%s' -> FOUND (%s)", narrow.c_str(), How);
        }
        else
        {
            missedAt[n] = NowTp;

            static int missesReported = 0;
            if (++missesReported <= 30)
                LogLine("[ASSET] Lookup '%s' -> NOT FOUND in any known folder (will try again in 30s)", narrow.c_str());
        }

        return found;
    }

    static AFortInventory* EnsureBotInventory(AActor* BotControllerActor)
    {
        if (!BotControllerActor)
            return nullptr;

        auto BotCtrl = (AFortAthenaAIBotController*)BotControllerActor;

        if (BotCtrl->HasInventory() && BotCtrl->Inventory)
            return BotCtrl->Inventory;

        static TSubclassOf<AFortInventory> CachedInvClass{};
        static bool bClassResolved = false;

        if (!bClassResolved)
        {
            auto World = UWorld::GetWorld();
            if (World && World->GameState)
            {
                auto GS = (AFortGameStateAthena*)World->GameState;
                if (GS->HasPlayerArray())
                {
                    for (int i = 0; i < GS->PlayerArray.Num(); i++)
                    {
                        auto PS = (AFortPlayerStateAthena*)GS->PlayerArray[i];
                        if (!PS || (PS->HasbIsABot() && PS->bIsABot))
                            continue;

                        auto PC = (AActor*)PS->Owner;
                        if (!PC || !PC->IsA<AFortPlayerControllerAthena>())
                            continue;

                        auto RealPC = (AFortPlayerControllerAthena*)PC;
                        if (RealPC->HasWorldInventoryClass() && RealPC->WorldInventoryClass.ClassPtr)
                        {
                            CachedInvClass = RealPC->WorldInventoryClass;
                            bClassResolved = true;
                            LogLine("[INV] Resolved WorldInventoryClass from the real player -- can now build bot inventories.");
                        }
                        break;
                    }
                }
            }
        }

        if (!CachedInvClass.ClassPtr)
        {
            static bool bTriedDirectClass = false;
            if (!bTriedDirectClass)
            {
                bTriedDirectClass = true;

                auto DirectClass = FindClass("FortInventory");
                if (DirectClass)
                {
                    CachedInvClass = DirectClass;
                    LogLine("[INV] Resolved inventory class directly via FindClass(\"FortInventory\").");
                }
                else
                {
                    LogLine("[INV] FAILED: could not resolve an inventory class either from a player "
                            "controller or via FindClass. Bots cannot carry items.");
                }
            }
        }

        if (!CachedInvClass.ClassPtr)
            return nullptr;

        auto NewInv = UWorld::SpawnActor<AFortInventory>(CachedInvClass, FVector{}, FRotator{}, BotControllerActor);
        if (!NewInv)
        {
            static bool bSpawnFailReported = false;
            if (!bSpawnFailReported)
            {
                bSpawnFailReported = true;
                LogLine("[INV] SpawnActor<AFortInventory> returned null -- class resolved but the actor could not be created.");
            }
            return nullptr;
        }

        if (NewInv->HasInventoryType())
            NewInv->InventoryType = 0;

        if (BotCtrl->HasInventory())
            BotCtrl->Inventory = NewInv;

        static int created = 0;
        if (++created <= 3)
            LogLine("[INV] Created inventory for bot #%d.", created);

        return NewInv;
    }

    enum EBotGunKind
    {
        BotGun_None = 0,
        BotGun_Launcher,
        BotGun_Sniper,
        BotGun_Pistol,
        BotGun_Shotgun,
        BotGun_Smg,
        BotGun_Rifle,
        BotGun_KindCount
    };

    static const char* BotGunKindName(int Kind)
    {
        static const char* Names[] = { "none", "launcher", "sniper", "pistol", "shotgun", "SMG", "rifle" };
        return (Kind >= 0 && Kind < BotGun_KindCount) ? Names[Kind] : "?";
    }

    static std::string LowerObjectName(const UObject* Object)
    {
        std::string Name = Object ? BotObjectName(Object) : std::string();

        for (auto& Ch : Name)
            Ch = (char)tolower((unsigned char)Ch);

        return Name;
    }

    static int BotGunKindOf(const UFortItemDefinition* Def)
    {
        if (!Def || !((const UObject*)Def)->IsA<UFortWeaponItemDefinition>() || ((const UObject*)Def)->IsA<UFortWeaponMeleeItemDefinition>())
            return BotGun_None;

        const std::string Name = LowerObjectName((const UObject*)Def);
        auto Has = [&](const char* Part) { return Name.find(Part) != std::string::npos; };

        if (Name.rfind("wid_", 0) != 0 || Has("harvest") || Has("pickaxe") || Has("trap"))
            return BotGun_None;
        if (Has("grenade") && !Has("launcher"))
            return BotGun_None;

        if (Has("launcher") || Has("rocket") || Has("rpg"))
            return BotGun_Launcher;
        if (Has("sniper"))
            return BotGun_Sniper;
        if (Has("shotgun"))
            return BotGun_Shotgun;
        if (Has("smg") || Has("autoheavy") || Has("scavenger") || Has("machinepistol"))
            return BotGun_Smg;
        if (Has("pistol") || Has("revolver") || Has("handcannon") || Has("sixshooter"))
            return BotGun_Pistol;
        if (Has("assault") || Has("rifle") || Has("minigun") || Has("lmg"))
            return BotGun_Rifle;

        return BotGun_None;
    }

    static int BotGunRarity(const UFortItemDefinition* Def)
    {
        const std::string Name = LowerObjectName((const UObject*)Def);

        if (Name.find("_sr_") != std::string::npos)
            return 5;
        if (Name.find("_vr_") != std::string::npos)
            return 4;
        if (Name.find("_uc_") != std::string::npos)
            return 2;
        if (Name.find("_r_") != std::string::npos)
            return 3;
        if (Name.find("_c_") != std::string::npos)
            return 1;

        return 0;
    }

    static int BotGunScore(const UFortItemDefinition* Def)
    {
        const int Kind = BotGunKindOf(Def);
        return Kind == BotGun_None ? -1 : Kind * 10 + BotGunRarity(Def);
    }

    static int BotGunMagazine(const UFortItemDefinition* Def)
    {
        auto Weapon = Def ? Def->Cast<UFortWeaponItemDefinition>() : nullptr;
        auto Stats = Weapon ? AFortInventory::GetStats(Weapon) : nullptr;

        const int Clip = Stats ? (int)Stats->ClipSize : 0;
        return (Clip > 0 && Clip <= 1000) ? Clip : 1;
    }

    static UFortWorldItemDefinition* BotGunAmmo(const UFortItemDefinition* Def)
    {
        auto World = Def ? Def->Cast<UFortWorldItemDefinition>() : nullptr;
        if (!World || !World->GetFunction("GetAmmoWorldItemDefinition_BP"))
            return nullptr;

        return World->GetAmmoWorldItemDefinition_BP();
    }

    static bool InventoryHolds(AFortInventory* Inventory, const UFortItemDefinition* Def)
    {
        if (!Inventory || !Def)
            return false;

        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            if (Entries.Get(e, FFortItemEntry::Size()).ItemDefinition == Def)
                return true;
        }

        return false;
    }

    static bool GBotLootGroupResolved = false;
    static bool GBotLootGroupFound = false;
    static FName GBotLootGroup{};
    static int GBotKitsFromLoot = 0;
    static int GBotKitsFixed = 0;
    static int GBotLootGuns[BotGun_KindCount] = {};
    static int GBotLootRarity[6] = {};

    static bool ResolveBotLootGroup()
    {
        if (GBotLootGroupResolved)
            return GBotLootGroupFound;

        GBotLootGroupResolved = true;

        static const wchar_t* Candidates[] = { L"Loot_AthenaTreasure", L"Loot_Treasure", L"Loot_AthenaFloorLoot" };

        for (auto Candidate : Candidates)
        {
            FName Group(Candidate);
            auto It = TierDataMap.find((int32)Group.ComparisonIndex);

            if (It != TierDataMap.end() && It->second.Num() > 0)
            {
                GBotLootGroup = Group;
                GBotLootGroupFound = true;

                std::wstring Wide(Candidate);
                LogLine("[LOOT] Bots get their loot from the game's own loot tables: tier group %s (%d tier row(s)) -- the chests' "
                        "lootpool.", std::string(Wide.begin(), Wide.end()).c_str(), (int)It->second.Num());
                return true;
            }
        }

        LogLine("[LOOT] None of this build's chest tier groups (Loot_AthenaTreasure, Loot_Treasure, Loot_AthenaFloorLoot) has any "
                "loot table rows loaded (%zu tier group(s) loaded in all) -- bots get the old fixed kit instead.", TierDataMap.size());
        return false;
    }

    static void RollBotLoot(std::vector<const UFortItemDefinition*>& OutGuns, std::vector<std::pair<const UFortItemDefinition*, int>>& OutOther,
                            int& OutRolls)
    {
        OutRolls = 0;

        if (!ResolveBotLootGroup())
            return;

        bool KindTaken[BotGun_KindCount] = {};

        for (int Roll = 0; Roll < 8 && OutGuns.size() < 3; Roll++)
        {
            TArray<FFortItemEntry*> Drops{};
            UFortLootPackage::ChooseLootForContainer(Drops, GBotLootGroup);
            OutRolls++;

            for (int d = 0; d < Drops.Num(); d++)
            {
                FFortItemEntry* Entry = Drops[d];
                if (!Entry)
                    continue;

                const UFortItemDefinition* Def = Entry->ItemDefinition;
                const int Count = Entry->Count;
                free(Entry);

                if (!Def)
                    continue;

                const int Kind = BotGunKindOf(Def);

                if (Kind != BotGun_None)
                {
                    if (!KindTaken[Kind] && OutGuns.size() < 3)
                    {
                        KindTaken[Kind] = true;
                        OutGuns.push_back(Def);
                    }

                    continue;
                }

                if (((const UObject*)Def)->IsA<UFortAmmoItemDefinition>() || ((const UObject*)Def)->IsA<UFortResourceItemDefinition>() ||
                    ((const UObject*)Def)->IsA<UFortWeaponItemDefinition>() || LowerObjectName((const UObject*)Def).find("trap") != std::string::npos)
                    continue;

                if (OutOther.size() < 2)
                    OutOther.push_back({ Def, Count > 0 ? Count : 1 });
            }

            Drops.Free();

            if (Roll >= 2 && OutGuns.size() >= 2)
                break;
        }
    }

    static std::string GiveBotSpareRounds(AFortInventory* Inventory, const UFortItemDefinition* Gun)
    {
        auto Ammo = BotGunAmmo(Gun);
        if (!Ammo)
            return " + no ammo type found";

        if (InventoryHolds(Inventory, Ammo))
            return std::string();

        int Spare = BotGunMagazine(Gun) * 4;
        if (Spare < 6)
            Spare = 6;

        const int MaxStack = Ammo->GetMaxStackSize();
        if (MaxStack > 0 && Spare > MaxStack)
            Spare = MaxStack;

        Inventory->GiveItem(Ammo, Spare, 0);
        return " + " + BotObjectName((const UObject*)Ammo) + " x" + std::to_string(Spare);
    }

    static int GiveBotLootpoolKit(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState)
    {
        auto Inventory = Controller ? Controller->WorldInventory : nullptr;
        if (!Inventory)
            return 0;

        std::vector<const UFortItemDefinition*> Guns;
        std::vector<std::pair<const UFortItemDefinition*, int>> Other;
        int Rolls = 0;

        RollBotLoot(Guns, Other, Rolls);

        if (Guns.empty())
            return 0;

        std::string Carried;

        for (auto Gun : Guns)
        {
            const int Clip = BotGunMagazine(Gun);
            Inventory->GiveItem(Gun, 1, Clip);

            GBotLootGuns[BotGunKindOf(Gun)]++;
            GBotLootRarity[BotGunRarity(Gun)]++;

            if (!Carried.empty())
                Carried += ", ";
            Carried += BotObjectName((const UObject*)Gun) + " (" + std::to_string(Clip) + ")";
            Carried += GiveBotSpareRounds(Inventory, Gun);
        }

        for (auto& [Def, Count] : Other)
        {
            int Give = Count;
            const int MaxStack = Def->GetMaxStackSize();

            if (MaxStack > 0 && Give > MaxStack)
                Give = MaxStack;

            Inventory->GiveItem(Def, Give, 0);
            Carried += ", " + BotObjectName((const UObject*)Def) + " x" + std::to_string(Give);
        }

        GBotKitsFromLoot++;

        if (GBotKitsFromLoot <= 12 || (GBotKitsFromLoot % 25) == 0)
            LogLine("[LOOT] '%s' looted %d chest(s)' worth: %s.", SafeNameOf(PlayerState).c_str(), Rolls, Carried.c_str());

        return (int)Guns.size();
    }

    static void LogBotLootSummary()
    {
        if (GBotKitsFromLoot == 0 && GBotKitsFixed == 0)
            return;

        LogLine("[LOOT] Bot kits from the lootpool: %d, the old fixed kit: %d | guns handed out: rifles %d, SMGs %d, shotguns %d, "
                "pistols %d, snipers %d, launchers %d | rarity: common %d, uncommon %d, rare %d, epic %d, legendary %d.",
                GBotKitsFromLoot, GBotKitsFixed, GBotLootGuns[BotGun_Rifle], GBotLootGuns[BotGun_Smg], GBotLootGuns[BotGun_Shotgun],
                GBotLootGuns[BotGun_Pistol], GBotLootGuns[BotGun_Sniper], GBotLootGuns[BotGun_Launcher], GBotLootRarity[1],
                GBotLootRarity[2], GBotLootRarity[3], GBotLootRarity[4], GBotLootRarity[5]);
    }

    static void NoteFixedBotKit(AFortPlayerStateAthena* PlayerState, const char* Why)
    {
        GBotKitsFixed++;

        if (GBotKitsFixed <= 5 || (GBotKitsFixed % 25) == 0)
            LogLine("[LOOT] '%s' got the old fixed kit (%s) -- %d bot(s) so far.", PlayerState ? SafeNameOf(PlayerState).c_str() : "a bot",
                    Why ? Why : "the loot tables gave no gun", GBotKitsFixed);
    }

    void GiveBotLoadouts()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        static const wchar_t* FixedWeapons[] = {
            L"WID_Assault_AutoHigh_Athena_SR_Ore_T03",
            L"WID_Shotgun_Standard_Athena_C_Ore_T03",
            L"WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03",
        };
        static const wchar_t* FixedConsumables[] = {
            L"Athena_PurpleStuff",
            L"Athena_PurpleStuff",
            L"Athena_Grenade",
        };

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto OwnerActor = PS->Owner;
            if (!OwnerActor)
                continue;

            if (LoadoutGiven.find((void*)PS) != LoadoutGiven.end())
                continue;

            auto Controller = OwnerActor->Cast<AFortPlayerControllerAthena>();
            if (!Controller || !Controller->Pawn)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn)
                continue;

            if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
                continue;

            if (Controller->WorldInventory && GiveBotLootpoolKit(Controller, PS) > 0)
            {
                BotInventories[(void*)PS] = Controller->WorldInventory;
                LoadoutGiven.insert((void*)PS);
                continue;
            }

            auto Inv = EnsureBotInventory(OwnerActor);
            if (Inv)
                BotInventories[(void*)PS] = Inv;

            bool gaveAny = false;
            FGuid primaryGuid{};

            for (auto WName : FixedWeapons)
            {
                auto WeaponDef = FindItemDefByName(WName);
                if (!WeaponDef)
                {
                    std::wstring wn(WName);
                    std::string nn(wn.begin(), wn.end());
                    static int miss = 0;
                    if (miss++ < 8)
                        LogLine("[LOADOUT] Missing weapon '%s' on this build.", nn.c_str());
                    continue;
                }

                int ClipSize = 30;
                if (auto RangedDef = WeaponDef->Cast<UFortWeaponItemDefinition>())
                {
                    if (auto Stats = AFortInventory::GetStats(RangedDef))
                        if (Stats->ClipSize > 0)
                            ClipSize = Stats->ClipSize;
                }

                if (Inv)
                {
                    auto Item = Inv->GiveItem(WeaponDef, 1, ClipSize);
                    if (Item && Item->HasItemEntry() && FFortItemEntry::HasItemGuid())
                    {
                        if (!gaveAny)
                            primaryGuid = Item->ItemEntry.ItemGuid;
                        gaveAny = true;
                    }

                    GiveBotSpareRounds(Inv, WeaponDef);
                }

                if (!gaveAny || primaryGuid.A == 0)
                {
                    auto Equipped = Pawn->EquipWeaponDefinition(WeaponDef, primaryGuid);
                    (void)Equipped;
                }
            }

            if (auto FirstDef = FindItemDefByName(FixedWeapons[0]))
            {
                Pawn->EquipWeaponDefinition(FirstDef, primaryGuid);
                BotWeaponClipSize[(void*)PS] = 30;
            }

            for (auto CName : FixedConsumables)
            {
                auto ConsDef = FindItemDefByName(CName);
                if (!ConsDef)
                {
                    std::wstring wn(CName);
                    std::string nn(wn.begin(), wn.end());
                    static int missc = 0;
                    if (missc++ < 6)
                        LogLine("[LOADOUT] Missing consumable '%s' on this build.", nn.c_str());
                    continue;
                }
                if (Inv)
                    Inv->GiveItem(ConsDef, 1, 0);
            }

            LoadoutGiven.insert((void*)PS);
            NoteFixedBotKit(PS, Controller->WorldInventory ? "the loot tables gave no gun" : "it has no inventory of its own");
        }
    }

    void RegisterBotsAsAlive()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameState->HasPlayerArray() || !GameMode->HasAlivePlayers())
            return;

        int newlyAdded = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AActor*)PlayerState->Owner;
            if (!Controller)
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            bool alreadyIn = false;
            for (int j = 0; j < GameMode->AlivePlayers.Num(); j++)
            {
                if (GameMode->AlivePlayers[j] == Controller)
                {
                    alreadyIn = true;
                    break;
                }
            }
            if (!alreadyIn)
            {
                if (!bBusDeparted)
                {
                    GameMode->AlivePlayers.Add(Controller);
                    newlyAdded++;
                }
            }
        }

        if (!bBusDeparted && GameState->HasPlayersLeft())
        {
            int32 realCount = GameMode->AlivePlayers.Num();
            if (GameState->PlayersLeft != realCount)
            {
                GameState->PlayersLeft = realCount;

                GameState->OnRep_PlayersLeft();

                if (GameState->HasTeamsLeft())
                    GameState->TeamsLeft = realCount;
            }
        }

        if (newlyAdded > 0)
            if (!bBusDeparted)
                LogLine("[BotAI] Registered %d new bot(s); player count now %d.\n", newlyAdded, GameMode->AlivePlayers.Num());
    }

    struct FBotNetId
    {
        bool bGiven = false;
        bool bShared = false;
    };

    static std::map<void*, FBotNetId> GBotNetIds;
    static int GNetIdState = 0;
    static std::vector<uint8> GNetIdPrefix;
    static bool GNetIdPacked = false;
    static void* GNetIdVTable = nullptr;
    static void* GNetIdObject = nullptr;
    static void* GNetIdController = nullptr;
    static uint32 GNetIdCounter = 0;
    static uint32 GNetIdMatchKey = 0;
    static int GNetIdsGiven = 0;
    static int GNetIdsRefused = 0;
    static int GNetIdsTakenBack = 0;
    static int GBotPlayerNumbers = 0;

    static std::string HexOfBytes(const uint8* Bytes, int Count)
    {
        std::string Out;
        char Pair[4];

        for (int i = 0; Bytes && i < Count && i < 96; i++)
        {
            snprintf(Pair, sizeof(Pair), "%02X", (unsigned)Bytes[i]);
            Out += Pair;
        }

        if (Count > 96)
            Out += "...";

        return Out;
    }

    static bool SkipArchiveString(const uint8* Bytes, int Count, int& At)
    {
        if (Count - At < 4)
            return false;

        int32 SaveNum = 0;
        memcpy(&SaveNum, Bytes + At, sizeof(SaveNum));
        At += 4;

        if (SaveNum == 0)
            return true;

        const long long Size = SaveNum > 0 ? (long long)SaveNum : -(long long)SaveNum * 2;

        if (Size > 1024 || (long long)(Count - At) < Size)
            return false;

        At += (int)Size;
        return true;
    }

    static bool ReadNetIdFormat(const uint8* Bytes, int Count, std::vector<uint8>& OutPrefix, bool& bOutPacked)
    {
        if (!Bytes || Count < 2)
            return false;

        int At = 0;
        const uint8 Flags = Bytes[At++];

        if (Flags & 2)
            return false;

        if ((Flags >> 3) == 31)
        {
            int32 TypeLength = 0;

            if (Count - At < 4)
                return false;

            memcpy(&TypeLength, Bytes + At, sizeof(TypeLength));

            if (TypeLength == 0 || !SkipArchiveString(Bytes, Count, At))
                return false;
        }

        const int PrefixEnd = At;

        if (Flags & 1)
        {
            if (Count - At < 1)
                return false;

            const int Size = Bytes[At++];

            if (Size <= 0 || Count - At != Size)
                return false;
        }
        else if (!SkipArchiveString(Bytes, Count, At) || At != Count)
        {
            return false;
        }

        OutPrefix.assign(Bytes, Bytes + PrefixEnd);
        OutPrefix[0] = (uint8)(Flags & ~4);
        bOutPacked = (Flags & 1) != 0;
        return true;
    }

    static uint8* NetIdOf(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState || !PlayerState->HasUniqueId())
            return nullptr;

        return (uint8*)&PlayerState->GetUniqueId();
    }

    static bool BotNetIdsMatter()
    {
        static int Answer = -1;

        if (Answer == -1)
        {
            auto Struct = FindStruct("UniqueNetIdRepl");
            const uint32 BytesAt = Struct ? Struct->GetOffset("ReplicationBytes") : (uint32)-1;
            const int32 Size = Struct ? (int32)Struct->GetPropertiesSize() : 0;

            Answer = (BytesAt == 0x18 && Size == 0x28) ? 1 : 0;

            if (!Answer)
            {
                GNetIdState = -1;
                LogLine("[NAMES] Bots keep their empty player ids on this build: its FUniqueNetIdRepl is not laid out the way this "
                        "code knows (ReplicationBytes at %d, %d bytes) -- they still get player numbers of their own.",
                        BytesAt == (uint32)-1 ? -1 : (int)BytesAt, Size);
            }
        }

        return Answer == 1;
    }

    static bool FindNetIdFormat(AFortGameStateAthena* GameState, float Now)
    {
        if (GNetIdState != 0)
            return GNetIdState == 1;

        if (!GameState || !GameState->HasPlayerArray() || !BotNetIdsMatter())
            return false;

        const char* Waiting = "there is no real player in the game yet";

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || (PS->HasbIsABot() && PS->bIsABot) || NPCs::IsNPC(PS))
                continue;

            uint8* Id = NetIdOf(PS);
            if (!Id)
                continue;

            void* Object = *(void**)(Id + 0x8);
            void* Controller = *(void**)(Id + 0x10);
            auto& Bytes = *(TArray<uint8>*)(Id + 0x18);

            if (!Object || !Controller)
            {
                Waiting = "the real player has no id object (yet)";
                continue;
            }

            if (!Bytes.Data || Bytes.Num() < 2 || Bytes.Num() > 512)
            {
                Waiting = "the real player's id has not gone out to anybody yet (the game makes its bytes the first time it does)";
                continue;
            }

            const int32 Shared = *(const int32*)((const uint8*)Controller + 8);
            const int32 Weak = *(const int32*)((const uint8*)Controller + 12);

            std::vector<uint8> Prefix;
            bool bPacked = false;
            const bool bReadable = ReadNetIdFormat(Bytes.Data, Bytes.Num(), Prefix, bPacked);

            if (Shared < 1 || Shared > 1000000 || Weak < 1 || Weak > 1000000 || !bReadable)
            {
                GNetIdState = -1;
                LogLine("[NAMES] Bots keep their empty player ids: '%s''s id does not read the way this code knows (%d byte(s): %s | "
                        "reference counts %d and %d) -- nothing is changed (they still get player numbers of their own).",
                        SafeNameOf(PS).c_str(), Bytes.Num(), HexOfBytes(Bytes.Data, Bytes.Num()).c_str(), Shared, Weak);
                return false;
            }

            InterlockedIncrement((volatile LONG*)((uint8*)Controller + 8));

            GNetIdPrefix = Prefix;
            GNetIdPacked = bPacked;
            GNetIdVTable = *(void**)Id;
            GNetIdObject = Object;
            GNetIdController = Controller;
            GNetIdMatchKey = (uint32)(GetTickCount64() & 0xFFFFFFFFull);
            GNetIdState = 1;

            LogLine("[NAMES] '%s''s player id goes out as %d byte(s) (%s): encoding byte 0x%02X -- type %u, the id %s. Every bot gets "
                    "an id of its own in that format.", SafeNameOf(PS).c_str(), Bytes.Num(), HexOfBytes(Bytes.Data, Bytes.Num()).c_str(),
                    (unsigned)Prefix[0], (unsigned)(Prefix[0] >> 3), bPacked ? "packed two digits to a byte" : "as a string");
            return true;
        }

        static float LastSaid = -100.f;
        static int TimesSaid = 0;

        if (strcmp(Waiting, "there is no real player in the game yet") != 0 && TimesSaid < 6 && Now - LastSaid >= 10.f)
        {
            LastSaid = Now;
            TimesSaid++;
            LogLine("[NAMES] Not copying the real player's id format yet: %s.", Waiting);
        }

        return false;
    }

    static bool GiveBotItsOwnNetId(AFortPlayerStateAthena* PlayerState, std::string& OutDigits)
    {
        uint8* Id = NetIdOf(PlayerState);
        if (!Id)
            return false;

        void*& Object = *(void**)(Id + 0x8);
        void*& Controller = *(void**)(Id + 0x10);
        auto& Bytes = *(TArray<uint8>*)(Id + 0x18);

        if (Object || Controller || Bytes.Num() != 0 || *(void**)Id != GNetIdVTable)
            return false;

        char Digits[40];
        GNetIdCounter++;
        snprintf(Digits, sizeof(Digits), "b07b07%02x%08x%08x%08x", (unsigned)(GNetIdCounter & 0xFF), (unsigned)GNetIdMatchKey,
                 (unsigned)GNetIdCounter, (unsigned)(((unsigned long long)(uintptr_t)PlayerState >> 4) & 0xFFFFFFFFull));
        OutDigits = Digits;

        std::vector<uint8> Out = GNetIdPrefix;

        if (GNetIdPacked)
        {
            auto Nibble = [](char Ch) -> uint8 { return (uint8)(Ch <= '9' ? Ch - '0' : Ch - 'a' + 10); };

            Out.push_back(16);

            for (int i = 0; i < 16; i++)
                Out.push_back((uint8)((Nibble(Digits[2 * i]) << 4) | Nibble(Digits[2 * i + 1])));
        }
        else
        {
            const int32 SaveNum = 33;
            const uint8* Length = (const uint8*)&SaveNum;

            Out.insert(Out.end(), Length, Length + sizeof(SaveNum));
            Out.insert(Out.end(), Digits, Digits + 32);
            Out.push_back(0);
        }

        Bytes.Reserve((int)Out.size());

        if (!Bytes.Data)
            return false;

        memcpy(Bytes.Data, Out.data(), Out.size());
        Bytes.NumElements = (int32)Out.size();

        InterlockedIncrement((volatile LONG*)((uint8*)GNetIdController + 8));
        Object = GNetIdObject;
        Controller = GNetIdController;
        return true;
    }

    static void GiveBotsTheirOwnNetIds(AFortGameStateAthena* GameState, float Now)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        if (GNetIdState == 0)
            FindNetIdFormat(GameState, Now);

        static int32 PlayerIdOffset = -2;
        static int32 NextBotPlayerId = 1000;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            if (PlayerIdOffset == -2)
            {
                const uint32 Off = ((UObject*)PS)->GetOffset("PlayerId");
                PlayerIdOffset = (Off == (uint32)-1) ? -1 : (int32)Off;

                if (PlayerIdOffset < 0)
                    LogLine("[NAMES] This build's player state has no PlayerId -- bots keep the number they have.");
            }

            if (PlayerIdOffset >= 0)
            {
                int32& Number = GetFromOffset<int32>(PS, (uint32)PlayerIdOffset);

                if (Number == 0)
                {
                    Number = NextBotPlayerId++;
                    GBotPlayerNumbers++;

                    if (GBotPlayerNumbers == 1)
                        LogLine("[NAMES] Bots get player numbers of their own (PlayerId %d and up) -- they all had 0.", Number);
                }
            }

            if (GNetIdState != 1 || GBotNetIds.find((void*)PS) != GBotNetIds.end())
                continue;

            FBotNetId Entry;
            std::string Digits;

            Entry.bGiven = GiveBotItsOwnNetId(PS, Digits);
            Entry.bShared = Entry.bGiven;

            if (Entry.bGiven)
            {
                GNetIdsGiven++;

                if (GNetIdsGiven <= 3)
                    LogLine("[NAMES] Bot '%s' has a player id of its own now (%s).", SafeNameOf(PS).c_str(), Digits.c_str());
            }
            else
            {
                GNetIdsRefused++;

                if (GNetIdsRefused <= 3)
                    LogLine("[NAMES] Bot '%s' was NOT given a player id of its own (it already had one, or its id is not laid out "
                            "like the real player's).", SafeNameOf(PS).c_str());
            }

            GBotNetIds.emplace((void*)PS, Entry);
        }

        static bool bTakenBack = false;

        if (!bTakenBack && bBusDeparted && Now - BusStartedTime > 5.f)
        {
            bTakenBack = true;

            for (auto& It : GBotNetIds)
            {
                if (!It.second.bShared)
                    continue;

                It.second.bShared = false;

                auto PS = (AFortPlayerStateAthena*)It.first;

                if (!ActorIsLive((const UObject*)PS))
                    continue;

                uint8* Id = NetIdOf(PS);
                if (!Id)
                    continue;

                void*& Object = *(void**)(Id + 0x8);
                void*& Controller = *(void**)(Id + 0x10);

                if (Object != GNetIdObject || Controller != GNetIdController)
                    continue;

                Object = nullptr;
                Controller = nullptr;

                InterlockedDecrement((volatile LONG*)((uint8*)GNetIdController + 8));
                GNetIdsTakenBack++;
            }

            LogLine("[NAMES] Player ids when the bus left: %d bot(s) were given one of their own, %d could not be | the real "
                    "player's id object taken back off %d of them | player numbers given: %d.", GNetIdsGiven, GNetIdsRefused,
                    GNetIdsTakenBack, GBotPlayerNumbers);
        }
    }

    void RenameUnnamedBots()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameMode)
            return;

        auto& Players = GameState->PlayerArray;

        static const std::vector<std::string> SpecialNames = {
            "&darkBeast&",
            "90cranker7",
            "A Sweaty Dog",
            "AboveMule633",
            "AimLikeIdaho",
            "AngryDuck51",
            "AthenaOrApollo",
            "AtTheBeach321",
            "BagelBoy82",
            "BatTacos",
            "Beebitme",
            "BellyFlop40",
            "Blackjack31",
            "BliceCake",
            "BlinkImGone44",
            "BoatingIsLife",
            "BobDobaleena",
            "BoldPrediction",
            "BraddleRoyale80",
            "BrainInvader",
            "Brauny Banana",
            "CactusDad80",
            "ColonelChugJug",
            "ConstantThorn",
            "CrazyPea96",
            "CrepeSalad",
            "DoctorLobby92",
            "DoubleDaring",
            "DoubleDuel75",
            "DoubleRainbow96",
            "DrPlanet",
            "DrumGunnar",
            "ElPollo85",
            "Discord.gg/4e0h",
            "FlavorCaptain",
            "FlimsyGoat",
            "FlossPatrol82",
            "Gatordile81",
            "GetItGotItGood",
            "GhostChicken12",
            "GnomeRider16",
            "Gooddoggo80",
            "Goosezilla13",
            "Grandma40",
            "HashtagToad57",
            "HeliumHog",
            "HeyThereFriend81",
            "Hoodwinked12",
            "HotelBlankets",
            "HowAreMy90s",
            "iHazHighGround",
            "JonseyForever35",
            "JustABitEpic",
            "KittyCat80",
            "Kregore73",
            "LetsBePals23",
            "LewtGoblin7",
            "LootTrooper51",
            "LousyCentaur",
            "LuckyNumber4279",
            "McCucumber71",
            "Meshuncle",
            "MintElephant26",
            "Mouthful95",
            "N0nDa1ry",
            "NotAPalidome",
            "Number141",
            "OldWaterBottle28",
            "ParanoidCactus",
            "PortableOx",
            "PositiveFeels",
            "PrancingPwnee",
            "PrinceWombat",
            "PurpleCrayon85",
            "Mehmehmeh",
            "QueenBeet74",
            "SergentSummer",
            "ShadowArrow58",
            "Shepard52",
            "ShieldHorse63",
            "ShootyMcGee40",
            "Neo_loves_overtonight",
            "SilverySilver",
            "SirTricksALot21",
            "SoggyCookie26",
            "SpiffyPowder6866",
            "SteelGoose18",
            "SweetPenguin16",
            "TAgYOuRIt9",
            "TheFreezer21",
            "ThermalDragon39",
            "TimeToGo80",
            "TooManyBeets",
            "WalkInThePark66",
            "WildCactusBob",
            "Wondertail",
            "Yeetman57",
            "AtomicPossum",
            "BananaBandit77",
            "CosmicBadger",
            "DiscoLizard42",
            "ElectricMoose",
            "FancyRaccoon",
            "GiantPanda88",
            "HappyTurnip",
            "IronOstrich",
            "JellyfishKing",
            "KillerToast",
            "LazyRocket",
            "MightyMuffin",
            "NinjaSquid",
            "AthenaFortPlayerController",
            "OrangeLlama",
            "PricklyPear55",
            "QuickQuokka",
            "RobotChicken",
            "SneakyPigeon",
            "TinyTornado",
            "UltraWombat",
            "VelvetShark",
            "WaffleKnight",
            "XenonTurtle",
            "YellowYeti",
            "ZanyZebra",
            "AmberAxolotl",
            "BlueberryBat",
            "CrankyCrab",
            "DizzyDolphin",
            "EmberFox",
            "FrostyFrog",
            "Overtonight",
            "GlowingGoat",
            "HiddenHedgehog",
            "IvoryIbis",
            "JollyJaguar",
            "KoalaKrusader",
            "LunarLemur",
            "MossyMole",
            "NeonNewt",
            "OceanOtter",
            "PinkPenguin",
            "QuietQuail",
            "RedRaccoon",
            "SilverSloth",
            "TurboToucan",
            "UrbanUmbreon",
            "VioletVulture",
            "WanderingWolf",
            "XRayYak",
            "YawningYak",
            "ZippyZorua",
            "AcornArmadillo",
            "BouncyBeaver",
            "CheerfulCheetah",
            "DapperDingo",
            "ElegantEmu",
            "FuzzyFerret",
            "GentleGiraffe",
            "HumbleHippo",
            "IcyIguana",
            "JeffDaKiller",
            "JazzyJackal",
            "KindKangaroo",
            "LuckyLynx",
            "MerryMeerkat",
            "NobleNarwhal",
            "OddOcelot",
            "PlayfulPanda",
            "QuirkyQuoll",
            "RowdyRhino",
            "SleepySeal",
            "TameTapir",
            "UniqueUrial",
            "VigilantVicuna",
            "WiseWalrus",
            "ZestyZebu",
            "BraveBison",
            "CleverCamel",
            "DaringDeer",
            "EagerEagle",
            "CoffeeGuzzler38",
            "FearlessFalcon",
            "GallantGazelle",
            "HonestHawk",
            "InvincibleIbis",
            "JoyfulJay",
            "KeenKiwi",
            "LoyalLion",
            "MysticMoth",
            "NimbleNumbat",
            "OptimisticOwl",
            "PatientParrot",
            "RadiantRaven",
            "SwiftSparrow",
            "TrustyTiger",
            "UpbeatUrchin",
            "ValiantVulture",
            "WildWoodpecker",
            "XenialXerus",
            "YoungYak",
            "ZealousZebra",
            "SoggyWaffle",
            "CrispyBacon",
            "JefferyEinstein.uasset",
            "SpicyNoodle",
            "FrozenPizza",
            "MeltyCheese",
            "CrunchyTaco",
            "ButteryBiscuit",
            "SaltyPretzel",
            "SweetMuffin",
            "SourLemon",
            "TangyOrange",
            "BitterMelon",
            "SavorySteak",
            "JuicyBurger",
            "CreamySoup",
            "ToastyBread",
            "StickyRice",
            "FluffyPancake",
            "GummyBear",
            "ChocolateMoose",
            "VanillaYeti",
            "StrawberrySlime",
            "BlueberryBison",
            "RaspberryRhino",
            "BananaBoat",
            "CherryBomb",
            "GrapeApe",
            "MangoMonkey",
            "PeachPanda",
            "PearParrot",
            "PlumPuppy",
            "KiwiKnight",
            "LemonLlama",
            "LimeLizard",
            "MelonMan",
            "OliveOtter",
            "PapayaPirate",
            "GuavaGoblin",
            "FigFalcon",
            "DateDragon",
            "LarryFizzlepot 2.0",
            "ApricotAngel",
            "CoconutCrab",
            "PecanPilot",
            "WalnutWizard",
            "AlmondArmadillo",
            "CashewCoyote",
            "PistachioPixie",
            "HazelnutHawk",
            "MacadamiaMole",
            "PeanutPanda",
            "CoffeeCat",
            "TeaTiger",
            "SodaShark",
            "JuiceJaguar",
            "MilkMoose",
            "WaterWombat",
            "FireFerret",
            "EarthEmu",
            "WindWolf",
            "StormStag",
            "ThunderTurtle",
            "LightningLynx",
            "SolarSquid",
            "LunarLobster",
            "StarfishSteve",
            "GalaxyGoat",
            "NebulaNewt",
            "CometCrab",
            "MeteorMoth",
            "AsteroidApe",
            "PlanetPigeon",
            "OrbitOtter",
            "RocketRaccoon",
            "CometCoyote",
            "GravityGoose",
            "CosmoCat",
            "AstroAnt",
            "NovaNewt",
            "QuasarQuail",
            "PulsarPug",
            "ShadowSheep",
            "GhostGecko",
            "DonaldMustardPlsComeBack",
            "PhantomPanda",
            "SpiritSparrow",
            "HauntedHamster",
            "SpookySquid",
            "CreepyCrab",
            "MysteriousMole",
            "SecretSquirrel",
            "HiddenHawk",
            "SilentSnake",
            "QuietQuokka",
            "WhisperWolf",
            "MurmurMoth",
            "EchoEmu",
            "BreezeBadger",
            "FogFox",
            "MistMoose",
            "CloudCoyote",
            "RainRaccoon",
            "SunnySalamander",
            "CloudyCat",
            "WindyWeasel",
            "RainyRabbit",
            "SnowySeal",
            "StormySwan",
            "FoggyFerret",
            "MistyMouse",
            "DustyDingo",
            "SandySquirrel",
            "MuddyMule",
            "RockyRaccoon",
            "PebblePenguin",
            "BoulderBear",
            "CrystalCrab",
            "DiamondDove",
            "RubyRabbit",
            "EmeraldEel",
            "SapphireSnake",
            "TopazTiger",
            "BronzeBadger",
            "SilverShark",
            "GoldenGoose",
            "PlatinumPanda",
            "CopperCoyote",
            "IronIguana",
            "SteelSeal",
            "TitaniumTurtle",
            "ChromeChicken",
            "NickelNewt",
            "BrassBeaver",
            "CobaltCat",
            "ZincZebra",
            "LeadLemur",
            "MercuryMoth",
            "NeonNarwhal",
            "ArgonApe",
            "HeliumHedgehog",
            "OxygenOtter",
            "CarbonCrab",
            "PixelPanda",
            "VectorVulture",
            "MatrixMole",
            "BinaryBear",
            "CyberCat",
            "DigitalDog",
            "AnalogAnt",
            "LaserLizard",
            "PlasmaPig",
            "QuantumQuail",
            "NanoNewt",
            "MicroMoose",
            "SophieRain",
            "MegaMoth",
            "GigaGoat",
            "TerraTiger",
            "ByteBadger",
            "DataDingo",
            "CodeCoyote",
            "GlitchGoose",
            "BugBunny",
            "AlphaAlpaca",
            "BetaBadger",
            "GammaGoose",
            "DeltaDuck",
            "EpsilonEmu",
            "ZetaZebra",
            "EtaEagle",
            "ThetaTiger",
            "IotaIbis",
            "KappaKoala",
            "LambdaLion",
            "MuMoose",
            "NuNewt",
            "XiXerus",
            "OmicronOwl",
            "PiPanda",
            "RhoRabbit",
            "SigmaSnake",
            "TauTurtle",
            "UpsilonUrchin",
            "PhiFox",
            "ChiCheetah",
            "PsiPenguin",
            "OmegaOtter",
            "PrimePanda",
            "ZeroZebra",
            "OneOtter",
            "Four_E_Zero_H",
            "TwoTiger",
            "ThreeThrush",
            "FourFox",
            "FiveFalcon",
            "SixSnake",
            "SevenSeal",
            "EightEagle",
            "NineNewt",
            "TenTurtle",
            "ElevenEmu",
            "TwelveToucan",
            "LuckyLlama",
            "HappyHippo",
            "GrumpyGoat",
            "SleepySloth",
            "DopeyDog",
            "BashfulBear",
            "SneezySnake",
            "DocDuck",
            "HappyHawk",
            "FunnyFox",
            "SillySeal",
            "CrazyCrab",
            "WackyWombat",
            "GoofyGoose",
            "ClumsyCat",
            "BouncyBear",
            "JumpyJaguar",
            "SkippySquid",
            "HoppingHare",
            "LeapingLizard",
            "DashingDeer",
            "PrancingPony",
            "GallopingGoat",
            "RunningRabbit",
            "FlyingFish",
            "SwimmingSwan",
            "DivingDolphin",
            "ClimbingCat",
            "CrawlingCrab",
            "SlitheringSnake",
            "WaddlingWalrus",
            "MarchingMoose",
            "DancingDingo",
            "SingingSparrow",
            "HummingHawk",
            "WhistlingWolf",
            "LaughingLlama",
            "SmilingSheep",
            "WinkingWolf",
            "BlinkingBat",
            "StaringStag",
            "GazingGoat",
            "NinjaNarwhal",
            "PiratePanda",
            "VikingVulture",
            "KnightKiwi",
            "WizardWeasel",
            "MageMoth",
            "RogueRabbit",
            "RangerRaccoon",
            "ClericCat",
            "DruidDuck",
            "BardBadger",
            "MonkMole",
            "PaladinPug",
            "WarlockWolf",
            "SorcererSnake",
            "AlchemistAnt",
            "NecromancerNewt",
            "SummonerSquid",
            "ArcherArmadillo",
            "HunterHawk",
            "SamuraiSwan",
            "CowboyCoyote",
            "SheriffShark",
            "DeputyDog",
            "MarshalMoose",
            "OutlawOtter",
            "BanditBadger",
            "RustlerRabbit",
            "GunfighterGoose",
            "SniperSparrow",
            "ScoutSquirrel",
            "MedicMoth",
            "EngineerEmu",
            "SoldierSnake",
            "CaptainCrab",
            "MajorMole",
            "GeneralGoat",
            "AdmiralApe",
            "CommanderCat",
            "SergeantSeal",
            "DoctorDove",
            "ProfessorPanda",
            "TeacherTiger",
            "StudentSquid",
            "ScientistSnail",
            "InventorIbis",
            "ExplorerEel",
            "AdventurerAnt",
            "TravelerToucan",
            "WandererWolf",
            "DreamerDuck",
            "ThinkerThrush",
            "BuilderBear",
            "MakerMoose",
            "CrafterCrab",
            "PainterPenguin",
            "SingerSwan",
            "DancerDeer",
            "WriterWren",
            "PlayerPanda",
            "PrintHelloWorld",
        };

        static std::vector<std::string> ActiveNames;
        static size_t ActiveNamesUsed = 0;
        static bool bActiveNamesReady = false;

        if (!bActiveNamesReady && !SpecialNames.empty())
        {
            ActiveNames = SpecialNames;

            for (size_t i = ActiveNames.size(); i > 1; --i)
            {
                size_t j = (size_t)(rand() % (int)i);
                std::swap(ActiveNames[i - 1], ActiveNames[j]);
            }
            const size_t Keep = ActiveNames.size() < 99 ? ActiveNames.size() : 99;
            ActiveNames.resize(Keep);

            bActiveNamesReady = true;
            LogLine("[NAMES] Picked %zu random names out of %zu for this match.", ActiveNames.size(), SpecialNames.size());
        }

        for (int i = 0; i < Players.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)Players[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller)
                continue;

            if (AssignedBotNames.find((void*)Controller) != AssignedBotNames.end())
                continue;

            if (ActiveNames.empty())
                continue;

            std::string NewName;
            {
                size_t idx = ActiveNamesUsed % ActiveNames.size();
                size_t pass = ActiveNamesUsed / ActiveNames.size();
                ActiveNamesUsed++;

                NewName = ActiveNames[idx];
                if (pass > 0)
                    NewName += " " + std::to_string(pass + 1);
            }

            AssignedBotNames[(void*)Controller] = NewName;

            std::wstring WideName(NewName.begin(), NewName.end());

            GameMode->ChangeName(Controller, FString(WideName.c_str()), true);
            PlayerState->OnRep_PlayerName();

            if (GNamesScrambled == -1)
            {
                const std::wstring Raw = RawStoredName(PlayerState);

                if (!Raw.empty())
                {
                    if (Raw == WideName)
                        GNamesScrambled = 0;
                    else if (UnscrambleName(Raw) == WideName)
                    {
                        GNamesScrambled = 1;
                        LogLine("[NAMES] This build stores player names scrambled (each character shifted, the way 9.10 does it; the "
                                "game's own GetPlayerName shifts them back) -- names in this log are shown unscrambled from here on.");
                    }
                    else
                    {

                        static int Undecided = 0;
                        if (++Undecided <= 3)
                            LogLine("[NAMES] The name the game stored for '%s' is neither what it was given nor that name scrambled the "
                                    "9.10 way -- tried again with the next bot (names in this log are shown as stored until then).",
                                    NewName.c_str());
                    }
                }
            }

            static int NamedSoFar = 0;
            const std::string ReadBack = NameAsStored(PlayerState);

            LogLine("[NAMES] Bot #%d is now '%s' (the game reads back '%s'%s).", ++NamedSoFar, NewName.c_str(), ReadBack.c_str(),
                    ReadBack == NewName ? "" : " -- NOT the name it was given");
        }
    }

    static const wchar_t* GSeasonOneOutfits[] = {
        L"CID_001_Athena_Commando_F_Default", L"CID_002_Athena_Commando_F_Default", L"CID_003_Athena_Commando_F_Default",
        L"CID_004_Athena_Commando_F_Default", L"CID_005_Athena_Commando_M_Default", L"CID_006_Athena_Commando_M_Default",
        L"CID_007_Athena_Commando_M_Default", L"CID_008_Athena_Commando_M_Default",
        L"CID_009_Athena_Commando_M", L"CID_010_Athena_Commando_M", L"CID_011_Athena_Commando_M", L"CID_012_Athena_Commando_M",
        L"CID_013_Athena_Commando_F", L"CID_014_Athena_Commando_F", L"CID_015_Athena_Commando_F", L"CID_016_Athena_Commando_F",
        L"CID_017_Athena_Commando_M", L"CID_018_Athena_Commando_M", L"CID_019_Athena_Commando_M", L"CID_020_Athena_Commando_M",
        L"CID_021_Athena_Commando_F", L"CID_022_Athena_Commando_F", L"CID_023_Athena_Commando_F", L"CID_024_Athena_Commando_F",
        L"CID_025_Athena_Commando_M", L"CID_026_Athena_Commando_M", L"CID_027_Athena_Commando_F", L"CID_028_Athena_Commando_F",
        L"CID_029_Athena_Commando_F_Halloween", L"CID_030_Athena_Commando_M_Halloween",
    };

    struct FBotOutfit
    {
        const UObject* Hero = nullptr;
        const UObject* Parts[6] = {};
        uint8 Gender = 2;
        uint8 BodyType = 0;
        std::string Name;
    };

    template <typename T>
    static const UObject* SoftArrayAt(TArray<TSoftObjectPtr<T>>& Array, int Index, const UClass* Class)
    {
        const int32 Stride = (VersionInfo.EngineVersion <= 4.16) ? 0x20 : (int32)FSoftObjectPtr::Size();
        return Array.Get(Index, Stride).InternalGet(Class);
    }

    static std::string OutfitPartName(const UObject* Part)
    {
        if (!Part)
            return std::string("none");

        auto Name = Part->Name.ToString();
        return std::string(Name.c_str());
    }

    static uint8 PartBodyTypes(const UObject* Part)
    {
        static int32 Offset = -2;

        if (!Part)
            return 0;

        if (Offset == -2)
            Offset = (int32)Part->GetOffset("BodyTypesPermitted");

        if (Offset <= 0)
            return 0;

        const uint8 Mask = GetFromOffset<uint8>(Part, (uint32)Offset);
        return (Mask >= 1 && Mask <= 7) ? Mask : 0;
    }

    static bool ResolveOneOutfitRaw(const wchar_t* Id, FBotOutfit* Out, int* Reason)
    {
        std::wstring Wide(Id);
        std::wstring Path = L"/Game/Athena/Items/Cosmetics/Characters/" + Wide + L"." + Wide;
        std::string Narrow(Wide.begin(), Wide.end());

        auto CID = (UAthenaCharacterItemDefinition*)FindObject<UAthenaCharacterItemDefinition>(Path.c_str());
        if (!CID)
            CID = (UAthenaCharacterItemDefinition*)TUObjectArray::FindObject<UAthenaCharacterItemDefinition>(Narrow.c_str());

        if (!CID)
        {
            *Reason = 1;
            return false;
        }

        auto Hero = CID->HeroDefinition;
        if (!Hero)
        {
            *Reason = 2;
            return false;
        }

        if (!Hero->HasSpecializations() || Hero->Specializations.Num() <= 0 || Hero->Specializations.Num() > 16)
        {
            *Reason = 3;
            return false;
        }

        Out->Hero = (const UObject*)Hero;
        Out->Name = Narrow;
        Out->Gender = (Narrow.find("_Commando_M") != std::string::npos) ? 1 : 2;

        for (int s = 0; s < Hero->Specializations.Num(); s++)
        {
            auto Spec = (UFortHeroSpecialization*)SoftArrayAt(Hero->Specializations, s, UFortHeroSpecialization::StaticClass());
            if (!Spec || Spec->CharacterParts.Num() <= 0 || Spec->CharacterParts.Num() > 16)
                continue;

            for (int j = 0; j < Spec->CharacterParts.Num(); j++)
            {
                auto Part = (UCustomCharacterPart*)SoftArrayAt(Spec->CharacterParts, j, UCustomCharacterPart::StaticClass());

                if (Part && Part->CharacterPartType < 6 && !Out->Parts[Part->CharacterPartType])
                    Out->Parts[Part->CharacterPartType] = (const UObject*)Part;
            }
        }

        if (!Out->Parts[1])
        {
            *Reason = 4;
            return false;
        }

        return true;
    }

    static bool ResolveOneOutfitGuarded(const wchar_t* Id, FBotOutfit* Out, int* Reason)
    {
        *Reason = 0;

        __try
        {
            return ResolveOneOutfitRaw(Id, Out, Reason);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Reason = 9;
            return false;
        }
    }

    static std::vector<FBotOutfit> GBotOutfits;
    static bool GBotOutfitsResolved = false;
    static std::map<void*, int> BotOutfitOf;
    static std::map<void*, void*> DressedBotPawns;
    static int GBotsDressed = 0;
    static bool GPartChoiceBroken = false;
    static bool GCustomizationBroken = false;
    static bool GDressNotifyBroken = false;

    static void ResolveSeasonOneOutfits()
    {
        if (GBotOutfitsResolved)
            return;

        GBotOutfitsResolved = true;

        static const char* Reasons[] = { "?", "not on this build", "no hero", "no specializations", "no body", "?", "?", "?", "?", "faulted" };

        std::vector<FBotOutfit> Found;
        std::string Skipped;
        int Faults = 0;

        for (auto Id : GSeasonOneOutfits)
        {
            FBotOutfit Outfit{};
            int Reason = 0;

            if (ResolveOneOutfitGuarded(Id, &Outfit, &Reason))
            {
                Found.push_back(Outfit);
                continue;
            }

            if (Reason == 9)
                Faults++;

            std::wstring Wide(Id);
            if (!Skipped.empty())
                Skipped += ", ";
            Skipped += std::string(Wide.begin(), Wide.end()) + " (" + Reasons[(Reason >= 0 && Reason <= 9) ? Reason : 0] + ")";
        }

        const UObject* FemaleHead = nullptr;
        const UObject* MaleHead = nullptr;

        for (auto& Outfit : Found)
        {
            if (!Outfit.Parts[0])
                continue;

            if (Outfit.Gender == 1 && !MaleHead)
                MaleHead = Outfit.Parts[0];
            if (Outfit.Gender == 2 && !FemaleHead)
                FemaleHead = Outfit.Parts[0];
        }

        if (!FemaleHead)
            FemaleHead = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");

        int HeadsLent = 0;

        for (auto& Outfit : Found)
        {
            if (!Outfit.Parts[0])
            {
                Outfit.Parts[0] = (Outfit.Gender == 1) ? MaleHead : FemaleHead;
                if (Outfit.Parts[0])
                    HeadsLent++;
            }

            if (!Outfit.Parts[0])
            {
                if (!Skipped.empty())
                    Skipped += ", ";
                Skipped += Outfit.Name + " (no head)";
                continue;
            }

            if (VersionInfo.EngineVersion <= 4.16)
            {
                uint8 Allowed = 7;
                const uint8 HeadTypes = PartBodyTypes(Outfit.Parts[0]);
                const uint8 BodyTypes = PartBodyTypes(Outfit.Parts[1]);

                if (HeadTypes)
                    Allowed &= HeadTypes;
                if (BodyTypes)
                    Allowed &= BodyTypes;

                Outfit.BodyType = (Allowed & 2) ? 2 : ((Allowed & 1) ? 1 : ((Allowed & 4) ? 4 : 2));
            }

            GBotOutfits.push_back(Outfit);
        }

        if (GBotOutfits.empty())
        {

            FBotOutfit Default{};
            Default.Hero = FindObject<UObject>(L"/Game/Athena/Heroes/HID_001_Athena_Commando_F.HID_001_Athena_Commando_F");
            if (!Default.Hero)
                Default.Hero = FindObject<UObject>(L"/Game/Athena/Heroes/HID_Commando_Athena_01.HID_Commando_Athena_01");
            Default.Parts[0] = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");
            Default.Parts[1] = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01");
            Default.Parts[3] = FindObject<UObject>(L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack");
            Default.Gender = 2;
            Default.BodyType = (VersionInfo.EngineVersion <= 4.16) ? 2 : 0;
            Default.Name = "the default commando";

            if (Default.Parts[0] && Default.Parts[1])
                GBotOutfits.push_back(Default);
        }

        LogLine("[DRESS] %d Season 1 outfit(s) with a head and a body found for the bots (%d borrowed a head from another outfit)%s%s%s.",
                (int)GBotOutfits.size(), HeadsLent,
                GBotOutfits.empty() ? " -- NONE, the bots cannot be dressed" : "",
                Skipped.empty() ? "" : " | not used: ", Skipped.c_str());

        if (Faults > 0)
            LogLine("[DRESS] Reading %d CID(s) faulted -- caught, those outfits are left out.", Faults);

        for (size_t i = 0; i < GBotOutfits.size() && i < 6; i++)
        {
            const auto& Outfit = GBotOutfits[i];

            LogLine("[DRESS]   %s: head %s, body %s, hat %s, backpack %s | %s, body type %d.", Outfit.Name.c_str(),
                    OutfitPartName(Outfit.Parts[0]).c_str(), OutfitPartName(Outfit.Parts[1]).c_str(),
                    OutfitPartName(Outfit.Parts[2]).c_str(), OutfitPartName(Outfit.Parts[3]).c_str(),
                    Outfit.Gender == 1 ? "male" : "female", (int)Outfit.BodyType);
        }
    }

    static bool ChoosePartRaw(AFortPlayerPawnAthena* Pawn, uint8 Slot, const UObject* Part)
    {
        UObject* Chosen = (UObject*)Part;
        Pawn->ServerChoosePart(Slot, Chosen);
        return true;
    }

    static bool ChoosePartGuarded(AFortPlayerPawnAthena* Pawn, uint8 Slot, const UObject* Part)
    {
        __try
        {
            return ChoosePartRaw(Pawn, Slot, Part);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ApplyCustomizationGuarded(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
    {
        __try
        {
            ((void (*)(AActor*, AActor*))ApplyCharacterCustomization)((AActor*)PlayerState, (AActor*)Pawn);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool InvokeNoArgsGuarded(const UObject* Object, const char* FunctionName)
    {
        __try
        {
            InvokeNoArgs(Object, FunctionName);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool InvokeOneByteRaw(const UObject* Object, const char* FunctionName, uint8 Value)
    {
        auto Fn = Object ? Object->GetFunction(FunctionName) : nullptr;
        if (!Fn)
            return false;

        const int32 Size = Fn->GetPropertiesSize();
        if (Size < 1 || Size > 0x100)
            return false;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));
        Params[0] = Value;

        Object->ProcessEvent(Fn, Params);
        return true;
    }

    static bool InvokeOneByteGuarded(const UObject* Object, const char* FunctionName, uint8 Value, bool* OutFaulted)
    {
        *OutFaulted = false;

        __try
        {
            return InvokeOneByteRaw(Object, FunctionName, Value);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *OutFaulted = true;
            return false;
        }
    }

    static bool SetReflectedBool(UObject* Object, const char* Name)
    {
        auto Prop = Object ? Object->GetProperty(Name, 0x20000) : nullptr;
        if (!Prop)
            return false;

        const uint32 Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
        const uint8 Mask = Prop->GetFieldMask();

        if (!Mask || Offset >= 0x10000)
            return false;

        GetFromOffset<uint8>(Object, Offset) |= Mask;
        return true;
    }

    static int32 PartSlotsInside(UObject* Object, const char* PropertyName, uint32& OutElementSize)
    {
        OutElementSize = 0;

        auto Prop = Object ? Object->GetProperty(PropertyName) : nullptr;
        if (Prop)
            OutElementSize = GetFromOffset<uint32>(Prop, Offsets::ElementSize);

        if (OutElementSize == 8 || OutElementSize > 0x1000)
            return 0;

        auto PartsStruct = FindStruct("CustomCharacterParts");
        const uint32 Inner = PartsStruct ? PartsStruct->GetOffset("Parts") : (uint32)-1;

        if (Inner == (uint32)-1 || Inner >= 0x40)
            return 0;

        if (OutElementSize && Inner + 6 * sizeof(void*) > OutElementSize)
            return 0;

        return (int32)Inner;
    }

    static void DressBotSeasonOne(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
    {
        if (!PlayerState || !Pawn)
            return;

        ResolveSeasonOneOutfits();
        if (GBotOutfits.empty())
            return;

        int Index;
        auto Picked = BotOutfitOf.find((void*)PlayerState);

        if (Picked != BotOutfitOf.end())
        {
            Index = Picked->second;
        }
        else
        {
            Index = rand() % (int)GBotOutfits.size();
            BotOutfitOf[(void*)PlayerState] = Index;
        }

        const FBotOutfit& Outfit = GBotOutfits[(size_t)Index];

        static int32 StatePartsOffset = -2, PawnPartsOffset = -2, GenderOffset = -2, BodyTypeOffset = -2;

        if (StatePartsOffset == -2)
            StatePartsOffset = (int32)((UObject*)PlayerState)->GetOffset("CharacterParts");
        if (PawnPartsOffset == -2)
            PawnPartsOffset = (int32)((UObject*)Pawn)->GetOffset("CharacterParts");
        if (GenderOffset == -2)
            GenderOffset = (int32)((UObject*)PlayerState)->GetOffset("CharacterGender");
        if (BodyTypeOffset == -2)
            BodyTypeOffset = (int32)((UObject*)PlayerState)->GetOffset("CharacterBodyType");

        if (StatePartsOffset < 0)
        {
            static bool bSaid = false;
            if (!bSaid)
            {
                bSaid = true;
                LogLine("[DRESS] The PlayerState has no CharacterParts on this build -- the bots cannot be dressed.");
            }
            return;
        }

        static int32 StatePartsInner = -2, PawnPartsInner = -2;

        if (StatePartsInner == -2)
        {
            uint32 ElementSize = 0;
            StatePartsInner = PartSlotsInside((UObject*)PlayerState, "CharacterParts", ElementSize);

            LogLine("[DRESS] A PlayerState's six part slots (head, body, hat, backpack, charm, face) are at +0x%X in its CharacterParts "
                    "(%u bytes: %s).", (unsigned)StatePartsInner, ElementSize,
                    StatePartsInner ? "an FCustomCharacterParts, the way 9.10 keeps them -- the first 8 bytes are not part slots"
                                    : "a plain row of six parts");
        }

        if (PawnPartsInner == -2)
        {
            uint32 ElementSize = 0;
            PawnPartsInner = PawnPartsOffset >= 0 ? PartSlotsInside((UObject*)Pawn, "CharacterParts", ElementSize) : 0;
        }

        auto WriteStateParts = [&]()
        {
            auto Slots = (const UObject**)((uint8*)PlayerState + StatePartsOffset + StatePartsInner);
            for (int s = 0; s < 6; s++)
                Slots[s] = Outfit.Parts[s];
        };

        WriteStateParts();

        if (PlayerState->HasHeroType() && Outfit.Hero)
        {
            const UObject* Hero = Outfit.Hero;
            PlayerState->HeroType = Hero;
        }

        if (GenderOffset >= 0)
            GetFromOffset<uint8>(PlayerState, (uint32)GenderOffset) = Outfit.Gender;

        if (BodyTypeOffset >= 0 && Outfit.BodyType)
            GetFromOffset<uint8>(PlayerState, (uint32)BodyTypeOffset) = Outfit.BodyType;

        SetReflectedBool((UObject*)PlayerState, "bShowHeroBackpack");
        SetReflectedBool((UObject*)PlayerState, "bShowHeroHeadAccessories");

        int Chosen = 0;
        if (!GPartChoiceBroken)
        {
            for (int s = 0; s < 6; s++)
            {
                if (!Outfit.Parts[s])
                    continue;

                if (!ChoosePartGuarded(Pawn, (uint8)s, Outfit.Parts[s]))
                {
                    GPartChoiceBroken = true;
                    LogLine("[DRESS] The game's ServerChoosePart faulted on a bot -- skipped from now on (the PlayerState's parts still dress them).");
                    break;
                }

                Chosen++;
            }
        }

        if (!GPartChoiceBroken)
        {
            bool bFaulted = false;
            InvokeOneByteGuarded((UObject*)Pawn, "ServerChooseGender", Outfit.Gender, &bFaulted);

            if (bFaulted)
            {
                GPartChoiceBroken = true;
                LogLine("[DRESS] The game's ServerChooseGender faulted on a bot -- part and gender choices skipped from now on.");
            }
        }

        bool bApplied = false;
        if (ApplyCharacterCustomization && !GCustomizationBroken)
        {
            bApplied = ApplyCustomizationGuarded(PlayerState, Pawn);

            if (!bApplied)
            {
                GCustomizationBroken = true;
                LogLine("[DRESS] The game's ApplyCharacterCustomization faulted on a bot -- skipped from now on.");
            }
        }

        WriteStateParts();

        if (!GDressNotifyBroken)
        {
            const bool bOk = InvokeNoArgsGuarded((UObject*)PlayerState, "OnRep_HeroType") &&
                             InvokeNoArgsGuarded((UObject*)Pawn, "OnRep_PlayerState");

            WriteStateParts();

            const bool bOk2 = bOk &&
                              InvokeNoArgsGuarded((UObject*)PlayerState, "OnRep_CharacterParts") &&
                              InvokeNoArgsGuarded((UObject*)PlayerState, "OnRep_ShowHeroBackpack") &&
                              InvokeNoArgsGuarded((UObject*)PlayerState, "OnRep_ShowHeroHeadAccessories");

            if (!bOk2)
            {
                GDressNotifyBroken = true;
                LogLine("[DRESS] One of the game's look notifications faulted on a bot -- skipped from now on.");
            }
        }

        if (PawnPartsOffset >= 0)
        {
            auto PawnSlots = (const UObject**)((uint8*)Pawn + PawnPartsOffset + PawnPartsInner);
            for (int s = 0; s < 6; s++)
                PawnSlots[s] = Outfit.Parts[s];
        }

        if (!GDressNotifyBroken)
            InvokeNoArgsGuarded((UObject*)Pawn, "OnCharacterPartsReinitialized");

        PlayerState->ForceNetUpdate();
        Pawn->ForceNetUpdate();

        GBotsDressed++;

        if (GBotsDressed <= 3 || (GBotsDressed % 25) == 0)
        {
            int Parts = 0;
            for (int s = 0; s < 6; s++)
                if (Outfit.Parts[s])
                    Parts++;

            auto Slots = (const UObject**)((uint8*)PlayerState + StatePartsOffset + StatePartsInner);

            LogLine("[DRESS] '%s' dressed as %s (%d part(s), %d chosen on the pawn, customization %s) | the PlayerState now holds: head %s, "
                    "body %s, body type %d -- %d bot(s) dressed so far.",
                    SafeNameOf(PlayerState).c_str(), Outfit.Name.c_str(), Parts, Chosen,
                    bApplied ? "applied" : (ApplyCharacterCustomization ? "not applied" : "not on this build"),
                    OutfitPartName(Slots[0]).c_str(), OutfitPartName(Slots[1]).c_str(),
                    BodyTypeOffset >= 0 ? (int)GetFromOffset<uint8>(PlayerState, (uint32)BodyTypeOffset) : -1, GBotsDressed);
        }
    }

    void DressUndressedBots()
    {
        static const std::vector<std::wstring> DefaultSkins = {
            L"/Game/Athena/Items/Cosmetics/Characters/CID_922_Athena_Commando_M_ParcelPrank.CID_922_Athena_Commando_M_ParcelPrank",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_921_Athena_Commando_F_ParcelPetal.CID_921_Athena_Commando_F_ParcelPetal",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_957_Athena_Commando_F_Ponytail.CID_957_Athena_Commando_F_Ponytail",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_925_Athena_Commando_F_TapDance.CID_925_Athena_Commando_F_TapDance",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_891_Athena_Commando_M_LunchBox.CID_891_Athena_Commando_M_LunchBox",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_886_Athena_Commando_M_ChOneRenegade.CID_886_Athena_Commando_M_ChOneRenegade",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_757_Athena_Commando_F_WildCat.CID_757_Athena_Commando_F_WildCat",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_902_Athena_Commando_M_PumpkinPunk.CID_902_Athena_Commando_M_PumpkinPunk",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_903_Athena_Commando_F_Frankie.CID_903_Athena_Commando_F_Frankie",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_919_Athena_Commando_F_BabaYaga.CID_919_Athena_Commando_F_BabaYaga",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_913_Athena_Commando_F_York_D.CID_913_Athena_Commando_F_York_D",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_914_Athena_Commando_F_York_E.CID_914_Athena_Commando_F_York_E",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_917_Athena_Commando_M_DurrburgerSkull.CID_917_Athena_Commando_M_DurrburgerSkull",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_920_Athena_Commando_M_PartyTrooper.CID_920_Athena_Commando_M_PartyTrooper",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_897_Athena_Commando_F_DarkBomberSummer.CID_897_Athena_Commando_F_DarkBomberSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_893_Athena_Commando_F_BlackWidowJacket.CID_893_Athena_Commando_F_BlackWidowJacket",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_892_Athena_Commando_F_VampireCasual.CID_892_Athena_Commando_F_VampireCasual",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_899_Athena_Commando_F_Poison.CID_899_Athena_Commando_F_Poison",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_895_Athena_Commando_M_DeliSandwich.CID_895_Athena_Commando_M_DeliSandwich",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_828_Athena_Commando_F_Valet.CID_828_Athena_Commando_F_Valet",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_829_Athena_Commando_M_Valet.CID_829_Athena_Commando_M_Valet",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_896_Athena_Commando_F_SpookyNeon.CID_896_Athena_Commando_F_SpookyNeon",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_869_Athena_Commando_F_Cavalry.CID_869_Athena_Commando_F_Cavalry",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_894_Athena_Commando_M_Palespooky.CID_894_Athena_Commando_M_Palespooky",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_871_Athena_Commando_F_StreetFashionGarnet.CID_871_Athena_Commando_F_StreetFashionGarnet",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_825_Athena_Commando_F_SportsFashion.CID_825_Athena_Commando_F_SportsFashion",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_868_Athena_Commando_M_Backspin_3U6CA.CID_868_Athena_Commando_M_Backspin_3U6CA",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_852_Athena_Commando_F_BlackWidowCorrupt.CID_852_Athena_Commando_F_BlackWidowCorrupt",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_853_Athena_Commando_F_SniperHoodCorrupt.CID_853_Athena_Commando_F_SniperHoodCorrupt",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_827_Athena_Commando_M_MultibotStealth.CID_827_Athena_Commando_M_MultibotStealth",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_679_Athena_Commando_M_ModernMilitaryEclipse.CID_679_Athena_Commando_M_ModernMilitaryEclipse",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_806_Athena_Commando_F_GreenJacket.CID_806_Athena_Commando_F_GreenJacket",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_544_Athena_Commando_M_Kurohomura.CID_544_Athena_Commando_M_Kurohomura",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_604_Athena_Commando_F_Razor.CID_604_Athena_Commando_F_Razor",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_763_Athena_Commando_F_ShinyJacket.CID_763_Athena_Commando_F_ShinyJacket",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_864_Athena_Commando_F_Elastic_E.CID_864_Athena_Commando_F_Elastic_E",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_840_Athena_Commando_M_HightowerGrape.CID_840_Athena_Commando_M_HightowerGrape",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_841_Athena_Commando_M_HightowerWasabi.CID_841_Athena_Commando_M_HightowerWasabi",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_842_Athena_Commando_F_HightowerHoneydew.CID_842_Athena_Commando_F_HightowerHoneydew",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_843_Athena_Commando_M_HightowerTomato_Casual.CID_843_Athena_Commando_M_HightowerTomato_Casual",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_844_Athena_Commando_F_HightowerMango.CID_844_Athena_Commando_F_HightowerMango",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_845_Athena_Commando_M_HightowerTapas.CID_845_Athena_Commando_M_HightowerTapas",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_846_Athena_Commando_M_HightowerDate.CID_846_Athena_Commando_M_HightowerDate",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_824_Athena_Commando_F_RaiderPink.CID_824_Athena_Commando_F_RaiderPink",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_800_Athena_Commando_M_Robro.CID_800_Athena_Commando_M_Robro",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_822_Athena_Commando_F_Angler.CID_822_Athena_Commando_F_Angler",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_823_Athena_Commando_F_Islander.CID_823_Athena_Commando_F_Islander",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_836_Athena_Commando_M_JonesyFlare.CID_836_Athena_Commando_M_JonesyFlare",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_833_Athena_Commando_F_TripleScoop.CID_833_Athena_Commando_F_TripleScoop",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_832_Athena_Commando_F_AntiLlama.CID_832_Athena_Commando_F_AntiLlama",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_826_Athena_Commando_M_FloatillaCaptain.CID_826_Athena_Commando_M_FloatillaCaptain",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_834_Athena_Commando_M_Axl.CID_834_Athena_Commando_M_Axl",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_798_Athena_Commando_M_JonesyVagabond.CID_798_Athena_Commando_M_JonesyVagabond",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_815_Athena_Commando_F_DurrburgerHero.CID_815_Athena_Commando_F_DurrburgerHero",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_795_Athena_Commando_M_Dummeez.CID_795_Athena_Commando_M_Dummeez",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_796_Athena_Commando_F_Tank.CID_796_Athena_Commando_F_Tank",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_816_Athena_Commando_F_DirtyDocks.CID_816_Athena_Commando_F_DirtyDocks",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_817_Athena_Commando_M_DirtyDocks.CID_817_Athena_Commando_M_DirtyDocks",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_814_Athena_Commando_M_BananaSummer.CID_814_Athena_Commando_M_BananaSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_810_Athena_Commando_M_MilitaryFashionSummer.CID_810_Athena_Commando_M_MilitaryFashionSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_775_Athena_Commando_F_StreetRacerCobraGold.CID_775_Athena_Commando_F_StreetRacerCobraGold",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_813_Athena_Commando_M_TeriyakiAtlantis.CID_813_Athena_Commando_M_TeriyakiAtlantis",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_801_Athena_Commando_F_GolfSummer.CID_801_Athena_Commando_F_GolfSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_802_Athena_Commando_F_HeartBreaker.CID_802_Athena_Commando_F_HeartBreaker",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_797_Athena_Commando_F_Taco.CID_797_Athena_Commando_F_Taco",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_799_Athena_Commando_F_CupidDark.CID_799_Athena_Commando_F_CupidDark",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_786_Athena_Commando_F_CavalryBandit_Ghost.CID_786_Athena_Commando_F_CavalryBandit_Ghost",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_787_Athena_Commando_M_Heist_Ghost.CID_787_Athena_Commando_M_Heist_Ghost",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_803_Athena_Commando_F_SharkSuit.CID_803_Athena_Commando_F_SharkSuit",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_808_Athena_Commando_F_ConstellationSun.CID_808_Athena_Commando_F_ConstellationSun",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_805_Athena_Commando_F_PunkDevilSummer.CID_805_Athena_Commando_F_PunkDevilSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_847_Athena_Commando_M_Soy_2AS3C.CID_847_Athena_Commando_M_Soy_2AS3C",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_784_Athena_Commando_F_RenegadeRaiderFire.CID_784_Athena_Commando_F_RenegadeRaiderFire",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_749_Athena_Commando_F_GraffitiAssassin.CID_749_Athena_Commando_F_GraffitiAssassin",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_764_Athena_Commando_F_Loofah.CID_764_Athena_Commando_F_Loofah",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_778_Athena_Commando_M_Gator.CID_778_Athena_Commando_M_Gator",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_785_Athena_Commando_F_Python.CID_785_Athena_Commando_F_Python",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_767_Athena_Commando_F_BlackKnight.CID_767_Athena_Commando_F_BlackKnight",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_776_Athena_Commando_M_ProfessorPup.CID_776_Athena_Commando_M_ProfessorPup",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_717_Athena_Commando_F_BlueFlames.CID_717_Athena_Commando_F_BlueFlames",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_771_Athena_Commando_F_OceanRider.CID_771_Athena_Commando_F_OceanRider",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_770_Athena_Commando_F_MechanicalEngineer.CID_770_Athena_Commando_F_MechanicalEngineer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_747_Athena_Commando_M_BadEgg.CID_747_Athena_Commando_M_BadEgg",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_751_Athena_Commando_F_NeonCatSpy.CID_751_Athena_Commando_F_NeonCatSpy",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_726_Athena_Commando_M_TargetPractice.CID_726_Athena_Commando_M_TargetPractice",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_758_Athena_Commando_M_TechExplorer.CID_758_Athena_Commando_M_TechExplorer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_759_Athena_Commando_F_RapVillainess.CID_759_Athena_Commando_F_RapVillainess",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_752_Athena_Commando_M_Comet.CID_752_Athena_Commando_M_Comet",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_753_Athena_Commando_F_Hostile.CID_753_Athena_Commando_F_Hostile",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_703_Athena_Commando_M_Cyclone.CID_703_Athena_Commando_M_Cyclone",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_761_Athena_Commando_M_CycloneSpace.CID_761_Athena_Commando_M_CycloneSpace",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_662_Athena_Commando_M_FlameSkull.CID_662_Athena_Commando_M_FlameSkull",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_748_Athena_Commando_F_Hitman.CID_748_Athena_Commando_F_Hitman",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_736_Athena_Commando_F_DonutDish.CID_736_Athena_Commando_F_DonutDish",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_724_Athena_Commando_M_SpyTech.CID_724_Athena_Commando_M_SpyTech",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_744_Athena_Commando_F_DuckHero.CID_744_Athena_Commando_F_DuckHero",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_741_Athena_Commando_F_HalloweenBunnySpring.CID_741_Athena_Commando_F_HalloweenBunnySpring",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_740_Athena_Commando_F_CardboardCrew.CID_740_Athena_Commando_F_CardboardCrew",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_728_Athena_Commando_M_MinotaurLuck.CID_728_Athena_Commando_M_MinotaurLuck",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_722_Athena_Commando_M_TeriyakiFishAssassin.CID_722_Athena_Commando_M_TeriyakiFishAssassin",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_725_Athena_Commando_F_AgentX.CID_725_Athena_Commando_F_AgentX",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_715_Athena_Commando_F_TwinDark.CID_715_Athena_Commando_F_TwinDark",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_720_Athena_Commando_F_StreetFashionEmerald.CID_720_Athena_Commando_F_StreetFashionEmerald",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_714_Athena_Commando_M_AnarchyAcresFarmer.CID_714_Athena_Commando_M_AnarchyAcresFarmer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_674_Athena_Commando_F_HoodieBandit.CID_674_Athena_Commando_F_HoodieBandit",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_697_Athena_Commando_F_GraffitiFuture.CID_697_Athena_Commando_F_GraffitiFuture",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_688_Athena_Commando_F_AgentRogue.CID_688_Athena_Commando_F_AgentRogue",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_695_Athena_Commando_F_DesertOpsCamo.CID_695_Athena_Commando_F_DesertOpsCamo",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_691_Athena_Commando_F_TNTina.CID_691_Athena_Commando_F_TNTina",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_690_Athena_Commando_F_Photographer.CID_690_Athena_Commando_F_Photographer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_699_Athena_Commando_F_BrokenHeart.CID_699_Athena_Commando_F_BrokenHeart",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_704_Athena_Commando_F_LollipopTrickster.CID_704_Athena_Commando_F_LollipopTrickster",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_625_Athena_Commando_F_PinkTrooper.CID_625_Athena_Commando_F_PinkTrooper",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_623_Athena_Commando_M_Frogman.CID_623_Athena_Commando_M_Frogman",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_683_Athena_Commando_F_TigerFashion.CID_683_Athena_Commando_F_TigerFashion",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_670_Athena_Commando_F_ArcticCamo.CID_670_Athena_Commando_F_ArcticCamo",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_673_Athena_Commando_F_ArcticCamo_Slate.CID_673_Athena_Commando_F_ArcticCamo_Slate",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_605_Athena_Commando_M_TourBus.CID_605_Athena_Commando_M_TourBus",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_577_Athena_Commando_F_CODSquadPlaid.CID_577_Athena_Commando_F_CODSquadPlaid",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_648_Athena_Commando_F_MsAlpine.CID_648_Athena_Commando_F_MsAlpine",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_650_Athena_Commando_F_HolidayPJ_B.CID_650_Athena_Commando_F_HolidayPJ_B",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_634_Athena_Commando_F_GalileoRocket_ARVEH.CID_634_Athena_Commando_F_GalileoRocket_ARVEH",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_616_Athena_Commando_F_CavalryBandit.CID_616_Athena_Commando_F_CavalryBandit",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_617_Athena_Commando_F_ForestQueen.CID_617_Athena_Commando_F_ForestQueen",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_614_Athena_Commando_M_MissingLink.CID_614_Athena_Commando_M_MissingLink",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_510_Athena_Commando_F_AngelEclipse.CID_510_Athena_Commando_F_AngelEclipse",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_574_Athena_Commando_F_CubeRockerPunk.CID_574_Athena_Commando_F_CubeRockerPunk",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_586_Athena_Commando_F_PunkDevil.CID_586_Athena_Commando_F_PunkDevil",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_575_Athena_Commando_F_BulletBlue.CID_575_Athena_Commando_F_BulletBlue",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_593_Athena_Commando_F_SoccerZombieA.CID_593_Athena_Commando_F_SoccerZombieA",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_598_Athena_Commando_M_Mastermind.CID_598_Athena_Commando_M_Mastermind",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_583_Athena_Commando_F_NoshHunter.CID_583_Athena_Commando_F_NoshHunter",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_578_Athena_Commando_F_Fisherman.CID_578_Athena_Commando_F_Fisherman",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_571_Athena_Commando_F_Sheath.CID_571_Athena_Commando_F_Sheath",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_546_Athena_Commando_F_TacticalRed.CID_546_Athena_Commando_F_TacticalRed",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_545_Athena_Commando_F_SushiNinja.CID_545_Athena_Commando_F_SushiNinja",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_523_Athena_Commando_F_Cupid.CID_523_Athena_Commando_F_Cupid",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_479_Athena_Commando_F_Davinci.CID_479_Athena_Commando_F_Davinci",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_482_Athena_Commando_F_PizzaPit.CID_482_Athena_Commando_F_PizzaPit",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_530_Athena_Commando_F_BlackMonday_1BV6J.CID_530_Athena_Commando_F_BlackMonday_1BV6J",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_524_Athena_Commando_F_FutureBikerWhite.CID_524_Athena_Commando_F_FutureBikerWhite",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_532_Athena_Commando_F_Punchy.CID_532_Athena_Commando_F_Punchy",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_511_Athena_Commando_M_CubePaintWildCard.CID_511_Athena_Commando_M_CubePaintWildCard",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_519_Athena_Commando_M_RaptorBlackOps.CID_519_Athena_Commando_M_RaptorBlackOps",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_514_Athena_Commando_F_ToxicKitty.CID_514_Athena_Commando_F_ToxicKitty",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_503_Athena_Commando_F_TacticalWoodlandFuture.CID_503_Athena_Commando_F_TacticalWoodlandFuture",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_480_Athena_Commando_F_Bubblegum.CID_480_Athena_Commando_F_Bubblegum",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_497_Athena_Commando_F_WildWest.CID_497_Athena_Commando_F_WildWest",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_483_Athena_Commando_F_GraffitiRemix.CID_483_Athena_Commando_F_GraffitiRemix",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_405_Athena_Commando_F_Masako.CID_405_Athena_Commando_F_Masako",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_028_Athena_Commando_F.CID_028_Athena_Commando_F",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_017_Athena_Commando_M.CID_017_Athena_Commando_M",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_029_Athena_Commando_F_Halloween.CID_029_Athena_Commando_F_Halloween",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_022_Athena_Commando_F.CID_022_Athena_Commando_F",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_030_Athena_Commando_M_Halloween.CID_030_Athena_Commando_M_Halloween",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_027_Athena_Commando_F.CID_027_Athena_Commando_F",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_024_Athena_Commando_F.CID_024_Athena_Commando_F",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_226_Athena_Commando_F_Octoberfest.CID_226_Athena_Commando_F_Octoberfest",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_278_Athena_Commando_M_Yeti.CID_278_Athena_Commando_M_Yeti",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_309_Athena_Commando_M_StreetGoth.CID_309_Athena_Commando_M_StreetGoth",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_333_Athena_Commando_M_Squishy.CID_333_Athena_Commando_M_Squishy",
        };

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        int DressedThisTick = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            if (!PlayerState->HasCharacterData())
            {
                auto SeasonOneController = (AFortPlayerControllerAthena*)PlayerState->Owner;
                auto SeasonOnePawn = SeasonOneController ? (AFortPlayerPawnAthena*)SeasonOneController->Pawn : nullptr;

                if (SeasonOnePawn && DressedThisTick < 6)
                {
                    auto Dressed = DressedBotPawns.find((void*)SeasonOnePawn);

                    if (Dressed == DressedBotPawns.end() || Dressed->second != (void*)PlayerState)
                    {
                        DressedBotPawns[(void*)SeasonOnePawn] = (void*)PlayerState;
                        DressBotSeasonOne(PlayerState, SeasonOnePawn);
                        DressedThisTick++;
                    }
                }

                continue;
            }

            auto* CharData = &PlayerState->CharacterData;
            if (!CharData)
                continue;

            auto** Parts = (UCustomCharacterPart**)((uint8_t*)CharData + 0x08);

            if (Parts[0] || Parts[1])
                continue;

            auto CID = FindObject<UAthenaCharacterItemDefinition>(DefaultSkins[rand() % DefaultSkins.size()].c_str());
            if (!CID)
            {
                static bool bReported = false;
                if (!bReported) { bReported = true; LogLine("[BotAI] Default CID asset NOT FOUND -- bots will be invisible\n"); }
                continue;
            }

            auto HeroType = CID->HeroDefinition;
            if (!HeroType || !HeroType->HasSpecializations() || HeroType->Specializations.Num() == 0)
                continue;

            auto SpecSoftPtr = HeroType->Specializations[0];
            auto Spec = (UFortHeroSpecialization*)SpecSoftPtr.Get();
            if (!Spec)
                continue;

            for (int j = 0; j < Spec->CharacterParts.Num() && j < 6; j++)
            {
                auto PartSoftPtr = Spec->CharacterParts[j];
                auto Part = (UCustomCharacterPart*)PartSoftPtr.Get();
                if (Part && (int)Part->CharacterPartType < 6)
                {
                    int slot = (int)Part->CharacterPartType;
                    Parts[slot] = Part;
                }
            }

            static UObject* KismetLibCDO = nullptr;
            static bool bKismetLooked = false;
            if (!bKismetLooked)
            {
                bKismetLooked = true;
                KismetLibCDO = (UObject*)FindObject<UObject>(L"/Script/FortniteGame.Default__FortKismetLibrary");
                LogLine("[BotAI] FortKismetLibrary CDO: %s\n", KismetLibCDO ? "found" : "not found");
            }
            if (KismetLibCDO)
            {
                static auto UpdateVisFn = KismetLibCDO->GetFunction("UpdatePlayerCustomCharacterPartsVisualization");
                if (UpdateVisFn)
                    KismetLibCDO->Call<void>(UpdateVisFn, PlayerState);
            }
            PlayerState->OnRep_CharacterData();

            static int dressed = 0;
            if (++dressed <= 3)
                LogLine("[BotAI] Dressed bot #%d via CharacterData path.\n", dressed);
        }
    }

    static std::map<void*, FVector> BotWanderGoals;

    static std::map<void*, FVector> BotAimError;
    static std::map<void*, int> BotAimErrorTick;

    static int GZoneOrdersIssued = 0;
    static int GZoneBotsInStorm = 0;
    static int GZoneBotsClosing = 0;
    static int GZoneBotsStuck = 0;

    static int GWanderUnstuck = 0;

    static void TopUpBotMagazine(void* PlayerStateKey)
    {
        if (!FConfiguration::bBotsNeverRunOutOfAmmo)
            return;

        auto InvIt = BotInventories.find(PlayerStateKey);
        if (InvIt == BotInventories.end() || !InvIt->second)
            return;

        auto WeaponIt = BotWeapons.find(PlayerStateKey);
        if (WeaponIt == BotWeapons.end() || !WeaponIt->second)
            return;

        auto ClipIt = BotWeaponClipSize.find(PlayerStateKey);

        int ClipSize = (ClipIt != BotWeaponClipSize.end() && ClipIt->second > 0) ? ClipIt->second : 30;

        auto Inventory = InvIt->second;
        if (!Inventory->HasInventory())
            return;

        for (int i = 0; i < Inventory->Inventory.ItemInstances.Num(); i++)
        {
            auto Item = Inventory->Inventory.ItemInstances[i];
            if (!Item || !Item->HasItemEntry())
                continue;

            auto& Entry = Item->ItemEntry;

            if (!FFortItemEntry::HasItemDefinition() || Entry.ItemDefinition != WeaponIt->second)
                continue;

            if (!FFortItemEntry::HasLoadedAmmo())
                return;

            int RefillBelow = ClipSize / 3;
            if (RefillBelow < 1)
                RefillBelow = 1;

            if (Entry.LoadedAmmo >= RefillBelow)
                return;

            Entry.LoadedAmmo = ClipSize;
            Inventory->Update(&Entry);
            GMagazinesRefilled++;
            return;
        }
    }

    static bool BotIsHoldingAWeapon(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasCurrentWeapon())
            return false;

        return Pawn->CurrentWeapon != nullptr;
    }

    static int GGunsFoundHolstered = 0;
    static int GGunsUnholstered = 0;
    static int GGunsStillHolstered = 0;
    static int GHolsterLinesLogged = 0;
    static bool GHolsterCallBroken = false;
    static std::string GHolsterReasonsSeen;
    static std::map<void*, float> BotLastUnholster;

    static int BotGunHolstered(AFortPlayerPawnAthena* Pawn)
    {
        static int32 Offset = -2;
        static uint8 Mask = 0;

        if (!Pawn)
            return -1;

        if (Offset == -2)
        {
            Offset = -1;

            auto Prop = ((UObject*)Pawn)->GetProperty("bWeaponHolstered", 0x20000);
            if (Prop)
            {
                const uint32 Off = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
                Mask = Prop->GetFieldMask();

                if (Off > 0 && Off < 0x10000)
                    Offset = (int32)Off;
            }
        }

        if (Offset < 0)
            return -1;

        return (GetFromOffset<uint8>(Pawn, (uint32)Offset) & (Mask ? Mask : (uint8)0xFF)) ? 1 : 0;
    }

    static std::string BotHolsterReasons(AFortPlayerPawnAthena* Pawn)
    {
        static int32 IdsOffset = -2, CounterOffset = -2;

        if (!Pawn)
            return std::string("?");

        if (IdsOffset == -2)
        {
            const uint32 Off = ((UObject*)Pawn)->GetOffset("WeaponHolsterIds");
            IdsOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
        }

        if (CounterOffset == -2)
        {
            const uint32 Off = ((UObject*)Pawn)->GetOffset("WeaponHolsterCounter");
            CounterOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
        }

        std::string Out;

        if (IdsOffset >= 0)
        {
            auto& Ids = GetFromOffset<TArray<FName>>(Pawn, (uint32)IdsOffset);
            const int Count = Ids.Num();

            if (Count > 0 && Count < 64 && Ids.Data)
            {
                for (int i = 0; i < Count; i++)
                {
                    if (!Out.empty())
                        Out += ", ";

                    auto Text = Ids.Data[i].ToString();
                    Out += Text.empty() ? std::string("None") : std::string(Text.c_str());
                }
            }
            else
            {
                Out = "no holster reasons listed";
            }
        }
        else
        {
            Out = "this build lists no holster reasons";
        }

        if (CounterOffset >= 0)
            Out += " | holster counter " + std::to_string(GetFromOffset<int32>(Pawn, (uint32)CounterOffset));

        return Out;
    }

    static bool UnholsterRaw(AFortPlayerPawnAthena* Pawn, UFunction* Fn, uint32 ForceOffset, uint32 HolsterOffset)
    {
        alignas(16) uint8 Params[0x80];
        memset(Params, 0, sizeof(Params));

        Params[HolsterOffset] = 0;
        Params[ForceOffset] = 1;

        ((UObject*)Pawn)->ProcessEvent(Fn, Params);
        return true;
    }

    static bool UnholsterGuarded(AFortPlayerPawnAthena* Pawn, UFunction* Fn, uint32 ForceOffset, uint32 HolsterOffset)
    {
        __try
        {
            return UnholsterRaw(Pawn, Fn, ForceOffset, HolsterOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void KeepBotGunOut(AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, float Now, const char* When)
    {
        if (!Pawn || GHolsterCallBroken || BotGunHolstered(Pawn) != 1)
            return;

        auto Last = BotLastUnholster.find((void*)Pawn);
        if (Last != BotLastUnholster.end() && Now - Last->second < 1.f)
            return;

        BotLastUnholster[(void*)Pawn] = Now;

        static UFunction* Fn = nullptr;
        static uint32 ForceOffset = (uint32)-1, HolsterOffset = (uint32)-1;
        static bool bLooked = false;

        if (!bLooked)
        {
            bLooked = true;
            Fn = ((UObject*)Pawn)->GetFunction("SetHolsterWeapon");

            if (Fn)
            {
                auto Params = Fn->GetParamsNamed();

                for (auto& P : Params.NameOffsetMap)
                {
                    if (P.Name == "bForce")
                        ForceOffset = P.Offset;
                    else if (P.Name == "bHolster")
                        HolsterOffset = P.Offset;
                }

                if (Params.Size > 0x80 || ForceOffset >= 0x80 || HolsterOffset >= 0x80)
                    Fn = nullptr;
            }

            if (!Fn)
            {
                GHolsterCallBroken = true;
                LogLine("[ARMS] A bot's gun is holstered, and this build's SetHolsterWeapon(bHolster, ..., bForce) was NOT found -- "
                        "it cannot be taken out of the holster.");
                return;
            }
        }

        GGunsFoundHolstered++;

        const std::string Reasons = BotHolsterReasons(Pawn);

        if (GHolsterReasonsSeen.size() < 300 && GHolsterReasonsSeen.find(Reasons) == std::string::npos)
        {
            if (!GHolsterReasonsSeen.empty())
                GHolsterReasonsSeen += " / ";
            GHolsterReasonsSeen += Reasons;
        }

        if (!UnholsterGuarded(Pawn, Fn, ForceOffset, HolsterOffset))
        {
            GHolsterCallBroken = true;
            LogLine("[ARMS] The game's SetHolsterWeapon faulted on '%s' -- bots' guns are left as the game has them from now on.",
                    SafeNameOf(PlayerState).c_str());
            return;
        }

        const int After = BotGunHolstered(Pawn);

        if (After == 0)
            GGunsUnholstered++;
        else
            GGunsStillHolstered++;

        if (++GHolsterLinesLogged <= 6)
            LogLine("[ARMS] '%s's gun (%s) was holstered %s -- nobody could see it and it could not fire (holster reasons: %s). "
                    "Taken out with the game's own SetHolsterWeapon (forced): holstered now: %s.",
                    SafeNameOf(PlayerState).c_str(),
                    BotIsHoldingAWeapon(Pawn) && ((AFortWeapon*)Pawn->CurrentWeapon)->HasWeaponData()
                        ? BotObjectName((const UObject*)((AFortWeapon*)Pawn->CurrentWeapon)->WeaponData).c_str()
                        : "nothing in its hands",
                    When, Reasons.c_str(), After == 0 ? "no" : (After == 1 ? "YES, still" : "cannot be read"));
    }

    static int32* WeaponAmmoCount(AActor* Weapon)
    {
        if (!Weapon)
            return nullptr;

        static int32 Offset = -2;

        if (Offset == -2)
        {
            Offset = (int32)Weapon->GetOffset("AmmoCount");
            LogLine("[RELOAD] The weapon's live round counter (AmmoCount) %s.",
                    Offset >= 0 ? "was found -- bots will reload" : "is NOT on this build's weapon class");
        }

        if (Offset < 0)
            return nullptr;

        return &GetFromOffset<int32>(Weapon, (uint32)Offset);
    }

    static int GDamageAirHits = 0;
    static int GDamageGroundHits = 0;
    static int GDamageOtherHits = 0;
    static double GDamageAirTotal = 0.0;
    static double GDamageGroundTotal = 0.0;
    static int GDamageAirLogged = 0;
    static int GDamageGroundLogged = 0;
    static int GRoundsFiredByPlayers = 0;
    static std::map<void*, float> GBotLifeSeen;
    static std::map<void*, std::pair<void*, int32>> GPlayerAmmoSeen;

    static void WatchDamageToBots(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray() || !bBusDeparted)
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || NPCs::IsNPC(PS))
                continue;

            auto Controller = (AActor*)PS->Owner;
            if (!Controller)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Controller)->Pawn;
            if (!Pawn)
                continue;

            if (!(PS->HasbIsABot() && PS->bIsABot))
            {
                if (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
                {
                    auto Weapon = (AActor*)Pawn->CurrentWeapon;

                    if (int32* Ammo = WeaponAmmoCount(Weapon))
                    {
                        auto& Seen = GPlayerAmmoSeen[(void*)Controller];

                        if (Seen.first == (void*)Weapon && *Ammo < Seen.second)
                            GRoundsFiredByPlayers += Seen.second - *Ammo;

                        Seen = { (void*)Weapon, *Ammo };
                    }
                }

                continue;
            }

            const float Life = Pawn->GetHealth() + Pawn->GetShield();
            auto Prev = GBotLifeSeen.find((void*)Pawn);
            const float Before = (Prev != GBotLifeSeen.end()) ? Prev->second : Life;

            GBotLifeSeen[(void*)Pawn] = Life;

            if (!(Life < Before - 0.5f))
                continue;

            if (Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone)
                continue;

            auto Last = BotLastDamager.find((void*)Controller);
            auto Damager = (Last != BotLastDamager.end()) ? (AFortPlayerStateAthena*)Last->second.first : nullptr;

            if (!Damager || (Damager->HasbIsABot() && Damager->bIsABot))
            {
                GDamageOtherHits++;
                continue;
            }

            const bool bInAir = (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) || (Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen);
            const float Amount = Before - Life;

            if (bInAir)
            {
                GDamageAirHits++;
                GDamageAirTotal += Amount;
            }
            else
            {
                GDamageGroundHits++;
                GDamageGroundTotal += Amount;
            }

            int& Logged = bInAir ? GDamageAirLogged : GDamageGroundLogged;

            if (++Logged <= (bInAir ? 3 : 8))
                LogLine("[DAMAGE] '%s' took %.0f damage from '%s' while %s (health + shield %.0f -> %.0f).", SafeNameOf(PS).c_str(),
                        Amount, SafeNameOf(Damager).c_str(), bInAir ? "in the air" : "on the ground", Before, Life);
        }
    }

    static UObject* GCueManager = nullptr;
    static int32 GCueOpenOffset = -2;
    static int32 GCueWaitingOffset = -1;
    static float GCueLookAgainAt = 0.f;
    static float GCueStuckSince = -1.f;
    static int GCueResets = 0;
    static int GCueMostOpen = 0;
    static int GCueMostWaiting = 0;

    static void FindCueSender(float Now)
    {
        if (GCueManager || Now < GCueLookAgainAt)
            return;

        GCueLookAgainAt = Now + 60.f;

        UObject* Manager = nullptr;
        const char* Where = "the object list";

        if (auto Globals = (UObject*)TUObjectArray::FindFirstObject("AbilitySystemGlobals"))
        {
            const uint32 Off = Globals->GetOffset("GlobalGameplayCueManager");

            if (Off != (uint32)-1)
            {
                Manager = GetFromOffset<UObject*>(Globals, Off);
                Where = "the ability system's globals";
            }
        }

        if (!Manager)
        {
            Manager = (UObject*)TUObjectArray::FindFirstObject("GameplayCueManager");
            Where = "the object list";
        }

        const uint32 Open = Manager ? Manager->GetOffset("GameplayCueSendContextCount") : (uint32)-1;
        const uint32 Waiting = Manager ? Manager->GetOffset("PendingExecuteCues") : (uint32)-1;
        const bool bFirstLook = GCueOpenOffset == -2;

        if (Manager && Open != (uint32)-1)
        {
            GCueManager = Manager;
            GCueOpenOffset = (int32)Open;
            GCueWaitingOffset = (Waiting == (uint32)-1) ? -1 : (int32)Waiting;

            LogLine("[CUES] Found the game's gameplay-cue sender (%s, through %s). Hit cues -- the damage numbers among them -- only go out "
                    "while its count of open send scopes is back at 0 between ticks. Watched from now on: if a fault inside the game's code "
                    "leaves one open for over a second, it is put back.",
                    BotObjectName(Manager).c_str(), Where);
            return;
        }

        GCueOpenOffset = -1;

        if (bFirstLook)
            LogLine("[CUES] The game's gameplay-cue sender %s -- not watched (looked for again every minute).",
                    Manager ? "has no readable count of open send scopes on this build" : "could not be found yet");
    }

    static void WatchCueSender()
    {
        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        FindCueSender(Now);

        if (!GCueManager || GCueOpenOffset < 0)
            return;

        int32& Open = GetFromOffset<int32>(GCueManager, (uint32)GCueOpenOffset);
        TArray<uint8>* Waiting = (GCueWaitingOffset >= 0) ? &GetFromOffset<TArray<uint8>>(GCueManager, (uint32)GCueWaitingOffset) : nullptr;
        const int WaitingNow = Waiting ? Waiting->Num() : -1;

        if (Open > GCueMostOpen)
            GCueMostOpen = Open;

        if (WaitingNow > GCueMostWaiting)
            GCueMostWaiting = WaitingNow;

        if (Open == 0 || !World)
        {
            GCueStuckSince = -1.f;
            return;
        }

        if (GCueStuckSince < 0.f)
        {
            GCueStuckSince = Now;
            return;
        }

        if (Now - GCueStuckSince < 1.f)
            return;

        GCueStuckSince = -1.f;
        GCueResets++;

        LogLine("[CUES] The game's gameplay-cue sender was stuck: its count of open send scopes sat at %d between ticks for over a second (a "
                "fault inside the game's code left a scope open) with %d hit cue(s) held back -- no damage numbers or hit effects go out "
                "while it is stuck. Count put back to 0 and the held cues dropped (put back %d time(s) so far).",
                Open, WaitingNow, GCueResets);

        Open = 0;

        if (Waiting && WaitingNow > 0)
            Waiting->NumElements = 0;
    }

    static void LogCueSenderSummary()
    {
        if (!GCueManager || GCueOpenOffset < 0)
            return;

        const int OpenNow = GetFromOffset<int32>(GCueManager, (uint32)GCueOpenOffset);
        const int WaitingNow = (GCueWaitingOffset >= 0) ? GetFromOffset<TArray<uint8>>(GCueManager, (uint32)GCueWaitingOffset).Num() : -1;

        LogLine("[CUES] The game's gameplay-cue sender (damage numbers go out through it): open send scopes right now %d, cues waiting %d | "
                "most seen between ticks: %d open, %d waiting | found stuck and put back %d time(s).",
                OpenNow, WaitingNow, GCueMostOpen, GCueMostWaiting, GCueResets);
    }

    static bool BotWeaponIsEmpty(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasCurrentWeapon() || !Pawn->CurrentWeapon)
            return false;

        auto Ammo = WeaponAmmoCount((AActor*)Pawn->CurrentWeapon);

        return Ammo && *Ammo <= 0;
    }

    static int BotMagazineSize(void* PlayerStateKey)
    {
        auto ClipIt = BotWeaponClipSize.find(PlayerStateKey);
        int ClipSize = (ClipIt != BotWeaponClipSize.end() && ClipIt->second > 0) ? ClipIt->second : 30;

        if (ClipSize < 4)
            ClipSize = 4;

        return ClipSize;
    }

    class BAIPerceptionComponent : public UObject
    {
    public:
        UCLASS_COMMON_MEMBERS(BAIPerceptionComponent);

        DEFINE_FUNC(SetSenseEnabled, void);
        DEFINE_FUNC(ForgetAll, void);
    };

    static std::set<void*> BotSensesSilenced;
    static int GSensesSilencedBots = 0;
    static bool GSensesFaulted = false;

    static void SilenceSensesRaw(UObject* Perception, UClass** Classes, int Count)
    {
        auto Senses = (BAIPerceptionComponent*)Perception;

        for (int s = 0; s < Count; s++)
        {
            if (!Classes[s])
                continue;

            UClass* SenseClass = Classes[s];
            bool bEnable = false;
            Senses->SetSenseEnabled(SenseClass, bEnable);
        }

        Senses->ForgetAll();
    }

    static bool SilenceSensesGuarded(UObject* Perception, UClass** Classes, int Count)
    {
        __try
        {
            SilenceSensesRaw(Perception, Classes, Count);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GSensesFaulted = true;
            return false;
        }
    }

    static void SilenceNativeSenses(::AAIController* AIController)
    {
        if (!AIController || GSensesFaulted || BotSensesSilenced.find((void*)AIController) != BotSensesSilenced.end())
            return;

        BotSensesSilenced.insert((void*)AIController);

        static int32 PerceptionOffset = -2;
        static int32 BlueprintPerceptionOffset = -2;

        if (PerceptionOffset == -2)
        {
            PerceptionOffset = (int32)AIController->GetOffset("PerceptionComponent");
            BlueprintPerceptionOffset = (int32)AIController->GetOffset("AIPerception");
        }

        UObject* Perception = nullptr;

        if (PerceptionOffset >= 0)
            Perception = GetFromOffset<UObject*>(AIController, (uint32)PerceptionOffset);

        if (!Perception && BlueprintPerceptionOffset >= 0)
            Perception = GetFromOffset<UObject*>(AIController, (uint32)BlueprintPerceptionOffset);

        if (!Perception)
        {
            static bool bSaid = false;

            if (!bSaid)
            {
                bSaid = true;
                LogLine("[SENSES] This bot controller has no built-in senses to switch off -- nothing to do.");
            }

            return;
        }

        static const char* SenseNames[] = { "AISense_Sight", "AISense_Hearing", "AthenaAISense_Hearing", "AISense_Damage",
                                            "AISense_Team", "AISense_Touch", "AISense_Prediction", "AISense_Blueprint" };
        static const int SenseCount = (int)(sizeof(SenseNames) / sizeof(SenseNames[0]));
        static UClass* SenseClasses[8] = {};
        static int SensesFound = 0;
        static bool bLookedUp = false;

        if (!bLookedUp)
        {
            bLookedUp = true;

            for (int s = 0; s < SenseCount && s < 8; s++)
            {
                SenseClasses[s] = const_cast<UClass*>(FindClass(SenseNames[s]));

                if (SenseClasses[s])
                    SensesFound++;
            }
        }

        if (!SilenceSensesGuarded(Perception, SenseClasses, SenseCount < 8 ? SenseCount : 8))
        {
            LogLine("[SENSES] Switching off the game's own built-in bot senses faulted on this build -- they are left alone "
                    "(the bot code still picks its own targets; the aim focus still keeps its bursts on them).");
            return;
        }

        GSensesSilencedBots++;

        if (GSensesSilencedBots == 1)
            LogLine("[SENSES] Switched off the game's own built-in bot senses (%d kinds found: sight, hearing, being shot...). "
                    "They fed Epic's own target picker, which kept turning bots to aim at whoever it picked -- only the "
                    "bot code's lock-on picks targets now.", SensesFound);
    }

    static bool GCameraResolved = false;
    static int32 GCamManagerOffset = -1, GCamCacheOffset = -1, GCamStampOffset = -1, GCamPovOffset = -1;
    static int32 GPovLocationOffset = -1, GPovRotationOffset = -1, GPovFovOffset = -1;

    static void KeepBotCameraOnItsEyes(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, FRotator Aim)
    {
        if (!Controller || !Pawn)
            return;

        if (!GCameraResolved)
        {

            const uint32 ManagerOff = ((UObject*)Controller)->GetOffset("PlayerCameraManager");
            auto Manager = (ManagerOff != (uint32)-1) ? *(UObject**)((uint8*)Controller + ManagerOff) : nullptr;

            if (ManagerOff != (uint32)-1 && !Manager)
                return;

            GCameraResolved = true;

            auto ToInt = [](uint32 Off) -> int32 { return Off == (uint32)-1 ? -1 : (int32)Off; };

            auto Entry = FindStruct("CameraCacheEntry");
            auto View = FindStruct("MinimalViewInfo");

            GCamManagerOffset = ToInt(ManagerOff);
            GCamCacheOffset = Manager ? ToInt(Manager->GetOffset("CameraCache")) : -1;
            GCamStampOffset = Entry ? ToInt(Entry->GetOffset("TimeStamp")) : -1;
            GCamPovOffset = Entry ? ToInt(Entry->GetOffset("POV")) : -1;
            GPovLocationOffset = View ? ToInt(View->GetOffset("Location")) : -1;
            GPovRotationOffset = View ? ToInt(View->GetOffset("Rotation")) : -1;
            GPovFovOffset = View ? ToInt(View->GetOffset("FOV")) : -1;

            const bool bUsable = GCamManagerOffset >= 0 && GCamCacheOffset >= 0 && GCamStampOffset >= 0 && GCamPovOffset >= 0 &&
                                 GPovLocationOffset >= 0 && GPovRotationOffset >= 0;

            LogLine("[FIRE] A bot's camera %s.", bUsable ? "can be kept on its eyes while it aims"
                                                       : "could NOT be found on this build -- left where it is");

            if (!bUsable)
                GCamManagerOffset = -1;
        }

        if (GCamManagerOffset < 0)
            return;

        uint8* Manager = *(uint8**)((uint8*)Controller + GCamManagerOffset);
        if (!Manager)
            return;

        uint8* Cache = Manager + GCamCacheOffset;
        uint8* Pov = Cache + GCamPovOffset;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 1.f;

        const FVector At = Pawn->K2_GetActorLocation();
        FVector Eyes(At.X, At.Y, At.Z + 60.0);

        memcpy(Pov + GPovLocationOffset, (const void*)&Eyes, (size_t)FVector::Size());
        memcpy(Pov + GPovRotationOffset, (const void*)&Aim, (size_t)FRotator::Size());

        if (GPovFovOffset >= 0 && !(*(float*)(Pov + GPovFovOffset) >= 1.f))
            *(float*)(Pov + GPovFovOffset) = 90.f;

        *(float*)(Cache + GCamStampOffset) = (Now > 0.f) ? Now : 1.f;
    }

    static const float kBotFallbackDamage = 20.f;
    static const float kBotFallbackRate = 3.f;

    static void BotGunTrigger(AFortPlayerPawnAthena* Pawn, bool bPull, AFortPlayerPawnAthena* Target = nullptr, float DistanceMetres = 0.f);

    struct FBotGunNumbers
    {
        bool bRead = false;
        bool bValid = false;
        float Dmg[4] = {};
        float Rng[4] = {};
        int Pellets = 1;
        float ReloadTime = 0.f;
        float FireRate = 0.f;
        bool bOneRoundAtATime = false;
    };

    static std::map<const void*, FBotGunNumbers> GBotGunNumbers;

    static const FBotGunNumbers& BotGunNumbers(const UFortItemDefinition* Def)
    {
        FBotGunNumbers& Numbers = GBotGunNumbers[(const void*)Def];

        if (Numbers.bRead)
            return Numbers;

        Numbers.bRead = true;

        static const char* Fields[11] = { "DmgPB", "DmgMid", "DmgLong", "DmgMaxRange", "RngPB", "RngMid", "RngLong", "RngMax",
                                          "BulletsPerCartridge", "ReloadTime", "FiringRate" };
        static int32 Offsets[11] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        static int32 OneAtATimeOffset = -1;
        static uint8 OneAtATimeMask = 0;
        static int32 ReloadTypeOffset = -1;
        static bool bResolved = false;

        if (!bResolved)
        {
            bResolved = true;

            auto Struct = FindStruct("FortRangedWeaponStats");
            int Found = 0;

            for (int f = 0; f < 11; f++)
            {
                const uint32 Off = Struct ? Struct->GetOffset(Fields[f]) : (uint32)-1;
                Offsets[f] = (Off == (uint32)-1 || Off > 0x1000) ? -1 : (int32)Off;

                if (Offsets[f] >= 0)
                    Found++;
            }

            if (auto Prop = Struct ? Struct->GetProperty("bReloadIndividualBullets") : nullptr)
            {
                const uint32 Off = Struct->GetOffset("bReloadIndividualBullets");
                OneAtATimeOffset = (Off == (uint32)-1 || Off > 0x1000) ? -1 : (int32)Off;
                OneAtATimeMask = Prop->GetFieldMask();
            }
            else if (Struct)
            {

                const uint32 Off = Struct->GetOffset("ReloadType");
                ReloadTypeOffset = (Off == (uint32)-1 || Off > 0x1000) ? -1 : (int32)Off;
            }

            LogLine("[HITS] The weapon data table's fields: %d of 11 found on this build (DmgPB at %d, BulletsPerCartridge at %d, "
                    "ReloadTime at %d, FiringRate at %d, bReloadIndividualBullets at %d, ReloadType at %d).",
                    Found, Offsets[0], Offsets[8], Offsets[9], Offsets[10], OneAtATimeOffset, ReloadTypeOffset);
        }

        auto Weapon = Def ? Def->Cast<UFortWeaponItemDefinition>() : nullptr;
        auto Stats = Weapon ? (const uint8*)AFortInventory::GetStats(Weapon) : nullptr;

        if (!Stats)
            return Numbers;

        auto ReadFloat = [&](int f) -> float
        {
            if (Offsets[f] < 0)
                return 0.f;

            const float Value = *(const float*)(Stats + Offsets[f]);
            return (Value >= 0.f && Value < 1000000.f) ? Value : 0.f;
        };

        for (int f = 0; f < 4; f++)
        {
            Numbers.Dmg[f] = ReadFloat(f);
            Numbers.Rng[f] = ReadFloat(4 + f);
        }

        Numbers.Pellets = (Offsets[8] >= 0) ? *(const int32*)(Stats + Offsets[8]) : 1;
        if (Numbers.Pellets < 1 || Numbers.Pellets > 32)
            Numbers.Pellets = 1;

        Numbers.ReloadTime = ReadFloat(9);
        Numbers.FireRate = ReadFloat(10);
        Numbers.bOneRoundAtATime = (OneAtATimeOffset >= 0 && (Stats[OneAtATimeOffset] & (OneAtATimeMask ? OneAtATimeMask : (uint8)0xFF)) != 0) ||
                                   (ReloadTypeOffset >= 0 && Stats[ReloadTypeOffset] == 1);
        Numbers.bValid = Numbers.Dmg[0] >= 1.f && Numbers.Dmg[0] < 1000.f;

        static int Reported = 0;
        if (++Reported <= 12)
            LogLine("[HITS] %s: %.1f damage up to %.0f m, %.1f at %.0f m, %.1f at %.0f m, %.1f at %.0f m | %d pellet(s) a shot | %.2f shots a "
                    "second | reload %.2fs%s%s.",
                    BotObjectName((const UObject*)Def).c_str(), Numbers.Dmg[0], Numbers.Rng[0] / 100.f, Numbers.Dmg[1], Numbers.Rng[1] / 100.f,
                    Numbers.Dmg[2], Numbers.Rng[2] / 100.f, Numbers.Dmg[3], Numbers.Rng[3] / 100.f, Numbers.Pellets, Numbers.FireRate,
                    Numbers.ReloadTime, Numbers.bOneRoundAtATime ? " a round" : "",
                    Numbers.bValid ? "" : " -- its damage is NOT readable, the bot code deals 20 a round with it");

        return Numbers;
    }

    static float BotRoundDamage(const FBotGunNumbers& Numbers, float DistanceUnits)
    {
        if (!Numbers.bValid)
            return 0.f;

        auto Lerp = [](float A, float B, float T) { return A + (B - A) * (T < 0.f ? 0.f : (T > 1.f ? 1.f : T)); };

        const float* D = Numbers.Dmg;
        const float* R = Numbers.Rng;
        float Damage = D[0];

        if (R[0] > 0.f && DistanceUnits > R[0] && D[1] > 0.f && R[1] > R[0])
        {
            if (DistanceUnits <= R[1])
                Damage = Lerp(D[0], D[1], (DistanceUnits - R[0]) / (R[1] - R[0]));
            else if (D[2] > 0.f && R[2] > R[1] && DistanceUnits <= R[2])
                Damage = Lerp(D[1], D[2], (DistanceUnits - R[1]) / (R[2] - R[1]));
            else if (D[2] > 0.f && R[2] > R[1] && D[3] > 0.f && R[3] > R[2])
                Damage = (DistanceUnits <= R[3]) ? Lerp(D[2], D[3], (DistanceUnits - R[2]) / (R[3] - R[2])) : D[3];
            else
                Damage = (D[2] > 0.f && R[2] > R[1]) ? D[2] : D[1];
        }

        if (Numbers.Pellets > 1)
        {
            float Share = 0.95f - DistanceUnits / 2500.f;
            if (Share < 0.2f)
                Share = 0.2f;

            Damage = (Damage * (float)Numbers.Pellets > 300.f) ? Damage * Share : Damage * (float)Numbers.Pellets * Share;
        }

        if (Damage > 150.f)
            Damage = 150.f;

        return Damage;
    }

    static int GCodeHits[2] = { 0, 0 };
    static double GCodeDamage[2] = { 0.0, 0.0 };
    static int GCodeKills[2] = { 0, 0 };
    static int GCodeKillsNotByForceKill = 0;
    static int GCodeHitLines = 0;
    static int GCodeHitsHeldByGodMode = 0;

    static const char* TargetKindName(int Kind)
    {
        return Kind == 1 ? "real players" : "bots";
    }

    static int PawnTargetKind(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->Controller)
            return -1;

        auto State = (AFortPlayerStateAthena*)((AFortPlayerControllerAthena*)Pawn->Controller)->PlayerState;
        if (!State || NPCs::IsNPC(State))
            return -1;

        return (State->HasbIsABot() && State->bIsABot) ? 0 : 1;
    }

    struct FBotShotAim
    {
        void* Target = nullptr;
        bool bOnTarget = false;
        bool bCanShoot = false;
        float Distance = 0.f;
    };

    static std::map<void*, FBotShotAim> BotShotAim;

    static UFunction* GForceKillFn = nullptr;
    static bool GForceKillLooked = false;
    static bool GForceKillBroken = false;
    static uint32 GForceKillController = 0, GForceKillActor = 0;

    static bool ForceKillRaw(UObject* Pawn, UFunction* Fn, uint8* Params)
    {
        Pawn->ProcessEvent(Fn, Params);
        return true;
    }

    static bool ForceKillGuarded(UObject* Pawn, UFunction* Fn, uint8* Params)
    {
        __try
        {
            return ForceKillRaw(Pawn, Fn, Params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool BotForceKill(AFortPlayerPawnAthena* Target, AFortPlayerControllerAthena* KillerController, AFortPlayerPawnAthena* KillerPawn)
    {
        if (GForceKillBroken || !Target)
            return false;

        if (!GForceKillLooked)
        {
            GForceKillLooked = true;

            auto Fn = ((UObject*)Target)->GetFunction("ForceKill");
            int Found = 0;
            uint32 Size = 0;

            if (Fn)
            {
                auto Params = Fn->GetParamsNamed();
                Size = Params.Size;

                for (auto& P : Params.NameOffsetMap)
                {
                    if (P.Name == "DeathReason" && P.ElementSize == 8)
                        Found |= 1;
                    else if (P.Name == "KillerController" && P.ElementSize == 8)
                    {
                        GForceKillController = P.Offset;
                        Found |= 2;
                    }
                    else if (P.Name == "KillerActor" && P.ElementSize == 8)
                    {
                        GForceKillActor = P.Offset;
                        Found |= 4;
                    }
                }
            }

            if (Fn && Found == 7 && Size > 0 && Size <= 0x100)
                GForceKillFn = Fn;

            LogLine("[HITS] The game's own ForceKill %s.", GForceKillFn ? "was found -- a bot's finishing shot eliminates through it, naming the bot"
                                                                       : "is NOT usable on this build -- a bot's finishing shot sets the health to 0");
        }

        if (!GForceKillFn)
            return false;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));

        *(void**)(Params + GForceKillController) = (void*)KillerController;
        *(void**)(Params + GForceKillActor) = (void*)KillerPawn;

        if (!ForceKillGuarded((UObject*)Target, GForceKillFn, Params))
        {
            GForceKillBroken = true;
            LogLine("[HITS] The game's ForceKill faulted -- a bot's finishing shot sets the health to 0 from now on.");
            return false;
        }

        return true;
    }

    static bool BotHitTarget(AFortPlayerPawnAthena* Shooter, AFortPlayerControllerAthena* ShooterController, AFortPlayerStateAthena* ShooterState,
                             AFortPlayerPawnAthena* Target, float Damage, int Kind, const UFortItemDefinition* Gun, bool* OutShield = nullptr,
                             bool* OutShieldGone = nullptr, bool* OutApplied = nullptr)
    {
        if (OutApplied)
            *OutApplied = false;

        if (!Target || !(Damage > 0.f) || Kind < 0)
            return false;

        if (Target->HasbIsInvulnerable() && Target->bIsInvulnerable)
            return false;

        const float Health = Target->GetHealth();
        if (Health <= 0.f)
            return true;

        const float Shield = Target->GetShield();
        const float FromShield = (Shield > 0.f) ? (Shield < Damage ? Shield : Damage) : 0.f;
        const float Rest = Damage - FromShield;
        float NewHealth = Health - Rest;

        if (OutShield)
            *OutShield = FromShield > 0.f;
        if (OutShieldGone)
            *OutShieldGone = FromShield > 0.f && Shield - FromShield <= 0.f;
        if (OutApplied)
            *OutApplied = true;

        const float Floor = (Target->HasHealthSet() && Target->HealthSet && FFortGameplayAttributeData::HasMinimum())
                                ? (float)Target->HealthSet->Health.Minimum
                                : 0.f;

        if (Floor >= 1.f && NewHealth < Floor)
        {
            NewHealth = Floor;
            GCodeHitsHeldByGodMode++;
        }

        if (auto TargetController = (AActor*)Target->Controller)
        {
            BotCodeLastHit[(void*)TargetController] = { (void*)ShooterState, GStormTick };

            if (Kind == 0)
            {
                BotLastDamager[(void*)TargetController] = { (void*)ShooterState, GStormTick };
                LastPlayerDamageTick[(void*)TargetController] = GStormTick;

                auto Under = BotUnderFireSince.find((void*)TargetController);
                if (Under == BotUnderFireSince.end() || (GStormTick - Under->second) > 60)
                    BotUnderFireSince[(void*)TargetController] = GStormTick;
            }
        }

        GCodeHits[Kind]++;
        GCodeDamage[Kind] += (double)Damage;

        if (NewHealth > 0.5f)
        {
            if (FromShield > 0.f)
                Target->SetShield(Shield - FromShield);
            if (Rest > 0.f)
                Target->SetHealth(NewHealth);

            if (++GCodeHitLines <= 10 || (GCodeHitLines % 250) == 0)
                LogLine("[HITS] '%s' hit '%s' with its %s for %.0f (shield %.0f -> %.0f, health %.0f -> %.0f) -- the bot code's hit #%d on %s.",
                        SafeNameOf(ShooterState).c_str(), BotPawnName(Target).c_str(), BotObjectName((const UObject*)Gun).c_str(), Damage,
                        Shield, Shield - FromShield, Health, NewHealth, GCodeHits[Kind], TargetKindName(Kind));
            return false;
        }

        const bool bForceKilled = BotForceKill(Target, ShooterController, Shooter);

        if (!bForceKilled)
        {
            GCodeKillsNotByForceKill++;

            if (FromShield > 0.f)
                Target->SetShield(0.f);
            Target->SetHealth(0.f);
        }

        GCodeKills[Kind]++;

        LogLine("[HITS] '%s' finished off '%s' with its %s (%.0f damage, %.0f health and %.0f shield left) -- %s | health read back: %.0f.",
                SafeNameOf(ShooterState).c_str(), BotPawnName(Target).c_str(), BotObjectName((const UObject*)Gun).c_str(), Damage, Health, Shield,
                bForceKilled ? "through the game's own ForceKill, naming the bot" : "health set to 0 (no ForceKill)", Target->GetHealth());

        return true;
    }

    static void LogBotDamageSummary()
    {
        LogLine("[HITS] Bot bullets that landed (the bot code's own hits): on bots %d for %.0f damage, %d finished off | on real players %d "
                "for %.0f damage, %d finished off | hits held at a god-mode health floor %d | finishing hits not through ForceKill: %d.",
                GCodeHits[0], GCodeDamage[0], GCodeKills[0], GCodeHits[1], GCodeDamage[1], GCodeKills[1], GCodeHitsHeldByGodMode,
                GCodeKillsNotByForceKill);
    }

    static float PawnLife(AFortPlayerPawnAthena* Pawn)
    {
        return Pawn ? Pawn->GetHealth() + Pawn->GetShield() : -1.f;
    }

    static int GReloadsRefilledByBotCode = 0;
    static int GReloadsCancelled = 0;
    static int GReloadsTopOff = 0;
    static int GStuckReloadFlags = 0;
    static int GStuckEquipFlags = 0;
    static int GReloadLines = 0;

    struct FGunFlag
    {
        int32 Offset = -2;
        uint8 Mask = 0;
    };

    static FGunFlag GGunReloadingFlag;
    static FGunFlag GGunEquippingFlag;

    static void FindGunFlag(AActor* Weapon, FGunFlag& Flag, const char* First, const char* Second)
    {
        if (Flag.Offset != -2 || !Weapon)
            return;

        Flag.Offset = -1;

        for (const char* Name : { First, Second })
        {
            if (!Name)
                continue;

            auto Prop = Weapon->GetProperty(Name, 0x20000);

            if (Prop)
            {
                Flag.Offset = (int32)GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
                Flag.Mask = Prop->GetFieldMask();

                LogLine("[RELOAD] The gun's \"%s\" flag is at +0x%X (mask 0x%02X).", Name, (unsigned)Flag.Offset, (unsigned)Flag.Mask);
                break;
            }
        }

        if (Flag.Offset < 0 || Flag.Mask == 0)
        {
            Flag.Offset = -1;
            LogLine("[RELOAD] The gun has no \"%s\" flag on this build.", First);
        }
    }

    static int ReadGunFlag(AActor* Weapon, const FGunFlag& Flag)
    {
        if (!Weapon || Flag.Offset < 0)
            return -1;

        return (GetFromOffset<uint8>(Weapon, (uint32)Flag.Offset) & Flag.Mask) ? 1 : 0;
    }

    static void WriteGunFlag(AActor* Weapon, const FGunFlag& Flag, bool bOn)
    {
        if (!Weapon || Flag.Offset < 0)
            return;

        uint8& Byte = GetFromOffset<uint8>(Weapon, (uint32)Flag.Offset);
        Byte = bOn ? (uint8)(Byte | Flag.Mask) : (uint8)(Byte & ~Flag.Mask);
    }

    static int GunIsReloading(AActor* Weapon)
    {
        FindGunFlag(Weapon, GGunReloadingFlag, "bIsReloadingWeapon", "bIsReloading");
        return ReadGunFlag(Weapon, GGunReloadingFlag);
    }

    static int GunIsEquipping(AActor* Weapon)
    {
        FindGunFlag(Weapon, GGunEquippingFlag, "bIsEquippingWeapon", "bIsEquipping");
        return ReadGunFlag(Weapon, GGunEquippingFlag);
    }

    static int GunMagazine(AActor* Weapon, void* PlayerStateKey)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false;

        if (!bLooked && Weapon)
        {
            bLooked = true;
            Fn = Weapon->GetFunction("GetBulletsPerClip");
        }

        int Magazine = 0;

        if (Fn && Weapon)
            Magazine = Weapon->Call<int32>(Fn);

        if (Magazine <= 0 || Magazine > 1000)
            Magazine = BotMagazineSize(PlayerStateKey);

        return Magazine;
    }

    static float GunReloadTime(AActor* Weapon)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false;

        if (!bLooked && Weapon)
        {
            bLooked = true;
            Fn = Weapon->GetFunction("GetReloadTime");
        }

        float Seconds = 0.f;

        if (Fn && Weapon)
            Seconds = Weapon->Call<float>(Fn);

        if (!(Seconds >= 0.3f && Seconds <= 8.f) && Weapon && ((AFortWeapon*)Weapon)->HasWeaponData() && ((AFortWeapon*)Weapon)->WeaponData)
            Seconds = BotGunNumbers((const UFortItemDefinition*)((AFortWeapon*)Weapon)->WeaponData).ReloadTime;

        if (!(Seconds >= 0.3f && Seconds <= 8.f))
            Seconds = 2.0f;

        return Seconds;
    }

    static float GunReloadAllowance(AActor* Weapon, int Rounds, int Magazine)
    {
        const float Each = GunReloadTime(Weapon);
        const int Missing = Magazine - Rounds;

        auto Def = (Weapon && ((AFortWeapon*)Weapon)->HasWeaponData()) ? (const UFortItemDefinition*)((AFortWeapon*)Weapon)->WeaponData : nullptr;

        if (Def && Missing > 1 && BotGunNumbers(Def).bOneRoundAtATime)
            return Each * (float)Missing;

        return Each;
    }

    static bool ActorIsLive(const UObject* Object)
    {
        if (!Object || Object->Index < 0 || Object->Index >= TUObjectArray::Num())
            return false;

        auto Item = TUObjectArray::GetItemByIndex(Object->Index);
        return Item && Item->Object == Object && !(Item->Flags & (0x20000000 | 0x10000000));
    }

    static bool BotIsReloading(AFortPlayerPawnAthena* Pawn)
    {
        auto It = BotReloads.find((void*)Pawn);
        return It != BotReloads.end() && It->second.bActive;
    }

    static void LetGoOfTheTrigger(AFortPlayerPawnAthena* Pawn)
    {
        auto FireIt = BotFire.find((void*)Pawn);

        if (FireIt != BotFire.end() && FireIt->second.bFiring)
        {
            BotGunTrigger(Pawn, false);

            FireIt->second.bFiring = false;
            FireIt->second.BurstTicksLeft = 0;
        }
    }

    static void FinishBotReloadNow(FBotReload& Reload, AFortPlayerStateAthena* PlayerState, AActor* Weapon, float Now)
    {
        int32* Ammo = WeaponAmmoCount(Weapon);

        if (Ammo && *Ammo < Reload.Magazine)
            *Ammo = Reload.Magazine;

        WriteGunFlag(Weapon, GGunReloadingFlag, false);
        TopUpBotMagazine((void*)PlayerState);

        GMagazinesRefilled++;
        GReloadsRefilledByBotCode++;

        if (++GReloadLines <= 20 || (GReloadLines % 50) == 0)
            LogLine("[RELOAD] '%s' finished reloading its %s after %.1fs (%s) -- %d -> %d/%d rounds.", SafeNameOf(PlayerState).c_str(),
                    BotObjectName((const UObject*)((AFortWeapon*)Weapon)->WeaponData).c_str(), Now - Reload.StartedAt, Reload.Why,
                    Reload.RoundsBefore, Ammo ? *Ammo : -1, Reload.Magazine);

        Reload.bActive = false;
        Reload.Weapon = nullptr;
    }

    static void TickBotReload(AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, bool bFighting)
    {
        if (!Pawn || !PlayerState)
            return;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        FBotReload& Reload = BotReloads[(void*)Pawn];

        AActor* Weapon = (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon) ? (AActor*)Pawn->CurrentWeapon : nullptr;

        if (bFighting)
            Reload.NoFightSince = -1.f;
        else if (Reload.NoFightSince < 0.f)
            Reload.NoFightSince = Now;

        if (Reload.bActive && (void*)Weapon != Reload.Weapon)
        {
            GReloadsCancelled++;

            if (Reload.Weapon && WeakStillThere(Reload.WeaponWeak, Reload.Weapon))
                WriteGunFlag((AActor*)Reload.Weapon, GGunReloadingFlag, false);

            if (++GReloadLines <= 20)
                LogLine("[RELOAD] '%s' stopped reloading -- %s.", SafeNameOf(PlayerState).c_str(),
                        Weapon ? "it is holding a different gun now" : "it has nothing in its hands now");

            Reload.bActive = false;
            Reload.Weapon = nullptr;
        }

        if (!Weapon)
            return;

        int32* Ammo = WeaponAmmoCount(Weapon);
        if (!Ammo)
            return;

        if (auto Gun = (AFortWeapon*)Weapon)
            if (Gun->HasWeaponData() && Gun->WeaponData && ((const UObject*)Gun->WeaponData)->IsA<UFortWeaponMeleeItemDefinition>())
                return;

        const int Rounds = *Ammo;
        const int ReloadingFlag = GunIsReloading(Weapon);
        const int EquippingFlag = GunIsEquipping(Weapon);

        if (EquippingFlag == 1)
        {
            if (Reload.EquipFlagSince < 0.f)
                Reload.EquipFlagSince = Now;
            else if (Now - Reload.EquipFlagSince > 3.f)
            {
                WriteGunFlag(Weapon, GGunEquippingFlag, false);
                GStuckEquipFlags++;

                if (GStuckEquipFlags <= 10 || (GStuckEquipFlags % 25) == 0)
                    LogLine("[RELOAD] '%s's gun was still \"equipping\" %.1fs after it was taken out -- the flag is cleared so it can fire "
                            "(%d time(s) so far).", SafeNameOf(PlayerState).c_str(), Now - Reload.EquipFlagSince, GStuckEquipFlags);

                Reload.EquipFlagSince = -1.f;
            }
        }
        else
        {
            Reload.EquipFlagSince = -1.f;
        }

        if (!Reload.bActive && ReloadingFlag == 1)
        {
            const int Magazine = (Reload.MagazineOf == (void*)Weapon && Reload.MagazineSize > 0) ? Reload.MagazineSize
                                                                                                : GunMagazine(Weapon, (void*)PlayerState);

            if (Reload.ReloadFlagSince < 0.f)
                Reload.ReloadFlagSince = Now;
            else if (Now - Reload.ReloadFlagSince > GunReloadAllowance(Weapon, Rounds, Magazine) + 1.5f)
            {
                WriteGunFlag(Weapon, GGunReloadingFlag, false);

                if (*Ammo < Magazine)
                    *Ammo = Magazine;

                GStuckReloadFlags++;

                if (GStuckReloadFlags <= 10 || (GStuckReloadFlags % 25) == 0)
                    LogLine("[RELOAD] '%s's gun had the game's \"reloading\" flag stuck on for %.1fs with no reload going -- cleared, and "
                            "the magazine filled to %d round(s) (%d time(s) so far).",
                            SafeNameOf(PlayerState).c_str(), Now - Reload.ReloadFlagSince, Magazine, GStuckReloadFlags);

                Reload.ReloadFlagSince = -1.f;
                return;
            }
        }
        else
        {
            Reload.ReloadFlagSince = -1.f;
        }

        if (Reload.bActive)
        {
            if (Now - Reload.StartedAt >= Reload.Duration)
                FinishBotReloadNow(Reload, PlayerState, Weapon, Now);

            return;
        }

        if (Reload.MagazineOf != (void*)Weapon)
        {
            Reload.MagazineOf = (void*)Weapon;
            Reload.MagazineSize = GunMagazine(Weapon, (void*)PlayerState);
        }

        const int Magazine = Reload.MagazineSize;
        const char* Why = nullptr;

        if (Rounds <= 0)
            Why = bFighting ? "its magazine ran dry mid-fight" : "its magazine was empty";
        else if (!bFighting && Rounds < Magazine && Reload.NoFightSince >= 0.f && Now - Reload.NoFightSince >= 1.5f)
            Why = "topping off after the fight";

        if (!Why)
            return;

        if (ReloadingFlag == 1)
            return;

        LetGoOfTheTrigger(Pawn);

        Reload.bActive = true;
        Reload.Weapon = (void*)Weapon;
        Reload.WeaponWeak = FWeakObjectPtr((const UObject*)Weapon);
        Reload.RoundsBefore = Rounds;
        Reload.Magazine = Magazine;
        Reload.StartedAt = Now;
        Reload.Duration = GunReloadAllowance(Weapon, Rounds, Magazine);
        Reload.Why = Why;

        WriteGunFlag(Weapon, GGunReloadingFlag, true);

        GReloadsStarted++;

        if (Rounds > 0)
            GReloadsTopOff++;

        if (++GReloadLines <= 20 || (GReloadLines % 50) == 0)
            LogLine("[RELOAD] '%s' is reloading its %s (%s, %d/%d rounds) -- %.1fs reload.", SafeNameOf(PlayerState).c_str(),
                    BotObjectName((const UObject*)((AFortWeapon*)Weapon)->WeaponData).c_str(), Why, Rounds, Magazine, Reload.Duration);
    }

    static void LogReloadSummary()
    {
        if (GReloadsStarted == 0 && GStuckReloadFlags == 0 && GStuckEquipFlags == 0)
            return;

        LogLine("[RELOAD] Reloads started %d (topping off after a fight %d), every one by the bot code: the gun's own reload time, then the "
                "magazine filled | finished %d | stopped by a gun change %d | stuck \"reloading\" flags cleared %d, stuck \"equipping\" "
                "flags cleared %d.",
                GReloadsStarted, GReloadsTopOff, GReloadsRefilledByBotCode, GReloadsCancelled, GStuckReloadFlags, GStuckEquipFlags);
    }

    static bool GFocusFaulted = false;
    static bool GAimReadFaulted = false;

    static void BotFocusRaw(::AAIController* AIController, int Mode, AActor* Focus, const FVector* Point)
    {
        if (Mode == 1)
        {
            AActor* NewFocus = Focus;
            AIController->K2_SetFocus(NewFocus);
        }
        else if (Mode == 2)
        {
            FVector FocalPoint = *Point;
            AIController->K2_SetFocalPoint(FocalPoint);
        }
        else
        {
            AIController->K2_ClearFocus();
        }
    }

    static void BotFocusGuarded(::AAIController* AIController, int Mode, AActor* Focus, const FVector* Point)
    {
        __try
        {
            BotFocusRaw(AIController, Mode, Focus, Point);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GFocusFaulted = true;
        }
    }

    static bool BotReadAimRaw(::AAIController* AIController, double* OutYaw)
    {
        const FRotator Actual = AIController->GetControlRotation();
        *OutYaw = (double)Actual.Yaw;
        return true;
    }

    static bool BotReadAimGuarded(::AAIController* AIController, double* OutYaw)
    {
        __try
        {
            return BotReadAimRaw(AIController, OutYaw);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GAimReadFaulted = true;
            return false;
        }
    }

    static bool GStopFaulted = false;

    static void BotStopWalkingRaw(::AAIController* AIController)
    {

        if (IsPlayerControllerBot((AActor*)AIController))
        {
            auto Pawn = ((AFortPlayerControllerAthena*)AIController)->Pawn;
            auto It = Pawn ? BotBodies.find((void*)Pawn) : BotBodies.end();

            if (It != BotBodies.end())
                It->second.bHasGoal = false;

            return;
        }

        AIController->StopMovement();
    }

    static uint8 BotMoveTo(::AAIController* AIController, AFortPlayerPawnAthena* Pawn, FVector Goal, float AcceptRadius)
    {
        if (IsPlayerControllerBot((AActor*)AIController))
        {
            if (!Pawn)
                return 0;

            auto& Body = BotBodies[(void*)Pawn];
            Body.bHasGoal = true;
            Body.Goal = Goal;
            Body.AcceptRadius = AcceptRadius;
            return 2;
        }

        return AIController->MoveToLocation(Goal, AcceptRadius, true, false, false, false, (UClass*)nullptr, true);
    }

    static void BotStopWalkingGuarded(::AAIController* AIController)
    {
        __try
        {
            BotStopWalkingRaw(AIController);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GStopFaulted = true;
        }
    }

    static void BotAimAt(::AAIController* AIController, AFortPlayerPawnAthena* OnTarget, const FVector& From,
                         double AimYaw, double AimPitch, double Distance)
    {
        if (!AIController || GFocusFaulted)
            return;

        static int Available = -1;

        if (Available == -1)
        {
            Available = (AIController->GetFunction("K2_SetFocus") && AIController->GetFunction("K2_SetFocalPoint") &&
                         AIController->GetFunction("K2_ClearFocus")) ? 1 : 0;

            LogLine("[AIM] The engine's aim focus %s.",
                    Available ? "is available -- a bot's whole burst goes where the bot code aims it"
                              : "is NOT on this build -- only the first bullet of a burst follows the bot code's aim");
        }

        if (Available != 1)
            return;

        if (OnTarget)
        {
            BotFocusGuarded(AIController, 1, (AActor*)OnTarget, nullptr);
        }
        else
        {
            const double R = 3.14159265358979323846 / 180.0;

            FVector Point{};
            Point.X = From.X + cos(AimPitch * R) * cos(AimYaw * R) * Distance;
            Point.Y = From.Y + cos(AimPitch * R) * sin(AimYaw * R) * Distance;
            Point.Z = From.Z + sin(AimPitch * R) * Distance;

            BotFocusGuarded(AIController, 2, nullptr, &Point);
        }

        if (GFocusFaulted)
        {
            LogLine("[AIM] The engine's aim focus faulted on this build -- switched off; only the first bullet of a burst "
                    "follows the bot code's aim from here on.");
            return;
        }

        BotHasFocus.insert((void*)AIController);
    }

    static void BotStopAiming(::AAIController* AIController)
    {
        if (!AIController)
            return;

        auto It = BotHasFocus.find((void*)AIController);
        if (It == BotHasFocus.end())
            return;

        BotHasFocus.erase(It);

        if (!GFocusFaulted)
            BotFocusGuarded(AIController, 0, nullptr, nullptr);
    }

    class BKismetSystemLibrary : public UObject
    {
    public:
        UCLASS_COMMON_MEMBERS(BKismetSystemLibrary);

        DEFINE_STATIC_FUNC(LineTraceSingle, bool);
    };

    struct FBotHit
    {
        bool bHit = false;
        AActor* Actor = nullptr;
    };

    static bool GTraceFaulted = false;

    static bool BotTraceAvailable()
    {
        static int Available = -1;

        if (Available == -1)
        {
            auto Library = BKismetSystemLibrary::GetDefaultObj();
            Available = (Library && Library->GetFunction("LineTraceSingle")) ? 1 : 0;

            LogLine("[EYES] The engine's line trace %s.",
                    Available ? "is available -- bots also check the ground is not in the way before they shoot"
                              : "is NOT on this build -- only the game's line-of-sight check decides what a bot can see");
        }

        return Available == 1 && !GTraceFaulted;
    }

    static bool BotTraceRaw(AActor* Self, const FVector& Start, const FVector& End, uint8 Channel, uint8* HitBuffer,
                            AActor* AlsoIgnore)
    {
        struct FColour
        {
            float R, G, B, A;
        };

        UObject* Context = (UObject*)Self;
        TArray<AActor*> Ignore{};

        AActor* IgnoreList[1] = { AlsoIgnore };

        if (AlsoIgnore)
        {
            Ignore.Data = IgnoreList;
            Ignore.NumElements = 1;
            Ignore.MaxElements = 1;
        }

        bool bComplex = false;
        uint8 DrawDebug = 0;
        void* HitOut = (void*)HitBuffer;
        bool bIgnoreSelf = true;
        FColour TraceColour{ 1.f, 0.f, 0.f, 1.f };
        FColour HitColour{ 0.f, 1.f, 0.f, 1.f };
        float DrawTime = 0.f;

        return BKismetSystemLibrary::LineTraceSingle(Context, Start, End, Channel, bComplex, Ignore, DrawDebug,
                                                     HitOut, bIgnoreSelf, TraceColour, HitColour, DrawTime);
    }

    static bool BotTraceGuarded(AActor* Self, const FVector* Start, const FVector* End, uint8 Channel, uint8* HitBuffer,
                                AActor* AlsoIgnore = nullptr)
    {
        __try
        {
            return BotTraceRaw(Self, *Start, *End, Channel, HitBuffer, AlsoIgnore);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GTraceFaulted = true;
            return false;
        }
    }

    static FBotHit BotTrace(AActor* Self, const FVector& Start, const FVector& End, uint8 Channel, AActor* AlsoIgnore = nullptr)
    {
        FBotHit Out{};

        if (!Self || !BotTraceAvailable())
            return Out;

        static int32 ActorOffset = -2;

        if (ActorOffset == -2)
        {
            auto HitStruct = FindStruct("HitResult");
            ActorOffset = HitStruct ? (int32)HitStruct->GetOffset("Actor") : -1;

            LogLine("[EYES] Line trace hit results: what was hit %s.", ActorOffset >= 0 ? "is readable" : "is NOT readable");
        }

        alignas(16) uint8 HitBuffer[0x400];
        memset(HitBuffer, 0, sizeof(HitBuffer));

        Out.bHit = BotTraceGuarded(Self, &Start, &End, Channel, HitBuffer, AlsoIgnore);

        if (GTraceFaulted)
        {
            LogLine("[EYES] The engine's line trace faulted on this build -- switched off; the game's line-of-sight check "
                    "alone decides what a bot can see from here on.");
            return FBotHit{};
        }

        if (Out.bHit && ActorOffset >= 0 && ActorOffset < 0x3F0)
            Out.Actor = (AActor*)((FWeakObjectPtr*)(HitBuffer + ActorOffset))->Get();

        return Out;
    }

    static std::string BotObjectName(const UObject* Object)
    {
        if (!Object)
            return std::string("nothing");

        auto Name = Object->Name.ToString();
        return std::string(Name.c_str());
    }

    static std::string BotClassName(const UObject* Object)
    {
        if (!Object || !Object->Class)
            return std::string("?");

        auto Name = Object->Class->Name.ToString();
        return std::string(Name.c_str());
    }

    static bool BotStraightLineSees(AFortPlayerPawnAthena* Me, AActor* Them, std::string* WhatIsInTheWay)
    {
        if (!Me || !Them)
            return false;

        if (!BotTraceAvailable())
            return true;

        const FVector MyLoc = Me->K2_GetActorLocation();
        const FVector ThemLoc = Them->K2_GetActorLocation();

        FVector Eyes(MyLoc.X, MyLoc.Y, MyLoc.Z + 60.0);

        auto MyGun = (Me->HasCurrentWeapon() && Me->CurrentWeapon) ? (AActor*)Me->CurrentWeapon : nullptr;

        const double Heights[2] = { 30.0, 70.0 };
        std::string FirstInTheWay;

        for (int h = 0; h < 2; h++)
        {
            FVector Point(ThemLoc.X, ThemLoc.Y, ThemLoc.Z + Heights[h]);

            const FBotHit Hit = BotTrace((AActor*)Me, Eyes, Point, 0, MyGun);

            if (GTraceFaulted)
                return true;

            if (!Hit.bHit)
                return true;

            if (Hit.Actor)
            {
                if (Hit.Actor == Them)
                    return true;

                if ((AActor*)Hit.Actor->Owner == Them)
                    return true;
            }

            if (FirstInTheWay.empty())
                FirstInTheWay = Hit.Actor ? (BotObjectName(Hit.Actor) + " (" + BotClassName(Hit.Actor) + ")")
                                          : std::string("something that is not an actor");
        }

        if (WhatIsInTheWay)
            *WhatIsInTheWay = FirstInTheWay;

        return false;
    }

    static bool BotGroundInTheWay(AFortPlayerPawnAthena* Me, const FVector& MyLoc, AFortPlayerPawnAthena* Them,
                                  const FVector& ThemLoc, std::string* WhatWasHit = nullptr)
    {
        if (!Me || !Them || !BotTraceAvailable())
            return false;

        FVector Eyes = MyLoc;
        Eyes.Z = MyLoc.Z + 60.0;

        FVector Chest = ThemLoc;
        Chest.Z = ThemLoc.Z + 30.0;

        const uint8 Channels[2] = { 1, 0 };

        for (int c = 0; c < 2; c++)
        {
            const FBotHit Hit = BotTrace((AActor*)Me, Eyes, Chest, Channels[c]);

            if (!Hit.bHit || !Hit.Actor || Hit.Actor == (AActor*)Them)
                continue;

            const std::string ClassName = BotClassName(Hit.Actor);

            if (ClassName.find("Landscape") != std::string::npos)
            {
                if (WhatWasHit)
                    *WhatWasHit = BotObjectName(Hit.Actor);

                return true;
            }
        }

        return false;
    }

    struct FBotGun
    {
        bool bBurst = false;
        float NextRoundAt = 0.f;
        void* Target = nullptr;
        FWeakObjectPtr TargetWeak{};
        std::string TargetName;
        float TargetLifeAtStart = -1.f;
        float Distance = 0.f;
        void* Weapon = nullptr;
        std::string GunName;
        int RoundsAtStart = -1;
        int Rounds = 0;
        int Hits = 0;
        float Damage = 0.f;
    };

    static std::map<void*, FBotGun> BotGuns;

    static int GGunBursts = 0;
    static int GGunBurstsEnded = 0;
    static int GGunRounds = 0;
    static int GGunMisses = 0;
    static int GGunNoDamage = 0;
    static int GGunHeldEquipping = 0;
    static int GGunHeldEmpty = 0;
    static int GGunHeldNoGun = 0;
    static int GGunHeldMelee = 0;
    static int GGunHeldDead = 0;
    static int GGunStandInRounds = 0;

    static UFunction* GShotFxFn = nullptr;
    static bool GShotFxLooked = false;
    static bool GShotFxBroken = false;
    static uint32 GShotFxSharedAt = 0;
    static uint32 GShotFxNonSharedAt = 0;
    static int32 GFxLocation = -1, GFxNormal = -1, GFxMagnitude = -1, GFxActivate = -1, GFxFatal = -1, GFxShield = -1, GFxShieldGone = -1,
                 GFxValid = -1, GFxHitActor = -1;
    static int GShotFxSent = 0;
    static int GShotFxSentHits = 0;

    static bool ShotFxRaw(UObject* Pawn, UFunction* Fn, uint8* Params)
    {
        Pawn->ProcessEvent(Fn, Params);
        return true;
    }

    static bool ShotFxGuarded(UObject* Pawn, UFunction* Fn, uint8* Params)
    {
        __try
        {
            return ShotFxRaw(Pawn, Fn, Params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static int32 ShotFxField(const UStruct* Struct, const char* Name, uint32 Width, uint32 Base, uint32 Size)
    {
        if (!Struct)
            return -1;

        const uint32 Off = Struct->GetOffset(Name);

        if (Off == (uint32)-1 || Base + Off + Width > Size)
            return -1;

        return (int32)Off;
    }

    static void ResolveShotFx(AFortPlayerPawnAthena* Pawn)
    {
        if (GShotFxLooked || !Pawn)
            return;

        GShotFxLooked = true;

        auto Fn = ((UObject*)Pawn)->GetFunction("NetMulticast_Athena_BatchedDamageCues");
        uint32 Size = 0;
        int Found = 0;

        if (Fn)
        {
            auto Params = Fn->GetParamsNamed();
            Size = Params.Size;

            for (auto& P : Params.NameOffsetMap)
            {
                if (P.Name == "SharedData")
                {
                    GShotFxSharedAt = P.Offset;
                    Found |= 1;
                }
                else if (P.Name == "NonSharedData")
                {
                    GShotFxNonSharedAt = P.Offset;
                    Found |= 2;
                }
            }
        }

        const bool bParams = Fn && Found == 3 && Size > 0 && Size <= 0x100;

        if (bParams)
        {
            auto Shared = FindStruct("AthenaBatchedDamageGameplayCues_Shared");
            auto NonShared = FindStruct("AthenaBatchedDamageGameplayCues_NonShared");
            const uint32 Vector = (uint32)FVector::Size();

            GFxLocation = ShotFxField(Shared, "Location", Vector, GShotFxSharedAt, Size);
            GFxNormal = ShotFxField(Shared, "Normal", Vector, GShotFxSharedAt, Size);
            GFxMagnitude = ShotFxField(Shared, "Magnitude", 4, GShotFxSharedAt, Size);
            GFxActivate = ShotFxField(Shared, "bWeaponActivate", 1, GShotFxSharedAt, Size);
            GFxFatal = ShotFxField(Shared, "bIsFatal", 1, GShotFxSharedAt, Size);
            GFxShield = ShotFxField(Shared, "bIsShield", 1, GShotFxSharedAt, Size);
            GFxShieldGone = ShotFxField(Shared, "bIsShieldDestroyed", 1, GShotFxSharedAt, Size);
            GFxValid = ShotFxField(Shared, "bIsValid", 1, GShotFxSharedAt, Size);
            GFxHitActor = ShotFxField(NonShared, "HitActor", 8, GShotFxNonSharedAt, Size);
        }

        if (bParams && GFxLocation >= 0 && GFxActivate >= 0 && GFxHitActor >= 0)
            GShotFxFn = Fn;

        LogLine("[FIRE] Shot effects for bot rounds (the game's own batched hit cue on the shooter, NetMulticast_Athena_BatchedDamageCues): %s "
                "(the cue %s, %u byte(s) of arguments | location at %d, gun-fired flag at %d, hit actor at %d, damage at %d, shield flag at %d).",
                GShotFxFn ? "ready -- every bot round sends one to the screens near the shooter (the 9.10 client DLL reports what arrives)"
                          : "NOT usable on this build -- bot rounds still land, but nobody sees or hears the shot",
                Fn ? "is there" : "is missing", (unsigned)Size, GFxLocation, GFxActivate, GFxHitActor, GFxMagnitude, GFxShield);
    }

    static void SendBotShotFx(AFortPlayerPawnAthena* Shooter, AActor* HitActor, const FVector& At, const FVector& Normal, float Magnitude,
                              bool bFatal, bool bShield, bool bShieldGone)
    {
        if (!FConfiguration::bBotShotEffects || GShotFxBroken || !Shooter)
            return;

        ResolveShotFx(Shooter);

        if (!GShotFxFn)
            return;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));

        uint8* Shared = Params + GShotFxSharedAt;

        memcpy(Shared + GFxLocation, (const void*)&At, (size_t)FVector::Size());

        if (GFxNormal >= 0)
            memcpy(Shared + GFxNormal, (const void*)&Normal, (size_t)FVector::Size());
        if (GFxMagnitude >= 0)
            *(float*)(Shared + GFxMagnitude) = Magnitude;

        Shared[GFxActivate] = 1;

        if (GFxFatal >= 0)
            Shared[GFxFatal] = bFatal ? 1 : 0;
        if (GFxShield >= 0)
            Shared[GFxShield] = bShield ? 1 : 0;
        if (GFxShieldGone >= 0)
            Shared[GFxShieldGone] = bShieldGone ? 1 : 0;
        if (GFxValid >= 0)
            Shared[GFxValid] = 1;

        *(void**)(Params + GShotFxNonSharedAt + GFxHitActor) = (void*)HitActor;

        if (!ShotFxGuarded((UObject*)Shooter, GShotFxFn, Params))
        {
            GShotFxBroken = true;
            LogLine("[FIRE] Sending a bot's shot effects faulted on the server -- switched off for the rest of the match (bot rounds still land).");
            return;
        }

        GShotFxSent++;

        if (HitActor)
            GShotFxSentHits++;
    }

    static FVector BotShotEndPoint(AFortPlayerPawnAthena* Shooter, const FVector& Eyes, const FVector& Far)
    {
        static int32 PointOffset = -2;

        if (PointOffset == -2)
        {
            auto HitStruct = FindStruct("HitResult");
            PointOffset = HitStruct ? (int32)HitStruct->GetOffset("ImpactPoint") : -1;
        }

        if (PointOffset < 0 || PointOffset > 0x3C0 || !BotTraceAvailable())
            return Far;

        alignas(16) uint8 HitBuffer[0x400];
        memset(HitBuffer, 0, sizeof(HitBuffer));

        auto Gun = (Shooter->HasCurrentWeapon() && Shooter->CurrentWeapon) ? (AActor*)Shooter->CurrentWeapon : nullptr;

        const bool bHit = BotTraceGuarded((AActor*)Shooter, &Eyes, &Far, 0, HitBuffer, Gun);

        if (GTraceFaulted)
        {
            LogLine("[EYES] The engine's line trace faulted on this build (finding where a bot's missed round landed) -- switched off; the game's "
                    "line-of-sight check alone decides what a bot can see from here on.");
            return Far;
        }

        if (!bHit)
            return Far;

        FVector Point{};
        memcpy((void*)&Point, HitBuffer + PointOffset, (size_t)FVector::Size());
        return Point;
    }

    static FVector BotShotNormal(const FVector& From, const FVector& At)
    {
        FVector Back(From.X - At.X, From.Y - At.Y, From.Z - At.Z);
        const double Length = Back.Magnitude();

        if (Length > 1.0)
            return Back / Length;

        return FVector(0.0, 0.0, 1.0);
    }

    static void BotFireRound(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState,
                             FBotGun& Gun, const UFortItemDefinition* Def, const FBotGunNumbers& Numbers, const std::set<void*>& LivePawns)
    {
        Gun.Rounds++;
        GGunRounds++;

        const FVector From = Pawn->K2_GetActorLocation();
        const FVector Eyes(From.X, From.Y, From.Z + 60.0);

        auto AimIt = BotShotAim.find((void*)Pawn);
        const bool bAim = AimIt != BotShotAim.end();
        auto Target = bAim ? (AFortPlayerPawnAthena*)AimIt->second.Target : nullptr;

        const bool bTargetLive = Target && LivePawns.find((void*)Target) != LivePawns.end() && Target->GetHealth() > 0.f;

        if (bTargetLive && AimIt->second.bCanShoot && AimIt->second.bOnTarget)
        {
            const int Kind = PawnTargetKind(Target);
            const float Damage = Numbers.bValid ? BotRoundDamage(Numbers, AimIt->second.Distance) : kBotFallbackDamage;

            bool bShield = false;
            bool bShieldGone = false;
            bool bFatal = false;
            bool bApplied = false;

            if (Kind >= 0 && Damage > 0.f)
                bFatal = BotHitTarget(Pawn, Controller, PlayerState, Target, Damage, Kind, Def, &bShield, &bShieldGone, &bApplied);

            if (bApplied)
            {
                Gun.Hits++;
                Gun.Damage += Damage;

                if (Kind == 0)
                {
                    GCombatBotOnBotHits++;
                    GHitsOnLockedTarget++;
                }
            }
            else
            {
                GGunNoDamage++;
            }

            const FVector TargetLoc = Target->K2_GetActorLocation();
            const FVector At(TargetLoc.X, TargetLoc.Y, TargetLoc.Z + 30.0);

            SendBotShotFx(Pawn, (AActor*)Target, At, BotShotNormal(Eyes, At), bApplied ? Damage : 0.f, bApplied && bFatal, bApplied && bShield,
                          bApplied && bShieldGone);
            return;
        }

        GGunMisses++;

        if (!FConfiguration::bBotShotEffects || GShotFxBroken)
            return;

        ResolveShotFx(Pawn);

        if (!GShotFxFn)
            return;

        double Yaw = 0.0, Pitch = 0.0;
        auto Intended = BotIntendedAim.find((void*)Pawn);

        if (Intended != BotIntendedAim.end())
        {
            Yaw = Intended->second.Yaw;
            Pitch = Intended->second.Pitch;
        }
        else if (Target)
        {
            const FVector TargetLoc = Target->K2_GetActorLocation();
            Yaw = atan2(TargetLoc.Y - From.Y, TargetLoc.X - From.X) * (180.0 / 3.14159265358979323846);
        }

        const double Range = ((bAim && AimIt->second.Distance > 0.f) ? (double)AimIt->second.Distance : 3000.0) + 2500.0;
        const double YawRad = Yaw * (3.14159265358979323846 / 180.0);
        const double PitchRad = Pitch * (3.14159265358979323846 / 180.0);

        const FVector Far(Eyes.X + cos(PitchRad) * cos(YawRad) * Range, Eyes.Y + cos(PitchRad) * sin(YawRad) * Range,
                          Eyes.Z + sin(PitchRad) * Range);
        const FVector At = BotShotEndPoint(Pawn, Eyes, Far);

        SendBotShotFx(Pawn, nullptr, At, BotShotNormal(Eyes, At), 0.f, false, false, false);
    }

    static void TickBotGun(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState,
                           const std::set<void*>& LivePawns)
    {
        auto It = BotGuns.find((void*)Pawn);
        if (It == BotGuns.end() || !It->second.bBurst)
            return;

        FBotGun& Gun = It->second;

        if (Pawn->GetHealth() <= 0.f)
        {
            GGunHeldDead++;
            return;
        }

        auto Weapon = (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon) ? (AActor*)Pawn->CurrentWeapon : nullptr;
        auto Def = (Weapon && ((AFortWeapon*)Weapon)->HasWeaponData()) ? (const UFortItemDefinition*)((AFortWeapon*)Weapon)->WeaponData : nullptr;

        if (!Weapon || !Def)
        {
            GGunHeldNoGun++;
            return;
        }

        if (((const UObject*)Def)->IsA<UFortWeaponMeleeItemDefinition>())
        {
            GGunHeldMelee++;
            return;
        }

        if (GunIsEquipping(Weapon) == 1)
        {
            GGunHeldEquipping++;
            return;
        }

        int32* Ammo = WeaponAmmoCount(Weapon);

        if (Ammo && *Ammo <= 0)
        {
            GGunHeldEmpty++;
            return;
        }

        const FBotGunNumbers& Numbers = BotGunNumbers(Def);
        float Rate = Numbers.FireRate;
        bool bStandIn = !Numbers.bValid;

        if (!(Rate >= 0.1f && Rate <= 30.f))
        {
            Rate = kBotFallbackRate;
            bStandIn = true;
        }

        const float Between = 1.f / Rate;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        if (Now + 0.001f < Gun.NextRoundAt)
            return;

        if (Ammo)
            (*Ammo)--;

        if (bStandIn)
            GGunStandInRounds++;

        BotFireRound(Pawn, Controller, PlayerState, Gun, Def, Numbers, LivePawns);

        Gun.NextRoundAt = (Now - Gun.NextRoundAt > Between) ? Now + Between : Gun.NextRoundAt + Between;
    }

    static void BotGunTrigger(AFortPlayerPawnAthena* Pawn, bool bPull, AFortPlayerPawnAthena* Target, float DistanceMetres)
    {
        if (!Pawn)
            return;

        FBotGun& Gun = BotGuns[(void*)Pawn];

        auto Weapon = (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon) ? (AActor*)Pawn->CurrentWeapon : nullptr;

        if (bPull)
        {
            auto World = UWorld::GetWorld();
            const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

            int32* Ammo = WeaponAmmoCount(Weapon);

            Gun.bBurst = true;
            Gun.Target = (void*)Target;
            Gun.TargetWeak = FWeakObjectPtr((const UObject*)Target);
            Gun.TargetName = Target ? BotPawnName(Target) : std::string("nobody");
            Gun.TargetLifeAtStart = PawnLife(Target);
            Gun.Distance = DistanceMetres;
            Gun.Weapon = (void*)Weapon;
            Gun.GunName = (Weapon && ((AFortWeapon*)Weapon)->HasWeaponData())
                              ? BotObjectName((const UObject*)((AFortWeapon*)Weapon)->WeaponData)
                              : std::string("nothing");
            Gun.RoundsAtStart = Ammo ? *Ammo : -1;
            Gun.Rounds = 0;
            Gun.Hits = 0;
            Gun.Damage = 0.f;

            if (Gun.NextRoundAt < Now)
                Gun.NextRoundAt = Now;

            GGunBursts++;
            return;
        }

        if (!Gun.bBurst)
            return;

        Gun.bBurst = false;
        GGunBurstsEnded++;

        if (GGunBurstsEnded > 8 && (GGunBurstsEnded % 50) != 0)
            return;

        int32* Ammo = ((void*)Weapon == Gun.Weapon) ? WeaponAmmoCount(Weapon) : nullptr;
        auto Shot = (AFortPlayerPawnAthena*)Gun.Target;

        char Life[64];

        if (Shot && WeakStillThere(Gun.TargetWeak, Shot) && Shot->GetHealth() > 0.f)
            snprintf(Life, sizeof(Life), "%.0f -> %.0f", Gun.TargetLifeAtStart, PawnLife(Shot));
        else
            snprintf(Life, sizeof(Life), "%.0f -> down (or gone)", Gun.TargetLifeAtStart);

        LogLine("[FIRE] Burst #%d: '%s' at '%s' (%.0f m) with its %s -- the bot code fired %d round(s), %d hit for %.0f damage | rounds in the "
                "gun %d -> %d | the target's health + shield: %s.",
                GGunBurstsEnded, BotPawnName(Pawn).c_str(), Gun.TargetName.c_str(), Gun.Distance, Gun.GunName.c_str(), Gun.Rounds, Gun.Hits,
                Gun.Damage, Gun.RoundsAtStart, Ammo ? *Ammo : -1, Life);
    }

    static void LogBotGunSummary()
    {
        const char* Effects = !FConfiguration::bBotShotEffects ? " -- switched off in the configuration"
                            : (GShotFxBroken ? " -- switched off after a fault"
                                             : ((GShotFxLooked && !GShotFxFn) ? " -- the cue is not usable on this build" : ""));

        LogLine("[FIRE] Bot guns (the game's own trigger fired no round from a bot's gun in any test, so the bot code fires every round itself, "
                "at the gun's own rate and damage): bursts %d | rounds %d -- hit for damage %d, on target but no damage taken (invulnerable, "
                "or not a player) %d, missed %d | ticks held back mid-burst: gun still equipping %d, gun empty %d, nothing in its hands %d, "
                "holding a pickaxe %d, the shooter itself down %d | rounds fired on stand-in numbers %d | shot effects sent to screens %d (%d "
                "of them on a target)%s.",
                GGunBursts, GGunRounds, GGunRounds - GGunMisses - GGunNoDamage, GGunNoDamage, GGunMisses, GGunHeldEquipping, GGunHeldEmpty,
                GGunHeldNoGun, GGunHeldMelee, GGunHeldDead, GGunStandInRounds, GShotFxSent, GShotFxSentHits, Effects);
    }

    static std::map<void*, std::pair<int, bool>> BotGroundCheck;

    static float TimeAccum = 0.f;
    static float LastFillTime = 0.f;
    static bool bMovedToIsland = false;

    static int GBodyVerdict = 0;
    static int GBodyChecks = 0;
    static int GBodyChecksFell = 0;
    static int GBodyScriptedOnTheirOwn = 0;
    static int GBodyMovesSent = 0;
    static int GBodyScriptedBlocked = 0;
    static float GLastBodyTick = -1.f;

    static UFunction* GServerMoveFn = nullptr;
    static bool GServerMoveResolved = false;
    static uint32 GSM_TimeStamp = 0, GSM_InAccel = 0, GSM_ClientLoc = 0, GSM_Flags = 0, GSM_Roll = 0, GSM_View = 0, GSM_Mode = 0, GSM_Size = 0;

    static bool ResolveServerMove(AFortPlayerPawnAthena* Pawn)
    {
        if (GServerMoveResolved)
            return GServerMoveFn != nullptr;

        GServerMoveResolved = true;

        auto Fn = Pawn ? ((UObject*)Pawn)->GetFunction("ServerMoveNoBase") : nullptr;
        if (!Fn)
        {
            LogLine("[BODY] ServerMoveNoBase is not on this build -- every bot is moved by script.");
            return false;
        }

        auto Params = Fn->GetParamsNamed();
        int Found = 0;

        for (auto& P : Params.NameOffsetMap)
        {

            if ((P.Name == "TimeStamp" || P.Name == "Timestamp") && P.ElementSize == 4) { GSM_TimeStamp = P.Offset; Found |= 1; }
            else if (P.Name == "InAccel" && P.ElementSize == 12)           { GSM_InAccel = P.Offset;   Found |= 2; }
            else if (P.Name == "ClientLoc" && P.ElementSize == 12)         { GSM_ClientLoc = P.Offset; Found |= 4; }
            else if (P.Name == "CompressedMoveFlags" && P.ElementSize == 1){ GSM_Flags = P.Offset;     Found |= 8; }
            else if (P.Name == "ClientRoll" && P.ElementSize == 1)         { GSM_Roll = P.Offset;      Found |= 16; }
            else if (P.Name == "View" && P.ElementSize == 4)               { GSM_View = P.Offset;      Found |= 32; }
            else if (P.Name == "ClientMovementMode" && P.ElementSize == 1) { GSM_Mode = P.Offset;      Found |= 64; }
        }

        GSM_Size = Params.Size;

        if (Found != 127 || GSM_Size == 0 || GSM_Size > 0x200)
        {
            int Count = 0;
            for (int Bit = 0; Bit < 7; Bit++)
                if (Found & (1 << Bit))
                    Count++;

            LogLine("[BODY] ServerMoveNoBase does not look the way it should on this build (found %d of 7 arguments, %u bytes) -- "
                    "every bot is moved by script.", Count, GSM_Size);
            return false;
        }

        GServerMoveFn = Fn;
        LogLine("[BODY] Found the game's ServerMoveNoBase (%u-byte move) -- bots send their moves the way a real player's game does.", GSM_Size);
        return true;
    }

    static bool SendMoveRaw(AFortPlayerPawnAthena* Pawn, uint8* Params)
    {
        ((UObject*)Pawn)->ProcessEvent(GServerMoveFn, Params);
        return true;
    }

    static bool SendMoveGuarded(AFortPlayerPawnAthena* Pawn, uint8* Params)
    {
        __try
        {
            return SendMoveRaw(Pawn, Params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static uint16 CompressAxis(double Angle)
    {
        double A = fmod(Angle, 360.0);
        if (A < 0.0)
            A += 360.0;

        return (uint16)(((long)lround(A * 65536.0 / 360.0)) & 0xFFFF);
    }

    static double BotHalfHeight(AFortPlayerPawnAthena* Pawn)
    {
        static double Cached = -1.0;
        if (Cached > 0.0 || !Pawn)
            return Cached > 0.0 ? Cached : 90.0;

        Cached = 90.0;

        auto Root = (UObject*)Pawn->RootComponent;
        if (Root)
        {
            const int32 Offset = (int32)Root->GetOffset("CapsuleHalfHeight");
            if (Offset > 0)
            {
                const float Half = GetFromOffset<float>(Root, (uint32)Offset);
                if (Half > 20.f && Half < 300.f)
                    Cached = Half;
            }
        }

        LogLine("[BODY] A bot is %.0f units from its middle to its feet (capsule half-height).", Cached);
        return Cached;
    }

    static float BotMaxAcceleration(AFortPlayerPawnAthena* Pawn)
    {
        static float Cached = -1.f;
        if (Cached > 0.f || !Pawn)
            return Cached > 0.f ? Cached : 2048.f;

        Cached = 2048.f;

        auto Move = Pawn->HasCharacterMovement() ? (UObject*)Pawn->CharacterMovement : nullptr;
        if (Move)
        {
            const int32 Offset = (int32)Move->GetOffset("MaxAcceleration");
            if (Offset > 0)
            {
                const float Accel = GetFromOffset<float>(Move, (uint32)Offset);
                if (Accel > 100.f && Accel < 100000.f)
                    Cached = Accel;
            }
        }

        return Cached;
    }

    static int32 GHitPointOffset = -2;

    static bool BotGroundBelow(AFortPlayerPawnAthena* Pawn, double X, double Y, double FromZ, double ToZ, double& OutZ)
    {
        if (!Pawn || !BotTraceAvailable())
            return false;

        if (GHitPointOffset == -2)
        {
            auto HitStruct = FindStruct("HitResult");
            GHitPointOffset = HitStruct ? (int32)HitStruct->GetOffset("ImpactPoint") : -1;

            if (GHitPointOffset < 0 && HitStruct)
                GHitPointOffset = (int32)HitStruct->GetOffset("Location");

            LogLine("[BODY] Ground traces: where the trace hit %s.", GHitPointOffset >= 0 ? "is readable"
                                                                                          : "is NOT readable -- scripted movement cannot find the ground");
        }

        if (GHitPointOffset < 0 || GHitPointOffset > 0x3C0)
            return false;

        alignas(16) uint8 HitBuffer[0x400];
        memset(HitBuffer, 0, sizeof(HitBuffer));

        FVector Start(X, Y, FromZ);
        FVector End(X, Y, ToZ);

        if (!BotTraceGuarded((AActor*)Pawn, &Start, &End, 0, HitBuffer))
            return false;

        if (VersionInfo.FortniteVersion >= 20)
            OutZ = *(double*)(HitBuffer + GHitPointOffset + 16);
        else
            OutZ = (double)*(float*)(HitBuffer + GHitPointOffset + 8);

        return true;
    }

    static int LandUnderSpot(AActor* Context, double X, double Y, double& OutZ, std::string& OutWhat)
    {
        OutZ = 0.0;
        OutWhat = "nothing";

        if (!Context || !BotTraceAvailable())
            return -1;

        static int32 PointOffset = -2;
        static int32 ActorOffset = -2;

        if (PointOffset == -2)
        {
            auto HitStruct = FindStruct("HitResult");
            PointOffset = HitStruct ? (int32)HitStruct->GetOffset("ImpactPoint") : -1;

            if (PointOffset < 0 && HitStruct)
                PointOffset = (int32)HitStruct->GetOffset("Location");

            ActorOffset = HitStruct ? (int32)HitStruct->GetOffset("Actor") : -1;
        }

        if (PointOffset < 0 || PointOffset > 0x3C0)
            return -1;

        alignas(16) uint8 HitBuffer[0x400];
        memset(HitBuffer, 0, sizeof(HitBuffer));

        FVector Start(X, Y, 20000.0);
        FVector End(X, Y, -40000.0);

        const bool bHit = BotTraceGuarded(Context, &Start, &End, 0, HitBuffer);

        if (GTraceFaulted)
            return -1;

        if (!bHit)
        {
            OutWhat = "nothing (open sea, or off the edge of the map)";
            return 0;
        }

        if (VersionInfo.FortniteVersion >= 20)
            OutZ = *(double*)(HitBuffer + PointOffset + 16);
        else
            OutZ = (double)*(float*)(HitBuffer + PointOffset + 8);

        AActor* HitActor = nullptr;

        if (ActorOffset >= 0 && ActorOffset < 0x3F0)
            HitActor = (AActor*)((FWeakObjectPtr*)(HitBuffer + ActorOffset))->Get();

        const std::string Class = HitActor ? BotClassName((const UObject*)HitActor) : std::string("something");

        char Text[192];
        snprintf(Text, sizeof(Text), "%s at Z=%.0f", Class.c_str(), OutZ);
        OutWhat = Text;

        std::string Lower = Class;
        for (auto& Ch : Lower)
            Ch = (char)tolower((unsigned char)Ch);

        if (Lower.find("water") != std::string::npos)
            return 0;

        if (OutZ < -2400.0)
            return 0;

        return 1;
    }

    static bool GroundZUnder(AActor* Context, double X, double Y, double FromZ, double& OutZ)
    {
        OutZ = 0.0;

        if (!Context || !BotTraceAvailable())
            return false;

        static int32 PointOffset = -2;

        if (PointOffset == -2)
        {
            auto HitStruct = FindStruct("HitResult");
            PointOffset = HitStruct ? (int32)HitStruct->GetOffset("ImpactPoint") : -1;

            if (PointOffset < 0 && HitStruct)
                PointOffset = (int32)HitStruct->GetOffset("Location");
        }

        if (PointOffset < 0 || PointOffset > 0x3C0)
            return false;

        alignas(16) uint8 HitBuffer[0x400];
        memset(HitBuffer, 0, sizeof(HitBuffer));

        FVector Start(X, Y, FromZ);
        FVector End(X, Y, -40000.0);

        const bool bHit = BotTraceGuarded(Context, &Start, &End, 0, HitBuffer);

        if (GTraceFaulted || !bHit)
            return false;

        if (VersionInfo.FortniteVersion >= 20)
            OutZ = *(double*)(HitBuffer + PointOffset + 16);
        else
            OutZ = (double)*(float*)(HitBuffer + PointOffset + 8);

        return OutZ > -40000.0 && OutZ < FromZ;
    }

    static void BodyRealStep(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc, float Dt)
    {

        if (Controller->AcknowledgedPawn != (AActor*)Pawn)
            Controller->AcknowledgedPawn = (AActor*)Pawn;

        double AX = 0.0, AY = 0.0;

        if (Body.bHasGoal)
        {
            const double DX = Body.Goal.X - Loc.X;
            const double DY = Body.Goal.Y - Loc.Y;
            const double Dist = sqrt(DX * DX + DY * DY);

            if (Dist <= (double)Body.AcceptRadius)
            {
                Body.bHasGoal = false;
            }
            else
            {
                const double Accel = (double)BotMaxAcceleration(Pawn);
                AX = DX / Dist * Accel;
                AY = DY / Dist * Accel;
            }
        }

        const FRotator Aim = Controller->GetControlRotation();
        const uint32 View = ((uint32)CompressAxis(Aim.Yaw) << 16) | (uint32)CompressAxis(Aim.Pitch);

        Body.Clock += (Dt > 0.125f) ? 0.125f : Dt;

        uint8 Mode = 0;
        if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement && Pawn->CharacterMovement->HasMovementMode())
            Mode = Pawn->CharacterMovement->MovementMode;

        alignas(16) uint8 Params[0x200];
        memset(Params, 0, sizeof(Params));

        *(float*)(Params + GSM_TimeStamp) = Body.Clock;

        float* Accel = (float*)(Params + GSM_InAccel);
        Accel[0] = (float)AX;
        Accel[1] = (float)AY;
        Accel[2] = 0.f;

        float* ClientLoc = (float*)(Params + GSM_ClientLoc);
        ClientLoc[0] = 1.f;
        ClientLoc[1] = 2.f;
        ClientLoc[2] = 3.f;

        Params[GSM_Flags] = 0;
        Params[GSM_Roll] = 0;
        *(uint32*)(Params + GSM_View) = View;
        Params[GSM_Mode] = Mode;

        if (!SendMoveGuarded(Pawn, Params))
        {
            GBodyVerdict = 2;
            LogLine("[BODY] Sending a bot's move faulted -- every bot is moved by script from now on.");
            return;
        }

        GBodyMovesSent++;
    }

    static void BodyScriptedStep(AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc, float Dt, bool bAirborne)
    {
        auto Move = (Pawn->HasCharacterMovement() && Pawn->CharacterMovement) ? Pawn->CharacterMovement : nullptr;
        const double Half = BotHalfHeight(Pawn);

        auto SetVelocity = [&](double VX, double VY, double VZ)
        {
            if (Move && Move->HasVelocity())
            {
                FVector Velocity(VX, VY, VZ);
                Move->Velocity = Velocity;
            }
        };

        bool bFalling = bAirborne;

        if (!bAirborne && !Body.bOnGround)
        {
            if (!Body.bGroundKnown || fabs(Body.GroundFor.X - Loc.X) > 300.0 || fabs(Body.GroundFor.Y - Loc.Y) > 300.0)
            {
                double Ground = 0.0;
                Body.GroundZ = BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z, Loc.Z - 40000.0, Ground) ? Ground : 0.0;
                Body.bGroundKnown = true;
                Body.GroundFor = FVector(Loc.X, Loc.Y, Loc.Z);
            }

            if (((double)Loc.Z - Half) - Body.GroundZ > 150.0)
                bFalling = true;
            else
                Body.bOnGround = true;
        }

        if (bFalling)
        {
            Body.bOnGround = false;

            const bool bGlider = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;
            const double Down = bGlider ? 700.0 : 2400.0;

            if (!Body.bGroundKnown || fabs(Body.GroundFor.X - Loc.X) > 300.0 || fabs(Body.GroundFor.Y - Loc.Y) > 300.0)
            {
                double Ground = 0.0;
                Body.GroundZ = BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z, Loc.Z - 40000.0, Ground) ? Ground : 0.0;
                Body.bGroundKnown = true;
                Body.GroundFor = FVector(Loc.X, Loc.Y, Loc.Z);
            }

            const double Floor = Body.GroundZ + Half + 5.0;
            double NewZ = (double)Loc.Z - Down * (double)Dt;
            bool bOnTheGround = false;

            if (NewZ <= Floor)
            {
                NewZ = Floor;
                bOnTheGround = true;
            }

            FVector To(Loc.X, Loc.Y, NewZ);
            Pawn->K2_SetActorLocation(To, false, (void*)nullptr, true);

            SetVelocity(0.0, 0.0, bOnTheGround ? 0.0 : -Down);

            if (bOnTheGround)
            {
                Body.bOnGround = true;

                if (Move && Move->HasMovementMode() && Move->MovementMode != 1)
                    Move->SetMovementMode((uint8)1, (uint8)0);
            }

            return;
        }

        if (!Body.bHasGoal)
        {
            SetVelocity(0.0, 0.0, 0.0);
            return;
        }

        const double DX = Body.Goal.X - Loc.X;
        const double DY = Body.Goal.Y - Loc.Y;
        const double Dist = sqrt(DX * DX + DY * DY);

        if (Dist <= (double)Body.AcceptRadius)
        {
            Body.bHasGoal = false;
            SetVelocity(0.0, 0.0, 0.0);
            return;
        }

        double Speed = 510.0;
        if (Move && Move->HasMaxWalkSpeed() && Move->MaxWalkSpeed > 100.f)
            Speed = (double)Move->MaxWalkSpeed;

        const double Step = (Dist < Speed * (double)Dt) ? Dist : Speed * (double)Dt;
        const double NX = Loc.X + DX / Dist * Step;
        const double NY = Loc.Y + DY / Dist * Step;

        const double Feet = (double)Loc.Z - Half;
        double Ground = 0.0;

        if (!BotGroundBelow(Pawn, NX, NY, Feet + 60.0, Feet - 3000.0, Ground) || Ground > Feet + 55.0 || Ground < Feet - 250.0)
        {
            GBodyScriptedBlocked++;
            SetVelocity(0.0, 0.0, 0.0);
            return;
        }

        FVector To(NX, NY, Ground + Half + 2.0);
        Pawn->K2_SetActorLocation(To, false, (void*)nullptr, true);

        Body.bOnGround = true;
        Body.bGroundKnown = true;
        Body.GroundZ = Ground;
        Body.GroundFor = FVector(NX, NY, Ground + Half + 2.0);

        SetVelocity(DX / Dist * Speed, DY / Dist * Speed, 0.0);
    }

    static void TickBotBodies(UWorld* World, AFortGameStateAthena* GameState)
    {
        if (!bBusDeparted || !World || !GameState || !GameState->HasPlayerArray())
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        float Dt = (GLastBodyTick < 0.f) ? 0.033f : (Now - GLastBodyTick);
        GLastBodyTick = Now;

        if (!(Dt > 0.001f))
            Dt = 0.001f;
        if (Dt > 0.25f)
            Dt = 0.25f;

        int OnReal = 0;
        int OnScript = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f)
                continue;

            if (MovedToIsland.find((void*)Pawn) == MovedToIsland.end())
                continue;

            auto& Body = BotBodies[(void*)Pawn];

            if (Body.Owner != (void*)Controller)
            {
                if (Body.Owner)
                    Body = FBotBody{};

                Body.Owner = (void*)Controller;
            }

            const FVector Loc = Pawn->K2_GetActorLocation();
            const bool bAirborne = GlidingBots.find((void*)Pawn) != GlidingBots.end();

            const bool bReal = GBodyVerdict != 2 && !Body.bScripted && ResolveServerMove(Pawn);

            if (!bReal)
            {
                BodyScriptedStep(Pawn, Body, Loc, Dt, bAirborne);
                OnScript++;
                continue;
            }

            if (bAirborne)
                HoldBotGlideDescent(Pawn);

            BodyRealStep(Controller, Pawn, Body, Loc, Dt);

            if (GBodyVerdict == 2)
            {
                OnScript++;
                continue;
            }

            OnReal++;

            if (Body.bChecked)
                continue;

            if (Body.WatchSince < 0.f)
            {
                double Ground = 0.0;
                const bool bHigh = BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z, Loc.Z - 40000.0, Ground)
                                       ? (((double)Loc.Z - Ground) > 1500.0)
                                       : ((double)Loc.Z > 8000.0);

                if (!bHigh)
                {
                    Body.bChecked = true;
                    continue;
                }

                Body.WatchSince = Now;
                Body.WatchZ = (float)Loc.Z;
                continue;
            }

            if ((float)Loc.Z > Body.WatchZ)
                Body.WatchZ = (float)Loc.Z;

            if ((Now - Body.WatchSince) < 1.f)
                continue;

            Body.bChecked = true;

            const float Fell = Body.WatchZ - (float)Loc.Z;
            const bool bFell = Fell >= 150.f;

            static int GoodReports = 0;
            if (bFell && ++GoodReports <= 3)
                LogLine("[BODY] '%s' came down %.0f units in 1s under real movement -- the game's own movement moves it.",
                        SafeNameOf(PlayerState).c_str(), Fell);

            if (GBodyVerdict == 0)
            {
                GBodyChecks++;
                if (bFell)
                    GBodyChecksFell++;

                if (GBodyChecks >= 6)
                {
                    GBodyVerdict = (GBodyChecksFell * 2 >= GBodyChecks) ? 1 : 2;

                    if (GBodyVerdict == 1)
                        LogLine("[BODY] Real movement works for bots on this build: %d of %d bots checked came down at least 150 units in 1s. "
                                "Bots skydive, glide, land and walk on the game's own movement.", GBodyChecksFell, GBodyChecks);
                    else
                        LogLine("[BODY] Real movement does NOT move bots on this build: only %d of %d bots checked came down in 1s. "
                                "Every bot is moved by script from now on.", GBodyChecksFell, GBodyChecks);
                }
            }

            if (!bFell && GBodyVerdict != 2 && !Body.bScripted)
            {
                Body.bScripted = true;
                GBodyScriptedOnTheirOwn++;

                if (GBodyScriptedOnTheirOwn <= 5)
                    LogLine("[BODY] '%s' came down only %.0f units in 1s under real movement -- this one is moved by script.",
                            SafeNameOf(PlayerState).c_str(), Fell);
            }
        }

        static float LastReport = -1000.f;

        if ((Now - LastReport) >= 30.f && (OnReal + OnScript) > 0)
        {
            LastReport = Now;
            LogLine("[BODY] %d bot(s) on real movement (%d moves sent so far), %d moved by script (%d scripted steps refused: wall, cliff or no ground) | "
                    "verdict: %s.", OnReal, GBodyMovesSent, OnScript, GBodyScriptedBlocked,
                    GBodyVerdict == 1 ? "real movement works" : (GBodyVerdict == 2 ? "scripted for everyone" : "still checking"));
        }
    }

    static const wchar_t* GBotKitGuns[] = {
        L"WID_Assault_AutoHigh_Athena_SR_Ore_T03",
        L"WID_Shotgun_Standard_Athena_C_Ore_T03",
        L"WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03",
    };

    static std::map<void*, float> BotArmRetryAt;
    static int GBotsArmed = 0;

    static void ArmLandedBots(UWorld* World, AFortGameStateAthena* GameState)
    {
        if (!bBusDeparted || !World || !GameState || !GameState->HasPlayerArray())
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f || !BotHasLanded((void*)Pawn))
                continue;

            if (BotIsHoldingAWeapon(Pawn))
            {

                if (BotWeapons.find((void*)PlayerState) == BotWeapons.end())
                {
                    auto Weapon = (AFortWeapon*)Pawn->CurrentWeapon;
                    if (Weapon->HasWeaponData() && Weapon->WeaponData)
                        BotWeapons[(void*)PlayerState] = (const UFortItemDefinition*)Weapon->WeaponData;
                }

                KeepBotGunOut(Pawn, PlayerState, Now, "on the ground");
                continue;
            }

            auto& RetryAt = BotArmRetryAt[(void*)Pawn];
            if (Now < RetryAt)
                continue;
            RetryAt = Now + 2.f;

            AFortInventory* Inventory = Controller->WorldInventory;
            if (!Inventory)
            {
                auto It = BotInventories.find((void*)PlayerState);
                if (It != BotInventories.end())
                    Inventory = It->second;
            }

            if (!Inventory)
                continue;

            const UFortItemDefinition* Gun = nullptr;
            FGuid GunGuid{};

            {
                int BestScore = -1;
                auto& Entries = Inventory->Inventory.ReplicatedEntries;

                for (int e = 0; e < Entries.Num(); e++)
                {
                    auto& Entry = Entries.Get(e, FFortItemEntry::Size());
                    const int Score = BotGunScore(Entry.ItemDefinition);

                    if (Score > BestScore)
                    {
                        BestScore = Score;
                        Gun = Entry.ItemDefinition;
                        GunGuid = Entry.ItemGuid;
                    }
                }
            }

            bool bGaveIt = false;

            if (!Gun)
            {
                auto Def = FindItemDefByName(GBotKitGuns[0]);

                if (Def)
                {
                    int Clip = 30;
                    if (auto RangedDef = Def->Cast<UFortWeaponItemDefinition>())
                        if (auto Stats = AFortInventory::GetStats(RangedDef))
                            if (Stats->ClipSize > 0)
                                Clip = Stats->ClipSize;

                    if (auto Item = Inventory->GiveItem(Def, 1, Clip))
                    {
                        Gun = Def;
                        if (Item->HasItemEntry() && FFortItemEntry::HasItemGuid())
                            GunGuid = Item->ItemEntry.ItemGuid;
                        bGaveIt = true;

                        GiveBotSpareRounds(Inventory, Def);
                    }
                }
            }

            auto GunDef = Gun ? Gun->Cast<UFortWeaponItemDefinition>() : nullptr;
            if (!GunDef)
                continue;

            Pawn->EquipWeaponDefinition(GunDef, GunGuid);

            KeepBotGunOut(Pawn, PlayerState, Now, "right after it took it out");

            BotWeapons[(void*)PlayerState] = Gun;
            if (BotInventories.find((void*)PlayerState) == BotInventories.end())
                BotInventories[(void*)PlayerState] = Inventory;

            if (auto Stats = AFortInventory::GetStats(GunDef))
                if (Stats->ClipSize > 0)
                    BotWeaponClipSize[(void*)PlayerState] = Stats->ClipSize;

            GBotsArmed++;

            if (GBotsArmed <= 5 || (GBotsArmed % 25) == 0)
                LogLine("[ARMS] '%s' was down from the bus with nothing in its hands -- %s its %s (%d armed so far; holding it now: %s, "
                        "holstered: %s).",
                        SafeNameOf(PlayerState).c_str(), bGaveIt ? "gave it" : "took out", BotObjectName(Gun).c_str(), GBotsArmed,
                        BotIsHoldingAWeapon(Pawn) ? "yes" : "NO",
                        BotGunHolstered(Pawn) == 1 ? "YES" : (BotGunHolstered(Pawn) == 0 ? "no" : "cannot be read"));
        }
    }

    static void TickMovement(UWorld* World, AFortGameStateAthena* GameState, AFortGameMode* GameMode)
    {
        int nativeBrainCount = 0;
        int fallbackCount = 0;

        static int reportCounter_forDir = 0;
        static std::map<void*, int> BotWanderDirTick;

        std::set<void*> LivePawns;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto LiveState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!LiveState || !LiveState->HasOwner() || !LiveState->Owner)
                continue;

            auto LivePawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)LiveState->Owner)->Pawn;

            if (LivePawn && LivePawn->GetHealth() > 0.f)
                LivePawns.insert((void*)LivePawn);
        }

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn)
                continue;

            GFightBot = (void*)PlayerState;
            GTickStage = "TickMovement: a bot's turn (movement, senses, the native brain)";

            if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
            {
                auto Move = Pawn->CharacterMovement;

                if (Move->HasMaxWalkSpeed())
                {
                    float Sprint = (FConfiguration::BotSprintSpeed > 0.f) ? FConfiguration::BotSprintSpeed : 510.f;
                    if (Move->MaxWalkSpeed < Sprint)
                        Move->MaxWalkSpeed = Sprint;
                }

                if (FConfiguration::bBotsFaceTheirDirection && Move->HasVelocity())
                {
                    const FVector Velocity = Move->Velocity;
                    const double SpeedX = Velocity.X;
                    const double SpeedY = Velocity.Y;
                    const double FlatSpeed = sqrt(SpeedX * SpeedX + SpeedY * SpeedY);

                    float Threshold = FConfiguration::BotFacingMinimumSpeed;
                    if (!(Threshold > 0.f))
                        Threshold = 40.f;

                    if (FlatSpeed > (double)Threshold)
                    {
                        const double TravelYaw = atan2(SpeedY, SpeedX) * (180.0 / 3.14159265358979323846);

                        auto Faced = BotFacingYaw.find(Pawn);
                        double Difference = (Faced != BotFacingYaw.end()) ? (TravelYaw - (double)Faced->second) : 999.0;

                        while (Difference > 180.0) Difference -= 360.0;
                        while (Difference < -180.0) Difference += 360.0;

                        if (fabs(Difference) > 4.0)
                        {
                            BotFacingYaw[Pawn] = (float)TravelYaw;

                            FRotator Heading{};
                            double HeadPitch = 0.0, HeadYaw = TravelYaw, HeadRoll = 0.0;
                            Heading.Pitch = HeadPitch;
                            Heading.Yaw = HeadYaw;
                            Heading.Roll = HeadRoll;

                            Pawn->K2_SetActorRotation(Heading, false);

                            ((::AAIController*)Controller)->SetControlRotation(Heading);
                        }
                    }
                }
            }

            auto AIController = (::AAIController*)Controller;

            SilenceNativeSenses(AIController);

            const bool bHasNativeBrain = AIController->HasBrainComponent() && AIController->BrainComponent;

            if (bHasNativeBrain)
            {
                nativeBrainCount++;

                if (!FConfiguration::bAllBotsFight)
                    continue;
            }
            else
            {
                fallbackCount++;
            }

            if (!bHasNativeBrain && BTRetried.find((void*)Controller) == BTRetried.end())
            {
                BTRetried.insert((void*)Controller);

                static UObject* PhoebeBT = nullptr;
                static bool bLookedUp = false;
                if (!bLookedUp)
                {
                    bLookedUp = true;
                    PhoebeBT = (UObject*)FindObject<UObject>(L"/Game/Athena/AI/Phoebe/BT_Phoebe.BT_Phoebe");
                    LogLine("[BotAI] BT_Phoebe asset lookup: %s\n", PhoebeBT ? "found" : "NOT FOUND");
                }

                if (PhoebeBT)
                {
                    bool started = AIController->RunBehaviorTree(PhoebeBT);
                    static int reported = 0;
                    if (reported < 3)
                    {
                        reported++;
                        LogLine("[BotAI] Manual RunBehaviorTree returned %s.\n", started ? "true" : "false");
                    }
                }
            }

            FVector MyLoc = Pawn->K2_GetActorLocation();

            FVector Direction;
            bool bOutsideSafeZone = Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone;

            auto combatIt = BotCombatTarget.find(Pawn);
            AFortPlayerPawnAthena* Target = (combatIt != BotCombatTarget.end()) ? combatIt->second : nullptr;

            const bool bCombatOpen = BotCombatIsOpen() && BotHasLanded((void*)Pawn);

            if (!bCombatOpen)
            {
                Target = nullptr;
                BotCombatTarget[Pawn] = nullptr;
                BotLastSaw.erase((void*)Pawn);
                AggroedByHuman.erase((void*)Controller);
                BotUnderFireSince.erase((void*)Controller);

                BotStopAiming(AIController);
                BotIntendedAim.erase((void*)Pawn);
                BotShotAim.erase((void*)Pawn);

                auto FireIt = BotFire.find(Pawn);

                if (FireIt != BotFire.end())
                {
                    if (FireIt->second.bFiring || FireIt->second.BurstTicksLeft > 0)
                    {
                        BotGunTrigger(Pawn, false);
                        FireIt->second.bFiring = false;
                    }

                    FireIt->second.BurstTicksLeft = 0;
                    FireIt->second.ReactionTicksLeft = -1;
                    FireIt->second.CooldownTicksLeft = 0;
                }

                continue;
            }

            GTickStage = "TickMovement: fighting (running the gun, picking a target, aiming, the trigger and the burst)";

            TickBotReload(Pawn, PlayerState, Target && LivePawns.find((void*)Target) != LivePawns.end());

            if (Target && LivePawns.find((void*)Target) == LivePawns.end())
            {
                GLocksEnded++;

                auto Locked = BotLockedOnName.find(Pawn);

                if (GLocksEnded <= 10)
                    LogLine("[LOCK] %s's target '%s' is down -- free to lock onto somebody new.", SafeNameOf(PlayerState).c_str(),
                            Locked != BotLockedOnName.end() ? Locked->second.c_str() : "?");

                if (Locked != BotLockedOnName.end())
                    BotLockedOnName.erase(Locked);

                Target = nullptr;
                BotCombatTarget[Pawn] = nullptr;
                BotLastSaw.erase((void*)Pawn);
                BotLastSeenSpot.erase((void*)Pawn);
                BotIntendedAim.erase((void*)Pawn);
                BotShotAim.erase((void*)Pawn);
                BotGroundCheck.erase((void*)Pawn);
                BotStoppedForFight.erase((void*)Pawn);
            }

            if (!Target)
            {
                AFortPlayerPawnAthena* Best = nullptr;

                const double BotRange = (FConfiguration::BotEngageRange > 0.f) ? (double)FConfiguration::BotEngageRange : 12000.0;
                const double HuntRange = (FConfiguration::BotHuntPlayerRange > 0.f) ? (double)FConfiguration::BotHuntPlayerRange : 24000.0;
                const double Preference = (FConfiguration::BotPlayerPreference > 0.f) ? (double)FConfiguration::BotPlayerPreference : 0.35;

                double bestScore = -1.0;

                for (int e = 0; e < GameState->PlayerArray.Num(); e++)
                {
                    auto OtherState = (AFortPlayerStateAthena*)GameState->PlayerArray[e];
                    if (!OtherState || OtherState == PlayerState)
                        continue;
                    auto OtherController = (AFortPlayerControllerAthena*)OtherState->Owner;
                    if (!OtherController)
                        continue;
                    auto OtherPawn = (AFortPlayerPawnAthena*)OtherController->Pawn;
                    if (!OtherPawn || OtherPawn->GetHealth() <= 0.0f)
                        continue;

                    const bool bIsHuman = OtherState->HasbIsABot() && !OtherState->bIsABot && !NPCs::IsNPC(OtherState);

                    if (!bIsHuman && !FConfiguration::bBotsFightEachOther)
                        continue;

                    if (bIsHuman && !FConfiguration::bBotsFightRealPlayers)
                        continue;

                    FVector OLoc = OtherPawn->K2_GetActorLocation();
                    const double odx = OLoc.X - MyLoc.X;
                    const double ody = OLoc.Y - MyLoc.Y;
                    const double Distance = sqrt(odx * odx + ody * ody);

                    const double kBotOnBotMaxRange = (FConfiguration::BotShootRealPlayerRange > 0.f)
                        ? (double)FConfiguration::BotShootRealPlayerRange : 5000.0;
                    double MaxRange = bIsHuman
                        ? (double)FConfiguration::BotShootRealPlayerRange
                        : kBotOnBotMaxRange;
                    if (bIsHuman && (MaxRange <= 0.0 || MaxRange > HuntRange))
                        MaxRange = HuntRange;

                    if (Distance > MaxRange)
                        continue;

                    if (!BotCanSee(AIController, Pawn, MyLoc, OtherPawn, (AActor*)OtherPawn, reportCounter_forDir, bIsHuman))
                        continue;

                    if (BotGroundInTheWay(Pawn, MyLoc, OtherPawn, OLoc))
                        continue;

                    const double Score = bIsHuman ? (Distance * Preference) : Distance;

                    if (bestScore < 0.0 || Score < bestScore)
                    {
                        bestScore = Score;
                        Best = OtherPawn;
                    }
                }

                if (!Best)
                {
                    auto Agg = AggroedByHuman.find((void*)Controller);
                    if (Agg != AggroedByHuman.end() && Agg->second.first &&
                        (GStormTick - Agg->second.second) <= (int)FConfiguration::BotShotAggroTicks)
                    {
                        auto AggState = (AFortPlayerStateAthena*)Agg->second.first;
                        if (AggState && AggState->HasOwner() && AggState->Owner)
                        {
                            auto AggCtrl = (AFortPlayerControllerAthena*)AggState->Owner;
                            auto AggPawn = (AFortPlayerPawnAthena*)AggCtrl->Pawn;
                            if (AggPawn && AggPawn->GetHealth() > 0.f)
                                Best = AggPawn;
                        }
                        else
                            AggroedByHuman.erase((void*)Controller);
                    }
                }

                Target = Best;
                BotCombatTarget[Pawn] = Target;

                if (Target)
                {
                    GLocksMade++;

                    bool bLockedOnHuman = false;
                    if (auto LockController = (AFortPlayerControllerAthena*)Target->Controller)
                        if (auto LockState = (AFortPlayerStateAthena*)LockController->PlayerState)
                            bLockedOnHuman = LockState->HasbIsABot() && !LockState->bIsABot && !NPCs::IsNPC(LockState);

                    const std::string TargetName = BotPawnName(Target);
                    BotLockedOnName[Pawn] = TargetName;

                    const FVector LockLoc = Target->K2_GetActorLocation();
                    const double LockDx = LockLoc.X - MyLoc.X;
                    const double LockDy = LockLoc.Y - MyLoc.Y;

                    static int LockReports = 0;
                    if (bLockedOnHuman || ++LockReports <= 10)
                        LogLine("[LOCK] '%s' locked onto '%s' (%s) %.0f m away -- it fights only them now, until one of them dies.",
                                SafeNameOf(PlayerState).c_str(), TargetName.c_str(), bLockedOnHuman ? "a real player" : "a bot",
                                (float)(sqrt(LockDx * LockDx + LockDy * LockDy) / 100.0));
                }
            }

            bool bInCombat = false;
            bool bLongRangeAggro = false;

            double EngageRange = (FConfiguration::BotEngageRange > 0.f) ? (double)FConfiguration::BotEngageRange : 12000.0;
            if (Target)
            {
                if (auto TgtController = (AFortPlayerControllerAthena*)Target->Controller)
                    if (auto TgtState = (AFortPlayerStateAthena*)TgtController->PlayerState)
                        if (TgtState->HasbIsABot() && TgtState->bIsABot)
                            EngageRange = (FConfiguration::BotShootRealPlayerRange > 0.f)
                                ? (double)FConfiguration::BotShootRealPlayerRange : 5000.0;
            }
            const double PreferredRange = 1400.0;

            const bool bMayFight = FConfiguration::bBotCombatEnabled && bCombatOpen;

            bool bTargetIsHuman = false;
            if (Target)
            {
                if (auto TgtController = (AFortPlayerControllerAthena*)Target->Controller)
                    if (auto TgtState = (AFortPlayerStateAthena*)TgtController->PlayerState)
                        bTargetIsHuman = TgtState->HasbIsABot() && !TgtState->bIsABot && !NPCs::IsNPC(TgtState);
            }

            bool bHumanTooFar = false;
            if (Target && bTargetIsHuman)
            {
                FVector TgtLoc = Target->K2_GetActorLocation();
                const double hx = TgtLoc.X - MyLoc.X;
                const double hy = TgtLoc.Y - MyLoc.Y;
                const double Dist = sqrt(hx * hx + hy * hy);
                bHumanTooFar = Dist > (double)FConfiguration::BotShootRealPlayerRange;

                if (bHumanTooFar)
                {
                    auto Agg = AggroedByHuman.find((void*)Controller);
                    if (Agg != AggroedByHuman.end() && Agg->second.first &&
                        (GStormTick - Agg->second.second) <= (int)FConfiguration::BotShotAggroTicks)
                    {
                        auto AggState = (AFortPlayerStateAthena*)Agg->second.first;
                        if (AggState)
                        {
                            if (auto TgtController = (AFortPlayerControllerAthena*)Target->Controller)
                                if (TgtController->PlayerState == AggState)
                                {
                                    bHumanTooFar = false;
                                    bLongRangeAggro = true;
                                }
                        }
                    }
                }
            }

            if (bMayFight && Target && Target->GetHealth() > 0.0f)
            {
                FVector TLoc = Target->K2_GetActorLocation();

                const double tdx = TLoc.X - MyLoc.X;
                const double tdy = TLoc.Y - MyLoc.Y;
                const double tdz = TLoc.Z - MyLoc.Z;
                const double Flat = sqrt(tdx * tdx + tdy * tdy);

                const double EffectiveEngage = bLongRangeAggro
                    ? ((FConfiguration::BotHuntPlayerRange > 0.f) ? (double)FConfiguration::BotHuntPlayerRange : 50000.0)
                    : EngageRange;

                {
                    bInCombat = true;

                    const bool bInShootRange = !bHumanTooFar && Flat < EffectiveEngage;
                    const bool bGameSaysVisible = BotHasLineOfSight(AIController, (AActor*)Target);

                    bool bGroundInTheWay = false;

                    if (bGameSaysVisible)
                    {
                        auto Checked = BotGroundCheck.find((void*)Pawn);

                        if (Checked == BotGroundCheck.end() || (reportCounter_forDir - Checked->second.first) >= 6)
                        {
                            std::string Blocker;
                            const bool bBlocked = BotGroundInTheWay(Pawn, MyLoc, Target, TLoc, &Blocker);

                            BotGroundCheck[(void*)Pawn] = { reportCounter_forDir, bBlocked };

                            if (bBlocked)
                            {
                                static int GroundReports = 0;

                                if (++GroundReports <= 8)
                                    LogLine("[EYES] The game's check said '%s' can see '%s' (%.0f m), but the ground (%s) is in "
                                            "the way -- no shot.",
                                            SafeNameOf(PlayerState).c_str(), BotPawnName(Target).c_str(),
                                            (float)(Flat / 100.0), Blocker.c_str());
                            }
                        }

                        bGroundInTheWay = BotGroundCheck[(void*)Pawn].second;

                        if (bGroundInTheWay)
                            GTerrainBlocks++;
                    }
                    else if (bInShootRange)
                    {

                        static int BlockedSamples = 0;

                        if (BlockedSamples < 6 && BotTraceAvailable())
                        {
                            BlockedSamples++;

                            FVector Eyes = MyLoc;
                            Eyes.Z = MyLoc.Z + 60.0;

                            FVector Chest = TLoc;
                            Chest.Z = TLoc.Z + 30.0;

                            const FBotHit Hit = BotTrace((AActor*)Pawn, Eyes, Chest, 0);

                            LogLine("[EYES] Sample: '%s' cannot see '%s' (%.0f m) -- a straight line to their chest "
                                    "hits %s (%s).",
                                    SafeNameOf(PlayerState).c_str(), BotPawnName(Target).c_str(), (float)(Flat / 100.0),
                                    Hit.bHit ? BotObjectName(Hit.Actor).c_str() : "nothing",
                                    Hit.bHit ? BotClassName(Hit.Actor).c_str() : "-");
                        }
                    }

                    const bool bTargetVisible = bGameSaysVisible && !bGroundInTheWay;
                    const bool bCanShoot = bInShootRange && bTargetVisible;

                    if (bTargetVisible)
                        BotLastSeenSpot[(void*)Pawn] = TLoc;

                    const double Yaw = atan2(tdy, tdx) * (180.0 / 3.14159265358979323846);
                    const double Pitch = atan2(tdz, Flat > 1.0 ? Flat : 1.0) * (180.0 / 3.14159265358979323846);

                    {
                        auto Was = BotIntendedAim.find((void*)Pawn);
                        auto FireNow = BotFire.find((void*)Pawn);

                        if (Was != BotIntendedAim.end() && FireNow != BotFire.end() && FireNow->second.bFiring && !GAimReadFaulted)
                        {
                            double ActualYaw = 0.0;

                            if (BotReadAimGuarded(AIController, &ActualYaw))
                            {
                                double Off = ActualYaw - Was->second.Yaw;
                                while (Off > 180.0) Off -= 360.0;
                                while (Off < -180.0) Off += 360.0;
                                Off = fabs(Off);

                                GAimChecks++;

                                if (Off <= 2.0)
                                    GAimHeld++;

                                GAimOffSum += Off;

                                if (Off > GAimOffMax)
                                    GAimOffMax = Off;
                            }
                        }
                    }

                    if (bTargetVisible)
                    {

                        double ErrYaw = 0.0, ErrPitch = 0.0;

                        const float AimDeg = bLongRangeAggro
                            ? FConfiguration::BotLongRangeAimErrorDegrees
                            : FConfiguration::BotAimErrorDegrees;

                        if (AimDeg > 0.f)
                        {
                            auto et = BotAimErrorTick.find(Pawn);
                            if (et == BotAimErrorTick.end() || (reportCounter_forDir - et->second) > 12)
                            {
                                BotAimErrorTick[Pawn] = reportCounter_forDir;

                                const double Max = (double)AimDeg;
                                const double PitchMax = Max * (double)FConfiguration::BotAimErrorPitchScale;

                                ErrYaw   = (((double)(rand() % 2001) / 1000.0) - 1.0) * Max;
                                ErrPitch = (((double)(rand() % 2001) / 1000.0) - 1.0) * PitchMax;

                                if (FConfiguration::BotAimMissChance > 0.f &&
                                    (double)(rand() % 10000) < (double)FConfiguration::BotAimMissChance * 10000.0)
                                {
                                    const double AimDist = sqrt(Flat * Flat + tdz * tdz);
                                    const double HalfWidth = atan2(45.0, AimDist > 1.0 ? AimDist : 1.0) * (180.0 / 3.14159265358979323846);
                                    const double Miss = HalfWidth + 1.5 + ((double)(rand() % 1000) / 1000.0) * 2.5;

                                    ErrYaw = (rand() % 2) ? Miss : -Miss;
                                    GAimDeliberateMisses++;
                                }

                                BotAimError[Pawn] = FVector{ ErrYaw, ErrPitch, 0.0 };
                            }
                            else
                            {
                                auto ae = BotAimError.find(Pawn);
                                if (ae != BotAimError.end())
                                {
                                    ErrYaw = ae->second.X;
                                    ErrPitch = ae->second.Y;
                                }
                            }
                        }

                        const double AimDist = sqrt(Flat * Flat + tdz * tdz);
                        const double HalfWide = atan2(45.0, AimDist > 1.0 ? AimDist : 1.0) * (180.0 / 3.14159265358979323846);
                        const double HalfTall = atan2(85.0, AimDist > 1.0 ? AimDist : 1.0) * (180.0 / 3.14159265358979323846);
                        const bool bOnTarget = fabs(ErrYaw) <= HalfWide && fabs(ErrPitch) <= HalfTall;

                        if (!bOnTarget && fabs(ErrYaw) <= HalfWide)
                            ErrYaw = (ErrYaw < 0.0 ? -1.0 : 1.0) * (HalfWide + 1.0);

                        FRotator Aim{};
                        double AimPitch = Pitch + ErrPitch, AimYaw = Yaw + ErrYaw, AimRoll = 0.0;
                        Aim.Pitch = AimPitch;
                        Aim.Yaw = AimYaw;
                        Aim.Roll = AimRoll;

                        AIController->SetControlRotation(Aim);

                        if (IsPlayerControllerBot((AActor*)Controller))
                            KeepBotCameraOnItsEyes(Controller, Pawn, Aim);

                        FRotator Face{};
                        double FacePitch = 0.0, FaceYaw = Yaw, FaceRoll = 0.0;
                        Face.Pitch = FacePitch;
                        Face.Yaw = FaceYaw;
                        Face.Roll = FaceRoll;
                        Pawn->K2_SetActorRotation(Face, false);

                        BotAimAt(AIController, bOnTarget ? Target : nullptr, MyLoc, AimYaw, AimPitch, AimDist);

                        FBotAimRecord AimRecord{};
                        AimRecord.Yaw = bOnTarget ? Yaw : AimYaw;
                        AimRecord.Pitch = AimPitch;
                        BotIntendedAim[(void*)Pawn] = AimRecord;

                        FBotShotAim& ShotAim = BotShotAim[(void*)Pawn];
                        ShotAim.Target = (void*)Target;
                        ShotAim.bOnTarget = bOnTarget;
                        ShotAim.bCanShoot = bCanShoot;
                        ShotAim.Distance = (float)AimDist;
                    }
                    else
                    {

                        BotStopAiming(AIController);
                        BotIntendedAim.erase((void*)Pawn);
                        BotShotAim.erase((void*)Pawn);
                    }

                    if (Flat > PreferredRange || !bTargetVisible)
                    {
                        BotStoppedForFight.erase((void*)Pawn);

                        FVector Goal = TLoc;

                        if (!bTargetVisible)
                        {
                            auto Spot = BotLastSeenSpot.find((void*)Pawn);

                            if (Spot != BotLastSeenSpot.end())
                            {
                                const double SpotX = Spot->second.X - MyLoc.X;
                                const double SpotY = Spot->second.Y - MyLoc.Y;

                                if ((SpotX * SpotX + SpotY * SpotY) > (500.0 * 500.0))
                                    Goal = Spot->second;
                                else
                                    BotLastSeenSpot.erase(Spot);
                            }
                        }

                        const double GoalDx = Goal.X - MyLoc.X;
                        const double GoalDy = Goal.Y - MyLoc.Y;
                        const double GoalLen = sqrt(GoalDx * GoalDx + GoalDy * GoalDy);

                        auto ct = BotCombatTick.find(Pawn);
                        if (ct == BotCombatTick.end() || (reportCounter_forDir - ct->second) > 12)
                        {
                            BotCombatTick[Pawn] = reportCounter_forDir;

                            BotMoveTo(AIController, Pawn, Goal, 400.f);
                        }

                        static std::map<void*, FVector> ChaseOrigin;
                        static std::map<void*, int> ChaseCheckTick;

                        auto ChaseTick = ChaseCheckTick.find(Pawn);

                        if (ChaseTick == ChaseCheckTick.end() || (reportCounter_forDir - ChaseTick->second) >= 30)
                        {
                            auto ChaseFrom = ChaseOrigin.find(Pawn);

                            if (ChaseTick != ChaseCheckTick.end() && ChaseFrom != ChaseOrigin.end() && GoalLen > 1.0)
                            {
                                const double MovedX = MyLoc.X - ChaseFrom->second.X;
                                const double MovedY = MyLoc.Y - ChaseFrom->second.Y;

                                if ((MovedX * MovedX + MovedY * MovedY) < (150.0 * 150.0))
                                {
                                    FVector Push{};
                                    Push.X = GoalDx / GoalLen;
                                    Push.Y = GoalDy / GoalLen;
                                    Push.Z = 0.0;
                                    Pawn->AddMovementInput(Push, 1.f, false);

                                    FVector Hopup{};
                                    Hopup.X = Push.X * 120.0;
                                    Hopup.Y = Push.Y * 120.0;
                                    Hopup.Z = 520.0;
                                    Pawn->LaunchCharacterJump(Hopup, false, nullptr, true);

                                    GChaseHops++;
                                }
                            }

                            ChaseOrigin[Pawn] = MyLoc;
                            ChaseCheckTick[Pawn] = reportCounter_forDir;
                        }
                    }
                    else if (!GStopFaulted && BotStoppedForFight.insert((void*)Pawn).second)
                    {

                        BotStopWalkingGuarded(AIController);
                    }

                    FBotFireState& Fire = BotFire[Pawn];

                    if (!bCanShoot)
                    {

                        if (Fire.bFiring)
                        {
                            BotGunTrigger(Pawn, false);

                            if (bInShootRange && !bTargetVisible)
                                GBurstsCutOutOfSight++;
                        }

                        Fire.bFiring = false;
                        Fire.BurstTicksLeft = 0;
                        Fire.ReactionTicksLeft = -1;

                        if (bInShootRange && !bTargetVisible)
                            GTicksHeldFireOutOfSight++;
                    }
                    else if (Fire.ReactionTicksLeft < 0)
                    {
                        Fire.ReactionTicksLeft = 8 + (rand() % 18);
                    }

                    if (BotIsReloading(Pawn))
                    {

                        if (Fire.bFiring)
                            BotGunTrigger(Pawn, false);

                        Fire.bFiring = false;
                        Fire.BurstTicksLeft = 0;
                        Fire.RoundsSinceReload = 0;

                        if (Fire.CooldownTicksLeft < 4)
                            Fire.CooldownTicksLeft = 4 + (rand() % 8);
                    }
                    else if (!bCanShoot)
                    {

                    }
                    else if (Fire.ReactionTicksLeft > 0)
                    {
                        Fire.ReactionTicksLeft--;
                    }
                    else if (Fire.BurstTicksLeft > 0)
                    {
                        Fire.BurstTicksLeft--;

                        if (Fire.BurstTicksLeft == 0)
                        {
                            BotGunTrigger(Pawn, false);
                            Fire.bFiring = false;
                            Fire.CooldownTicksLeft = 12 + (rand() % 24);
                        }
                    }
                    else if (Fire.CooldownTicksLeft > 0)
                    {
                        Fire.CooldownTicksLeft--;
                    }
                    else
                    {
                        if (BotIsHoldingAWeapon(Pawn))
                        {
                            BotGunTrigger(Pawn, true, Target, (float)(Flat / 100.0));
                            Fire.bFiring = true;
                            Fire.BurstTicksLeft = 8 + (rand() % 10);

                            Fire.RoundsSinceReload += 1 + (Fire.BurstTicksLeft / 5);

                            GCombatBurstsFired++;

                            if (auto TargetController = (AActor*)Target->Controller)
                                ShotAtBy[(void*)TargetController] = { (void*)PlayerState, GStormTick };
                        }
                        else
                        {
                            GCombatNoWeapon++;

                            Fire.CooldownTicksLeft = 60;

                            auto WeaponIt = BotWeapons.find((void*)PlayerState);
                            if (WeaponIt != BotWeapons.end() && WeaponIt->second)
                            {
                                FGuid ReEquipGuid{};
                                bool bFoundEntry = false;

                                auto InvIt = BotInventories.find((void*)PlayerState);
                                auto Inventory = (InvIt != BotInventories.end()) ? InvIt->second : nullptr;

                                if (Inventory && Inventory->HasInventory())
                                {
                                    for (int q = 0; q < Inventory->Inventory.ItemInstances.Num(); q++)
                                    {
                                        auto Item = Inventory->Inventory.ItemInstances[q];
                                        if (!Item || !Item->HasItemEntry())
                                            continue;
                                        if (!FFortItemEntry::HasItemDefinition() || Item->ItemEntry.ItemDefinition != WeaponIt->second)
                                            continue;
                                        if (FFortItemEntry::HasItemGuid())
                                            ReEquipGuid = Item->ItemEntry.ItemGuid;
                                        bFoundEntry = true;
                                        break;
                                    }

                                    if (!bFoundEntry)
                                    {
                                        int Clip = BotMagazineSize((void*)PlayerState);

                                        if (auto Fresh = Inventory->GiveItem(WeaponIt->second, 1, Clip))
                                        {
                                            if (Fresh->HasItemEntry() && FFortItemEntry::HasItemGuid())
                                                ReEquipGuid = Fresh->ItemEntry.ItemGuid;

                                            GWeaponsReissued++;
                                        }
                                    }
                                }

                                Pawn->EquipWeaponDefinition(WeaponIt->second, ReEquipGuid);
                            }
                        }
                    }

                    if (Fire.bFiring && bCanShoot)
                        TickBotGun(Pawn, Controller, PlayerState, LivePawns);

                    GCombatEngagements++;
                }
            }

            if (!bInCombat)
            {
                auto FireIt = BotFire.find(Pawn);
                if (FireIt != BotFire.end())
                {
                    if (FireIt->second.bFiring)
                        BotGunTrigger(Pawn, false);

                    BotFire.erase(FireIt);
                }

                BotStopAiming(AIController);
                BotIntendedAim.erase((void*)Pawn);
                BotShotAim.erase((void*)Pawn);
                BotStoppedForFight.erase((void*)Pawn);
                BotLastSeenSpot.erase((void*)Pawn);
            }

            FVector ZoneCentre{};
            float ZoneRadius = 0.f;
            const bool bZoneKnown = FConfiguration::bBotsFleeTheStorm && GetSafeZoneCircle(GameMode, ZoneCentre, ZoneRadius);

            double ZoneDx = MyLoc.X - ZoneCentre.X;
            double ZoneDy = MyLoc.Y - ZoneCentre.Y;
            double ZoneDistance = bZoneKnown ? sqrt(ZoneDx * ZoneDx + ZoneDy * ZoneDy) : 0.0;

            const bool bWouldBeOutsideNextCircle = bZoneKnown && (ZoneDistance > (double)ZoneRadius * 0.85);

            if (bZoneKnown && (bOutsideSafeZone || bWouldBeOutsideNextCircle))
            {
                double dx = ZoneDx;
                double dy = ZoneDy;
                double Length = ZoneDistance;

                if (bOutsideSafeZone)
                    GZoneBotsInStorm++;
                {
                    auto WasAt = BotLastZoneDistance.find(Pawn);
                    if (WasAt != BotLastZoneDistance.end() && bOutsideSafeZone)
                    {
                        if (Length < WasAt->second - 1.0)
                            GZoneBotsClosing++;
                        else if (Length < WasAt->second + 1.0)
                            GZoneBotsStuck++;
                    }
                    BotLastZoneDistance[Pawn] = (float)Length;
                }

                FVector Goal = ZoneCentre;

                float Fraction = FConfiguration::BotZoneEntryFraction;
                if (!(Fraction >= 0.f) || Fraction > 0.9f)
                    Fraction = 0.5f;

                {
                    const uint32 Seed = BotPersonalSeed((void*)Pawn);

                    const double Angle = ((double)(Seed % 3600) / 10.0) * 3.14159265358979323846 / 180.0;

                    const double Spread = 0.30 + ((double)((Seed >> 12) % 60) / 100.0);

                    const double Out = (double)ZoneRadius * Spread * (double)(Fraction / 0.5f);

                    Goal.X = ZoneCentre.X + cos(Angle) * Out;
                    Goal.Y = ZoneCentre.Y + sin(Angle) * Out;
                    Goal.Z = MyLoc.Z;
                }

                if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
                {
                    auto Move = Pawn->CharacterMovement;
                    if (Move->HasMaxWalkSpeed() && Move->MaxWalkSpeed < 500.f)
                        Move->MaxWalkSpeed = 510.f;
                }

                FVector Hop = Goal;

                {
                    double HopDx = Goal.X - MyLoc.X;
                    double HopDy = Goal.Y - MyLoc.Y;
                    double HopLen = sqrt(HopDx * HopDx + HopDy * HopDy);

                    const double HopDistance = 8000.0;

                    if (HopLen > HopDistance)
                    {
                        Hop = MyLoc;
                        Hop.X += (HopDx / HopLen) * HopDistance;
                        Hop.Y += (HopDy / HopLen) * HopDistance;
                    }
                }

                auto it = BotWanderGoals.find(Pawn);
                auto zt = BotZoneMoveTick.find(Pawn);

                const bool bGoalMoved = (it == BotWanderGoals.end()) ||
                                        (fabs(it->second.X - Hop.X) > 100.0) || (fabs(it->second.Y - Hop.Y) > 100.0);
                const bool bDueAgain = (zt == BotZoneMoveTick.end()) || ((reportCounter_forDir - zt->second) > 30);

                if (bGoalMoved || bDueAgain)
                {
                    BotWanderGoals[Pawn] = Hop;
                    BotZoneMoveTick[Pawn] = reportCounter_forDir;

                    BotMoveTo(AIController, Pawn, Hop, 100.f);

                    double GoalDx = Goal.X - MyLoc.X;
                    double GoalDy = Goal.Y - MyLoc.Y;
                    double GoalLen = sqrt(GoalDx * GoalDx + GoalDy * GoalDy);

                    if (GoalLen > 1.0)
                    {
                        FRotator Look{};
                        Look.Pitch = 0.0;
                        Look.Yaw = (double)(atan2f((float)GoalDy, (float)GoalDx) * (180.f / 3.14159265f));
                        Look.Roll = 0.0;
                        Pawn->K2_SetActorRotation(Look, false);
                    }

                    GZoneOrdersIssued++;
                }

                {
                    static std::map<void*, FVector> ZoneMoveOrigin;
                    static std::set<void*> ZoneStuck;

                    auto Origin = ZoneMoveOrigin.find(Pawn);

                    if (bGoalMoved || bDueAgain || Origin == ZoneMoveOrigin.end())
                    {
                        if (Origin != ZoneMoveOrigin.end())
                        {
                            const double MovedX = MyLoc.X - Origin->second.X;
                            const double MovedY = MyLoc.Y - Origin->second.Y;

                            if ((MovedX * MovedX + MovedY * MovedY) < (200.0 * 200.0))
                                ZoneStuck.insert(Pawn);
                            else
                                ZoneStuck.erase(Pawn);
                        }

                        ZoneMoveOrigin[Pawn] = MyLoc;
                    }

                    if (ZoneStuck.find(Pawn) != ZoneStuck.end() && Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
                    {
                        double GoalDx = Hop.X - MyLoc.X;
                        double GoalDy = Hop.Y - MyLoc.Y;
                        double GoalLen = sqrt(GoalDx * GoalDx + GoalDy * GoalDy);

                        if (GoalLen > 1.0)
                        {
                            FVector Push{};
                            Push.X = GoalDx / GoalLen;
                            Push.Y = GoalDy / GoalLen;
                            Push.Z = 0.0;
                            Pawn->AddMovementInput(Push, 1.f, false);

                            if ((reportCounter_forDir % 22) == 0)
                            {
                                FVector Hopup{};
                                Hopup.X = Push.X * 120.0;
                                Hopup.Y = Push.Y * 120.0;
                                Hopup.Z = 520.0;
                                Pawn->LaunchCharacterJump(Hopup, false, nullptr, true);
                            }
                        }
                    }
                }

                continue;
            }

            if (bInCombat)
                continue;

            {
                if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
                {
                    auto Move = Pawn->CharacterMovement;
                    if (Move->HasMaxWalkSpeed() && Move->MaxWalkSpeed < 500.f)
                        Move->MaxWalkSpeed = 510.f;
                }

                auto it = BotWanderDirTick.find(Pawn);

                bool bNeedsNewDest = (it == BotWanderDirTick.end()) ||
                                     ((reportCounter_forDir - it->second) > 30);
                if (bNeedsNewDest)
                {
                    BotWanderDirTick[Pawn] = reportCounter_forDir;

                    FVector Dest;

                    {
                        float angle = (float)(rand() % 360) * (3.14159265f / 180.f);
                        float dist  = (float)(3000 + (rand() % 8000));
                        Dest = MyLoc;
                        Dest.X += cosf(angle) * dist;
                        Dest.Y += sinf(angle) * dist;
                    }

                    BotWanderGoals[Pawn] = Dest;

                    uint8_t moveResult = BotMoveTo(AIController, Pawn, Dest, 50.f);

                    if (moveResult == 0) MoveResult_Failed++;
                    else if (moveResult == 1) MoveResult_AlreadyAtGoal++;
                    else if (moveResult == 2) MoveResult_Success++;
                    else MoveResult_Other++;
                }

                {
                    static std::map<void*, FVector> WanderOrigin;
                    static std::map<void*, int> WanderStuckSince;

                    auto Origin = WanderOrigin.find(Pawn);

                    if (Origin == WanderOrigin.end())
                    {
                        WanderOrigin[Pawn] = MyLoc;
                    }
                    else if ((reportCounter_forDir % 20) == 0)
                    {
                        const double MovedX = MyLoc.X - Origin->second.X;
                        const double MovedY = MyLoc.Y - Origin->second.Y;
                        const bool bBarelyMoved = (MovedX * MovedX + MovedY * MovedY) < (150.0 * 150.0);

                        if (bBarelyMoved)
                            WanderStuckSince[Pawn] = WanderStuckSince.count(Pawn) ? WanderStuckSince[Pawn] + 1 : 1;
                        else
                            WanderStuckSince.erase(Pawn);

                        WanderOrigin[Pawn] = MyLoc;
                    }

                    auto StuckIt = WanderStuckSince.find(Pawn);

                    if (StuckIt != WanderStuckSince.end() && StuckIt->second >= 1 &&
                        Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
                    {
                        auto GoalIt = BotWanderGoals.find(Pawn);

                        if (GoalIt != BotWanderGoals.end())
                        {
                            double GoalDx = GoalIt->second.X - MyLoc.X;
                            double GoalDy = GoalIt->second.Y - MyLoc.Y;
                            double GoalLen = sqrt(GoalDx * GoalDx + GoalDy * GoalDy);

                            if (GoalLen > 1.0)
                            {
                                FVector Push{};
                                Push.X = GoalDx / GoalLen;
                                Push.Y = GoalDy / GoalLen;
                                Push.Z = 0.0;
                                Pawn->AddMovementInput(Push, 1.f, false);

                                if ((reportCounter_forDir % 8) == 0)
                                {
                                    const double FaceYaw = atan2(GoalDy, GoalDx) * (180.0 / 3.14159265358979323846);
                                    FRotator Face = Pawn->K2_GetActorRotation();
                                    Face.Yaw = FaceYaw;
                                    Pawn->K2_SetActorRotation(Face, true);
                                }

                                if ((reportCounter_forDir % 24) == 0)
                                {
                                    FVector Hopup{};
                                    Hopup.X = Push.X * 120.0;
                                    Hopup.Y = Push.Y * 120.0;
                                    Hopup.Z = 520.0;
                                    Pawn->LaunchCharacterJump(Hopup, false, nullptr, true);
                                }
                            }
                        }

                        if (StuckIt->second >= 3)
                        {
                            BotWanderDirTick.erase(Pawn);
                            BotWanderGoals.erase(Pawn);
                            WanderStuckSince.erase(Pawn);
                            GWanderUnstuck++;
                        }
                    }
                }
            }
        }

        if (FConfiguration::bBotLog && (reportCounter_forDir % 150) == 0 && GZoneBotsInStorm > 0)
        {
            LogLine("[ZONE] In the storm: %d | closing on the circle: %d | not moving: %d | orders given: %d (straight-line, no navmesh)",
                    GZoneBotsInStorm, GZoneBotsClosing, GZoneBotsStuck, GZoneOrdersIssued);
        }

        if (FConfiguration::bBotLog && (reportCounter_forDir % 300) == 0 && GCombatEngagements > 0)
        {
            LogLine("[COMBAT] Engagements: %d | bursts fired: %d | shots that actually landed bot-on-bot: %d | "
                    "tried to fire holding nothing: %d | weapons handed back: %d | reloads: %d | magazines refilled: %d | "
                    "unstuck while wandering: %d",
                    GCombatEngagements, GCombatBurstsFired, GCombatBotOnBotHits,
                    GCombatNoWeapon, GWeaponsReissued, GReloadsStarted, GMagazinesRefilled, GWanderUnstuck);

            LogLine("[ARMS] Guns found holstered on the ground: %d time(s) -- taken out of the holster %d, still holstered right after %d%s%s.",
                    GGunsFoundHolstered, GGunsUnholstered, GGunsStillHolstered,
                    GHolsterReasonsSeen.empty() ? "" : " | holster reasons seen: ", GHolsterReasonsSeen.c_str());

            LogLine("[DAMAGE] Damage real players did to bots since the bus: in the air %d hit(s), %.0f in all | on the ground %d hit(s), "
                    "%.0f in all | rounds real players fired: %d | health bots lost to other bots or anything else (storm, falls): %d time(s).",
                    GDamageAirHits, GDamageAirTotal, GDamageGroundHits, GDamageGroundTotal, GRoundsFiredByPlayers, GDamageOtherHits);

            LogCueSenderSummary();
            LogEliminationSummary();
            LogReloadSummary();
            LogBotGunSummary();
            LogBotDamageSummary();
            LogBotLootSummary();

            int LockedNow = 0;
            for (auto& Lock : BotLockedOnName)
                if (LivePawns.find(Lock.first) != LivePawns.end())
                    LockedNow++;

            LogLine("[LOCK] Locked on: %d | ended because the target died: %d | locked right now: %d | bursts cut off when the target "
                    "went out of sight: %d | ticks holding fire with the target in range but out of sight: %d | deliberate misses: %d | "
                    "hops while chasing: %d",
                    GLocksMade, GLocksEnded, LockedNow, GBurstsCutOutOfSight, GTicksHeldFireOutOfSight, GAimDeliberateMisses, GChaseHops);

            LogLine("[EYES] Straight-line sight checks: clear %d, blocked %d%s%s.",
                    GSightByLineClear, GSightByLineBlocked,
                    GSightFirstBlocker.empty() ? "" : " | the first thing that was in the way: ", GSightFirstBlocker.c_str());

            LogLine("[AIM] Mid-burst, the gun was still pointed within 2 degrees of where the bot code aimed it on %d of %d checks "
                    "(off by %.1f degrees on average, worst %.0f) | built-in senses switched off on %d bot(s).",
                    GAimHeld, GAimChecks, GAimChecks > 0 ? GAimOffSum / (double)GAimChecks : 0.0, GAimOffMax, GSensesSilencedBots);

            LogLine("[HITS] Bot-on-bot hits on the one the shooter is locked onto: %d | on somebody else: %d | ticks a shot was "
                    "held because the ground was in the way: %d",
                    GHitsOnLockedTarget, GHitsOnSomebodyElse, GTerrainBlocks);
        }

        GZoneBotsInStorm = 0;
        GZoneBotsClosing = 0;
        GZoneBotsStuck = 0;

        reportCounter_forDir++;

        static int reportCounter = 0;
        if (((++reportCounter) % 900) == 0 && (nativeBrainCount + fallbackCount) > 0)
        {
            int movers = 0, stuck = 0;
            double totalDist = 0.0;
            double maxDist = 0.0;

            for (int i = 0; i < GameState->PlayerArray.Num(); i++)
            {
                auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                if (!PS || !PS->HasbIsABot() || !PS->bIsABot) continue;
                auto PC = (AFortPlayerControllerAthena*)PS->Owner;
                if (!PC) continue;
                auto Pw = (AFortPlayerPawnAthena*)PC->Pawn;
                if (!Pw) continue;

                FVector Now = Pw->K2_GetActorLocation();
                auto prev = MovementProbe.find((void*)Pw);
                if (prev != MovementProbe.end())
                {
                    double dx = Now.X - prev->second.X;
                    double dy = Now.Y - prev->second.Y;
                    double dz = Now.Z - prev->second.Z;
                    double d = sqrt(dx*dx + dy*dy + dz*dz);
                    totalDist += d;
                    if (d > maxDist) maxDist = d;
                    if (d > 200.0) movers++; else stuck++;
                }
                MovementProbe[(void*)Pw] = Now;
            }

            int probed = movers + stuck;
            LogLine("[BotAI] Movement: %d native-brain, %d fallback | moved-since-last-report: %d, stationary: %d, avg %.0f units, max %.0f units\n",
                   nativeBrainCount, fallbackCount, movers, stuck,
                   probed > 0 ? (float)(totalDist / probed) : 0.f, (float)maxDist);

            LogLine("[BotAI] Wander MoveOrders(total): success=%d failed=%d alreadyAtGoal=%d other=%d | all straight-line (navmesh removed)\n",
                   MoveResult_Success, MoveResult_Failed, MoveResult_AlreadyAtGoal, MoveResult_Other);
        }
    }

    static void GameThreadTick_Inner();

    static int SehFaultCount = 0;

    static DWORD GGuardCode = 0;
    static uint64 GGuardAddress = 0;
    static uint64 GGuardModuleBase = 0;
    static int GGuardAccess = -1;
    static uint64 GGuardTouched = 0;

    static int NoteGuardFault(EXCEPTION_POINTERS* Info)
    {
        GGuardCode = 0;
        GGuardAddress = 0;
        GGuardModuleBase = 0;
        GGuardAccess = -1;
        GGuardTouched = 0;

        if (Info && Info->ExceptionRecord)
        {
            auto Record = Info->ExceptionRecord;

            GGuardCode = Record->ExceptionCode;
            GGuardAddress = (uint64)Record->ExceptionAddress;

            HMODULE Module = nullptr;

            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)Record->ExceptionAddress, &Module) && Module)
                GGuardModuleBase = (uint64)Module;

            if (Record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && Record->NumberParameters >= 2)
            {
                GGuardAccess = (int)Record->ExceptionInformation[0];
                GGuardTouched = (uint64)Record->ExceptionInformation[1];
            }
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }

    static void LogGuardFault(bool bWholeTickSkipped)
    {
        SehFaultCount++;

        if (SehFaultCount <= 20)
        {
            LogLine("[GUARD] Caught an access violation inside the bot tick (fault #%d). Server kept alive; %s.", SehFaultCount,
                    bWholeTickSkipped ? "this tick was skipped"
                                      : "the rest of the bot movement/fighting step was skipped, the rest of the tick still ran");

            char ModulePath[MAX_PATH] = "?";

            if (GGuardModuleBase)
                GetModuleFileNameA((HMODULE)GGuardModuleBase, ModulePath, MAX_PATH);

            const char* ModuleName = strrchr(ModulePath, '\\');
            ModuleName = ModuleName ? ModuleName + 1 : ModulePath;

            std::string Who;

            if (GFightBot && ActorIsLive((const UObject*)GFightBot))
                Who = std::string(" [the bot being handled: '") + SafeNameOf((AFortPlayerStateAthena*)GFightBot) + "']";
            else if (GFightBot)
                Who = " [the bot being handled is no longer in the game -- a freed pawn or target]";

            LogLine("[GUARD]   fault #%d happened while %s%s -- code 0x%08X at %s+0x%llX%s0x%llX.", SehFaultCount, GTickStage,
                    Who.c_str(), (unsigned)GGuardCode, ModuleName, (unsigned long long)(GGuardAddress - GGuardModuleBase),
                    GGuardAccess == 0 ? ", reading " : (GGuardAccess == 1 ? ", writing " : (GGuardAccess == 8 ? ", executing " : ", touching ")),
                    (unsigned long long)GGuardTouched);
        }

        if (SehFaultCount == 21)
            LogLine("[GUARD] Further faults will not be logged individually.");
    }

    static int TickMovementGuarded(UWorld* World, AFortGameStateAthena* GameState, AFortGameMode* GameMode)
    {
        __try
        {
            TickMovement(World, GameState, GameMode);
            return 0;
        }
        __except (NoteGuardFault(GetExceptionInformation()))
        {
            LogGuardFault(false);
            return 1;
        }
    }

    void GameThreadTick()
    {
        FCrashReporter::EnterGuardedSection();

        __try
        {
            GameThreadTick_Inner();
        }
        __except (NoteGuardFault(GetExceptionInformation()))
        {
            LogGuardFault(true);
        }

        FCrashReporter::LeaveGuardedSection();
    }

    static void GameThreadTick_Inner()
    {
        static bool bAnnounced = false;
        if (!bAnnounced)
        {
            bAnnounced = true;
            LogLine("[BotAI] Game-thread tick is running.\n");

            FindObject<UObject>(L"/Game/Athena/GameplayCueNotifies/GCN_Athena_OutsideSafeZoneDamage.GCN_Athena_OutsideSafeZoneDamage_C");
            FindObject<UObject>(L"/Game/Athena/GameplayCueNotifies/GCN_Athena_PetrolPickup_EnterWater.GCN_Athena_PetrolPickup_EnterWater_C");
            FindObject<UObject>(L"/Game/Athena/GameplayCueNotifies/GCN_Athena_PetrolPickup_HitWorld.GCN_Athena_PetrolPickup_HitWorld_C");
            LogLine("[BotAI] Preloaded storm-damage gameplay cue.\n");
        }

        GTickStage = "the start of the tick";
        KeepTheClockHonest();

        GTickStage = "watching a player's \"cheat speed\" (TickSpeedWatch)";
        TickSpeedWatch();

        NPCs::GameThreadTick();

        GTickStage = "the bus launch";

        if (const char* AskedFor = GBusLaunchRequest.exchange(nullptr))
            LaunchTheBus(AskedFor, nullptr);

        if (GBusCheckUntil >= 0.f)
        {
            auto BusWorld = UWorld::GetWorld();

            if (BusWorld && BusWorld->GameState)
                CheckTheBusLeft(BusWorld, (AFortGameStateAthena*)BusWorld->GameState, (float)UGameplayStatics::GetTimeSeconds(BusWorld));
        }

        if (FConfiguration::bDisableStormSurge)
        {
            auto SurgeWorld = UWorld::GetWorld();

            if (SurgeWorld && SurgeWorld->AuthorityGameMode)
            {
                auto SurgeMode = (AFortGameMode*)SurgeWorld->AuthorityGameMode;

                static int Reported = 0;
                static bool bFoundAny = false;

                if (SurgeMode->HasbDisableStormCapSystem() && !SurgeMode->bDisableStormCapSystem)
                {
                    SurgeMode->bDisableStormCapSystem = true;
                    bFoundAny = true;
                }

                if (SurgeMode->HasbStormCapSystemEnabled() && SurgeMode->bStormCapSystemEnabled)
                {
                    SurgeMode->bStormCapSystemEnabled = false;
                    bFoundAny = true;
                }

                if (SurgeMode->HasPlaylistSupportsStormCapSystem() && SurgeMode->PlaylistSupportsStormCapSystem)
                {
                    SurgeMode->PlaylistSupportsStormCapSystem = false;
                    bFoundAny = true;
                }

                if (SurgeWorld->GameState)
                {
                    auto SurgeState = (AFortGameStateAthena*)SurgeWorld->GameState;
                    if (SurgeState->HasStormCapState() && SurgeState->StormCapState != 0)
                    {
                        uint8 Off = 0;
                        SurgeState->StormCapState = Off;
                        bFoundAny = true;
                    }
                }

                if (bFoundAny && ++Reported == 1)
                    LogLine("[SURGE] Storm surge turned off (Epic calls it the Storm Cap system). It will be held off for the whole match.");
            }
        }

        if (FConfiguration::AutoFillTargetPlayerCount <= 0)
            return;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameState->HasPlayerArray() || !GameMode->HasAlivePlayers())
            return;

        float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        GStormTick++;

        GTickStage = "keeping bots on their own pawns (KeepBotsOnTheirOwnPawns)";
        KeepBotsOnTheirOwnPawns(GameState);

        GTickStage = "the recorded places (PlaceDump)";
        PlaceDump::EnsureLoaded();
        PlaceDump::ReportOnce();
        PlaceDump::AnnounceHotkey();
        PlaceDump::RecordTick();

        GTickStage = "the storm";
        if (!FConfiguration::bDisableStorm)
        {
            SilenceDistantStorm();
            EnsureStormTimingData(GameState);
            ApplyCustomZoneTimings(GameMode);
            LogStormState(GameMode, GameState);
            LogStormChanges(GameMode, GameState);

            WipeTheSlateAtBusTime(GameState);

            TrackEveryBotAlive(GameState, GStormTick);

            ApplyStormDamage(GameMode, GameState, GStormTick);
        }

        if (FConfiguration::bDisableStorm)
        {
            float FarFutureTime = Now + 999999999.f;

            if (GameState->HasSafeZonesStartTime())
                GameState->SafeZonesStartTime = Now + 60.f;

            if (GameMode->HasSafeZonePhase())
                GameMode->SafeZonePhase = 0;

            if (GameMode->HasbSafeZonePaused())
                GameMode->bSafeZonePaused = true;
            if (GameMode->HasbSafeZoneActive())
                GameMode->bSafeZoneActive = false;

            if (GameMode->SafeZoneIndicator)
            {
                auto Ind = GameMode->SafeZoneIndicator;

                if (Ind->HasSafeZoneStartShrinkTime())
                    Ind->SafeZoneStartShrinkTime = FarFutureTime;
                if (Ind->HasSafeZoneFinishShrinkTime())
                    Ind->SafeZoneFinishShrinkTime = FarFutureTime;
                if (Ind->HasCurrentPhase())
                    Ind->CurrentPhase = 0;

                if (Ind->HasNextRadius())
                    Ind->NextRadius = 1000000.f;
                if (Ind->HasNextNextRadius())
                    Ind->NextNextRadius = 1000000.f;
                if (Ind->HasPreviousRadius())
                    Ind->PreviousRadius = 1000000.f;
                if (Ind->HasLastRadius())
                    Ind->LastRadius = 1000000.f;
            }

            static bool bAnnouncedStormOff = false;
            if (!bAnnouncedStormOff)
            {
                bAnnouncedStormOff = true;
                LogLine("[STORM] Freeze active. SafeZonesStartTime=%s SafeZonePhase=%s Paused=%s Active=%s Indicator=%s",
                        GameState->HasSafeZonesStartTime() ? "pinned" : "PROPERTY MISSING",
                        GameMode->HasSafeZonePhase()       ? "pinned" : "PROPERTY MISSING",
                        GameMode->HasbSafeZonePaused()     ? "set"    : "PROPERTY MISSING",
                        GameMode->HasbSafeZoneActive()     ? "set"    : "PROPERTY MISSING",
                        GameMode->SafeZoneIndicator        ? "present": "null (warmup)");
            }
        }
        else if (GameMode->SafeZoneIndicator)
        {
            auto Ind = GameMode->SafeZoneIndicator;

            static int LastStormPhaseSeen = -1;

            if (GameMode->HasSafeZonePhase() && Ind->HasSafeZoneStartShrinkTime() && Ind->HasSafeZoneFinishShrinkTime())
            {
                const int StormPhaseNow = (int)GameMode->SafeZonePhase;

                if (StormPhaseNow != LastStormPhaseSeen)
                {
                    LastStormPhaseSeen = StormPhaseNow;

                    const bool bDegenerate = !((float)Ind->SafeZoneFinishShrinkTime > Now + 1.f);

                    const float LastR = Ind->HasLastRadius() ? (float)Ind->LastRadius : -1.f;
                    const float NextR = Ind->HasNextRadius() ? (float)Ind->NextRadius : -1.f;
                    const bool bWallStays = LastR > 0.f && NextR > 0.f && NextR >= LastR - 1.f;

                    if (GStormTimedByTick && StormPhaseNow <= 0 && bWallStays)
                    {
                        const float HadStart = (float)Ind->SafeZoneStartShrinkTime - Now;
                        const float HadFinish = (float)Ind->SafeZoneFinishShrinkTime - Now;

                        if (!bDegenerate)
                        {
                            Ind->SafeZoneStartShrinkTime = Now;
                            Ind->SafeZoneFinishShrinkTime = Now;
                        }

                        LogLine("[Storm] Phase %d is the game's no-storm-yet step (the whole map: radius %.0f -> %.0f)%s -- the first real "
                                "circle forms now.", StormPhaseNow, LastR, NextR,
                                bDegenerate ? "" : (" and it had " + std::to_string((int)HadStart) + "s to hold and " +
                                                    std::to_string((int)HadFinish) + "s until done on it: ended at once").c_str());
                    }
                    else if (GStormTimedByTick && StormPhaseNow >= 1)
                    {
                        int Index = StormPhaseNow - 1;
                        if (Index >= kZoneCount)
                            Index = kZoneCount - 1;

                        const float Hold = kZoneWait[Index] > 0.f ? kZoneWait[Index] : 0.f;
                        const float Close = kZoneShrink[Index] >= 10.f ? kZoneShrink[Index] : 10.f;

                        const float GameHold = (float)Ind->SafeZoneStartShrinkTime - Now;
                        const float GameClose = (float)Ind->SafeZoneFinishShrinkTime - (float)Ind->SafeZoneStartShrinkTime;

                        Ind->SafeZoneStartShrinkTime = Now + Hold;
                        Ind->SafeZoneFinishShrinkTime = Now + Hold + Close;

                        LogLine("[Storm] Phase %d: holds %.0fs, then closes over %.0fs (radius %.0f -> %.0f; the game had given it %.0fs and %.0fs).",
                                StormPhaseNow, Hold, Close, LastR, NextR, GameHold, GameClose);
                    }
                    else if (bDegenerate && (StormPhaseNow <= 0 || bWallStays))
                    {
                        LogLine("[Storm] Phase %d is the game's no-storm-yet step (radius %.0f -> %.0f, no time on it) -- left alone, so "
                                "the game goes straight on to the first real circle instead of an empty 30s countdown.",
                                StormPhaseNow, LastR, NextR);
                    }
                    else if (bDegenerate)
                    {

                        float RescueWait = 0.f, RescueShrink = 0.f;
                        SeasonOneStormTiming(StormPhaseNow, RescueWait, RescueShrink);

                        Ind->SafeZoneStartShrinkTime = Now + RescueWait;
                        Ind->SafeZoneFinishShrinkTime = Now + RescueWait + RescueShrink;

                        LogLine("[Storm] Phase %d arrived with its schedule already over (radius %.0f -> %.0f) -- given Season 1's timing "
                                "for phase %d: %.0fs hold, %.0fs close, so it cannot run away.",
                                StormPhaseNow, LastR, NextR, StormPhaseNow, RescueWait, RescueShrink);
                    }
                    else if (FConfiguration::bStormLog)
                    {
                        printf("[Storm] Tick check on phase %d: schedule looks fine -- starts closing in %.1fs, done in %.1fs. Left alone.\n",
                               StormPhaseNow,
                               (float)Ind->SafeZoneStartShrinkTime - Now,
                               (float)Ind->SafeZoneFinishShrinkTime - Now);
                        fflush(stdout);
                    }
                }
            }
        }

        GTickStage = "the lobby and the bus countdown";
        {
            static bool bAnnouncedWaiting = false;
            int humanCount = 0;
            int botCount = 0;
            for (int i = 0; i < GameState->PlayerArray.Num(); i++)
            {
                auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                if (PS && !(PS->HasbIsABot() && PS->bIsABot))
                    humanCount++;
                else if (PS)
                    botCount++;
            }

            const bool bIdsMatter = BotNetIdsMatter();
            const float NoHumanWait = bIdsMatter ? 300.f : 20.f;

            static float FirstTickTime = -1.f;
            if (FirstTickTime < 0.f)
                FirstTickTime = Now;
            const bool bWaitedLongEnoughNoHuman = (Now - FirstTickTime) >= NoHumanWait;

            if (humanCount == 0 && !bBusDeparted && !bWaitedLongEnoughNoHuman)
            {
                if (!bAnnouncedWaiting)
                {
                    bAnnouncedWaiting = true;
                    LogLine("Waiting for a player to join before spawning bots...");
                }
                return;
            }
            if (humanCount == 0 && bWaitedLongEnoughNoHuman)
            {
                static bool bForced = false;
                if (!bForced)
                {
                    bForced = true;
                    LogLine("[BotAI] No human detected after %.0fs -- starting bot fill anyway.", NoHumanWait);
                }
            }

            if (humanCount > 0 && botCount == 0 && bIdsMatter && GNetIdState == 0 && !bBusDeparted)
            {
                static float FirstHumanSeen = -1.f;
                if (FirstHumanSeen < 0.f)
                    FirstHumanSeen = Now;

                if (!FindNetIdFormat(GameState, Now))
                {
                    if (Now - FirstHumanSeen < 30.f)
                    {
                        static bool bSaid = false;
                        if (!bSaid)
                        {
                            bSaid = true;
                            LogLine("[NAMES] The bots wait for your player id to go out once -- it is what their own ids are made from "
                                    "-- before the first one is spawned (30 seconds at most).");
                        }
                        return;
                    }

                    static bool bGaveUp = false;
                    if (!bGaveUp)
                    {
                        bGaveUp = true;
                        LogLine("[NAMES] Your player id had not gone out 30 seconds after you came in -- the bots are spawned anyway: "
                                "they get player numbers of their own, but no ids your game will see.");
                    }
                }
            }

            if (!bBusDeparted && botCount == 0 && !WorldReadyForBots(GameMode, Now))
                return;
        }

        if (!bBusDeparted && bBotSpawningPaused)
        {
            static bool bSaidPaused = false;
            static int LastSeenCount = -1;

            const int WaitingCount = GameMode->AlivePlayers.Num();

            if (!bSaidPaused || WaitingCount != LastSeenCount)
            {
                bSaidPaused = true;
                LastSeenCount = WaitingCount;
                LogLine("[BotAI] Bot spawning is paused -- %d player(s) in the lobby, waiting for \"cheat resume bot spawn\".",
                        WaitingCount);
            }

            BusCountdownStart = -1.f;
            LastBusSecondAnnounced = -1;
        }
        else if (!bBusDeparted)
        {
            int currentCount = GameMode->AlivePlayers.Num();

            if (currentCount >= FConfiguration::AutoFillTargetPlayerCount)
            {
                if (BusCountdownStart < 0.f)
                {
                    BusCountdownStart = Now;
                    LogLine("[BotAI] Lobby full (%d players). Bus starting in 10 seconds...", currentCount);
                    ServerConsole::Say("Player count reached %d! Starting bus...", currentCount);

                    if (GameState->HasWarmupCountdownStartTime())
                        GameState->WarmupCountdownStartTime = Now;
                    if (GameState->HasWarmupCountdownEndTime())
                        GameState->WarmupCountdownEndTime = Now + 10.f;

                    if (GameMode->HasWarmupCountdownDuration())
                        GameMode->WarmupCountdownDuration = 10.f;
                    if (GameMode->HasWarmupEarlyCountdownDuration())
                        GameMode->WarmupEarlyCountdownDuration = 10.f;

                    LogLine("[BotAI] HUD countdown set (WarmupCountdownEndTime = now + 10s).");
                }

                float remaining = 10.f - (Now - BusCountdownStart);

                int secsLeft = (int)ceil(remaining);
                if (secsLeft != LastBusSecondAnnounced && secsLeft >= 1 && secsLeft <= 10)
                {
                    LastBusSecondAnnounced = secsLeft;
                    LogLine("[BotAI] Bus starting in %d...", secsLeft);
                }

                if (remaining <= 0.f)
                {
                    LogLine("[BotAI] Bus launching now.");

                    LaunchTheBus("the lobby filled up and the countdown finished", nullptr);
                    bBusDeparted = true;
                    BusStartedTime = Now;
                }
            }
            else if ((Now - LastFillTime) >= (float)FConfiguration::AutoFillIntervalSeconds)
            {
                LastFillTime = Now;
                int remaining = FConfiguration::AutoFillTargetPlayerCount - currentCount;
                int batch = remaining < FConfiguration::AutoFillBatchSize ? remaining : FConfiguration::AutoFillBatchSize;
                SpawnBots(FVector(0, 0, 0), batch);
                LogLine("[BotAI] AutoFill: %d/%d players.\n", currentCount, FConfiguration::AutoFillTargetPlayerCount);
            }
        }

        int BotsInLobby = 0;
        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];

            if (PS && PS->HasbIsABot() && PS->bIsABot && !NPCs::IsNPC(PS))
                BotsInLobby++;
        }

        const bool bEveryBotThanked = (BotsInLobby > 0) && ((int)ThankedBots.size() >= BotsInLobby);
        const bool bWaitedLongEnough = bBusDeparted && (Now - BusStartedTime) >= 12.f;

        if (bBusDeparted && !bMovedToIsland && !bEveryBotThanked && bWaitedLongEnough)
        {
            static bool bSaidSo = false;
            if (!bSaidSo)
            {
                bSaidSo = true;
                LogLine("[BotAI] %d of %d bot(s) thanked the driver before the drop -- going anyway so nobody is left on the bus.",
                        (int)ThankedBots.size(), BotsInLobby);
            }
        }

        const bool bThanksDone = bEveryBotThanked || bWaitedLongEnough;

        if (bBusDeparted && !bMovedToIsland && bThanksDone)
        {
            GTickStage = "getting bots off the bus (MoveSomeBotsToIsland)";
            if (MoveSomeBotsToIsland(4))
            {
                bMovedToIsland = true;
                LogLine("[BotAI] Finished moving bots onto the map.\n");
            }
        }

        {
            static int lastLoggedPhase = -999;
            if (GameMode->HasSafeZonePhase())
            {
                int phase = GameMode->SafeZonePhase;
                if (phase != lastLoggedPhase)
                {
                    lastLoggedPhase = phase;
                    LogLine("[BotAI] === STORM PHASE -> %d | AlivePlayers=%d PlayerArray=%d PlayersLeft=%d eliminated=%zu ===\n",
                           phase,
                           GameMode->AlivePlayers.Num(),
                           GameState->PlayerArray.Num(),
                           GameState->HasPlayersLeft() ? (int)GameState->PlayersLeft : -1,
                           EliminatedBots.size());
                }
            }
        }

        GTickStage = "holding or releasing the storm";
        HoldTheStorm(GameMode, Now);
        ReleaseTheStorm(GameMode, Now);

        GTickStage = "applying the weapon hotfixes";
        ApplyWeaponHotfixes();
        TopUpGrapplers(GameState, GStormTick);

        ClaimServerOwner(GameState);

        GTickStage = "giving fresh bot pawns health";
        GiveFreshBotPawnsHealth(GameState);
        GTickStage = "updating the HUD counts";
        LogHudState();
        UpdateHudCounts();

        GTickStage = "tracking bot damage (TrackBotDamage)";
        TrackBotDamage();
        GTickStage = "watching the damage real players do to bots (WatchDamageToBots)";
        WatchDamageToBots(GameState);
        GTickStage = "watching the game's gameplay-cue sender (WatchCueSender)";
        WatchCueSender();
        GTickStage = "reactive building (TryBotReactiveBuild)";
        TryBotReactiveBuild(GameState);
        GTickStage = "the dead-bot sweep (RemoveDeadBots)";
        RemoveDeadBots();

        GTickStage = "checking for a winner (CheckForVictory)";
        CheckForVictory();
        GTickStage = "damage protection (ApplyDamageProtection)";
        ApplyDamageProtection();
        GTickStage = "registering bots as alive";
        RegisterBotsAsAlive();
        GTickStage = "assigning bot teams";
        AssignBotTeams();
        GTickStage = "giving bots pickaxes";
        GiveBotPickaxes();
        GTickStage = "giving bots their loot (GiveBotLoadouts)";
        GiveBotLoadouts();
        GTickStage = "thanking the bus driver (ThankAllBots)";
        ThankAllBots();
        GTickStage = "giving bots player ids of their own (GiveBotsTheirOwnNetIds)";
        GiveBotsTheirOwnNetIds(GameState, Now);
        GTickStage = "naming bots (RenameUnnamedBots)";
        RenameUnnamedBots();
        GTickStage = "dressing bots (DressUndressedBots)";
        DressUndressedBots();
        GTickStage = "arming landed bots (ArmLandedBots)";
        ArmLandedBots(World, GameState);
        GTickStage = "bot movement and fighting (TickMovement)";
        GFightBot = nullptr;
        TickMovementGuarded(World, GameState, GameMode);
        GFightBot = nullptr;
        GTickStage = "bot bodies (TickBotBodies)";
        TickBotBodies(World, GameState);
        GTickStage = "gliders (TickParachutes)";
        TickParachutes(World);
        GTickStage = "landings (TickLandings)";
        TickLandings(World);
        GTickStage = "between ticks";
    }
}
